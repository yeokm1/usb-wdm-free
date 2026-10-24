/* SPDX-License-Identifier: GPL-2.0-or-later
 * ASIX AX88179/178A USB 3.0/2.0 to Gigabit Ethernet Devices
 * Copyright (C) 2011-2013 ASIX
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * AX179F chip core. Register sequences adapted from Linux
 * drivers/net/usb/ax88179_178a.c at f47d223627f80cb7a1eb14c585fe6cb35de5e984:
 * ax88179_reset (Start), ax88179_auto_detach, ax88179_get_mac_addr,
 * ax88179_link_reset (Link), ax88179_set_multicast (SetFilter),
 * ax88179_status (Status), ax88179_stop and ax88179_unbind (Stop),
 * ax88179_suspend (Suspend), ax88179_resume (Resume), and
 * ax88179_tx_fixup (TxHeader). Receive framing is axframe.c.
 *
 * Where this core departs from Linux, on purpose:
 * - Checksum offload is written off (RXCOE/TXCOE 0): v1 offloads nothing
 *   (ethernet.md section 4.6). Linux enables both.
 * - LED setting (ax88179_led_setting) and the WoL capability read are not
 *   done; LEDs keep the chip's default and no wake-up is offered.
 * - The MAC is read but not written back by Start; the core writes it
 *   through SetMac when it uses another address (section 4.8).
 * - link_reset's TX FIFO poll is bounded to AX_FIFO_POLLS reads with a
 *   10 ms sleep, instead of a 100 ms busy loop.
 * - Start begins with a settle cycle (Settle, below): the chip is run
 *   briefly with this driver's configuration and stopped as Stop stops
 *   it, then brought up. Linux has no such step.
 * The register writes are otherwise those of Linux, in its order.
 */
#include "axchip.h"
#include "axframe.h"

#define AX_FIFO_POLLS 10
/* The medium a gigabit full-duplex link programs (Link): what the chip
 * held when a session that followed the first one was stopped. */
#define SETTLE_MEDIUM (AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN | AX_MEDIUM_RXFLOW_CTRLEN | \
                       AX_MEDIUM_FULL_DUPLEX | AX_MEDIUM_GIGAMODE | AX_MEDIUM_EN_125MHZ)

static const nc_u8 BulkIn[4][5] = {     /* AX88179_BULKIN_SIZE */
    { 7, 0x4f, 0,    0x12, 0xff },
    { 7, 0x20, 3,    0x16, 0xff },
    { 7, 0xae, 7,    0x18, 0xff },
    { 7, 0xcc, 0x4c, 0x18, 8 },
};

static void W16(nc_u8 *b, nc_u16 v) { b[0] = (nc_u8)v; b[1] = (nc_u8)(v >> 8); }
static nc_u16 R16(const nc_u8 *b) { return (nc_u16)(b[0] | (b[1] << 8)); }
static nc_u32 R32(const nc_u8 *b)
{
    return (nc_u32)b[0] | ((nc_u32)b[1] << 8) | ((nc_u32)b[2] << 16) | ((nc_u32)b[3] << 24);
}

static void Write8(NET_IO *io, nc_u16 reg, nc_u8 v)
{
    NcIoWrite(io, AX_ACCESS_MAC, reg, 1, &v, 1);
}

static nc_u8 Read8(NET_IO *io, nc_u16 reg)
{
    nc_u8 v = 0;
    NcIoRead(io, AX_ACCESS_MAC, reg, 1, &v, 1);
    return v;
}

static void Write16(NET_IO *io, nc_u16 reg, nc_u16 v)
{
    nc_u8 b[2];
    W16(b, v);
    NcIoWrite(io, AX_ACCESS_MAC, reg, 2, b, 2);
}

static nc_u16 Read16(NET_IO *io, nc_u16 reg)
{
    nc_u8 b[2] = { 0, 0 };
    NcIoRead(io, AX_ACCESS_MAC, reg, 2, b, 2);
    return R16(b);
}

static void PhyWrite(NET_IO *io, nc_u16 reg, nc_u16 v)
{
    nc_u8 b[2];
    W16(b, v);
    NcIoWrite(io, AX_ACCESS_PHY, AX88179_PHY_ID, reg, b, 2);
}

static nc_u16 PhyRead(NET_IO *io, nc_u16 reg)
{
    nc_u8 b[2] = { 0, 0 };
    NcIoRead(io, AX_ACCESS_PHY, AX88179_PHY_ID, reg, b, 2);
    return R16(b);
}

long AxProbe(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
             const nc_u32 *size, nc_u32 count, NET_PROBE *probe)
{
    long status;
    nc_u32 i;
    if (NcParseDevice(device, deviceSize, probe) != NC_OK) return NC_E_MALFORMED;
    /* The AX88179 has one configuration with one vendor interface
     * (ax88179_178a.c matches interface class FF/FF/00); take the first
     * configuration that has it. */
    for (i = 0; i < count; i++) {
        if (!config[i] || !size[i]) continue;   /* a configuration that could not be read */
        status = NcFindVendorInterface(config[i], size[i], 1, probe);
        if (status == NC_OK) { probe->ConfigIndex = (nc_u8)i; return NC_OK; }
        if (status == NC_E_MALFORMED) return status;
    }
    return NC_E_UNSUPPORTED;
}

/* ax88179_auto_detach: EEPROM word 0x43 bit 8 enables auto-detach. A read
 * that fails leaves the chip as it is, as in Linux. */
static void AutoDetach(NET_IO *io)
{
    nc_u8 b[2];
    nc_u16 eeprom, v;
    if (NcIoTry(io, AX_ACCESS_EEPROM, 0x43, 1, b, 2) < 0) return;
    eeprom = R16(b);
    if (eeprom == 0xffff || !(eeprom & 0x0100)) return;
    Write8(io, AX_CLK_SELECT, (nc_u8)(Read8(io, AX_CLK_SELECT) | AX_CLK_SELECT_ULR));
    v = Read16(io, AX_PHYPWR_RSTCTL);
    Write16(io, AX_PHYPWR_RSTCTL, (nc_u16)(v | AX_PHYPWR_RSTCTL_AT));
}

/* The settle cycle. The owner's AX88179 forwarded the FCS of every
 * received frame in the first session after it was plugged in, and in no
 * later session, with the same receive configuration (the 2026-10-21 host
 * run, and the 98 SE legs of 2026-10-22: docs/testing/2026-10-22-ax88179-9x.md;
 * this cycle: 2026-10-22-ax88179-fcs.md, unverified on a first session).
 * Linux, FreeBSD and ASIX's own Linux driver name no register bit for it.
 * On 98 SE none of the undocumented bits of RX_CTL, MEDIUM_STATUS_MODE
 * and the bulk-IN queue control that were tried turned it back on, nor
 * did clearing MONITOR_MOD, an EEPROM/eFuse reload, or writing back the
 * values the host run read before it started the chip; MAC registers
 * 00-FF read the same. The state is held where the driver cannot read
 * or set it. What every later session had in common is how it began:
 * from the state Stop leaves after a session (the chip run with this
 * configuration, wake on magic packet armed, gigabit medium; then receive,
 * clocks and PHY off), which the host run's own teardown never produced.
 * Settle puts the chip through that before each bring-up, so the first
 * session after power-up begins where the others do. The receive path
 * keeps its per-frame FCS check (AxRxParse) and the driver logs any frame
 * that still has one as an anomaly. */
static void Settle(NET_IO *io)
{
    Write16(io, AX_PHYPWR_RSTCTL, 0);
    Write16(io, AX_PHYPWR_RSTCTL, AX_PHYPWR_RSTCTL_IPRL);
    NcIoSleep(io, 500);
    Write8(io, AX_CLK_SELECT, AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS);
    NcIoSleep(io, 200);
    NcIoWrite(io, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, BulkIn[0], 5);
    Write8(io, AX_PAUSE_WATERLVL_LOW, 0x34);
    Write8(io, AX_PAUSE_WATERLVL_HIGH, 0x52);
    Write8(io, AX_RXCOE_CTL, 0);
    Write8(io, AX_TXCOE_CTL, 0);
    Write16(io, AX_RX_CTL, AX_RX_CTL_DROPCRCERR | AX_RX_CTL_IPE | AX_RX_CTL_START |
                           AX_RX_CTL_AP | AX_RX_CTL_AMALL | AX_RX_CTL_AB);
    Write8(io, AX_MONITOR_MOD, AX_MONITOR_MODE_PMETYPE | AX_MONITOR_MODE_PMEPOL |
                               AX_MONITOR_MODE_RWMP);
    Write16(io, AX_MEDIUM_STATUS_MODE, SETTLE_MEDIUM);
    NcIoSleep(io, 100);
    /* As Stop: receive off, then clocks and PHY. */
    Write16(io, AX_MEDIUM_STATUS_MODE, (nc_u16)(SETTLE_MEDIUM & ~AX_MEDIUM_RECEIVE_EN));
    Write16(io, AX_RX_CTL, 0);
    Write8(io, AX_CLK_SELECT, 0);
    Write16(io, AX_PHYPWR_RSTCTL, 0);
    NcIoSleep(io, 100);
}

long AxStart(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params)
{
    AX_STATE *s = chip;
    nc_u16 bmcr;
    nc_u8 zero = 0;
    nc_u8 b[2];

    {
        nc_u8 *p = (nc_u8 *)s;
        nc_u32 i;
        for (i = 0; i < sizeof(*s); i++) p[i] = 0;
    }
    s->UsbSpeed = probe->Speed;

    Settle(io);

    /* Power up the Ethernet PHY. */
    Write16(io, AX_PHYPWR_RSTCTL, 0);
    Write16(io, AX_PHYPWR_RSTCTL, AX_PHYPWR_RSTCTL_IPRL);
    NcIoSleep(io, 500);
    Write8(io, AX_CLK_SELECT, AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS);
    NcIoSleep(io, 200);

    AutoDetach(io);

    /* The address the chip loaded from its EEPROM. */
    NcIoRead(io, AX_ACCESS_MAC, AX_NODE_ID, 6, s->Mac, 6);

    /* RX bulk configuration, before a link is known. */
    NcIoWrite(io, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, BulkIn[0], 5);
    s->RxLength = 1024UL * 20;

    Write8(io, AX_PAUSE_WATERLVL_LOW, 0x34);
    Write8(io, AX_PAUSE_WATERLVL_HIGH, 0x52);

    /* No checksum offload in v1 (Linux enables both here). */
    Write8(io, AX_RXCOE_CTL, zero);
    Write8(io, AX_TXCOE_CTL, zero);

    /* The receive mode, set explicitly and read back for the log: the
     * bits Linux's ax88179_reset writes. Neither Linux nor ASIX's public
     * datasheet names an FCS-forwarding bit, so none is set: the frames
     * come without an FCS (after Settle), and RxParse keeps a per-frame
     * check that strips one that has it, which the driver logs as an
     * anomaly. */
    s->RxCtl = AX_RX_CTL_DROPCRCERR | AX_RX_CTL_IPE | AX_RX_CTL_START |
               AX_RX_CTL_AP | AX_RX_CTL_AMALL | AX_RX_CTL_AB;
    Write16(io, AX_RX_CTL, s->RxCtl);
    s->RxCtlRead = Read16(io, AX_RX_CTL);

    Write8(io, AX_MONITOR_MOD, AX_MONITOR_MODE_PMETYPE | AX_MONITOR_MODE_PMEPOL |
                               AX_MONITOR_MODE_RWMP);

    s->Medium = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN | AX_MEDIUM_RXFLOW_CTRLEN |
                AX_MEDIUM_FULL_DUPLEX | AX_MEDIUM_GIGAMODE;
    Write16(io, AX_MEDIUM_STATUS_MODE, s->Medium);

    /* ax88179_disable_eee, then advertise no EEE mode
     * (ax88179_ethtool_set_eee with an empty set). */
    PhyWrite(io, GMII_PHY_PAGE_SELECT, GMII_PHY_PGSEL_PAGE3);
    PhyWrite(io, MII_PHYADDR, 0x3246);
    PhyWrite(io, GMII_PHY_PAGE_SELECT, GMII_PHY_PGSEL_PAGE0);
    PhyWrite(io, MII_MMD_CTRL, MDIO_MMD_AN);
    PhyWrite(io, MII_MMD_DATA, MDIO_AN_EEE_ADV);
    PhyWrite(io, MII_MMD_CTRL, MDIO_MMD_AN | MII_MMD_CTRL_NOINCR);
    W16(b, 0);
    NcIoWrite(io, AX_ACCESS_PHY, AX88179_PHY_ID, MII_MMD_DATA, b, 2);

    /* mii_nway_restart */
    bmcr = PhyRead(io, MII_BMCR);
    if (bmcr & BMCR_ANENABLE) PhyWrite(io, MII_BMCR, (nc_u16)(bmcr | BMCR_ANRESTART));

    if (io->Status < 0) return io->Status;
    {
        nc_u32 i;
        for (i = 0; i < 6; i++) params->Mac[i] = s->Mac[i];
    }
    params->RxSize = AX_RX_BUFFER;
    params->RxCount = 4;
    params->TxHeader = 8;
    params->TxAlign = 1;
    params->TxBatch = 1;          /* Linux sends one frame per bulk-OUT transfer */
    params->TxSlots = 4;
    params->TxSize = 8 + NC_ETH_MAX + 2;
    params->Version = 0x179;
    params->RxConfig = s->RxCtl;
    params->RxConfigRead = s->RxCtlRead;
    return NC_OK;
}

long AxSetMac(void *chip, NET_IO *io, const nc_u8 mac[6])
{
    AX_STATE *s = chip;
    nc_u32 i;
    NcIoWrite(io, AX_ACCESS_MAC, AX_NODE_ID, 6, mac, 6);
    if (io->Status < 0) return io->Status;
    for (i = 0; i < 6; i++) s->Mac[i] = mac[i];
    return NC_OK;
}

long AxLink(void *chip, NET_IO *io, NET_LINK *link)
{
    AX_STATE *s = chip;
    nc_u8 b[4], status, queue[5];
    nc_u16 mode, phy;
    nc_u32 i, fifo = 0x40000000UL;
    const nc_u8 *row;

    link->Up = 0; link->Mbps = 0; link->FullDuplex = 0;
    link->RxLength = s->RxLength;

    /* Wait until the TX FIFO is not full (bit 30), restarting receive
     * control each time, as link_reset does. */
    for (i = 0; i < AX_FIFO_POLLS && (fifo & 0x40000000UL); i++) {
        if (i) NcIoSleep(io, 10);
        Write16(io, AX_RX_CTL, 0);
        Write16(io, AX_RX_CTL, s->RxCtl);
        NcIoRead(io, AX_FIFO_STATUS_CMD, 0x8c, 0, b, 4);
        fifo = R32(b);
    }
    if (io->Status < 0) return io->Status;
    if (fifo & 0x40000000UL) {
        /* As Linux: give up this round; the link stays down and Poll
         * asks again. */
        s->FifoBusy++;
        s->LinkUp = 0;
        return NC_OK;
    }

    mode = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN | AX_MEDIUM_RXFLOW_CTRLEN;
    status = Read8(io, AX_PHYSICAL_LINK_STATUS);
    phy = PhyRead(io, GMII_PHY_PHYSR);
    if (io->Status < 0) return io->Status;
    s->Phy = phy;
    if (!(phy & GMII_PHY_PHYSR_LINK)) {
        s->LinkUp = 0;
        return NC_OK;
    }
    if ((phy & GMII_PHY_PHYSR_SMASK) == GMII_PHY_PHYSR_GIGA) {
        mode |= AX_MEDIUM_GIGAMODE | AX_MEDIUM_EN_125MHZ;
        if (status & AX_USB_SS) row = BulkIn[0];
        else if (status & AX_USB_HS) row = BulkIn[1];
        else row = BulkIn[3];
        link->Mbps = 1000;
    } else if ((phy & GMII_PHY_PHYSR_SMASK) == GMII_PHY_PHYSR_100) {
        mode |= AX_MEDIUM_PS;
        if (status & (AX_USB_SS | AX_USB_HS)) row = BulkIn[2];
        else row = BulkIn[3];
        link->Mbps = 100;
    } else {
        row = BulkIn[3];
        link->Mbps = 10;
    }
    for (i = 0; i < 5; i++) queue[i] = row[i];
    NcIoWrite(io, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, queue, 5);
    s->RxLength = 1024UL * (queue[3] + 2);
    if (phy & GMII_PHY_PHYSR_FULL) mode |= AX_MEDIUM_FULL_DUPLEX;
    Write16(io, AX_MEDIUM_STATUS_MODE, mode);
    if (io->Status < 0) return io->Status;
    for (i = 0; i < 5; i++) s->Queue[i] = queue[i];
    s->Medium = mode;
    s->LinkUp = 1;
    link->Up = 1;
    link->FullDuplex = (phy & GMII_PHY_PHYSR_FULL) != 0;
    link->RxLength = s->RxLength;
    return NC_OK;
}

int AxPoll(void *chip, NET_IO *io)
{
    AX_STATE *s = chip;
    nc_u16 phy = PhyRead(io, GMII_PHY_PHYSR);
    if (io->Status < 0) return 0;
    return ((phy & GMII_PHY_PHYSR_LINK) != 0) != (s->LinkUp != 0);
}

long AxSetFilter(void *chip, NET_IO *io, nc_u32 filter, const nc_u8 *multicast, nc_u32 count)
{
    AX_STATE *s = chip;
    nc_u16 rxctl = AX_RX_CTL_START | AX_RX_CTL_AB | AX_RX_CTL_IPE;
    nc_u8 hash[8];
    nc_u32 i;

    /* ax88179_set_multicast: directed and broadcast are always on; the
     * core's software filter drops what NDIS did not ask for. */
    if (filter & NC_FILTER_PROMISCUOUS) {
        rxctl |= AX_RX_CTL_PRO;
    } else if ((filter & NC_FILTER_ALL_MULTICAST) || count > 64) {
        rxctl |= AX_RX_CTL_AMALL;
    } else if ((filter & NC_FILTER_MULTICAST) && count) {
        for (i = 0; i < 8; i++) hash[i] = 0;
        for (i = 0; i < count; i++) {
            nc_u32 bit = NcEtherCrc(multicast + 6 * i, 6) >> 26;
            hash[bit >> 3] |= (nc_u8)(1 << (bit & 7));
        }
        NcIoWrite(io, AX_ACCESS_MAC, AX_MULFLTARY, 8, hash, 8);
        rxctl |= AX_RX_CTL_AM;
    }
    Write16(io, AX_RX_CTL, rxctl);
    if (io->Status < 0) return io->Status;
    s->RxCtl = rxctl;
    return NC_OK;
}

long AxStop(void *chip, NET_IO *io)
{
    AX_STATE *s = chip;
    nc_u16 v;
    /* ax88179_stop */
    v = Read16(io, AX_MEDIUM_STATUS_MODE);
    Write16(io, AX_MEDIUM_STATUS_MODE, (nc_u16)(v & ~AX_MEDIUM_RECEIVE_EN));
    /* ax88179_unbind: receive stop, clocks off, PHY powered down */
    Write16(io, AX_RX_CTL, 0);
    Write8(io, AX_CLK_SELECT, 0);
    Write16(io, AX_PHYPWR_RSTCTL, 0);
    s->LinkUp = 0;
    return io->Status < 0 ? io->Status : NC_OK;
}

long AxSuspend(void *chip, NET_IO *io)
{
    AX_STATE *s = chip;
    nc_u16 v;
    /* ax88179_suspend without the WoL part. */
    v = Read16(io, AX_MEDIUM_STATUS_MODE);
    Write16(io, AX_MEDIUM_STATUS_MODE, (nc_u16)(v & ~AX_MEDIUM_RECEIVE_EN));
    v = Read16(io, AX_PHYPWR_RSTCTL);
    Write16(io, AX_PHYPWR_RSTCTL, (nc_u16)(v | AX_PHYPWR_RSTCTL_BZ | AX_PHYPWR_RSTCTL_IPRL));
    Write8(io, AX_CLK_SELECT, 0);
    Write16(io, AX_RX_CTL, 0);
    s->LinkUp = 0;
    return io->Status < 0 ? io->Status : NC_OK;
}

long AxResume(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params)
{
    /* ax88179_resume runs ax88179_reset again. */
    return AxStart(chip, io, probe, params);
}

int AxStatus(void *chip, const nc_u8 *data, nc_u32 length)
{
    (void)chip;
    if (length < 8) return -1;
    return (R32(data) & AX_INT_LINK) ? 1 : 0;
}

typedef struct AX_RX {
    NET_EMIT Emit;
    void *Context;
} AX_RX;

static void Deliver(void *context, const unsigned char *frame, unsigned int bytes,
                    unsigned long rawStatus)
{
    AX_RX *rx = context;
    (void)rawStatus;
    rx->Emit(rx->Context, frame, bytes);
}

long AxRxParse(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
               NET_RX_STATS *stats)
{
    AX_RX rx;
    AX_FRAME_STATS fs;
    (void)chip;
    rx.Emit = emit;
    rx.Context = context;
    /* Mode: no FCS (AxStart). AxFrameReceiveAuto also strips and counts
     * an FCS where a record has one (the safety net; the driver logs it). */
    if (!AxFrameReceiveAuto(data, length, NC_ETH_MAX, Deliver, &rx, &fs)) {
        stats->Frames = 0; stats->Dropped = 0;
        return NC_E_MALFORMED;
    }
    stats->Frames = fs.Delivered;
    stats->Dropped = fs.Dropped;
    stats->FcsStripped = fs.FcsStripped;
    return (long)fs.Delivered;
}

void AxTxHeader(void *chip, nc_u8 *hdr, nc_u32 length, nc_u32 maxPacket, int single)
{
    nc_u32 flags = 0;
    (void)chip; (void)single;
    /* ax88179_tx_fixup: the padding flag instead of a zero-length packet
     * when header and frame end on a packet boundary. */
    if (maxPacket && (length + 8) % maxPacket == 0) flags = 0x80008000UL;
    hdr[0] = (nc_u8)length; hdr[1] = (nc_u8)(length >> 8);
    hdr[2] = (nc_u8)(length >> 16); hdr[3] = (nc_u8)(length >> 24);
    hdr[4] = (nc_u8)flags; hdr[5] = (nc_u8)(flags >> 8);
    hdr[6] = (nc_u8)(flags >> 16); hdr[7] = (nc_u8)(flags >> 24);
}

int AxTxEnd(void *chip, nc_u8 *buffer, nc_u32 *total, nc_u32 capacity, nc_u32 maxPacket)
{
    (void)chip; (void)buffer; (void)total; (void)capacity; (void)maxPacket;
    return 0;                   /* the header's padding flag covers it */
}

const NET_CHIP NetPackageChip = {
    "ax179f",
    "usb-wdm-free AX179F (ASIX AX88179/AX88178A USB Gigabit Ethernet)",
    0x0001,
    AxProbe, AxStart, AxSetMac, AxLink, AxPoll, AxSetFilter, AxStop, AxSuspend, AxResume,
    AxStatus, AxRxParse, AxTxHeader, AxTxEnd
};

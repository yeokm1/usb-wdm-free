/* SPDX-License-Identifier: GPL-2.0-or-later
 * ASIX AX8817X based USB 2.0 Ethernet Devices
 * Copyright (C) 2003-2006 David Hollis <dhollis@davehollis.com>
 * Copyright (C) 2005 Phil Chang <pchang23@sbcglobal.net>
 * Copyright (C) 2006 James Painter <jamie.painter@iname.com>
 * Copyright (c) 2002-2003 TiVo Inc.
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * AX772F chip core (ethernet.md section 6.8). Adapted from Linux
 * drivers/net/usb at v7.3: asix_common.c (asix_check_host_enable and
 * __asix_mdio_read/__asix_mdio_write for MII access, asix_set_multicast
 * for SetFilter, asix_tx_fixup for TxHeader/TxEnd,
 * asix_rx_fixup_internal for RxParse) and asix_devices.c (ax88772_bind
 * for the MAC, PHY address and chip code; ax88772a_hw_reset and
 * ax88772_reset for Start; ax88772_mac_link_up/_link_down for Link;
 * ax88772_suspend for Suspend). The PHY's soft reset writes BMCR 0
 * first, as drivers/net/phy/ax88796b.c's asix_soft_reset does.
 *
 * The CoreChips SR9800 (ethernet.md section 6.9.1) runs on the same code
 * with its own bring-up (SrStart), MII access without the host-enable
 * wait, medium and receive defaults, transfer size and transmit pad
 * rule; those facts are from Linux sr9800.c and sr9800.h (facts only).
 *
 * Where this core departs from Linux, on purpose:
 * - Linux's phylink drives the PHY; here Start resets it and starts
 *   autonegotiation (IEEE 802.3 clause 22), and Link resolves the result
 *   from ADVERTISE and LPA, falling back to BMCR when the partner
 *   reports nothing (as ax88796b.c's asix_ax88772a_read_status does).
 * - A zero or multicast MAC is reported as read; the core's address
 *   policy (ethernet.md 4.8) replaces it, where Linux picks a random one.
 * - RxParse checks a whole transfer before it delivers any frame of it
 *   (netchip.h): a transfer Linux would refuse half way delivers nothing.
 * - Only chip code 0x20 (AX88772B, AX88772C) with its embedded PHY is
 *   served. An AX88772 (0x00) resets differently, an AX88772A (0x10)
 *   needs its PHY's link workarounds (ax88796b.c), and an external PHY
 *   needs the embedded one parked (ax88772_init_phy): none is expected
 *   behind the listed ids, so each is refused at Start.
 * - The PHY reset is polled for 500 ms; Resume tries the bring-up three
 *   times, as ax88772_resume does.
 */
#include "ax7chip.h"

/* The chip state must fit the core's (this package builds with
 * NET_CHIP_STATE 2048: build.cmd, test.cmd). */
typedef char A7_STATE_FITS[(sizeof(A7_STATE) <= NET_CHIP_STATE) ? 1 : -1];

static nc_u16 R16(const nc_u8 *b) { return (nc_u16)(b[0] | (b[1] << 8)); }
static nc_u32 R32(const nc_u8 *b)
{
    return (nc_u32)b[0] | ((nc_u32)b[1] << 8) | ((nc_u32)b[2] << 16) | ((nc_u32)b[3] << 24);
}

static void Cmd(NET_IO *io, nc_u8 request, nc_u16 value, nc_u16 index)
{
    NcIoWrite(io, request, value, index, 0, 0);
}

static nc_u16 Read16(NET_IO *io, nc_u8 request)
{
    nc_u8 b[2] = { 0, 0 };
    NcIoRead(io, request, 0, 0, b, 2);
    return R16(b);
}

/* asix_check_host_enable: software MII access, then wait for the chip to
 * hand the management interface to the host. 1 when it did. */
static int HostEnable(NET_IO *io)
{
    nc_u8 smsr;
    int i;
    for (i = 0; i < AX_HOST_EN_RETRIES; i++) {
        Cmd(io, AX_CMD_SET_SW_MII, 0, 0);
        if (io->Status < 0) return 0;
        NcIoSleep(io, 1);
        smsr = 0;
        if (NcIoTry(io, AX_CMD_STATMNGSTS_REG, 0, 0, &smsr, 1) < 0) continue;
        if (smsr & AX_HOST_EN) return 1;
    }
    return 0;
}

static nc_u16 MiiRead(A7_STATE *s, NET_IO *io, nc_u8 reg)
{
    nc_u8 b[2] = { 0, 0 };
    s->MiiReadOk = 0;
    if (s->Kind == A7_KIND_SR9800) {
        Cmd(io, AX_CMD_SET_SW_MII, 0, 0);         /* sr9800.c: no host-enable wait */
    } else if (!HostEnable(io)) {
        /* Linux returns the error; the sticky status ends the stage. */
        if (io->Status >= 0) io->Status = NC_E_DEVICE;
        return 0;
    }
    /* MiiReadOk: the value was read, even if handing the MII back fails
     * (a latched BMSR bit is consumed by the read itself). */
    s->MiiReadOk = (nc_u8)(NcIoRead(io, AX_CMD_READ_MII_REG, s->PhyAddr, reg, b, 2) >= 0);
    Cmd(io, AX_CMD_SET_HW_MII, 0, 0);
    return R16(b);
}

static void MiiWrite(A7_STATE *s, NET_IO *io, nc_u8 reg, nc_u16 value)
{
    nc_u8 b[2];
    b[0] = (nc_u8)value; b[1] = (nc_u8)(value >> 8);
    if (s->Kind == A7_KIND_SR9800) {
        Cmd(io, AX_CMD_SET_SW_MII, 0, 0);
    } else if (!HostEnable(io)) {
        /* Linux writes anyway; here a PHY the host cannot reach ends the
         * stage. */
        if (io->Status >= 0) io->Status = NC_E_DEVICE;
        return;
    }
    NcIoWrite(io, AX_CMD_WRITE_MII_REG, s->PhyAddr, reg, b, 2);
    Cmd(io, AX_CMD_SET_HW_MII, 0, 0);
}

static void Zero(void *p, nc_u32 n)
{
    nc_u8 *b = p;
    while (n--) *b++ = 0;
}

long A7Probe(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
             const nc_u32 *size, nc_u32 count, NET_PROBE *probe)
{
    long status;
    nc_u32 i;
    if (NcParseDevice(device, deviceSize, probe) != NC_OK) return NC_E_MALFORMED;
    /* usbnet_get_endpoints: one vendor interface with a bulk IN, a bulk
     * OUT and an interrupt IN; the first configuration that has one. */
    for (i = 0; i < count; i++) {
        if (!config[i] || !size[i]) continue;
        status = NcFindVendorInterface(config[i], size[i], 0, probe);
        if (status == NC_OK) {
            if (probe->Speed == NC_SPEED_SUPER) return NC_E_UNSUPPORTED;   /* a USB 2.0 chip */
            probe->ConfigIndex = (nc_u8)i;
            return NC_OK;
        }
        if (status == NC_E_MALFORMED) return status;
    }
    return NC_E_UNSUPPORTED;
}

/* FLAG_EEPROM_MAC: the ax88772b_info ids read EEPROM words 4-6;
 * ax88772_info (D-Link 2001:1A02) reads NODE_ID. */
static int EepromMac(const NET_PROBE *probe)
{
    return !(probe->Vendor == 0x2001 && probe->Product == 0x1a02);
}

static void ReadMac(A7_STATE *s, NET_IO *io, const NET_PROBE *probe)
{
    nc_u32 i;
    if (EepromMac(probe)) {
        for (i = 0; i < 3; i++) NcIoRead(io, AX_CMD_READ_EEPROM, (nc_u16)(0x04 + i), 0, s->Mac + 2 * i, 2);
    } else {
        NcIoRead(io, AX_CMD_READ_NODE_ID, 0, 0, s->Mac, 6);
    }
}

/* ax88772a_hw_reset for chip code 0x20, then the rest of ax88772_reset. */
static void HwReset(A7_STATE *s, NET_IO *io)
{
    Cmd(io, AX_CMD_WRITE_GPIOS, AX_GPIO_RSE, 0);
    NcIoSleep(io, 5);
    Cmd(io, AX_CMD_SW_PHY_SELECT, (nc_u16)(s->Embedded | AX_PHYSEL_SSEN), 0);
    NcIoSleep(io, 10);
    Cmd(io, AX_CMD_SW_RESET, AX_SWRESET_IPPD | AX_SWRESET_IPRL, 0);
    NcIoSleep(io, 10);
    Cmd(io, AX_CMD_SW_RESET, AX_SWRESET_IPRL, 0);
    NcIoSleep(io, 160);
    Cmd(io, AX_CMD_SW_RESET, AX_SWRESET_CLEAR, 0);
    Cmd(io, AX_CMD_SW_RESET, AX_SWRESET_IPRL, 0);
    NcIoSleep(io, 200);
    /* The AX88772B's bulk-IN burst (its 2 KiB setting). */
    Cmd(io, AX_QCTCTRL, 0x8000, 0x8001);
    /* The value as Linux writes it: IPG0 | IPG1 (0x1d), IPG2 in wIndex. */
    Cmd(io, AX_CMD_WRITE_IPG0, AX88772_IPG0_DEFAULT | AX88772_IPG1_DEFAULT, AX88772_IPG2_DEFAULT);
    NcIoWrite(io, AX_CMD_WRITE_NODE_ID, 0, 0, s->Mac, 6);
    Cmd(io, AX_CMD_WRITE_RX_CTL, AX_DEFAULT_RX_CTL, 0);
    Cmd(io, AX_CMD_WRITE_MEDIUM_MODE, AX88772_MEDIUM_DEFAULT, 0);
    Cmd(io, AX_CMD_WRITE_RX_CTL, AX_DEFAULT_RX_CTL, 0);
    s->RxCtl = AX_DEFAULT_RX_CTL;
    s->Medium = AX88772_MEDIUM_DEFAULT;
    s->RxCtlRead = Read16(io, AX_CMD_READ_RX_CTL);
    s->MediumRead = Read16(io, AX_CMD_READ_MEDIUM_STATUS);
}

static long PhyReset(A7_STATE *s, NET_IO *io);

/* The PHY: find it, then reset it and start autonegotiation. */
static long PhyStart(A7_STATE *s, NET_IO *io)
{
    int i;
    for (i = 0; i < 100; i++) {          /* asix_get_phyid: the PHY may not answer yet */
        s->PhyId1 = MiiRead(s, io, 0x02);
        if (io->Status < 0) return io->Status;
        if (s->PhyId1 != 0 && s->PhyId1 != 0xffff) break;
        NcIoSleep(io, 1);
    }
    if (i == 100) return NC_E_DEVICE;    /* no PHY answered */
    s->PhyId2 = MiiRead(s, io, 0x03);
    return PhyReset(s, io);
}

/* asix_soft_reset (BMCR 0, then reset, polled), then advertise 10/100
 * with pause and start autonegotiation (phylib's config_aneg). */
static long PhyReset(A7_STATE *s, NET_IO *io)
{
    int i;
    MiiWrite(s, io, MII_BMCR, 0);
    MiiWrite(s, io, MII_BMCR, BMCR_RESET);
    for (i = 0; i < A7_PHY_RESET_POLLS; i++) {
        NcIoSleep(io, 10);
        if (!(MiiRead(s, io, MII_BMCR) & BMCR_RESET)) break;
        if (io->Status < 0) return io->Status;
    }
    if (io->Status < 0) return io->Status;
    if (i == A7_PHY_RESET_POLLS) return NC_E_DEVICE;
    MiiWrite(s, io, MII_ADVERTISE, ADVERTISE_CSMA | ADVERTISE_10HALF | ADVERTISE_10FULL |
                                   ADVERTISE_100HALF | ADVERTISE_100FULL |
                                   ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);
    MiiWrite(s, io, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
    return io->Status < 0 ? io->Status : NC_OK;
}

static void FillParams(A7_STATE *s, NET_PARAMS *params)
{
    nc_u32 i;
    for (i = 0; i < 6; i++) params->Mac[i] = s->Mac[i];
    params->RxSize = s->RxTransfer;
    params->RxCount = 1;          /* one at a time: the stream is parsed in order (6.8) */
    params->TxHeader = 4;
    params->TxAlign = 1;
    params->TxBatch = 1;          /* Linux sends one frame per bulk-OUT transfer */
    params->TxSlots = 4;
    params->TxSize = 4 + NC_ETH_MAX + 4;
    params->RxConfig = s->RxCtl;
    params->RxConfigRead = s->RxCtlRead;
}

/* The SR9800's default mode (sr9800_set_default_mode): the PHY reset,
 * advertisement and autonegotiation, the medium, the gaps and the
 * receive mode. */
static void SrDefaultMode(A7_STATE *s, NET_IO *io)
{
    MiiWrite(s, io, MII_BMCR, BMCR_RESET);
    MiiWrite(s, io, MII_ADVERTISE, ADVERTISE_CSMA | ADVERTISE_10HALF | ADVERTISE_10FULL |
                                   ADVERTISE_100HALF | ADVERTISE_100FULL);
    MiiWrite(s, io, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
    Cmd(io, AX_CMD_WRITE_MEDIUM_MODE, S98_MEDIUM_MODE, 0);
    Cmd(io, AX_CMD_WRITE_IPG0, AX88772_IPG0_DEFAULT | AX88772_IPG1_DEFAULT, AX88772_IPG2_DEFAULT);
    Cmd(io, AX_CMD_WRITE_RX_CTL, S98_RX_MODE, 0);
    s->RxCtl = S98_RX_MODE;
    s->Medium = S98_MEDIUM_MODE;
    s->RxCtlRead = Read16(io, AX_CMD_READ_RX_CTL);
    s->MediumRead = Read16(io, AX_CMD_READ_MEDIUM_STATUS);
}

/* The SR9800: Linux's bind, then its reset (ethernet.md section 6.9.1). */
static long SrStart(A7_STATE *s, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params)
{
    nc_u8 b[2] = { 0, 0 };
    int i;

    Cmd(io, S98_LEDS_REQ, 0x4080, 0x4001);
    NcIoRead(io, AX_CMD_READ_NODE_ID, 0, 0, s->Mac, 6);
    NcIoRead(io, AX_CMD_READ_PHY_ID, 0, 0, b, 2);
    if (io->Status < 0) return io->Status;
    if (b[1] >= 32) return NC_E_DEVICE;
    s->PhyAddr = b[1];
    s->Embedded = (nc_u8)((s->PhyAddr & 0x1f) == AX_EMBD_PHY_ADDR);
    params->Version = 0x9800UL | ((nc_u32)s->PhyAddr << 16);
    Cmd(io, AX_CMD_SW_PHY_SELECT, s->Embedded, 0);
    /* The PHY's power-up (sr9800_phy_powerup). */
    Cmd(io, AX_CMD_SW_RESET, AX_SWRESET_IPPD | AX_SWRESET_IPRL, 0);
    NcIoSleep(io, 20);
    Cmd(io, AX_CMD_SW_RESET, AX_SWRESET_IPRL, 0);
    NcIoSleep(io, 600);
    Cmd(io, AX_CMD_SW_RESET, AX_SWRESET_CLEAR, 0);
    NcIoSleep(io, 20);
    Cmd(io, AX_CMD_SW_RESET, AX_SWRESET_IPRL, 0);
    Cmd(io, AX_CMD_WRITE_RX_CTL, 0, 0);
    for (i = 0; i < 100; i++) {           /* sr_get_phyid */
        s->PhyId1 = MiiRead(s, io, 0x02);
        if (io->Status < 0) return io->Status;
        if (s->PhyId1 != 0 && s->PhyId1 != 0xffff) break;
        NcIoSleep(io, 1);
    }
    if (i == 100) return NC_E_DEVICE;
    s->PhyId2 = MiiRead(s, io, 0x03);
    SrDefaultMode(s, io);
    if (probe->Speed == NC_SPEED_HIGH) {
        Cmd(io, S98_INBURST_REQ, 0x8100, 0x8147);
        s->RxTransfer = S98_RX_TRANSFER_HS;
    } else {
        Cmd(io, S98_INBURST_REQ, 0x8000, 0x8001);
        s->RxTransfer = A7_RX_TRANSFER;
    }
    /* sr9800_reset */
    Cmd(io, AX_CMD_WRITE_GPIOS, S98_GPIO_AT_RESET, 0);
    NcIoSleep(io, 5);
    NcIoRead(io, AX_CMD_READ_PHY_ID, 0, 0, b, 2);
    Cmd(io, AX_CMD_SW_PHY_SELECT, s->Embedded, 0);
    Cmd(io, AX_CMD_SW_RESET, AX_SWRESET_IPPD | S98_RESET_PHYLINE, 0);
    NcIoSleep(io, 150);
    Cmd(io, AX_CMD_SW_RESET, AX_SWRESET_CLEAR, 0);
    NcIoSleep(io, 150);
    Cmd(io, AX_CMD_SW_RESET, (nc_u16)(s->Embedded ? AX_SWRESET_IPRL : S98_RESET_EXTPHY), 0);
    NcIoSleep(io, 150);
    Cmd(io, AX_CMD_WRITE_RX_CTL, 0, 0);
    Cmd(io, AX_CMD_SW_RESET, S98_RESET_PHYLINE, 0);
    NcIoSleep(io, 150);
    Cmd(io, AX_CMD_SW_RESET, AX_SWRESET_IPRL | S98_RESET_PHYLINE, 0);
    NcIoSleep(io, 150);
    SrDefaultMode(s, io);
    NcIoWrite(io, AX_CMD_WRITE_NODE_ID, 0, 0, s->Mac, 6);
    if (io->Status < 0) return io->Status;
    FillParams(s, params);
    return NC_OK;
}

long A7Start(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params)
{
    A7_STATE *s = chip;
    nc_u8 b[2] = { 0, 0 };
    nc_u8 code = 0;
    long r;

    Zero(s, sizeof(*s));
    if (probe->Vendor == 0x0fe6 && probe->Product == 0x9800) {
        s->Kind = A7_KIND_SR9800;
        s->RxBase = S98_RX_MODE;
        return SrStart(s, io, probe, params);
    }
    s->Kind = A7_KIND_AX772B;
    s->RxBase = AX_DEFAULT_RX_CTL;
    s->RxTransfer = A7_RX_TRANSFER;

    ReadMac(s, io, probe);
    NcIoRead(io, AX_CMD_READ_PHY_ID, 0, 0, b, 2);
    s->PhyAddr = (nc_u8)(b[1] & 0x1f);
    if (io->Status < 0) return io->Status;
    if (b[1] >= 32) return NC_E_DEVICE;       /* asix_read_phy_addr: PHY_MAX_ADDR */
    s->Embedded = (nc_u8)(s->PhyAddr == AX_EMBD_PHY_ADDR);
    NcIoRead(io, AX_CMD_STATMNGSTS_REG, 0, 0, &code, 1);
    if (io->Status < 0) return io->Status;
    s->ChipCode = (nc_u8)(code & AX_CHIPCODE_MASK);
    params->Version = (nc_u32)s->ChipCode | ((nc_u32)s->PhyAddr << 8);
    if (s->ChipCode != AX_AX88772B_CHIPCODE || !s->Embedded) return NC_E_UNSUPPORTED;

    HwReset(s, io);
    if (io->Status < 0) return io->Status;
    r = PhyStart(s, io);
    if (r < 0) return r;
    FillParams(s, params);
    return NC_OK;
}

long A7SetMac(void *chip, NET_IO *io, const nc_u8 mac[6])
{
    A7_STATE *s = chip;
    nc_u32 i;
    /* A write that fails stays pending: Poll asks for Link, which writes
     * the address again before the link is applied. */
    for (i = 0; i < 6; i++) s->WantMac[i] = mac[i];
    s->MacPending = 1;
    NcIoWrite(io, AX_CMD_WRITE_NODE_ID, 0, 0, mac, 6);
    if (io->Status < 0) return io->Status;
    for (i = 0; i < 6; i++) s->Mac[i] = mac[i];
    s->MacPending = 0;
    return NC_OK;
}

/* ax88772_mac_link_down, and for the AX88772A/B PHY (003b1861) the PHY
 * reset Linux does when the link goes (asix_ax88772a_link_change_notify
 * in ax88796b.c: some partners otherwise leave a stale LPA). DownPending
 * stays set until all of it is done. */
static long LinkDown(A7_STATE *s, NET_IO *io)
{
    s->DownPending = 1;
    Cmd(io, AX_CMD_WRITE_MEDIUM_MODE, 0, 0);
    if (io->Status < 0) return io->Status;
    s->Medium = 0;
    if (s->Kind == A7_KIND_AX772B && s->PhyId1 == 0x003b && s->PhyId2 == 0x1861) {
        long r = PhyReset(s, io);
        if (r < 0) return r;
    }
    s->DownPending = 0;
    return NC_OK;
}

long A7Link(void *chip, NET_IO *io, NET_LINK *link)
{
    A7_STATE *s = chip;
    nc_u16 bmsr, bmcr, adv, lpa, common, medium;
    int up, fast, full, txPause, rxPause;

    link->Up = 0; link->Mbps = 0; link->FullDuplex = 0;
    link->RxLength = s->RxTransfer;
    /* Until this Link has finished, LinkPending stays set and Poll asks
     * for it again: a Link that fails part way is run again, whatever the
     * PHY then reports (the core treats a failed Link as link down).
     * LinkUp keeps what was last applied. */
    s->LinkPending = 1;

    /* The core runs Link with the data path closed (netusb.c ApplyLink,
     * after Quiesce or before the data path starts), so the receive
     * stream may be restarted here. When RxParse refused several
     * transfers in a row the stream is out of step (data was lost and the
     * chip's transfers do not start on a record): receive off and on
     * again, which empties the chip's receive buffer (inferred: no Linux
     * file says so), and the stream is read from the next record. */
    if (s->BadRun >= A7_REALIGN_AFTER) {
        Cmd(io, AX_CMD_WRITE_RX_CTL, 0, 0);
        NcIoSleep(io, 2);
        Cmd(io, AX_CMD_WRITE_RX_CTL, (nc_u16)(s->RxCtl ? s->RxCtl : s->RxBase), 0);
        if (io->Status < 0) return io->Status;
        Zero(&s->Rx, sizeof(s->Rx));
        s->BadRun = 0;
        s->Realigns++;
    }
    /* Work a failed call left: the address, and the link-down work, which
     * is finished whatever the carrier does now (an interrupted PHY reset
     * leaves the PHY with autonegotiation off). */
    if (s->MacPending) {
        long r = A7SetMac(s, io, s->WantMac);
        if (r < 0) return r;
    }
    /* BMSR's link bit latches low: a first read that shows it clear while
     * the link was up means it went down since the last read, even if it
     * is back now. The link-down work is done for that too, so the
     * AX88772A/B PHY is reset after every drop (Poll keeps the same
     * evidence). */
    bmsr = MiiRead(s, io, MII_BMSR);
    if (s->MiiReadOk && s->LinkUp && !(bmsr & BMSR_LSTATUS)) s->DownPending = 1;
    if (io->Status < 0) return io->Status;
    if (s->DownPending) {
        long r = LinkDown(s, io);
        if (r < 0) return r;
        s->LinkUp = 0;
    }

    bmsr = MiiRead(s, io, MII_BMSR);
    bmcr = MiiRead(s, io, MII_BMCR);
    if (io->Status < 0) return io->Status;
    up = (bmsr & BMSR_LSTATUS) && (!(bmcr & BMCR_ANENABLE) || (bmsr & BMSR_ANEGCOMPLETE));
    if (!up) {
        if (s->LinkUp) {
            long r = LinkDown(s, io);
            if (r < 0) return r;
        }
        s->LinkUp = 0;
        s->LinkPending = 0;
        return NC_OK;
    }
    adv = MiiRead(s, io, MII_ADVERTISE);
    lpa = MiiRead(s, io, MII_LPA);
    if (io->Status < 0) return io->Status;
    common = (nc_u16)(adv & lpa);
    txPause = rxPause = 0;
    if ((bmcr & BMCR_ANENABLE) &&
        (common & (ADVERTISE_100FULL | ADVERTISE_100HALF | ADVERTISE_10FULL | ADVERTISE_10HALF))) {
        fast = (common & (ADVERTISE_100FULL | ADVERTISE_100HALF)) != 0;
        full = fast ? (common & ADVERTISE_100FULL) != 0 : (common & ADVERTISE_10FULL) != 0;
        /* IEEE 802.3 Annex 28B (Table 28B-3), with both pause bits
         * advertised here: a partner with PAUSE pauses both ways; one with
         * ASM_DIR only sends pause frames, which this end obeys. */
        if (full && (lpa & ADVERTISE_PAUSE_CAP)) txPause = rxPause = 1;
        else if (full && (lpa & ADVERTISE_PAUSE_ASYM)) rxPause = 1;
    } else {
        /* No common ability read (parallel detection or a forced link):
         * BMCR's speed and duplex. */
        fast = (bmcr & 0x2000) != 0;
        full = (bmcr & 0x0100) != 0;
    }
    /* ax88772_mac_link_up: TFC with tx_pause, RFC with rx_pause. The
     * SR9800 (sr9800_link_reset): its default with PS and FD cleared as
     * the link needs, RFC and TFC always (it advertises no pause). */
    if (s->Kind == A7_KIND_SR9800) {
        medium = S98_MEDIUM_MODE;
        if (!fast) medium &= (nc_u16)~AX_MEDIUM_PS;
        if (!full) medium &= (nc_u16)~AX_MEDIUM_FD;
        txPause = rxPause = 1;
    } else {
        medium = AX_MEDIUM_AC | AX_MEDIUM_RE;
        if (full) medium |= AX_MEDIUM_FD;
        if (fast) medium |= AX_MEDIUM_PS;
        if (txPause) medium |= AX_MEDIUM_TFC;
        if (rxPause) medium |= AX_MEDIUM_RFC;
    }
    Cmd(io, AX_CMD_WRITE_MEDIUM_MODE, medium, 0);
    if (io->Status < 0) return io->Status;
    s->Medium = medium;
    s->LinkUp = 1;
    s->DownPending = 0;
    s->LinkPending = 0;
    s->Mbps100 = (nc_u8)fast; s->Full = (nc_u8)full;
    s->Pause = (nc_u8)(txPause | (rxPause << 1));
    link->Up = 1;
    link->Mbps = fast ? 100 : 10;
    link->FullDuplex = full;
    return NC_OK;
}

int A7Poll(void *chip, NET_IO *io)
{
    A7_STATE *s = chip;
    nc_u16 bmsr = MiiRead(s, io, MII_BMSR);
    /* A drop seen here, even one already over, is kept for Link, also
     * when handing the MII back then failed. */
    if (s->MiiReadOk && s->LinkUp && !(bmsr & BMSR_LSTATUS)) s->DownPending = 1;
    if (io->Status < 0) return s->DownPending;
    /* A receive stream out of step is restarted by Link (above); a
     * link-down step or a filter that failed is done again (the core
     * applies the filter after Link when the link is up). */
    if (s->BadRun >= A7_REALIGN_AFTER || s->DownPending || s->LinkPending || s->MacPending) return 1;
    if (s->FilterPending && s->LinkUp) return 1;
    return ((bmsr & BMSR_LSTATUS) != 0) != (s->LinkUp != 0);
}

long A7SetFilter(void *chip, NET_IO *io, nc_u32 filter, const nc_u8 *multicast, nc_u32 count)
{
    A7_STATE *s = chip;
    nc_u16 rxctl = s->RxBase;
    nc_u8 hash[8];
    nc_u32 i;

    /* asix_set_multicast: directed and broadcast always; the core's
     * software filter drops what NDIS did not ask for. */
    if (filter & NC_FILTER_PROMISCUOUS) {
        rxctl |= AX_RX_CTL_PRO;
    } else if ((filter & NC_FILTER_ALL_MULTICAST) || count > AX_MAX_MCAST) {
        rxctl |= AX_RX_CTL_AMALL;
    } else if ((filter & NC_FILTER_MULTICAST) && count) {
        for (i = 0; i < 8; i++) hash[i] = 0;
        for (i = 0; i < count; i++) {
            nc_u32 bit = NcEtherCrc(multicast + 6 * i, 6) >> 26;
            hash[bit >> 3] |= (nc_u8)(1 << (bit & 7));
        }
        NcIoWrite(io, AX_CMD_WRITE_MULTI_FILTER, 0, 0, hash, 8);
        rxctl |= AX_RX_CTL_AM;
    }
    Cmd(io, AX_CMD_WRITE_RX_CTL, rxctl, 0);
    if (io->Status < 0) {
        s->FilterPending = 1;
        return io->Status;
    }
    s->RxCtl = rxctl;
    s->FilterPending = 0;
    return NC_OK;
}

long A7Stop(void *chip, NET_IO *io)
{
    A7_STATE *s = chip;
    Cmd(io, AX_CMD_WRITE_MEDIUM_MODE, 0, 0);
    Cmd(io, AX_CMD_WRITE_RX_CTL, 0, 0);
    MiiWrite(s, io, MII_BMCR, BMCR_PDOWN);
    s->Medium = 0;
    s->RxCtl = 0;
    s->LinkUp = 0;
    return io->Status < 0 ? io->Status : NC_OK;
}

long A7Suspend(void *chip, NET_IO *io)
{
    A7_STATE *s = chip;
    nc_u16 medium = Read16(io, AX_CMD_READ_MEDIUM_STATUS);
    Cmd(io, AX_CMD_WRITE_MEDIUM_MODE, (nc_u16)(medium & ~AX_MEDIUM_RE), 0);
    s->LinkUp = 0;
    return io->Status < 0 ? io->Status : NC_OK;
}

long A7Resume(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params)
{
    long r = NC_E_DEVICE;
    int i;
    /* ax88772_resume: the reset again, up to three times. A failed
     * transfer is not carried from one try to the next; a refused chip is
     * final. */
    for (i = 0; i < 3; i++) {
        io->Status = NC_OK;
        r = A7Start(chip, io, probe, params);
        if (r >= 0 || r == NC_E_UNSUPPORTED) break;
    }
    return r;
}

int A7Status(void *chip, const nc_u8 *data, nc_u32 length)
{
    (void)chip;
    /* asix_status's report (struct ax88172_int_data): byte 2 bit 0. */
    if (length < 8) return -1;
    return (data[2] & 1) ? 1 : 0;
}

/* ---- receive: asix_rx_fixup_internal ---------------------------------- */

typedef struct A7_WALK {
    A7_RX Rx;
    nc_u32 Frames, Dropped;
    int Resync;
} A7_WALK;

/* Walk one transfer from w->Rx. With frame NULL nothing is stored or
 * delivered: the checking pass. Returns 0, or NC_E_MALFORMED with the
 * walk stopped where Linux would refuse the transfer. */
static long Walk(A7_WALK *w, const nc_u8 *data, nc_u32 length, nc_u8 *frame,
                 NET_EMIT emit, void *context)
{
    A7_RX *rx = &w->Rx;
    nc_u32 offset = 0, copy;
    nc_u16 size;

    /* A frame continues from the last transfer: check the header that
     * should follow it before taking any of it. */
    if (rx->Remaining && (((nc_u32)rx->Remaining + 1) & ~1UL) + 4 <= length) {
        nc_u32 at = ((nc_u32)rx->Remaining + 1) & ~1UL;
        nc_u32 h = R32(data + at);
        if ((h & 0x7ff) != ((~h >> 16) & 0x7ff)) {
            w->Resync = 1;
            if (rx->Keep) w->Dropped++;
            rx->Remaining = 0; rx->Split = 0; rx->Keep = 0; rx->Header = 0;
        }
    }

    while (offset + 2 <= length) {
        if (!rx->Remaining) {
            /* A half header from the last transfer is completed first
             * (Linux tests for a trailing half header before it, which
             * would lose it when only two bytes came). */
            if (!rx->Split && length - offset == 2) {
                rx->Header = R16(data + offset);
                rx->Split = 1;
                offset += 2;
                break;
            }
            if (rx->Split) {
                rx->Header |= (nc_u32)R16(data + offset) << 16;
                rx->Split = 0;
                offset += 2;
            } else {
                if (length - offset < 4) return NC_E_MALFORMED;
                rx->Header = R32(data + offset);
                offset += 4;
            }
            size = (nc_u16)(rx->Header & 0x7ff);
            if (size != (nc_u16)((~rx->Header >> 16) & 0x7ff)) return NC_E_MALFORMED;
            if (size > A7_FRAME_MAX) return NC_E_MALFORMED;
            rx->Size = size;
            rx->Remaining = size;
            rx->Keep = (nc_u8)(size >= NC_ETH_MIN && size <= NC_ETH_MAX);
            if (!size) { w->Dropped++; continue; }
        }
        copy = length - offset;
        if (copy > rx->Remaining) copy = rx->Remaining;
        if (frame && rx->Keep) {
            nc_u32 at = (nc_u32)rx->Size - rx->Remaining, i;
            for (i = 0; i < copy; i++) frame[at + i] = data[offset + i];
        }
        rx->Remaining = (nc_u16)(rx->Remaining - copy);
        if (!rx->Remaining) {
            if (rx->Keep) {
                w->Frames++;
                if (frame) emit(context, frame, rx->Size);
            } else {
                w->Dropped++;
            }
            rx->Keep = 0;
        }
        offset += (copy + 1) & ~1UL;
    }
    if (offset != length) return NC_E_MALFORMED;
    return 0;
}

long A7RxParse(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
               NET_RX_STATS *stats)
{
    A7_STATE *s = chip;
    A7_WALK w;

    stats->Frames = 0; stats->Dropped = 0; stats->FcsStripped = 0;
    /* The checking pass, on a copy of the stream's position. */
    w.Rx = s->Rx; w.Frames = 0; w.Dropped = 0; w.Resync = 0;
    if (Walk(&w, data, length, 0, 0, 0) < 0) {
        /* Linux resets the stream (reset_asix_rx_fixup_info) and drops
         * the transfer; a frame being carried is lost with it. */
        Zero(&s->Rx, sizeof(s->Rx));
        s->Malformed++;
        s->BadRun++;
        return NC_E_MALFORMED;
    }
    s->BadRun = 0;
    w.Frames = 0; w.Dropped = 0; w.Resync = 0;
    w.Rx = s->Rx;
    (void)Walk(&w, data, length, s->Frame, emit, context);
    s->Rx = w.Rx;
    if (w.Resync) s->Resyncs++;
    stats->Frames = w.Frames;
    stats->Dropped = w.Dropped;
    return (long)w.Frames;
}

void A7RxReset(void *chip)
{
    A7_STATE *s = chip;
    /* A transfer was lost: whatever was carried is gone, and the next
     * transfer is read from its first byte as a header. */
    if (s->Rx.Remaining || s->Rx.Split) s->Resyncs++;
    Zero(&s->Rx, sizeof(s->Rx));
}

/* ---- transmit: asix_tx_fixup ------------------------------------------ */

void A7TxHeader(void *chip, nc_u8 *hdr, nc_u32 length, nc_u32 maxPacket, int single)
{
    nc_u32 word = ((length ^ 0xffffUL) << 16) + length;
    (void)chip; (void)maxPacket; (void)single;
    hdr[0] = (nc_u8)word; hdr[1] = (nc_u8)(word >> 8);
    hdr[2] = (nc_u8)(word >> 16); hdr[3] = (nc_u8)(word >> 24);
}

int A7TxEnd(void *chip, nc_u8 *buffer, nc_u32 *total, nc_u32 capacity, nc_u32 maxPacket)
{
    A7_STATE *s = chip;
    nc_u32 unit = maxPacket;
    /* A transfer that ends on a packet boundary gets the pad word
     * 0xffff0000 (a zero-length record) instead of a zero-length packet.
     * The SR9800 gets it where Linux's sr_tx_fixup puts it, when header
     * and frame are a multiple of maxpacket - 1, and no terminator
     * otherwise (ethernet.md section 6.9.1). */
    if (s->Kind == A7_KIND_SR9800) unit = maxPacket ? maxPacket - 1 : 0;
    if (unit && *total % unit == 0 && *total + 4 <= capacity) {
        buffer[*total] = 0; buffer[*total + 1] = 0;
        buffer[*total + 2] = 0xff; buffer[*total + 3] = 0xff;
        *total += 4;
    }
    return 0;
}

const NET_CHIP NetPackageChip = {
    "ax772f",
    "usb-wdm-free AX772F (ASIX AX88772B/AX88772C, CoreChips SR9800 USB Fast Ethernet)",
    0x0001,
    A7Probe, A7Start, A7SetMac, A7Link, A7Poll, A7SetFilter, A7Stop, A7Suspend, A7Resume,
    A7Status, A7RxParse, A7TxHeader, A7TxEnd, A7RxReset
};

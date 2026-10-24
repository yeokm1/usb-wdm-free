/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host tests for the RTL815X chip core (rtlchip.c) and the shared pure
 * helpers (netchip.c), against a register-file model of the chip: the PLA
 * and USB OCP spaces with the byte-enable rules of r8152.c's
 * generic_ocp_read/write, the PHY OCP window behind PLA_OCP_GPHY_BASE, and
 * the self-clearing bits the bring-up waits for. The model and every
 * descriptor here are synthetic; no RTL8152/8153 was read. Expected values
 * come from Linux r8152.c at f47d2236, as cited at each check.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtlchip.h"
#pragma warning(disable: 4127)

static unsigned long assertions;
static void Check(int ok, const char *expression, int line)
{
    assertions++;
    if (!ok) { printf("FAIL %d: %s\n", line, expression); exit(1); }
}
#define CHECK(x) Check((x), #x, __LINE__)

/* ---------------- the chip model ---------------- */

typedef struct ENTRY {
    int In;
    nc_u16 Value, Index, Length;
    nc_u8 Data[8];
} ENTRY;

typedef struct MODEL {
    nc_u8 Pla[65536], Usb[65536];
    nc_u16 Phy[65536 / 2];      /* PHY OCP space, word addressed by addr/2 */
    nc_u32 Count, FailAt, FailFirst, Slept;
    nc_u32 Writes;              /* SET_REGS */
    nc_u32 VersionTcr0;
    int LinkUp;
    ENTRY Last[64];             /* ring of the last requests */
} MODEL;

static nc_u16 PhyAddr(MODEL *m, nc_u16 pla)
{
    nc_u16 base = (nc_u16)(m->Pla[PLA_OCP_GPHY_BASE] | (m->Pla[PLA_OCP_GPHY_BASE + 1] << 8));
    return (nc_u16)(base | (pla & 0x0fff));
}

static void PhyWritten(MODEL *m, nc_u16 addr, nc_u16 v)
{
    if (addr == OCP_BASE_MII + MII_BMCR * 2) v = (nc_u16)(v & ~BMCR_RESET);  /* reset completes */
    if (addr == OCP_PHY_PATCH_CMD) {
        /* PATCH_READY follows PATCH_REQUEST. */
        nc_u16 st = m->Phy[OCP_PHY_PATCH_STAT / 2];
        m->Phy[OCP_PHY_PATCH_STAT / 2] = (nc_u16)((v & PATCH_REQUEST) ? st | PATCH_READY : st & ~PATCH_READY);
    }
    m->Phy[addr / 2] = v;
}

static nc_u8 *Space(MODEL *m, nc_u16 index) { return (index & MCU_TYPE_PLA) ? m->Pla : m->Usb; }

static long Control(void *context, int in, nc_u8 request, nc_u16 value, nc_u16 index,
                    void *data, nc_u16 length, nc_u16 *actual)
{
    MODEL *m = context;
    nc_u8 *b = data, *space;
    nc_u16 byen = (nc_u16)(index & 0xff), k;
    ENTRY *e = &m->Last[m->Count % 64];
    m->Count++;
    e->In = in; e->Value = value; e->Index = index; e->Length = length;
    memset(e->Data, 0, 8);
    if (!in) memcpy(e->Data, b, length < 8 ? length : 8);
    if (m->FailAt == m->Count || m->Count <= m->FailFirst) return -5;
    CHECK(request == RTL8152_REQ_GET_REGS);
    CHECK((index & ~0x1ff) == 0);                  /* type | byte enables only */
    CHECK(!(value & 3) && length && !(length & 3) && (nc_u32)value + length <= 0x10000);
    *actual = length;
    space = Space(m, index);
    if (in) {
        CHECK(length <= 64);                       /* generic_ocp_read's limit */
        for (k = 0; k < length; k++) {
            nc_u16 a = (nc_u16)(value + k);
            if ((index & MCU_TYPE_PLA) && a >= 0xb000 && a < 0xc000) {
                nc_u16 w = m->Phy[PhyAddr(m, (nc_u16)(a & ~1)) / 2];
                b[k] = (nc_u8)((a & 1) ? w >> 8 : w);
            } else {
                b[k] = space[a];
            }
        }
        if ((index & MCU_TYPE_PLA) && value == PLA_TCR0 && length >= 4) {
            b[2] = (nc_u8)(m->VersionTcr0 >> 16); b[3] = (nc_u8)(m->VersionTcr0 >> 24);
        }
        return 0;
    }
    m->Writes++;
    CHECK(length <= 512);
    /* A partial-dword write carries exactly one dword. */
    if (byen != BYTE_EN_DWORD) CHECK(length == 4 && byen);
    for (k = 0; k < length; k++) {
        nc_u16 a = (nc_u16)(value + k);
        if (byen != BYTE_EN_DWORD && !(byen & (1 << (k & 3)))) continue;
        if ((index & MCU_TYPE_PLA) && a >= 0xb000 && a < 0xc000) {
            if (a & 1) {
                nc_u16 pa = PhyAddr(m, (nc_u16)(a & ~1));
                PhyWritten(m, pa, (nc_u16)((m->Phy[pa / 2] & 0x00ff) | (b[k] << 8)));
            } else {
                nc_u16 pa = PhyAddr(m, a);
                m->Phy[pa / 2] = (nc_u16)((m->Phy[pa / 2] & 0xff00) | b[k]);
            }
            continue;
        }
        space[a] = b[k];
    }
    /* Self-clearing and status bits the bring-up polls. */
    m->Pla[PLA_CR] &= ~CR_RST;
    return 0;
}

static void Sleeper(void *context, nc_u32 ms) { ((MODEL *)context)->Slept += ms; }

static nc_u16 Word(const nc_u8 *s, nc_u16 a) { return (nc_u16)(s[a] | (s[a + 1] << 8)); }
static nc_u32 Dword(const nc_u8 *s, nc_u16 a) { return Word(s, a) | ((nc_u32)Word(s, (nc_u16)(a + 2)) << 16); }

static MODEL *m;
static NET_IO io;
static nc_u8 state[NET_CHIP_STATE];
static const nc_u8 backupMac[6] = { 0x00, 0xe0, 0x4c, 0x68, 0x01, 0x02 };

static void Reset(nc_u32 tcr0)
{
    memset(m, 0, sizeof(*m));
    m->VersionTcr0 = tcr0;
    m->Pla[PLA_OOB_CTRL] = LINK_LIST_READY | FIFO_EMPTY;
    m->Pla[PLA_TCR0 + 1] = (nc_u8)(TCR0_TX_EMPTY >> 8);
    m->Pla[PLA_BOOT_CTRL] = AUTOLOAD_DONE;
    m->Pla[0xe000 + 1] = 0x01;                    /* r8153_aldps_en's wait bit */
    m->Phy[OCP_PHY_STATUS / 2] = PHY_STAT_LAN_ON;
    memcpy(m->Pla + PLA_BACKUP, backupMac, 6);
    memset(m->Pla + PLA_IDR, 0x11, 6);            /* differs from BACKUP */
    m->Pla[PLA_RCR] = (nc_u8)RCR_ACPT_ALL;
    m->Usb[USB_USB_CTRL] = RX_AGG_DISABLE | RX_ZERO_EN;
    m->Pla[PLA_CPCR] = CPCR_RX_VLAN;
    memset(&io, 0, sizeof(io));
    io.Context = m; io.Control = Control; io.Sleep = Sleeper;
    memset(state, 0, sizeof(state));
}

static NET_PROBE Probe(nc_u8 speed)
{
    NET_PROBE p;
    memset(&p, 0, sizeof(p));
    p.Speed = speed;
    p.In = 0x81; p.Out = 0x02; p.Interrupt = 0x83;
    p.InMax = p.OutMax = (nc_u16)(speed == NC_SPEED_SUPER ? 1024 : speed == NC_SPEED_HIGH ? 512 : 64);
    p.IntMax = 2;
    return p;
}

/* ---------------- register access encoding ---------------- */

static void Encoding(void)
{
    nc_u32 tcr;
    static const struct { nc_u16 id; nc_u8 ver; } map[] = {
        { 0x4c00, RTL_VER_01 }, { 0x4c10, RTL_VER_02 }, { 0x5c00, RTL_VER_03 },
        { 0x5c10, RTL_VER_04 }, { 0x5c20, RTL_VER_05 }, { 0x5c30, RTL_VER_06 },
        { 0x4800, RTL_VER_07 }, { 0x6000, RTL_VER_08 }, { 0x6010, RTL_VER_09 },
        { 0x6400, RTL_VER_14 },
    };
    unsigned i;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        CHECK(RtlVersionFromTcr0((nc_u32)map[i].id << 16) == map[i].ver);
        /* bits outside VERSION_MASK are ignored */
        CHECK(RtlVersionFromTcr0(((nc_u32)map[i].id << 16) | 0x830f0000UL | 0xffff) == map[i].ver);
    }
    /* RTL8156/8156B/8157/8159 and the test chip: later, not this core. */
    for (tcr = 0; tcr < 0x10000; tcr++) {
        nc_u8 v = RtlVersionFromTcr0(tcr << 16);
        int known = 0;
        for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) if ((tcr & VERSION_MASK) == map[i].id) known = 1;
        CHECK(known ? v != RTL_VER_UNKNOWN : v == RTL_VER_UNKNOWN);
    }
}

/* ---------------- bring-up per version ---------------- */

static void BringUp(nc_u16 id, nc_u8 speed, nc_u8 ver)
{
    NET_PROBE p = Probe(speed);
    NET_PARAMS par;
    RTL_STATE *s = (RTL_STATE *)state;
    Reset((nc_u32)id << 16);
    CHECK(sizeof(RTL_STATE) <= NET_CHIP_STATE);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK(s->Version == ver && s->Timeouts == 0);
    CHECK(m->Count < 2000);
    CHECK(memcmp(par.Mac, ver == RTL_VER_01 ? m->Pla + PLA_IDR : backupMac, 6) == 0);
    if (ver != RTL_VER_01) {
        CHECK(memcmp(m->Pla + PLA_IDR, backupMac, 6) == 0);       /* set_ethernet_addr */
        CHECK(m->Pla[PLA_CRWECR] == CRWECR_NORAML || ver == RTL_VER_14);
    }
    /* rx_buf_sz (rtl_ops_init) */
    if (ver <= RTL_VER_02 || ver == RTL_VER_07) CHECK(par.RxSize == 16384);
    else if (ver <= RTL_VER_06) CHECK(par.RxSize == (speed == NC_SPEED_SUPER ? 32768UL : 16384UL));
    else CHECK(par.RxSize == 32768);
    CHECK(par.TxHeader == 8 && par.TxAlign == 4 && par.TxSize == 16384 && par.TxSlots == 4);
    /* After up: receive accept off, RX aggregation on, VLAN untag off,
     * RMS 1522, the MAC not yet receiving (CR RE/TE clear). */
    CHECK((Dword(m->Pla, PLA_RCR) & RCR_ACPT_ALL) == 0);
    CHECK((Word(m->Usb, USB_USB_CTRL) & (RX_AGG_DISABLE | RX_ZERO_EN)) == 0);
    CHECK((Word(m->Pla, PLA_CPCR) & CPCR_RX_VLAN) == 0);
    CHECK(Word(m->Pla, PLA_RMS) == RTL8152_RMS);
    CHECK((m->Pla[PLA_CR] & (CR_RE | CR_TE)) == 0);
    CHECK(m->Pla[PLA_OOB_CTRL] & LINK_LIST_READY);
    CHECK((m->Pla[PLA_OOB_CTRL] & NOW_IS_OOB) == 0);
    /* rtl8152_set_speed: autonegotiation restarted with a PHY reset;
     * 10/100 advertised, 1000 full with a gigabit PHY, pause advertised. */
    CHECK((m->Phy[(OCP_BASE_MII + MII_BMCR * 2) / 2] & (BMCR_ANENABLE | BMCR_ANRESTART)) ==
          (BMCR_ANENABLE | BMCR_ANRESTART));
    CHECK((m->Phy[(OCP_BASE_MII + MII_ADVERTISE * 2) / 2] & 0x0de0) == 0x0de0);
    if (ver == RTL_VER_01 || ver == RTL_VER_02 || ver == RTL_VER_07)
        CHECK(m->Phy[(OCP_BASE_MII + MII_CTRL1000 * 2) / 2] == 0);
    else
        CHECK(m->Phy[(OCP_BASE_MII + MII_CTRL1000 * 2) / 2] == ADVERTISE_1000FULL);
    /* EEE advertisement (rtl_eee_enable with eee_en) */
    if (ver >= RTL_VER_03 && ver != RTL_VER_07)
        CHECK(m->Phy[OCP_EEE_ADV / 2] == (MDIO_EEE_1000T | MDIO_EEE_100TX));
    /* No wake-up source left on. */
    CHECK((Word(m->Pla, PLA_CFG_WOL) & MAGIC_EN) == 0);
    CHECK((Word(m->Pla, PLA_CONFIG5) & (UWF_EN | BWF_EN | MWF_EN)) == 0);
    if (ver == RTL_VER_04 || ver == RTL_VER_05 || ver == RTL_VER_06 || ver == RTL_VER_03)
        CHECK(Word(m->Usb, USB_CONNECT_TIMER) == 1);
}

static void LinkAndFilter(nc_u16 id, nc_u8 ver)
{
    NET_PROBE p = Probe(NC_SPEED_HIGH);
    NET_PARAMS par;
    NET_LINK l;
    RTL_STATE *s = (RTL_STATE *)state;
    static const nc_u8 groups[4][6] = {
        { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 },   /* bin 31 */
        { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 },   /* bin 62 */
        { 0x01, 0x00, 0x5e, 0x7f, 0xff, 0xfa },   /* bin 43 */
        { 0x33, 0x33, 0xff, 0x12, 0x34, 0x56 },   /* bin 2 */
    };
    nc_u32 before;
    Reset((nc_u32)id << 16);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    /* A filter set while the carrier is off is only remembered. */
    before = m->Writes;
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_DIRECTED | NC_FILTER_BROADCAST |
                                   NC_FILTER_MULTICAST, &groups[0][0], 4) == NC_OK);
    CHECK(m->Writes == before);
    /* Link down: nothing applied. */
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && !l.Up);
    CHECK(NetPackageChip.Poll(state, &io) == 0);
    /* Link up at 1000 full: enable and the receive mode. */
    m->Pla[PLA_PHYSTATUS] = RTL_LINK_STATUS | RTL_1000BPS | RTL_FULL_DUP;
    CHECK(NetPackageChip.Poll(state, &io) == 1);
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK);
    CHECK(l.Up && l.Mbps == 1000 && l.FullDuplex && l.RxLength == par.RxSize && s->Carrier);
    CHECK((m->Pla[PLA_CR] & (CR_RE | CR_TE)) == (CR_RE | CR_TE));
    CHECK((Word(m->Pla, PLA_MISC_1) & RXDY_GATED_EN) == 0);
    CHECK((Dword(m->Pla, PLA_RCR) & RCR_ACPT_ALL) == (RCR_AB | RCR_APM | RCR_AM));
    /* _rtl8152_set_rx_mode: mc_filter[bit >> 5] |= 1 << (bit & 31), then
     * PLA_MAR = le32(swab32(mc[1])), le32(swab32(mc[0])).
     * bins 31, 2 -> mc[0] = 0x80000004; bins 62, 43 -> mc[1] = 0x40000800 */
    CHECK(m->Pla[PLA_MAR + 0] == 0x40 && m->Pla[PLA_MAR + 1] == 0x00 &&
          m->Pla[PLA_MAR + 2] == 0x08 && m->Pla[PLA_MAR + 3] == 0x00);
    CHECK(m->Pla[PLA_MAR + 4] == 0x80 && m->Pla[PLA_MAR + 5] == 0x00 &&
          m->Pla[PLA_MAR + 6] == 0x00 && m->Pla[PLA_MAR + 7] == 0x04);
    if (ver >= RTL_VER_03 && ver != RTL_VER_07) {
        nc_u32 early = par.RxSize - (1522 + 24 + 8);
        CHECK(Word(m->Usb, USB_RX_EARLY_SIZE) == (ver <= RTL_VER_06 ? early / 4 : early / 8));
        CHECK((Word(m->Pla, PLA_TCR1) & IFG_MASK) == IFG_96NS);
    }
    /* Promiscuous and all-multicast. */
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_PROMISCUOUS, NULL, 0) == NC_OK);
    CHECK((Dword(m->Pla, PLA_RCR) & RCR_ACPT_ALL) == RCR_ACPT_ALL);
    CHECK(Dword(m->Pla, PLA_MAR) == 0xffffffffUL && Dword(m->Pla, PLA_MAR + 4) == 0xffffffffUL);
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_ALL_MULTICAST | NC_FILTER_DIRECTED, NULL, 0) == NC_OK);
    CHECK((Dword(m->Pla, PLA_RCR) & RCR_ACPT_ALL) == (RCR_AB | RCR_APM | RCR_AM));
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_DIRECTED | NC_FILTER_BROADCAST, NULL, 0) == NC_OK);
    CHECK((Dword(m->Pla, PLA_RCR) & RCR_ACPT_ALL) == (RCR_AB | RCR_APM));
    CHECK(Dword(m->Pla, PLA_MAR) == 0 && Dword(m->Pla, PLA_MAR + 4) == 0);
    /* Link up again: no second enable. */
    before = m->Writes;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Up);
    CHECK(m->Writes == before);
    /* 100 half: speed, duplex. */
    m->Pla[PLA_PHYSTATUS] = RTL_LINK_STATUS | RTL_100BPS;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Mbps == 100 && !l.FullDuplex);
    /* Link down: disable gates receive and resets the NIC. */
    m->Pla[PLA_PHYSTATUS] = 0;
    CHECK(NetPackageChip.Poll(state, &io) == 1);
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && !l.Up && !s->Carrier);
    CHECK((Dword(m->Pla, PLA_RCR) & RCR_ACPT_ALL) == 0);
    CHECK(Word(m->Pla, PLA_MISC_1) & RXDY_GATED_EN);
    CHECK(s->Timeouts == 0);
    /* Suspend (down) enters OOB with the wake filters; Stop likewise. */
    CHECK(NetPackageChip.Suspend(state, &io) == NC_OK);
    CHECK(m->Pla[PLA_OOB_CTRL] & NOW_IS_OOB);
    CHECK((Dword(m->Pla, PLA_RCR) & RCR_ACPT_ALL) == (RCR_APM | RCR_AM | RCR_AB));
    /* Resume is the whole bring-up, version kept. */
    m->Count = 0;
    CHECK(NetPackageChip.Resume(state, &io, &p, &par) == NC_OK);
    CHECK(s->Version == ver && (m->Pla[PLA_OOB_CTRL] & NOW_IS_OOB) == 0);
    CHECK(NetPackageChip.Stop(state, &io) == NC_OK);
    CHECK(m->Pla[PLA_OOB_CTRL] & NOW_IS_OOB);
}

static void Failures(void)
{
    NET_PROBE p = Probe(NC_SPEED_HIGH);
    NET_PARAMS par;
    nc_u32 total, n;
    static const nc_u8 laa[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
    /* Unknown version: refused after reading TCR0, nothing written. */
    Reset(0x7020UL << 16);                        /* RTL_VER_10, RTL8156 */
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_E_UNSUPPORTED);
    CHECK(m->Count == 1 && m->Writes == 0);
    /* The lower edge's cleanup after a refused start touches nothing. */
    CHECK(NetPackageChip.Stop(state, &io) == NC_OK && NetPackageChip.Suspend(state, &io) == NC_OK);
    CHECK(m->Count == 1);
    /* __rtl_get_hw_ver retries: two failed reads of TCR0 are tolerated,
     * three refuse the device. */
    Reset(0x5c30UL << 16);
    m->FailFirst = 2;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    Reset(0x5c30UL << 16);
    m->FailFirst = 3;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) < 0);
    CHECK(m->Count == 3 && m->Writes == 0);
    /* Every failing request stops the bring-up at once. */
    Reset(0x5c30UL << 16);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    total = m->Count;
    for (n = 2; n <= total; n += 7) {
        Reset(0x5c30UL << 16);
        m->FailAt = n;
        CHECK(NetPackageChip.Start(state, &io, &p, &par) < 0);
        CHECK(m->Count == n);
    }
    /* SetMac: CRWECR config, six bytes of IDR (two writes), normal. */
    Reset(0x6010UL << 16);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    m->Count = 0;
    CHECK(NetPackageChip.SetMac(state, &io, laa) == NC_OK);
    CHECK(m->Count == 4);
    CHECK(m->Last[0].Value == (PLA_CRWECR & ~3) && m->Last[0].Index == (MCU_TYPE_PLA | (BYTE_EN_BYTE << 0)));
    CHECK(m->Last[1].Value == PLA_IDR && m->Last[1].Index == (MCU_TYPE_PLA | 0xff) && m->Last[1].Length == 4);
    CHECK(m->Last[2].Value == PLA_IDR + 4 && m->Last[2].Index == (MCU_TYPE_PLA | 0x33) && m->Last[2].Length == 4);
    CHECK(m->Last[3].Value == (PLA_CRWECR & ~3));
    CHECK(memcmp(m->Pla + PLA_IDR, laa, 6) == 0 && m->Pla[PLA_IDR + 6] == 0);
}

/* Word and byte writes at unaligned offsets use shifted byte enables. */
static void ByteEnables(void)
{
    NET_PROBE p = Probe(NC_SPEED_HIGH);
    NET_PARAMS par;
    Reset(0x4c10UL << 16);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    m->Count = 0;
    m->Pla[PLA_PHYSTATUS] = RTL_LINK_STATUS | RTL_100BPS | RTL_FULL_DUP;
    {
        NET_LINK l;
        CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Up);
    }
    /* PLA_PHYSTATUS 0xe908 is dword aligned: the word read uses 0x33. */
    CHECK(m->Last[0].In && m->Last[0].Value == PLA_PHYSTATUS && m->Last[0].Index == (MCU_TYPE_PLA | 0x33));
}

/* ---------------- receive, transmit, interrupt ---------------- */

static nc_u32 emitted, bytes;
static void Emit(void *c, const nc_u8 *f, nc_u32 n) { (void)c; (void)f; emitted++; bytes += n; }

static nc_u32 Agg(nc_u8 *b, const nc_u32 *len, nc_u32 count)
{
    nc_u32 at = 0, i, k;
    for (i = 0; i < count; i++) {
        nc_u32 pkt = len[i] + 4;
        memset(b + at, 0, 24);
        b[at] = (nc_u8)pkt; b[at + 1] = (nc_u8)(pkt >> 8);
        for (k = 0; k < pkt; k++) b[at + 24 + k] = (nc_u8)(i + k);
        at += 24 + pkt;
        while (at & 7) b[at++] = 0xee;
    }
    return at;
}

static void Datapath(void)
{
    nc_u8 b[16384], h[8], irq[2] = { 0, 0 };
    NET_RX_STATS st;
    nc_u32 lens[4] = { 60, 1514, 99, 1518 }, n, len;
    n = Agg(b, lens, 4);
    emitted = bytes = 0;
    CHECK(NetPackageChip.RxParse(state, b, n, Emit, NULL, &st) == 3);
    CHECK(emitted == 3 && bytes == 60 + 1514 + 99 && st.Dropped == 1);
    /* A trailing descriptor shorter than ETH_ZLEN ends the walk. */
    memset(b + n, 0, 32);
    emitted = 0;
    CHECK(NetPackageChip.RxParse(state, b, n + 32, Emit, NULL, &st) == 3 && emitted == 3);
    /* Truncations: a frame past the end rejects the transfer, nothing
     * emitted; a cut inside padding or descriptor-only tail is fine. */
    for (len = 0; len <= n; len++) {
        long r;
        emitted = 0;
        r = NetPackageChip.RxParse(state, b, len, Emit, NULL, &st);
        CHECK(r < 0 ? emitted == 0 : (nc_u32)r == emitted);
    }
    CHECK(NetPackageChip.RxParse(state, b, 59, Emit, NULL, &st) == 0);
    /* Lengths with high bits: only bits 0-14 count. */
    n = Agg(b, lens, 1);
    b[3] = 0xff; b[2] = 0xff; b[1] |= 0x80;
    emitted = 0;
    CHECK(NetPackageChip.RxParse(state, b, n, Emit, NULL, &st) == 1 && emitted == 1);

    NetPackageChip.TxHeader(state, h, 60, 512, 1);
    CHECK(h[0] == 60 && h[1] == 0 && h[2] == 0 && h[3] == 0xc0 && h[4] == 0 && h[7] == 0);
    NetPackageChip.TxHeader(state, h, 1514, 64, 0);
    CHECK(h[0] == (1514 & 0xff) && h[1] == (1514 >> 8) && h[3] == 0xc0);
    n = 1024;
    CHECK(NetPackageChip.TxEnd(state, b, &n, sizeof(b), 512) == 0 && n == 1024);

    CHECK(NetPackageChip.Status(state, irq, 1) == -1);
    CHECK(NetPackageChip.Status(state, irq, 2) == 0);
    irq[0] = INTR_LINK;
    CHECK(NetPackageChip.Status(state, irq, 2) == 1);
}

/* ---------------- probe ---------------- */

static void Probes(void)
{
    static const nc_u8 dev[18] = { 18,1,0x10,0x02,0,0,0,64, 0xda,0x0b, 0x53,0x81, 0x00,0x30, 1,2,6, 2 };
    /* RTL8153-like: configuration 1 vendor, 2 CDC-ECM (synthetic). */
    static const nc_u8 vendor[] = {
        9,2,39,0,1,1,0,0xa0,0x32,
        9,4,0,0,3,0xff,0xff,0,0,
        7,5,0x81,2,0x00,0x02,0,
        7,5,0x02,2,0x00,0x02,0,
        7,5,0x83,3,2,0,8
    };
    static const nc_u8 ecm[] = {
        9,2,80,0,2,2,0,0xa0,0x32,
        9,4,0,0,1,2,6,0,0,
        5,0x24,0,0x10,1,
        5,0x24,6,0,1,
        13,0x24,15,3,0,0,0,0,0xea,5,0,0,0,
        7,5,0x83,3,16,0,8,
        9,4,1,0,0,10,0,0,0,
        9,4,1,1,2,10,0,0,0,
        7,5,0x81,2,0x00,0x02,0,
        7,5,0x02,2,0x00,0x02,0
    };
    nc_u8 copy[sizeof(vendor)];
    const nc_u8 *cfg[2];
    nc_u32 size[2], i;
    NET_PROBE p;
    CHECK(sizeof(ecm) == 80);
    cfg[0] = vendor; size[0] = sizeof(vendor);
    cfg[1] = ecm; size[1] = sizeof(ecm);
    CHECK(NetPackageChip.Probe(dev, 18, cfg, size, 2, &p) == NC_OK);
    CHECK(p.ConfigIndex == 0 && p.ConfigValue == 1 && p.Speed == NC_SPEED_HIGH && p.Product == 0x8153);
    CHECK(p.In == 0x81 && p.Out == 0x02 && p.Interrupt == 0x83 && p.IntMax == 2);
    /* "The vendor mode is not always config #1". */
    cfg[0] = ecm; size[0] = sizeof(ecm);
    cfg[1] = vendor; size[1] = sizeof(vendor);
    CHECK(NetPackageChip.Probe(dev, 18, cfg, size, 2, &p) == NC_OK && p.ConfigIndex == 1);
    /* A configuration that could not be read is skipped. */
    cfg[0] = NULL; size[0] = 0;
    CHECK(NetPackageChip.Probe(dev, 18, cfg, size, 2, &p) == NC_OK && p.ConfigIndex == 1);
    /* ECM only: not this driver's. */
    cfg[0] = ecm; size[0] = sizeof(ecm);
    CHECK(NetPackageChip.Probe(dev, 18, cfg, size, 1, &p) == NC_E_UNSUPPORTED);
    /* rtl_check_vendor_ok: endpoint numbers 1, 2, 3. */
    memcpy(copy, vendor, sizeof(vendor));
    copy[20] = 0x84;
    cfg[0] = copy; size[0] = sizeof(copy);
    CHECK(NetPackageChip.Probe(dev, 18, cfg, size, 1, &p) == NC_E_UNSUPPORTED);
    for (i = 0; i < sizeof(vendor) * 8; i++) {
        long r;
        memcpy(copy, vendor, sizeof(vendor));
        copy[i / 8] ^= (nc_u8)(1 << (i % 8));
        r = NetPackageChip.Probe(dev, 18, cfg, size, 1, &p);
        CHECK(r == NC_OK || r == NC_E_UNSUPPORTED || r == NC_E_MALFORMED);
    }
}

int main(void)
{
    m = malloc(sizeof(*m));
    CHECK(m != NULL);
    Encoding();
    BringUp(0x4c00, NC_SPEED_HIGH, RTL_VER_01);
    BringUp(0x4c10, NC_SPEED_HIGH, RTL_VER_02);
    BringUp(0x4c10, NC_SPEED_FULL, RTL_VER_02);
    BringUp(0x4800, NC_SPEED_HIGH, RTL_VER_07);
    BringUp(0x5c00, NC_SPEED_SUPER, RTL_VER_03);
    BringUp(0x5c10, NC_SPEED_HIGH, RTL_VER_04);
    BringUp(0x5c10, NC_SPEED_SUPER, RTL_VER_04);
    BringUp(0x5c20, NC_SPEED_SUPER, RTL_VER_05);
    BringUp(0x5c30, NC_SPEED_HIGH, RTL_VER_06);
    BringUp(0x5c30, NC_SPEED_FULL, RTL_VER_06);
    BringUp(0x6000, NC_SPEED_SUPER, RTL_VER_08);
    BringUp(0x6010, NC_SPEED_HIGH, RTL_VER_09);
    BringUp(0x6400, NC_SPEED_SUPER, RTL_VER_14);
    LinkAndFilter(0x4c10, RTL_VER_02);
    LinkAndFilter(0x5c30, RTL_VER_06);
    LinkAndFilter(0x6010, RTL_VER_09);
    LinkAndFilter(0x6400, RTL_VER_14);
    Failures();
    ByteEnables();
    Datapath();
    Probes();
    printf("PASS: %lu RTL815X chip-core checks\n", assertions);
    return 0;
}

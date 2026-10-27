/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host tests for the RTL815X chip core (rtlchip.c) and the shared pure
 * helpers (netchip.c), against a register-file model of the chip: the MAC
 * and USB register spaces with the byte-lane rules of the vendor request
 * (rtlchip.h), the PHY window behind MR_PHY_PAGE, and the self-clearing
 * bits the bring-up waits for. The model and every descriptor here are
 * synthetic; no RTL8152/8153/8156 was read. The expected values are register
 * facts of the chips (outcomes of the bring-up, link and filter
 * sequences), not code.
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
    int NoGrant;                /* the PHY never grants a patch window */
    ENTRY Last[64];             /* ring of the last requests */
} MODEL;

static nc_u16 PhyAddr(MODEL *m, nc_u16 pla)
{
    nc_u16 base = (nc_u16)(m->Pla[MR_PHY_PAGE] | (m->Pla[MR_PHY_PAGE + 1] << 8));
    return (nc_u16)(base | (pla & 0x0fff));
}

static void PhyWritten(MODEL *m, nc_u16 addr, nc_u16 v)
{
    if (addr == MII(MII_CONTROL)) v = (nc_u16)(v & ~MIICTL_RESET);  /* reset completes */
    if (addr == PR_PATCH_CMD) {
        /* PATCH_GRANTED follows PATCH_ASK. */
        nc_u16 st = m->Phy[PR_PATCH_STATE / 2];
        m->Phy[PR_PATCH_STATE / 2] = (nc_u16)((v & PATCH_ASK) && !m->NoGrant ? st | PATCH_GRANTED : st & ~PATCH_GRANTED);
    }
    m->Phy[addr / 2] = v;
}

static nc_u8 *Space(MODEL *m, nc_u16 index) { return (index & RTL_SPACE_MAC) ? m->Pla : m->Usb; }

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
    CHECK(request == RTL_REQ);
    CHECK((index & ~0x1ff) == 0);                  /* type | byte enables only */
    CHECK(!(value & 3) && length && !(length & 3) && (nc_u32)value + length <= 0x10000);
    *actual = length;
    space = Space(m, index);
    if (in) {
        CHECK(length <= 64);                       /* the read limit */
        for (k = 0; k < length; k++) {
            nc_u16 a = (nc_u16)(value + k);
            if ((index & RTL_SPACE_MAC) && a >= 0xb000 && a < 0xc000) {
                nc_u16 w = m->Phy[PhyAddr(m, (nc_u16)(a & ~1)) / 2];
                b[k] = (nc_u8)((a & 1) ? w >> 8 : w);
            } else {
                b[k] = space[a];
            }
        }
        if ((index & RTL_SPACE_MAC) && value == MR_TX_CONFIG && length >= 4) {
            b[2] = (nc_u8)(m->VersionTcr0 >> 16); b[3] = (nc_u8)(m->VersionTcr0 >> 24);
        }
        return 0;
    }
    m->Writes++;
    CHECK(length <= 512);
    /* A partial-dword write carries exactly one dword. */
    if (byen != RTL_LANES_ALL) CHECK(length == 4 && byen);
    for (k = 0; k < length; k++) {
        nc_u16 a = (nc_u16)(value + k);
        if (byen != RTL_LANES_ALL && !(byen & (1 << (k & 3)))) continue;
        if ((index & RTL_SPACE_MAC) && a >= 0xb000 && a < 0xc000) {
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
    m->Pla[MR_COMMAND] &= ~CMD_RESET;
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
    m->Pla[MR_OOB] = OOB_LIST_READY | OOB_FIFOS_EMPTY;
    m->Pla[MR_TX_CONFIG + 1] = (nc_u8)(TXC_EMPTY >> 8);
    m->Pla[MR_BOOT] = BOOT_LOADED;
    m->Pla[0xe000 + 1] = 0x01;                    /* MR_POWER_STATE bit 8, polled after ALDPS off */
    m->Phy[PR_STATE / 2] = PHYST_LAN_ON;
    memcpy(m->Pla + MR_NODE_ID_SAVED, backupMac, 6);
    memset(m->Pla + MR_NODE_ID, 0x11, 6);            /* differs from BACKUP */
    m->Pla[MR_RX_ACCEPT] = (nc_u8)ACC_MASK;
    m->Usb[UR_CTRL] = CTRL_RX_AGG_OFF | CTRL_RX_ZERO;
    m->Pla[MR_CPLUS] = CPLUS_RX_VLAN;
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
    static const nc_u16 map[] = {
        0x4c00, 0x4c10, 0x5c00, 0x5c10, 0x5c20, 0x5c30, 0x4800, 0x6000, 0x6010, 0x6400,
        0x7020, 0x7030, 0x7400, 0x7410, 0x7420
    };
    unsigned i;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        CHECK(RtlChipId((nc_u32)map[i] << 16) == map[i]);
        /* bits outside TXC_ID_MASK are ignored */
        CHECK(RtlChipId(((nc_u32)map[i] << 16) | 0x830f0000UL | 0xffff) == map[i]);
    }
    /* RTL8157/8159 (1030, 2020) and the test chip 7010: not this core. */
    for (tcr = 0; tcr < 0x10000; tcr++) {
        nc_u16 v = RtlChipId(tcr << 16);
        int known = 0;
        for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) if ((tcr & TXC_ID_MASK) == map[i]) known = 1;
        CHECK(known ? v == (tcr & TXC_ID_MASK) : v == 0);
    }
}

/* ---------------- bring-up per chip ---------------- */

static int Is8152(nc_u16 id) { return id == ID_4C00 || id == ID_4C10 || id == ID_4800; }
static int Is8153(nc_u16 id) { return (id & 0xff00) == 0x5c00; }
static int Is8156(nc_u16 id) { return id == ID_7020 || id == ID_7030 || id == ID_7400 || id == ID_7410 || id == ID_7420; }
static int Is2G5(nc_u16 id) { return Is8156(id) && id != ID_7420; }

static void BringUp(nc_u16 id, nc_u8 speed)
{
    NET_PROBE p = Probe(speed);
    NET_PARAMS par;
    RTL_STATE *s = (RTL_STATE *)state;
    Reset((nc_u32)id << 16);
    CHECK(sizeof(RTL_STATE) <= NET_CHIP_STATE);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK(s->Id == id && s->Timeouts == 0);
    CHECK(m->Count < 2000);
    CHECK(par.Version == id);
    CHECK(memcmp(par.Mac, id == ID_4C00 ? m->Pla + MR_NODE_ID : backupMac, 6) == 0);
    if (id != ID_4C00) {
        CHECK(memcmp(m->Pla + MR_NODE_ID, backupMac, 6) == 0);       /* copied from the saved address */
        CHECK(m->Pla[MR_CFG_LOCK] == CFG_LOCKED || id == ID_6400);
    }
    /* bulk-IN buffer */
    if (Is8152(id)) CHECK(par.RxSize == 16384);
    else if (Is8153(id)) CHECK(par.RxSize == (speed == NC_SPEED_SUPER ? 32768UL : 16384UL));
    else CHECK(par.RxSize == 32768);
    CHECK(par.TxHeader == 8 && par.TxAlign == 4 && par.TxSize == 16384 && par.TxSlots == 4);
    /* After up: receive accept off, RX aggregation on, VLAN untag off,
     * RMS 1522, the MAC not yet receiving (CR RE/TE clear). */
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == 0);
    CHECK((Word(m->Usb, UR_CTRL) & (CTRL_RX_AGG_OFF | CTRL_RX_ZERO)) == 0);
    if (Is8156(id)) CHECK((Word(m->Pla, MR_RX_CONFIG1) & 0x00c0) == 0);
    else CHECK((Word(m->Pla, MR_CPLUS) & CPLUS_RX_VLAN) == 0);
    CHECK(Word(m->Pla, MR_RX_MAX_LEN) == RX_MAX_LEN);
    CHECK((m->Pla[MR_COMMAND] & (CMD_RX_ON | CMD_TX_ON)) == 0);
    CHECK(m->Pla[MR_OOB] & OOB_LIST_READY);
    CHECK((m->Pla[MR_OOB] & OOB_ACTIVE) == 0);
    /* Autonegotiation restarted with a PHY reset;
     * 10/100 advertised, 1000 full with a gigabit PHY, pause advertised. */
    CHECK((m->Phy[(MII(MII_CONTROL)) / 2] & (MIICTL_AN_ENABLE | MIICTL_AN_RESTART)) ==
          (MIICTL_AN_ENABLE | MIICTL_AN_RESTART));
    CHECK((m->Phy[(MII(MII_AN_ADV)) / 2] & 0x0de0) == 0x0de0);
    if (Is8152(id))
        CHECK(m->Phy[(MII(MII_GIG_CONTROL)) / 2] == 0);
    else
        CHECK(m->Phy[(MII(MII_GIG_CONTROL)) / 2] == GIGCTL_1000_FULL);
    /* EEE advertisement: none on the 7020 (EEE stays off there) */
    if (id == ID_7020)
        CHECK(m->Phy[PR_EEE_ADV / 2] == 0 && m->Phy[PR_EEE_ADV2 / 2] == 0);
    else if (!Is8152(id))
        CHECK(m->Phy[PR_EEE_ADV / 2] == (EEEADV_1000T | EEEADV_100TX));
    if (Is8156(id) && id != ID_7020) CHECK(m->Phy[PR_EEE_ADV2 / 2] == EEEADV2_2G5);
    /* 2.5G advertised only at SuperSpeed; never by the 8153D or older chips */
    CHECK(m->Phy[PR_MGIG_CTRL / 2] == (((Is2G5(id) && speed == NC_SPEED_SUPER) ? MGIG_2G5 : 0) |
                                       (id == ID_7030 ? MGIG_FAST_RETRAIN : 0)));
    if (Is8156(id)) {
        /* 8156 core: FIFO and size thresholds for a 1500-byte MTU, aggregation on */
        CHECK(Word(m->Pla, MR_RX_FIFO_FULL) == 512 && Word(m->Pla, MR_RX_FIFO_EMPTY) == 1024);
        CHECK(Word(m->Pla, MR_TXFIFO_FULL) == 128 && Word(m->Pla, MR_TXFIFO_TH) == 8);
        CHECK(m->Pla[MR_TX_MAX_PKT] == 12 * 1024 / 64);
        CHECK(Dword(m->Usb, UR_RX_BUF_TH) == 0x00600400UL);
        CHECK(Word(m->Pla, MR_POWER1) == 0x0403 && (Word(m->Pla, MR_POWER2) & 0x80ff) == 0x8003);
        CHECK(Word(m->Usb, UR_U1U2_TMR) == 500 && Word(m->Usb, UR_MSC_TMR) == 0x0fff);
        CHECK((m->Usb[UR_ECM_OPTION] & 0x20) && !(m->Usb[UR_ECM_OP] & 0x01));
        CHECK(Word(m->Usb, UR_U2P3) & 0x0001);
        CHECK((Word(m->Usb, UR_LPM_CFG) & 1) == (speed == NC_SPEED_SUPER));
        if (id == ID_7400 || id == ID_7410 || id == ID_7420)
            CHECK(Word(m->Usb, UR_FC_TIMER) == (0x8000 | 75) && (Word(m->Usb, UR_FW_TASK) & 2) &&
                  (Word(m->Usb, UR_FW_CTRL) & 0x0108) == 0x0100);
    }
    /* No wake-up source left on. */
    CHECK((Word(m->Pla, MR_WOL) & WOL_MAGIC) == 0);
    CHECK((Word(m->Pla, MR_CFG5) & CFG5_WAKE_FRAMES) == 0);
    if (Is8153(id))
        CHECK(Word(m->Usb, UR_CONNECT_TMR) == 1);
}

static void LinkAndFilter(nc_u16 id)
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
    m->Pla[MR_LINK] = LNK_UP | LNK_1000 | LNK_FULL;
    CHECK(NetPackageChip.Poll(state, &io) == 1);
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK);
    CHECK(l.Up && l.Mbps == 1000 && l.FullDuplex && l.RxLength == par.RxSize && s->Carrier);
    CHECK((m->Pla[MR_COMMAND] & (CMD_RX_ON | CMD_TX_ON)) == (CMD_RX_ON | CMD_TX_ON));
    CHECK((Word(m->Pla, MR_MISC1) & MISC1_RX_GATE) == 0);
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == (ACC_BCAST | ACC_MY_PHYS | ACC_MCAST));
    /* Hash bins 32-63 in bytes 0-3, bins 0-31 in bytes 4-7, the highest
     * bin of each half in the first byte: bins 62, 43 -> 40 00 08 00;
     * bins 31, 2 -> 80 00 00 04. */
    CHECK(m->Pla[MR_HASH + 0] == 0x40 && m->Pla[MR_HASH + 1] == 0x00 &&
          m->Pla[MR_HASH + 2] == 0x08 && m->Pla[MR_HASH + 3] == 0x00);
    CHECK(m->Pla[MR_HASH + 4] == 0x80 && m->Pla[MR_HASH + 5] == 0x00 &&
          m->Pla[MR_HASH + 6] == 0x00 && m->Pla[MR_HASH + 7] == 0x04);
    if (!Is8152(id)) {
        nc_u32 early = par.RxSize - (1522 + 24 + 8);
        CHECK(Word(m->Usb, UR_RX_EARLY_LEN) == (Is8153(id) ? early / 4 : early / 8));
        CHECK((Word(m->Pla, MR_TX_CONFIG1) & IFG_FIELD) == IFG_SHORT);
    }
    /* Promiscuous and all-multicast. */
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_PROMISCUOUS, NULL, 0) == NC_OK);
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == ACC_MASK);
    CHECK(Dword(m->Pla, MR_HASH) == 0xffffffffUL && Dword(m->Pla, MR_HASH + 4) == 0xffffffffUL);
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_ALL_MULTICAST | NC_FILTER_DIRECTED, NULL, 0) == NC_OK);
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == (ACC_BCAST | ACC_MY_PHYS | ACC_MCAST));
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_DIRECTED | NC_FILTER_BROADCAST, NULL, 0) == NC_OK);
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == (ACC_BCAST | ACC_MY_PHYS));
    CHECK(Dword(m->Pla, MR_HASH) == 0 && Dword(m->Pla, MR_HASH + 4) == 0);
    /* Link up again: no second enable. */
    before = m->Writes;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Up);
    CHECK(m->Writes == before);
    /* 100 half: speed, duplex. */
    m->Pla[MR_LINK] = LNK_UP | LNK_100;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Mbps == 100 && !l.FullDuplex);
    /* Link down: disable gates receive and resets the NIC. */
    m->Pla[MR_LINK] = 0;
    CHECK(NetPackageChip.Poll(state, &io) == 1);
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && !l.Up && !s->Carrier);
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == 0);
    CHECK(Word(m->Pla, MR_MISC1) & MISC1_RX_GATE);
    CHECK(s->Timeouts == 0);
    /* Suspend (down) enters OOB with the wake filters; Stop likewise. */
    CHECK(NetPackageChip.Suspend(state, &io) == NC_OK);
    CHECK(m->Pla[MR_OOB] & OOB_ACTIVE);
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == (ACC_MY_PHYS | ACC_MCAST | ACC_BCAST));
    /* Resume is the whole bring-up, version kept. */
    m->Count = 0;
    CHECK(NetPackageChip.Resume(state, &io, &p, &par) == NC_OK);
    CHECK(s->Id == id && (m->Pla[MR_OOB] & OOB_ACTIVE) == 0);
    CHECK(NetPackageChip.Stop(state, &io) == NC_OK);
    CHECK(m->Pla[MR_OOB] & OOB_ACTIVE);
}

/* The 8156 core: multi-gigabit link reports, the receive settings at
 * link-up, a PHY that loads from flash, a refused patch window, and an
 * RTL8156 with its pairs reversed. */
static void Multigig(void)
{
    NET_PROBE p = Probe(NC_SPEED_SUPER);
    NET_PARAMS par;
    NET_LINK l;
    RTL_STATE *s = (RTL_STATE *)state;

    Reset(0x7410UL << 16);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && s->Timeouts == 0);
    m->Pla[MR_LINK] = LNK_UP | LNK_FULL | LNK_TX_FLOW | LNK_RX_FLOW;
    m->Pla[MR_LINK + 1] = (nc_u8)(LNK_2500 >> 8);
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Up && l.Mbps == 2500 && l.FullDuplex);
    CHECK((Word(m->Pla, MR_POWER4) & 0x0040) == 0);                 /* idle slow-down off at 2.5G */
    CHECK(Word(m->Usb, UR_RX_EARLY_TMO) == 640 / 8 && Word(m->Usb, UR_PM_STATUS) == 15000 / 8);
    CHECK(Word(m->Usb, UR_RX_EARLY_LEN) == (32768 - (1522 + 24 + 8)) / 8);
    CHECK((Word(m->Usb, UR_RX_AGG_NUM) & 0x01ff) == 0);
    CHECK(m->Usb[UR_RXDMA_OWN] == 0x03);
    CHECK((m->Pla[MR_COMMAND] & (CMD_RX_ON | CMD_TX_ON)) == (CMD_RX_ON | CMD_TX_ON));
    CHECK((Word(m->Pla, MR_MISC1) & MISC1_RX_GATE) == 0);
    /* 5G and 10G report as such; 1000 brings the idle slow-down back */
    m->Pla[MR_LINK + 1] = (nc_u8)(LNK_5000 >> 8);
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Mbps == 5000);
    m->Pla[MR_LINK + 1] = (nc_u8)(LNK_10000 >> 8);
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Mbps == 10000);
    m->Pla[MR_LINK + 1] = 0;
    m->Pla[MR_LINK] = LNK_UP | LNK_FULL | LNK_1000;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Mbps == 1000);
    m->Pla[MR_LINK] = 0;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && !l.Up);
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == 0 && (Word(m->Pla, MR_MISC1) & MISC1_RX_GATE));
    m->Pla[MR_LINK] = LNK_UP | LNK_FULL | LNK_1000;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Up && (Word(m->Pla, MR_POWER4) & 0x0040));
    CHECK(NetPackageChip.Stop(state, &io) == NC_OK && (m->Pla[MR_OOB] & OOB_ACTIVE));
    CHECK((Word(m->Pla, MR_RX_CONFIG1) & 0x00c0) == 0x00c0 && Word(m->Pla, MR_RX_FIFO_EMPTY) == 4096 / 16);
    CHECK(s->Timeouts == 0);

    /* The RTL8156 at High Speed: 1000 is its top; the FIFO thresholds
     * zeroed when the link goes down. */
    p = Probe(NC_SPEED_HIGH);
    Reset(0x7030UL << 16);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK((m->Phy[PR_MGIG_CTRL / 2] & MGIG_2G5) == 0);
    m->Pla[MR_LINK] = LNK_UP | LNK_FULL | LNK_1000;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Mbps == 1000);
    CHECK(Word(m->Pla, MR_EEE_TW) == 0x11 && (Word(m->Usb, UR_L1_CTRL) & 0x000f) == 0x0001);
    m->Pla[MR_LINK] = 0;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && !l.Up);
    CHECK(Word(m->Pla, MR_RX_FIFO_FULL) == 0 && Word(m->Pla, MR_RX_FIFO_EMPTY) == 0);

    /* A PHY loading from flash that never reports done: 100 ms, counted. */
    p = Probe(NC_SPEED_SUPER);
    Reset(0x7420UL << 16);
    m->Pla[MR_GPHY_FLASH] = 0x02;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && s->Timeouts == 2);
    Reset(0x7420UL << 16);
    m->Pla[MR_GPHY_FLASH] = 0x02;
    m->Usb[UR_GPHY] = GPHY_PATCH_DONE;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && s->Timeouts == 0);

    /* A patch window refused: the rest of the PHY setup is skipped (no
     * EEE), autonegotiation and the rest of bring-up go on. */
    Reset(0x7400UL << 16);
    m->NoGrant = 1;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK((m->Phy[PR_PATCH_CMD / 2] & PATCH_ASK) == 0);
    CHECK(m->Phy[PR_EEE_ADV / 2] == 0 && (m->Phy[0xa86a / 2] & 1) == 0);
    CHECK((m->Phy[(MII(MII_CONTROL)) / 2] & MIICTL_AN_RESTART) && m->Phy[PR_MGIG_CTRL / 2] == MGIG_2G5);
    CHECK((m->Pla[MR_OOB] & OOB_ACTIVE) == 0 && Dword(m->Usb, UR_RX_BUF_TH) == 0x00600400UL);

    /* An RTL8156 with its pairs reversed: the PHY's pair tables swapped. */
    Reset(0x7030UL << 16);
    m->Usb[UR_UPS_CFG] = 0x20;
    m->Phy[0xd068 / 2] = 0x0002;
    m->Phy[0xbd5a / 2] = 0x0102; m->Phy[0xbd5c / 2] = 0x0304;
    m->Phy[0xbc18 / 2] = 0x1f00; m->Phy[0xbc1a / 2] = 0x0011;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK(m->Phy[0xd068 / 2] == 0x2011);
    CHECK(m->Phy[0xbd5a / 2] == 0x0403 && m->Phy[0xbd5c / 2] == 0x0201);
    CHECK(m->Phy[0xbc18 / 2] == 0x1100 && m->Phy[0xbc1a / 2] == 0x001f);
    /* and without the reversal, untouched */
    Reset(0x7030UL << 16);
    m->Phy[0xbd5a / 2] = 0x0102; m->Phy[0xbd5c / 2] = 0x0304;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK(m->Phy[0xbd5a / 2] == 0x0102 && m->Phy[0xbd5c / 2] == 0x0304);
}

static void Failures(void)
{
    NET_PROBE p = Probe(NC_SPEED_HIGH);
    NET_PARAMS par;
    nc_u32 total, n;
    static const nc_u8 laa[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
    /* Unknown version: refused after reading TCR0, nothing written. */
    Reset(0x7010UL << 16);                        /* a test chip: not this core's */
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_E_UNSUPPORTED);
    CHECK(m->Count == 1 && m->Writes == 0);
    /* The lower edge's cleanup after a refused start touches nothing. */
    CHECK(NetPackageChip.Stop(state, &io) == NC_OK && NetPackageChip.Suspend(state, &io) == NC_OK);
    CHECK(m->Count == 1);
    /* The id read is tried three times: two failed reads are tolerated,
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
    CHECK(m->Last[0].Value == (MR_CFG_LOCK & ~3) && m->Last[0].Index == (RTL_SPACE_MAC | (RTL_LANES_BYTE << 0)));
    CHECK(m->Last[1].Value == MR_NODE_ID && m->Last[1].Index == (RTL_SPACE_MAC | 0xff) && m->Last[1].Length == 4);
    CHECK(m->Last[2].Value == MR_NODE_ID + 4 && m->Last[2].Index == (RTL_SPACE_MAC | 0x33) && m->Last[2].Length == 4);
    CHECK(m->Last[3].Value == (MR_CFG_LOCK & ~3));
    CHECK(memcmp(m->Pla + MR_NODE_ID, laa, 6) == 0 && m->Pla[MR_NODE_ID + 6] == 0);
}

/* Word and byte writes at unaligned offsets use shifted byte enables. */
static void ByteEnables(void)
{
    NET_PROBE p = Probe(NC_SPEED_HIGH);
    NET_PARAMS par;
    Reset(0x4c10UL << 16);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    m->Count = 0;
    m->Pla[MR_LINK] = LNK_UP | LNK_100 | LNK_FULL;
    {
        NET_LINK l;
        CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Up);
    }
    /* MR_LINK 0xe908 is dword aligned: the word read uses 0x33. */
    CHECK(m->Last[0].In && m->Last[0].Value == MR_LINK && m->Last[0].Index == (RTL_SPACE_MAC | 0x33));
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
    irq[0] = IRQ_LINK;
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
    /* The vendor configuration need not be the first. */
    cfg[0] = ecm; size[0] = sizeof(ecm);
    cfg[1] = vendor; size[1] = sizeof(vendor);
    CHECK(NetPackageChip.Probe(dev, 18, cfg, size, 2, &p) == NC_OK && p.ConfigIndex == 1);
    /* A configuration that could not be read is skipped. */
    cfg[0] = NULL; size[0] = 0;
    CHECK(NetPackageChip.Probe(dev, 18, cfg, size, 2, &p) == NC_OK && p.ConfigIndex == 1);
    /* ECM only: not this driver's. */
    cfg[0] = ecm; size[0] = sizeof(ecm);
    CHECK(NetPackageChip.Probe(dev, 18, cfg, size, 1, &p) == NC_E_UNSUPPORTED);
    /* The vendor interface's endpoint numbers are 1, 2, 3. */
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
    BringUp(0x4c00, NC_SPEED_HIGH);
    BringUp(0x4c10, NC_SPEED_HIGH);
    BringUp(0x4c10, NC_SPEED_FULL);
    BringUp(0x4800, NC_SPEED_HIGH);
    BringUp(0x5c00, NC_SPEED_SUPER);
    BringUp(0x5c10, NC_SPEED_HIGH);
    BringUp(0x5c10, NC_SPEED_SUPER);
    BringUp(0x5c20, NC_SPEED_SUPER);
    BringUp(0x5c30, NC_SPEED_HIGH);
    BringUp(0x5c30, NC_SPEED_FULL);
    BringUp(0x6000, NC_SPEED_SUPER);
    BringUp(0x6010, NC_SPEED_HIGH);
    BringUp(0x6400, NC_SPEED_SUPER);
    BringUp(0x7020, NC_SPEED_SUPER);
    BringUp(0x7020, NC_SPEED_HIGH);
    BringUp(0x7030, NC_SPEED_SUPER);
    BringUp(0x7030, NC_SPEED_HIGH);
    BringUp(0x7400, NC_SPEED_SUPER);
    BringUp(0x7410, NC_SPEED_SUPER);
    BringUp(0x7410, NC_SPEED_HIGH);
    BringUp(0x7420, NC_SPEED_SUPER);
    BringUp(0x7420, NC_SPEED_FULL);
    LinkAndFilter(0x4c10);
    LinkAndFilter(0x5c30);
    LinkAndFilter(0x6010);
    LinkAndFilter(0x6400);
    LinkAndFilter(0x7030);
    LinkAndFilter(0x7410);
    LinkAndFilter(0x7420);
    Multigig();
    Failures();
    ByteEnables();
    Datapath();
    Probes();
    printf("PASS: %lu RTL815X chip-core checks\n", assertions);
    return 0;
}

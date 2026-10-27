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
    int NoGrant;                /* the PHY refuses patch windows (after GrantFirst granted) */
    int GrantFirst, Asks;
    nc_u32 Trail[64];           /* writes to MR_COMMAND, UR_BMU, UR_CTRL in order (TrailOn) */
    nc_u32 TrailV[64];          /* and the dword each wrote */
    int TrailN, TrailOn;
    ENTRY Last[64];             /* ring of the last requests */
    nc_u32 Ind[2][16384];       /* behind the indirect window: BMU, IP (addr / 4) */
    int IndStuck;               /* the indirect window stays busy */
    nc_u16 Sram[2][65536];      /* the PHY SRAM windows */
    nc_u16 Lane[4];             /* 0xd06a per pair, selected by 0xd068 bits 3-4 */
    nc_u16 SramAt[2];
    nc_u32 BmcrResets, BmcrWrites, CmdResets;
} MODEL;

static nc_u16 PhyAddr(MODEL *m, nc_u16 pla)
{
    nc_u16 base = (nc_u16)(m->Pla[MR_PHY_PAGE] | (m->Pla[MR_PHY_PAGE + 1] << 8));
    return (nc_u16)(base | (pla & 0x0fff));
}

/* The RTL8157/8159 parts of the model: the indirect window, the two PHY
 * SRAM windows (the model advances the SRAM address after each data
 * write, its own choice), a PHY in external init that settles once both
 * its flags are let go of, and counts of PHY and MAC resets. */
static void PhyWritten(MODEL *m, nc_u16 addr, nc_u16 v)
{
    if (addr == MII(MII_CONTROL)) {
        m->BmcrWrites++;
        if (v & MIICTL_RESET) m->BmcrResets++;
    }
    if ((addr == 0xa466 || addr == 0xa468) && (m->Phy[PR_STATE / 2] & PHYST_MASK) == PHYST_EXT_INIT &&
        !((addr == 0xa466 ? v : m->Phy[0xa466 / 2]) & 0x0001) &&
        !((addr == 0xa468 ? v : m->Phy[0xa468 / 2]) & 0x000a))
        m->Phy[PR_STATE / 2] = PHYST_LAN_ON;
    if (addr == 0xd06a) { m->Lane[(m->Phy[0xd068 / 2] >> 3) & 3] = v; return; }
    if (addr == PR_SRAM_ADDR) m->SramAt[0] = v;
    if (addr == PR_SRAM2_ADDR) m->SramAt[1] = v;
    if (addr == PR_SRAM_DATA) m->Sram[0][m->SramAt[0]++] = v;
    if (addr == PR_SRAM2_DATA) m->Sram[1][m->SramAt[1]++] = v;
    if (addr == MII(MII_CONTROL)) v = (nc_u16)(v & ~MIICTL_RESET);  /* reset completes */
    if (addr == PR_PATCH_CMD) {
        /* PATCH_GRANTED follows PATCH_ASK. */
        nc_u16 st = m->Phy[PR_PATCH_STATE / 2];
        int grant = (v & PATCH_ASK) && (!m->NoGrant || m->Asks < m->GrantFirst);
        if (v & PATCH_ASK) m->Asks++;
        m->Phy[PR_PATCH_STATE / 2] = (nc_u16)(grant ? st | PATCH_GRANTED : st & ~PATCH_GRANTED);
    }
    m->Phy[addr / 2] = v;
}

static nc_u16 PhyRead(MODEL *m, nc_u16 addr)
{
    if (addr == 0xd06a) return m->Lane[(m->Phy[0xd068 / 2] >> 3) & 3];
    if (addr == PR_SRAM_DATA) return m->Sram[0][m->SramAt[0]];
    if (addr == PR_SRAM2_DATA) return m->Sram[1][m->SramAt[1]];
    return m->Phy[addr / 2];
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
                nc_u16 w = PhyRead(m, PhyAddr(m, (nc_u16)(a & ~1)));
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
    if (m->TrailOn && m->TrailN < 64 &&
        (((index & RTL_SPACE_MAC) && value == (MR_COMMAND & ~3)) ||
         (!(index & RTL_SPACE_MAC) && (value == UR_BMU || value == (UR_CTRL & ~3)))))
    {
        m->TrailV[m->TrailN] = (nc_u32)b[0] | ((nc_u32)b[1] << 8) | ((nc_u32)b[2] << 16) | ((nc_u32)b[3] << 24);
        m->Trail[m->TrailN++] = ((index & RTL_SPACE_MAC) ? 0x10000UL : 0) | value;
    }
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
    if ((index & RTL_SPACE_MAC) && value == (MR_COMMAND & ~3) && (m->Pla[MR_COMMAND] & CMD_RESET))
        m->CmdResets++;
    m->Pla[MR_COMMAND] &= ~CMD_RESET;
    /* The indirect window runs a command at once unless it is stuck. */
    if (!(index & RTL_SPACE_MAC) && value == UR_ADV_CMD && (m->Usb[UR_ADV_CMD] & ADV_BUSY) && !m->IndStuck) {
        nc_u8 cmd = m->Usb[UR_ADV_CMD];
        nc_u16 addr = (nc_u16)(m->Usb[UR_ADV_ADDR] | (m->Usb[UR_ADV_ADDR + 1] << 8));
        nc_u32 *cell = &m->Ind[(cmd & ADV_IP) ? 1 : 0][(addr / 4) & 16383];
        if (cmd & ADV_WRITE)
            *cell = (nc_u32)m->Usb[UR_ADV_DATA] | ((nc_u32)m->Usb[UR_ADV_DATA + 1] << 8) |
                    ((nc_u32)m->Usb[UR_ADV_DATA + 2] << 16) | ((nc_u32)m->Usb[UR_ADV_DATA + 3] << 24);
        else
            for (k = 0; k < 4; k++) m->Usb[UR_ADV_DATA + k] = (nc_u8)(*cell >> (8 * k));
        m->Usb[UR_ADV_CMD] = (nc_u8)(cmd & ~ADV_BUSY);
    }
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
        0x7020, 0x7030, 0x7400, 0x7410, 0x7420, 0x1030, 0x2020
    };
    unsigned i;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        CHECK(RtlChipId((nc_u32)map[i] << 16) == map[i]);
        /* bits outside TXC_ID_MASK are ignored */
        CHECK(RtlChipId(((nc_u32)map[i] << 16) | 0x830f0000UL | 0xffff) == map[i]);
    }
    /* The test chip 7010 and anything else: not this core. */
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
    m->Usb[UR_GPHY] = GPHY_CODE_LOADED;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && s->Timeouts == 0);

    /* A patch window refused: the rest of the PHY setup is skipped (no
     * EEE), autonegotiation and the rest of bring-up go on. */
    Reset(0x7400UL << 16);
    m->NoGrant = 1;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK((m->Phy[PR_PATCH_CMD / 2] & PATCH_ASK) == 0);
    CHECK(m->Phy[PR_EEE_ADV / 2] == 0 && (m->Phy[0xa86a / 2] & 1) == 0);
    CHECK((m->Phy[(MII(MII_CONTROL)) / 2] & MIICTL_AN_RESTART) && m->Phy[PR_MGIG_CTRL / 2] == MGIG_2G5);
    CHECK(m->BmcrResets == 0);                      /* no PHY reset after a PHY setup that gave up */
    CHECK((m->Pla[MR_OOB] & OOB_ACTIVE) == 0 && Dword(m->Usb, UR_RX_BUF_TH) == 0x00600400UL);

    /* The 7400's second window refused (its first granted): the table's
     * own window ran, the common clocks and the rest did not. */
    Reset(0x7400UL << 16);
    m->NoGrant = 1; m->GrantFirst = 1;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && m->Asks == 2);
    CHECK((m->Phy[0xb896 / 2] & 1) && m->Phy[0xb890 / 2] == 0x1416);
    CHECK((Word(m->Pla, MR_POWER4) & 1) == 0 && m->Phy[PR_EEE_ADV / 2] == 0 && m->BmcrResets == 0);
    CHECK((m->Phy[PR_PATCH_CMD / 2] & PATCH_ASK) == 0);
    /* The 7030's window refused: nothing after it in its table */
    Reset(0x7030UL << 16);
    m->NoGrant = 1;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && m->Asks == 1);
    CHECK((m->Phy[0xd068 / 2] & 0x2000) == 0 && (m->Phy[PR_INTR / 2] & 0x0008) == 0);
    CHECK(m->Sram[0][0x80ea] == 0x7843 && m->Phy[PR_EEE_ADV / 2] == 0 && m->BmcrResets == 0);

    /* The receiver and transmitter reset at link down: the 7030's
     * interleaved order; the 7410's MAC reset. */
    {
        static const nc_u32 order56a[6] = {
            0x10000UL | (MR_COMMAND & ~3), UR_BMU, UR_CTRL & ~3, 0x10000UL | (MR_COMMAND & ~3), UR_BMU, UR_CTRL & ~3 };
        nc_u16 ids[2] = { 0x7030, 0x7410 };
        int k, j;
        for (k = 0; k < 2; k++) {
            Reset((nc_u32)ids[k] << 16);
            CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
            m->Pla[MR_LINK] = LNK_UP | LNK_FULL | LNK_1000;
            CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Up);
            m->Pla[MR_LINK] = 0;
            m->TrailOn = 1;
            m->CmdResets = 0;
            CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && !l.Up);
            m->TrailOn = 0;
            if (k == 0) {
                /* gate first, then the six reset writes, then the buffer-unit reset */
                for (j = 0; j < m->TrailN && m->Trail[j] != order56a[0]; j++) {}
                CHECK(j + 6 <= m->TrailN);
                CHECK(memcmp(m->Trail + j, order56a, sizeof(order56a)) == 0);
                /* the transmitter off first (receiver still on), then the receiver */
                CHECK(((m->TrailV[j] >> 24) & (CMD_TX_ON | CMD_RX_ON)) == CMD_RX_ON);
                CHECK(((m->TrailV[j + 3] >> 24) & (CMD_TX_ON | CMD_RX_ON)) == 0);
                CHECK(((m->TrailV[j + 2] >> 16) & CTRL_ECM) && !((m->TrailV[j + 5] >> 16) & CTRL_ECM));
                CHECK(!(m->TrailV[j + 1] & 1) && (m->TrailV[j + 4] & 1) && m->CmdResets == 0);
                CHECK((Word(m->Usb, UR_CTRL) & CTRL_ECM) == 0 && (m->Usb[UR_BMU] & 0x03) == 0x03);
                CHECK((m->Pla[MR_COMMAND] & (CMD_RX_ON | CMD_TX_ON)) == 0);
            } else {
                CHECK(m->TrailN >= 3 && m->Trail[0] == (0x10000UL | (MR_COMMAND & ~3)));
                CHECK(m->CmdResets == 1);              /* a MAC reset, not a clear of RX/TX */
            }
        }
    }

    /* An RTL8156 with its pairs reversed: the PHY's pair tables swapped. */
    Reset(0x7030UL << 16);
    m->Usb[UR_UPS_CFG] = 0x20;
    m->Phy[0xd068 / 2] = 0x0002;
    m->Phy[0xbd5a / 2] = 0x0102; m->Phy[0xbd5c / 2] = 0x0304;
    m->Phy[0xbc18 / 2] = 0x1f00; m->Phy[0xbc1a / 2] = 0x0011;
    m->Lane[0] = 0xa111; m->Lane[1] = 0xb222; m->Lane[2] = 0xc333; m->Lane[3] = 0xd444;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK(m->Phy[0xd068 / 2] == 0x2011);
    CHECK(m->Lane[0] == 0xa444 && m->Lane[3] == 0xd111 && m->Lane[1] == 0xb333 && m->Lane[2] == 0xc222);
    CHECK(m->Phy[0xbd5a / 2] == 0x0403 && m->Phy[0xbd5c / 2] == 0x0201);
    CHECK(m->Phy[0xbc18 / 2] == 0x1100 && m->Phy[0xbc1a / 2] == 0x001f);
    /* and without the reversal, untouched */
    Reset(0x7030UL << 16);
    m->Phy[0xbd5a / 2] = 0x0102; m->Phy[0xbd5c / 2] = 0x0304;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK(m->Phy[0xbd5a / 2] == 0x0102 && m->Phy[0xbd5c / 2] == 0x0304);
    CHECK(m->Lane[0] == 0 && m->Lane[3] == 0);
    /* PHY values that land in the SRAM windows, one per table */
    CHECK(m->Sram[0][0x8257] == 0x020f && m->Sram[0][0x80ea] == 0x7843 && m->Sram[1][0x80a2] == 0x0153);
    CHECK(m->Sram[1][0x8159] == 0x0700 && (m->Sram[0][SR_GREEN] & 0x8000));
    Reset(0x7020UL << 16);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK(m->Sram[0][0x819f] == 0xddb6 && m->Sram[0][0x80ea] == 0xc400 && m->Sram[0][0x80df] == 0x0100);
    Reset(0x7400UL << 16);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && m->BmcrResets == 1);
    CHECK(m->Sram[1][0x8fd8] == 0xf600 && m->Sram[0][0x80a9] == 0xc000 && (m->Sram[0][0x817d] & 0x1000));
    CHECK(m->Phy[0xb890 / 2] == 0x1416 && (m->Phy[0xb896 / 2] & 1));
    Reset(0x7420UL << 16);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK(m->Sram[0][0x807a] == 0x2417 && m->Sram[1][0x8551] == 0x0800 && m->Sram[1][0x80f5] == 0x760e);
    CHECK(m->Sram[0][0x8257] == 0 && m->Sram[1][0x8fd8] == 0);
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

/* ---------------- the RTL8157 and RTL8159 ---------------- */


/* What the model holds before a V2 bring-up: bits Start must clear. */
static void ResetV2(nc_u16 id)
{
    Reset((nc_u32)id << 16);
    m->Usb[UR_CTRL + 1] = (nc_u8)((CTRL_RX_DESC16 | 0x0800) >> 8);
    m->Ind[1][0xc2c0 / 4] = 0x20000000UL | 0x5;
    m->Ind[0][0x2300 / 4] = 0x0000010f;
    m->Usb[0xd4ae] = 0x02;
    m->Usb[0xcf04] = 0xff;
    m->Usb[UR_POWER_CUT] = 0x39;
    m->Pla[MR_RX_CONFIG1] = 0x00c0 | 0x0008;
    m->Phy[PR_FORCE_SEL / 2] = 0x8000;
    m->Phy[PR_EEE_ADV2 / 2] = 0;
    m->Phy[PR_MGIG_CTRL / 2] = (MGIG_2G5 | MGIG_5G | MGIG_10G) | 0x0020;
    m->Sram[0][0x8011] = 0x0011;
    m->Pla[MR_PHY_POWER] = 0x41;                   /* bits Start clears, and one it keeps */
    m->Pla[0xd3c8] = 0x03;
    m->Phy[PR_LITE_A / 2] = 0x0201;
    m->Phy[PR_LITE_B / 2] = 0x0007;
}

static void BringUpV2(nc_u16 id, nc_u8 speed)
{
    NET_PROBE p = Probe(speed);
    NET_PARAMS par;
    RTL_STATE *s = (RTL_STATE *)state;
    nc_u16 mg = (nc_u16)(speed != NC_SPEED_SUPER ? 0 : id == ID_2020 ? (MGIG_2G5 | MGIG_5G | MGIG_10G) : (MGIG_2G5 | MGIG_5G));
    ResetV2(id);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK(s->Id == id && s->Timeouts == 0 && par.Version == id);
    CHECK(m->Count < 3000);
    CHECK(memcmp(par.Mac, backupMac, 6) == 0 && memcmp(m->Pla + MR_NODE_ID, backupMac, 6) == 0);
    CHECK(par.RxSize == 32768UL);
    CHECK(par.TxHeader == 16 && par.TxAlign == 16 && par.TxSize == 16384 && par.TxSlots == 4);
    /* base setup */
    CHECK(m->Usb[0xcffe] & 0x08);
    CHECK((m->Usb[UR_ECM_OP] & 1) == 0 && (Word(m->Usb, UR_ECM_OPTION) & 0x0020));
    CHECK((m->Ind[1][0xc2c0 / 4]) == 0x5);                     /* U2P3 off, the rest kept */
    CHECK((m->Ind[0][0x2300 / 4]) == 0x00000107);
    CHECK((m->Usb[0xd4ae] & 0x02) == 0);
    CHECK(Word(m->Usb, UR_MSC_TMR) == 0x0fff && Word(m->Usb, UR_U1U2_TMR) == 500);
    CHECK((m->Usb[UR_POWER_CUT] & 0x31) == 0 && (m->Usb[UR_POWER_CUT] & 0x08));
    CHECK(Word(m->Pla, MR_POWER1) == 0x0403 && Word(m->Pla, MR_POWER2) == 0x8003);
    CHECK((Word(m->Usb, UR_CTRL) & (CTRL_RX_AGG_OFF | CTRL_RX_DESC16)) == 0);
    CHECK((Word(m->Usb, UR_CTRL) & 0x0800) == (id == ID_2020 ? 0 : 0x0800));
    CHECK((Word(m->Usb, UR_LPM_CFG) & 1) == 0);                 /* up leaves U1/U2 off */
    CHECK((Word(m->Pla, MR_EXTRA_STATUS) & 0x8001) == 0x0001);
    CHECK((m->Phy[PR_FORCE_SEL / 2] & 0x8000) == 0);
    /* PHY setup: green Ethernet, EEE (with 2.5G and 5G, and 10G on the 8159) */
    CHECK(m->Sram[0][0x8011] == 0x8011);
    CHECK(m->Phy[PR_EEE_ADV / 2] == (id == ID_2020 ? (EEEADV_10GT | EEEADV_1000T | EEEADV_100TX) :
                                                     (EEEADV_1000T | EEEADV_100TX)));
    CHECK(m->Phy[PR_EEE_ADV2 / 2] == (EEEADV2_2G5 | EEEADV2_5G));
    CHECK((Word(m->Pla, MR_EEE) & 3) == 3 && (m->Phy[PR_EEE_CFG / 2] & 0x0010));
    CHECK((m->Phy[PR_POWER / 2] & 0x8007) == 0x8007);              /* clocks, ALDPS and 0-1 */
    CHECK((m->Phy[PR_SPEED_DOWN / 2] & 0x3800) == 0x0800);
    CHECK((Word(m->Pla, MR_POWER4) & 1) && (m->Phy[PR_PATCH_CMD / 2] & PATCH_ASK) == 0);
    CHECK((Word(m->Pla, MR_PHY_POWER) & 0x0041) == 0x0001);
    CHECK(m->Phy[PR_LITE_A / 2] == 0x0001 && m->Phy[PR_LITE_B / 2] == 0x0004);
    CHECK(m->Pla[0xd3c8] == (id == ID_2020 ? 0x02 : 0x03));
    /* a few tuning values of each chip */
    if (id == ID_1030) {
        CHECK(m->Sram[0][0x8183] == 0x5900 && m->Sram[1][0x8647] == 0xe600);
        CHECK(m->Sram[0][0x8f9c] == 0x0005 && m->Sram[0][0x8f9c + 5] == 0xd401);
        CHECK(m->Sram[1][0x82c8] == 0x03ed && m->Sram[1][0x82c8 + 19] == 0x03ed);
        CHECK(m->Sram[1][0x82a0 + 19] == 0x03f1 && m->Sram[0][0x8566 + 8] == 0);
        CHECK((m->Phy[0xb54c / 2] & 0xffc0) == 0x3700 && (m->Phy[0xb648 / 2] & 0x4040) == 0x4040);
        CHECK(m->Sram[1][0x809d] == 0x5000);
    } else {
        CHECK(m->Sram[0][0x8415] == 0x9300 && m->Sram[0][0x8173] == 0x8620);
        CHECK(m->Sram[0][0x81a8] == 0 && m->Sram[0][0x81be] == 0x2000 && m->Sram[0][0x81bf] == 0xa600);
        CHECK(m->Sram[1][0x84f0] == 0x201c && m->Phy[0xae20 / 2] == 0xffff && m->Phy[0xadb8 / 2] == 0x0150);
        CHECK(m->Sram[0][0x8012] == 0x4800 && m->Sram[1][0x8217] == 0x2a00);
        CHECK((m->Phy[0xbf0c / 2] & 0x3800) == 0x3800 && m->Phy[0xbfb6 / 2] == 0x3e14);
        CHECK(m->Sram[0][0x81c4] == 0x003b && m->Sram[0][0x81c4 + 11] == 0x0023);
        CHECK(m->Sram[1][0x88d5] == 0x0200 && (m->Pla[0xd3c8] & 1) == 0);
    }
    /* autonegotiation: 10/100/1000, multi-gigabit at SuperSpeed only, PHY reset */
    CHECK((m->Phy[(MII(MII_AN_ADV)) / 2] & 0x0de0) == 0x0de0);
    CHECK(m->Phy[(MII(MII_GIG_CONTROL)) / 2] == GIGCTL_1000_FULL);
    CHECK(m->Phy[PR_MGIG_CTRL / 2] == (mg | 0x0020));
    CHECK(m->BmcrResets == 1);
    /* up: receiver ready, off; no MAC reset, VLAN tags kept; frame sizes */
    CHECK(m->CmdResets == 0);
    CHECK((m->Pla[MR_COMMAND] & (CMD_RX_ON | CMD_TX_ON)) == 0);
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == 0);
    CHECK((m->Pla[MR_OOB] & OOB_ACTIVE) == 0 && (Word(m->Pla, MR_SFF7) & 0x4000) == 0);
    CHECK((Word(m->Pla, MR_RX_CONFIG1) & (0x00c0 | 0x0008)) == 0);
    CHECK(Word(m->Pla, MR_RX_MAX_LEN) == RX_MAX_LEN && m->Pla[MR_TX_MAX_PKT] == 192);
    CHECK(Word(m->Pla, MR_RX_FIFO_FULL) == 512 && Word(m->Pla, MR_RX_FIFO_EMPTY) == 1024);
    CHECK(Word(m->Pla, MR_TXFIFO_TH) == 8 && Word(m->Pla, MR_TXFIFO_FULL) == 128);
    CHECK((Word(m->Pla, MR_RXFIFO_TH1_HI) & 0x0fff) == 8);
    CHECK(Dword(m->Usb, UR_RX_BUF_TH) == 0x00600400UL);
    CHECK((Word(m->Pla, MR_POWER3) & 0x4000) == 0);
    CHECK((m->Phy[PR_POWER / 2] & 0x0004) && (Word(m->Pla, MR_MISC1) & MISC1_RX_GATE));
}

/* Start's other paths: a power cut seen, a PHY in external init, flash
 * loading, a patch request not granted, a stuck indirect window. */
static void PathsV2(nc_u16 id)
{
    NET_PROBE p = Probe(NC_SPEED_SUPER);
    NET_PARAMS par;
    RTL_STATE *s = (RTL_STATE *)state;
    nc_u32 plain, plainSlept;
    /* external init: both bits let go of, then LAN on */
    ResetV2(id);
    m->Phy[PR_STATE / 2] = PHYST_EXT_INIT;
    m->Phy[0xa466 / 2] = 0x0003;
    m->Phy[0xa468 / 2] = 0x00ff;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && s->Timeouts == 0);
    CHECK(m->Phy[0xa466 / 2] == 0x0002 && m->Phy[0xa468 / 2] == 0x00f5);
    /* flash loading: waited for while not done (100 reads 1 ms apart, at
     * each of the two checks), not when done */
    ResetV2(id);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    plain = m->Count;
    plainSlept = m->Slept;
    ResetV2(id);
    m->Pla[MR_GPHY_FLASH] = 0x02;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && s->Timeouts == 2);
    CHECK(m->Count == plain + 2 * 101 && m->Slept == plainSlept + 2 * 100);
    ResetV2(id);
    m->Pla[MR_GPHY_FLASH] = 0x02;
    m->Usb[UR_GPHY] = GPHY_CODE_LOADED;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && s->Timeouts == 0);
    CHECK(m->Count == plain + 4);                   /* two more reads at each of the two checks */
    ResetV2(id);
    m->Pla[MR_GPHY_FLASH] = 0x02;
    m->Usb[UR_GPHY] = GPHY_NO_FLASH;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && s->Timeouts == 0 && m->Count == plain + 2);
    /* a power cut seen at start: the 8159 waits for the restore, then the
     * flag is cleared before the PHY setup checks it */
    ResetV2(id);
    m->Usb[UR_MISC0] = 0x01;
    m->Usb[UR_GPHY] = GPHY_RESTORED;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && s->Timeouts == 0);
    CHECK((m->Usb[UR_MISC0] & 1) == 0);
    ResetV2(id);
    m->Usb[UR_MISC0] = 0x01;
    if (id == ID_2020) {
        CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_E_DEVICE);
        CHECK(m->Writes == 5);                      /* only the first five setup writes */
    } else {
        CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK && (m->Usb[UR_MISC0] & 1) == 0);
    }
    /* a patch window refused: withdrawn; the PHY setup ends (no clocks,
     * no green, no EEE); autonegotiation goes on, with no PHY reset */
    ResetV2(id);
    m->NoGrant = 1;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK((m->Phy[PR_PATCH_CMD / 2] & PATCH_ASK) == 0);
    CHECK((Word(m->Pla, MR_POWER4) & 1) == 0 && m->Sram[0][0x8011] == 0x0011);
    CHECK(m->Phy[PR_EEE_ADV / 2] == 0 && (m->Phy[PR_POWER / 2] & 0x8000) == 0);
    CHECK(m->BmcrResets == 0 && m->BmcrWrites == 1);
    CHECK((m->Phy[(MII(MII_CONTROL)) / 2] & (MIICTL_AN_ENABLE | MIICTL_AN_RESTART)) ==
          (MIICTL_AN_ENABLE | MIICTL_AN_RESTART));
    m->NoGrant = 0;
    m->BmcrResets = 0;
    CHECK(NetPackageChip.Resume(state, &io, &p, &par) == NC_OK && m->BmcrResets == 1);
    CHECK(m->Phy[PR_EEE_ADV2 / 2] == (EEEADV2_2G5 | EEEADV2_5G));
    /* a stuck indirect window: each wait given up after 10 reads 2 ms
     * apart; the U2P3 access waits once after its command, the buffer
     * unit's access once before it, and neither moves data */
    ResetV2(id);
    m->IndStuck = 1;
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK(s->Timeouts == 2 && m->Ind[1][0xc2c0 / 4] == (0x20000000UL | 0x5));
    CHECK(m->Ind[0][0x2300 / 4] == 0x0000010f);
    CHECK(m->Slept == plainSlept + 2 * 20 && m->Count == plain + (13 + 10) - 2 * 9);
    /* Every failing request stops the bring-up at once. */
    ResetV2(id);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    plain = m->Count;
    {
        nc_u32 n;
        for (n = 2; n <= plain; n++) {
            ResetV2(id);
            m->FailAt = n;
            CHECK(NetPackageChip.Start(state, &io, &p, &par) < 0);
            CHECK(m->Count == n);
        }
    }
}

static void LinkV2(nc_u16 id)
{
    NET_PROBE p = Probe(NC_SPEED_SUPER);
    NET_PARAMS par;
    NET_LINK l;
    RTL_STATE *s = (RTL_STATE *)state;
    nc_u32 early;
    ResetV2(id);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    early = (par.RxSize - (1522 + 16 + 16)) / 16;
    CHECK(early == 1950UL);
    m->Usb[UR_RX_AGG_NUM] = 0xff; m->Usb[UR_RX_AGG_NUM + 1] = 0xff;
    /* 10 Gb/s, 5 Gb/s, 2.5 Gb/s */
    m->Pla[MR_LINK] = LNK_UP | LNK_FULL;
    m->Pla[MR_LINK + 1] = (nc_u8)((id == ID_2020 ? LNK_10000 : LNK_5000) >> 8);
    CHECK(NetPackageChip.Poll(state, &io) == 1);
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Up && l.FullDuplex && s->Carrier);
    CHECK(l.Mbps == (id == ID_2020 ? 10000UL : 5000UL) && l.RxLength == par.RxSize);
    CHECK(Word(m->Usb, UR_RX_AGG_NUM) == 0xfe00);
    CHECK(Word(m->Usb, UR_RX_EARLY_TMO) == 80 && Word(m->Usb, UR_PM_STATUS) == 1875);
    CHECK(Word(m->Usb, UR_RX_EARLY_LEN) == early);
    CHECK((Word(m->Pla, MR_TX_CONFIG1) & IFG_FIELD) == IFG_SHORT && (Word(m->Pla, MR_POWER4) & 0x0100));
    CHECK((m->Pla[MR_COMMAND] & (CMD_RX_ON | CMD_TX_ON)) == (CMD_RX_ON | CMD_TX_ON));
    CHECK(m->Usb[UR_RXDMA_OWN] == 0x03 && (Word(m->Pla, MR_MISC1) & MISC1_RX_GATE) == 0);
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == (ACC_BCAST | ACC_MY_PHYS));
    m->Pla[MR_LINK + 1] = (nc_u8)(LNK_5000 >> 8);
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Mbps == 5000);
    m->Pla[MR_LINK + 1] = (nc_u8)(LNK_2500 >> 8);
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Mbps == 2500);
    m->Pla[MR_LINK + 1] = 0;
    m->Pla[MR_LINK] = LNK_UP | LNK_1000 | LNK_FULL;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Mbps == 1000);
    /* down: receiver and transmitter off, not reset; buffer units reset */
    m->Pla[MR_LINK] = 0;
    m->Usb[UR_BMU] = 0;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && !l.Up && !s->Carrier);
    CHECK((m->Pla[MR_COMMAND] & (CMD_RX_ON | CMD_TX_ON)) == 0 && m->CmdResets == 0);
    CHECK((m->Usb[UR_BMU] & 3) == 3 && (Word(m->Pla, MR_MISC1) & MISC1_RX_GATE));
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == 0);
    /* 10 half: the long gap; EEE+ on the 8157 only */
    m->Pla[MR_LINK] = LNK_UP | LNK_10;
    m->Pla[MR_EEE_PLUS] = (nc_u8)(id == ID_2020 ? 0x02 : 0x00);   /* the bit must move */
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK && l.Mbps == 10 && !l.FullDuplex);
    CHECK((Word(m->Pla, MR_TX_CONFIG1) & IFG_FIELD) == IFG_LONG);
    CHECK((Word(m->Pla, MR_EEE_PLUS) & 2) == (id == ID_2020 ? 0 : 2));
    CHECK(s->Timeouts == 0);
    /* Suspend: back into OOB with the wake filters, VLAN removal on */
    m->Usb[UR_LPM_CFG] = 1;
    CHECK(NetPackageChip.Suspend(state, &io) == NC_OK);
    CHECK((m->Pla[MR_OOB] & OOB_ACTIVE) && (Word(m->Pla, MR_SFF7) & 0x4000));
    CHECK((Dword(m->Pla, MR_RX_ACCEPT) & ACC_MASK) == (ACC_MY_PHYS | ACC_MCAST | ACC_BCAST));
    CHECK((Word(m->Pla, MR_RX_CONFIG1) & 0x00c0) == 0x00c0);
    CHECK(Word(m->Pla, MR_RXFIFO_TH1_HI) == 4 && Word(m->Pla, MR_RX_FIFO_FULL) == 64 && Word(m->Pla, MR_RX_FIFO_EMPTY) == 256);
    CHECK(m->Pla[MR_TX_MAX_PKT] == 96 && Word(m->Pla, MR_TEREDO_WAKE) == 0x00ff);
    CHECK((Word(m->Pla, MR_POWER3) & 0x4000) && (m->Usb[UR_LPM_CFG] & 1) == 0);
    CHECK((Word(m->Pla, MR_MISC1) & MISC1_RX_GATE) == 0 && m->CmdResets == 0);
    /* Resume is the whole bring-up; Stop also lets go of the chip */
    CHECK(NetPackageChip.Resume(state, &io, &p, &par) == NC_OK && s->Id == id);
    CHECK((m->Pla[MR_OOB] & OOB_ACTIVE) == 0);
    m->Usb[UR_POWER_CUT] = 0x09; m->Usb[UR_MISC0] = 1;
    CHECK(NetPackageChip.Stop(state, &io) == NC_OK);
    CHECK(m->Pla[MR_OOB] & OOB_ACTIVE);
    CHECK((m->Usb[UR_POWER_CUT] & 0x09) == 0 && (m->Usb[UR_MISC0] & 1) == 0);
    CHECK(m->Usb[0xcf04] == 0x78);
    /* at High Speed nothing above 1000 is advertised */
    ResetV2(id);
    p = Probe(NC_SPEED_HIGH);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    CHECK((m->Phy[PR_MGIG_CTRL / 2] & (MGIG_2G5 | MGIG_5G | MGIG_10G)) == 0);
}

static nc_u32 AggV2(nc_u8 *b, const nc_u32 *len, nc_u32 count)
{
    nc_u32 at = 0, i, k;
    for (i = 0; i < count; i++) {
        nc_u32 pkt = len[i] + 4, d = (pkt << 17) | 0x0000c00bUL;  /* other bits set */
        memset(b + at, 0x5a, 16);
        b[at] = (nc_u8)d; b[at + 1] = (nc_u8)(d >> 8); b[at + 2] = (nc_u8)(d >> 16); b[at + 3] = (nc_u8)(d >> 24);
        for (k = 0; k < pkt; k++) b[at + 16 + k] = (nc_u8)(i + k);
        at += 16 + pkt;
        while (at & 15) b[at++] = 0xee;
    }
    return at;
}

static void DatapathV2(nc_u16 id)
{
    nc_u8 b[16384], h[16];
    NET_RX_STATS st;
    NET_PROBE p = Probe(NC_SPEED_SUPER);
    NET_PARAMS par;
    nc_u32 lens[5] = { 60, 1514, 99, 1518, 61 }, n, len;
    ResetV2(id);
    CHECK(NetPackageChip.Start(state, &io, &p, &par) == NC_OK);
    n = AggV2(b, lens, 5);
    emitted = bytes = 0;
    CHECK(NetPackageChip.RxParse(state, b, n, Emit, NULL, &st) == 4);
    CHECK(emitted == 4 && bytes == 60 + 1514 + 99 + 61 && st.Dropped == 1);
    /* the records are 16-byte aligned: 60+4 -> 80, 1514+4 -> 1536 */
    CHECK(n == 80 + 1536 + 128 + 1552 + 96);
    memset(b + n, 0, 32);
    emitted = 0;
    CHECK(NetPackageChip.RxParse(state, b, n + 32, Emit, NULL, &st) == 4 && emitted == 4);
    for (len = 0; len <= n; len++) {
        long r;
        nc_u32 at = 0, i, cut = 0;
        emitted = 0;
        r = NetPackageChip.RxParse(state, b, len, Emit, NULL, &st);
        CHECK(r < 0 ? emitted == 0 : (nc_u32)r == emitted);
        /* a cut inside any record's frame (its header complete) is malformed */
        for (i = 0; i < 5; i++) {
            if (len >= 60 && len > at + 16 && len < at + 16 + lens[i] + 4) cut = 1;
            at = (at + 16 + lens[i] + 4 + 15) & ~15UL;
        }
        CHECK(cut ? r == NC_E_MALFORMED : r >= 0);
    }
    /* a length of 0x7fff runs past the end */
    b[3] = 0xff; b[2] = 0xfe;
    CHECK(NetPackageChip.RxParse(state, b, n, Emit, NULL, &st) == NC_E_MALFORMED);
    /* the 24-byte format of the others is not read here */
    n = Agg(b, lens, 2);
    CHECK(NetPackageChip.RxParse(state, b, n, Emit, NULL, &st) == 0);

    memset(h, 0x77, sizeof(h));
    NetPackageChip.TxHeader(state, h, 60, 1024, 1);
    CHECK(Dword(h, 0) == (0xc0000000UL | 60) && Dword(h, 4) == 0);
    CHECK(Dword(h, 8) == (60UL << 4) && Dword(h, 12) == 0xa8000000UL);
    NetPackageChip.TxHeader(state, h, 1514, 1024, 0);
    CHECK(Dword(h, 0) == (0xc0000000UL | 1514) && Dword(h, 8) == (1514UL << 4));
    n = 1024;
    CHECK(NetPackageChip.TxEnd(state, b, &n, sizeof(b), 1024) == 0 && n == 1024);
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
    BringUpV2(ID_1030, NC_SPEED_SUPER);
    BringUpV2(ID_1030, NC_SPEED_HIGH);
    BringUpV2(ID_2020, NC_SPEED_SUPER);
    BringUpV2(ID_2020, NC_SPEED_HIGH);
    BringUpV2(ID_2020, NC_SPEED_FULL);
    PathsV2(ID_1030);
    PathsV2(ID_2020);
    LinkV2(ID_1030);
    LinkV2(ID_2020);
    DatapathV2(ID_1030);
    DatapathV2(ID_2020);
    Probes();
    printf("PASS: %lu RTL815X chip-core checks\n", assertions);
    return 0;
}

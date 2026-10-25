/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * RTL815X chip core behind the netchip.h interface (ethernet.md section
 * 6.4): Realtek RTL8152 (ids 4C00, 4C10, 4800), RTL8153 (5C00 to 5C30),
 * RTL8153B (6000, 6010) and RTL8153C (6400) in their vendor configuration.
 *
 * Every chip-side sequence is a table of register steps (STEP), run by
 * one small interpreter (Run). A step names a register, what to clear and
 * what to set; guard steps limit the steps after them to some chips or
 * USB speeds; the few decisions that depend on a value read back are
 * hooks (Hook). The register addresses, values and the order of writes
 * are facts of the chips' register interface; rtlchip.h names them.
 *
 * What the core does, in the netchip.h terms:
 * - Start: read the chip id, then Setup (the chip's base setup), PhySetup,
 *   Negotiate (advertise and restart autonegotiation), take the station
 *   address, and Wake (out of the low-power "OOB" state, receiver ready
 *   but off).
 * - Link: read MR_LINK; on a change to up, RxOn and the receive mode; on a
 *   change to down, RxOff.
 * - Stop and Suspend: Sleep (back into OOB, as the chip waits while no
 *   driver runs it); Stop also turns the power-cut feature as Detach says.
 * - Resume: the whole of Start but the id read, since a 98 or 2000 system
 *   sleep may have cut the chip's power.
 *
 * Choices of this core: no PHY firmware patch is loaded (ethernet.md 6.4);
 * VLAN tags are left in received frames; no checksum offload or TSO; no
 * wake-up source; the chip's runtime power saving (UPS) stays off. The
 * lower edge stops the bulk transfers before RxOff and Sleep run, so their
 * FIFO-empty waits may run to their bound (1000 tries, 1 ms apart).
 */
#include "rtlchip.h"

/* ------------------------------------------------------------------ */
/* Chips and guards */

/* One bit per supported id, in the order of Ids[]. */
static const nc_u16 Ids[] = {
    ID_4C00, ID_4C10, ID_4800, ID_5C00, ID_5C10, ID_5C20, ID_5C30, ID_6000, ID_6010, ID_6400
};
#define K_4C00  0x0001
#define K_4C10  0x0002
#define K_4800  0x0004
#define K_5C00  0x0008
#define K_5C10  0x0010
#define K_5C20  0x0020
#define K_5C30  0x0040
#define K_6000  0x0080
#define K_6010  0x0100
#define K_6400  0x0200
#define K_8152  (K_4C00 | K_4C10 | K_4800)
#define K_8153  (K_5C00 | K_5C10 | K_5C20 | K_5C30)
#define K_8153B (K_6000 | K_6010)
#define K_LATER (K_8153B | K_6400)          /* 8153B and 8153C */
#define K_GIG   (K_8153 | K_LATER)
#define K_ANY   0xffff

/* USB speeds, for guards. */
#define U_FULL  1
#define U_HIGH  2
#define U_SUPER 4
#define U_ANY   7

static int SpeedBit(const RTL_STATE *s)
{
    return s->UsbSpeed == NC_SPEED_SUPER ? U_SUPER : s->UsbSpeed == NC_SPEED_HIGH ? U_HIGH : U_FULL;
}

/* ------------------------------------------------------------------ */
/* Register access */

static nc_u32 Le32(const nc_u8 *b)
{
    return (nc_u32)b[0] | ((nc_u32)b[1] << 8) | ((nc_u32)b[2] << 16) | ((nc_u32)b[3] << 24);
}

static void SetLe32(nc_u8 *b, nc_u32 v)
{
    int i;
    for (i = 0; i < 4; i++) b[i] = (nc_u8)(v >> (8 * i));
}

/* A register of 1, 2 or 4 bytes is read as its whole dword. A word read
 * names the word's two lanes in wIndex; byte and dword reads name none. */
static nc_u32 Get(NET_IO *io, nc_u16 space, nc_u16 reg, int size)
{
    nc_u8 b[4];
    int shift = (reg & 3) * 8;
    nc_u16 lanes = (nc_u16)(size == 2 ? RTL_LANES_WORD << (reg & 2) : 0);
    NcIoRead(io, RTL_REQ, (nc_u16)(reg & ~3), (nc_u16)(space | lanes), b, 4);
    if (size == 4) return Le32(b);
    return (Le32(b) >> shift) & (size == 2 ? 0xffffUL : 0xffUL);
}

/* A register write is one dword with the register's lanes enabled. */
static void Put(NET_IO *io, nc_u16 space, nc_u16 reg, int size, nc_u32 value)
{
    nc_u8 b[4];
    int at = reg & 3;
    nc_u16 lanes = RTL_LANES_ALL;
    if (size == 2) {
        lanes = (nc_u16)(RTL_LANES_WORD << (reg & 2));
        value = (value & 0xffff) << (8 * (reg & 2));
    } else if (size == 1) {
        lanes = (nc_u16)(RTL_LANES_BYTE << at);
        value = (value & 0xff) << (8 * at);
    }
    SetLe32(b, value);
    NcIoWrite(io, RTL_REQ, (nc_u16)(reg & ~3), (nc_u16)(space | lanes), b, 4);
}

static void Edit(NET_IO *io, nc_u16 space, nc_u16 reg, int size, nc_u32 clear, nc_u32 set)
{
    Put(io, space, reg, size, (Get(io, space, reg, size) & ~clear) | set);
}

/* Eight bytes at a dword boundary: one request, or, with the last two
 * bytes left alone, a full dword and then a dword with two lanes. */
static void PutEight(NET_IO *io, nc_u16 space, nc_u16 reg, const nc_u8 *data, int six)
{
    if (!six) {
        NcIoWrite(io, RTL_REQ, reg, (nc_u16)(space | RTL_LANES_ALL), data, 8);
        return;
    }
    NcIoWrite(io, RTL_REQ, reg, (nc_u16)(space | RTL_LANES_ALL), data, 4);
    NcIoWrite(io, RTL_REQ, (nc_u16)(reg + 4), (nc_u16)(space | RTL_LANES_WORD), data + 4, 4);
}

/* PHY registers: MR_PHY_PAGE selects the 4 KiB page, the window at
 * MR_PHY_WINDOW shows it. The page last written is remembered. */
static nc_u16 PhyAt(RTL_STATE *s, NET_IO *io, nc_u16 reg)
{
    nc_u16 page = (nc_u16)(reg & 0xf000);
    if (s->PhyPage != page) {
        Put(io, RTL_SPACE_MAC, MR_PHY_PAGE, 2, page);
        s->PhyPage = page;
    }
    return (nc_u16)(MR_PHY_WINDOW | (reg & 0x0fff));
}

static nc_u16 PhyGet(RTL_STATE *s, NET_IO *io, nc_u16 reg)
{
    return (nc_u16)Get(io, RTL_SPACE_MAC, PhyAt(s, io, reg), 2);
}

static void PhyPut(RTL_STATE *s, NET_IO *io, nc_u16 reg, nc_u16 value)
{
    Put(io, RTL_SPACE_MAC, PhyAt(s, io, reg), 2, value);
}


/* ------------------------------------------------------------------ */
/* Steps */

enum {
    OP_END,
    OP_ONLY,        /* Kinds, Speeds: the next Count steps run only on a match */
    OP_PUT,         /* Space Reg Size <- Set */
    OP_EDIT,        /* Space Reg Size: clear Clear, set Set */
    OP_SIX,         /* Space Reg: eight bytes of Set's low byte, last two left alone */
    OP_WAIT,        /* Space Reg Size until (value & Clear) == Set; Count tries Ms apart */
    OP_WAIT_LATE,   /* the same, sleeping before each read; no timeout counted */
    OP_PHY_PUT,     /* PHY Reg <- Set */
    OP_PHY_EDIT,    /* PHY Reg: clear Clear, set Set */
    OP_PHY_TRIM,    /* PHY Reg: clear Clear, written only if any of it was set */
    OP_PHY_WAIT,    /* PHY Reg until (value & Clear) == Set, sleeping first; Count x Ms */
    OP_PHY_STATE,   /* PHY Reg until bit (value & Clear) of Set is set; Count x Ms */
    OP_SRAM_PUT,    /* PHY SRAM Reg <- Set */
    OP_SRAM_EDIT,   /* PHY SRAM Reg: clear Clear, set Set */
    OP_TRIM,        /* Space Reg Size: clear Clear, written only if any of it was set */
    OP_SLEEP,       /* Ms */
    OP_RUN,         /* the sequence Seq[Count] */
    OP_HOOK         /* Hook(Count) */
};

typedef struct STEP {
    nc_u8 Op, Size, Ms, Space;   /* Space: 1 MAC, 0 USB */
    nc_u16 Reg, Count;
    nc_u32 Clear, Set;
} STEP;

#define MAC 1
#define USB 0
#define END                      { OP_END, 0, 0, 0, 0, 0, 0, 0 }
#define ONLY(kinds, speeds, n)   { OP_ONLY, 0, 0, 0, (nc_u16)(kinds), (nc_u16)(n), (speeds), 0 }
#define PUT(sp, size, r, v)      { OP_PUT, size, 0, sp, r, 0, 0, (v) }
#define EDIT(sp, size, r, c, v)  { OP_EDIT, size, 0, sp, r, 0, (c), (v) }
#define SET(sp, size, r, v)      EDIT(sp, size, r, 0, v)
#define CLR(sp, size, r, c)      EDIT(sp, size, r, c, 0)
#define TRIM(sp, size, r, c)     { OP_TRIM, size, 0, sp, r, 0, (c), 0 }
#define SIX(sp, r, byte)         { OP_SIX, 0, 0, sp, r, 0, 0, (byte) }
#define WAIT(sp, size, r, mask, want, n, ms) { OP_WAIT, size, ms, sp, r, n, (mask), (want) }
#define WAIT_LATE(sp, size, r, mask, want, n, ms) { OP_WAIT_LATE, size, ms, sp, r, n, (mask), (want) }
#define PHY_PUT(r, v)            { OP_PHY_PUT, 2, 0, 0, r, 0, 0, (v) }
#define PHY_EDIT(r, c, v)        { OP_PHY_EDIT, 2, 0, 0, r, 0, (c), (v) }
#define PHY_SET(r, v)            PHY_EDIT(r, 0, v)
#define PHY_CLR(r, c)            PHY_EDIT(r, c, 0)
#define PHY_TRIM(r, c)           { OP_PHY_TRIM, 2, 0, 0, r, 0, (c), 0 }
#define PHY_WAIT(r, mask, want, n, ms) { OP_PHY_WAIT, 2, ms, 0, r, n, (mask), (want) }
#define PHY_STATE(states)        { OP_PHY_STATE, 2, 20, 0, PR_STATE, 500, PHYST_MASK, (states) }
#define SRAM_PUT(r, v)           { OP_SRAM_PUT, 2, 0, 0, r, 0, 0, (v) }
#define SRAM_SET(r, v)           { OP_SRAM_EDIT, 2, 0, 0, r, 0, 0, (v) }
#define SLEEP(ms)                { OP_SLEEP, 0, ms, 0, 0, 0, 0, 0 }
#define RUN(q)                   { OP_RUN, 0, 0, 0, 0, q, 0, 0 }
#define HOOK(h)                  { OP_HOOK, 0, 0, 0, 0, h, 0, 0 }

/* Sequence numbers (Seq[]) */
enum {
    Q_RX_GATE, Q_RX_UNGATE, Q_TEREDO_OFF, Q_RESET_MAC, Q_RESET_BMU, Q_REBUILD_LIST,
    Q_LEAVE_OOB, Q_QUIESCE, Q_POWER_CUT_OFF, Q_U1U2_OFF, Q_U1U2_ON, Q_ALDPS_OFF, Q_ALDPS_ON,
    Q_EEE_OFF, Q_EEE_ON, Q_NO_WAKE, Q_CFG34_BIT8, Q_AUTOLOAD, Q_DRAIN, Q_QUEUE_WAKE_OFF,
    Q_SETUP_8152, Q_SETUP_8153, Q_SETUP_LATER, Q_PHY_8152, Q_PHY_8153, Q_PHY_LATER,
    Q_FIRST_FILL, Q_WAKE_8152, Q_WAKE_8153, Q_WAKE_8153B, Q_WAKE_8153C, Q_SLEEP, Q_DETACH,
    Q_RX_ON, Q_RX_OFF, Q_BIAS_FULL, Q_BIAS_LOW
};

/* Hook numbers (Hook()) */
enum {
    H_PHY_WAKE, H_BURST, H_LINK_FLAGS, H_PACKAGE, H_UPS_CHECK,
    H_BIAS, H_ADC_TRIM, H_SAW_TIMER, H_EEE_CLOCKS, H_EEE_PLUS, H_GAP
};

/* PHY states a wait accepts (PR_STATE low bits, as bits of a set). */
#define ST_LAN_ON   (1u << PHYST_LAN_ON)
#define ST_SETTLED  ((1u << PHYST_LAN_ON) | (1u << PHYST_POWER_DOWN) | (1u << PHYST_EXT_INIT))

/* Receive-path gate: while set, the MAC hands nothing to USB. */
static const STEP SqRxGate[] = { SET(MAC, 2, MR_MISC1, MISC1_RX_GATE), END };
static const STEP SqRxUngate[] = { CLR(MAC, 2, MR_MISC1, MISC1_RX_GATE), END };

/* Teredo wake-up offload off, its timers cleared. */
static const STEP SqTeredoOff[] = {
    ONLY(K_8152 | K_8153, U_ANY, 1), CLR(MAC, 2, MR_TEREDO, 0x80ff),
    ONLY(K_LATER, U_ANY, 1), PUT(MAC, 1, MR_TEREDO, 0xff),     /* low byte: write 1 to clear */
    PUT(MAC, 2, MR_WDT6, 0x0010),
    PUT(MAC, 2, MR_WOW_TMR, 0),
    PUT(MAC, 4, MR_TEREDO_TMR, 0),
    END
};

static const STEP SqResetMac[] = {
    PUT(MAC, 1, MR_COMMAND, CMD_RESET),
    WAIT(MAC, 1, MR_COMMAND, CMD_RESET, 0, 1000, 1),
    END
};

/* Bulk endpoint buffer units: both directions reset. */
static const STEP SqResetBmu[] = { CLR(USB, 1, UR_BMU, 0x03), SET(USB, 1, UR_BMU, 0x03), END };

static const STEP SqRebuildList[] = {
    WAIT(MAC, 1, MR_OOB, OOB_LIST_READY, OOB_LIST_READY, 1000, 1),
    SET(MAC, 2, MR_SFF7, 0x8000),
    WAIT(MAC, 1, MR_OOB, OOB_LIST_READY, OOB_LIST_READY, 1000, 1),
    END
};

static const STEP SqLeaveOob[] = {
    CLR(MAC, 1, MR_OOB, OOB_ACTIVE),
    CLR(MAC, 2, MR_SFF7, 0x4000),
    RUN(Q_REBUILD_LIST),
    END
};

/* The 8153 family's way out of OOB, up to the frame size. */
static const STEP SqQuiesce[] = {
    RUN(Q_RX_GATE),
    RUN(Q_TEREDO_OFF),
    CLR(MAC, 4, MR_RX_ACCEPT, ACC_MASK),
    RUN(Q_RESET_MAC),
    RUN(Q_RESET_BMU),
    RUN(Q_LEAVE_OOB),
    CLR(MAC, 2, MR_CPLUS, CPLUS_RX_VLAN),
    PUT(MAC, 2, MR_RX_MAX_LEN, RX_MAX_LEN),
    END
};

static const STEP SqPowerCutOff[] = {
    ONLY(K_8152, U_ANY, 2), CLR(USB, 2, UR_UPS, 0x0100), CLR(USB, 2, UR_PM_STATUS, 0x0001),
    ONLY(K_8153, U_ANY, 2), CLR(USB, 2, UR_POWER_CUT, 0x0009), CLR(USB, 2, UR_MISC0, 0x0001),
    ONLY(K_LATER, U_ANY, 2), CLR(USB, 2, UR_POWER_CUT, 0x0001), CLR(USB, 2, UR_MISC0, 0x0001),
    END
};

/* USB 3 U1/U2 link states. */
static const STEP SqU1U2Off[] = {
    ONLY(K_8153, U_ANY, 1), SIX(USB, UR_U1U2, 0x00),
    ONLY(K_LATER, U_ANY, 1), CLR(USB, 2, UR_LPM_CFG, 0x0001),
    END
};
static const STEP SqU1U2On[] = {
    ONLY(K_8153, U_ANY, 1), SIX(USB, UR_U1U2, 0xff),
    ONLY(K_LATER, U_ANY, 1), SET(USB, 2, UR_LPM_CFG, 0x0001),
    END
};

/* Advanced link-down power saving of the PHY. */
static const STEP SqAldpsOff[] = {
    ONLY(K_8152, U_ANY, 2), PHY_PUT(PR_ALDPS_A, 0x0310), SLEEP(20),
    ONLY(K_GIG, U_ANY, 2), PHY_CLR(PR_POWER, 0x0004),
    WAIT_LATE(MAC, 2, MR_POWER_STATE, 0x0100, 0x0100, 20, 1),
    END
};
static const STEP SqAldpsOn[] = {
    ONLY(K_8152, U_ANY, 1), PHY_PUT(PR_ALDPS_A, 0x8310),
    ONLY(K_GIG, U_ANY, 1), PHY_SET(PR_POWER, 0x0004),
    END
};

/* Energy-efficient Ethernet: MAC side, PHY side, advertisement. */
static const STEP SqEeeOff[] = {                       /* 8153 and later only */
    CLR(MAC, 2, MR_EEE, 0x0003),
    PHY_CLR(PR_EEE_CFG, 0x0010),
    PHY_PUT(PR_EEE_ADV, 0),
    END
};
static const STEP SqEeeOn[] = {
    SET(MAC, 2, MR_EEE, 0x0003),
    ONLY(K_8152, U_ANY, 9),
    PHY_EDIT(PR_EEE_A1, 0x0070, 0x3310),
    PHY_SET(PR_EEE_A2, 0x0600),
    PHY_EDIT(PR_EEE_A3, 0xff80, 0x1500),
    /* MMD 7 register 60 through the MMD access pair */
    PHY_PUT(PR_MMD_CTRL, 0x0007), PHY_PUT(PR_MMD_DATA, 60),
    PHY_PUT(PR_MMD_CTRL, 0x4007), PHY_PUT(PR_MMD_DATA, EEEADV_100TX),
    PHY_PUT(PR_MMD_CTRL, 0x0000),
    ONLY(K_GIG, U_ANY, 2),
    PHY_SET(PR_EEE_CFG, 0x0010),
    PHY_PUT(PR_EEE_ADV, EEEADV_1000T | EEEADV_100TX),
    END
};

/* No wake-up source: link, magic packet and wake frames off. */
static const STEP SqNoWake[] = {
    PUT(MAC, 1, MR_CFG_LOCK, CFG_UNLOCKED),
    CLR(MAC, 2, MR_CFG34, 0x0010),
    CLR(MAC, 2, MR_CFG5, CFG5_WAKE_FRAMES),
    PUT(MAC, 1, MR_CFG_LOCK, CFG_LOCKED),
    CLR(MAC, 2, MR_WOL, WOL_MAGIC),
    PUT(MAC, 1, MR_CFG_LOCK, CFG_UNLOCKED),
    CLR(MAC, 2, MR_CFG34, 0x0008),
    PUT(MAC, 1, MR_CFG_LOCK, CFG_LOCKED),
    END
};

static const STEP SqCfg34Bit8[] = {                    /* 8153C */
    PUT(MAC, 1, MR_CFG_LOCK, CFG_UNLOCKED),
    SET(MAC, 2, MR_CFG34, 0x0100),
    PUT(MAC, 1, MR_CFG_LOCK, CFG_LOCKED),
    END
};

static const STEP SqAutoload[] = { WAIT(MAC, 2, MR_BOOT, BOOT_LOADED, BOOT_LOADED, 500, 20), END };

/* Receive off, then wait for both FIFOs and the transmitter to empty
 * and reset the MAC. */
static const STEP SqDrain[] = {
    CLR(MAC, 4, MR_RX_ACCEPT, ACC_MASK),
    RUN(Q_RX_GATE),
    WAIT(MAC, 1, MR_OOB, OOB_FIFOS_EMPTY, OOB_FIFOS_EMPTY, 1000, 1),
    WAIT(MAC, 2, MR_TX_CONFIG, TXC_EMPTY, TXC_EMPTY, 1000, 1),
    RUN(Q_RESET_MAC),
    END
};

static const STEP SqQueueWakeOff[] = {
    CLR(MAC, 1, MR_INDICATE, 0x01),
    CLR(MAC, 1, MR_SUSPEND_FLAG, 0x01),
    CLR(MAC, 2, MR_EXTRA_STATUS, 0x0100),
    END
};

/* ---- base setup, once per bring-up ---- */

static const STEP SqSetup8152[] = {
    PHY_TRIM(MII(MII_CONTROL), MIICTL_POWERDOWN),
    RUN(Q_ALDPS_OFF),
    ONLY(K_4C00, U_ANY, 1), CLR(MAC, 2, MR_LED, 0x0700),
    RUN(Q_POWER_CUT_OFF),
    SET(MAC, 2, MR_PHY_POWER, 0x00c0),
    EDIT(MAC, 4, MR_POWER1, 0x0f0f0f0fUL, 0x07014f07UL),
    PUT(MAC, 2, MR_PHY_IRQ_MASK, 0x000f),
    SET(MAC, 2, MR_TALLY, 0x0001),
    CLR(USB, 2, UR_CTRL, CTRL_RX_AGG_OFF | CTRL_RX_ZERO),   /* receive aggregation on */
    END
};

static const STEP SqSetup8153[] = {
    RUN(Q_U1U2_OFF),
    RUN(Q_AUTOLOAD),
    PHY_STATE(ST_SETTLED),
    ONLY(K_5C00 | K_5C10 | K_5C20, U_ANY, 1), PHY_PUT(PR_ADC_CFG, 0x01c0),
    PHY_TRIM(MII(MII_CONTROL), MIICTL_POWERDOWN),
    PHY_STATE(ST_LAN_ON),
    CLR(USB, 2, UR_U2P3, 0x0001),
    ONLY(K_5C10, U_ANY, 2),
    EDIT(USB, 2, UR_SS_LINK2, 0x3ffe, 0x00c0),
    SET(USB, 1, UR_U2_PHY, 0x03),
    ONLY(K_5C20, U_ANY, 1), CLR(MAC, 1, MR_SPARE0, 0x02),
    ONLY(K_5C20 | K_5C30, U_ANY, 1), HOOK(H_BURST),
    ONLY(K_5C30, U_ANY, 2), RUN(Q_QUEUE_WAKE_OFF), HOOK(H_LINK_FLAGS),
    SET(USB, 1, UR_DUMMY2, 0x01),
    CLR(USB, 2, UR_WDT11, 0x0001),
    CLR(MAC, 2, MR_LED, 0x0700),
    ONLY(K_5C10, U_FULL | U_HIGH, 1), PUT(USB, 1, UR_LPM_CTRL, 0x36),
    ONLY(K_5C10, U_SUPER, 1), PUT(USB, 1, UR_LPM_CTRL, 0x3e),
    ONLY(K_5C00 | K_5C20 | K_5C30, U_ANY, 1), PUT(USB, 1, UR_LPM_CTRL, 0x3e),
    EDIT(USB, 2, UR_AFE2, 0xf800, 0xa100),
    PUT(USB, 2, UR_CONNECT_TMR, 0x0001),
    RUN(Q_POWER_CUT_OFF),
    RUN(Q_NO_WAKE),
    CLR(MAC, 2, MR_POWER2, 0x8000),
    RUN(Q_U1U2_ON),
    SET(MAC, 1, MR_CFG6, 0x01),
    CLR(MAC, 1, MR_LANWAKE, 0x80),
    CLR(USB, 2, UR_CTRL, CTRL_RX_AGG_OFF | CTRL_RX_ZERO),
    SET(MAC, 2, MR_TALLY, 0x0001),
    END
};

static const STEP SqSetupLater[] = {
    RUN(Q_U1U2_OFF),
    ONLY(K_6400, U_ANY, 3),
    PUT(MAC, 1, MR_CFG_LOCK, CFG_UNLOCKED),
    CLR(MAC, 2, MR_CFG5, 0x0008),
    SET(USB, 2, UR_SPI, 0x0002),
    RUN(Q_AUTOLOAD),
    PHY_STATE(ST_SETTLED),
    PHY_TRIM(MII(MII_CONTROL), MIICTL_POWERDOWN),
    PHY_STATE(ST_LAN_ON),
    CLR(USB, 2, UR_U2P3, 0x0001),
    PUT(USB, 2, UR_MSC_TMR, 0x0fff),
    RUN(Q_POWER_CUT_OFF),
    CLR(USB, 1, UR_POWER_CUT, 0x30),
    CLR(USB, 1, UR_MISC2, 0x01),
    HOOK(H_UPS_CHECK),
    ONLY(K_6400, U_ANY, 1), RUN(Q_CFG34_BIT8),
    RUN(Q_QUEUE_WAKE_OFF),
    RUN(Q_NO_WAKE),
    HOOK(H_LINK_FLAGS),
    ONLY(K_6400, U_ANY, 1), RUN(Q_U1U2_ON),
    ONLY(K_8153B, U_SUPER, 1), RUN(Q_U1U2_ON),
    SET(MAC, 2, MR_POWER2, 0x8000),
    ONLY(K_6400, U_ANY, 1), CLR(USB, 1, UR_MISC2, 0x80),
    ONLY(K_8153B, U_ANY, 1), CLR(MAC, 2, MR_POWER3, 0x4000),
    ONLY(K_6010, U_ANY, 1), HOOK(H_PACKAGE),
    CLR(USB, 2, UR_CTRL, CTRL_RX_AGG_OFF | CTRL_RX_ZERO),
    SET(MAC, 2, MR_TALLY, 0x0001),
    END
};

/* ---- PHY setup ---- */

static const STEP SqPhy8152[] = {
    RUN(Q_EEE_ON),
    RUN(Q_ALDPS_ON),
    PHY_SET(MII(MII_AN_ADV), ANADV_PAUSE),
    END
};

static const STEP SqPhy8153[] = {
    RUN(Q_ALDPS_OFF),
    RUN(Q_EEE_OFF),
    ONLY(K_5C00, U_ANY, 1), PHY_CLR(PR_EEE_CFG, 0x0040),
    PHY_SET(PR_POWER, 0x8000),
    PHY_SET(PR_SPEED_DOWN, 0x0080),
    PHY_SET(PR_POWER, 0x0001),
    SRAM_PUT(SR_IMPEDANCE, 0x0b13),
    SET(MAC, 2, MR_PHY_POWER, 0x0040),
    SRAM_PUT(SR_LPF, 0xf70f),
    SRAM_PUT(SR_AMP_10A, 0x00af),
    SRAM_PUT(SR_AMP_10B, 0x0208),
    RUN(Q_EEE_ON),
    RUN(Q_ALDPS_ON),
    PHY_SET(MII(MII_AN_ADV), ANADV_PAUSE),
    ONLY(K_5C20 | K_5C30, U_ANY, 1), SET(USB, 2, UR_U2P3, 0x0001),
    END
};

static const STEP SqPhyLater[] = {
    TRIM(USB, 2, UR_MISC0, 0x0001),
    RUN(Q_ALDPS_OFF),
    RUN(Q_EEE_OFF),
    PUT(USB, 2, UR_U1U2_TMR, 500),          /* idle time before U1/U2/L1, us */
    HOOK(H_PHY_WAKE),
    HOOK(H_BIAS),
    SRAM_SET(SR_GREEN, 0x0800),
    PHY_SET(PR_NCTL, 0x0002),
    HOOK(H_ADC_TRIM),
    HOOK(H_SAW_TIMER),
    SET(MAC, 2, MR_PHY_POWER, 0x0040),
    HOOK(H_EEE_CLOCKS),
    RUN(Q_EEE_ON),
    RUN(Q_ALDPS_ON),
    PHY_SET(MII(MII_AN_ADV), ANADV_PAUSE),
    END
};

static const STEP SqBiasFull[] = {
    SRAM_PUT(SR_BIAS_10, 0x2444), SRAM_PUT(SR_BIAS_100, 0x2444), SRAM_PUT(SR_BIAS_1000, 0x2444),
    SRAM_SET(SR_GREEN, 0x8000),
    END
};
static const STEP SqBiasLow[] = {
    SRAM_PUT(SR_BIAS_10, 0x0000), SRAM_PUT(SR_BIAS_100, 0x1222), SRAM_PUT(SR_BIAS_1000, 0x0022),
    SRAM_SET(SR_GREEN, 0x8000),
    END
};

/* ---- out of OOB: receiver ready, not yet on ---- */

/* The 8153 and 8153B: thresholds and frame size after the quiesce. */
static const STEP SqFirstFill[] = {
    RUN(Q_QUIESCE),
    PUT(MAC, 1, MR_TX_MAX_PKT, 12 * 1024 / 64),
    SET(MAC, 2, MR_TX_CONFIG, TXC_AUTO_FIFO),
    RUN(Q_RESET_MAC),
    PUT(MAC, 4, MR_RXFIFO_TH1, 0x00080002UL),
    PUT(MAC, 2, MR_RXFIFO_TH2, 0x00a0),
    PUT(MAC, 2, MR_RXFIFO_TH3, 0x0110),
    PUT(MAC, 4, MR_TXFIFO_TH, 0x01000008UL),
    END
};

static const STEP SqWake8152[] = {
    RUN(Q_ALDPS_OFF),
    CLR(MAC, 4, MR_RX_ACCEPT, ACC_MASK),
    RUN(Q_RX_GATE),
    RUN(Q_TEREDO_OFF),
    PUT(MAC, 1, MR_CFG_LOCK, CFG_LOCKED),
    PUT(MAC, 1, MR_COMMAND, 0x00),
    RUN(Q_LEAVE_OOB),
    RUN(Q_RESET_MAC),
    PUT(MAC, 4, MR_RXFIFO_TH1, 0x00080002UL),
    ONLY(K_ANY, U_FULL, 2),
    PUT(MAC, 4, MR_RXFIFO_TH2, 0x00000060UL), PUT(MAC, 4, MR_RXFIFO_TH3, 0x00000078UL),
    ONLY(K_ANY, U_HIGH | U_SUPER, 2),
    PUT(MAC, 4, MR_RXFIFO_TH2, 0x00000038UL), PUT(MAC, 4, MR_RXFIFO_TH3, 0x00000048UL),
    PUT(MAC, 4, MR_TXFIFO_TH, 0x01000008UL),
    PUT(USB, 1, UR_TX_AGG, 0x03),
    PUT(USB, 4, UR_RX_BUF_TH, 0x7a120180UL),
    PUT(USB, 4, UR_TX_DMA, 0x00000101UL),
    CLR(MAC, 2, MR_CPLUS, CPLUS_RX_VLAN),
    PUT(MAC, 2, MR_RX_MAX_LEN, RX_MAX_LEN),
    SET(MAC, 2, MR_TX_CONFIG, TXC_AUTO_FIFO),
    RUN(Q_ALDPS_ON),
    END
};

static const STEP SqWake8153[] = {
    RUN(Q_U1U2_OFF),
    CLR(USB, 2, UR_U2P3, 0x0001),
    RUN(Q_ALDPS_OFF),
    RUN(Q_FIRST_FILL),
    SET(MAC, 1, MR_CFG6, 0x01),
    CLR(MAC, 1, MR_LANWAKE, 0x80),
    CLR(USB, 2, UR_SS_LINK1, 0x0002),
    RUN(Q_ALDPS_ON),
    ONLY(K_5C20 | K_5C30, U_ANY, 1), SET(USB, 2, UR_U2P3, 0x0001),
    RUN(Q_U1U2_ON),
    END
};

static const STEP SqWake8153B[] = {
    RUN(Q_U1U2_OFF),
    CLR(USB, 2, UR_U2P3, 0x0001),
    RUN(Q_ALDPS_OFF),
    RUN(Q_FIRST_FILL),
    PUT(USB, 4, UR_RX_BUF_TH, 0x00010001UL),
    CLR(MAC, 2, MR_POWER3, 0x4000),
    RUN(Q_ALDPS_ON),
    ONLY(K_ANY, U_SUPER, 1), RUN(Q_U1U2_ON),
    END
};

static const STEP SqWake8153C[] = {
    RUN(Q_U1U2_OFF),
    CLR(USB, 2, UR_U2P3, 0x0001),
    RUN(Q_ALDPS_OFF),
    RUN(Q_QUIESCE),
    PUT(MAC, 1, MR_TX_MAX_PKT, 10 * 1024 / 64),
    PUT(MAC, 2, MR_TXFIFO_TH, 512 / 64),
    PUT(MAC, 2, MR_TXFIFO_FULL, 2048 / 8),
    RUN(Q_RESET_MAC),
    PUT(MAC, 1, MR_RXFIFO_TH1, 0x02),
    PUT(MAC, 1, MR_RXFIFO_TH1_HI, 0x08),
    PUT(MAC, 2, MR_RXFIFO_TH2, 0x00a0),
    PUT(MAC, 2, MR_RXFIFO_TH3, 0x0110),
    PUT(USB, 4, UR_RX_BUF_TH, 0x00010001UL),
    RUN(Q_CFG34_BIT8),
    CLR(MAC, 2, MR_POWER3, 0x4000),
    RUN(Q_ALDPS_ON),
    RUN(Q_U1U2_ON),
    END
};

/* ---- back into OOB (Stop, Suspend) ---- */

static const STEP SqSleep[] = {
    ONLY(K_8152, U_ANY, 1), RUN(Q_POWER_CUT_OFF),
    ONLY(K_8153, U_ANY, 1), CLR(MAC, 1, MR_CFG6, 0x01),
    ONLY(K_LATER, U_ANY, 1), SET(MAC, 2, MR_POWER3, 0x4000),
    ONLY(K_GIG, U_ANY, 3), RUN(Q_U1U2_OFF), CLR(USB, 2, UR_U2P3, 0x0001), RUN(Q_POWER_CUT_OFF),
    RUN(Q_ALDPS_OFF),
    CLR(MAC, 1, MR_OOB, OOB_ACTIVE),
    PUT(MAC, 4, MR_RXFIFO_TH1, 0x01800003UL),
    ONLY(K_8152, U_ANY, 2),
    PUT(MAC, 4, MR_RXFIFO_TH2, 0x0000004aUL), PUT(MAC, 4, MR_RXFIFO_TH3, 0x0000005aUL),
    ONLY(K_GIG, U_ANY, 2),
    PUT(MAC, 2, MR_RXFIFO_TH2, 0x004a), PUT(MAC, 2, MR_RXFIFO_TH3, 0x005a),
    RUN(Q_DRAIN),
    ONLY(K_GIG, U_ANY, 1), RUN(Q_RESET_BMU),
    RUN(Q_REBUILD_LIST),
    PUT(MAC, 2, MR_RX_MAX_LEN, RX_MAX_LEN),
    ONLY(K_GIG, U_ANY, 1), PUT(MAC, 1, MR_TX_MAX_PKT, 6 * 1024 / 64),
    ONLY(K_8153, U_ANY, 1), CLR(MAC, 2, MR_TEREDO, 0x7f00),
    ONLY(K_LATER, U_ANY, 1), PUT(MAC, 2, MR_TEREDO_WAKE, 0x00ff),
    SET(MAC, 2, MR_CPLUS, CPLUS_RX_VLAN),
    SET(MAC, 2, MR_BDC, 0x0001),
    SET(MAC, 1, MR_OOB, OOB_ACTIVE | OOB_HOLD),
    ONLY(K_GIG, U_ANY, 1), SET(MAC, 2, MR_SFF7, 0x4000),
    RUN(Q_RX_UNGATE),
    SET(MAC, 4, MR_RX_ACCEPT, ACC_MY_PHYS | ACC_MCAST | ACC_BCAST),
    RUN(Q_ALDPS_ON),
    END
};

/* After Sleep, when the driver lets go of the chip. */
static const STEP SqDetach[] = {
    ONLY(K_4C10 | K_4800, U_ANY, 2), SET(USB, 2, UR_UPS, 0x0100), CLR(USB, 2, UR_PM_STATUS, 0x0001),
    ONLY(K_8153 | K_6400, U_ANY, 2), CLR(USB, 2, UR_POWER_CUT, 0x0009), CLR(USB, 2, UR_MISC0, 0x0001),
    ONLY(K_8153B, U_ANY, 2), CLR(USB, 2, UR_POWER_CUT, 0x0001), CLR(USB, 2, UR_MISC0, 0x0001),
    END
};

/* ---- link up and down ---- */

#define RX_EARLY(buffer) (((buffer) - (RX_MAX_LEN + RX_DESC_LEN + RX_RECORD_ALIGN)))

static const STEP SqRxOn[] = {
    HOOK(H_EEE_PLUS),
    /* Early receive: timeout in 8 ns units, size in 4 (8153) or 8 bytes. */
    ONLY(K_8153, U_SUPER, 2),
    PUT(USB, 2, UR_RX_EARLY_TMO, 85000UL / 8), PUT(USB, 2, UR_RX_EARLY_LEN, RX_EARLY(32768UL) / 4),
    ONLY(K_8153, U_HIGH, 2),
    PUT(USB, 2, UR_RX_EARLY_TMO, 250000UL / 8), PUT(USB, 2, UR_RX_EARLY_LEN, RX_EARLY(16384UL) / 4),
    ONLY(K_8153, U_FULL, 2),
    PUT(USB, 2, UR_RX_EARLY_TMO, 524280UL / 8), PUT(USB, 2, UR_RX_EARLY_LEN, RX_EARLY(16384UL) / 4),
    ONLY(K_LATER, U_ANY, 3),
    PUT(USB, 2, UR_RX_EARLY_TMO, 128 / 8), PUT(USB, 2, UR_PM_STATUS, 15000UL / 8),
    PUT(USB, 2, UR_RX_EARLY_LEN, RX_EARLY(32768UL) / 8),
    ONLY(K_GIG, U_ANY, 1), HOOK(H_GAP),
    ONLY(K_6010 | K_6400, U_ANY, 3),
    CLR(USB, 2, UR_FW_TASK, 0x0002), SLEEP(1), SET(USB, 2, UR_FW_TASK, 0x0002),
    /* filter MCU restart, receiver and transmitter on, gate open */
    CLR(MAC, 2, MR_FILTER_MCU, 0x0001),
    SET(MAC, 2, MR_FILTER_MCU, 0x0001),
    SET(MAC, 1, MR_COMMAND, CMD_RX_ON | CMD_TX_ON),
    ONLY(K_LATER, U_ANY, 1), PUT(USB, 1, UR_RXDMA_OWN, 0x03),
    RUN(Q_RX_UNGATE),
    END
};

static const STEP SqRxOff[] = {
    RUN(Q_ALDPS_OFF),
    RUN(Q_DRAIN),
    ONLY(K_GIG, U_ANY, 1), RUN(Q_RESET_BMU),
    RUN(Q_ALDPS_ON),
    END
};

static const STEP *const Seq[] = {
    SqRxGate, SqRxUngate, SqTeredoOff, SqResetMac, SqResetBmu, SqRebuildList,
    SqLeaveOob, SqQuiesce, SqPowerCutOff, SqU1U2Off, SqU1U2On, SqAldpsOff, SqAldpsOn,
    SqEeeOff, SqEeeOn, SqNoWake, SqCfg34Bit8, SqAutoload, SqDrain, SqQueueWakeOff,
    SqSetup8152, SqSetup8153, SqSetupLater, SqPhy8152, SqPhy8153, SqPhyLater,
    SqFirstFill, SqWake8152, SqWake8153, SqWake8153B, SqWake8153C, SqSleep, SqDetach,
    SqRxOn, SqRxOff, SqBiasFull, SqBiasLow
};

/* ------------------------------------------------------------------ */
/* The interpreter */

static void Run(RTL_STATE *s, NET_IO *io, int q);

/* Polls a register up to Count times, Ms apart: OP_WAIT and OP_PHY_STATE
 * read first and sleep after a miss, the others sleep before each read.
 * Returns 1 on a match; *last (if given) receives the last masked value.
 * A wait that runs out on a working device counts in Timeouts, except
 * the two "late" kinds, which are short settling waits. */
static int WaitFor(RTL_STATE *s, NET_IO *io, const STEP *p, nc_u32 *last)
{
    int early = p->Op == OP_WAIT || p->Op == OP_PHY_STATE;
    int phy = p->Op == OP_PHY_WAIT || p->Op == OP_PHY_STATE;
    nc_u32 i, v = 0;
    for (i = 0; i < p->Count && io->Status >= 0; i++) {
        if (!early) NcIoSleep(io, p->Ms);
        v = phy ? PhyGet(s, io, p->Reg)
                : Get(io, (nc_u16)(p->Space ? RTL_SPACE_MAC : RTL_SPACE_USB), p->Reg, p->Size);
        if (io->Status < 0) break;
        v &= p->Clear;
        if (p->Op == OP_PHY_STATE ? ((p->Set >> v) & 1) != 0 : v == p->Set) {
            if (last) *last = v;
            return 1;
        }
        if (early) NcIoSleep(io, p->Ms);
    }
    if (last) *last = v;
    if (early && io->Status >= 0) s->Timeouts++;
    return 0;
}

static void Hook(RTL_STATE *s, NET_IO *io, int h);

static void Run(RTL_STATE *s, NET_IO *io, int q)
{
    const STEP *p;
    for (p = Seq[q]; p->Op != OP_END; p++) {
        nc_u16 space = (nc_u16)(p->Space ? RTL_SPACE_MAC : RTL_SPACE_USB);
        if (io->Status < 0) return;
        switch (p->Op) {
        case OP_ONLY:
            if (!(p->Reg & s->Kind) || !(p->Clear & SpeedBit(s))) p += p->Count;
            break;
        case OP_PUT: Put(io, space, p->Reg, p->Size, p->Set); break;
        case OP_EDIT: Edit(io, space, p->Reg, p->Size, p->Clear, p->Set); break;
        case OP_TRIM: {
            nc_u32 v = Get(io, space, p->Reg, p->Size);
            if (v & p->Clear) Put(io, space, p->Reg, p->Size, v & ~p->Clear);
            break;
        }
        case OP_SIX: {
            nc_u8 b[8];
            int i;
            for (i = 0; i < 8; i++) b[i] = (nc_u8)p->Set;
            PutEight(io, space, p->Reg, b, 1);
            break;
        }
        case OP_WAIT: case OP_WAIT_LATE: case OP_PHY_WAIT: case OP_PHY_STATE:
            WaitFor(s, io, p, 0);
            break;
        case OP_PHY_PUT: PhyPut(s, io, p->Reg, (nc_u16)p->Set); break;
        case OP_PHY_EDIT:
            PhyPut(s, io, p->Reg, (nc_u16)((PhyGet(s, io, p->Reg) & ~p->Clear) | p->Set));
            break;
        case OP_PHY_TRIM: {
            nc_u16 v = PhyGet(s, io, p->Reg);
            if (v & p->Clear) PhyPut(s, io, p->Reg, (nc_u16)(v & ~p->Clear));
            break;
        }
        case OP_SRAM_PUT:
            PhyPut(s, io, PR_SRAM_ADDR, p->Reg);
            PhyPut(s, io, PR_SRAM_DATA, (nc_u16)p->Set);
            break;
        case OP_SRAM_EDIT: {
            nc_u16 v;
            PhyPut(s, io, PR_SRAM_ADDR, p->Reg);
            v = PhyGet(s, io, PR_SRAM_DATA);
            PhyPut(s, io, PR_SRAM_DATA, (nc_u16)((v & ~p->Clear) | p->Set));
            break;
        }
        case OP_SLEEP: NcIoSleep(io, p->Ms); break;
        case OP_RUN: Run(s, io, p->Count); break;
        case OP_HOOK: Hook(s, io, p->Count); break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Hooks: the steps that depend on a value read back */

/* Autonegotiation: 10/100 (and 1000 full on a gigabit chip) advertised,
 * then the PHY reset with autonegotiation enabled and restarted. */
static void Negotiate(RTL_STATE *s, NET_IO *io)
{
    static const STEP reset = PHY_WAIT(PR_MII, MIICTL_RESET, 0, 50, 20);
    nc_u16 was = PhyGet(s, io, MII(MII_AN_ADV));
    if ((was | ANADV_10_100) != was) PhyPut(s, io, MII(MII_AN_ADV), (nc_u16)(was | ANADV_10_100));
    if (s->Kind & K_GIG) {
        nc_u16 now;
        was = PhyGet(s, io, MII(MII_GIG_CONTROL));
        now = (nc_u16)((was & ~GIGCTL_1000_MASK) | GIGCTL_1000_FULL);
        if (now != was) PhyPut(s, io, MII(MII_GIG_CONTROL), now);
    }
    PhyPut(s, io, MII(MII_CONTROL), MIICTL_RESET | MIICTL_AN_ENABLE | MIICTL_AN_RESTART);
    WaitFor(s, io, &reset, 0);
}

static int PhySequence(const RTL_STATE *s)
{
    return (s->Kind & K_8152) ? Q_PHY_8152 : (s->Kind & K_8153) ? Q_PHY_8153 : Q_PHY_LATER;
}

static void Hook(RTL_STATE *s, NET_IO *io, int h)
{
    nc_u32 v;
    switch (h) {
    case H_PHY_WAKE: {  /* a PHY found powered down is powered up */
        static const STEP settled = PHY_STATE(ST_SETTLED);
        WaitFor(s, io, &settled, &v);
        if (io->Status < 0) break;
        if (v == PHYST_POWER_DOWN || v == PHYST_EXT_INIT)
            PhyPut(s, io, MII(MII_CONTROL), (nc_u16)(PhyGet(s, io, MII(MII_CONTROL)) & ~MIICTL_POWERDOWN));
        break;
    }
    case H_BURST:       /* dynamic burst follows whether a burst size is set */
        if (Get(io, RTL_SPACE_USB, UR_BURST, 2) == 0) Edit(io, RTL_SPACE_USB, UR_DUMMY1, 1, 0x01, 0);
        else Edit(io, RTL_SPACE_USB, UR_DUMMY1, 1, 0, 0x01);
        break;
    case H_LINK_FLAGS:  /* the firmware's link flags follow the link now */
        if (Get(io, RTL_SPACE_MAC, MR_LINK, 2) & LNK_UP) Edit(io, RTL_SPACE_MAC, MR_EXTRA_STATUS, 2, 0, 0x8001);
        else Edit(io, RTL_SPACE_MAC, MR_EXTRA_STATUS, 2, 0x8000, 0x0001);
        break;
    case H_PACKAGE:     /* the 32-pin package */
        if (Get(io, RTL_SPACE_MAC, MR_PACKAGE, 1) & 0x20) Edit(io, RTL_SPACE_MAC, MR_PHY_POWER, 2, 0, 0x0010);
        break;
    case H_UPS_CHECK:   /* power was cut: the PHY is set up again */
        if (Get(io, RTL_SPACE_USB, UR_MISC0, 2) & 0x0001) {
            Run(s, io, Q_AUTOLOAD);
            if (io->Status < 0) break;
            Run(s, io, PhySequence(s));
            Negotiate(s, io);
        }
        break;
    case H_BIAS:        /* PHY bias: lower once the chip has been set up */
        Run(s, io, s->SetUpOnce ? Q_BIAS_LOW : Q_BIAS_FULL);
        break;
    case H_ADC_TRIM: {  /* ADC offset from efuse word 0x7d (17 bits, bit 3 unused) */
        nc_u16 trim;
        Put(io, RTL_SPACE_MAC, MR_EFUSE_CMD, 2, 0x8000 | 0x7d);
        v = (Get(io, RTL_SPACE_MAC, MR_EFUSE_CMD, 2) & 0x0080) << 9;
        v |= Get(io, RTL_SPACE_MAC, MR_EFUSE_DATA, 2);
        trim = (nc_u16)(((v >> 4) << 3) | (v & 0x7));
        if (trim != 0xffff) PhyPut(s, io, PR_ADC_OFFSET, trim);
        break;
    }
    case H_SAW_TIMER:   /* ticks per millisecond from the PHY's saw counter */
        v = PhyGet(s, io, PR_SAW_COUNT) & 0x3fff;
        if (v) Edit(io, RTL_SPACE_USB, UR_UPS_CFG, 2, 0x0fff, (16000000UL / v) & 0x0fff);
        break;
    case H_EEE_CLOCKS: {
        /* PHY clocks for EEE, changed only while the PHY grants a patch
         * request; a request not granted is withdrawn. */
        static const STEP granted = PHY_WAIT(PR_PATCH_STATE, PATCH_GRANTED, PATCH_GRANTED, 5000, 1);
        static const STEP released = PHY_WAIT(PR_PATCH_STATE, PATCH_GRANTED, 0, 5000, 1);
        PhyPut(s, io, PR_PATCH_CMD, (nc_u16)(PhyGet(s, io, PR_PATCH_CMD) | PATCH_ASK));
        WaitFor(s, io, &granted, 0);
        if (io->Status < 0) break;
        if (!(PhyGet(s, io, PR_PATCH_STATE) & PATCH_GRANTED)) {
            PhyPut(s, io, PR_PATCH_CMD, (nc_u16)(PhyGet(s, io, PR_PATCH_CMD) & ~PATCH_ASK));
            break;
        }
        PhyPut(s, io, PR_POWER, (nc_u16)(PhyGet(s, io, PR_POWER) | 0x8000));
        PhyPut(s, io, PR_SPEED_DOWN, (nc_u16)(PhyGet(s, io, PR_SPEED_DOWN) | 0x6800));
        PhyPut(s, io, PR_SYSCLK, 0);
        PhyPut(s, io, PR_SYSCLK, 5 << 4);
        PhyPut(s, io, PR_PATCH_CMD, (nc_u16)(PhyGet(s, io, PR_PATCH_CMD) & ~PATCH_ASK));
        WaitFor(s, io, &released, 0);
        break;
    }
    case H_EEE_PLUS:    /* EEE+ on at 10 Mb/s only */
        if (Get(io, RTL_SPACE_MAC, MR_LINK, 2) & LNK_10) Edit(io, RTL_SPACE_MAC, MR_EEE_PLUS, 2, 0, 0x0002);
        else Edit(io, RTL_SPACE_MAC, MR_EEE_PLUS, 2, 0x0002, 0);
        break;
    case H_GAP:         /* the long inter-frame gap at 10/100 half duplex */
        v = Get(io, RTL_SPACE_MAC, MR_LINK, 2);
        if ((v & (LNK_10 | LNK_100)) && !(v & LNK_FULL)) {
            Edit(io, RTL_SPACE_MAC, MR_TX_CONFIG1, 2, IFG_FIELD, IFG_LONG);
            Edit(io, RTL_SPACE_MAC, MR_POWER4, 2, 0x0100, 0);
        } else {
            Edit(io, RTL_SPACE_MAC, MR_TX_CONFIG1, 2, IFG_FIELD, IFG_SHORT);
            Edit(io, RTL_SPACE_MAC, MR_POWER4, 2, 0, 0x0100);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* The netchip.h interface */

nc_u16 RtlChipId(nc_u32 txConfig)
{
    nc_u16 id = (nc_u16)((txConfig >> 16) & TXC_ID_MASK);
    unsigned i;
    for (i = 0; i < sizeof(Ids) / sizeof(Ids[0]); i++)
        if (Ids[i] == id) return id;
    return 0;
}

static nc_u16 KindOf(nc_u16 id)
{
    unsigned i;
    for (i = 0; i < sizeof(Ids) / sizeof(Ids[0]); i++)
        if (Ids[i] == id) return (nc_u16)(1u << i);
    return 0;
}

/* Bulk-IN buffer: 16 KiB on the 8152, and on the 8153 below SuperSpeed. */
static nc_u32 RxBuffer(const RTL_STATE *s)
{
    if (s->Kind & K_8152) return 16384;
    if ((s->Kind & K_8153) && s->UsbSpeed != NC_SPEED_SUPER) return 16384;
    return 32768;
}

long RtlProbe(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
              const nc_u32 *size, nc_u32 count, NET_PROBE *probe)
{
    nc_u32 i;
    if (NcParseDevice(device, deviceSize, probe) != NC_OK) return NC_E_MALFORMED;
    /* The vendor configuration is the one whose first interface is class
     * FF; it need not be the first configuration. Its endpoints are 1
     * (bulk IN), 2 (bulk OUT) and 3 (interrupt IN). */
    for (i = 0; i < count; i++) {
        long r;
        if (!config[i] || !size[i]) continue;
        r = NcFindVendorInterface(config[i], size[i], 1, probe);
        if (r == NC_E_MALFORMED) return r;
        if (r != NC_OK) continue;
        if ((probe->In & 0x0f) != 1 || (probe->Out & 0x0f) != 2 || (probe->Interrupt & 0x0f) != 3)
            return NC_E_UNSUPPORTED;
        probe->ConfigIndex = (nc_u8)i;
        return NC_OK;
    }
    return NC_E_UNSUPPORTED;
}

static void WriteNodeId(NET_IO *io, const nc_u8 mac[6])
{
    nc_u8 b[8];
    int i;
    for (i = 0; i < 6; i++) b[i] = mac[i];
    b[6] = b[7] = 0;
    Put(io, RTL_SPACE_MAC, MR_CFG_LOCK, 1, CFG_UNLOCKED);
    PutEight(io, RTL_SPACE_MAC, MR_NODE_ID, b, 1);
    Put(io, RTL_SPACE_MAC, MR_CFG_LOCK, 1, CFG_LOCKED);
}

static long BringUp(RTL_STATE *s, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params, int again)
{
    nc_u8 b[8];
    int i;
    if (!again) {
        nc_u8 *p = (nc_u8 *)s;
        for (i = 0; i < (int)sizeof(*s); i++) p[i] = 0;
        s->UsbSpeed = probe->Speed;
        /* The chip id, in MR_TX_CONFIG's high word; three tries. */
        for (i = 0; i < 3; i++)
            if (NcIoTry(io, RTL_REQ, MR_TX_CONFIG, RTL_SPACE_MAC, b, 4) >= 0) break;
        if (i == 3) { io->Status = NC_E_IO; return io->Status; }
        s->Id = RtlChipId(Le32(b));
        if (!s->Id) return NC_E_UNSUPPORTED;
        s->Kind = KindOf(s->Id);
    }
    s->PhyPage = 0xffff;
    s->Carrier = 0;

    Run(s, io, (s->Kind & K_8152) ? Q_SETUP_8152 : (s->Kind & K_8153) ? Q_SETUP_8153 : Q_SETUP_LATER);
    if (s->Kind & K_LATER) s->SetUpOnce = 1;
    Run(s, io, PhySequence(s));
    Negotiate(s, io);
    /* The station address: the 4C00 keeps it in MR_NODE_ID; the others
     * load it into MR_NODE_ID_SAVED, from where it is copied. */
    NcIoRead(io, RTL_REQ, (nc_u16)(s->Id == ID_4C00 ? MR_NODE_ID : MR_NODE_ID_SAVED), RTL_SPACE_MAC, b, 8);
    for (i = 0; i < 6; i++) s->Mac[i] = b[i];
    if (s->Id != ID_4C00 && NcValidMac(s->Mac)) WriteNodeId(io, s->Mac);
    Run(s, io, (s->Kind & K_8152) ? Q_WAKE_8152 : (s->Kind & K_8153) ? Q_WAKE_8153 :
               (s->Kind & K_8153B) ? Q_WAKE_8153B : Q_WAKE_8153C);
    if (io->Status < 0) return io->Status;

    for (i = 0; i < 6; i++) params->Mac[i] = s->Mac[i];
    params->RxSize = RxBuffer(s);
    params->RxCount = 4;
    params->TxSize = TX_BUFFER;
    params->TxSlots = 4;
    params->TxBatch = 32;
    params->TxHeader = TX_DESC_LEN;
    params->TxAlign = TX_ALIGN;
    params->Version = s->Id;
    return NC_OK;
}

static long RtlStart(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params)
{
    return BringUp(chip, io, probe, params, 0);
}

static long RtlResume(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params)
{
    return BringUp(chip, io, probe, params, 1);
}

static long RtlSetMac(void *chip, NET_IO *io, const nc_u8 mac[6])
{
    RTL_STATE *s = chip;
    int i;
    WriteNodeId(io, mac);
    if (io->Status < 0) return io->Status;
    for (i = 0; i < 6; i++) s->Mac[i] = mac[i];
    return NC_OK;
}

/* The receive mode: broadcast and own address always, plus the filter's
 * bits, and the hash table. */
static void ApplyFilter(RTL_STATE *s, NET_IO *io)
{
    nc_u32 accept = Get(io, RTL_SPACE_MAC, MR_RX_ACCEPT, 4);
    accept = (accept & ~(nc_u32)ACC_MASK) | ACC_BCAST | ACC_MY_PHYS | s->Accept;
    PutEight(io, RTL_SPACE_MAC, MR_HASH, s->Hash, 0);
    Put(io, RTL_SPACE_MAC, MR_RX_ACCEPT, 4, accept);
}

static long RtlLink(void *chip, NET_IO *io, NET_LINK *link)
{
    RTL_STATE *s = chip;
    nc_u32 st = Get(io, RTL_SPACE_MAC, MR_LINK, 2);
    if (io->Status < 0) return io->Status;
    link->RxLength = RxBuffer(s);
    if (!(st & LNK_UP)) {
        if (s->Carrier) {
            s->Carrier = 0;
            Run(s, io, Q_RX_OFF);
        }
        link->Up = 0;
        link->Mbps = 0;
        link->FullDuplex = 0;
        return io->Status < 0 ? io->Status : NC_OK;
    }
    if (!s->Carrier) {
        Run(s, io, Q_RX_ON);
        ApplyFilter(s, io);
        if (io->Status < 0) return io->Status;
        s->Carrier = 1;
    }
    link->Up = 1;
    link->Mbps = (st & LNK_1000) ? 1000 : (st & LNK_100) ? 100 : 10;
    link->FullDuplex = (st & LNK_FULL) ? 1 : 0;
    return io->Status < 0 ? io->Status : NC_OK;
}

static int RtlPoll(void *chip, NET_IO *io)
{
    RTL_STATE *s = chip;
    int up = (Get(io, RTL_SPACE_MAC, MR_LINK, 2) & LNK_UP) != 0;
    if (io->Status < 0) return 0;
    return up != (s->Carrier != 0);
}

/* 64 bins: the top six bits of the Ethernet CRC (NcEtherCrc) of each
 * address. Bins 32-63 go to hash bytes 0-3 and bins 0-31 to bytes 4-7,
 * the highest bin of each half in the first byte. More than
 * NC_MULTICAST_MAX addresses accept all multicast. */
void RtlMulticastHash(nc_u32 filter, const nc_u8 *multicast, nc_u32 count, nc_u32 *accept,
                      nc_u8 hash[8])
{
    nc_u32 i;
    nc_u8 fill = 0;
    *accept = 0;
    if (filter & NC_FILTER_PROMISCUOUS) {
        *accept = ACC_ALL_PHYS | ACC_MCAST;
        fill = 0xff;
    } else if ((filter & NC_FILTER_ALL_MULTICAST) || count > NC_MULTICAST_MAX) {
        *accept = ACC_MCAST;
        fill = 0xff;
    }
    for (i = 0; i < 8; i++) hash[i] = fill;
    if (fill || !(filter & NC_FILTER_MULTICAST)) return;
    for (i = 0; i < count; i++) {
        nc_u32 bin = NcEtherCrc(multicast + 6 * i, 6) >> 26;
        int at = (bin >= 32 ? 3 : 7) - (int)((bin & 31) >> 3);
        hash[at] = (nc_u8)(hash[at] | (1u << (bin & 7)));
        *accept = ACC_MCAST;
    }
}

static long RtlSetFilter(void *chip, NET_IO *io, nc_u32 filter, const nc_u8 *multicast, nc_u32 count)
{
    RTL_STATE *s = chip;
    RtlMulticastHash(filter, multicast, count, &s->Accept, s->Hash);
    /* With the link down it is applied when the link comes up. */
    if (s->Carrier) ApplyFilter(s, io);
    return io->Status < 0 ? io->Status : NC_OK;
}

static long RtlStop(void *chip, NET_IO *io)
{
    RTL_STATE *s = chip;
    if (!s->Id) return NC_OK;      /* Start refused the chip: nothing to undo */
    s->Carrier = 0;
    Run(s, io, Q_SLEEP);
    Run(s, io, Q_DETACH);
    return io->Status < 0 ? io->Status : NC_OK;
}

static long RtlSuspend(void *chip, NET_IO *io)
{
    RTL_STATE *s = chip;
    if (!s->Id) return NC_OK;
    s->Carrier = 0;
    Run(s, io, Q_SLEEP);
    return io->Status < 0 ? io->Status : NC_OK;
}

static int RtlStatus(void *chip, const nc_u8 *data, nc_u32 length)
{
    (void)chip;
    if (length < 2) return -1;
    return (data[0] & IRQ_LINK) ? 1 : 0;
}

/* One bulk-IN transfer holds records: a 24-byte header whose first dword
 * gives, in bits 0-14, the frame length with its 4-byte FCS, then the
 * frame, then padding to 8 bytes. A length below 60 marks the end. The
 * transfer is checked whole (first pass) before any frame is handed up
 * (second pass); a frame running past its end rejects it. Frames longer
 * than NC_ETH_MAX are counted as dropped. */
long RtlRxParse(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
                NET_RX_STATS *stats)
{
    int deliver;
    nc_u32 frames = 0, dropped = 0;
    (void)chip;
    stats->Frames = 0;
    stats->Dropped = 0;
    if (length < NC_ETH_PAD) return 0;
    for (deliver = 0; deliver < 2; deliver++) {
        nc_u32 at = 0;
        frames = dropped = 0;
        while (at < length && length - at > RX_DESC_LEN) {
            nc_u32 left = length - at - RX_DESC_LEN;
            nc_u32 wire = Le32(data + at) & 0x7fff;
            if (wire < NC_ETH_PAD) break;
            if (wire > left) return NC_E_MALFORMED;
            if (wire - 4 <= NC_ETH_MAX) {
                if (deliver) emit(context, data + at + RX_DESC_LEN, wire - 4);
                frames++;
            } else {
                dropped++;
            }
            at = (at + RX_DESC_LEN + wire + RX_RECORD_ALIGN - 1) & ~(nc_u32)(RX_RECORD_ALIGN - 1);
        }
    }
    stats->Frames = frames;
    stats->Dropped = dropped;
    return (long)frames;
}

/* Each frame is preceded by an 8-byte header: first and last segment
 * bits and the length; no offloads. */
static void RtlTxHeader(void *chip, nc_u8 *hdr, nc_u32 length, nc_u32 maxPacket, int single)
{
    (void)chip; (void)maxPacket; (void)single;
    SetLe32(hdr, TX_FIRST_LAST | length);
    SetLe32(hdr + 4, 0);
}

/* No padding and no zero-length packet after a transfer. */
static int RtlTxEnd(void *chip, nc_u8 *buffer, nc_u32 *total, nc_u32 capacity, nc_u32 maxPacket)
{
    (void)chip; (void)buffer; (void)total; (void)capacity; (void)maxPacket;
    return 0;
}

const NET_CHIP NetPackageChip = {
    "rtl815x",
    "usb-wdm-free RTL815X (Realtek RTL8152/RTL8153 USB Ethernet)",
    0x0001,
    RtlProbe, RtlStart, RtlSetMac, RtlLink, RtlPoll, RtlSetFilter, RtlStop, RtlSuspend, RtlResume,
    RtlStatus, RtlRxParse, RtlTxHeader, RtlTxEnd
};

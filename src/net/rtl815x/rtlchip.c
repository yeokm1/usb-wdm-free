/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * RTL815X chip core behind the netchip.h interface (ethernet.md section
 * 6.4): Realtek RTL8152 (ids 4C00, 4C10, 4800), RTL8153 (5C00 to 5C30),
 * RTL8153B (6000, 6010), RTL8153C (6400), RTL8156 (7020, 7030), RTL8156B
 * (7400, 7410), RTL8153D (7420), RTL8157 (1030, 5 Gb/s) and RTL8159
 * (2020, 10 Gb/s) in their vendor configuration.
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
    ID_4C00, ID_4C10, ID_4800, ID_5C00, ID_5C10, ID_5C20, ID_5C30, ID_6000, ID_6010, ID_6400,
    ID_7020, ID_7030, ID_7400, ID_7410, ID_7420, ID_1030, ID_2020
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
#define K_7020  0x0400UL
#define K_7030  0x0800UL
#define K_7400  0x1000UL
#define K_7410  0x2000UL
#define K_7420  0x4000UL
#define K_1030  0x8000UL                    /* RTL8157 */
#define K_2020  0x10000UL                   /* RTL8159 */
#define K_8152  (K_4C00 | K_4C10 | K_4800)
#define K_8153  (K_5C00 | K_5C10 | K_5C20 | K_5C30)
#define K_8153B (K_6000 | K_6010)
#define K_LATER (K_8153B | K_6400)          /* 8153B and 8153C */
#define K_GIG   (K_8153 | K_LATER)
#define K_56A   (K_7020 | K_7030)           /* RTL8156 */
#define K_56B   (K_7400 | K_7410 | K_7420)  /* RTL8156B and RTL8153D */
#define K_57    (K_1030 | K_2020)           /* RTL8157 and RTL8159 */
#define K_56    (K_56A | K_56B)             /* the 8156 core, first descriptor format */
#define K_MG    (K_56 | K_57)               /* the 8156 core and its successors */
#define K_2G5   (K_56A | K_7400 | K_7410)   /* 2.5 Gb/s capable */
#define K_ANY   0xffffffffUL

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
    OP_ONLY,        /* Clear (kinds), Reg (speeds): the next Count steps run only on a match */
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
    OP_SRAM_PUT,    /* PHY SRAM Reg <- Set (Space 1: the second SRAM window) */
    OP_SRAM_EDIT,   /* PHY SRAM Reg: clear Clear, set Set (Space as OP_SRAM_PUT) */
    OP_ADV_EDIT,    /* indirect register Reg of space Count (ADV_*): clear Clear, set Set */
    OP_PATCH_ASK,   /* ask the PHY for a patch window; refused: withdraw and Abort */
    OP_PATCH_DONE,  /* end the patch window */
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
#define ONLY(kinds, speeds, n)   { OP_ONLY, 0, 0, 0, (nc_u16)(speeds), (nc_u16)(n), (kinds), 0 }
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
#define SRAM_EDIT(r, c, v)       { OP_SRAM_EDIT, 2, 0, 0, r, 0, (c), (v) }
#define SRAM2_PUT(r, v)          { OP_SRAM_PUT, 2, 0, 1, r, 0, 0, (v) }
#define SRAM2_EDIT(r, c, v)      { OP_SRAM_EDIT, 2, 0, 1, r, 0, (c), (v) }
#define ADV_EDIT(sp, r, c, v)    { OP_ADV_EDIT, 4, 0, 0, r, sp, (c), (v) }
#define PATCH_OPEN               { OP_PATCH_ASK, 0, 0, 0, 0, 0, 0, 0 }
#define PATCH_CLOSE              { OP_PATCH_DONE, 0, 0, 0, 0, 0, 0, 0 }
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
    Q_RX_ON, Q_RX_OFF, Q_BIAS_FULL, Q_BIAS_LOW,
    Q_SETUP_MG, Q_PHY_56A, Q_PHY_7020, Q_PHY_7030, Q_PHY_56B, Q_PHY_7400_PRE, Q_PHY_7400,
    Q_PHY_7410, Q_PHY_7420, Q_PHY_MG_TAIL, Q_NIC_RESET_MG, Q_DRAIN_MG, Q_WAKE_MG, Q_RX_ON_MG,
    Q_RX_OFF_MG, Q_SLEEP_MG, Q_DETACH_MG, Q_PHY_57, Q_PHY_1030, Q_PHY_2020
};

/* Hook numbers (Hook()) */
enum {
    H_PHY_WAKE, H_BURST, H_LINK_FLAGS, H_PACKAGE, H_UPS_CHECK,
    H_BIAS, H_ADC_TRIM, H_SAW_TIMER, H_EEE_CLOCKS, H_EEE_PLUS, H_GAP,
    H_FLASH_WAIT, H_RESTORE_WAIT, H_EXT_INIT, H_EXT_INIT_PHY, H_FW_CTRL, H_UPS_CHECK_MG,
    H_MDI_SWAP, H_RX_EARLY, H_IDLE_2G5, H_EEE_TW, H_L1_NYET
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
    ONLY(K_LATER | K_MG, U_ANY, 1), PUT(MAC, 1, MR_TEREDO, 0xff),     /* low byte: write 1 to clear */
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
    ONLY(K_GIG | K_MG, U_ANY, 2), PHY_CLR(PR_POWER, 0x0004),
    WAIT_LATE(MAC, 2, MR_POWER_STATE, 0x0100, 0x0100, 20, 1),
    END
};
static const STEP SqAldpsOn[] = {
    ONLY(K_8152, U_ANY, 1), PHY_PUT(PR_ALDPS_A, 0x8310),
    ONLY(K_GIG | K_MG, U_ANY, 1), PHY_SET(PR_POWER, 0x0004),
    END
};

/* Energy-efficient Ethernet: MAC side, PHY side, advertisement. */
static const STEP SqEeeOff[] = {                       /* 8153 and later only */
    CLR(MAC, 2, MR_EEE, 0x0003),
    PHY_CLR(PR_EEE_CFG, 0x0010),
    ONLY(K_MG, U_ANY, 1), PHY_CLR(PR_EEE_ADV2, EEEADV2_2G5 | EEEADV2_5G),
    PHY_PUT(PR_EEE_ADV, 0),
    END
};
static const STEP SqEeeOn[] = {
    SET(MAC, 2, MR_EEE, 0x0003),
    ONLY(K_8152, U_ANY, 8),
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
    /* The 8156 core: 2.5G EEE advertised too (also by the 1G 8153D) */
    ONLY(K_56, U_ANY, 3),
    PHY_SET(PR_EEE_CFG, 0x0010),
    PHY_EDIT(PR_EEE_ADV2, EEEADV2_2G5 | EEEADV2_5G, EEEADV2_2G5),
    PHY_PUT(PR_EEE_ADV, EEEADV_1000T | EEEADV_100TX),
    /* The 8157 and 8159: 2.5G and 5G, and on the 8159 10G */
    ONLY(K_57, U_ANY, 2),
    PHY_SET(PR_EEE_CFG, 0x0010),
    PHY_SET(PR_EEE_ADV2, EEEADV2_2G5 | EEEADV2_5G),
    ONLY(K_1030, U_ANY, 1), PHY_PUT(PR_EEE_ADV, EEEADV_1000T | EEEADV_100TX),
    ONLY(K_2020, U_ANY, 1), PHY_PUT(PR_EEE_ADV, EEEADV_10GT | EEEADV_1000T | EEEADV_100TX),
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

/* ==== The 8156 core (RTL8156, RTL8156B, RTL8153D) and its successors ====
 * Lines guarded by K_57 are the steps of the shared base setup, wake and
 * sleep that differ on the RTL8157/8159; their PHY setup, descriptor
 * format and advertisement belong to the 8157/8159 work. */

/* ---- base setup, once per bring-up ---- */

static const STEP SqSetupMG[] = {
    ONLY(K_57, U_ANY, 2), SET(USB, 1, 0xcffe, 0x08), CLR(USB, 1, 0xd3ca, 0x01),
    CLR(USB, 1, UR_ECM_OP, 0x01),                   /* the ECM side's all-speed mode off */
    ONLY(K_56, U_ANY, 1), PUT(USB, 2, UR_SPEED_OPT, 0),
    SET(USB, 2, UR_ECM_OPTION, 0x0020),             /* the MAC is not reset with ECM */
    ONLY(K_56B, U_ANY, 1), SET(USB, 2, UR_U2P3, 0x0008),
    CLR(USB, 2, UR_LPM_CFG, 0x0001),                /* U1/U2 off */
    ONLY(K_7410 | K_7420 | K_57, U_ANY, 1), HOOK(H_FLASH_WAIT),
    RUN(Q_AUTOLOAD),
    ONLY(K_2020, U_ANY, 1), HOOK(H_RESTORE_WAIT),
    HOOK(H_EXT_INIT),
    PHY_TRIM(MII(MII_CONTROL), MIICTL_POWERDOWN),
    PHY_STATE(ST_LAN_ON),
    ONLY(K_57, U_ANY, 1), ADV_EDIT(ADV_IP, 0xc2c0, 0x20000000UL, 0),   /* U2P3 off */
    ONLY(K_56, U_ANY, 1), CLR(USB, 2, UR_U2P3, 0x0001),
    PUT(USB, 2, UR_MSC_TMR, 0x0fff),
    PUT(USB, 2, UR_U1U2_TMR, 500),                  /* idle time before U1/U2/L1, us */
    /* power cut off */
    CLR(USB, 2, UR_POWER_CUT, 0x0001),
    CLR(USB, 2, UR_MISC0, 0x0001),
    ONLY(K_57, U_ANY, 1), CLR(USB, 1, UR_MISC2, 0x02),
    /* runtime power saving (UPS) off */
    CLR(USB, 1, UR_POWER_CUT, 0x30),
    CLR(USB, 1, UR_MISC2, 0x01),
    HOOK(H_UPS_CHECK_MG),
    RUN(Q_QUEUE_WAKE_OFF),
    RUN(Q_NO_WAKE),
    ONLY(K_ANY, U_SUPER, 1), SET(USB, 2, UR_LPM_CFG, 0x0001),
    /* 8156B and 8153D: flow control on, its timer 600 ms */
    ONLY(K_56B, U_ANY, 5),
    CLR(MAC, 2, MR_RX_ACCEPT, 0x0800),
    SET(MAC, 2, MR_CPLUS, 0x0001),
    PUT(USB, 2, UR_FC_TIMER, 0x8000 | (600 / 8)),
    HOOK(H_FW_CTRL),
    SET(USB, 2, UR_FW_TASK, 0x0002),
    /* MAC clock slow-down ratios */
    PUT(MAC, 2, MR_POWER1, 0x0403),
    EDIT(MAC, 2, MR_POWER2, 0x00ff, 0x8003),
    ONLY(K_56, U_ANY, 1), CLR(MAC, 2, MR_POWER3, 0x4000),
    HOOK(H_LINK_FLAGS),
    /* receive aggregation on; the descriptor size bit off */
    ONLY(K_2020, U_ANY, 1), CLR(USB, 2, UR_CTRL, CTRL_RX_AGG_OFF | CTRL_RX_DESC16 | 0x0800),
    ONLY(K_1030, U_ANY, 1), CLR(USB, 2, UR_CTRL, CTRL_RX_AGG_OFF | CTRL_RX_DESC16),
    ONLY(K_56, U_ANY, 1), CLR(USB, 2, UR_CTRL, CTRL_RX_AGG_OFF | CTRL_RX_ZERO),
    ONLY(K_56A, U_ANY, 1), SET(USB, 1, UR_BMU_CFG, 0x02),
    ONLY(K_57, U_ANY, 2), ADV_EDIT(ADV_BMU, 0x2300, 0x00000008UL, 0), CLR(USB, 1, 0xd4ae, 0x02),
    PHY_TRIM(PR_FORCE_SEL, 0x8000),
    SET(MAC, 2, MR_TALLY, 0x0001),
    END
};

/* ---- PHY setup ---- */

/* The RTL8156 (7020, 7030). */
static const STEP SqPhy56A[] = {
    TRIM(USB, 2, UR_MISC0, 0x0001),
    HOOK(H_EXT_INIT_PHY),
    RUN(Q_ALDPS_OFF),
    RUN(Q_EEE_OFF),
    PHY_STATE(ST_LAN_ON),
    SET(MAC, 2, MR_PHY_POWER, 0x0040),
    ONLY(K_7020, U_ANY, 1), RUN(Q_PHY_7020),
    ONLY(K_7030, U_ANY, 1), RUN(Q_PHY_7030),
    RUN(Q_PHY_MG_TAIL),
    SET(USB, 2, UR_U2P3, 0x0001),
    END
};

static const STEP SqPhy7020[] = {
    PHY_EDIT(0xad40, 0x03ff, 0x0084),
    PHY_SET(0xad4e, 0x0010),
    PHY_EDIT(0xad16, 0x03ff, 0x0006),
    PHY_EDIT(0xad32, 0x003f, 0x0006),
    PHY_CLR(0xac08, 0x1100),
    PHY_EDIT(0xac8a, 0x8000, 0x7000),
    PHY_SET(0xad18, 0x0400),
    PHY_SET(0xad1a, 0x03ff),
    PHY_SET(0xad1c, 0x03ff),
    SRAM_EDIT(0x80ea, 0xff00, 0xc400),
    SRAM_EDIT(0x80eb, 0x0700, 0x0300),
    SRAM_EDIT(0x80f8, 0xff00, 0x1c00),
    SRAM_EDIT(0x80f1, 0xff00, 0x3000),
    SRAM_EDIT(0x80fe, 0xff00, 0xa500),
    SRAM_EDIT(0x8102, 0xff00, 0x5000),
    SRAM_EDIT(0x8015, 0xff00, 0x3300),
    SRAM_EDIT(0x8100, 0xff00, 0x7000),
    SRAM_EDIT(0x8014, 0xff00, 0xf000),
    SRAM_EDIT(0x8016, 0xff00, 0x6500),
    SRAM_EDIT(0x80dc, 0xff00, 0xed00),
    SRAM_EDIT(0x80df, 0, 0x0100),
    SRAM_EDIT(0x80e1, 0x0100, 0),
    PHY_EDIT(0xbf06, 0x003f, 0x0038),
    SRAM_PUT(0x819f, 0xddb6),
    PHY_PUT(0xbc34, 0x5555),
    PHY_EDIT(0xbf0a, 0x0e00, 0x0a00),
    PHY_CLR(0xbd2c, 0x2000),
    END
};

static const STEP SqPhy7030[] = {
    PHY_SET(0xad16, 0x03ff),
    PHY_EDIT(0xad32, 0x003f, 0x0006),
    PHY_CLR(0xac08, 0x1100),
    PHY_EDIT(0xacc0, 0x0003, 0x0002),
    PHY_EDIT(0xad40, 0x00e7, 0x0044),
    PHY_CLR(0xac14, 0x0080),
    PHY_CLR(0xac80, 0x0300),
    PHY_EDIT(0xac5e, 0x0007, 0x0002),
    PHY_PUT(0xad4c, 0x00a8),
    PHY_PUT(0xac5c, 0x01ff),
    PHY_EDIT(0xac8a, 0x00f0, 0x0030),
    SRAM2_EDIT(0x8157, 0xff00, 0x0500),
    SRAM2_EDIT(0x8159, 0xff00, 0x0700),
    SRAM2_PUT(0x80a2, 0x0153),                      /* receive gain control */
    SRAM2_PUT(0x809c, 0x0153),
    PUT(MAC, 2, MR_EEE_TW_2G5, 0x0056),
    SET(MAC, 2, MR_USB_CFG, 0x0006),
    SRAM_PUT(0x8257, 0x020f),
    SRAM_PUT(0x80ea, 0x7843),
    PATCH_OPEN,
    SET(MAC, 2, MR_POWER4, 0x0001),
    PHY_EDIT(PR_SPEED_DOWN, 0x3000, 0x0800),
    PHY_CLR(PR_POWER, 0x8000),
    PHY_PUT(PR_SYSCLK, 0),
    PHY_PUT(PR_SYSCLK, 5 << 8),
    PATCH_CLOSE,
    PHY_SET(0xd068, 0x2000),
    SRAM_EDIT(0x81a2, 0x0100, 0),
    PHY_EDIT(0xb54c, 0xff00, 0xdb00),
    PHY_CLR(0xa454, 0x0001),
    PHY_SET(PR_MGIG_CTRL, MGIG_FAST_RETRAIN),
    PHY_CLR(0xad4e, 0x0010),
    PHY_CLR(0xa86a, 0x0001),
    HOOK(H_MDI_SWAP),
    PHY_SET(PR_INTR, 0x0008),                       /* a forced speed change interrupts */
    END
};

/* The RTL8156B (7400, 7410) and RTL8153D (7420). */
static const STEP SqPhy56B[] = {
    ONLY(K_7400, U_ANY, 1), RUN(Q_PHY_7400_PRE),
    ONLY(K_7410 | K_7420, U_ANY, 1), HOOK(H_FLASH_WAIT),
    TRIM(USB, 2, UR_MISC0, 0x0001),
    HOOK(H_EXT_INIT_PHY),
    PHY_TRIM(MII(MII_CONTROL), MIICTL_POWERDOWN),
    RUN(Q_ALDPS_OFF),
    RUN(Q_EEE_OFF),
    PHY_STATE(ST_LAN_ON),
    SET(MAC, 2, MR_PHY_POWER, 0x0040),
    ONLY(K_7400, U_ANY, 1), RUN(Q_PHY_7400),
    ONLY(K_7410, U_ANY, 1), RUN(Q_PHY_7410),
    ONLY(K_7410 | K_7420, U_ANY, 1), RUN(Q_PHY_7420),
    PHY_SET(PR_INTR, 0x0008),
    PATCH_OPEN,
    SET(MAC, 2, MR_POWER4, 0x0001),
    PHY_EDIT(PR_SPEED_DOWN, 0x3000, 0x0800),
    PHY_CLR(PR_POWER, 0x8000),
    PATCH_CLOSE,
    RUN(Q_PHY_MG_TAIL),
    SET(USB, 2, UR_U2P3, 0x0001),
    END
};

static const STEP SqPhy7400Pre[] = {
    PHY_PUT(0xbf86, 0x9000),
    PHY_SET(0xc402, 0x0400),
    PHY_CLR(0xc402, 0x0400),
    PHY_PUT(0xbd86, 0x1010),
    PHY_PUT(0xbd88, 0x1010),
    PHY_EDIT(0xbd4e, 0x0c00, 0x0800),
    PHY_EDIT(0xbf46, 0x0f00, 0x0700),
    END
};

static const STEP SqPhy7400[] = {
    PHY_SET(0xbc08, 0x000c),
    SRAM_EDIT(0x8fff, 0xff00, 0x0400),
    PHY_SET(0xacda, 0xff00),
    PHY_SET(0xacde, 0xf000),
    PHY_PUT(0xac8c, 0x0ffc), PHY_PUT(0xac46, 0xb7b4), PHY_PUT(0xac50, 0x0fbc),
    PHY_PUT(0xac3c, 0x9240), PHY_PUT(0xac4e, 0x0db4), PHY_PUT(0xacc6, 0x0707),
    PHY_PUT(0xacc8, 0xa0d3), PHY_PUT(0xad08, 0x0007),
    SRAM2_PUT(0x8560, 0x19cc), SRAM2_PUT(0x8562, 0x19cc), SRAM2_PUT(0x8564, 0x19cc),
    SRAM2_PUT(0x8566, 0x147d), SRAM2_PUT(0x8568, 0x147d), SRAM2_PUT(0x856a, 0x147d),
    SRAM2_PUT(0x8ffe, 0x0907), SRAM2_PUT(0x80d6, 0x2801), SRAM2_PUT(0x80f2, 0x2801),
    SRAM2_PUT(0x80f4, 0x6077),
    PHY_PUT(0xb506, 0x01e7),
    SRAM2_PUT(0x8013, 0x0700), SRAM2_PUT(0x8fb9, 0x2801), SRAM2_PUT(0x8fba, 0x0100),
    SRAM2_PUT(0x8fbc, 0x1900), SRAM2_PUT(0x8fbe, 0xe100), SRAM2_PUT(0x8fc0, 0x0800),
    SRAM2_PUT(0x8fc2, 0xe500), SRAM2_PUT(0x8fc4, 0x0f00), SRAM2_PUT(0x8fc6, 0xf100),
    SRAM2_PUT(0x8fc8, 0x0400), SRAM2_PUT(0x8fca, 0xf300), SRAM2_PUT(0x8fcc, 0xfd00),
    SRAM2_PUT(0x8fce, 0xff00), SRAM2_PUT(0x8fd0, 0xfb00), SRAM2_PUT(0x8fd2, 0x0100),
    SRAM2_PUT(0x8fd4, 0xf400), SRAM2_PUT(0x8fd6, 0xff00), SRAM2_PUT(0x8fd8, 0xf600),
    SET(MAC, 1, MR_USB_CFG, 0x06),
    SRAM2_PUT(0x813d, 0x390e), SRAM2_PUT(0x814f, 0x790e), SRAM2_PUT(0x80b0, 0x0f31),
    PHY_SET(0xbf4c, 0x0002),
    PHY_SET(0xbcca, 0x0300),
    SRAM2_PUT(0x8141, 0x320e), SRAM2_PUT(0x8153, 0x720e), SRAM2_PUT(0x8529, 0x050e),
    PHY_CLR(PR_EEE_CFG, 0x0040),
    SRAM_PUT(0x816c, 0xc4a0), SRAM_PUT(0x8170, 0xc4a0), SRAM_PUT(0x8174, 0x04a0),
    SRAM_PUT(0x8178, 0x04a0), SRAM_PUT(0x817c, 0x0719), SRAM_PUT(0x8ff4, 0x0400),
    SRAM_PUT(0x8ff1, 0x0404),
    PHY_PUT(0xbf4a, 0x001b),
    SRAM2_PUT(0x8033, 0x7c13), SRAM2_PUT(0x8037, 0x7c13), SRAM2_PUT(0x803b, 0xfc32),
    SRAM2_PUT(0x803f, 0x7c13), SRAM2_PUT(0x8043, 0x7c13), SRAM2_PUT(0x8047, 0x7c13),
    SRAM2_PUT(0x8145, 0x370e), SRAM2_PUT(0x8157, 0x770e), SRAM2_PUT(0x8169, 0x0d0a),
    SRAM2_PUT(0x817b, 0x1d0a),
    SRAM_EDIT(0x8217, 0xff00, 0x5000),
    SRAM_EDIT(0x821a, 0xff00, 0x5000),
    SRAM_PUT(0x80da, 0x0403),
    SRAM_EDIT(0x80dc, 0xff00, 0x1000),
    SRAM_PUT(0x80b3, 0x0384),
    SRAM_PUT(0x80b7, 0x2007),
    SRAM_EDIT(0x80ba, 0xff00, 0x6c00),
    SRAM_PUT(0x80b5, 0xf009),
    SRAM_EDIT(0x80bd, 0xff00, 0x9f00),
    SRAM_PUT(0x80c7, 0xf083),
    SRAM_PUT(0x80dd, 0x03f0),
    SRAM_EDIT(0x80df, 0xff00, 0x1000),
    SRAM_PUT(0x80cb, 0x2007),
    SRAM_EDIT(0x80ce, 0xff00, 0x6c00),
    SRAM_PUT(0x80c9, 0x8009),
    SRAM_EDIT(0x80d1, 0xff00, 0x8000),
    SRAM_PUT(0x80a3, 0x200a),
    SRAM_PUT(0x80a5, 0xf0ad),
    SRAM_PUT(0x809f, 0x6073),
    SRAM_PUT(0x80a1, 0x000b),
    SRAM_EDIT(0x80a9, 0xff00, 0xc000),
    PATCH_OPEN,
    PHY_CLR(0xb896, 0x0001),
    PHY_CLR(0xb892, 0xff00),
    PHY_PUT(0xb88e, 0xc23e), PHY_PUT(0xb890, 0x0000),
    PHY_PUT(0xb88e, 0xc240), PHY_PUT(0xb890, 0x0103),
    PHY_PUT(0xb88e, 0xc242), PHY_PUT(0xb890, 0x0507),
    PHY_PUT(0xb88e, 0xc244), PHY_PUT(0xb890, 0x090b),
    PHY_PUT(0xb88e, 0xc246), PHY_PUT(0xb890, 0x0c0e),
    PHY_PUT(0xb88e, 0xc248), PHY_PUT(0xb890, 0x1012),
    PHY_PUT(0xb88e, 0xc24a), PHY_PUT(0xb890, 0x1416),
    PHY_SET(0xb896, 0x0001),
    PATCH_CLOSE,
    PHY_SET(0xa86a, 0x0001),
    PHY_SET(0xa6f0, 0x0001),
    PHY_PUT(0xbfa0, 0xd70d), PHY_PUT(0xbfa2, 0x4100), PHY_PUT(0xbfa4, 0xe868),
    PHY_PUT(0xbfa6, 0xdc59), PHY_PUT(0xb54c, 0x3c18),
    PHY_CLR(0xbfa4, 0x0020),
    SRAM_SET(0x817d, 0x1000),
    END
};

static const STEP SqPhy7410[] = {                      /* 2.5G receive */
    PHY_EDIT(0xac46, 0x00f0, 0x0090),
    PHY_EDIT(0xad30, 0x0003, 0x0001),
    END
};

static const STEP SqPhy7420[] = {                      /* 7410 and 7420 */
    SRAM2_PUT(0x80f5, 0x760e),                      /* EEE */
    SRAM2_PUT(0x8107, 0x360e),
    SRAM2_EDIT(0x8551, 0xff00, 0x0800),
    PHY_EDIT(0xbf00, 0xe000, 0xa000),               /* ADC gain */
    PHY_EDIT(0xbf46, 0x0f00, 0x0300),
    SRAM_PUT(0x8044, 0x2417), SRAM_PUT(0x804a, 0x2417), SRAM_PUT(0x8050, 0x2417),
    SRAM_PUT(0x8056, 0x2417), SRAM_PUT(0x805c, 0x2417), SRAM_PUT(0x8062, 0x2417),
    SRAM_PUT(0x8068, 0x2417), SRAM_PUT(0x806e, 0x2417), SRAM_PUT(0x8074, 0x2417),
    SRAM_PUT(0x807a, 0x2417),
    PHY_EDIT(0xbf84, 0xe000, 0xa000),               /* PLL */
    END
};

/* The end of every 8156-core PHY setup: green Ethernet on, reduced
 * ("lite") modes off, EEE (not on the 7020), ALDPS, pause advertised. */
static const STEP SqPhyMGTail[] = {
    SRAM_SET(SR_GREEN, 0x8000),
    PHY_CLR(PR_LITE_A, 0x0200),
    ONLY(K_56, U_ANY, 1), PHY_CLR(PR_LITE_B, 0x0001),
    ONLY(K_57, U_ANY, 1), PHY_CLR(PR_LITE_B, 0x0003),
    ONLY(K_7030 | K_56B | K_57, U_ANY, 1), RUN(Q_EEE_ON),
    RUN(Q_ALDPS_ON),
    PHY_SET(MII(MII_AN_ADV), ANADV_PAUSE),
    END
};

/* ---- receiver and transmitter off, the MAC reset ---- */

static const STEP SqNicResetMG[] = {
    ONLY(K_56A, U_ANY, 6),
    CLR(MAC, 1, MR_COMMAND, CMD_TX_ON),
    CLR(USB, 2, UR_BMU, 0x0001),
    SET(USB, 2, UR_CTRL, CTRL_ECM),
    CLR(MAC, 1, MR_COMMAND, CMD_RX_ON),
    SET(USB, 2, UR_BMU, 0x0001),
    CLR(USB, 2, UR_CTRL, CTRL_ECM),
    ONLY(K_56B, U_ANY, 1), RUN(Q_RESET_MAC),
    ONLY(K_57, U_ANY, 1), CLR(MAC, 1, MR_COMMAND, CMD_RX_ON | CMD_TX_ON),
    END
};

static const STEP SqDrainMG[] = {
    CLR(MAC, 4, MR_RX_ACCEPT, ACC_MASK),
    RUN(Q_RX_GATE),
    WAIT(MAC, 1, MR_OOB, OOB_FIFOS_EMPTY, OOB_FIFOS_EMPTY, 1000, 1),
    WAIT(MAC, 2, MR_TX_CONFIG, TXC_EMPTY, TXC_EMPTY, 1000, 1),
    RUN(Q_NIC_RESET_MG),
    END
};

/* ---- out of OOB: receiver ready, not yet on ---- */

static const STEP SqWakeMG[] = {
    CLR(USB, 2, UR_LPM_CFG, 0x0001),
    ONLY(K_56, U_ANY, 1), CLR(USB, 2, UR_U2P3, 0x0001),
    RUN(Q_ALDPS_OFF),
    RUN(Q_RX_GATE),
    RUN(Q_TEREDO_OFF),
    CLR(MAC, 4, MR_RX_ACCEPT, ACC_MASK),
    RUN(Q_NIC_RESET_MG),
    RUN(Q_RESET_BMU),
    CLR(MAC, 1, MR_OOB, OOB_ACTIVE),
    ONLY(K_57, U_ANY, 1), CLR(MAC, 2, MR_RX_CONFIG1, 0x0008),
    CLR(MAC, 2, MR_SFF7, 0x4000),
    CLR(MAC, 2, MR_RX_CONFIG1, 0x00c0),             /* VLAN tags stay in the frame */
    /* frame sizes and FIFO thresholds for a 1500-byte MTU */
    PUT(MAC, 2, MR_RX_MAX_LEN, RX_MAX_LEN),
    PUT(MAC, 1, MR_TX_MAX_PKT, 12 * 1024 / 64),
    PUT(MAC, 2, MR_RX_FIFO_FULL, (2048 + 6 * 1024) / 16),
    PUT(MAC, 2, MR_RX_FIFO_EMPTY, (2048 + 14 * 1024) / 16),
    PUT(MAC, 2, MR_TXFIFO_TH, 512 / 64),
    PUT(MAC, 2, MR_TXFIFO_FULL, 2048 / 16),
    ONLY(K_56A, U_ANY, 1), SET(USB, 2, UR_BMU_CFG, 0x0002),
    EDIT(MAC, 2, MR_RXFIFO_TH1_HI, 0x0fff, 0x0008),
    CLR(MAC, 2, MR_POWER3, 0x4000),
    ONLY(K_56, U_ANY, 1), CLR(USB, 2, UR_SPEED_OPT, 0x0300),
    PUT(USB, 4, UR_RX_BUF_TH, 0x00600400UL),
    RUN(Q_ALDPS_ON),
    ONLY(K_56, U_ANY, 1), SET(USB, 2, UR_U2P3, 0x0001),
    ONLY(K_56, U_SUPER, 1), SET(USB, 2, UR_LPM_CFG, 0x0001),
    END
};

/* ---- link up and down ---- */

static const STEP SqRxOnMG[] = {
    ONLY(K_56A, U_ANY, 2),
    PUT(MAC, 2, MR_RX_FIFO_FULL, (2048 + 6 * 1024) / 16),
    PUT(MAC, 2, MR_RX_FIFO_EMPTY, (2048 + 14 * 1024) / 16),
    HOOK(H_EEE_PLUS),
    ONLY(K_56B | K_57, U_ANY, 1), CLR(USB, 2, UR_RX_AGG_NUM, 0x01ff),
    /* Early receive: 640 ns, the extra aggregation timer 15 us. */
    PUT(USB, 2, UR_RX_EARLY_TMO, 640 / 8),
    PUT(USB, 2, UR_PM_STATUS, 15000UL / 8),
    HOOK(H_RX_EARLY),
    HOOK(H_GAP),
    ONLY(K_56, U_ANY, 1), HOOK(H_IDLE_2G5),
    ONLY(K_56A, U_ANY, 1), HOOK(H_EEE_TW),
    ONLY(K_56, U_HIGH, 1), HOOK(H_L1_NYET),
    ONLY(K_56, U_ANY, 3),
    CLR(USB, 2, UR_FW_TASK, 0x0002), SLEEP(1), SET(USB, 2, UR_FW_TASK, 0x0002),
    CLR(MAC, 2, MR_FILTER_MCU, 0x0001),
    SET(MAC, 2, MR_FILTER_MCU, 0x0001),
    SET(MAC, 1, MR_COMMAND, CMD_RX_ON | CMD_TX_ON),
    PUT(USB, 1, UR_RXDMA_OWN, 0x03),
    RUN(Q_RX_UNGATE),
    END
};

static const STEP SqRxOffMG[] = {
    ONLY(K_56A, U_ANY, 2), PUT(MAC, 2, MR_RX_FIFO_FULL, 0), PUT(MAC, 2, MR_RX_FIFO_EMPTY, 0),
    RUN(Q_ALDPS_OFF),
    RUN(Q_DRAIN_MG),
    RUN(Q_RESET_BMU),
    RUN(Q_ALDPS_ON),
    END
};

/* ---- back into OOB (Stop, Suspend) ---- */

static const STEP SqSleepMG[] = {
    SET(MAC, 2, MR_POWER3, 0x4000),
    CLR(USB, 2, UR_LPM_CFG, 0x0001),
    ONLY(K_56, U_ANY, 3),
    CLR(USB, 2, UR_U2P3, 0x0001), CLR(USB, 2, UR_POWER_CUT, 0x0001), CLR(USB, 2, UR_MISC0, 0x0001),
    RUN(Q_ALDPS_OFF),
    CLR(MAC, 1, MR_OOB, OOB_ACTIVE),
    PUT(MAC, 2, MR_RXFIFO_TH1_HI, 64 / 16),
    PUT(MAC, 2, MR_RX_FIFO_FULL, 1024 / 16),
    PUT(MAC, 2, MR_RX_FIFO_EMPTY, 4096 / 16),
    RUN(Q_DRAIN_MG),
    RUN(Q_RESET_BMU),
    PUT(MAC, 2, MR_RX_MAX_LEN, RX_MAX_LEN),
    PUT(MAC, 1, MR_TX_MAX_PKT, 6 * 1024 / 64),
    PUT(MAC, 2, MR_TEREDO_WAKE, 0x00ff),
    SET(MAC, 1, MR_OOB, OOB_ACTIVE),
    SET(MAC, 2, MR_SFF7, 0x4000),
    SET(MAC, 2, MR_RX_CONFIG1, 0x00c0),
    RUN(Q_RX_UNGATE),
    SET(MAC, 4, MR_RX_ACCEPT, ACC_MY_PHYS | ACC_MCAST | ACC_BCAST),
    RUN(Q_ALDPS_ON),
    END
};

static const STEP SqDetachMG[] = {
    CLR(USB, 2, UR_POWER_CUT, 0x0009),
    CLR(USB, 2, UR_MISC0, 0x0001),
    ONLY(K_57, U_ANY, 1), CLR(USB, 1, 0xcf04, 0x87),   /* interrupt mitigation off */
    END
};

/* The RTL8157 and RTL8159 (1030, 2020): the chip's PHY tuning values, then
 * EEE clock division in a patch window, then the common end. */
static const STEP SqPhy57[] = {
    HOOK(H_FLASH_WAIT),
    TRIM(USB, 2, UR_MISC0, 0x0001),
    HOOK(H_EXT_INIT_PHY),
    PHY_TRIM(MII(MII_CONTROL), MIICTL_POWERDOWN),
    RUN(Q_ALDPS_OFF),
    RUN(Q_EEE_OFF),
    PHY_STATE(ST_LAN_ON),
    CLR(MAC, 2, MR_PHY_POWER, 0x0040),
    PHY_SET(PR_POWER, 0x0003),
    PHY_CLR(0xa44a, 0x0004),                        /* ALDPS not forced */
    ONLY(K_1030, U_ANY, 1), RUN(Q_PHY_1030),
    ONLY(K_2020, U_ANY, 1), RUN(Q_PHY_2020),
    PATCH_OPEN,
    SET(MAC, 2, MR_POWER4, 0x0001),
    PHY_EDIT(PR_SPEED_DOWN, 0x3000, 0x0800),
    PHY_SET(PR_POWER, 0x8000),
    PATCH_CLOSE,
    RUN(Q_PHY_MG_TAIL),
    END
};

/* PHY tuning values of the RTL8157 (1030): (address, mask, value) facts
 * of the chip, through the PHY window and its two SRAM windows. */
static const STEP SqPhy1030[] = {
    SRAM_EDIT(0x8183, 0xff00, 0x5900),
    PHY_SET(0xa654, 0x0800),
    PHY_SET(0xb648, 0x4000),
    PHY_CLR(0xad2c, 0x8000),
    PHY_SET(0xad94, 0x0020),
    PHY_SET(0xada0, 0x0002),
    PHY_EDIT(0xae06, 0xfc00, 0x7c00),
    SRAM2_EDIT(0x8647, 0xff00, 0xe600),
    SRAM2_EDIT(0x8036, 0xff00, 0x3000),
    SRAM2_EDIT(0x8078, 0xff00, 0x3000),
    SRAM2_EDIT(0x89e9, 0xff00, 0),
    SRAM2_EDIT(0x8ffd, 0xff00, 0x0100),
    SRAM2_EDIT(0x8ffe, 0xff00, 0x0200),
    SRAM2_EDIT(0x8fff, 0xff00, 0x0400),
    SRAM_EDIT(0x8018, 0xff00, 0x7700),
    PHY_PUT(PR_SRAM_ADDR, 0x8f9c),
    PHY_PUT(PR_SRAM_DATA, 0x0005),
    PHY_PUT(PR_SRAM_DATA, 0x0000),
    PHY_PUT(PR_SRAM_DATA, 0x00ed),
    PHY_PUT(PR_SRAM_DATA, 0x0502),
    PHY_PUT(PR_SRAM_DATA, 0x0b00),
    PHY_PUT(PR_SRAM_DATA, 0xd401),
    SRAM_EDIT(0x8fa8, 0xff00, 0x2900),
    SRAM2_EDIT(0x814b, 0xff00, 0x1100),
    SRAM2_EDIT(0x814d, 0xff00, 0x1100),
    SRAM2_EDIT(0x814f, 0xff00, 0x0b00),
    SRAM2_EDIT(0x8142, 0xff00, 0x0100),
    SRAM2_EDIT(0x8144, 0xff00, 0x0100),
    SRAM2_EDIT(0x8150, 0xff00, 0x0100),
    SRAM2_EDIT(0x8118, 0xff00, 0x0700),
    SRAM2_EDIT(0x811a, 0xff00, 0x0700),
    SRAM2_EDIT(0x811c, 0xff00, 0x0500),
    SRAM2_EDIT(0x810f, 0xff00, 0x0100),
    SRAM2_EDIT(0x8111, 0xff00, 0x0100),
    SRAM2_EDIT(0x811d, 0xff00, 0x0100),
    PHY_CLR(0xad1c, 0x0100),
    PHY_EDIT(0xade8, 0xffc0, 0x1400),
    SRAM2_EDIT(0x864b, 0xff00, 0x9d00),
    SRAM2_EDIT(0x862c, 0xff00, 0x1200),
    PHY_PUT(PR_SRAM_ADDR, 0x8566),
    PHY_PUT(PR_SRAM_DATA, 0x003f),
    PHY_PUT(PR_SRAM_DATA, 0x3f02),
    PHY_PUT(PR_SRAM_DATA, 0x023c),
    PHY_PUT(PR_SRAM_DATA, 0x3b0a),
    PHY_PUT(PR_SRAM_DATA, 0x1c00),
    PHY_PUT(PR_SRAM_DATA, 0x0000),
    PHY_PUT(PR_SRAM_DATA, 0x0000),
    PHY_PUT(PR_SRAM_DATA, 0x0000),
    PHY_PUT(PR_SRAM_DATA, 0x0000),
    PHY_SET(0xad9c, 0x0020),
    SRAM2_EDIT(0x8122, 0xff00, 0x0c00),
    PHY_PUT(PR_SRAM2_ADDR, 0x82c8),
    PHY_PUT(PR_SRAM2_DATA, 0x03ed),
    PHY_PUT(PR_SRAM2_DATA, 0x03ff),
    PHY_PUT(PR_SRAM2_DATA, 0x0009),
    PHY_PUT(PR_SRAM2_DATA, 0x03fe),
    PHY_PUT(PR_SRAM2_DATA, 0x000b),
    PHY_PUT(PR_SRAM2_DATA, 0x0021),
    PHY_PUT(PR_SRAM2_DATA, 0x03f7),
    PHY_PUT(PR_SRAM2_DATA, 0x03b8),
    PHY_PUT(PR_SRAM2_DATA, 0x03e0),
    PHY_PUT(PR_SRAM2_DATA, 0x0049),
    PHY_PUT(PR_SRAM2_DATA, 0x0049),
    PHY_PUT(PR_SRAM2_DATA, 0x03e0),
    PHY_PUT(PR_SRAM2_DATA, 0x03b8),
    PHY_PUT(PR_SRAM2_DATA, 0x03f7),
    PHY_PUT(PR_SRAM2_DATA, 0x0021),
    PHY_PUT(PR_SRAM2_DATA, 0x000b),
    PHY_PUT(PR_SRAM2_DATA, 0x03fe),
    PHY_PUT(PR_SRAM2_DATA, 0x0009),
    PHY_PUT(PR_SRAM2_DATA, 0x03ff),
    PHY_PUT(PR_SRAM2_DATA, 0x03ed),
    SRAM2_EDIT(0x80ef, 0xff00, 0x0c00),
    PHY_PUT(PR_SRAM2_ADDR, 0x82a0),
    PHY_PUT(PR_SRAM2_DATA, 0x000e),
    PHY_PUT(PR_SRAM2_DATA, 0x03fe),
    PHY_PUT(PR_SRAM2_DATA, 0x03ed),
    PHY_PUT(PR_SRAM2_DATA, 0x0006),
    PHY_PUT(PR_SRAM2_DATA, 0x001a),
    PHY_PUT(PR_SRAM2_DATA, 0x03f1),
    PHY_PUT(PR_SRAM2_DATA, 0x03d8),
    PHY_PUT(PR_SRAM2_DATA, 0x0023),
    PHY_PUT(PR_SRAM2_DATA, 0x0054),
    PHY_PUT(PR_SRAM2_DATA, 0x0322),
    PHY_PUT(PR_SRAM2_DATA, 0x00dd),
    PHY_PUT(PR_SRAM2_DATA, 0x03ab),
    PHY_PUT(PR_SRAM2_DATA, 0x03dc),
    PHY_PUT(PR_SRAM2_DATA, 0x0027),
    PHY_PUT(PR_SRAM2_DATA, 0x000e),
    PHY_PUT(PR_SRAM2_DATA, 0x03e5),
    PHY_PUT(PR_SRAM2_DATA, 0x03f9),
    PHY_PUT(PR_SRAM2_DATA, 0x0012),
    PHY_PUT(PR_SRAM2_DATA, 0x0001),
    PHY_PUT(PR_SRAM2_DATA, 0x03f1),
    PHY_EDIT(0xb54c, 0xffc0, 0x3700),
    PHY_SET(0xb648, 0x0040),
    SRAM2_EDIT(0x8082, 0xff00, 0x5d00),
    SRAM2_EDIT(0x807c, 0xff00, 0x5000),
    SRAM2_EDIT(0x809d, 0xff00, 0x5000),
    END
};

/* PHY tuning values of the RTL8159 (2020). */
static const STEP SqPhy2020[] = {
    CLR(MAC, 1, 0xd3c8, 0x0001),
    SRAM_EDIT(0x8415, 0xff00, 0x9300),
    SRAM_EDIT(0x81a3, 0xff00, 0x0f00),
    SRAM_EDIT(0x81ae, 0xff00, 0x0f00),
    SRAM_EDIT(0x81b9, 0xff00, 0xb900),
    SRAM2_EDIT(0x83b0, 0x0e00, 0),
    SRAM2_EDIT(0x83c5, 0x0e00, 0),
    SRAM2_EDIT(0x83da, 0x0e00, 0),
    SRAM2_EDIT(0x83ef, 0x0e00, 0),
    SRAM_PUT(0x8173, 0x8620),
    SRAM_PUT(0x8175, 0x8671),
    SRAM_EDIT(0x817c, 0, 0x2000),
    SRAM_EDIT(0x8187, 0, 0x2000),
    SRAM_EDIT(0x8192, 0, 0x2000),
    SRAM_EDIT(0x819d, 0, 0x2000),
    SRAM_EDIT(0x81a8, 0x2000, 0),
    SRAM_EDIT(0x81b3, 0x2000, 0),
    SRAM_EDIT(0x81be, 0, 0x2000),
    SRAM_EDIT(0x817d, 0xff00, 0xa600),
    SRAM_EDIT(0x8188, 0xff00, 0xa600),
    SRAM_EDIT(0x8193, 0xff00, 0xa600),
    SRAM_EDIT(0x819e, 0xff00, 0xa600),
    SRAM_EDIT(0x81a9, 0xff00, 0x1400),
    SRAM_EDIT(0x81b4, 0xff00, 0x1400),
    SRAM_EDIT(0x81bf, 0xff00, 0xa600),
    PHY_CLR(0xaeaa, 0x0028),
    SRAM2_PUT(0x84f0, 0x201c),
    SRAM2_PUT(0x84f2, 0x3117),
    PHY_PUT(0xaec6, 0x0000),
    PHY_PUT(0xae20, 0xffff),
    PHY_PUT(0xaece, 0xffff),
    PHY_PUT(0xaed2, 0xffff),
    PHY_PUT(0xaec8, 0x0000),
    PHY_CLR(0xaed0, 0x0001),
    PHY_PUT(0xadb8, 0x0150),
    SRAM2_EDIT(0x8197, 0xff00, 0x5000),
    SRAM2_EDIT(0x8231, 0xff00, 0x5000),
    SRAM2_EDIT(0x82cb, 0xff00, 0x5000),
    SRAM2_EDIT(0x82cd, 0xff00, 0x5700),
    SRAM2_EDIT(0x8233, 0xff00, 0x5700),
    SRAM2_EDIT(0x8199, 0xff00, 0x5700),
    SRAM2_PUT(0x815a, 0x0150),
    SRAM2_PUT(0x81f4, 0x0150),
    SRAM2_PUT(0x828e, 0x0150),
    SRAM2_PUT(0x81b1, 0x0000),
    SRAM2_PUT(0x824b, 0x0000),
    SRAM2_PUT(0x82e5, 0x0000),
    SRAM2_EDIT(0x84f7, 0xff00, 0x2800),
    PHY_SET(0xaec2, 0x1000),
    SRAM2_EDIT(0x81b3, 0xff00, 0xad00),
    SRAM2_EDIT(0x824d, 0xff00, 0xad00),
    SRAM2_EDIT(0x82e7, 0xff00, 0xad00),
    PHY_EDIT(0xae4e, 0x000f, 0x0001),
    SRAM2_EDIT(0x82ce, 0xf000, 0x4000),
    SRAM2_EDIT(0x83a5, 0xff00, 0x0400),
    SRAM2_EDIT(0x83a6, 0xff00, 0x0400),
    SRAM2_EDIT(0x83a7, 0xff00, 0x0400),
    SRAM2_EDIT(0x83a8, 0xff00, 0x0400),
    SRAM2_PUT(0x84ac, 0x0000),
    SRAM2_PUT(0x84ae, 0x0000),
    SRAM2_PUT(0x84b0, 0xf818),
    SRAM2_EDIT(0x84b2, 0xff00, 0x6000),
    SRAM2_PUT(0x8ffc, 0x6008),
    SRAM2_PUT(0x8ffe, 0xf450),
    SRAM2_EDIT(0x8015, 0, 0x0200),
    SRAM2_EDIT(0x8016, 0, 0x0800),
    SRAM2_EDIT(0x8fe6, 0xff00, 0x0800),
    SRAM2_PUT(0x8fe4, 0x2114),
    SRAM2_PUT(0x8647, 0xa7b1),
    SRAM2_PUT(0x8649, 0xbbca),
    SRAM2_EDIT(0x864b, 0xff00, 0xdc00),
    SRAM2_EDIT(0x8154, 0xc000, 0x4000),
    SRAM2_EDIT(0x8158, 0xc000, 0),
    SRAM2_PUT(0x826c, 0xffff),
    SRAM2_PUT(0x826e, 0xffff),
    SRAM2_EDIT(0x8872, 0xff00, 0x0e00),
    SRAM_EDIT(0x8012, 0, 0x0800),
    SRAM_EDIT(0x8012, 0, 0x4000),
    PHY_SET(0xb576, 0x0001),
    SRAM_EDIT(0x834a, 0xff00, 0x0700),
    SRAM2_EDIT(0x8217, 0x3f00, 0x2a00),
    SRAM_EDIT(0x81b1, 0xff00, 0x0b00),
    SRAM2_EDIT(0x8fed, 0xff00, 0x4e00),
    SRAM2_EDIT(0x88ac, 0xff00, 0x2300),
    PHY_SET(0xbf0c, 0x3800),
    SRAM2_EDIT(0x88de, 0xff00, 0),
    SRAM2_PUT(0x80b4, 0x5195),
    SRAM_PUT(0x8370, 0x8671),
    SRAM_PUT(0x8372, 0x86c8),
    SRAM_PUT(0x8401, 0x86c8),
    SRAM_PUT(0x8403, 0x86da),
    SRAM_EDIT(0x8406, 0x1800, 0x1000),
    SRAM_EDIT(0x8408, 0x1800, 0x1000),
    SRAM_EDIT(0x840a, 0x1800, 0x1000),
    SRAM_EDIT(0x840c, 0x1800, 0x1000),
    SRAM_EDIT(0x840e, 0x1800, 0x1000),
    SRAM_EDIT(0x8410, 0x1800, 0x1000),
    SRAM_EDIT(0x8412, 0x1800, 0x1000),
    SRAM_EDIT(0x8414, 0x1800, 0x1000),
    SRAM_EDIT(0x8416, 0x1800, 0x1000),
    SRAM_PUT(0x82bd, 0x1f40),
    PHY_EDIT(0xbfb4, 0x07ff, 0x0328),
    PHY_PUT(0xbfb6, 0x3e14),
    PHY_PUT(PR_SRAM_ADDR, 0x81c4),
    PHY_PUT(PR_SRAM_DATA, 0x003b),
    PHY_PUT(PR_SRAM_DATA, 0x0086),
    PHY_PUT(PR_SRAM_DATA, 0x00b7),
    PHY_PUT(PR_SRAM_DATA, 0x00db),
    PHY_PUT(PR_SRAM_DATA, 0x00fe),
    PHY_PUT(PR_SRAM_DATA, 0x00fe),
    PHY_PUT(PR_SRAM_DATA, 0x00fe),
    PHY_PUT(PR_SRAM_DATA, 0x00fe),
    PHY_PUT(PR_SRAM_DATA, 0x00c3),
    PHY_PUT(PR_SRAM_DATA, 0x0078),
    PHY_PUT(PR_SRAM_DATA, 0x0047),
    PHY_PUT(PR_SRAM_DATA, 0x0023),
    SRAM2_PUT(0x88d7, 0x01a0),
    SRAM2_PUT(0x88d9, 0x01a0),
    SRAM2_PUT(0x8ffa, 0x002a),
    SRAM2_PUT(0x8fee, 0xffdf),
    SRAM2_PUT(0x8ff0, 0xffff),
    SRAM2_PUT(0x8ff2, 0x0a4a),
    SRAM2_PUT(0x8ff4, 0xaa5a),
    SRAM2_PUT(0x8ff6, 0x0a4a),
    SRAM2_PUT(0x8ff8, 0xaa5a),
    SRAM2_EDIT(0x88d5, 0xff00, 0x0200),
    END
};

static const STEP *const Seq[] = {
    SqRxGate, SqRxUngate, SqTeredoOff, SqResetMac, SqResetBmu, SqRebuildList,
    SqLeaveOob, SqQuiesce, SqPowerCutOff, SqU1U2Off, SqU1U2On, SqAldpsOff, SqAldpsOn,
    SqEeeOff, SqEeeOn, SqNoWake, SqCfg34Bit8, SqAutoload, SqDrain, SqQueueWakeOff,
    SqSetup8152, SqSetup8153, SqSetupLater, SqPhy8152, SqPhy8153, SqPhyLater,
    SqFirstFill, SqWake8152, SqWake8153, SqWake8153B, SqWake8153C, SqSleep, SqDetach,
    SqRxOn, SqRxOff, SqBiasFull, SqBiasLow,
    SqSetupMG, SqPhy56A, SqPhy7020, SqPhy7030, SqPhy56B, SqPhy7400Pre, SqPhy7400,
    SqPhy7410, SqPhy7420, SqPhyMGTail, SqNicResetMG, SqDrainMG, SqWakeMG, SqRxOnMG,
    SqRxOffMG, SqSleepMG, SqDetachMG, SqPhy57, SqPhy1030, SqPhy2020
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

/* The indirect register window (UR_ADV_*): wait while it is busy, give
 * the address and the command, wait, then move the dword. A window that
 * stays busy for 20 ms skips the access. */
static int AdvIdle(RTL_STATE *s, NET_IO *io)
{
    static const STEP idle = WAIT(USB, 2, UR_ADV_CMD, ADV_BUSY, 0, 10, 2);
    return WaitFor(s, io, &idle, 0);
}

static int AdvGet(RTL_STATE *s, NET_IO *io, nc_u16 space, nc_u16 reg, nc_u32 *value)
{
    if (!AdvIdle(s, io)) return 0;
    Put(io, RTL_SPACE_USB, UR_ADV_ADDR, 2, reg);
    Put(io, RTL_SPACE_USB, UR_ADV_CMD, 2, space | ADV_BUSY);
    if (!AdvIdle(s, io)) return 0;
    *value = Get(io, RTL_SPACE_USB, UR_ADV_DATA, 4);
    return io->Status >= 0;
}

static void AdvPut(RTL_STATE *s, NET_IO *io, nc_u16 space, nc_u16 reg, nc_u32 value)
{
    if (!AdvIdle(s, io)) return;
    Put(io, RTL_SPACE_USB, UR_ADV_DATA, 4, value);
    Put(io, RTL_SPACE_USB, UR_ADV_ADDR, 2, reg);
    Put(io, RTL_SPACE_USB, UR_ADV_CMD, 2, space | ADV_WRITE | ADV_BUSY);
}

/* The PHY's patch window: asked for, then granted within 5 s, or the
 * request is withdrawn and 0 returned. */
static int PatchAsk(RTL_STATE *s, NET_IO *io)
{
    static const STEP granted = PHY_WAIT(PR_PATCH_STATE, PATCH_GRANTED, PATCH_GRANTED, 5000, 1);
    PhyPut(s, io, PR_PATCH_CMD, (nc_u16)(PhyGet(s, io, PR_PATCH_CMD) | PATCH_ASK));
    WaitFor(s, io, &granted, 0);
    if (io->Status < 0) return 0;
    if (!(PhyGet(s, io, PR_PATCH_STATE) & PATCH_GRANTED)) {
        PhyPut(s, io, PR_PATCH_CMD, (nc_u16)(PhyGet(s, io, PR_PATCH_CMD) & ~PATCH_ASK));
        return 0;
    }
    return 1;
}

static void PatchDone(RTL_STATE *s, NET_IO *io)
{
    static const STEP released = PHY_WAIT(PR_PATCH_STATE, PATCH_GRANTED, 0, 5000, 1);
    PhyPut(s, io, PR_PATCH_CMD, (nc_u16)(PhyGet(s, io, PR_PATCH_CMD) & ~PATCH_ASK));
    WaitFor(s, io, &released, 0);
}

/* A sequence ends early on an I/O failure, or when a step gave up
 * (Abort): then every sequence running ends, as the caller wants. */
static void Run(RTL_STATE *s, NET_IO *io, int q)
{
    const STEP *p;
    for (p = Seq[q]; p->Op != OP_END; p++) {
        nc_u16 space = (nc_u16)(p->Space ? RTL_SPACE_MAC : RTL_SPACE_USB);
        if (io->Status < 0 || s->Abort) return;
        switch (p->Op) {
        case OP_ONLY:
            if (!(p->Clear & s->Kind) || !(p->Reg & SpeedBit(s))) p += p->Count;
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
            PhyPut(s, io, (nc_u16)(p->Space ? PR_SRAM2_ADDR : PR_SRAM_ADDR), p->Reg);
            PhyPut(s, io, (nc_u16)(p->Space ? PR_SRAM2_DATA : PR_SRAM_DATA), (nc_u16)p->Set);
            break;
        case OP_SRAM_EDIT: {
            nc_u16 v, data = (nc_u16)(p->Space ? PR_SRAM2_DATA : PR_SRAM_DATA);
            PhyPut(s, io, (nc_u16)(p->Space ? PR_SRAM2_ADDR : PR_SRAM_ADDR), p->Reg);
            v = PhyGet(s, io, data);
            PhyPut(s, io, data, (nc_u16)((v & ~p->Clear) | p->Set));
            break;
        }
        case OP_ADV_EDIT: {
            nc_u32 v;
            if (AdvGet(s, io, p->Count, p->Reg, &v)) AdvPut(s, io, p->Count, p->Reg, (v & ~p->Clear) | p->Set);
            break;
        }
        case OP_PATCH_ASK: if (!PatchAsk(s, io)) s->Abort = 1; break;
        case OP_PATCH_DONE: PatchDone(s, io); break;
        case OP_SLEEP: NcIoSleep(io, p->Ms); break;
        case OP_RUN: Run(s, io, p->Count); break;
        case OP_HOOK: Hook(s, io, p->Count); break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Hooks: the steps that depend on a value read back */

/* The multi-gigabit speeds advertised (register 7.32): only on a
 * SuperSpeed link, where the USB side can carry them. */
static nc_u16 MultiGig(const RTL_STATE *s)
{
    if (s->UsbSpeed != NC_SPEED_SUPER) return 0;
    if (s->Kind & K_2G5) return MGIG_2G5;
    if (s->Kind & K_1030) return MGIG_2G5 | MGIG_5G;
    if (s->Kind & K_2020) return MGIG_2G5 | MGIG_5G | MGIG_10G;
    return 0;
}

/* Autonegotiation: 10/100 (and 1000 full on a gigabit chip, and the
 * multi-gigabit speeds on a chip that has them) advertised, then the PHY
 * reset with autonegotiation enabled and restarted. */
static void Negotiate(RTL_STATE *s, NET_IO *io)
{
    static const STEP reset = PHY_WAIT(PR_MII, MIICTL_RESET, 0, 50, 20);
    nc_u16 was = PhyGet(s, io, MII(MII_AN_ADV));
    if ((was | ANADV_10_100) != was) PhyPut(s, io, MII(MII_AN_ADV), (nc_u16)(was | ANADV_10_100));
    if (s->Kind & (K_GIG | K_MG)) {
        nc_u16 now;
        was = PhyGet(s, io, MII(MII_GIG_CONTROL));
        now = (nc_u16)((was & ~GIGCTL_1000_MASK) | GIGCTL_1000_FULL);
        if (now != was) PhyPut(s, io, MII(MII_GIG_CONTROL), now);
    }
    if (s->Kind & (K_2G5 | K_57)) {
        nc_u16 now;
        was = PhyGet(s, io, PR_MGIG_CTRL);
        now = (nc_u16)((was & ~(MGIG_2G5 | MGIG_5G | MGIG_10G)) | MultiGig(s));
        if (now != was) PhyPut(s, io, PR_MGIG_CTRL, now);
    }
    PhyPut(s, io, MII(MII_CONTROL), MIICTL_RESET | MIICTL_AN_ENABLE | MIICTL_AN_RESTART);
    WaitFor(s, io, &reset, 0);
}

/* The sequence each chip runs for each part of its life. */
enum { R_SETUP, R_PHY, R_WAKE, R_RX_ON, R_RX_OFF, R_SLEEP, R_DETACH };

static int SeqFor(const RTL_STATE *s, int role)
{
    if (s->Kind & K_MG) {
        switch (role) {
        case R_SETUP: return Q_SETUP_MG;
        case R_PHY: return (s->Kind & K_56A) ? Q_PHY_56A : (s->Kind & K_57) ? Q_PHY_57 : Q_PHY_56B;
        case R_WAKE: return Q_WAKE_MG;
        case R_RX_ON: return Q_RX_ON_MG;
        case R_RX_OFF: return Q_RX_OFF_MG;
        case R_SLEEP: return Q_SLEEP_MG;
        default: return Q_DETACH_MG;
        }
    }
    switch (role) {
    case R_SETUP: return (s->Kind & K_8152) ? Q_SETUP_8152 : (s->Kind & K_8153) ? Q_SETUP_8153 : Q_SETUP_LATER;
    case R_PHY: return (s->Kind & K_8152) ? Q_PHY_8152 : (s->Kind & K_8153) ? Q_PHY_8153 : Q_PHY_LATER;
    case R_WAKE:
        return (s->Kind & K_8152) ? Q_WAKE_8152 : (s->Kind & K_8153) ? Q_WAKE_8153 :
               (s->Kind & K_8153B) ? Q_WAKE_8153B : Q_WAKE_8153C;
    case R_RX_ON: return Q_RX_ON;
    case R_RX_OFF: return Q_RX_OFF;
    case R_SLEEP: return Q_SLEEP;
    default: return Q_DETACH;
    }
}

static int PhySequence(const RTL_STATE *s)
{
    return SeqFor(s, R_PHY);
}

/* The PHY setup: a step that gave up (a patch window refused) ends it,
 * and bring-up goes on, as the chips' own sequences do. */
static void PhySetup(RTL_STATE *s, NET_IO *io)
{
    Run(s, io, PhySequence(s));
    s->Abort = 0;
}

/* Descriptor formats (DESC_*). */
typedef struct FORMAT {
    nc_u8 RxDesc, RxAlign;      /* receive record header and record alignment */
    nc_u8 TxDesc, TxAlign;      /* transmit header and its alignment */
    nc_u8 EarlyUnit;            /* UR_RX_EARLY_LEN units, 8156 core on */
} FORMAT;

static const FORMAT Formats[] = {
    { RX_DESC_LEN, RX_RECORD_ALIGN, TX_DESC_LEN, TX_ALIGN, 8 },     /* DESC_V1 */
    { RX2_DESC_LEN, RX2_ALIGN, TX2_DESC_LEN, TX2_ALIGN, 16 }        /* DESC_V2 */
};

static const FORMAT *Fmt(const RTL_STATE *s)
{
    return &Formats[s && s->Desc < sizeof(Formats) / sizeof(Formats[0]) ? s->Desc : DESC_V1];
}

static nc_u32 RxBuffer(const RTL_STATE *s);

/* One pair of the PHY's per-pair settings (selected through 0xd068
 * bits 3-4, read and written at 0xd06a) exchanged in the low 11 bits. */
static void SwapLanes(RTL_STATE *s, NET_IO *io, nc_u16 *sel, nc_u16 p, nc_u16 q)
{
    nc_u16 a, b;
    *sel = (nc_u16)((*sel & ~0x18) | p); PhyPut(s, io, 0xd068, *sel); a = PhyGet(s, io, 0xd06a);
    *sel = (nc_u16)((*sel & ~0x18) | q); PhyPut(s, io, 0xd068, *sel); b = PhyGet(s, io, 0xd06a);
    *sel = (nc_u16)((*sel & ~0x18) | p); PhyPut(s, io, 0xd068, *sel);
    PhyPut(s, io, 0xd06a, (nc_u16)((a & ~0x7ff) | (b & 0x7ff)));
    *sel = (nc_u16)((*sel & ~0x18) | q); PhyPut(s, io, 0xd068, *sel);
    PhyPut(s, io, 0xd06a, (nc_u16)((b & ~0x7ff) | (a & 0x7ff)));
}

/* Two registers holding two 5-bit pair fields each (bits 0-4, 8-12):
 * each register takes the other's fields, crossed. */
static void SwapFields(RTL_STATE *s, NET_IO *io, nc_u16 x, nc_u16 y)
{
    nc_u16 a = PhyGet(s, io, x), b = PhyGet(s, io, y);
    PhyPut(s, io, x, (nc_u16)((a & ~0x1f1f) | ((b & 0x1f) << 8) | ((b >> 8) & 0x1f)));
    PhyPut(s, io, y, (nc_u16)((b & ~0x1f1f) | ((a & 0x1f) << 8) | ((a >> 8) & 0x1f)));
}

/* Pairs A-D and B-C exchanged, for a board with its MDI pairs reversed. */
static void SwapPairs(RTL_STATE *s, NET_IO *io)
{
    nc_u16 sel = (nc_u16)((PhyGet(s, io, 0xd068) & ~0x1f) | 0x01);
    SwapLanes(s, io, &sel, 0x00, 0x18);
    SwapLanes(s, io, &sel, 0x08, 0x10);
    SwapFields(s, io, 0xbd5a, 0xbd5c);
    SwapFields(s, io, 0xbc18, 0xbc1a);
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
        if (!PatchAsk(s, io)) break;
        PhyPut(s, io, PR_POWER, (nc_u16)(PhyGet(s, io, PR_POWER) | 0x8000));
        PhyPut(s, io, PR_SPEED_DOWN, (nc_u16)(PhyGet(s, io, PR_SPEED_DOWN) | 0x6800));
        PhyPut(s, io, PR_SYSCLK, 0);
        PhyPut(s, io, PR_SYSCLK, 5 << 4);
        PatchDone(s, io);
        break;
    }
    case H_EEE_PLUS:    /* EEE+ on at 10 Mb/s only (never on the RTL8159) */
        if (!(s->Kind & K_2020) && (Get(io, RTL_SPACE_MAC, MR_LINK, 2) & LNK_10))
            Edit(io, RTL_SPACE_MAC, MR_EEE_PLUS, 2, 0, 0x0002);
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

    /* ---- the 8156 core ---- */
    case H_FLASH_WAIT: { /* a PHY that loads its code from flash: up to 100 ms for it */
        static const STEP loaded = WAIT(USB, 2, UR_GPHY, GPHY_PATCH_DONE, GPHY_PATCH_DONE, 100, 1);
        if ((Get(io, RTL_SPACE_MAC, MR_GPHY_FLASH, 2) & 0x0002) &&
            !(Get(io, RTL_SPACE_USB, UR_GPHY, 2) & GPHY_NO_FLASH))
            WaitFor(s, io, &loaded, 0);
        break;
    }
    case H_RESTORE_WAIT: { /* after a power cut, the chip's own restore: 20 ms, else the chip is refused */
        static const STEP restored = WAIT(USB, 2, UR_GPHY, GPHY_RESTORED, GPHY_RESTORED, 20, 1);
        if ((Get(io, RTL_SPACE_USB, UR_MISC0, 2) & 0x0001) && !WaitFor(s, io, &restored, 0) && io->Status >= 0) {
            s->Failed = 1;
            s->Abort = 1;
        }
        break;
    }
    case H_EXT_INIT:     /* a PHY waiting for external init: its flags cleared */
    case H_EXT_INIT_PHY: {
        static const STEP settled = PHY_STATE(ST_SETTLED);
        WaitFor(s, io, &settled, &v);
        if (io->Status < 0 || v != PHYST_EXT_INIT) break;
        /* In the base setup the B flags go first, in the PHY setup the A flag. */
        if (h == H_EXT_INIT_PHY && !(s->Kind & K_56A)) PhyPut(s, io, PR_EXT_A, (nc_u16)(PhyGet(s, io, PR_EXT_A) & ~0x0001));
        PhyPut(s, io, PR_EXT_B, (nc_u16)(PhyGet(s, io, PR_EXT_B) & ~0x000a));
        if (h == H_EXT_INIT && !(s->Kind & K_56A)) PhyPut(s, io, PR_EXT_A, (nc_u16)(PhyGet(s, io, PR_EXT_A) & ~0x0001));
        break;
    }
    case H_FW_CTRL:      /* flow-control patch: the second form without DACK detection */
        v = Get(io, RTL_SPACE_USB, UR_FW_CTRL, 2);
        if (!(Get(io, RTL_SPACE_MAC, MR_GPIO, 2) & 0x8000)) v |= 0x0100;
        Put(io, RTL_SPACE_USB, UR_FW_CTRL, 2, v & ~0x0008UL);
        break;
    case H_UPS_CHECK_MG: /* power was cut: the PHY is set up again */
        if (Get(io, RTL_SPACE_USB, UR_MISC0, 2) & 0x0001) {
            PhySetup(s, io);
            Negotiate(s, io);
        }
        break;
    case H_MDI_SWAP:     /* an RTL8156 wired with its pairs reversed: the PHY's tables swapped */
        if ((Get(io, RTL_SPACE_USB, UR_UPS_CFG, 2) & 0x0020) && (PhyGet(s, io, 0xd068) & 0x0002))
            SwapPairs(s, io);
        break;
    case H_RX_EARLY: {   /* early receive: a buffer less one full record */
        const FORMAT *f = Fmt(s);
        v = RxBuffer(s) - (RX_MAX_LEN + f->RxDesc + f->RxAlign);
        Put(io, RTL_SPACE_USB, UR_RX_EARLY_LEN, 2, v / f->EarlyUnit);
        break;
    }
    case H_IDLE_2G5:     /* idle slow-down off at 2.5 Gb/s */
        if (Get(io, RTL_SPACE_MAC, MR_LINK, 2) & LNK_2500) Edit(io, RTL_SPACE_MAC, MR_POWER4, 2, 0x0040, 0);
        else Edit(io, RTL_SPACE_MAC, MR_POWER4, 2, 0, 0x0040);
        break;
    case H_EEE_TW:       /* EEE wake time by speed */
        v = Get(io, RTL_SPACE_MAC, MR_LINK, 2);
        if (v & LNK_1000) Put(io, RTL_SPACE_MAC, MR_EEE_TW, 2, 0x11);
        else if (v & LNK_500) Put(io, RTL_SPACE_MAC, MR_EEE_TW, 2, 0x3d);
        break;
    case H_L1_NYET:      /* USB 2 L1: the NYET timing by flow control */
        if ((Get(io, RTL_SPACE_MAC, MR_LINK, 2) & (LNK_TX_FLOW | LNK_RX_FLOW)) == (LNK_TX_FLOW | LNK_RX_FLOW))
            Edit(io, RTL_SPACE_USB, UR_L1_CTRL, 2, 0x000f, 0x000f);
        else Edit(io, RTL_SPACE_USB, UR_L1_CTRL, 2, 0x000f, 0x0001);
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

static nc_u32 KindOf(nc_u16 id)
{
    unsigned i;
    for (i = 0; i < sizeof(Ids) / sizeof(Ids[0]); i++)
        if (Ids[i] == id) return 1UL << i;
    return 0;
}

/* Bulk-IN buffer: 16 KiB on the 8152, and on the 8153 below SuperSpeed;
 * else 32 KiB, the lower edge's largest (RX_BUFFER_MAX). The 8156 core's
 * early-receive size is set from this buffer (H_RX_EARLY), so a chip
 * that could fill more never sends a transfer longer than it. */
static nc_u32 RxBuffer(const RTL_STATE *s)
{
    if (s->Kind & K_8152) return 16384;
    if ((s->Kind & K_8153) && s->UsbSpeed != NC_SPEED_SUPER) return 16384;
    return RX_BUFFER_MAX;
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
    s->Abort = 0;
    s->Failed = 0;
    s->Desc = (nc_u8)((s->Kind & K_57) ? DESC_V2 : DESC_V1);

    Run(s, io, SeqFor(s, R_SETUP));
    if (s->Failed) {
        if (io->Status >= 0) io->Status = NC_E_DEVICE;
        return io->Status;
    }
    s->Abort = 0;
    if (s->Kind & K_LATER) s->SetUpOnce = 1;
    PhySetup(s, io);
    Negotiate(s, io);
    /* The station address: the 4C00 keeps it in MR_NODE_ID; the others
     * load it into MR_NODE_ID_SAVED, from where it is copied. */
    NcIoRead(io, RTL_REQ, (nc_u16)(s->Id == ID_4C00 ? MR_NODE_ID : MR_NODE_ID_SAVED), RTL_SPACE_MAC, b, 8);
    for (i = 0; i < 6; i++) s->Mac[i] = b[i];
    if (s->Id != ID_4C00 && NcValidMac(s->Mac)) WriteNodeId(io, s->Mac);
    Run(s, io, SeqFor(s, R_WAKE));
    if (io->Status < 0) return io->Status;

    for (i = 0; i < 6; i++) params->Mac[i] = s->Mac[i];
    params->RxSize = RxBuffer(s);
    params->RxCount = 4;
    params->TxSize = TX_BUFFER;
    params->TxSlots = 4;
    params->TxBatch = 32;
    params->TxHeader = Fmt(s)->TxDesc;
    params->TxAlign = Fmt(s)->TxAlign;
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
            Run(s, io, SeqFor(s, R_RX_OFF));
        }
        link->Up = 0;
        link->Mbps = 0;
        link->FullDuplex = 0;
        return io->Status < 0 ? io->Status : NC_OK;
    }
    if (!s->Carrier) {
        Run(s, io, SeqFor(s, R_RX_ON));
        ApplyFilter(s, io);
        if (io->Status < 0) return io->Status;
        s->Carrier = 1;
    }
    link->Up = 1;
    link->Mbps = (st & LNK_10000) ? 10000 : (st & LNK_5000) ? 5000 : (st & LNK_2500) ? 2500 :
                 (st & LNK_1000) ? 1000 : (st & LNK_100) ? 100 : 10;
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
    Run(s, io, SeqFor(s, R_SLEEP));
    Run(s, io, SeqFor(s, R_DETACH));
    return io->Status < 0 ? io->Status : NC_OK;
}

static long RtlSuspend(void *chip, NET_IO *io)
{
    RTL_STATE *s = chip;
    if (!s->Id) return NC_OK;
    s->Carrier = 0;
    Run(s, io, SeqFor(s, R_SLEEP));
    return io->Status < 0 ? io->Status : NC_OK;
}

static int RtlStatus(void *chip, const nc_u8 *data, nc_u32 length)
{
    (void)chip;
    if (length < 2) return -1;
    return (data[0] & IRQ_LINK) ? 1 : 0;
}

/* The frame length a receive header gives, with its 4-byte FCS. */
static nc_u32 RxWire(const RTL_STATE *s, const nc_u8 *desc)
{
    switch (s ? s->Desc : DESC_V1) {
    case DESC_V2: return Le32(desc) >> RX2_LEN_SHIFT;   /* bits 17-31 */
    default: return Le32(desc) & 0x7fff;     /* bits 0-14 */
    }
}

/* One bulk-IN transfer holds records: a header (24 bytes in the first
 * format, 16 in the second) whose first dword gives the frame length with
 * its 4-byte FCS, then the frame, then padding to the format's record
 * alignment (8, or 16). A
 * length below 60 marks the end. The transfer is checked whole (first
 * pass) before any frame is handed up (second pass); a frame running past
 * its end rejects it. Frames longer than NC_ETH_MAX are counted as
 * dropped. */
long RtlRxParse(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
                NET_RX_STATS *stats)
{
    const RTL_STATE *s = chip;
    const FORMAT *f = Fmt(s);
    nc_u32 head = f->RxDesc, align = f->RxAlign;
    int deliver;
    nc_u32 frames = 0, dropped = 0;
    stats->Frames = 0;
    stats->Dropped = 0;
    if (length < NC_ETH_PAD) return 0;
    for (deliver = 0; deliver < 2; deliver++) {
        nc_u32 at = 0;
        frames = dropped = 0;
        while (at < length && length - at > head) {
            nc_u32 left = length - at - head;
            nc_u32 wire = RxWire(s, data + at);
            if (wire < NC_ETH_PAD) break;
            if (wire > left) return NC_E_MALFORMED;
            if (wire - 4 <= NC_ETH_MAX) {
                if (deliver) emit(context, data + at + head, wire - 4);
                frames++;
            } else {
                dropped++;
            }
            at = (at + head + wire + align - 1) & ~(align - 1);
        }
    }
    stats->Frames = frames;
    stats->Dropped = dropped;
    return (long)frames;
}

/* Each frame is preceded by a header: in the first format 8 bytes, the
 * first and last segment bits and the length; no offloads. The second
 * format's 16 bytes add the length again, shifted left 4, and the
 * signature the chip checks. */
static void RtlTxHeader(void *chip, nc_u8 *hdr, nc_u32 length, nc_u32 maxPacket, int single)
{
    const RTL_STATE *s = chip;
    (void)maxPacket; (void)single;
    switch (s ? s->Desc : DESC_V1) {
    case DESC_V2:
        SetLe32(hdr, TX_FIRST_LAST | length);
        SetLe32(hdr + 4, 0);
        SetLe32(hdr + 8, length << TX2_LEN_SHIFT);
        SetLe32(hdr + 12, TX2_SIGNATURE);
        break;
    default:
        SetLe32(hdr, TX_FIRST_LAST | length);
        SetLe32(hdr + 4, 0);
        break;
    }
}

/* No padding and no zero-length packet after a transfer. */
static int RtlTxEnd(void *chip, nc_u8 *buffer, nc_u32 *total, nc_u32 capacity, nc_u32 maxPacket)
{
    (void)chip; (void)buffer; (void)total; (void)capacity; (void)maxPacket;
    return 0;
}

const NET_CHIP NetPackageChip = {
    "rtl815x",
    "usb-wdm-free RTL815X (Realtek RTL8152/8153/8156/8157/8159 USB Ethernet)",
    0x0001,
    RtlProbe, RtlStart, RtlSetMac, RtlLink, RtlPoll, RtlSetFilter, RtlStop, RtlSuspend, RtlResume,
    RtlStatus, RtlRxParse, RtlTxHeader, RtlTxEnd
};

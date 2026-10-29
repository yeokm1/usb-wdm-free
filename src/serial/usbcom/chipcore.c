/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chipcore.c - usbcom's chip modules and CDC-ACM's control requests as one
 * step generator (chipcore.h; docs/design/serial.md 6.2, 6.3).
 *
 * Original code. The facts for FTDI, PL2303 and CP210x (ids, request codes,
 * register values and masks, bit positions, rate encodings, limits and the
 * AN205 rate table) are those the design lists from Linux v6.12
 * drivers/usb/serial/ftdi_sio.c, ftdi_sio.h, pl2303.c and cp210x.c; from the
 * GPL-2.0-only files only facts are taken (AGENTS.md, "Sources"). It calls
 * nothing and compiles into USBCOM.SYS and the host tests.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include "chipcore.h"

static AC_U16 Le16(const AC_U8 *p) { return (AC_U16)(p[0] | (p[1] << 8)); }
static AC_U32 Le32(const AC_U8 *p)
{
    return (AC_U32)p[0] | ((AC_U32)p[1] << 8) | ((AC_U32)p[2] << 16) | ((AC_U32)p[3] << 24);
}
static void Put32(AC_U8 *p, AC_U32 v)
{
    p[0] = (AC_U8)v; p[1] = (AC_U8)(v >> 8); p[2] = (AC_U8)(v >> 16); p[3] = (AC_U8)(v >> 24);
}

/* ---- the family table ---------------------------------------------------- */

typedef struct AC_ID { AC_U16 Vid, Pid; AC_U8 Family; } AC_ID;

/* USBCOM.INF lists the ids marked "INF"; the others only its manual model. */
static const AC_ID g_Ids[] = {
    { 0x0403, 0x6001, AC_FAM_FTDI },     /* FT232AM/BM/R (manual only) */
    { 0x0403, 0x6010, AC_FAM_FTDI },     /* FT2232C/D/H (manual only) */
    { 0x0403, 0x6011, AC_FAM_FTDI },     /* FT4232H (INF) */
    { 0x0403, 0x6014, AC_FAM_FTDI },     /* FT232H (INF) */
    { 0x0403, 0x6015, AC_FAM_FTDI },     /* FT-X (INF) */
    { 0x067B, 0x2303, AC_FAM_PL2303 },   /* H, HX, HXD, TA (INF) */
    { 0x067B, 0x2304, AC_FAM_PL2303 },   /* TB (INF) */
    { 0x067B, 0x23A3, AC_FAM_PL2303 },   /* GC (INF) */
    { 0x067B, 0x23B3, AC_FAM_PL2303 },   /* GB (INF) */
    { 0x067B, 0x23C3, AC_FAM_PL2303 },   /* GT (INF) */
    { 0x067B, 0x23D3, AC_FAM_PL2303 },   /* GL (INF) */
    { 0x067B, 0x23E3, AC_FAM_PL2303 },   /* GE (INF) */
    { 0x067B, 0x23F3, AC_FAM_PL2303 },   /* GS (INF) */
    { 0x10C4, 0xEA60, AC_FAM_CP210X },   /* CP2101 to CP2104, CP2102N (manual only) */
    { 0x10C4, 0xEA70, AC_FAM_CP210X },   /* CP2105 (INF) */
    { 0x10C4, 0xEA71, AC_FAM_CP210X },   /* CP2108 (INF) */
    { 0, 0, 0 }
};

int AcChipFamily(AC_U16 vid, AC_U16 pid)
{
    int i;
    for (i = 0; g_Ids[i].Vid; i++)
        if (g_Ids[i].Vid == vid && g_Ids[i].Pid == pid)
            return g_Ids[i].Family;
    return AC_FAM_CDC;
}

/* ---- limits ---------------------------------------------------------------- */

#define DB(n)        (1UL << (n))
#define DB_5_TO_8    (DB(5) | DB(6) | DB(7) | DB(8))
#define SB_1         0x01UL      /* DCB StopBits 0 */
#define SB_15        0x02UL      /* DCB StopBits 1 */
#define SB_2         0x04UL      /* DCB StopBits 2 */
#define PAR_ALL      0x1FUL      /* none, odd, even, mark, space */

static void Limits(UCIF_LIMITS *l, AC_U32 lo, AC_U32 hi, AC_U32 bits, AC_U32 stops, AC_U32 par)
{
    l->MinBaud = lo;
    l->MaxBaud = hi;
    l->DataBits = bits;
    l->StopBits = stops;
    l->Parity = par;
    l->Max56Baud = 0;
}

static int FtHigh(const AC_CHIP *c)
{
    return c->Part == AC_FT_2232H || c->Part == AC_FT_4232H || c->Part == AC_FT_232H;
}

/* CP210x limits by part (Linux cp210x_init_max_speed). */
static void CpLimits(AC_CHIP *c)
{
    AC_U32 lo = 300, hi = 2000000, bits = DB_5_TO_8, par = PAR_ALL, stops = SB_1 | SB_2, max56 = 0;

    c->Flags &= (AC_U8)~AC_CF_ACTUAL;
    switch (c->Part) {
    case AC_CP_2101:
        hi = 921600; bits = DB(8); par = 0x07; stops = SB_1;   /* Linux cp210x_set_termios */
        break;
    case AC_CP_2102: case AC_CP_2103:
        hi = 1000000;
        break;
    case AC_CP_2104:
        c->Flags |= AC_CF_ACTUAL;
        break;
    case AC_CP_2105:
        if (c->Iface == 0) {
            c->Flags |= AC_CF_ACTUAL;
            max56 = 921600;                     /* CP2105 data sheet, Table 10 */
        } else {
            lo = 2400; hi = 921600;             /* the SCI: 8 bits, 1 stop, N/O/E */
            bits = DB(8); par = 0x07; stops = SB_1;
        }
        break;
    case AC_CP_2102N_28: case AC_CP_2102N_24: case AC_CP_2102N_20:
        c->Flags |= AC_CF_ACTUAL;
        hi = 3000000;
        break;
    default:                                    /* CP2108, unknown */
        break;
    }
    Limits(&c->Limits, lo, hi, bits, stops, par);
    c->Limits.Max56Baud = max56;
}

static void PlLimits(AC_CHIP *c)
{
    AC_U32 hi = 12000000;
    if (c->Part == AC_PL_H) hi = 1228800;
    else if (c->Part == AC_PL_HX || c->Part == AC_PL_TA) hi = 6000000;
    Limits(&c->Limits, 75, hi, DB_5_TO_8, SB_1 | SB_15 | SB_2, PAR_ALL);
}

/* PL2303 part from the descriptor (Linux pl2303_detect_type). */
static AC_U8 PlPart(const AC_CHIP *c)
{
    if (c->DevClass == 0x02 || c->MaxPacket0 != 0x40)
        return AC_PL_H;
    if (c->BcdUsb == 0x0101 || c->BcdUsb == 0x0110)
        return (AC_U8)(c->BcdDevice == 0x0400 ? AC_PL_HXD : AC_PL_HX);
    if (c->BcdUsb == 0x0200) {
        switch (c->BcdDevice) {
        case 0x0300: case 0x0500:
            return AC_PL_PROBE;
        case 0x0100: case 0x0105: case 0x0305: case 0x0400: case 0x0405:
        case 0x0505: case 0x0600: case 0x0605: case 0x0700: case 0x0705:
            return AC_PL_G;
        default:
            break;
        }
    }
    return AC_PL_HX;          /* Linux refuses an unknown part; it is taken as HX here */
}

static AC_U8 FtPart(AC_U16 bcd)
{
    switch (bcd) {
    case 0x0200: return AC_FT_AM;
    case 0x0400: return AC_FT_BM;
    case 0x0500: return AC_FT_2232C;
    case 0x0600: return AC_FT_R;
    case 0x0700: return AC_FT_2232H;
    case 0x0800: return AC_FT_4232H;
    case 0x0900: return AC_FT_232H;
    case 0x1000: return AC_FT_X;
    default:     return 0;
    }
}

void AcChipSetup(AC_CHIP *c, int family, const AC_U8 *dev, AC_U8 iface, AC_U16 inMax)
{
    AC_U8 *p = (AC_U8 *)c;
    AC_U32 i;
    for (i = 0; i < sizeof(*c); i++)
        p[i] = 0;
    c->Family = (AC_U8)family;
    c->Iface = iface;
    c->InMax = (AC_U16)(inMax ? inMax : 64);
    c->DevClass = dev[4];
    c->MaxPacket0 = dev[7];
    c->SerialIndex = dev[16];
    c->BcdUsb = Le16(dev + 2);
    c->BcdDevice = Le16(dev + 12);
    c->Flags = AC_CF_DTR | AC_CF_RTS;
    c->FtData = 8;                              /* 8N1 */
    switch (family) {
    case AC_FAM_FTDI:
        c->Part = FtPart(c->BcdDevice);
        if (c->Part == 0)
            c->Part = AC_FT_BM;                 /* SIO and unknown parts: as the BM */
        c->Channel = (AC_U8)((c->Part == AC_FT_AM || c->Part == AC_FT_BM || c->Part == AC_FT_R)
                     ? 0 : 1 + iface);
        Limits(&c->Limits, 184, FtHigh(c) ? 12000000UL : 3000000UL, DB(7) | DB(8), SB_1 | SB_2, PAR_ALL);
        break;
    case AC_FAM_PL2303:
        c->Part = PlPart(c);
        PlLimits(c);
        break;
    case AC_FAM_CP210X:
        CpLimits(c);
        break;
    default:
        Limits(&c->Limits, 1, 0xFFFFFFFFUL, DB_5_TO_8 | DB(16), SB_1 | SB_15 | SB_2, PAR_ALL);
        break;
    }
}

int AcChipFindPorts(int family, const AC_U8 *cfg, AC_U32 len, AC_FUNC *out, int max)
{
    return AcFindVendorPorts(cfg, len, family == AC_FAM_PL2303, out, max);
}

int AcChipPolls(const AC_CHIP *c)
{
    return c->Family == AC_FAM_CP210X;
}

/* The CP2102N: its flow control is not used (Linux disables it on early
   firmware, which reads the flow block wrongly; the firmware is not read here). */
static int CpNoFlow(const AC_CHIP *c)
{
    return c->Part == AC_CP_2102N_28 || c->Part == AC_CP_2102N_24 || c->Part == AC_CP_2102N_20;
}

AC_U32 AcChipFlowCaps(const AC_CHIP *c)
{
    switch (c->Family) {
    case AC_FAM_FTDI:   return UCIF_CAP_FLOW | UCIF_CAP_CTSNOW | UCIF_CAP_PAIRFLOW;
    case AC_FAM_PL2303: return UCIF_CAP_FLOW | UCIF_CAP_PAIRFLOW;
    case AC_FAM_CP210X: return UCIF_CAP_FLOW | UCIF_CAP_CTSNOW | (CpNoFlow(c) ? 0 : UCIF_CAP_CTSFLOW);
    default:            return 0;
    }
}

int AcChipResetsPipes(const AC_CHIP *c)
{
    return c->Family == AC_FAM_PL2303 && c->Part == AC_PL_H;    /* Linux pl2303_open */
}

int AcChipBreak(const AC_CHIP *c)
{
    /* The CP2105's second interface has no break (Linux cp210x_break_ctl). */
    return !(c->Family == AC_FAM_CP210X && c->Part == AC_CP_2105 && c->Iface == 1);
}

/* ---- rates ---------------------------------------------------------------- */

/* AN205 Table 1 as Linux has it: the rate a CP2102/3 runs for requests up
   to `High`. */
static const AC_U32 g_An205[][2] = {
    { 300, 300 }, { 600, 600 }, { 1200, 1200 }, { 1800, 1800 }, { 2400, 2400 },
    { 4000, 4000 }, { 4800, 4803 }, { 7200, 7207 }, { 9600, 9612 },
    { 14400, 14428 }, { 16000, 16062 }, { 19200, 19250 }, { 28800, 28912 },
    { 38400, 38601 }, { 51200, 51558 }, { 56000, 56280 }, { 57600, 58053 },
    { 64000, 64111 }, { 76800, 77608 }, { 115200, 117028 }, { 128000, 129347 },
    { 153600, 156868 }, { 230400, 237832 }, { 250000, 254234 },
    { 256000, 273066 }, { 460800, 491520 }, { 500000, 567138 },
    { 576000, 670254 }, { 921600, 0xFFFFFFFFUL }
};

AC_U32 AcChipRate(const AC_CHIP *c, AC_U32 rate)
{
    if (rate < c->Limits.MinBaud) rate = c->Limits.MinBaud;
    if (rate > c->Limits.MaxBaud) rate = c->Limits.MaxBaud;
    if (c->Family == AC_FAM_CP210X) {
        if (c->Flags & AC_CF_ACTUAL) {
            AC_U32 prescale = rate <= 365 ? 4 : 1;
            AC_U32 div = (48000000UL + prescale * rate) / (2 * prescale * rate);
            if (div == 0) div = 1;
            rate = 48000000UL / (2 * prescale * div);
        } else if (rate < 1000000) {
            AC_U32 i;
            for (i = 0; i < sizeof(g_An205) / sizeof(g_An205[0]); i++)
                if (rate <= g_An205[i][1])
                    break;
            rate = g_An205[i][0];
        }
    }
    return rate;
}

AC_U32 AcFtdiDivisor(const AC_CHIP *c, AC_U32 rate)
{
    rate = AcChipRate(c, rate);
    if (c->Part == AC_FT_AM)
        return AcFtdi232amDivisor(rate, 48000000UL);
    if (FtHigh(c) && rate >= 1200)
        return AcFtdi2232hDivisor(rate, 120000000UL);
    return AcFtdi232bmDivisor(rate, 48000000UL);
}

/* The rates the PL2303 takes as they are (Linux's direct list). */
static const AC_U32 g_PlDirect[] = {
    75, 150, 300, 600, 1200, 1800, 2400, 3600, 4800, 7200, 9600, 14400, 19200,
    28800, 38400, 57600, 115200, 230400, 460800, 614400, 921600, 1228800,
    2457600, 3000000, 6000000
};

/*
 * The PL2303 rate field. A rate the part takes as it is (the G series:
 * every rate; the others: the list above) is sent as a 32-bit number.
 * Otherwise byte 3 is 0x80 and the field holds m and e of
 * rate = 384 MHz / (m * 4^e), m below 512 in bits 0-8 and e (0 to 7) in
 * bits 9-11; on the TA and TB rate = 384 MHz / (m * 2^e), m below 2048 in
 * bits 0-10 and e (0 to 15) split over bits 13-15 and 16 (design 6.3).
 */
void AcPl2303Rate(const AC_CHIP *c, AC_U32 rate, AC_U8 out[4])
{
    int alt = c->Part == AC_PL_TA || c->Part == AC_PL_TB;
    AC_U32 i, q, m, e, top = alt ? 2048 : 512, emax = alt ? 15 : 7, bits = alt ? 1 : 2;

    rate = AcChipRate(c, rate);
    for (i = 0; i < sizeof(g_PlDirect) / sizeof(g_PlDirect[0]); i++)
        if (g_PlDirect[i] == rate)
            break;
    if (c->Part == AC_PL_G || i < sizeof(g_PlDirect) / sizeof(g_PlDirect[0])) {
        Put32(out, rate);
        return;
    }
    q = 384000000UL / rate;                       /* m * 4^e (or 2^e), rounded down */
    for (e = 0; e < emax && (q >> (bits * e)) >= top; e++)
        ;
    m = q >> (bits * e);
    if (m >= top) m = top - 1;
    if (m == 0) m = 1;
    out[0] = (AC_U8)m;
    out[3] = 0x80;
    if (alt) {
        out[1] = (AC_U8)((m >> 8) | ((e >> 1) << 5));
        out[2] = (AC_U8)(e & 1);
    } else {
        out[1] = (AC_U8)((m >> 8) | (e << 1));
        out[2] = 0;
    }
}

/* ---- the step generator ---------------------------------------------------- */

static int Send(AC_SETUP *s, AC_U8 type, AC_U8 req, AC_U16 value, AC_U16 index, AC_U16 len)
{
    AC_U32 i;
    s->Type = type;
    s->Request = req;
    s->Value = value;
    s->Index = index;
    s->Length = len;
    for (i = 0; i < AC_SETUP_MAX; i++)
        s->Data[i] = 0;
    return AC_STEP_SEND;
}

#define T_CLASS_IF_OUT   0x21
#define T_VENDOR_DEV_OUT 0x40
#define T_VENDOR_DEV_IN  0xC0
#define T_VENDOR_IF_OUT  0x41
#define T_VENDOR_IF_IN   0xC1

/* The line in a UCOP_LINE_CODING payload. */
typedef struct AC_LC { AC_U32 Rate; AC_U8 Stop, Parity, Bits; } AC_LC;

static void Lc(const AC_U8 *d, AC_LC *lc)
{
    lc->Rate = Le32(d);
    lc->Stop = d[4];
    lc->Parity = d[5];
    lc->Bits = d[6];
}

static void SetLines(AC_CHIP *c, AC_U32 value)
{
    c->Flags &= (AC_U8)~(AC_CF_DTR | AC_CF_RTS);
    if (value & 1) c->Flags |= AC_CF_DTR;
    if (value & 2) c->Flags |= AC_CF_RTS;
}

static AC_U16 LineBits(const AC_CHIP *c)
{
    return (AC_U16)(((c->Flags & AC_CF_DTR) ? 1 : 0) | ((c->Flags & AC_CF_RTS) ? 2 : 0));
}

static int StepCdc(AC_CHIP *c, AC_U32 op, AC_U32 value, const AC_U8 *data, AC_U32 step, AC_SETUP *s)
{
    AC_U32 i;
    if (step > 0)
        return AC_STEP_DONE;
    switch (op) {
    case UCOP_LINE_CODING:
        Send(s, T_CLASS_IF_OUT, 0x20, 0, c->Iface, 7);       /* SET_LINE_CODING */
        for (i = 0; i < 7; i++)
            s->Data[i] = data[i];
        return AC_STEP_SEND;
    case UCOP_CONTROL:
        return Send(s, T_CLASS_IF_OUT, 0x22, (AC_U16)(value & 3), c->Iface, 0);
    case UCOP_BREAK:
        return Send(s, T_CLASS_IF_OUT, 0x23, (AC_U16)value, c->Iface, 0);
    case AC_OP_CLOSE:
        /* DTR and RTS low again: the VxD's own drop may not have fitted. */
        return Send(s, T_CLASS_IF_OUT, 0x22, 0, c->Iface, 0);
    default:
        return AC_STEP_DONE;                 /* FLOW, INIT, OPEN, POLL */
    }
}

static void SetFlow(AC_CHIP *c, AC_U32 value)
{
    c->Flags &= (AC_U8)~(AC_CF_CTSFLOW | AC_CF_RTSFLOW);
    if (value & UCIF_FLOW_CTS) c->Flags |= AC_CF_CTSFLOW;
    if (value & UCIF_FLOW_RTS) c->Flags |= AC_CF_RTSFLOW;
}

static int BothFlow(const AC_CHIP *c)
{
    return (c->Flags & (AC_CF_CTSFLOW | AC_CF_RTSFLOW)) == (AC_CF_CTSFLOW | AC_CF_RTSFLOW);
}

static int StepFtdi(AC_CHIP *c, AC_U32 op, AC_U32 value, const AC_U8 *data, AC_U32 step, int ok,
                    AC_U32 inLen, AC_SETUP *s)
{
    AC_U16 ch = c->Channel;
    AC_LC lc;
    AC_U32 div;

    switch (op) {
    case AC_OP_INIT:
        /* An FT232BM without a serial number reports 0x0200, as the AM;
           its latency timer reads, the AM's does not (Linux). */
        if (c->Part != AC_FT_AM || c->SerialIndex != 0)
            return AC_STEP_DONE;
        if (step == 0)
            return Send(s, T_VENDOR_DEV_IN, 0x0A, 0, 0, 1);    /* GET_LATENCY_TIMER */
        if (ok && inLen >= 1) {
            c->Part = AC_FT_BM;
            c->Channel = 0;
        }
        return AC_STEP_DONE;
    case UCOP_LINE_CODING:
        Lc(data, &lc);
        if (step == 0) {
            c->FtData = (AC_U16)((lc.Bits == 7 ? 7 : 8) | ((AC_U16)(lc.Parity <= 4 ? lc.Parity : 0) << 8) |
                                 ((AC_U16)(lc.Stop == 2 ? 2 : 0) << 11));
            return Send(s, T_VENDOR_DEV_OUT, 4, (AC_U16)(c->FtData | ((c->Flags & AC_CF_BREAK) ? 0x4000 : 0)), ch, 0);
        }
        if (step == 1) {
            div = AcFtdiDivisor(c, lc.Rate);
            return Send(s, T_VENDOR_DEV_OUT, 3, (AC_U16)div,
                        (AC_U16)(ch ? (((div >> 16) & 0xFF) << 8) | ch : (div >> 16)), 0);
        }
        return AC_STEP_DONE;
    case UCOP_CONTROL:
        if (step > 0) return AC_STEP_DONE;
        SetLines(c, value);
        return Send(s, T_VENDOR_DEV_OUT, 1, (AC_U16)(0x0300 | LineBits(c)), ch, 0);
    case UCOP_BREAK:
        if (step > 0) return AC_STEP_DONE;
        if (value) c->Flags |= AC_CF_BREAK; else c->Flags &= (AC_U8)~AC_CF_BREAK;
        return Send(s, T_VENDOR_DEV_OUT, 4, (AC_U16)(c->FtData | (value ? 0x4000 : 0)), ch, 0);
    case UCOP_FLOW:
        /* One mode for both directions: on only when both are asked for.
           Off, the lines are written again: RTS writes made while the
           chip drove RTS itself were ignored (ftdi_sio.h). */
        if (step == 0) {
            SetFlow(c, value);
            return Send(s, T_VENDOR_DEV_OUT, 2, 0, (AC_U16)((BothFlow(c) ? 0x0100 : 0) | ch), 0);
        }
        if (step == 1 && !BothFlow(c))
            return Send(s, T_VENDOR_DEV_OUT, 1, (AC_U16)(0x0300 | LineBits(c)), ch, 0);
        return AC_STEP_DONE;
    case AC_OP_OPEN:
        if (step > 0) return AC_STEP_DONE;
        c->Flags &= (AC_U8)~(AC_CF_BREAK | AC_CF_STATE);
        return Send(s, T_VENDOR_DEV_OUT, 0, 0, ch, 0);        /* RESET */
    case AC_OP_CLOSE:
        /* Flow control off first: the chip ignores RTS writes while it is
           on (ftdi_sio.h), so DTR and RTS are dropped again after it. */
        if (step == 0) {
            c->Flags &= (AC_U8)~(AC_CF_CTSFLOW | AC_CF_RTSFLOW);
            return Send(s, T_VENDOR_DEV_OUT, 2, 0, ch, 0);
        }
        if (step == 1) {
            SetLines(c, 0);
            return Send(s, T_VENDOR_DEV_OUT, 1, 0x0300, ch, 0);
        }
        if (step == 2) {
            c->Flags &= (AC_U8)~AC_CF_BREAK;
            return Send(s, T_VENDOR_DEV_OUT, 4, c->FtData, ch, 0);
        }
        return AC_STEP_DONE;
    default:
        return AC_STEP_DONE;
    }
}

/* The PL2303 register sequence of the non-G parts' start (reads, writes). */
static const AC_U16 g_PlInit[11][2] = {
    { 0x8484, 0xFFFF }, { 0x0404, 0 }, { 0x8484, 0xFFFF }, { 0x8383, 0xFFFF },
    { 0x8484, 0xFFFF }, { 0x0404, 1 }, { 0x8484, 0xFFFF }, { 0x8383, 0xFFFF },
    { 0x0000, 1 }, { 0x0001, 0 }, { 0x0002, 0x44 }
};

static int PlRead(AC_CHIP *c, AC_SETUP *s, AC_U16 reg)
{
    return Send(s, T_VENDOR_DEV_IN, (AC_U8)(c->Part == AC_PL_G ? 0x81 : 0x01), reg, 0, 1);
}

static int PlWrite(AC_CHIP *c, AC_SETUP *s, AC_U16 reg, AC_U16 val)
{
    return Send(s, T_VENDOR_DEV_OUT, (AC_U8)(c->Part == AC_PL_G ? 0x80 : 0x01), reg, val, 0);
}

static int StepPl(AC_CHIP *c, AC_U32 op, AC_U32 value, const AC_U8 *data, AC_U32 *step, int ok,
                  const AC_U8 *in, AC_U32 inLen, AC_SETUP *s)
{
    AC_LC lc;
    AC_U32 i;

    switch (op) {
    case AC_OP_INIT:
        if (*step == 0) {
            if (c->Part == AC_PL_PROBE)
                return PlRead(c, s, 0x8080);      /* TA/TB answer it, the G series not */
            *step = 1;
        } else if (*step == 1 && c->Part == AC_PL_PROBE) {
            c->Part = (AC_U8)(ok && inLen >= 1 ? (c->BcdDevice == 0x0300 ? AC_PL_TA : AC_PL_TB) : AC_PL_G);
            PlLimits(c);
        }
        if (c->Part == AC_PL_G || *step > 11)
            return AC_STEP_DONE;
        i = *step - 1;
        if (g_PlInit[i][1] == 0xFFFF)
            return PlRead(c, s, g_PlInit[i][0]);
        return PlWrite(c, s, g_PlInit[i][0],
                       (AC_U16)(i == 10 && c->Part == AC_PL_H ? 0x24 : g_PlInit[i][1]));
    case UCOP_LINE_CODING:
        /* The class requests go to wIndex 0 (Linux), whichever interface
           holds the bulk endpoints. */
        if (*step == 1 && ok) {
            for (i = 0; i < 7; i++)
                c->PlLine[i] = c->PlNext[i];
            c->Flags |= AC_CF_PLLINE;
        }
        if (*step > 0) return AC_STEP_DONE;
        Lc(data, &lc);
        Send(s, T_CLASS_IF_OUT, 0x20, 0, 0, 7);
        AcPl2303Rate(c, lc.Rate, s->Data);
        s->Data[4] = lc.Stop;
        s->Data[5] = lc.Parity;
        s->Data[6] = lc.Bits;
        /* Not again when nothing changes: the part is known to lose bytes
           on a rewrite of the same settings (Linux pl2303_set_termios). */
        for (i = 0; i < 7 && (c->Flags & AC_CF_PLLINE) && s->Data[i] == c->PlLine[i]; i++)
            ;
        if (i == 7)
            return AC_STEP_DONE;
        for (i = 0; i < 7; i++)
            c->PlNext[i] = s->Data[i];
        return AC_STEP_SEND;
    case UCOP_CONTROL:
        if (*step > 0) return AC_STEP_DONE;
        SetLines(c, value);
        return Send(s, T_CLASS_IF_OUT, 0x22, LineBits(c), 0, 0);
    case UCOP_BREAK:
        if (*step > 0) return AC_STEP_DONE;
        return Send(s, T_CLASS_IF_OUT, 0x23, (AC_U16)(value ? 0xFFFF : 0), 0, 0);
    case UCOP_FLOW:
        /* One mode for both directions: on only when both are asked for. */
        if (*step == 0) {
            SetFlow(c, value);
            return PlRead(c, s, (AC_U16)(c->Part == AC_PL_G ? 0x0A : 0x80));
        }
        if (*step == 1 && ok && inLen >= 1) {
            AC_U8 mask, on, off, v = in[0];
            if (c->Part == AC_PL_G) { mask = 0x1C; on = 0x18; off = 0x1C; }
            else { mask = 0xF0; on = (AC_U8)(c->Part == AC_PL_H ? 0x40 : 0x60); off = 0; }
            v = (AC_U8)((v & ~mask) | ((BothFlow(c) ? on : off) & mask));
            return PlWrite(c, s, (AC_U16)(c->Part == AC_PL_G ? 0x0A : 0), v);
        }
        if (*step == 2 && !BothFlow(c))         /* the lines again, as on the FTDI */
            return Send(s, T_CLASS_IF_OUT, 0x22, LineBits(c), 0, 0);
        return AC_STEP_DONE;
    case AC_OP_OPEN:
        c->Flags &= (AC_U8)~AC_CF_PLLINE;           /* each open sets the line afresh */
        if (c->Part == AC_PL_H)
            return AC_STEP_DONE;
        if (c->Part == AC_PL_G)
            return *step == 0 ? PlWrite(c, s, 0x07, 0x03) : AC_STEP_DONE;
        if (*step == 0) return PlWrite(c, s, 0x08, 0);
        if (*step == 1) return PlWrite(c, s, 0x09, 0);
        return AC_STEP_DONE;
    case AC_OP_CLOSE:
        if (*step == 0) return Send(s, T_CLASS_IF_OUT, 0x23, 0, 0, 0);
        if (*step == 1) {
            SetLines(c, 0);
            return Send(s, T_CLASS_IF_OUT, 0x22, 0, 0, 0);
        }
        return AC_STEP_DONE;
    default:
        return AC_STEP_DONE;
    }
}

/* CP210x SET_FLOW from the block GET_FLOW read (ulControlHandshake,
   ulFlowReplace; Linux cp210x_set_flow_control and _tiocmset_port). */
static int CpSetFlow(AC_CHIP *c, AC_SETUP *s, const AC_U8 *in, int lines)
{
    AC_U32 hs = Le32(in), repl = Le32(in + 4), i;

    Send(s, T_VENDOR_IF_OUT, 0x13, 0, c->Iface, 16);
    for (i = 0; i < 16; i++)
        s->Data[i] = in[i];
    hs &= ~0x03UL;                                      /* DTR mode */
    if (c->Flags & AC_CF_DTR) hs |= 0x01;               /* active */
    repl &= ~0xC0UL;                                    /* RTS mode */
    if (!lines) {
        hs &= ~(0x10UL | 0x20UL | 0x40UL);              /* DSR, DCD handshake; DSR sensitivity */
        /* Auto transmit/receive (XON/XOFF is the VxD's), error character,
           null stripping, break character: none in the chip. */
        repl &= ~0x1FUL;
        if (c->Flags & AC_CF_CTSFLOW) hs |= 0x08; else hs &= ~0x08UL;   /* CTS handshake */
    }
    if (c->Flags & AC_CF_RTS)
        repl |= (c->Flags & AC_CF_RTSFLOW) ? 0x80UL : 0x40UL;    /* flow control or active */
    Put32(s->Data, hs);
    Put32(s->Data + 4, repl);
    return AC_STEP_SEND;
}

static int StepCp(AC_CHIP *c, AC_U32 op, AC_U32 value, const AC_U8 *data, AC_U32 *step, int ok,
                  const AC_U8 *in, AC_U32 inLen, AC_SETUP *s, AC_U16 *state)
{
    AC_LC lc;
    AC_U16 st;

    switch (op) {
    case AC_OP_INIT:
        if (*step == 0)
            return Send(s, T_VENDOR_DEV_IN, 0xFF, 0x370B, c->Iface, 1);    /* GET_PARTNUM */
        if (*step == 1) {
            c->Part = (AC_U8)(ok && inLen >= 1 ? in[0] : 0);
            CpLimits(c);
        }
        return AC_STEP_DONE;
    case AC_OP_OPEN:
        if (*step > 0) return AC_STEP_DONE;
        c->Flags &= (AC_U8)~AC_CF_STATE;
        return Send(s, T_VENDOR_IF_OUT, 0x00, 1, c->Iface, 0);              /* IFC_ENABLE */
    case UCOP_LINE_CODING:
        Lc(data, &lc);
        if (*step == 0) {
            Send(s, T_VENDOR_IF_OUT, 0x1E, 0, c->Iface, 4);                 /* SET_BAUDRATE */
            Put32(s->Data, AcChipRate(c, lc.Rate));
            return AC_STEP_SEND;
        }
        if (*step == 1)
            return Send(s, T_VENDOR_IF_OUT, 0x03,                           /* SET_LINE_CTL */
                        (AC_U16)(((AC_U16)lc.Bits << 8) | ((AC_U16)(lc.Parity <= 4 ? lc.Parity : 0) << 4) |
                                 (lc.Stop == 2 ? 2 : lc.Stop == 1 ? 1 : 0)), c->Iface, 0);
        return AC_STEP_DONE;
    case UCOP_CONTROL:
        if (*step == 0) {
            SetLines(c, value);
            if (!(c->Flags & AC_CF_RTSFLOW) || CpNoFlow(c))
                return Send(s, T_VENDOR_IF_OUT, 0x07, (AC_U16)(0x0300 | LineBits(c)), c->Iface, 0);
            return Send(s, T_VENDOR_IF_IN, 0x14, 0, c->Iface, 16);          /* GET_FLOW */
        }
        if (*step == 1 && (c->Flags & AC_CF_RTSFLOW) && !CpNoFlow(c) && ok && inLen >= 16)
            return CpSetFlow(c, s, in, 1);
        return AC_STEP_DONE;
    case UCOP_FLOW:
        if (*step == 0) {
            SetFlow(c, value);
            if (CpNoFlow(c))
                return AC_STEP_DONE;
            return Send(s, T_VENDOR_IF_IN, 0x14, 0, c->Iface, 16);
        }
        if (*step == 1 && ok && inLen >= 16)
            return CpSetFlow(c, s, in, 0);
        return AC_STEP_DONE;
    case UCOP_BREAK:
        if (*step > 0) return AC_STEP_DONE;
        return Send(s, T_VENDOR_IF_OUT, 0x05, (AC_U16)(value ? 1 : 0), c->Iface, 0);
    case AC_OP_CLOSE:
        /* DTR and RTS inactive and no handshake, through the flow block
           (SET_MHS cannot move an RTS in flow mode), then purge, disable. */
        if (*step == 0) {
            SetLines(c, 0);
            c->Flags &= (AC_U8)~(AC_CF_CTSFLOW | AC_CF_RTSFLOW);
            if (CpNoFlow(c)) {
                *step = 1;
                return Send(s, T_VENDOR_IF_OUT, 0x07, 0x0300, c->Iface, 0);
            }
            return Send(s, T_VENDOR_IF_IN, 0x14, 0, c->Iface, 16);          /* GET_FLOW */
        }
        if (*step == 1 && ok && inLen >= 16)
            return CpSetFlow(c, s, in, 0);
        if (*step <= 2) {
            *step = 2;
            return Send(s, T_VENDOR_IF_OUT, 0x12, 0x000F, c->Iface, 0);   /* PURGE */
        }
        if (*step == 3) return Send(s, T_VENDOR_IF_OUT, 0x00, 0, c->Iface, 0);
        return AC_STEP_DONE;
    case AC_OP_POLL:
        if (*step == 0)
            return Send(s, T_VENDOR_IF_IN, 0x08, 0, c->Iface, 1);            /* GET_MDMSTS */
        if (*step == 1 && ok && inLen >= 1) {
            AC_U8 b = in[0];
            st = (AC_U16)(((b & 0x80) ? AC_SS_DCD : 0) | ((b & 0x20) ? AC_SS_DSR : 0) |
                          ((b & 0x40) ? AC_SS_RING : 0) | ((b & 0x10) ? 0 : AC_SS_NOCTS));
            if (!(c->Flags & AC_CF_STATE) || st != c->LastState) {
                c->Flags |= AC_CF_STATE;
                c->LastState = st;
                *state = st;
                return AC_STEP_DONE | AC_STEP_STATE;
            }
        }
        return AC_STEP_DONE;
    default:
        return AC_STEP_DONE;
    }
}

int AcChipStep(AC_CHIP *c, AC_U32 op, AC_U32 value, const AC_U8 *data,
               AC_U32 *step, int ok, const AC_U8 *in, AC_U32 inLen,
               AC_SETUP *s, AC_U16 *state)
{
    static const AC_U8 zero[8] = { 0 };
    if (data == 0)
        data = zero;
    switch (c->Family) {
    case AC_FAM_FTDI:   return StepFtdi(c, op, value, data, *step, ok, inLen, s);
    case AC_FAM_PL2303: return StepPl(c, op, value, data, step, ok, in, inLen, s);
    case AC_FAM_CP210X: return StepCp(c, op, value, data, step, ok, in, inLen, s, state);
    default:            return StepCdc(c, op, value, data, *step, s);
    }
}

/* ---- status ----------------------------------------------------------------- */

AC_U32 AcChipRx(AC_CHIP *c, AC_U8 *buf, AC_U32 len, AC_U16 *states, int *nstates, int max)
{
    AC_U32 off = 0, put = 0, n, i;
    AC_U16 lines, errors;

    *nstates = 0;
    if (c->Family != AC_FAM_FTDI)
        return len;
    /* Packet by packet, so a change and its undoing within one transfer
       are both passed on, in order. */
    while (off < len) {
        n = len - off;
        if (n > c->InMax)
            n = c->InMax;
        if (n >= 2) {
            AC_U8 s0 = buf[off], s1 = buf[off + 1];
            lines = (AC_U16)(((s0 & 0x80) ? AC_SS_DCD : 0) | ((s0 & 0x20) ? AC_SS_DSR : 0) |
                             ((s0 & 0x40) ? AC_SS_RING : 0) | ((s0 & 0x10) ? 0 : AC_SS_NOCTS));
            errors = 0;
            if (n > 2) {                       /* errors only with data (Linux) */
                if (s1 & 0x02) errors |= AC_SS_OVERRUN;
                if (s1 & 0x04) errors |= AC_SS_PARITY;
                if (s1 & 0x08) errors |= AC_SS_FRAMING;
                if ((s1 & 0x10) && buf[off + n - 1] == 0) errors |= AC_SS_BREAK;
                for (i = 2; i < n; i++)
                    buf[put++] = buf[off + i];
            }
            if (errors || !(c->Flags & AC_CF_STATE) || lines != c->LastState) {
                c->Flags |= AC_CF_STATE;
                c->LastState = lines;
                if (*nstates < max)
                    states[(*nstates)++] = (AC_U16)(lines | errors);
                else                           /* full: the last one carries the newest */
                    states[max - 1] = (AC_U16)(lines | errors | (states[max - 1] & 0x0074));
            }
        }
        off += n;
    }
    return put;
}

AC_U32 AcChipStrip(const AC_CHIP *c, AC_U8 *buf, AC_U32 len)
{
    AC_U32 off = 0, put = 0, n, i;
    if (c->Family != AC_FAM_FTDI)
        return len;
    while (off < len) {
        n = len - off;
        if (n > c->InMax)
            n = c->InMax;
        for (i = 2; i < n; i++)
            buf[put++] = buf[off + i];
        off += n;
    }
    return put;
}

int AcChipNotify(AC_CHIP *c, const AC_U8 *buf, AC_U32 len, AC_U32 *off, AC_U16 *state)
{
    if (c->Family == AC_FAM_PL2303) {
        AC_U8 b;
        if (*off != 0 || len < 9)
            return 0;
        *off = len;
        b = buf[8];
        *state = (AC_U16)((b & 0x7F) | ((b & 0x80) ? 0 : AC_SS_NOCTS));
        return 1;
    }
    if (c->Family != AC_FAM_CDC)
        return 0;
    if (!AcNextNotify(buf, len, off, c->Iface, state))
        return 0;
    *state &= 0x007F;                          /* D0-D6; bit 15 is ours */
    return 1;
}

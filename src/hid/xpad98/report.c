/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * xpad98: options, the synthesized HID report descriptor and the input
 * report (docs/design/gamepad.md sections 6.2-6.4). Item encodings are
 * those of the USB HID 1.11 specification (section 6.2.2) and its usage
 * tables (Generic Desktop 0x01, Button 0x09).
 */
#include "xpcore.h"

void XpOptionsUnset(XP_OPTIONS *opt)
{
    opt->Layout = XP_OPT_UNSET;
    opt->TriggerButtons = XP_OPT_UNSET;
    opt->TriggerThreshold = XP_OPT_UNSET;
    opt->TopUsage = XP_OPT_UNSET;
    opt->GuideButton = XP_OPT_UNSET;
    opt->Deadzone = XP_OPT_UNSET;
    opt->Win9x = 0;
}

void XpOptionsResolve(XP_OPTIONS *opt, XU16 deviceFlags)
{
    if (opt->Layout > 2)
        opt->Layout = 0;                        /* Owner, 2026-10-20 */
    if (opt->TriggerButtons == XP_OPT_UNSET)
        opt->TriggerButtons = (opt->Layout == 1 || (deviceFlags & XP_F_TRIGBTN)) ? 1 : 0;
    else
        opt->TriggerButtons = opt->TriggerButtons ? 1 : 0;
    if (opt->TriggerThreshold == XP_OPT_UNSET)
        opt->TriggerThreshold = 30;     /* XINPUT_GAMEPAD_TRIGGER_THRESHOLD */
    else if (opt->TriggerThreshold > 254)
        opt->TriggerThreshold = 254;
    if (opt->TopUsage != 4 && opt->TopUsage != 5)
        opt->TopUsage = 5;
    opt->Win9x = opt->Win9x ? 1 : 0;
    if (opt->GuideButton == XP_OPT_UNSET)
        opt->GuideButton = 1;
    else
        opt->GuideButton = opt->GuideButton ? 1 : 0;
    if (opt->Deadzone == XP_OPT_UNSET)
        opt->Deadzone = 0;                      /* Owner, 2026-10-20: off */
    else if (opt->Deadzone > 32767)
        opt->Deadzone = 32767;
}

#define U_X  0x30
#define U_Y  0x31
#define U_Z  0x32
#define U_RX 0x33
#define U_RY 0x34
#define U_RZ 0x35
#define U_SLIDER 0x36
#define U_DIAL 0x37

static unsigned AxesOfLayout(const XP_OPTIONS *opt, XU8 *usage, XU8 *src);

/* What each axis slot carries. */
#define A_LX 0
#define A_LY 1
#define A_RX 2
#define A_RY 3
#define A_TRIGS 4       /* both triggers combined, centred */
#define A_LT 5
#define A_RT 6

/* 98 SE's joyhid.vxd refuses a collection with Rx or Ry: joy.cpl reports
 * the pad "not connected correctly" and WinMM reads nothing (2026-10-23
 * legs, a hidusb reference pad with the same descriptor: Rx/Ry fail,
 * Rz/Slider work and land on WinMM R and U). On 9x the right stick takes
 * Rz and Slider (layout 0), or Slider and Dial where Rz is taken
 * (layout 2, unverified). NT keeps Rx/Ry. */
static void Win9xUsages(XU8 *usage, unsigned n)
{
    unsigned i, rz = 0;
    for (i = 0; i < n; i++)
        if (usage[i] == U_RZ)
            rz = 1;
    for (i = 0; i < n; i++) {
        if (usage[i] == U_RX)
            usage[i] = (XU8)(rz ? U_SLIDER : U_RZ);
        else if (usage[i] == U_RY)
            usage[i] = (XU8)(rz ? U_DIAL : U_SLIDER);
    }
}

static unsigned AxesOf(const XP_OPTIONS *opt, XU8 *usage, XU8 *src)
{
    unsigned n = AxesOfLayout(opt, usage, src);
    if (opt->Win9x)
        Win9xUsages(usage, n);
    return n;
}

static unsigned AxesOfLayout(const XP_OPTIONS *opt, XU8 *usage, XU8 *src)
{
    switch (opt->Layout) {
    case 1:     /* "4-axis": X, Y, Z, R as two sticks */
        usage[0] = U_X;  src[0] = A_LX;
        usage[1] = U_Y;  src[1] = A_LY;
        usage[2] = U_Z;  src[2] = A_RX;
        usage[3] = U_RZ; src[3] = A_RY;
        return 4;
    case 2:     /* "separate": triggers each on their own axis */
        usage[0] = U_X;  src[0] = A_LX;
        usage[1] = U_Y;  src[1] = A_LY;
        usage[2] = U_RX; src[2] = A_RX;
        usage[3] = U_RY; src[3] = A_RY;
        usage[4] = U_Z;  src[4] = A_LT;
        usage[5] = U_RZ; src[5] = A_RT;
        return 6;
    default:    /* 0: triggers combined on Z */
        usage[0] = U_X;  src[0] = A_LX;
        usage[1] = U_Y;  src[1] = A_LY;
        usage[2] = U_Z;  src[2] = A_TRIGS;
        usage[3] = U_RX; src[3] = A_RX;
        usage[4] = U_RY; src[4] = A_RY;
        return 5;
    }
}

void XpLayoutOf(const XP_OPTIONS *opt, XP_LAYOUT *lay)
{
    XU8 src[6];
    lay->NumAxes = AxesOf(opt, lay->AxisUsage, src);
    lay->NumButtons = 10 + (opt->GuideButton ? 1 : 0) + (opt->TriggerButtons ? 2 : 0);
    lay->ReportLen = 2 * lay->NumAxes + 1 + 2;
}

typedef struct BUF {
    XU8 *p;
    unsigned n, cap;
    int over;
} BUF;

static void Put(BUF *b, XU8 v)
{
    if (b->n < b->cap)
        b->p[b->n] = v;
    else
        b->over = 1;
    b->n++;
}

static void Put2(BUF *b, XU8 a, XU8 v)
{
    Put(b, a);
    Put(b, v);
}

unsigned XpBuildReportDescriptor(const XP_OPTIONS *opt, unsigned collections,
                                 XU8 *buf, unsigned cap)
{
    BUF b;
    XP_LAYOUT lay;
    unsigned c, i;

    b.p = buf; b.n = 0; b.cap = cap; b.over = 0;
    if (collections < 1 || collections > XP_MAX_CHAN)
        return 0;
    XpLayoutOf(opt, &lay);
    for (c = 0; c < collections; c++) {
        Put2(&b, 0x05, 0x01);                   /* Usage Page (Generic Desktop) */
        Put2(&b, 0x09, (XU8)opt->TopUsage);     /* Usage (Game Pad / Joystick) */
        Put2(&b, 0xA1, 0x01);                   /* Collection (Application) */
        if (collections > 1)
            Put2(&b, 0x85, (XU8)(c + 1));       /* Report ID */

        /* Axes: 0..32767 in 16-bit fields. 98 SE's HID parser reads a
         * 16-bit field as signed, so 0..65535 put every value from 32768
         * up (the centre too) out of range (HIDTEST on the 2026-10-23 leg:
         * HidP_GetScaledUsageValue C0110005); and VJoyD maps a signed
         * -32768..32767 range wrongly (centre reads 0, negatives clamp
         * to 0). 0..32767 reads the same signed or unsigned; WinMM maps
         * it to 0..65535 on every system. */
        Put2(&b, 0x15, 0x00);                   /* Logical Minimum (0) */
        Put(&b, 0x26); Put(&b, 0xFF); Put(&b, 0x7F);  /* Logical Maximum (32767) */
        Put2(&b, 0x75, 0x10);                   /* Report Size (16) */
        Put2(&b, 0x95, (XU8)lay.NumAxes);       /* Report Count */
        for (i = 0; i < lay.NumAxes; i++)
            Put2(&b, 0x09, lay.AxisUsage[i]);
        Put2(&b, 0x81, 0x02);                   /* Input (Data,Var,Abs) */

        /* D-pad: one hat, 8 directions, Null State when centred. */
        Put2(&b, 0x09, 0x39);                   /* Usage (Hat switch) */
        Put2(&b, 0x15, 0x00);                   /* Logical Minimum (0) */
        Put2(&b, 0x25, 0x07);                   /* Logical Maximum (7) */
        Put2(&b, 0x35, 0x00);                   /* Physical Minimum (0) */
        Put(&b, 0x46); Put(&b, 0x3B); Put(&b, 0x01);  /* Physical Maximum (315) */
        Put2(&b, 0x65, 0x14);                   /* Unit (English Rotation, degrees) */
        Put2(&b, 0x75, 0x04);                   /* Report Size (4) */
        Put2(&b, 0x95, 0x01);                   /* Report Count (1) */
        Put2(&b, 0x81, 0x42);                   /* Input (Data,Var,Abs,Null) */
        Put2(&b, 0x65, 0x00);                   /* Unit (None) */
        Put2(&b, 0x45, 0x00);                   /* Physical Maximum (0) */
        Put2(&b, 0x81, 0x03);                   /* Input (Const,Var,Abs): 4 bits */

        /* Buttons 1..N, padded to 16 bits. */
        Put2(&b, 0x05, 0x09);                   /* Usage Page (Button) */
        Put2(&b, 0x19, 0x01);                   /* Usage Minimum (1) */
        Put2(&b, 0x29, (XU8)lay.NumButtons);    /* Usage Maximum (N) */
        Put2(&b, 0x15, 0x00);                   /* Logical Minimum (0) */
        Put2(&b, 0x25, 0x01);                   /* Logical Maximum (1) */
        Put2(&b, 0x75, 0x01);                   /* Report Size (1) */
        Put2(&b, 0x95, (XU8)lay.NumButtons);    /* Report Count (N) */
        Put2(&b, 0x81, 0x02);                   /* Input (Data,Var,Abs) */
        if (lay.NumButtons < 16) {
            Put2(&b, 0x75, (XU8)(16 - lay.NumButtons));
            Put2(&b, 0x95, 0x01);
            Put2(&b, 0x81, 0x03);               /* Input (Const): padding */
        }
        Put(&b, 0xC0);                          /* End Collection */
    }
    return b.over ? 0 : b.n;
}

XU8 XpHatOf(XU8 dpad)
{
    int dx = ((dpad & XP_D_RIGHT) ? 1 : 0) - ((dpad & XP_D_LEFT) ? 1 : 0);
    int dy = ((dpad & XP_D_DOWN) ? 1 : 0) - ((dpad & XP_D_UP) ? 1 : 0);
    if (dy < 0) return (XU8)(dx < 0 ? 7 : dx > 0 ? 1 : 0);
    if (dy > 0) return (XU8)(dx < 0 ? 5 : dx > 0 ? 3 : 4);
    if (dx > 0) return 2;
    if (dx < 0) return 6;
    return 8;   /* out of 0-7: the Null State, centred */
}

static XU16 Stick(XS16 v)
{
    return (XU16)((long)v + 32768L);
}

/* Separate trigger: 0..Max to 0..65535. */
static XU16 Trig(XU16 v, XU16 max)
{
    if (max == 0)
        return 0;
    if (v >= max)
        return 65535;
    return (XU16)(((XU32)v * 65535UL) / max);
}

/* Combined: 32768 + (LT - RT) * 32768 / (Max + 1), LT positive: * 128
 * for 8-bit triggers, * 32 for 10-bit ones (design 6.3). */
static XU16 Combined(XU16 lt, XU16 rt, XU16 max)
{
    long v = (((long)lt - (long)rt) * 32768L) / ((long)max + 1);
    v += 32768L;
    if (v < 0) v = 0;
    if (v > 65535L) v = 65535L;
    return (XU16)v;
}

static void Deadzone(XS16 *x, XS16 *y, XU32 dz)
{
    XU32 ax, ay;
    if (dz == 0)
        return;
    ax = (XU32)(*x < 0 ? -(long)*x : *x);
    ay = (XU32)(*y < 0 ? -(long)*y : *y);
    if (ax * ax + ay * ay < dz * dz) {
        *x = 0;
        *y = 0;
    }
}

static int TrigPressed(XU16 v, XU16 max, XU32 threshold)
{
    if (max == 0)
        return 0;
    return ((XU32)v * 255UL) / max > threshold;
}

unsigned XpBuildReport(const XP_OPTIONS *opt, const XP_PAD *pad,
                       XU8 reportId, XU8 *buf, unsigned cap)
{
    XP_LAYOUT lay;
    XU8 usage[6], src[6];
    unsigned n = 0, i, bit = 0;
    XS16 lx = pad->LX, ly = pad->LY, rx = pad->RX, ry = pad->RY;
    XU16 buttons = 0;
    static const XU16 order[10] = {
        XP_B_A, XP_B_B, XP_B_X, XP_B_Y, XP_B_LB, XP_B_RB,
        XP_B_BACK, XP_B_START, XP_B_LS, XP_B_RS
    };

    XpLayoutOf(opt, &lay);
    AxesOf(opt, usage, src);
    if (cap < lay.ReportLen + (reportId ? 1u : 0u))
        return 0;
    Deadzone(&lx, &ly, opt->Deadzone);
    Deadzone(&rx, &ry, opt->Deadzone);

    if (reportId)
        buf[n++] = reportId;
    for (i = 0; i < lay.NumAxes; i++) {
        XU16 v;
        switch (src[i]) {
        case A_LX: v = Stick(lx); break;
        case A_LY: v = Stick(ly); break;
        case A_RX: v = Stick(rx); break;
        case A_RY: v = Stick(ry); break;
        case A_TRIGS: v = Combined(pad->LT, pad->RT, pad->TriggerMax); break;
        case A_LT: v = Trig(pad->LT, pad->TriggerMax); break;
        default: v = Trig(pad->RT, pad->TriggerMax); break;
        }
        v >>= 1;        /* 0..65535 (centre 32768) as 0..32767 (centre 16384) */
        buf[n++] = (XU8)v;
        buf[n++] = (XU8)(v >> 8);
    }
    buf[n++] = XpHatOf(pad->Dpad);      /* high nibble: padding */

    for (i = 0; i < 10; i++, bit++)
        if (pad->Buttons & order[i])
            buttons |= (XU16)(1u << bit);
    if (opt->GuideButton) {
        if (pad->Buttons & XP_B_GUIDE)
            buttons |= (XU16)(1u << bit);
        bit++;
    }
    if (opt->TriggerButtons) {
        if (TrigPressed(pad->LT, pad->TriggerMax, opt->TriggerThreshold))
            buttons |= (XU16)(1u << bit);
        bit++;
        if (TrigPressed(pad->RT, pad->TriggerMax, opt->TriggerThreshold))
            buttons |= (XU16)(1u << bit);
        bit++;
    }
    buf[n++] = (XU8)buttons;
    buf[n++] = (XU8)(buttons >> 8);
    return n;
}

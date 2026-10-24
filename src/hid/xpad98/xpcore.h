/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * xpad98 portable cores: the normalised pad state, the protocol parsers
 * (xusb.c, gip.c), the device table (devtab.c), the configuration
 * descriptor walk (xpcfg.c) and the HID report synthesis (report.c).
 * Plain C with no kernel or C library dependency, so the same files
 * build into xpad98.sys and into the host test (xptest.c).
 * docs/design/gamepad.md sections 5 and 6.
 */
#ifndef XPCORE_H
#define XPCORE_H

typedef unsigned char XU8;
typedef unsigned short XU16;
typedef unsigned long XU32;
typedef short XS16;
typedef long XS32;

#define XP_PKT_LEN 64           /* Linux XPAD_PKT_LEN: IN and OUT buffers */
#define XP_MAX_CHAN 4           /* receiver slots; a wired pad uses one */

/* Families (design section 1). */
#define XP_FAM_NONE  0
#define XP_FAM_X360  1          /* XUSB wired, ff/5d/01 */
#define XP_FAM_X360W 2          /* 360 wireless receiver, ff/5d/81 per slot */
#define XP_FAM_GIP   3          /* Xbox One/Series, ff/47/d0 interface 0 */

/* Device table flags. */
#define XP_F_TRIGBTN   0x0001   /* Linux MAP_TRIGGERS_TO_BUTTONS: digital triggers */
#define XP_F_DPADBTN   0x0002   /* Linux MAP_DPAD_TO_BUTTONS (dance pads, drums) */
#define XP_F_DELAYINIT 0x0004   /* Linux FLAG_DELAY_INIT: GIP init after announce */
#define XP_F_NOINF     0x0008   /* composite keyboards/keypads: not in any INF */

/* Normalised buttons (XP_PAD.Buttons). */
#define XP_B_A      0x0001
#define XP_B_B      0x0002
#define XP_B_X      0x0004
#define XP_B_Y      0x0008
#define XP_B_LB     0x0010
#define XP_B_RB     0x0020
#define XP_B_BACK   0x0040
#define XP_B_START  0x0080
#define XP_B_LS     0x0100
#define XP_B_RS     0x0200
#define XP_B_GUIDE  0x0400

/* D-pad bits (XP_PAD.Dpad), as XUSB data[2] bits 0-3. */
#define XP_D_UP     0x01
#define XP_D_DOWN   0x02
#define XP_D_LEFT   0x04
#define XP_D_RIGHT  0x08

typedef struct XP_PAD {
    XS16 LX, LY, RX, RY;        /* signed; Y already inverted: up is negative */
    XU16 LT, RT;                /* 0..TriggerMax */
    XU16 TriggerMax;            /* 255 (XUSB) or 1023 (GIP) */
    XU16 Buttons;               /* XP_B_* */
    XU8 Dpad;                   /* XP_D_* */
    XU8 Present;                /* receiver slot: a pad is connected */
} XP_PAD;

/* Parser events. */
#define XP_EV_DATA       0x01   /* pad state was rewritten */
#define XP_EV_CONNECT    0x02   /* receiver: a pad connected to this slot */
#define XP_EV_DISCONNECT 0x04   /* receiver: the slot's pad went away */
#define XP_EV_ACK        0x08   /* GIP: send the acknowledgement built in Ack */
#define XP_EV_ANNOUNCE   0x10   /* GIP: the pad announced itself */

void XpPadReset(XP_PAD *pad, XU16 triggerMax);
int XpPadEqual(const XP_PAD *a, const XP_PAD *b);

/* ---- xusb.c: Xbox 360 wired and wireless (adapted from Linux xpad.c) ---- */

/* The start request every XUSB pad gets (xpad_start_input): vendor,
 * device-to-host, interface; bRequest 1, wValue 0x0100, wIndex 0, 20 bytes,
 * result ignored. */
#define XP_X360_MAGIC_TYPE   0xC1
#define XP_X360_MAGIC_REQ    0x01
#define XP_X360_MAGIC_VALUE  0x0100
#define XP_X360_MAGIC_INDEX  0x0000
#define XP_X360_MAGIC_LEN    20

unsigned XpX360Parse(const XU8 *data, unsigned len, XP_PAD *pad);
unsigned XpX360wParse(const XU8 *data, unsigned len, XP_PAD *pad);
unsigned XpX360Led(XU8 *out, unsigned command);
unsigned XpX360wLed(XU8 *out, unsigned command);
unsigned XpX360wPresence(XU8 *out);
unsigned XpX360wPowerOff(XU8 *out);
unsigned XpX360Rumble(XU8 *out, XU8 strong, XU8 weak);
unsigned XpX360wRumble(XU8 *out, XU8 strong, XU8 weak);
unsigned XpLedForPlayer(unsigned player);   /* (n % 4) + 2: blink, then on */

/* ---- gip.c: Xbox One and Series (adapted from Linux xpad.c) ---- */

/* Copies init packet number *cursor and on, the next one that applies to
 * vid:pid, into out (sequence byte left 0 for the caller); advances
 * *cursor past it. Returns its length, 0 when the sequence is done. */
unsigned XpGipNextInit(XU16 vid, XU16 pid, unsigned *cursor, XU8 *out);
unsigned XpGipParse(const XU8 *data, unsigned len, XP_PAD *pad,
                    XU8 *ack, unsigned *ackLen);

/* ---- devtab.c: ids (adapted from Linux xpad.c xpad_device[]) ---- */

typedef struct XP_DEVICE {
    XU16 Vid, Pid;
    XU8 Family;
    XU16 Flags;
    const char *Name;
} XP_DEVICE;

const XP_DEVICE *XpFindDevice(XU16 vid, XU16 pid);
unsigned XpDeviceCount(void);
const XP_DEVICE *XpDeviceAt(unsigned i);
int XpIsX360c(XU16 vid, XU16 pid);          /* one of Morii's x360c's 25 */
unsigned XpX360cCount(void);
void XpX360cAt(unsigned i, XU16 *vid, XU16 *pid);
const char *XpFamilyName(unsigned family);

/* ---- xpcfg.c: configuration descriptor walk ---- */

#define XP_MAX_IF 16
#define XP_MAX_EP 8

typedef struct XP_IFACE {
    XU8 Number, Class, SubClass, Protocol, NumEndpoints;
    XU8 EpAddr[XP_MAX_EP];
    XU8 EpAttr[XP_MAX_EP];
    XU16 EpMaxPacket[XP_MAX_EP];
} XP_IFACE;

typedef struct XP_CONFIG {
    XU8 ConfigValue;
    unsigned NumInterfaces;     /* alternate setting 0 of each */
    XP_IFACE Iface[XP_MAX_IF];
    int AllVendor;              /* every interface is class 0xFF */
    unsigned Family;            /* from the interface triples */
    unsigned NumChannels;
    XU8 ChanIface[XP_MAX_CHAN]; /* index into Iface[] */
    XU8 ChanIn[XP_MAX_CHAN], ChanOut[XP_MAX_CHAN];  /* endpoint addresses */
} XP_CONFIG;

/* 0 on success; otherwise a negative XP_CFG_E* code. */
#define XP_CFG_EBAD     (-1)    /* malformed descriptor */
#define XP_CFG_ETOOMANY (-2)    /* more interfaces/endpoints than handled */
#define XP_CFG_ENOPAD   (-3)    /* no interface this driver drives */
int XpParseConfig(const XU8 *cfg, unsigned len, XP_CONFIG *out);

/* The interfaces a SELECT_CONFIGURATION lists, as indices into Iface[]:
 * every one (drivenOnly 0), or only those a channel uses (drivenOnly 1).
 * The second is the fallback when the first is refused: xHCI98 3.1.0.0
 * opens at most four endpoints per device and refuses the wired 360
 * pad's seven with USBD_STATUS_NO_MEMORY (2026-10-23 98 SE leg); the
 * headset, plug-in and security interfaces are never used here. Returns
 * the count and the endpoints those interfaces hold in *endpoints. */
unsigned XpSelectSet(const XP_CONFIG *k, int drivenOnly, XU8 *idx, unsigned *endpoints);

/* ---- report.c: options, HID report descriptor and reports ---- */

typedef struct XP_OPTIONS {
    XU32 Layout;            /* 0 combined triggers, 1 four-axis, 2 separate */
    XU32 TriggerButtons;    /* 0/1 */
    XU32 TriggerThreshold;  /* 0-255, on the 8-bit scale */
    XU32 TopUsage;          /* 4 joystick, 5 game pad */
    XU32 GuideButton;       /* 0/1 */
    XU32 Deadzone;          /* 0-32767, radial, per stick */
    XU32 Win9x;             /* set by the driver, not the registry: 98 SE / ME
                             * joyhid usages for the right stick (6.2) */
} XP_OPTIONS;

#define XP_OPT_UNSET 0xFFFFFFFFUL

/* Fills defaults for fields equal to XP_OPT_UNSET and clamps the rest. */
void XpOptionsResolve(XP_OPTIONS *opt, XU16 deviceFlags);
void XpOptionsUnset(XP_OPTIONS *opt);

typedef struct XP_LAYOUT {
    unsigned NumAxes;
    XU8 AxisUsage[6];       /* HID Generic Desktop usages */
    unsigned NumButtons;
    unsigned ReportLen;     /* bytes, without a report id */
} XP_LAYOUT;

void XpLayoutOf(const XP_OPTIONS *opt, XP_LAYOUT *lay);
/* collections 1: no report id; 2-4: ids 1..n, one top-level each. */
unsigned XpBuildReportDescriptor(const XP_OPTIONS *opt, unsigned collections,
                                 XU8 *buf, unsigned cap);
unsigned XpBuildReport(const XP_OPTIONS *opt, const XP_PAD *pad,
                       XU8 reportId, XU8 *buf, unsigned cap);
XU8 XpHatOf(XU8 dpad);      /* 0-7, or 8 for centred */

#define XP_REPORT_DESC_MAX 512
#define XP_REPORT_MAX 16

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original wire codec from UVC 1.1 Table 4-47 and 1.5 Table 4-75.
 * No USB requests, allocation, negotiation policy or packed structures.
 */
#ifndef UVCCTRL_H
#define UVCCTRL_H
typedef struct UVC_CTRL {
    unsigned int Hint, Format, Frame;
    unsigned long Interval;
    unsigned int KeyRate, PFrameRate, Quality, Window, Delay;
    unsigned long FrameBytes, PayloadBytes, Clock;
    unsigned int Framing, Preferred, Minimum, Maximum;
    /* 1.5 fields34..47 retained verbatim; typed accessor below. No encoder policy. */
    unsigned char Tail[14];
} UVC_CTRL;

/* UVC1.5 Table4-75 fields34..47. Values are wire fields, not a promise
 * that an encoder, consumer or negotiated payload supports each setting. */
typedef struct UVC_CTRL15 {
    unsigned int Usage, BitDepthLuma, Settings, MaxReferencesPlus1;
    unsigned int RateControlModes;
    unsigned int Layout[4];
} UVC_CTRL15;

/* Only explicitly supported revisions 0x0100/0x0110/0x0150. */
unsigned int UvcCtrlSize(unsigned int revision);
/* Exact wire length required. Decode zeroes output on failure; encode
 * validates representable field widths before touching destination.
 * Do not alias wire storage with the structure. */
int UvcCtrlDecode(unsigned int revision, const unsigned char *wire,
    unsigned int bytes, UVC_CTRL *control);
int UvcCtrlEncode(unsigned int revision, const UVC_CTRL *control,
    unsigned char *wire, unsigned int bytes);
/* Typed access to the preserved1.5 tail. Get zeroes output on failure;
 * Set validates all widths before changing any tail byte. Older revisions
 * are rejected. Inputs/outputs must be separate, nonaliasing storage. */
int UvcCtrl15Get(unsigned int revision, const UVC_CTRL *control,
    UVC_CTRL15 *fields);
int UvcCtrl15Set(unsigned int revision, UVC_CTRL *control,
    const UVC_CTRL15 *fields);
#endif

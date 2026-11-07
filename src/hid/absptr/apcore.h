/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * absptr portable core (docs/design/absptr.md sections 5 and 6): the HID
 * report descriptor parser, the choice of the pointer source, the
 * translation of a device report into the pointer state, the Wacom
 * PenPartner front end, and the report descriptor and report presented to
 * Windows. Plain C89 with no kernel or C library dependency, so the same
 * file builds into absptr.sys and into the host test (aptest.c).
 */
#ifndef APCORE_H
#define APCORE_H

typedef unsigned char AU8;
typedef unsigned short AU16;
typedef unsigned long AU32;
typedef long AS32;

/* Sources (design 5.1, 5.2). */
#define AP_KIND_NONE       0
#define AP_KIND_TOUCH      1    /* Digitizer page, Touch Screen */
#define AP_KIND_PEN        2    /* Digitizer page, Pen or Digitizer */
#define AP_KIND_MOUSE      3    /* Generic Desktop Mouse/Pointer, absolute X/Y */
#define AP_KIND_PENPARTNER 4    /* Wacom PenPartner, its tablet mode */

/* Parser results. */
#define AP_OK              0
#define AP_E_SHORT        -1    /* an item runs past the end */
#define AP_E_NESTING      -2    /* collections or Push deeper than the limits */
#define AP_E_BALANCE      -3    /* End Collection or Pop without its opener */
#define AP_E_LIMIT        -4    /* descriptor, usages or report beyond the limits */
#define AP_E_NOSOURCE     -5    /* no collection absptr can drive */

#define AP_DESC_MAX     4096    /* report descriptor bytes accepted */
#define AP_DEPTH_MAX      64    /* collection nesting and global stack */
#define AP_USAGES_MAX    256    /* local usages (ranges) per main item */
#define AP_REPORT_MAX    256    /* report bytes, report id included */
#define AP_SCALE_MAX   32767    /* the presented X/Y range, design 6 */

/* The PenPartner's range (Linux wacom_features_0x00). */
#define AP_PP_XMAX      5040
#define AP_PP_YMAX      3780

/* Pointer buttons (AP_STATE.Buttons and byte 0 of the report). */
#define AP_BTN_LEFT   0x01
#define AP_BTN_RIGHT  0x02
#define AP_BTN_MIDDLE 0x04

/* One field of an input or feature report. Present is 0 when absent. */
typedef struct AP_FIELD {
    AU8 Present;
    AU8 Signed;                 /* Logical Minimum negative: two's complement */
    AU8 Size;                   /* bits, 1..32 */
    AU8 ReportId;               /* 0 when the descriptor has no report ids */
    AU16 BitOffset;             /* from the first bit after the report id */
    AS32 LogMin, LogMax;
} AP_FIELD;

#define AP_SLOTS 10             /* contact slots of a multi-touch report (5.3) */

/* One contact slot (a Finger collection) of a multi-touch screen. */
typedef struct AP_SLOT {
    AP_FIELD Tip, Id, X, Y, Confidence;
} AP_SLOT;

typedef struct AP_SOURCE {
    unsigned Kind;
    AU8 UsesIds;                /* the descriptor has Report ID items */
    AU8 ReportId;               /* the input report carrying X and Y */
    AU16 ReportBytes;           /* that report's length, id included */
    AP_FIELD X, Y, Tip, InRange, Barrel, Barrel2, Eraser, Confidence;
    AP_FIELD Button[3];         /* AP_KIND_MOUSE: Button page usages 1-3 */
    /* Multi-touch (design 5.3): with two or more slots, or a Contact
     * Identifier, the pointer follows one contact by its identifier. */
    unsigned NumSlots;
    AP_SLOT Slot[AP_SLOTS];
    AP_FIELD Count;             /* Contact Count (Digitizer 54h) */
    /* The Device Mode feature (Digitizer 52h in a Device Configuration
     * collection, 0Eh), design 5.1. */
    AP_FIELD DevMode;
    AU8 DevModeId;
    AU16 DevModeBytes;          /* the feature report's length, id included */
    /* Every input report of the descriptor, chosen or not, so a transfer
     * can be split at report boundaries and read in whole (HID 1.11 8.4). */
    AU16 MaxInBytes;            /* the longest input report, id included */
    AU16 InBytes[256];          /* each input report's length by id, 0 if none */
} AP_SOURCE;

/* The pointer state presented to Windows, and (multi-touch) the contact
 * it follows. Only X, Y and Buttons are reported. */
typedef struct AP_STATE {
    AU16 X, Y;                  /* 0..AP_SCALE_MAX */
    AU8 Buttons;                /* AP_BTN_* */
    AU8 Tracking;               /* a contact is followed */
    AU8 Seen;                   /* it was in the current frame */
    AU16 Remaining;             /* hybrid mode: contacts still to come in the frame */
    AU32 TrackId;
} AP_STATE;

/* Parses a report descriptor and chooses the source (design 5.1). work
 * must point to ApWorkSize() bytes; it is only used during the call.
 * Returns AP_OK with *src filled, or an AP_E_* code. */
unsigned ApWorkSize(void);
int ApParse(const AU8 *desc, unsigned len, void *work, AP_SOURCE *src);

/* The PenPartner source (design 5.2): no descriptor parse. */
void ApPenPartnerSource(AP_SOURCE *src);

/* The length of the report that starts at rep (avail bytes left in the
 * transfer): its descriptor length, which may be more than avail (a
 * short report, or one cut off at the end of the transfer: the caller
 * decides); avail when the source has no descriptor (the PenPartner); 0
 * when it is not an input report of the descriptor. */
unsigned ApReportLength(const AP_SOURCE *src, const AU8 *rep, unsigned avail);

/* Translates one input report into *st. Returns 1 when *st changed, 0
 * when it did not or the report is not the source's. */
int ApTranslate(const AP_SOURCE *src, const AU8 *rep, unsigned len, AP_STATE *st);

/* The PenPartner mode switch (design 5.2): the SET_REPORT data (feature
 * report 2, "02 02") and the check of the GET_REPORT answer. */
unsigned ApPenPartnerModeData(AU8 *out, unsigned cap);
int ApPenPartnerModeOk(const AU8 *answer, unsigned len);

/* The Device Mode feature (design 5.1): the value to set (2, multi-input,
 * when the field's range allows it, else 1, single input; 0 when the
 * source has no usable field), and, applied to the feature report read
 * with GET_REPORT, that value written into its field with every other
 * byte kept. Returns 1 when buf (len bytes, id first when the descriptor
 * uses ids) was changed. */
unsigned ApDevModeValue(const AP_SOURCE *src);
int ApDevModeApply(const AP_SOURCE *src, AU8 *buf, unsigned len);

/* The report descriptor and the 6-byte report presented (design 6). */
unsigned ApReportDescriptor(AU8 *out, unsigned cap);
#define AP_REPORT_LEN 6
unsigned ApBuildReport(const AP_STATE *st, AU8 *out, unsigned cap);

/* Initial state: centre, no buttons. */
void ApStateReset(AP_STATE *st);

#endif

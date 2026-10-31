/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * prncore.h - the pure parts of usbprn (docs/design/printing.md): the
 * printer interface and alternate choice, IEEE 1284 device id parsing,
 * port names, write chunking and the status-to-error mapping. No kernel
 * or Win32 headers: compiled into USBPRN.SYS, USBPRNMN.DLL and the host
 * tests alike.
 *
 * The alternate-setting order, the bidirectional quirk rows and the device
 * id length clamp follow Linux drivers/usb/class/usblp.c, v6.12
 * (usblp_select_alts(), quirk_printers[], usblp_cache_device_id_string()),
 * GPL-2.0+, as NOTICE.md records.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 * Copyright (c) 1999 Michael Gee <michael@linuxspecific.com>
 * Copyright (c) 1999 Pavel Machek <pavel@ucw.cz>
 * Copyright (c) 2000 Randy Dunlap <rdunlap@xenotime.net>
 * Copyright (c) 2000 Vojtech Pavlik <vojtech@suse.cz>
 * Copyright (c) 2001 Pete Zaitcev <zaitcev@redhat.com>
 * Copyright (c) 2001 David Paschal <paschal@rcsis.com>
 * Copyright (c) 2006 Oliver Neukum <oliver@neukum.name>
 */
#ifndef PRNCORE_H
#define PRNCORE_H

typedef unsigned char  PC_U8;
typedef unsigned short PC_U16;
typedef unsigned long  PC_U32;

/* ---- interface and alternate choice (design 5.1) ---------------------- */

#define PC_PROTO_UNI    1   /* 07/01/01: bulk OUT */
#define PC_PROTO_BIDI   2   /* 07/01/02: bulk OUT and bulk IN */
#define PC_PROTO_1284_4 3   /* 07/01/03: IEEE 1284.4; started, data refused */

typedef struct PC_ALT {
    int     Found;          /* 1 when a usable alternate was chosen */
    PC_U8   Interface;      /* bInterfaceNumber */
    PC_U8   Alternate;      /* bAlternateSetting */
    PC_U8   Protocol;       /* PC_PROTO_* */
    PC_U8   OutEp, InEp;    /* endpoint addresses; InEp 0 when none */
    PC_U16  OutMax, InMax;  /* wMaxPacketSize */
    PC_U32  IfOffset;       /* offset of the chosen interface descriptor */
    PC_U8   NumEps;         /* its bNumEndpoints (checked against its endpoint descriptors) */
} PC_ALT;

/*
 * Walks a configuration descriptor (cfg, len bytes as read) and picks the
 * printer interface and alternate: the first interface that has a 07/01
 * alternate; among its alternates protocol 02 (with bulk OUT and bulk IN)
 * first, then 01 (bulk OUT), then 03. The bidirectional quirk (flags
 * PC_Q_NO_BIDI) treats 02 as 01 by dropping the IN endpoint. Returns 1 when
 * an alternate was chosen, 0 when the descriptor holds no usable printer
 * interface, -1 when the descriptor is malformed (including a printer
 * alternate whose endpoint descriptors differ from its bNumEndpoints).
 */
#define PC_Q_NO_BIDI    0x01

/* Quirk flags for a device (Linux usblp.c quirk_printers[], the
   USBLP_QUIRK_BIDIR rows): PC_Q_NO_BIDI or 0. */
PC_U32 PcQuirks(PC_U16 vid, PC_U16 pid);
int PcSelectAlt(const PC_U8 *cfg, PC_U32 len, PC_U32 flags, PC_ALT *out);

/* ---- IEEE 1284 device id (design 5.1, 6) ------------------------------ */

#define PC_ID_MAX       1023    /* id bytes kept, after the 2-byte length */

/*
 * The id length from a GET_DEVICE_ID answer of got bytes: the big-endian
 * length field counts itself; it is clamped to 2..1023+2 and to got. Returns
 * the number of id bytes (after the length field), 0 when none.
 */
PC_U32 PcIdLength(const PC_U8 *buf, PC_U32 got);

/*
 * Finds a key's value in an id string ("KEY:value;KEY:value;"). Keys are
 * matched case-insensitively against the long and short names (MFG and
 * MANUFACTURER, MDL and MODEL, CMD and COMMAND SET, CLS and CLASS, DES and
 * DESCRIPTION). Copies at most cap-1 bytes of the value into out, ends it
 * with a NUL, strips surrounding blanks, and returns its length; 0 and an
 * empty string when the key is absent.
 */
#define PC_KEY_MFG  0
#define PC_KEY_MDL  1
#define PC_KEY_CMD  2
#define PC_KEY_CLS  3
#define PC_KEY_DES  4
#define PC_KEY_CID  5
PC_U32 PcIdValue(const char *id, PC_U32 len, int key, char *out, PC_U32 cap);

/*
 * A one-line description for the user from an id: DES when present, else
 * "MFG MDL", else "USB printer". Returns its length (cap-1 at most).
 */
PC_U32 PcIdDescribe(const char *id, PC_U32 len, char *out, PC_U32 cap);

/* ---- port names (design 4.2) ------------------------------------------ */

#define PC_PORT_MAX     64      /* USB001 .. USB064 */

/* "USB001" for n = 1; out holds at least 8 bytes. Returns 0 for a bad n. */
int PcPortName(PC_U32 n, char *out);

/* n from "USBnnn" (exactly three digits, 1..PC_PORT_MAX; a trailing ':' is
   accepted), 0 when the name is not one of ours. */
PC_U32 PcParsePortName(const char *s, PC_U32 len);

/* "\\.\USBPRNn" for CreateFile; out holds at least 16 bytes. */
int PcDevicePath(PC_U32 n, char *out);

/* The lowest number in 1..PC_PORT_MAX whose bit is clear in used (bit n of
   used[n >> 3]), 0 when all are taken. */
PC_U32 PcLowestFree(const PC_U8 *used /* PC_PORT_MAX / 8 + 1 bytes */);

/* ---- writes (design 5.1) ---------------------------------------------- */

/* Bytes of the next bulk OUT transfer: min(left, max), max 0 treated as
   4096. */
PC_U32 PcChunk(PC_U32 left, PC_U32 max);

/* ---- status (design 7.1) ---------------------------------------------- */

#define PC_ST_PAPER_EMPTY   0x20
#define PC_ST_SELECT        0x10
#define PC_ST_NOT_ERROR     0x08

/* What a stalled write reports, from the latest status byte. */
#define PC_ERR_NONE         0   /* no stall */
#define PC_ERR_PAPER        1   /* ERROR_OUT_OF_PAPER */
#define PC_ERR_OFFLINE      2   /* ERROR_NOT_READY */
#define PC_ERR_FAULT        3   /* ERROR_GEN_FAILURE */
#define PC_ERR_TIMEOUT      4   /* ERROR_TIMEOUT: status benign or unknown */
#define PC_ERR_GONE         5   /* ERROR_DEVICE_NOT_CONNECTED */

/* valid: whether status was read at all. Order: paper, then error, then
   offline (a printer out of paper is usually also not selected). */
int PcStatusError(int valid, PC_U8 status);

#endif

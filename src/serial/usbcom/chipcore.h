/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chipcore.h - the operating-system-independent USB side of usbcom's ports:
 * the chip modules (docs/design/serial.md 6.3) and CDC-ACM's own control
 * requests, as one step generator the SYS runs one control transfer at a
 * time, plus the chips' receive framing and status decoding.
 *
 * Original code. The protocol facts (ids, request codes, register values,
 * bit positions, rate encodings and limits) are those section 6.3 lists
 * from Linux v6.12 drivers/usb/serial (facts only; no code is taken from
 * its GPL-2.0-only files). The FTDI divisors are in ftdibaud.c, adapted
 * from ftdi_sio.c (GPL-2.0-or-later) with its notices.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#ifndef CHIPCORE_H
#define CHIPCORE_H

#include "acmcore.h"
#include "ucif.h"

/* Families */
#define AC_FAM_CDC     0
#define AC_FAM_FTDI    1
#define AC_FAM_PL2303  2
#define AC_FAM_CP210X  3

/* Parts (AC_CHIP.Part) */
#define AC_FT_AM     1
#define AC_FT_BM     2
#define AC_FT_2232C  3
#define AC_FT_R      4
#define AC_FT_2232H  5
#define AC_FT_4232H  6
#define AC_FT_232H   7
#define AC_FT_X      8

#define AC_PL_H      1
#define AC_PL_HX     2
#define AC_PL_TA     3
#define AC_PL_TB     4
#define AC_PL_HXD    5
#define AC_PL_G      6
#define AC_PL_PROBE  7       /* TA/TB or G: decided by the 0x8080 read */

/* CP210x: Part is the part number GET_PARTNUM returns (0 = not read). */
#define AC_CP_2101   0x01
#define AC_CP_2102   0x02
#define AC_CP_2103   0x03
#define AC_CP_2104   0x04
#define AC_CP_2105   0x05
#define AC_CP_2108   0x08
#define AC_CP_2102N_28 0x20
#define AC_CP_2102N_24 0x21
#define AC_CP_2102N_20 0x22

/* Operations the SYS adds to the VxD's UCOP_* (ucif.h). */
#define AC_OP_INIT   0x101   /* FDO start, synchronous, before the ports run */
#define AC_OP_OPEN   0x102   /* each Attach, at the head of the FIFO */
#define AC_OP_CLOSE  0x103   /* each Detach, after the DTR drop */
#define AC_OP_POLL   0x104   /* CP210x modem status, every 100 ms while open */

/* AC_CHIP.Flags */
#define AC_CF_CTSFLOW 0x01   /* UCOP_FLOW bit 0: transmit held by CTS */
#define AC_CF_BREAK   0x02   /* break being sent */
#define AC_CF_DTR     0x04
#define AC_CF_RTS     0x08
#define AC_CF_ACTUAL  0x10   /* CP210x: the actual-rate rule, not AN205 */
#define AC_CF_STATE   0x20   /* LastState holds a reported state */
#define AC_CF_RTSFLOW 0x40   /* UCOP_FLOW bit 1: RTS driven by the receive side */
#define AC_CF_PLLINE  0x80   /* PL2303: PlLine holds the line the part has */

/* SERIAL_STATE bit 15: CTS off (acmcore.h; CDC never sets it). */

/* One port's chip state. Zeroed, then AcChipSetup. */
typedef struct AC_CHIP {
    AC_U8  Family;
    AC_U8  Part;
    AC_U8  Iface;        /* bInterfaceNumber of the port (CDC: CommIf) */
    AC_U8  Channel;      /* FTDI wIndex low byte: 0, or 1 + interface */
    AC_U8  Flags;
    AC_U8  DevClass, MaxPacket0;
    AC_U8  SerialIndex;  /* iSerialNumber */
    AC_U16 BcdUsb, BcdDevice;
    AC_U16 InMax;        /* bulk IN wMaxPacketSize (FTDI framing) */
    AC_U16 FtData;       /* FTDI SET_DATA value without the break bit */
    AC_U16 LastState;    /* the state last reported (FTDI, CP210x) */
    AC_U8  PlLine[7];    /* PL2303: the line coding the part has */
    AC_U8  PlNext[7];    /* PL2303: the one being sent */
    UCIF_LIMITS Limits;
} AC_CHIP;

/* One control transfer. Type is bmRequestType; Data holds an OUT stage. */
typedef struct AC_SETUP {
    AC_U8  Type;
    AC_U8  Request;
    AC_U16 Value;
    AC_U16 Index;
    AC_U16 Length;
    AC_U8  Data[16];
} AC_SETUP;

#define AC_SETUP_MAX 16

/* The family of a device, by its ids; AC_FAM_CDC if no chip claims it. */
int    AcChipFamily(AC_U16 vid, AC_U16 pid);

/*
 * Fills `c` for one port: `dev` is the 18-byte device descriptor, `iface`
 * the port's interface (CDC: its communication interface) and `inMax` its
 * bulk IN wMaxPacketSize. Sets the part where the descriptor decides it,
 * and the limits.
 */
void   AcChipSetup(AC_CHIP *c, int family, const AC_U8 *dev, AC_U8 iface,
                   AC_U16 inMax);

/*
 * A chip's ports: every interface at alternate setting 0 with a bulk IN and
 * a bulk OUT endpoint, in interface order. Fills up to `max` functions
 * (CommIf = DataIf, AC_F_COMBINED, Caps AC_CAP_LINE | AC_CAP_BREAK, the
 * interrupt IN as NotifyEp); returns the count or -1 if malformed.
 */
int    AcChipFindPorts(int family, const AC_U8 *cfg, AC_U32 len, AC_FUNC *out, int max);

/* AcChipStep results */
#define AC_STEP_DONE   0     /* the operation is finished */
#define AC_STEP_SEND   1     /* send *s, then call again with *step + 1 */
#define AC_STEP_STATE  2     /* ORed in: *state is a new SERIAL_STATE */

/*
 * The step generator. For operation `op` (UCOP_* or AC_OP_*) with `value`
 * and `data` (UCOP_LINE_CODING: the 7-byte CDC payload), returns the
 * control transfer of step *step, or DONE. On entry *step is the step
 * wanted (0 first); the generator may skip steps and stores the one it
 * returns. `ok`, `in` and `inLen` are the result of the previous step (the
 * IN stage's bytes; ignored for step 0). It updates `c` as steps complete.
 */
int    AcChipStep(AC_CHIP *c, AC_U32 op, AC_U32 value, const AC_U8 *data,
                  AC_U32 *step, int ok, const AC_U8 *in, AC_U32 inLen,
                  AC_SETUP *s, AC_U16 *state);

/*
 * Receive framing, in place: removes the FTDI status bytes from a bulk IN
 * transfer of `len` bytes and returns the data length left (other families
 * return `len`). Stores, in order, a SERIAL_STATE for each packet whose
 * lines changed or whose data came with a line error, up to `max` (the
 * last one then carries the newest lines), and their count in *nstates.
 */
AC_U32 AcChipRx(AC_CHIP *c, AC_U8 *buf, AC_U32 len, AC_U16 *states, int *nstates, int max);

/* The same framing removal with no status taken (a transfer from before a
   suspend or an open: its status is stale and must not touch LastState). */
AC_U32 AcChipStrip(const AC_CHIP *c, AC_U8 *buf, AC_U32 len);

/*
 * An interrupt IN transfer: CDC's SERIAL_STATE notifications (as
 * AcNextNotify, for c->Iface) or the PL2303's UART state at byte 8.
 * Same iterator contract as AcNextNotify.
 */
int    AcChipNotify(AC_CHIP *c, const AC_U8 *buf, AC_U32 len, AC_U32 *off, AC_U16 *state);

/* Whether the family wants AC_OP_POLL while the port is open. */
int    AcChipPolls(const AC_CHIP *c);

/* Whether the port can send a break (CDC: decided by its ACM descriptor). */
int    AcChipBreak(const AC_CHIP *c);

/* The UCIF_CAP_FLOW, _CTSFLOW and _CTSNOW of the port (none for CDC). */
AC_U32 AcChipFlowCaps(const AC_CHIP *c);

/* Whether the SYS resets the bulk pipes at each open (the PL2303 H). */
int    AcChipResetsPipes(const AC_CHIP *c);

/* The rate the chip is given for a requested one (clamped, quantised). */
AC_U32 AcChipRate(const AC_CHIP *c, AC_U32 rate);

/* ftdibaud.c: the divisors (adapted from Linux ftdi_sio.c, GPL-2.0+). */
AC_U32 AcFtdi232amDivisor(AC_U32 baud, AC_U32 base);
AC_U32 AcFtdi232bmDivisor(AC_U32 baud, AC_U32 base);
AC_U32 AcFtdi2232hDivisor(AC_U32 baud, AC_U32 base);

/* The divisor the FTDI part takes for `rate` (wValue and the high bits). */
AC_U32 AcFtdiDivisor(const AC_CHIP *c, AC_U32 rate);

/* PL2303: the 4-byte rate field for `rate`. */
void   AcPl2303Rate(const AC_CHIP *c, AC_U32 rate, AC_U8 out[4]);

#endif /* CHIPCORE_H */

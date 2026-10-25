/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * acmcore.h - the operating-system-independent part of usbcom: the CDC-ACM
 * descriptor walk, line coding, SERIAL_STATE decoding, the VCOMM-style
 * queue and flow-control logic, and the COM number choice.
 *
 * Original code, written from USB CDC 1.2 / PSTN 1.2 as quoted in
 * docs/design/serial.md (sections 6 and 7) and from the Windows 98 DDK's
 * VCOMM documentation and vcomm.h. The same source is compiled into
 * USBCOM.SYS, USBCOMV.VXD and the host tests; it calls nothing.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#ifndef ACMCORE_H
#define ACMCORE_H

typedef unsigned char  AC_U8;
typedef unsigned short AC_U16;
typedef unsigned long  AC_U32;

/* ---- Descriptor walk (design 6.1) ---------------------------------- */

#define AC_MAX_FUNCS 4

/* AC_FUNC.Flags */
#define AC_F_UNION      0x01  /* a Union descriptor named the pair */
#define AC_F_SWAPPED    0x02  /* the Union named them the wrong way round */
#define AC_F_CALLMGMT   0x04  /* the pair came from Call Management */
#define AC_F_NEXT       0x08  /* the pair is "the next interface" */
#define AC_F_COMBINED   0x10  /* control and data in one interface */

/* Quirks (subset named in design 6.1 item 4) */
#define AC_Q_NO_UNION_NORMAL   0x0001
#define AC_Q_SINGLE_RX_URB     0x0002
#define AC_Q_NO_CAP_LINE       0x0004
#define AC_Q_IGNORE_DEVICE     0x0008
#define AC_Q_CONTROL_LINE_STATE 0x0010
#define AC_Q_CLEAR_HALT        0x0020
#define AC_Q_SEND_ZERO_PACKET  0x0040

/* ACM functional descriptor bmCapabilities */
#define AC_CAP_LINE   0x02    /* D1: line coding, control lines, SERIAL_STATE */
#define AC_CAP_BREAK  0x04    /* D2: SEND_BREAK */

typedef struct AC_FUNC {
    AC_U8  CommIf;        /* bInterfaceNumber of the communication interface */
    AC_U8  CommAlt;
    AC_U8  DataIf;        /* == CommIf when AC_F_COMBINED */
    AC_U8  DataAlt;
    AC_U8  NotifyEp;      /* bEndpointAddress, 0 if none */
    AC_U8  NotifyInterval;
    AC_U16 NotifyMax;
    AC_U8  BulkIn;
    AC_U8  BulkOut;
    AC_U16 InMax;
    AC_U16 OutMax;
    AC_U8  Caps;          /* ACM bmCapabilities, 0 if no ACM descriptor */
    AC_U8  Flags;
} AC_FUNC;

/* One interface alternate setting, as found in a configuration. */
typedef struct AC_IFACE {
    AC_U8 Number, Alt, Class, SubClass, Protocol, NumEp;
} AC_IFACE;

/*
 * Walks a configuration descriptor of `len` bytes (wTotalLength already
 * fetched). Fills up to `max` functions; returns how many, or -1 if the
 * descriptor is malformed. Bounds are checked on every descriptor; nothing
 * is read past `len` or past wTotalLength.
 */
int  AcFindFunctions(const AC_U8 *cfg, AC_U32 len, AC_U32 quirks,
                     AC_FUNC *out, int max);

/*
 * Lists the interfaces of the configuration (every alternate setting) into
 * `out`; returns the count or -1 if malformed. Used to build the select-
 * configuration request.
 */
int  AcListInterfaces(const AC_U8 *cfg, AC_U32 len, AC_IFACE *out, int max);

/* ---- Line coding and the 98 DCB (design 6.2, 7.5) ------------------ */

/* Layout of VCOMM's ring-0 _DCB (vcomm.h); USBCOMV.VXD checks it matches. */
#pragma pack(1)
typedef struct AC_DCB {
    AC_U32 DCBLength;
    AC_U32 BaudRate;
    AC_U32 BitMask;
    AC_U32 XonLim;
    AC_U32 XoffLim;
    AC_U16 wReserved;
    AC_U8  ByteSize;
    AC_U8  Parity;
    AC_U8  StopBits;
    char   XonChar;
    char   XoffChar;
    char   ErrorChar;
    char   EofChar;
    char   EvtChar1;
    char   EvtChar2;
    AC_U8  bReserved;
    AC_U32 RlsTimeout;
    AC_U32 CtsTimeout;
    AC_U32 DsrTimeout;
    AC_U32 TxDelay;
} AC_DCB;
#pragma pack()

/* BitMask flags and ActionMask fields, values from vcomm.h */
#define AC_fBinary        0x00000001
#define AC_fRTSDisable    0x00000002
#define AC_fParity        0x00000004
#define AC_fOutXCTSFlow   0x00000008
#define AC_fOutXDSRFlow   0x00000010
#define AC_fDTRDisable    0x00000080
#define AC_fOutX          0x00000100
#define AC_fInX           0x00000200
#define AC_fNullStrip     0x00000800
#define AC_fDTRFlow       0x00002000
#define AC_fRTSFlow       0x00004000
#define AC_fDtrEnable     0x00040000
#define AC_fRTSEnable     0x00100000
#define AC_fRTSToggle     0x00200000

#define AC_mBaudRate   0x00000001
#define AC_mBitMask    0x00000002
#define AC_mXonLim     0x00000004
#define AC_mXoffLim    0x00000008
#define AC_mByteSize   0x00000010
#define AC_mParity     0x00000020
#define AC_mStopBits   0x00000040
#define AC_mXonChar    0x00000080
#define AC_mXoffChar   0x00000100
#define AC_mErrorChar  0x00000200
#define AC_mEofChar    0x00000400
#define AC_mEvtChar1   0x00000800
#define AC_mEvtChar2   0x00001000
#define AC_mRlsTimeout 0x00002000
#define AC_mCtsTimeout 0x00004000
#define AC_mDsrTimeout 0x00008000
#define AC_mTxDelay    0x00010000

/* VCOMM error codes (dwLastError), vcomm.h */
#define AC_IE_DEFAULT      (-5)
#define AC_IE_HARDWARE     (-10)
#define AC_IE_BYTESIZE     (-11)
#define AC_IE_BAUDRATE     (-12)
#define AC_IE_EXTINVALID   (-20)
#define AC_IE_INVALIDPARAM (-21)
#define AC_IE_TRANSMITCHARFAILED (-30)

/* Maps a DCB BaudRate (a rate, or a Win16 CBR_* index) to bit/s; 0 = bad. */
AC_U32 AcMapBaud(AC_U32 dcbBaud);

/* The 7-byte SET_LINE_CODING payload (PSTN 1.2 6.3.11). */
void   AcEncodeLineCoding(AC_U32 rate, AC_U8 stopBits, AC_U8 parity,
                          AC_U8 dataBits, AC_U8 out[7]);

/* What a DCB means for the wire. */
typedef struct AC_LINE {
    AC_U32 Rate;
    AC_U8  StopBits, Parity, DataBits;
    AC_U8  Dtr, Rts;       /* 1 = asserted */
} AC_LINE;

/* Derives the line from a validated DCB. */
void   AcDcbToLine(const AC_DCB *dcb, AC_LINE *line);

/* Default DCB for a new port: 9600 8N1, DTR and RTS enabled. */
void   AcDefaultDcb(AC_DCB *dcb, AC_U32 rxQueueSize);

/*
 * Applies the fields of `req` selected by `mask` to `cur`, after checking
 * them. On a bad value returns the VCOMM error (negative) and leaves `cur`
 * unchanged; otherwise returns 0.
 */
long   AcApplyDcb(AC_DCB *cur, const AC_DCB *req, AC_U32 mask);

/* ---- SERIAL_STATE (design 6.2, 7.2) -------------------------------- */

/* SERIAL_STATE bits (PSTN 1.2 6.5.4) */
#define AC_SS_DCD     0x0001
#define AC_SS_DSR     0x0002
#define AC_SS_BREAK   0x0004
#define AC_SS_RING    0x0008
#define AC_SS_FRAMING 0x0010
#define AC_SS_PARITY  0x0020
#define AC_SS_OVERRUN 0x0040

/* 8250 MSR layout ("Notifications", comm_12np) */
#define AC_MSR_DCTS 0x01
#define AC_MSR_DDSR 0x02
#define AC_MSR_TERI 0x04
#define AC_MSR_DDCD 0x08
#define AC_MSR_CTS  0x10
#define AC_MSR_DSR  0x20
#define AC_MSR_RI   0x40
#define AC_MSR_DCD  0x80

/* Events and errors, vcomm.h values */
#define AC_EV_RXCHAR  0x00000001
#define AC_EV_RXFLAG  0x00000002
#define AC_EV_TXEMPTY 0x00000004
#define AC_EV_CTS     0x00000008
#define AC_EV_DSR     0x00000010
#define AC_EV_RLSD    0x00000020
#define AC_EV_BREAK   0x00000040
#define AC_EV_ERR     0x00000080
#define AC_EV_RING    0x00000100
#define AC_EV_CTSS    0x00000400
#define AC_EV_DSRS    0x00000800
#define AC_EV_RLSDS   0x00001000
#define AC_EV_RingTe  0x00002000

#define AC_CE_RXOVER   0x00000001
#define AC_CE_OVERRUN  0x00000002
#define AC_CE_RXPARITY 0x00000004
#define AC_CE_FRAME    0x00000008
#define AC_CE_BREAK    0x00000010

#define AC_MS_CTS_ON  0x0010
#define AC_MS_DSR_ON  0x0020
#define AC_MS_RING_ON 0x0040
#define AC_MS_RLSD_ON 0x0080

/*
 * Finds the SERIAL_STATE notifications for interface `commIf` in an
 * interrupt-IN transfer (which may hold several notifications). Calls
 * nothing; stores the last state found in *state and returns how many
 * were found. Malformed or foreign notifications are skipped.
 */
int    AcParseNotify(const AC_U8 *buf, AC_U32 len, AC_U8 commIf,
                     AC_U16 *state);

/*
 * Iterator form: starting at *off, finds the next SERIAL_STATE for
 * `commIf`, stores it in *state, advances *off past it and returns 1;
 * returns 0 when there are no more. Start with *off = 0.
 */
int    AcNextNotify(const AC_U8 *buf, AC_U32 len, AC_U32 *off, AC_U8 commIf,
                    AC_U16 *state);

/*
 * The modem status shadow after a new SERIAL_STATE. `prevState` is the
 * previous SERIAL_STATE. Returns the MSR byte (current lines plus the
 * deltas of this change) and ORs the events and errors raised into
 * *events / *errors. CTS reads as always asserted (design 7.2).
 */
AC_U8  AcStateToMsr(AC_U16 prevState, AC_U16 state, AC_U32 *events,
                    AC_U32 *errors);

/* MS_* modem status from an MSR byte. */
AC_U32 AcMsrToModemStatus(AC_U8 msr);

/* ---- Queues (VCOMM _PortData queue fields, comm_12pf) --------------- */

/*
 * The queue fields live in VCOMM's _PortData and are modified by VCOMM
 * as well as the port driver, so the core works on pointers to them.
 */
typedef struct AC_QUEUE {
    AC_U32 *Addr, *Size, *Count, *Get, *Put;
} AC_QUEUE;

/* COMSTAT BitMask flags (vcomm.h) */
#define AC_fCtsHold   0x00000001
#define AC_fDsrHold   0x00000002
#define AC_fRlsdHold  0x00000004
#define AC_fXoffHold  0x00000008
#define AC_fXoffSent  0x00000010
#define AC_fEof       0x00000020
#define AC_fTximmed   0x00000040

/* Flow state shared by receive and transmit. */
typedef struct AC_FLOW {
    AC_U32 Status;        /* AC_fXoffHold | AC_fXoffSent | AC_fEof | AC_fTximmed */
    AC_U8  Immediate;     /* TransmitCommChar byte (valid if AC_fTximmed) */
    AC_U8  FlowChar;      /* XON/XOFF we owe the device */
    AC_U8  FlowPending;   /* 1 if FlowChar must be sent */
    AC_U8  Pad;
} AC_FLOW;

/* Results of AcRxPut / AcRxGet beyond the byte count. */
#define AC_RX_KICK_TX   0x01   /* XON received or flow char queued: pump TX */

/*
 * Receive: moves bytes from a USB packet into the queue, applying the
 * DCB's XON/XOFF output control, null stripping and event characters.
 * Returns the number of input bytes consumed; bytes not consumed did not
 * fit and stay with the caller (lossless, design 6.2). ORs AC_EV_* into
 * *events and AC_RX_* into *actions.
 */
AC_U32 AcRxPut(const AC_QUEUE *q, const AC_DCB *dcb, AC_FLOW *flow,
               const AC_U8 *data, AC_U32 len, AC_U32 *events,
               AC_U32 *actions);

/* PortRead: copies up to `len` bytes out; may owe the device an XON. */
AC_U32 AcRxGet(const AC_QUEUE *q, const AC_DCB *dcb, AC_FLOW *flow,
               AC_U8 *out, AC_U32 len, AC_U32 *actions);

/* PortWrite: copies up to `len` bytes into the transmit queue. */
AC_U32 AcTxPut(const AC_QUEUE *q, const AC_U8 *data, AC_U32 len);

/*
 * The USB side asks for bytes to send: an owed XON/XOFF first, then a
 * TransmitCommChar byte, then the queue unless an XOFF holds it.
 */
AC_U32 AcTxPull(const AC_QUEUE *q, AC_FLOW *flow, AC_U8 *out, AC_U32 max);

/* Discards a queue's contents. */
void   AcQueueClear(const AC_QUEUE *q);

/* Peeks at the first queued byte; returns 0 if the queue is empty. */
int    AcQueuePeek(const AC_QUEUE *q, AC_U8 *ch);

/* ---- COM number choice (design 5.1.4) ------------------------------ */

#define AC_COM_MAX 255

/* Parses "COMn" (case-insensitive, n = 1..255, no leading zero). 0 = not. */
AC_U32 AcParsePortName(const char *name, AC_U32 len);

/* Formats "COMn" into out (at least 7 bytes); returns the length. */
AC_U32 AcFormatPortName(AC_U32 n, char *out);

/*
 * Picks the COM number: `remembered` if it is set and free, else the
 * lowest free n >= `minimum`. `used` is a 256-bit map (bit n = COMn in
 * use). Returns 0 if none is free.
 */
AC_U32 AcChoosePort(const AC_U8 used[32], AC_U32 remembered, AC_U32 minimum);

/* ---- Trace text (test builds only) ------------------------------------ */

/*
 * Formats `fmt` into out (cap bytes, always NUL-terminated); returns the
 * length. Arguments come from v[0..n-1] in order (missing ones read as 0).
 * Conversions: %u %d %x %X %c, %s (v[i] is a char pointer), %w (a 16-bit
 * character string, printed as ASCII with '?' for others), %%. An 'l' and a
 * width with an optional leading '0' (at most 2 digits) are accepted, so
 * "%08lx" works. Used for the 0xE9 trace of the trace flavour; no CRT.
 */
AC_U32 AcFormat(char *out, AC_U32 cap, const char *fmt, const AC_U32 *v, int n);

#endif /* ACMCORE_H */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * usbcom.h - internal declarations of USBCOM.SYS, the WDM half of the
 * Windows 98 SE / ME CDC-ACM driver (docs/design/serial.md 5.1.2).
 *
 * Original code, written from the Windows 98 and 2000 DDK documentation
 * and headers. It uses only routines the 98 DDK's WDM.H declares; the
 * build compiles it against both DDKs' headers and checks the imports.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#ifndef USBCOM_H
#define USBCOM_H

#include <wdm.h>
#include <usbdi.h>
#include <usbdlib.h>
#include "acmcore.h"
#include "ucif.h"

/*
 * Trace. The trace flavour (build98.ps1 -Flavour trace, UC_TRACE_E9) writes
 * each line to QEMU's debug console port 0xE9; it is test-only and never
 * released. Checked builds (DBG) also send it to DbgPrint. Free builds
 * compile it out. Arguments go in an array, so no varargs are needed.
 */
#if defined(UC_TRACE_E9) || DBG
#define UC_TRACING 1
void UcTraceV(const char *fmt, const AC_U32 *v, int n);
#define UCT0(f)             UcTraceV(f, 0, 0)
#define UCT1(f, a)          do { AC_U32 v_[1]; v_[0] = (AC_U32)(a); UcTraceV(f, v_, 1); } while (0)
#define UCT2(f, a, b)       do { AC_U32 v_[2]; v_[0] = (AC_U32)(a); v_[1] = (AC_U32)(b); \
                                 UcTraceV(f, v_, 2); } while (0)
#define UCT3(f, a, b, c)    do { AC_U32 v_[3]; v_[0] = (AC_U32)(a); v_[1] = (AC_U32)(b); \
                                 v_[2] = (AC_U32)(c); UcTraceV(f, v_, 3); } while (0)
#define UCT4(f, a, b, c, d) do { AC_U32 v_[4]; v_[0] = (AC_U32)(a); v_[1] = (AC_U32)(b); \
                                 v_[2] = (AC_U32)(c); v_[3] = (AC_U32)(d); UcTraceV(f, v_, 4); } while (0)
#else
#define UCT0(f)             ((void)0)
#define UCT1(f, a)          ((void)0)
#define UCT2(f, a, b)       ((void)0)
#define UCT3(f, a, b, c)    ((void)0)
#define UCT4(f, a, b, c, d) ((void)0)
#endif

#define UC_IN_URBS      8        /* design 6.2 */
#define UC_IN_SIZE      4096
/*
 * One bulk OUT transfer in flight, not four. Under NUSB 3.3's EHCI driver
 * (QEMU usb-ehci, 98 SE), with several OUT transfers queued, QEMU reports
 * "guest updated active qTD", cancels the queue and sends it again; an OUT
 * the device had already taken then reached it twice (a 64 KiB echo failed
 * in 35 of 70 runs: 2026-10-23-usbcom-nusb-echo.md). With one OUT
 * in flight that never happened (50 runs). A transfer still carries 4096
 * bytes, so the cost is one completion gap per 4 KiB; a 64 KiB echo took as
 * long as before in the High Speed VM runs (Full Speed and real hardware
 * not measured).
 */
#define UC_OUT_URBS     1
#define UC_OUT_SIZE     4096
#define UC_NOTIFY_SIZE  64
#define UC_OPS          32       /* control operations queued per port */
#define UC_RESET_TRIES  3        /* pipe resets before a pipe is left halted */

typedef enum UC_KIND { UcKindFdo = 1, UcKindPdo = 2 } UC_KIND;

typedef struct UC_FDO UC_FDO;
typedef struct UC_PORT UC_PORT;

/* A driver-owned IRP (IoInitializeIrp before every use) with its URB. */
typedef struct UC_XFER {
    PIRP    Irp;
    UC_PORT *Port;
    union {
        struct _URB_BULK_OR_INTERRUPT_TRANSFER Bulk;
        struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST Class;
        struct _URB_PIPE_REQUEST Pipe;
    } Urb;
    PUCHAR  Buffer;
    ULONG   Length;        /* valid bytes (IN: received; OUT: to send) */
    ULONG   Offset;        /* IN: bytes already delivered */
    UCHAR   Busy;          /* submitted, completion not yet run */
    UCHAR   Held;          /* IN: completed, data not yet all delivered */
    UCHAR   Index;
    UCHAR   Kind;          /* UC_X_* */
#if defined(UC_TEST_SPLIT_CANCEL)
    ULONG   Seq;           /* IN: submission number (test flavour "split") */
#endif
} UC_XFER;

#define UC_X_IN      1
#define UC_X_OUT     2
#define UC_X_NOTIFY  3
#define UC_X_CONTROL 4

struct UC_PORT {
    KSPIN_LOCK      Lock;
    KDPC            Dpc;
    UC_FDO         *Fdo;
    PDEVICE_OBJECT  Pdo;
    ULONG           Index;          /* function number within the device */
    AC_FUNC         Func;
    USBD_PIPE_HANDLE InPipe, OutPipe, NotifyPipe;
    ULONG           PortNumber;     /* n of COMn */
    WCHAR           InstanceId[64];

    /* State (under Lock) */
    BOOLEAN         Started;        /* PDO started and the device configured */
    BOOLEAN         PdoStarted;     /* PnP state of the PDO alone */
    BOOLEAN         SerialCommWritten;
    BOOLEAN         Attached;
    BOOLEAN         Gone;
    BOOLEAN         Stopping;       /* no new submissions */
    BOOLEAN         Suspended;
    BOOLEAN         NamesWritten;
    BOOLEAN         RxHalted, TxHalted, NotifyHalted;
    BOOLEAN         HaveLine, HaveControl;
    UCIF_OP         SentLine, SentControl;  /* last sent, resent on resume */
    ULONG           RxResets, TxResets;
    UCIF_CALLBACKS  Cb;
    LONG            KickFlags;      /* interlocked */

#if defined(UC_TEST_SPLIT_CANCEL)
    ULONG           InSeq;          /* last IN submission number */
#endif

    /* IN completions in arrival order (indexes into In[]) */
    UCHAR           RxOrder[UC_IN_URBS];
    ULONG           RxHead, RxCount;

    UC_XFER         In[UC_IN_URBS];
    UC_XFER         Out[UC_OUT_URBS];
    UC_XFER         Notify;
    UC_XFER         Ctl;
    UCHAR           CtlData[8];

    UCIF_OP         Ops[UC_OPS];
    ULONG           OpHead, OpCount;
    USHORT          LastState;

    /* Outstanding IRPs and table callers; Drained is set at zero. */
    LONG            Outstanding;
    LONG            Busy;
    KEVENT          Drained;
    BOOLEAN         ResetQueued;
    WORK_QUEUE_ITEM ResetItem;

    UCIF_TABLE      Table;
};

typedef struct UC_COMMON {
    UC_KIND Kind;
} UC_COMMON;

struct UC_FDO {
    UC_COMMON       Common;
    PDEVICE_OBJECT  Self;
    PDEVICE_OBJECT  Pdo;
    PDEVICE_OBJECT  Lower;
    LONG            IoCount;        /* starts at 1; REMOVE drops it */
    KEVENT          RemoveEvent;
    BOOLEAN         Started;
    BOOLEAN         Removed;
    USB_DEVICE_DESCRIPTOR DevDesc;
    PUSB_CONFIGURATION_DESCRIPTOR Config;
    ULONG           ConfigLength;
    PURB            Select;         /* the select-configuration URB */
    ULONG           Quirks;
    ULONG           NumPorts;
    UC_PORT        *Ports[AC_MAX_FUNCS];
    WCHAR           KeyTag[24];     /* from the device's driver key name */
};

typedef struct UC_PDOX {
    UC_COMMON       Common;
    UC_PORT        *Port;
} UC_PDOX;

extern PDRIVER_OBJECT UcDriver;

/* usbcom.c */
NTSTATUS UcCallLowerSync(UC_FDO *fdo, PIRP irp);
NTSTATUS UcSubmitUrbSync(UC_FDO *fdo, PURB urb);

/* ucio.c */
NTSTATUS UcPortCreate(UC_FDO *fdo, ULONG index, const AC_FUNC *f, UC_PORT **out);
void     UcPortFree(UC_PORT *port);
void     UcPortStop(UC_PORT *port);
void     UcPortStart(UC_PORT *port);
void     UcPortSuspend(UC_PORT *port);
void     UcPortResume(UC_PORT *port);

#endif /* USBCOM_H */

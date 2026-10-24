/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * xpad98: the WDM HID minidriver's shared definitions
 * (docs/design/gamepad.md sections 3, 5.5 and 6).
 */
#ifndef XPAD98_H
#define XPAD98_H

#include <wdm.h>
#pragma warning(push)
#pragma warning(disable:4200)   /* zero-sized arrays in usbdi.h */
#include <usbdi.h>
#include <usbioctl.h>
#pragma warning(pop)
#include <hidclass.h>
#include "xpcore.h"

/*
 * The HID minidriver interface. Declared here from the published
 * hidport.h interface (Windows 98 DDK HID_DDK\SRC_HID_INC_HIDPORT.H and
 * WDK 7.1 inc\ddk\hidport.h, which agree on everything used here), so
 * that one source builds with the Windows 2000 DDK, which ships no
 * hidport.h, and with WDK 7.1. HID_REVISION and the HID_CTL_CODE macro
 * come from the kit's hidclass.h.
 */
#ifndef __HIDPORT_H__
typedef struct _HID_MINIDRIVER_REGISTRATION {
    ULONG Revision;
    PDRIVER_OBJECT DriverObject;
    PUNICODE_STRING RegistryPath;
    ULONG DeviceExtensionSize;
    BOOLEAN DevicesArePolled;
    UCHAR Reserved[3];
} HID_MINIDRIVER_REGISTRATION, *PHID_MINIDRIVER_REGISTRATION;

typedef struct _HID_DEVICE_EXTENSION {
    PDEVICE_OBJECT PhysicalDeviceObject;
    PDEVICE_OBJECT NextDeviceObject;
    PVOID MiniDeviceExtension;
} HID_DEVICE_EXTENSION, *PHID_DEVICE_EXTENSION;

typedef struct _HID_DEVICE_ATTRIBUTES {
    ULONG Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
    USHORT Reserved[11];
} HID_DEVICE_ATTRIBUTES, *PHID_DEVICE_ATTRIBUTES;

#include <pshpack1.h>
typedef struct _HID_DESCRIPTOR {
    UCHAR bLength;
    UCHAR bDescriptorType;
    USHORT bcdHID;
    UCHAR bCountry;
    UCHAR bNumDescriptors;
    struct _HID_DESCRIPTOR_DESC_LIST {
        UCHAR bReportType;
        USHORT wReportLength;
    } DescriptorList[1];
} HID_DESCRIPTOR, *PHID_DESCRIPTOR;
#include <poppack.h>

NTSTATUS HidRegisterMinidriver(PHID_MINIDRIVER_REGISTRATION MinidriverRegistration);

#define IOCTL_HID_GET_DEVICE_DESCRIPTOR          HID_CTL_CODE(0)
#define IOCTL_HID_GET_REPORT_DESCRIPTOR          HID_CTL_CODE(1)
#define IOCTL_HID_READ_REPORT                    HID_CTL_CODE(2)
#define IOCTL_HID_WRITE_REPORT                   HID_CTL_CODE(3)
#define IOCTL_HID_GET_STRING                     HID_CTL_CODE(4)
#define IOCTL_HID_ACTIVATE_DEVICE                HID_CTL_CODE(7)
#define IOCTL_HID_DEACTIVATE_DEVICE              HID_CTL_CODE(8)
#define IOCTL_HID_GET_DEVICE_ATTRIBUTES          HID_CTL_CODE(9)
#define IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST HID_CTL_CODE(10)

#define HID_HID_DESCRIPTOR_TYPE    0x21
#define HID_REPORT_DESCRIPTOR_TYPE 0x22

#define HID_STRING_ID_IMANUFACTURER 14
#define HID_STRING_ID_IPRODUCT      15
#define HID_STRING_ID_ISERIALNUMBER 16
#endif

#define XP_TAG 'dapX'
#define XP_IN_PER_CHAN 2        /* interrupt IN kept armed twice (5.5) */
#define XP_OUTQ 8               /* queued OUT packets per channel */
#define XP_IN_ERROR_LIMIT 8     /* consecutive IN errors before parking */
#define XP_CFG_MAX 1024         /* configuration descriptor bytes read */

struct XP_DEV;
struct XP_CHAN;

/* One transfer the driver owns: its IRP is driver-allocated memory set up
 * with IoInitializeIrp, so it is reused on every system from 98 SE on
 * without IoReuseIrp (2000 and later only). */
typedef struct XP_XFER {
    PIRP Irp;
    union {
        struct _URB_BULK_OR_INTERRUPT_TRANSFER Int;
        struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST Vendor;
    } Urb;
    UCHAR Buf[XP_PKT_LEN];
    struct XP_DEV *Dev;
    struct XP_CHAN *Chan;
    BOOLEAN Busy;               /* owned by the USB stack */
    BOOLEAN Parked;             /* stopped after errors; re-armed on D0 */
    ULONG Errors;
} XP_XFER;

typedef struct XP_CHAN {
    UCHAR Index;                /* slot number; report id is Index + 1 */
    UCHAR InAddr, OutAddr;
    USBD_PIPE_HANDLE InPipe, OutPipe;
    XP_PAD Pad;
    BOOLEAN Dirty;              /* changed since the last report taken */
    XP_XFER In[XP_IN_PER_CHAN];
    XP_XFER Out;
    UCHAR OutQ[XP_OUTQ][XP_PKT_LEN];
    UCHAR OutQLen[XP_OUTQ];
    ULONG OutHead, OutCount;
    UCHAR GipSeq;               /* GIP sequence number for our packets */
    BOOLEAN DelayedInitDone;
} XP_CHAN;

typedef struct XP_DEV {
    PDEVICE_OBJECT Fdo;         /* hidclass's FDO */
    PDEVICE_OBJECT Lower;       /* HID_DEVICE_EXTENSION.NextDeviceObject */
    PDEVICE_OBJECT Pdo;
    KSPIN_LOCK Lock;
    KEVENT IoIdle;              /* signalled when Outstanding is 0 */
    LONG Outstanding;
    BOOLEAN Started, Stopping, PoweredDown;

    USB_DEVICE_DESCRIPTOR DevDesc;
    const XP_DEVICE *Known;     /* devtab row, or NULL */
    unsigned Family;
    XU16 Flags;
    USBD_CONFIGURATION_HANDLE ConfigHandle;
    PUSBD_INTERFACE_INFORMATION Interfaces;     /* in SelectUrb */
    PURB SelectUrb;
    unsigned NumChan;
    XP_CHAN Chan[XP_MAX_CHAN];
    XP_XFER Ctl;                /* the XUSB start request */
    LONG Player;                /* LED number from the driver-wide bitmap, -1 none */

    XP_OPTIONS Options;
    UCHAR ReportDesc[XP_REPORT_DESC_MAX];
    USHORT ReportDescLen;

    LIST_ENTRY Reads;           /* pending IOCTL_HID_READ_REPORT */
} XP_DEV;

#define XP_DEV_OF(fdo) \
    ((XP_DEV *)((PHID_DEVICE_EXTENSION)(fdo)->DeviceExtension)->MiniDeviceExtension)

/* usbio.c */
NTSTATUS XpUsbStart(XP_DEV *dev);
VOID XpUsbStop(XP_DEV *dev, BOOLEAN present);
VOID XpUsbFree(XP_DEV *dev);
VOID XpUsbResume(XP_DEV *dev);
VOID XpQueueOut(XP_DEV *dev, XP_CHAN *chan, const UCHAR *data, ULONG len, BOOLEAN gipSeq);

/* The trace flavour (build.cmd wdmx86 trace, test-only, never released):
 * one line per event to QEMU's debug console port 0xE9, "xp: tag a b c". */
#if defined(XP_TRACE_E9)
VOID XpTrace(const char *tag, ULONG a, ULONG b, ULONG c);
VOID XpTraceBytes(const char *tag, const UCHAR *p, ULONG n);
#define XPT(t, a, b, c) XpTrace((t), (ULONG)(a), (ULONG)(b), (ULONG)(c))
#define XPTB(t, p, n) XpTraceBytes((t), (const UCHAR *)(p), (ULONG)(n))
#else
#define XPT(t, a, b, c) ((void)0)
#define XPTB(t, p, n) ((void)0)
#endif

/* xpad98.c */
VOID XpStateChanged(XP_DEV *dev);
LONG XpAllocPlayer(VOID);
VOID XpFreePlayer(LONG player);

#endif

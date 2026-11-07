/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * absptr: the WDM HID minidriver's shared definitions
 * (docs/design/absptr.md sections 4 to 7). Adapted from xpad98's
 * xpad98.h (this repository).
 */
#ifndef ABSPTR_H
#define ABSPTR_H

#include <wdm.h>
#pragma warning(push)
#pragma warning(disable:4200)   /* zero-sized arrays in usbdi.h */
#include <usbdi.h>
#include <usbioctl.h>
#pragma warning(pop)
#include <hidclass.h>
#include "apcore.h"

/*
 * The HID minidriver interface, declared from the published hidport.h
 * (Windows 98 DDK HID_DDK\SRC_HID_INC_HIDPORT.H and WDK 7.1
 * inc\ddk\hidport.h), as xpad98.h does, so that one source builds with the
 * Windows 2000 DDK, which ships no hidport.h. HID_REVISION and HID_CTL_CODE
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

#define AP_TAG 'ptpA'
#define AP_PKT_MAX 1024        /* interrupt IN buffer: a High Speed wMaxPacketSize */
#define AP_IN_COUNT 2           /* interrupt IN kept armed twice (design 7) */
#define AP_IN_ERROR_LIMIT 8     /* consecutive IN errors before parking */
#define AP_CFG_MAX 1024         /* configuration descriptor bytes read */
#define AP_QLEN 16              /* pointer states waiting for a read */
#define AP_NAME_MAX 64          /* product string characters kept */
#define AP_MODE_TRIES 3         /* PenPartner mode switch attempts (5.2) */
#define AP_FIRST_GAP_MS 100     /* pause after the first report of a start (design 6) */
#define AP_RESUME_TRIES 3       /* device setup attempts after a resume (design 7) */
#define AP_RESUME_GAP_MS 250    /* between them */

#define AP_PENPARTNER_VID 0x056A
#define AP_PENPARTNER_PID 0x0000

struct AP_DEV;

/* One transfer the driver owns: its IRP is driver-allocated memory set up
 * with IoInitializeIrp, as xpad98's, so it is reused on every system from
 * 98 SE on without IoReuseIrp. */
typedef struct AP_XFER {
    PIRP Irp;
    union {
        struct _URB_BULK_OR_INTERRUPT_TRANSFER Int;
        struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST Class;
    } Urb;
    UCHAR Buf[AP_PKT_MAX];
    struct AP_DEV *Dev;
    BOOLEAN Busy;               /* owned by the USB stack */
    BOOLEAN Parked;             /* stopped after errors; re-armed on D0 */
    ULONG Errors;
} AP_XFER;

typedef struct AP_DEV {
    PDEVICE_OBJECT Fdo;         /* hidclass's FDO */
    PDEVICE_OBJECT Lower;       /* HID_DEVICE_EXTENSION.NextDeviceObject */
    PDEVICE_OBJECT Pdo;
    KSPIN_LOCK Lock;
    KEVENT IoIdle;              /* signalled when Outstanding is 0 */
    LONG Outstanding;
    LONG Submitting;            /* transfers between the resubmit decision and IoCallDriver */
    KEVENT SubmitIdle;          /* signalled when Submitting is 0 */
    BOOLEAN Started, Stopping, PoweredDown;
    BOOLEAN ResumeQueued;       /* the D0 work item is queued or running */
    ULONG PowerGen;             /* moves on at every D0 and every power-down */
    ULONG ResumeGen;            /* PowerGen of the latest D0 */
    BOOLEAN InSetup;            /* the D0 work item is sending its setup requests */
    BOOLEAN Failed;             /* setup failed after a resume: reported to PnP */
    KEVENT SetupIdle;           /* signalled when InSetup is FALSE */
    WORK_QUEUE_ITEM ResumeWork;

    USB_DEVICE_DESCRIPTOR DevDesc;
    UCHAR IfNum;                /* the HID interface */
    UCHAR SubClass;             /* its subclass: 1 = boot interface */
    UCHAR InAddr;
    USHORT InMax;               /* wMaxPacketSize of the interrupt IN */
    USBD_PIPE_HANDLE InPipe;
    USBD_CONFIGURATION_HANDLE ConfigHandle;

    AP_SOURCE Src;              /* where the pointer comes from (apcore.h) */
    AP_STATE State;             /* the last state translated */
    AP_STATE Q[AP_QLEN];        /* states not yet read by hidclass */
    ULONG QHead, QCount;
    AU8 LastButtons;            /* the buttons of the last report read */
    BOOLEAN Reported;           /* a report was read since the start */
    BOOLEAN Gated;              /* no report is read until GapTimer fires */
    KTIMER GapTimer;
    KDPC GapDpc;

    AP_XFER In[AP_IN_COUNT];

    UCHAR ReportDesc[128];
    USHORT ReportDescLen;
    WCHAR Name[AP_NAME_MAX];    /* the device's product string */
    ULONG NameLen;              /* characters, 0 when none was read */

    LIST_ENTRY Reads;           /* pending IOCTL_HID_READ_REPORT */
} AP_DEV;

#define AP_DEV_OF(fdo) \
    ((AP_DEV *)((PHID_DEVICE_EXTENSION)(fdo)->DeviceExtension)->MiniDeviceExtension)

/* usbio.c */
NTSTATUS ApUsbStart(AP_DEV *dev);
VOID ApUsbStop(AP_DEV *dev, BOOLEAN present);
VOID ApUsbFree(AP_DEV *dev);
VOID ApUsbResume(AP_DEV *dev, ULONG gen);
VOID ApUsbPowerDown(AP_DEV *dev);
VOID ApUsbQueueResume(AP_DEV *dev);

/* absptr.c */
VOID ApQueueStateLocked(AP_DEV *dev, const AP_STATE *prev);
PIRP ApTakeReadyReadLocked(AP_DEV *dev);
VOID ApFailReads(AP_DEV *dev, NTSTATUS status);
VOID ApCancelGap(AP_DEV *dev);

/* The trace flavour (build.cmd wdmx86 trace, test-only, never released):
 * one line per event to QEMU's debug console port 0xE9, "ap: tag a b c". */
#if defined(AP_TRACE_E9)
VOID ApTrace(const char *tag, ULONG a, ULONG b, ULONG c);
VOID ApTraceBytes(const char *tag, const UCHAR *p, ULONG n);
#define APT(t, a, b, c) ApTrace((t), (ULONG)(a), (ULONG)(b), (ULONG)(c))
#define APTB(t, p, n) ApTraceBytes((t), (const UCHAR *)(p), (ULONG)(n))
#else
#define APT(t, a, b, c) ((void)0)
#define APTB(t, p, n) ((void)0)
#endif

#endif

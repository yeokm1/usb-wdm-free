/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * absptr: the USB side (docs/design/absptr.md section 7). Adapted from
 * xpad98's usbio.c (this repository), cut down to one HID interface with
 * one interrupt IN endpoint: the descriptors, SELECT_CONFIGURATION, the
 * HID report descriptor, SET_IDLE, the front end's start requests
 * (design 5.1 and 5.2), and the interrupt IN kept armed twice. Plain USBD
 * URBs from the DDK's usbdi.h; no import from usbd.sys.
 *
 * Transfers use driver-owned IRPs (pool memory set up with
 * IoInitializeIrp) so they can be resubmitted from their completion
 * routines on every target system. Outstanding counts every transfer the
 * USB stack holds and the queued D0 work item, plus one bias while the
 * device is started; stop drops the bias and waits for zero. Submitting
 * counts the transfers between the decision to send (taken under the
 * lock, never once Stopping or PoweredDown is set) and their IoCallDriver,
 * so stop and power-down first wait for it to be zero: after that every
 * transfer is in the USB stack, where the pipe abort or IoCancelIrp
 * reaches it.
 */
#include "absptr.h"

static NTSTATUS InComplete(PDEVICE_OBJECT unused, PIRP irp, PVOID context);

#define HID_SET_REPORT 0x09
#define HID_GET_REPORT 0x01
#define HID_SET_IDLE   0x0A
#define HID_SET_PROTOCOL 0x0B
#define HID_FEATURE    3

static VOID Drop(AP_DEV *dev)
{
    if (InterlockedDecrement(&dev->Outstanding) == 0)
        KeSetEvent(&dev->IoIdle, IO_NO_INCREMENT, FALSE);
}

/* Lock held: a transfer is about to be sent outside the lock. */
static VOID BeginSubmitLocked(AP_DEV *dev)
{
    if (dev->Submitting++ == 0)
        KeClearEvent(&dev->SubmitIdle);
}

static VOID EndSubmit(AP_DEV *dev)
{
    KIRQL irql;
    KeAcquireSpinLock(&dev->Lock, &irql);
    if (--dev->Submitting == 0)
        KeSetEvent(&dev->SubmitIdle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&dev->Lock, irql);
}

/* PASSIVE_LEVEL: sleeps ms milliseconds. */
static VOID Sleep(ULONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

/* ---- synchronous URBs, PASSIVE_LEVEL only ---- */

static NTSTATUS SyncDone(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    UNREFERENCED_PARAMETER(unused);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS SyncUrb(AP_DEV *dev, PURB urb, ULONG ms)
{
    PIRP irp = IoAllocateIrp(dev->Lower->StackSize, FALSE);
    PIO_STACK_LOCATION s;
    KEVENT done;
    LARGE_INTEGER timeout;
    NTSTATUS status;

    if (!irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent(&done, NotificationEvent, FALSE);
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    s->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    s->Parameters.Others.Argument1 = urb;
    IoSetCompletionRoutine(irp, SyncDone, &done, TRUE, TRUE, TRUE);
    (void)IoCallDriver(dev->Lower, irp);
    timeout.QuadPart = -(LONGLONG)ms * 10000;
    if (KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
        IoCancelIrp(irp);
        /* The stack still owns the IRP and URB until it completes. */
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
        status = irp->IoStatus.Status == STATUS_CANCELLED ? STATUS_IO_TIMEOUT : irp->IoStatus.Status;
    } else {
        status = irp->IoStatus.Status;
    }
    APT("urb", urb->UrbHeader.Function, status, urb->UrbHeader.Status);
    if (NT_SUCCESS(status) && !USBD_SUCCESS(urb->UrbHeader.Status))
        status = STATUS_DEVICE_DATA_ERROR;
    IoFreeIrp(irp);
    return status;
}

/* A standard GET_DESCRIPTOR. With fromInterface, the recipient is the
 * interface and LanguageId carries the interface number as the setup
 * packet's wIndex (HID 1.11 7.1.1: the class descriptors are read from
 * the interface). */
static NTSTATUS GetDescriptor(AP_DEV *dev, BOOLEAN fromInterface, UCHAR type, UCHAR index,
                              USHORT langOrIf, PVOID buf, ULONG len, ULONG *got)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST u;
    NTSTATUS status;
    RtlZeroMemory(&u, sizeof(u));
    u.Hdr.Length = sizeof(u);
    u.Hdr.Function = fromInterface ? URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE
                                   : URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    u.DescriptorType = type;
    u.Index = index;
    u.LanguageId = langOrIf;
    u.TransferBuffer = buf;
    u.TransferBufferLength = len;
    status = SyncUrb(dev, (PURB)&u, 5000);
    *got = NT_SUCCESS(status) ? u.TransferBufferLength : 0;
    return status;
}

/* A HID class request to the interface (HID 1.11 7.2). */
static NTSTATUS ClassRequest(AP_DEV *dev, UCHAR request, USHORT value, BOOLEAN in,
                             PVOID buf, ULONG len, ULONG *got)
{
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST u;
    NTSTATUS status;
    RtlZeroMemory(&u, sizeof(u));
    u.Hdr.Length = sizeof(u);
    u.Hdr.Function = URB_FUNCTION_CLASS_INTERFACE;
    u.TransferFlags = in ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK) : USBD_TRANSFER_DIRECTION_OUT;
    u.TransferBuffer = len ? buf : NULL;
    u.TransferBufferLength = len;
    u.Request = request;
    u.Value = value;
    u.Index = dev->IfNum;
    status = SyncUrb(dev, (PURB)&u, 2000);
    if (got)
        *got = NT_SUCCESS(status) ? u.TransferBufferLength : 0;
    return status;
}

static VOID AbortPipe(AP_DEV *dev, USBD_PIPE_HANDLE pipe)
{
    struct _URB_PIPE_REQUEST u;
    if (!pipe)
        return;
    RtlZeroMemory(&u, sizeof(u));
    u.Hdr.Length = sizeof(u);
    u.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
    u.PipeHandle = pipe;
    (void)SyncUrb(dev, (PURB)&u, 5000);
}

static VOID Unconfigure(AP_DEV *dev)
{
    struct _URB_SELECT_CONFIGURATION u;
    RtlZeroMemory(&u, sizeof(u));
    u.Hdr.Length = sizeof(u);
    u.Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    u.ConfigurationDescriptor = NULL;
    (void)SyncUrb(dev, (PURB)&u, 5000);
}

/* ---- the configuration descriptor (design 7) ---- */

typedef struct AP_CFG {
    UCHAR IfNum, NumEndpoints, InAddr, SubClass;
    USHORT InMax, ReportLen;
    unsigned Interfaces;        /* distinct interface numbers */
} AP_CFG;

/* One HID interface at alternate 0, its HID descriptor's report length,
 * its first interrupt IN endpoint; any second interface refuses the
 * device (a composite, design 7 and Q5). Returns 0 when usable. */
static int ParseConfig(const UCHAR *cfg, ULONG len, AP_CFG *k)
{
    ULONG pos = 0;
    int inHid = 0, haveIf = 0;
    UCHAR lastIf = 0xFF;
    RtlZeroMemory(k, sizeof(*k));
    while (pos + 2 <= len) {
        UCHAR dl = cfg[pos], dt = cfg[pos + 1];
        if (dl < 2 || pos + dl > len)
            return -1;
        if (dt == USB_INTERFACE_DESCRIPTOR_TYPE && dl >= 9) {
            if (cfg[pos + 2] != lastIf) {
                k->Interfaces++;
                lastIf = cfg[pos + 2];
            }
            inHid = 0;
            if (cfg[pos + 3] == 0 && !haveIf && cfg[pos + 5] == 0x03) {
                haveIf = 1;
                inHid = 1;
                k->IfNum = cfg[pos + 2];
                k->NumEndpoints = cfg[pos + 4];
                k->SubClass = cfg[pos + 6];
            }
        } else if (dt == HID_HID_DESCRIPTOR_TYPE && inHid && dl >= 9) {
            UCHAR n = cfg[pos + 5], i;
            for (i = 0; i < n && 6 + 3 * (ULONG)i + 3 <= dl; i++)
                if (cfg[pos + 6 + 3 * i] == HID_REPORT_DESCRIPTOR_TYPE && !k->ReportLen)
                    k->ReportLen = (USHORT)(cfg[pos + 7 + 3 * i] | (cfg[pos + 8 + 3 * i] << 8));
        } else if (dt == USB_ENDPOINT_DESCRIPTOR_TYPE && inHid && dl >= 7) {
            if ((cfg[pos + 2] & 0x80) && (cfg[pos + 3] & 3) == 3 && !k->InAddr) {
                k->InAddr = cfg[pos + 2];
                k->InMax = (USHORT)((cfg[pos + 4] | (cfg[pos + 5] << 8)) & 0x7FF);
            }
        }
        pos += dl;
    }
    if (!haveIf || k->Interfaces != 1 || !k->InAddr || !k->ReportLen || k->ReportLen > AP_DESC_MAX ||
        !k->InMax || k->InMax > AP_PKT_MAX)
        return -1;
    return 0;
}

#define IF_SIZE(n) (FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) + (n) * sizeof(USBD_PIPE_INFORMATION))

static NTSTATUS SelectConfig(AP_DEV *dev, PUCHAR cfg, const AP_CFG *k)
{
    ULONG size = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface) + IF_SIZE(k->NumEndpoints);
    struct _URB_SELECT_CONFIGURATION *u;
    PUSBD_INTERFACE_INFORMATION info;
    NTSTATUS status;
    unsigned p;

    u = ExAllocatePoolWithTag(NonPagedPool, size, AP_TAG);
    if (!u)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(u, size);
    u->Hdr.Length = (USHORT)size;
    u->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    u->ConfigurationDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)cfg;
    info = &u->Interface;
    info->Length = (USHORT)IF_SIZE(k->NumEndpoints);
    info->InterfaceNumber = k->IfNum;
    info->AlternateSetting = 0;
    info->NumberOfPipes = k->NumEndpoints;
    for (p = 0; p < k->NumEndpoints; p++)
        info->Pipes[p].MaximumTransferSize = PAGE_SIZE;
    status = SyncUrb(dev, (PURB)u, 5000);
    APT("select-done", status, (ULONG_PTR)u->ConfigurationHandle, u->Hdr.Status);
    if (NT_SUCCESS(status) && !u->ConfigurationHandle)
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (NT_SUCCESS(status)) {
        dev->ConfigHandle = u->ConfigurationHandle;
        for (p = 0; p < info->NumberOfPipes && p < k->NumEndpoints; p++)
            if (info->Pipes[p].EndpointAddress == k->InAddr)
                dev->InPipe = info->Pipes[p].PipeHandle;
        if (!dev->InPipe)
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    ExFreePool(u);
    return status;
}

/* ---- driver-owned transfers ---- */

static NTSTATUS XferAlloc(AP_DEV *dev, AP_XFER *x)
{
    CCHAR stack = dev->Lower->StackSize;
    USHORT size = IoSizeOfIrp(stack);
    x->Dev = dev;
    x->Busy = FALSE;
    x->Parked = FALSE;
    x->Errors = 0;
    x->Irp = ExAllocatePoolWithTag(NonPagedPool, size, AP_TAG);
    if (!x->Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    IoInitializeIrp(x->Irp, size, stack);
    return STATUS_SUCCESS;
}

static VOID XferFree(AP_XFER *x)
{
    if (x->Irp) {
        ExFreePool(x->Irp);
        x->Irp = NULL;
    }
}

static VOID XferSend(AP_XFER *x, PIO_COMPLETION_ROUTINE done)
{
    PIRP irp = x->Irp;
    PIO_STACK_LOCATION s;
    CCHAR stack = x->Dev->Lower->StackSize;
    IoInitializeIrp(irp, IoSizeOfIrp(stack), stack);
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    s = IoGetNextIrpStackLocation(irp);
    s->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    s->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    s->Parameters.Others.Argument1 = &x->Urb;
    IoSetCompletionRoutine(irp, done, x, TRUE, TRUE, TRUE);
    (void)IoCallDriver(x->Dev->Lower, irp);
}

/* One wMaxPacketSize, so a report that fills its packet ends the
 * transfer (HID devices send no zero-length packet); when the longest
 * input report of the descriptor, chosen or not, is longer than a packet,
 * as many whole packets as it needs, so no report is split across two
 * transfers (a read is a multiple of wMaxPacketSize, as the USB stacks
 * require; HID 1.11 8.4). */
static ULONG InLength(const AP_DEV *dev)
{
    ULONG mps = dev->InMax ? dev->InMax : 64, len = mps;
    ULONG need = dev->Src.MaxInBytes > dev->Src.ReportBytes ? dev->Src.MaxInBytes : dev->Src.ReportBytes;
    if (need > len)
        len = (need + mps - 1) / mps * mps;
    while (len > AP_PKT_MAX && len > mps)
        len -= mps;
    return len;
}

static VOID SendIn(AP_XFER *x)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *u = &x->Urb.Int;
    RtlZeroMemory(u, sizeof(*u));
    u->Hdr.Length = sizeof(*u);
    u->Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    u->PipeHandle = x->Dev->InPipe;
    u->TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    u->TransferBuffer = x->Buf;
    u->TransferBufferLength = InLength(x->Dev);
    XferSend(x, InComplete);
}

/* ---- interrupt IN ---- */

static NTSTATUS InComplete(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    AP_XFER *x = context;
    AP_DEV *dev = x->Dev;
    NTSTATUS status = irp->IoStatus.Status;
    KIRQL irql;
    BOOLEAN resubmit;
    PIRP read;
    AP_STATE prev;
    UNREFERENCED_PARAMETER(unused);

    KeAcquireSpinLock(&dev->Lock, &irql);
    if (NT_SUCCESS(status) && USBD_SUCCESS(x->Urb.Int.Hdr.Status)) {
        unsigned len = x->Urb.Int.TransferBufferLength;
#if defined(AP_TRACE_E9)
        static ULONG traced;
        if (traced < 24) {
            traced++;
            APTB("in", x->Buf, len > 24 ? 24 : len);
        }
#endif
        if (len > AP_PKT_MAX)
            len = AP_PKT_MAX;
        x->Errors = 0;
        /* A transfer may hold more than one report (a report that fills
         * whole packets does not end it): each is translated on its own.
         * The first may be short (the device sent fewer bytes); a later
         * one that does not fit is the start of a report cut off at the
         * end of the transfer and is dropped, not read as a report. */
        {
            unsigned off = 0, n;
            while (off < len && (n = ApReportLength(&dev->Src, x->Buf + off, len - off)) != 0) {
                if (n > len - off) {
                    if (off)
                        break;
                    n = len - off;
                }
                prev = dev->State;
                if (ApTranslate(&dev->Src, x->Buf + off, n, &dev->State))
                    ApQueueStateLocked(dev, &prev);
                off += n;
            }
        }
    } else if (status != STATUS_CANCELLED) {
        APT("in-error", status, x->Urb.Int.Hdr.Status, x->Errors);
        x->Errors++;
    }
    /* Only stop and power-down cancel these transfers, and both set their
     * flag first; a cancelled completion that arrives after a resume (its
     * cancel was still outstanding) is sent again (design 7). */
    resubmit = (BOOLEAN)(!dev->Stopping && !dev->PoweredDown && x->Errors < AP_IN_ERROR_LIMIT);
    if (!resubmit) {
        x->Busy = FALSE;
        x->Parked = (BOOLEAN)!dev->Stopping;
    } else {
        BeginSubmitLocked(dev);
    }
    read = ApTakeReadyReadLocked(dev);
    KeReleaseSpinLock(&dev->Lock, irql);

    if (read)
        IoCompleteRequest(read, IO_NO_INCREMENT);
    if (resubmit) {
        SendIn(x);
        EndSubmit(dev);
    } else {
        Drop(dev);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* ---- the front ends' start requests (design 5.1, 5.2) ---- */

/* The PenPartner's tablet mode (design 5.2): SET_REPORT of feature report
 * 2 with 02 02, read back with GET_REPORT, up to AP_MODE_TRIES times while
 * the write fails or the answer's second byte is not 2, as Linux's
 * wacom_set_device_mode does. A write that went through but was never
 * confirmed does not fail the start (QEMU's model answers GET_REPORT with
 * its current input report, so its read-back cannot confirm); a write that
 * never went through does, as the tablet is then still in its mouse mode. */
static NTSTATUS PenPartnerMode(AP_DEV *dev)
{
    UCHAR buf[8];
    ULONG got, n;
    unsigned tries;
    BOOLEAN written = FALSE;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    for (tries = 0; tries < AP_MODE_TRIES; tries++) {
        n = ApPenPartnerModeData(buf, sizeof(buf));
        status = ClassRequest(dev, HID_SET_REPORT, (USHORT)((HID_FEATURE << 8) | buf[0]), FALSE, buf, n, NULL);
        APT("pp-set", status, tries, 0);
        if (!NT_SUCCESS(status))
            continue;
        written = TRUE;
        RtlZeroMemory(buf, sizeof(buf));
        got = 0;
        status = ClassRequest(dev, HID_GET_REPORT, (USHORT)((HID_FEATURE << 8) | 2), TRUE, buf, 2, &got);
        APT("pp-get", status, got, buf[1]);
        if (NT_SUCCESS(status) && ApPenPartnerModeOk(buf, got))
            break;
    }
    return written ? STATUS_SUCCESS : status;
}

/* The Device Mode feature (design 5.1): read the feature report, write
 * the mode into its field with the rest kept, send it back. Any failure
 * leaves the device as it is (the start goes on). */
static VOID DeviceMode(AP_DEV *dev)
{
    UCHAR f[AP_REPORT_MAX];
    ULONG n = dev->Src.DevModeBytes, got = 0;
    USHORT value = (USHORT)((HID_FEATURE << 8) | dev->Src.DevModeId);
    NTSTATUS st;
    if (n == 0 || n > sizeof(f))
        return;
    RtlZeroMemory(f, sizeof(f));
    st = ClassRequest(dev, HID_GET_REPORT, value, TRUE, f, n, &got);
    APT("devmode-get", st, got, n);
    if (!NT_SUCCESS(st) || got != n)
        return;
    if (dev->Src.UsesIds && f[0] != dev->Src.DevModeId)
        f[0] = dev->Src.DevModeId;      /* some devices omit the id byte's value */
    if (!ApDevModeApply(&dev->Src, f, n))
        return;
    st = ClassRequest(dev, HID_SET_REPORT, value, FALSE, f, n, NULL);
    APT("devmode-set", st, n, ApDevModeValue(&dev->Src));
}

/* The device's protocol and mode (design 5.1, 5.2, 7): at start, and
 * again after a return to D0, since a device may reset them. PASSIVE. */
static NTSTATUS DeviceSetup(AP_DEV *dev)
{
    /* A boot interface may be left in its boot protocol: select the
     * report protocol (HID 1.11 7.2.6). Then SET_IDLE(0): report only on
     * change. A device may refuse either. */
    if (dev->SubClass == 1)
        (void)ClassRequest(dev, HID_SET_PROTOCOL, 1, FALSE, NULL, 0, NULL);
    (void)ClassRequest(dev, HID_SET_IDLE, 0, FALSE, NULL, 0, NULL);
    if (dev->Src.Kind == AP_KIND_PENPARTNER)
        return PenPartnerMode(dev);
    if (ApDevModeValue(&dev->Src))
        DeviceMode(dev);
    return STATUS_SUCCESS;
}

static VOID ReadName(AP_DEV *dev)
{
    UCHAR buf[2 + 2 * AP_NAME_MAX];
    ULONG got, i, n;
    dev->NameLen = 0;
    if (!dev->DevDesc.iProduct)
        return;
    if (!NT_SUCCESS(GetDescriptor(dev, FALSE, USB_STRING_DESCRIPTOR_TYPE, dev->DevDesc.iProduct, 0x0409,
                                  buf, sizeof(buf), &got)) || got < 4 || buf[1] != USB_STRING_DESCRIPTOR_TYPE)
        return;
    if (buf[0] < got)
        got = buf[0];
    if (got < 2 || (got & 1))
        return;
    n = (got - 2) / 2;
    if (n > AP_NAME_MAX - 1)
        n = AP_NAME_MAX - 1;
    for (i = 0; i < n; i++)
        dev->Name[i] = (WCHAR)(buf[2 + 2 * i] | (buf[3 + 2 * i] << 8));
    dev->NameLen = n;
}

/* ---- start, stop, resume ---- */

VOID ApUsbFree(AP_DEV *dev)
{
    unsigned i;
    for (i = 0; i < AP_IN_COUNT; i++)
        XferFree(&dev->In[i]);
}

NTSTATUS ApUsbStart(AP_DEV *dev)
{
    PUCHAR cfg = NULL, desc = NULL;
    PVOID work = NULL;
    AP_CFG k;
    ULONG got;
    NTSTATUS status;
    unsigned total, i;
    int r;

    ApUsbFree(dev);
    dev->InPipe = NULL;
    dev->ConfigHandle = NULL;
    dev->QHead = dev->QCount = 0;
    RtlZeroMemory(&dev->Src, sizeof(dev->Src));
    RtlZeroMemory(dev->In, sizeof(dev->In));

    status = GetDescriptor(dev, FALSE, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, &dev->DevDesc, sizeof(dev->DevDesc), &got);
    if (NT_SUCCESS(status) && got != sizeof(dev->DevDesc))
        status = STATUS_DEVICE_DATA_ERROR;
    if (!NT_SUCCESS(status))
        return status;

    cfg = ExAllocatePoolWithTag(NonPagedPool, AP_CFG_MAX, AP_TAG);
    if (!cfg) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    status = GetDescriptor(dev, FALSE, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, cfg, 9, &got);
    if (!NT_SUCCESS(status))
        goto done;
    total = cfg[2] | ((unsigned)cfg[3] << 8);
    if (got != 9 || total < 9 || total > AP_CFG_MAX) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto done;
    }
    status = GetDescriptor(dev, FALSE, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, cfg, total, &got);
    if (!NT_SUCCESS(status))
        goto done;
    APTB("config", cfg, got > 64 ? 64 : got);
    if (got != total || ParseConfig(cfg, got, &k) != 0) {
        /* Not one HID interface with an interrupt IN (design 7). */
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto done;
    }
    dev->IfNum = k.IfNum;
    dev->SubClass = k.SubClass;
    dev->InAddr = k.InAddr;
    dev->InMax = k.InMax;

    status = SelectConfig(dev, cfg, &k);
    if (!NT_SUCCESS(status))
        goto done;
    if (dev->DevDesc.idVendor == AP_PENPARTNER_VID && dev->DevDesc.idProduct == AP_PENPARTNER_PID) {
        ApPenPartnerSource(&dev->Src);
    } else {
        desc = ExAllocatePoolWithTag(NonPagedPool, k.ReportLen, AP_TAG);
        work = ExAllocatePoolWithTag(NonPagedPool, ApWorkSize(), AP_TAG);
        if (!desc || !work) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto done;
        }
        status = GetDescriptor(dev, TRUE, HID_REPORT_DESCRIPTOR_TYPE, 0, dev->IfNum, desc, k.ReportLen, &got);
        if (!NT_SUCCESS(status))
            goto done;
        APTB("hiddesc", desc, got > 48 ? 48 : got);
        r = ApParse(desc, got, work, &dev->Src);
        APT("parse", r, dev->Src.Kind, dev->Src.ReportBytes);
        if (r != AP_OK) {
            /* No collection absptr can drive (design 5.1). */
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
            goto done;
        }
    }
    status = DeviceSetup(dev);
    if (!NT_SUCCESS(status))
        goto done;
    ReadName(dev);

    dev->ReportDescLen = (USHORT)ApReportDescriptor(dev->ReportDesc, sizeof(dev->ReportDesc));
    if (!dev->ReportDescLen) {
        status = STATUS_INTERNAL_ERROR;
        goto done;
    }
    for (i = 0; i < AP_IN_COUNT && NT_SUCCESS(status); i++)
        status = XferAlloc(dev, &dev->In[i]);
    if (!NT_SUCCESS(status))
        goto done;

    ApStateReset(&dev->State);
    dev->LastButtons = 0;
    dev->Reported = FALSE;
    dev->Gated = FALSE;
    KeClearEvent(&dev->IoIdle);
    dev->Outstanding = 1;               /* the bias, dropped by stop */
    dev->Submitting = 0;
    KeSetEvent(&dev->SubmitIdle, IO_NO_INCREMENT, FALSE);
    dev->Stopping = FALSE;
    dev->PoweredDown = FALSE;
    dev->Failed = FALSE;
    dev->ResumeQueued = FALSE;
    {
        KIRQL irql;
        KeAcquireSpinLock(&dev->Lock, &irql);
        dev->Started = TRUE;
        for (i = 0; i < AP_IN_COUNT; i++) {
            dev->In[i].Busy = TRUE;
            InterlockedIncrement(&dev->Outstanding);
            BeginSubmitLocked(dev);
        }
        KeReleaseSpinLock(&dev->Lock, irql);
    }
    for (i = 0; i < AP_IN_COUNT; i++) {
        SendIn(&dev->In[i]);
        EndSubmit(dev);
    }
    status = STATUS_SUCCESS;

done:
    APT("usbstart", status, dev->Src.Kind, dev->ReportDescLen);
    if (cfg)
        ExFreePool(cfg);
    if (desc)
        ExFreePool(desc);
    if (work)
        ExFreePool(work);
    if (!NT_SUCCESS(status) && !dev->Started) {
        if (dev->ConfigHandle)
            Unconfigure(dev);
        dev->ConfigHandle = NULL;
        dev->InPipe = NULL;
        ApUsbFree(dev);
    }
    return status;
}

/* PASSIVE_LEVEL: STOP_DEVICE, SURPRISE_REMOVAL, REMOVE_DEVICE. */
VOID ApUsbStop(AP_DEV *dev, BOOLEAN present)
{
    KIRQL irql;
    LARGE_INTEGER timeout;
    unsigned i;

    if (!dev->Started)
        return;
    KeAcquireSpinLock(&dev->Lock, &irql);
    dev->Stopping = TRUE;
    KeReleaseSpinLock(&dev->Lock, irql);
    /* No send starts once Stopping is set; wait for those already decided
     * to reach the USB stack, so the abort below reaches them. */
    KeWaitForSingleObject(&dev->SubmitIdle, Executive, KernelMode, FALSE, NULL);

    AbortPipe(dev, dev->InPipe);
    ApCancelGap(dev);
    Drop(dev);      /* the bias */
    timeout.QuadPart = -20000000;       /* 2 s */
    if (KeWaitForSingleObject(&dev->IoIdle, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
        for (i = 0; i < AP_IN_COUNT; i++)
            if (dev->In[i].Busy)
                IoCancelIrp(dev->In[i].Irp);
        KeWaitForSingleObject(&dev->IoIdle, Executive, KernelMode, FALSE, NULL);
    }
    ApFailReads(dev, STATUS_DEVICE_NOT_CONNECTED);
    if (present)
        Unconfigure(dev);
    dev->InPipe = NULL;
    for (i = 0; i < AP_IN_COUNT; i++)
        dev->In[i].Parked = FALSE;
    dev->Started = FALSE;
}

/* PASSIVE_LEVEL, from the D0 work item: re-arm the parked IN transfers,
 * unless a power-down came after the D0 this work is for (gen). */
VOID ApUsbResume(AP_DEV *dev, ULONG gen)
{
    KIRQL irql;
    AP_XFER *arm[AP_IN_COUNT];
    unsigned n = 0, i;

    KeAcquireSpinLock(&dev->Lock, &irql);
    if (!dev->Started || dev->Stopping || dev->PowerGen != gen) {
        KeReleaseSpinLock(&dev->Lock, irql);
        return;
    }
    dev->PoweredDown = FALSE;
    for (i = 0; i < AP_IN_COUNT; i++) {
        AP_XFER *x = &dev->In[i];
        if (!x->Busy) {
            x->Busy = TRUE;
            x->Parked = FALSE;
            x->Errors = 0;
            InterlockedIncrement(&dev->Outstanding);
            BeginSubmitLocked(dev);
            arm[n++] = x;
        }
    }
    KeReleaseSpinLock(&dev->Lock, irql);
    for (i = 0; i < n; i++) {
        SendIn(arm[i]);
        EndSubmit(dev);
    }
}

/* The D0 work item: the device's protocol and mode again (a device may
 * have reset them, design 5.1, 5.2), then the IN transfers. */
static VOID ResumeWorker(PVOID context)
{
    AP_DEV *dev = context;
    KIRQL irql;
    BOOLEAN go, failed = FALSE;
    ULONG gen;
    for (;;) {
        KeAcquireSpinLock(&dev->Lock, &irql);
        gen = dev->ResumeGen;
        /* Only for the latest D0, and only while no power-down followed
         * it (PowerGen moves on at every transition). */
        go = (BOOLEAN)(dev->Started && !dev->Stopping && dev->PowerGen == gen);
        if (!go) {
            dev->ResumeQueued = FALSE;
        } else {
            /* A PASSIVE_LEVEL power-down waits for this before it lets D3
             * go down, so no setup request reaches a powered-down device. */
            dev->InSetup = TRUE;
            KeClearEvent(&dev->SetupIdle);
        }
        KeReleaseSpinLock(&dev->Lock, irql);
        if (!go)
            break;
        {
            /* Only the PenPartner's mode switch can fail (no write went
             * through): a tablet left in its mouse mode sends nothing the
             * driver reads. Tried again a few times; if it still fails the
             * device is reported failed to PnP, as a failed start is, so
             * Windows stops it instead of leaving a pointer that never
             * moves (design 7). */
            NTSTATUS st = DeviceSetup(dev);
            unsigned tries;
            for (tries = 1; !NT_SUCCESS(st) && tries < AP_RESUME_TRIES; tries++) {
                Sleep(AP_RESUME_GAP_MS);
                if (dev->Stopping || dev->PowerGen != gen)
                    break;
                st = DeviceSetup(dev);
            }
            APT("resume-setup", st, dev->Src.Kind, tries);
            failed = (BOOLEAN)!NT_SUCCESS(st);
        }
        KeAcquireSpinLock(&dev->Lock, &irql);
        dev->InSetup = FALSE;
        KeSetEvent(&dev->SetupIdle, IO_NO_INCREMENT, FALSE);
        if (failed && dev->Started && !dev->Stopping && dev->PowerGen == gen)
            dev->Failed = TRUE;
        else
            failed = FALSE;
        KeReleaseSpinLock(&dev->Lock, irql);
        if (failed) {
            IoInvalidateDeviceState(dev->Pdo);
            KeAcquireSpinLock(&dev->Lock, &irql);
            dev->ResumeQueued = FALSE;
            KeReleaseSpinLock(&dev->Lock, irql);
            break;
        }
        ApUsbResume(dev, gen);
        KeAcquireSpinLock(&dev->Lock, &irql);
        /* A newer D0 while this one ran: run again for it. */
        go = (BOOLEAN)(dev->ResumeGen != gen);
        if (!go)
            dev->ResumeQueued = FALSE;
        KeReleaseSpinLock(&dev->Lock, irql);
        if (!go)
            break;
    }
    Drop(dev);
}

/* Any IRQL up to DISPATCH_LEVEL (the D0 completion routine): queue the
 * resume work, counted in Outstanding so stop waits for it. */
VOID ApUsbQueueResume(AP_DEV *dev)
{
    KIRQL irql;
    BOOLEAN queue = FALSE;
    KeAcquireSpinLock(&dev->Lock, &irql);
    dev->PowerGen++;
    dev->ResumeGen = dev->PowerGen;
    if (dev->Started && !dev->Stopping && !dev->ResumeQueued) {
        dev->ResumeQueued = TRUE;
        InterlockedIncrement(&dev->Outstanding);
        queue = TRUE;
    }
    KeReleaseSpinLock(&dev->Lock, irql);
    if (queue) {
        ExInitializeWorkItem(&dev->ResumeWork, ResumeWorker, dev);
        ExQueueWorkItem(&dev->ResumeWork, DelayedWorkQueue);
    }
}

/* A device power state below D0 is coming: no transfer is sent from now
 * on (and a pending D0 work item is void), the ones in the USB stack are
 * cancelled, and at PASSIVE_LEVEL the driver first waits for sends
 * already decided to reach the USB stack, then (up to a second) for the
 * cancelled transfers to complete, before the power request goes down.
 * At DISPATCH_LEVEL it can only cancel: a transfer decided just before
 * may then still be sent, and stays pending until the device answers, a
 * later power-down or stop cancels it, or, after the resume, it is sent
 * again (design 7). */
VOID ApUsbPowerDown(AP_DEV *dev)
{
    KIRQL irql;
    unsigned i, waited;
    BOOLEAN busy;
    KeAcquireSpinLock(&dev->Lock, &irql);
    dev->PoweredDown = TRUE;
    dev->PowerGen++;
    KeReleaseSpinLock(&dev->Lock, irql);
    if (!dev->Started)
        return;
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        /* the D0 work item's setup requests first (each has a timeout) */
        KeWaitForSingleObject(&dev->SetupIdle, Executive, KernelMode, FALSE, NULL);
        KeWaitForSingleObject(&dev->SubmitIdle, Executive, KernelMode, FALSE, NULL);
    }
    for (i = 0; i < AP_IN_COUNT; i++)
        if (dev->In[i].Busy && dev->In[i].Irp)
            IoCancelIrp(dev->In[i].Irp);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    for (waited = 0; waited < 1000; waited += 10) {
        busy = FALSE;
        KeAcquireSpinLock(&dev->Lock, &irql);
        for (i = 0; i < AP_IN_COUNT; i++)
            if (dev->In[i].Busy)
                busy = TRUE;
        KeReleaseSpinLock(&dev->Lock, irql);
        if (!busy)
            break;
        Sleep(10);
    }
}

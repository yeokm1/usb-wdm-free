/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * absptr: the HID minidriver (docs/design/absptr.md sections 4 and 6).
 * DriverEntry registers with hidclass.sys; hidclass owns AddDevice's FDO
 * and calls the dispatch routines below with it. Adapted from xpad98.c
 * (this repository); the minidriver interface is that of the DDK
 * documentation and the published hidport.h; no DDK sample was read.
 */
#include "absptr.h"

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath);

#if defined(AP_TRACE_E9)
/* _outp is the compiler's intrinsic for the x86 OUT instruction. */
int __cdecl _outp(unsigned short port, int value);
#pragma intrinsic(_outp)

static VOID Put(const char *s)
{
    while (*s)
        _outp(0xE9, *s++);
}

static VOID PutHex(ULONG v)
{
    int i;
    for (i = 28; i >= 0; i -= 4)
        _outp(0xE9, "0123456789abcdef"[(v >> i) & 15]);
}

VOID ApTrace(const char *tag, ULONG a, ULONG b, ULONG c)
{
    Put("ap: ");
    Put(tag);
    _outp(0xE9, ' '); PutHex(a);
    _outp(0xE9, ' '); PutHex(b);
    _outp(0xE9, ' '); PutHex(c);
    _outp(0xE9, '\n');
}

VOID ApTraceBytes(const char *tag, const UCHAR *p, ULONG n)
{
    ULONG i;
    Put("ap: ");
    Put(tag);
    for (i = 0; i < n; i++) {
        _outp(0xE9, ' ');
        _outp(0xE9, "0123456789abcdef"[p[i] >> 4]);
        _outp(0xE9, "0123456789abcdef"[p[i] & 15]);
    }
    _outp(0xE9, '\n');
}

static ULONG TraceReads;
#endif

/* ---- the state queue (design 6) ---- */

/* Lock held: queues one state. A move replaces the newest queued state
 * only when that state is itself a move (its buttons equal those before
 * it), so a read sees the latest position while every button change stays
 * a report of its own, in order, at the position it happened: a press and
 * release between two reads are both reported, and a move after a press
 * does not merge into the press (design 6). A full queue replaces its
 * newest entry. */
static VOID QueueOne(AP_DEV *dev, const AP_STATE *st)
{
    ULONG last;
    AU8 before;
    if (dev->QCount) {
        last = (dev->QHead + dev->QCount - 1) % AP_QLEN;
        before = dev->QCount > 1 ? dev->Q[(last + AP_QLEN - 1) % AP_QLEN].Buttons : dev->LastButtons;
        if ((dev->Q[last].Buttons == st->Buttons && dev->Q[last].Buttons == before) ||
            dev->QCount == AP_QLEN) {
            dev->Q[last] = *st;
            return;
        }
    }
    dev->Q[(dev->QHead + dev->QCount) % AP_QLEN] = *st;
    dev->QCount++;
}

/* Lock held: dev->State changed from prev; queue it for the next read.
 * When the buttons and the position change together (a touch landing
 * somewhere new), the move goes first as a report of its own with the old
 * buttons: 98 SE's mouse mapper does not move the pointer on a report
 * that also changes a button (design 6). */
VOID ApQueueStateLocked(AP_DEV *dev, const AP_STATE *prev)
{
    if (prev->Buttons != dev->State.Buttons &&
        (prev->X != dev->State.X || prev->Y != dev->State.Y)) {
        AP_STATE move = dev->State;
        move.Buttons = prev->Buttons;
        QueueOne(dev, &move);
    }
    QueueOne(dev, &dev->State);
}

/* Lock held: writes the oldest queued state into the read IRP. */
static VOID FillRead(AP_DEV *dev, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    unsigned n = ApBuildReport(&dev->Q[dev->QHead], (AU8 *)irp->UserBuffer,
                               s->Parameters.DeviceIoControl.OutputBufferLength);
    if (n) {
        dev->LastButtons = dev->Q[dev->QHead].Buttons;
        dev->QHead = (dev->QHead + 1) % AP_QLEN;
        dev->QCount--;
        if (!dev->Reported) {
            /* The first report after a start: 98 SE now and then loses a
             * button change that follows it at once (a touch's press after
             * its move), so the next report waits (design 6). The timer
             * counts as outstanding I/O, so stop waits for its DPC. */
            LARGE_INTEGER due;
            dev->Reported = TRUE;
            if (!dev->Stopping) {
                dev->Gated = TRUE;
                InterlockedIncrement(&dev->Outstanding);
                due.QuadPart = -(LONGLONG)AP_FIRST_GAP_MS * 10000;
                KeSetTimer(&dev->GapTimer, due, &dev->GapDpc);
            }
        }
    }
#if defined(AP_TRACE_E9)
    if (TraceReads < 40) {
        TraceReads++;
        APTB("report", irp->UserBuffer, n);
    }
#endif
    irp->IoStatus.Information = n;
    irp->IoStatus.Status = n ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}

PIRP ApTakeReadyReadLocked(AP_DEV *dev)
{
    PLIST_ENTRY e;
    PIRP irp;
    /* RemoveHeadList is a statement macro in the 2000 DDK. */
    while (dev->QCount && !dev->Gated && !IsListEmpty(&dev->Reads)) {
        e = RemoveHeadList(&dev->Reads);
        irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(irp, NULL) == NULL) {
            /* Its cancel routine is running and will complete it; make
             * that routine's RemoveEntryList harmless. */
            InitializeListHead(e);
            continue;
        }
        FillRead(dev, irp);
        return irp;
    }
    return NULL;
}

VOID ApFailReads(AP_DEV *dev, NTSTATUS status)
{
    LIST_ENTRY done;
    KIRQL irql;
    PLIST_ENTRY e;
    PIRP irp;
    InitializeListHead(&done);
    KeAcquireSpinLock(&dev->Lock, &irql);
    while (!IsListEmpty(&dev->Reads)) {
        e = RemoveHeadList(&dev->Reads);
        irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(irp, NULL) == NULL) {
            InitializeListHead(e);
            continue;
        }
        InsertTailList(&done, e);
    }
    KeReleaseSpinLock(&dev->Lock, irql);
    while (!IsListEmpty(&done)) {
        e = RemoveHeadList(&done);
        irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        irp->IoStatus.Status = status;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

static VOID GapDone(PKDPC dpc, PVOID context, PVOID a1, PVOID a2)
{
    AP_DEV *dev = context;
    KIRQL irql;
    PIRP irp;
    UNREFERENCED_PARAMETER(dpc);
    UNREFERENCED_PARAMETER(a1);
    UNREFERENCED_PARAMETER(a2);
    KeAcquireSpinLock(&dev->Lock, &irql);
    dev->Gated = FALSE;
    irp = ApTakeReadyReadLocked(dev);
    KeReleaseSpinLock(&dev->Lock, irql);
    if (irp)
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    if (InterlockedDecrement(&dev->Outstanding) == 0)
        KeSetEvent(&dev->IoIdle, IO_NO_INCREMENT, FALSE);
}

/* PASSIVE_LEVEL, stop: a gap timer still waiting will not fire; drop its
 * count (a DPC already queued or running drops its own). */
VOID ApCancelGap(AP_DEV *dev)
{
    if (KeCancelTimer(&dev->GapTimer) &&
        InterlockedDecrement(&dev->Outstanding) == 0)
        KeSetEvent(&dev->IoIdle, IO_NO_INCREMENT, FALSE);
}

static VOID CancelRead(PDEVICE_OBJECT fdo, PIRP irp)
{
    AP_DEV *dev = AP_DEV_OF(fdo);
    KIRQL irql;
    IoReleaseCancelSpinLock(irp->CancelIrql);
    KeAcquireSpinLock(&dev->Lock, &irql);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&dev->Lock, irql);
    irp->IoStatus.Status = STATUS_CANCELLED;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static NTSTATUS ReadReport(AP_DEV *dev, PIRP irp)
{
    KIRQL irql;

    KeAcquireSpinLock(&dev->Lock, &irql);
    if (!dev->Started || dev->Stopping) {
        KeReleaseSpinLock(&dev->Lock, irql);
        irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (dev->QCount && !dev->Gated) {
        NTSTATUS status;
        FillRead(dev, irp);
        status = irp->IoStatus.Status;
        KeReleaseSpinLock(&dev->Lock, irql);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return status;
    }
    /* Nothing new: the read waits for the next state change. */
    IoSetCancelRoutine(irp, CancelRead);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL) != NULL) {
        KeReleaseSpinLock(&dev->Lock, irql);
        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_CANCELLED;
    }
    IoMarkIrpPending(irp);
    InsertTailList(&dev->Reads, &irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&dev->Lock, irql);
    return STATUS_PENDING;
}

/* ---- HID IOCTLs ---- */

static NTSTATUS GetString(AP_DEV *dev, PIRP irp, PIO_STACK_LOCATION s)
{
    ULONG id = (ULONG)((ULONG_PTR)s->Parameters.DeviceIoControl.Type3InputBuffer & 0xFFFF);
    ULONG cap = s->Parameters.DeviceIoControl.OutputBufferLength / sizeof(WCHAR);
    PWCHAR out = (PWCHAR)irp->UserBuffer;
    static const char fallback[] = "absptr pointer";
    const char *text = NULL;
    ULONG i, n = 0;

    if (id == HID_STRING_ID_IPRODUCT) {
        if (dev->NameLen) {
            n = dev->NameLen;
            if (cap < n + 1)
                return STATUS_INVALID_BUFFER_SIZE;
            for (i = 0; i < n; i++)
                out[i] = dev->Name[i];
            out[n] = 0;
            irp->IoStatus.Information = (n + 1) * sizeof(WCHAR);
            return STATUS_SUCCESS;
        }
        text = fallback;
    } else if (id == HID_STRING_ID_IMANUFACTURER) {
        text = "absptr";
    } else {
        return STATUS_INVALID_PARAMETER;
    }
    while (text[n])
        n++;
    if (cap < n + 1)
        return STATUS_INVALID_BUFFER_SIZE;
    for (i = 0; i < n; i++)
        out[i] = (WCHAR)(UCHAR)text[i];
    out[n] = 0;
    irp->IoStatus.Information = (n + 1) * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

static NTSTATUS ApIoctl(PDEVICE_OBJECT fdo, PIRP irp)
{
    AP_DEV *dev = AP_DEV_OF(fdo);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG outLen = s->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status;

    irp->IoStatus.Information = 0;
#if defined(AP_TRACE_E9)
    if (s->Parameters.DeviceIoControl.IoControlCode != IOCTL_HID_READ_REPORT || TraceReads < 40)
        APT("ioctl", s->Parameters.DeviceIoControl.IoControlCode, outLen,
            s->Parameters.DeviceIoControl.InputBufferLength);
#endif
    switch (s->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_HID_READ_REPORT:
        return ReadReport(dev, irp);

    case IOCTL_HID_GET_DEVICE_DESCRIPTOR: {
        PHID_DESCRIPTOR d = (PHID_DESCRIPTOR)irp->UserBuffer;
        if (outLen < sizeof(HID_DESCRIPTOR)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(d, sizeof(*d));
        d->bLength = sizeof(HID_DESCRIPTOR);
        d->bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
        d->bcdHID = 0x0111;
        d->bCountry = 0;
        d->bNumDescriptors = 1;
        d->DescriptorList[0].bReportType = HID_REPORT_DESCRIPTOR_TYPE;
        d->DescriptorList[0].wReportLength = dev->ReportDescLen;
        irp->IoStatus.Information = sizeof(HID_DESCRIPTOR);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        if (outLen < dev->ReportDescLen) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlCopyMemory(irp->UserBuffer, dev->ReportDesc, dev->ReportDescLen);
        irp->IoStatus.Information = dev->ReportDescLen;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES: {
        PHID_DEVICE_ATTRIBUTES a = (PHID_DEVICE_ATTRIBUTES)irp->UserBuffer;
        if (outLen < sizeof(HID_DEVICE_ATTRIBUTES)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(a, sizeof(*a));
        a->Size = sizeof(HID_DEVICE_ATTRIBUTES);
        a->VendorID = dev->DevDesc.idVendor;
        a->ProductID = dev->DevDesc.idProduct;
        a->VersionNumber = dev->DevDesc.bcdDevice;
        irp->IoStatus.Information = sizeof(HID_DEVICE_ATTRIBUTES);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_HID_GET_STRING:
        status = GetString(dev, irp, s);
        break;

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        status = STATUS_SUCCESS;
        break;

    default:
        /* Write and feature reports, idle notification and anything else:
         * the presented device has none (design 6). */
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    APT("ioctl-done", s->Parameters.DeviceIoControl.IoControlCode, status, irp->IoStatus.Information);
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

/* ---- PnP ---- */

static NTSTATUS StartDone(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    UNREFERENCED_PARAMETER(unused);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS ForwardSync(AP_DEV *dev, PIRP irp)
{
    KEVENT done;
    KeInitializeEvent(&done, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, StartDone, &done, TRUE, TRUE, TRUE);
    if (IoCallDriver(dev->Lower, irp) == STATUS_PENDING)
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
    return irp->IoStatus.Status;
}

static NTSTATUS ApPnp(PDEVICE_OBJECT fdo, PIRP irp)
{
    AP_DEV *dev = AP_DEV_OF(fdo);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status;

    APT("pnp", s->MinorFunction, 0, 0);
    switch (s->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = ForwardSync(dev, irp);
        APT("start-lower", status, 0, 0);
        if (NT_SUCCESS(status))
            status = ApUsbStart(dev);
        APT("start", status, dev->Src.Kind, dev->Src.ReportId);
        irp->IoStatus.Status = status;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return status;

    case IRP_MN_STOP_DEVICE:
        ApUsbStop(dev, TRUE);
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        ApUsbStop(dev, FALSE);
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE:
        /* hidclass detaches and deletes its FDO; the minidriver frees only
         * its own resources. */
        ApUsbStop(dev, FALSE);
        ApUsbFree(dev);
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_PNP_DEVICE_STATE:
        /* A resume left the device unusable (design 7). */
        if (dev->Failed) {
            irp->IoStatus.Information |= PNP_DEVICE_FAILED;
            irp->IoStatus.Status = STATUS_SUCCESS;
        }
        break;

    case IRP_MN_QUERY_CAPABILITIES:
        /* On the way up: the device may be unplugged at any time, and
         * with SurpriseRemovalOK set 2000 does not show its "Unsafe
         * Removal of Device" dialog for it (98 SE and ME read no such
         * field). */
        status = ForwardSync(dev, irp);
        if (NT_SUCCESS(status) && s->Parameters.DeviceCapabilities.Capabilities)
            s->Parameters.DeviceCapabilities.Capabilities->SurpriseRemovalOK = TRUE;
        irp->IoStatus.Status = status;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return status;

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    default:
        break;
    }
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(dev->Lower, irp);
}

/* ---- WMI: not used; passed down (HID minidriver contract) ---- */

static NTSTATUS ApSystemControl(PDEVICE_OBJECT fdo, PIRP irp)
{
    AP_DEV *dev = AP_DEV_OF(fdo);
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(dev->Lower, irp);
}

/* ---- power (as xpad98: D0/D3, transfers again on D0) ---- */

static NTSTATUS D0Done(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    AP_DEV *dev = context;
    UNREFERENCED_PARAMETER(unused);
    if (irp->PendingReturned)
        IoMarkIrpPending(irp);
    PoStartNextPowerIrp(irp);
    if (NT_SUCCESS(irp->IoStatus.Status))
        ApUsbQueueResume(dev);
    return STATUS_SUCCESS;
}

static NTSTATUS ApPower(PDEVICE_OBJECT fdo, PIRP irp)
{
    AP_DEV *dev = AP_DEV_OF(fdo);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);

    APT("power", s->MinorFunction, s->Parameters.Power.Type, s->Parameters.Power.State.DeviceState);
    if (s->MinorFunction == IRP_MN_SET_POWER && s->Parameters.Power.Type == DevicePowerState) {
        if (s->Parameters.Power.State.DeviceState == PowerDeviceD0) {
            IoCopyCurrentIrpStackLocationToNext(irp);
            IoSetCompletionRoutine(irp, D0Done, dev, TRUE, TRUE, TRUE);
            return PoCallDriver(dev->Lower, irp);
        } else {
            /* No transfer crosses the power-down: stop sending, cancel
             * those in the USB stack and wait for them (design 7). */
            ApUsbPowerDown(dev);
        }
    }
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(dev->Lower, irp);
}

/* ---- registration ---- */

static NTSTATUS ApAddDevice(PDRIVER_OBJECT driver, PDEVICE_OBJECT fdo)
{
    PHID_DEVICE_EXTENSION hid = (PHID_DEVICE_EXTENSION)fdo->DeviceExtension;
    AP_DEV *dev = (AP_DEV *)hid->MiniDeviceExtension;
    UNREFERENCED_PARAMETER(driver);
    RtlZeroMemory(dev, sizeof(*dev));
    dev->Fdo = fdo;
    dev->Lower = hid->NextDeviceObject;
    dev->Pdo = hid->PhysicalDeviceObject;
    KeInitializeSpinLock(&dev->Lock);
    KeInitializeEvent(&dev->IoIdle, NotificationEvent, FALSE);
    KeInitializeEvent(&dev->SubmitIdle, NotificationEvent, TRUE);
    KeInitializeEvent(&dev->SetupIdle, NotificationEvent, TRUE);
    KeInitializeTimer(&dev->GapTimer);
    KeInitializeDpc(&dev->GapDpc, GapDone, dev);
    InitializeListHead(&dev->Reads);
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static VOID ApUnload(PDRIVER_OBJECT driver)
{
    UNREFERENCED_PARAMETER(driver);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath)
{
    HID_MINIDRIVER_REGISTRATION reg;

    driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = ApIoctl;
    driver->MajorFunction[IRP_MJ_PNP] = ApPnp;
    driver->MajorFunction[IRP_MJ_POWER] = ApPower;
    driver->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = ApSystemControl;
    driver->DriverExtension->AddDevice = ApAddDevice;
    driver->DriverUnload = ApUnload;

    RtlZeroMemory(&reg, sizeof(reg));
    reg.Revision = HID_REVISION;
    reg.DriverObject = driver;
    reg.RegistryPath = registryPath;
    reg.DeviceExtensionSize = sizeof(AP_DEV);
    reg.DevicesArePolled = FALSE;   /* interrupt IN, not polled */
#if defined(AP_TRACE_E9)
    {
        NTSTATUS st = HidRegisterMinidriver(&reg);
        APT("driverentry", st, HID_REVISION, sizeof(AP_DEV));
        return st;
    }
#else
    return HidRegisterMinidriver(&reg);
#endif
}

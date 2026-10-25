/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * xpad98: the HID minidriver (docs/design/gamepad.md sections 3 and 6.1).
 * DriverEntry registers with hidclass.sys; hidclass owns AddDevice's FDO
 * and calls the dispatch routines below with it. The minidriver interface
 * is that of the DDK documentation and the published hidport.h; no DDK
 * sample was read.
 */
#include "xpad98.h"

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath);
PIRP XpTakeReadyReadLocked(XP_DEV *dev);
VOID XpFailReads(XP_DEV *dev, NTSTATUS status);
VOID XpReadOptions(XP_DEV *dev);

#if defined(XP_TRACE_E9)
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

VOID XpTrace(const char *tag, ULONG a, ULONG b, ULONG c)
{
    Put("xp: ");
    Put(tag);
    _outp(0xE9, ' '); PutHex(a);
    _outp(0xE9, ' '); PutHex(b);
    _outp(0xE9, ' '); PutHex(c);
    _outp(0xE9, '\n');
}

VOID XpTraceBytes(const char *tag, const UCHAR *p, ULONG n)
{
    ULONG i;
    Put("xp: ");
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

/* Player numbers for the 360 pads' LED ring, lowest free first, as Linux
 * xpad_led_probe takes them from an IDA (design 6.5). */
static KSPIN_LOCK PlayerLock;
static ULONG PlayerBits;

LONG XpAllocPlayer(VOID)
{
    KIRQL irql;
    LONG i, got = -1;
    KeAcquireSpinLock(&PlayerLock, &irql);
    for (i = 0; i < 32; i++)
        if (!(PlayerBits & (1UL << i))) {
            PlayerBits |= 1UL << i;
            got = i;
            break;
        }
    KeReleaseSpinLock(&PlayerLock, irql);
    return got;
}

VOID XpFreePlayer(LONG player)
{
    KIRQL irql;
    if (player < 0 || player >= 32)
        return;
    KeAcquireSpinLock(&PlayerLock, &irql);
    PlayerBits &= ~(1UL << player);
    KeReleaseSpinLock(&PlayerLock, irql);
}

/* ---- registry options (design 6.4) ---- */

static VOID ReadKey(HANDLE key, XP_OPTIONS *opt)
{
    static const struct { PCWSTR Name; SIZE_T Offset; } values[] = {
        { L"Layout", FIELD_OFFSET(XP_OPTIONS, Layout) },
        { L"TriggerButtons", FIELD_OFFSET(XP_OPTIONS, TriggerButtons) },
        { L"TriggerThreshold", FIELD_OFFSET(XP_OPTIONS, TriggerThreshold) },
        { L"TopUsage", FIELD_OFFSET(XP_OPTIONS, TopUsage) },
        { L"GuideButton", FIELD_OFFSET(XP_OPTIONS, GuideButton) },
        { L"Deadzone", FIELD_OFFSET(XP_OPTIONS, Deadzone) },
    };
    ULONG i;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        UNICODE_STRING name;
        UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
        PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
        ULONG got;
        RtlInitUnicodeString(&name, values[i].Name);
        if (!NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation, v, sizeof(buf), &got)))
            continue;
        /* REG_DWORD, or the 4-byte REG_BINARY a 9x INF may write. */
        if ((v->Type == REG_DWORD || v->Type == REG_BINARY) && v->DataLength == sizeof(ULONG))
            *(XU32 *)((PUCHAR)opt + values[i].Offset) =
                v->Data[0] | ((XU32)v->Data[1] << 8) | ((XU32)v->Data[2] << 16) | ((XU32)v->Data[3] << 24);
    }
}

/* The driver key (HKR of the 9x INF) first, then the device key
 * ("Device Parameters" on NT), whose values win. Absent means default. */
VOID XpReadOptions(XP_DEV *dev)
{
    static const ULONG which[2] = { PLUGPLAY_REGKEY_DRIVER, PLUGPLAY_REGKEY_DEVICE };
    ULONG i;
    XpOptionsUnset(&dev->Options);
    for (i = 0; i < 2; i++) {
        HANDLE key;
        if (NT_SUCCESS(IoOpenDeviceRegistryKey(dev->Pdo, which[i], KEY_READ, &key))) {
            ReadKey(key, &dev->Options);
            ZwClose(key);
        }
    }
    /* WDM 1.10 is 2000 and later; 98 SE and ME report 1.00 (98) or 1.05 (ME). */
    dev->Options.Win9x = IoIsWdmVersionAvailable(1, 0x10) ? 0 : 1;
    XpOptionsResolve(&dev->Options, dev->Flags);
}

/* ---- the read queue ---- */

static unsigned Collections(XP_DEV *dev)
{
    return dev->Family == XP_FAM_X360W ? dev->NumChan : 1;
}

/* Lock held: the first channel with a changed state, or NULL. */
static XP_CHAN *DirtyChan(XP_DEV *dev)
{
    unsigned c;
    for (c = 0; c < dev->NumChan; c++)
        if (dev->Chan[c].Dirty)
            return &dev->Chan[c];
    return NULL;
}

/* Lock held: writes the report of ch into the read IRP. */
static VOID FillRead(XP_DEV *dev, XP_CHAN *ch, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    XU8 id = (XU8)(Collections(dev) > 1 ? ch->Index + 1 : 0);
    unsigned n = XpBuildReport(&dev->Options, &ch->Pad, id, (XU8 *)irp->UserBuffer,
                               s->Parameters.DeviceIoControl.OutputBufferLength);
    ch->Dirty = FALSE;
#if defined(XP_TRACE_E9)
    if (TraceReads < 40) {
        TraceReads++;
        XPT("fill", s->Parameters.DeviceIoControl.OutputBufferLength, n, id);
        XPTB("report", irp->UserBuffer, n);
    }
#endif
    irp->IoStatus.Information = n;
    irp->IoStatus.Status = n ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}

PIRP XpTakeReadyReadLocked(XP_DEV *dev)
{
    XP_CHAN *ch = DirtyChan(dev);
    PLIST_ENTRY e;
    PIRP irp;
    /* RemoveHeadList is a statement macro in the 2000 DDK. */
    while (ch && !IsListEmpty(&dev->Reads)) {
        e = RemoveHeadList(&dev->Reads);
        irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        if (IoSetCancelRoutine(irp, NULL) == NULL) {
            /* Its cancel routine is running and will complete it; make
             * that routine's RemoveEntryList harmless. */
            InitializeListHead(e);
            continue;
        }
        FillRead(dev, ch, irp);
        return irp;
    }
    return NULL;
}

VOID XpFailReads(XP_DEV *dev, NTSTATUS status)
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

static VOID CancelRead(PDEVICE_OBJECT fdo, PIRP irp)
{
    XP_DEV *dev = XP_DEV_OF(fdo);
    KIRQL irql;
    IoReleaseCancelSpinLock(irp->CancelIrql);
    KeAcquireSpinLock(&dev->Lock, &irql);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&dev->Lock, irql);
    irp->IoStatus.Status = STATUS_CANCELLED;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static NTSTATUS ReadReport(XP_DEV *dev, PIRP irp)
{
    KIRQL irql;
    XP_CHAN *ch;

    KeAcquireSpinLock(&dev->Lock, &irql);
    if (!dev->Started || dev->Stopping) {
        KeReleaseSpinLock(&dev->Lock, irql);
        irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    ch = DirtyChan(dev);
    if (ch) {
        NTSTATUS status;
        FillRead(dev, ch, irp);
        status = irp->IoStatus.Status;
        KeReleaseSpinLock(&dev->Lock, irql);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return status;
    }
    /* Nothing changed: the read waits for the next state change. */
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

static NTSTATUS GetString(XP_DEV *dev, PIRP irp, PIO_STACK_LOCATION s)
{
    ULONG id = (ULONG)((ULONG_PTR)s->Parameters.DeviceIoControl.Type3InputBuffer & 0xFFFF);
    ULONG cap = s->Parameters.DeviceIoControl.OutputBufferLength / sizeof(WCHAR);
    PWCHAR out = (PWCHAR)irp->UserBuffer;
    const char *text;
    ULONG i, n = 0;

    if (id == HID_STRING_ID_IPRODUCT)
        text = dev->Known ? dev->Known->Name : XpFamilyName(dev->Family);
    else if (id == HID_STRING_ID_IMANUFACTURER)
        text = "xpad98";
    else
        return STATUS_INVALID_PARAMETER;
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

static NTSTATUS XpIoctl(PDEVICE_OBJECT fdo, PIRP irp)
{
    XP_DEV *dev = XP_DEV_OF(fdo);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG outLen = s->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status;

    irp->IoStatus.Information = 0;
#if defined(XP_TRACE_E9)
    if (s->Parameters.DeviceIoControl.IoControlCode != IOCTL_HID_READ_REPORT || TraceReads < 40)
        XPT("ioctl", s->Parameters.DeviceIoControl.IoControlCode, outLen,
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
        XPTB("reportdesc", dev->ReportDesc, dev->ReportDescLen);
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
        /* Write reports (no rumble in v1, design 6.6), feature reports,
         * idle notification and anything else. */
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    XPT("ioctl-done", s->Parameters.DeviceIoControl.IoControlCode, status, irp->IoStatus.Information);
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

static NTSTATUS ForwardSync(XP_DEV *dev, PIRP irp)
{
    KEVENT done;
    KeInitializeEvent(&done, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, StartDone, &done, TRUE, TRUE, TRUE);
    if (IoCallDriver(dev->Lower, irp) == STATUS_PENDING)
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
    return irp->IoStatus.Status;
}

static NTSTATUS XpPnp(PDEVICE_OBJECT fdo, PIRP irp)
{
    XP_DEV *dev = XP_DEV_OF(fdo);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status;

    XPT("pnp", s->MinorFunction, 0, 0);
    switch (s->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = ForwardSync(dev, irp);
        XPT("start-lower", status, 0, 0);
        if (NT_SUCCESS(status))
            status = XpUsbStart(dev);
        XPT("start", status, dev->Family, dev->NumChan);
        irp->IoStatus.Status = status;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return status;

    case IRP_MN_STOP_DEVICE:
        XpUsbStop(dev, TRUE);
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        XpUsbStop(dev, FALSE);
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE:
        /* hidclass detaches and deletes its FDO; the minidriver frees only
         * its own resources. */
        XpUsbStop(dev, FALSE);
        XpUsbFree(dev);
        XpFreePlayer(dev->Player);
        dev->Player = -1;
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

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

/* ---- power (design 12: D0/D3 only, start packets again on D0) ---- */

static NTSTATUS D0Done(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    XP_DEV *dev = context;
    UNREFERENCED_PARAMETER(unused);
    if (irp->PendingReturned)
        IoMarkIrpPending(irp);
    PoStartNextPowerIrp(irp);
    if (NT_SUCCESS(irp->IoStatus.Status))
        XpUsbResume(dev);
    return STATUS_SUCCESS;
}

static NTSTATUS XpPower(PDEVICE_OBJECT fdo, PIRP irp)
{
    XP_DEV *dev = XP_DEV_OF(fdo);
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);

    XPT("power", s->MinorFunction, s->Parameters.Power.Type, s->Parameters.Power.State.DeviceState);
    if (s->MinorFunction == IRP_MN_SET_POWER && s->Parameters.Power.Type == DevicePowerState) {
        if (s->Parameters.Power.State.DeviceState == PowerDeviceD0) {
            IoCopyCurrentIrpStackLocationToNext(irp);
            IoSetCompletionRoutine(irp, D0Done, dev, TRUE, TRUE, TRUE);
            return PoCallDriver(dev->Lower, irp);
        } else {
            KIRQL irql;
            /* IN transfers that fail while powered down park until D0. */
            KeAcquireSpinLock(&dev->Lock, &irql);
            dev->PoweredDown = TRUE;
            KeReleaseSpinLock(&dev->Lock, irql);
        }
    }
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(dev->Lower, irp);
}

/* ---- registration ---- */

static NTSTATUS XpAddDevice(PDRIVER_OBJECT driver, PDEVICE_OBJECT fdo)
{
    PHID_DEVICE_EXTENSION hid = (PHID_DEVICE_EXTENSION)fdo->DeviceExtension;
    XP_DEV *dev = (XP_DEV *)hid->MiniDeviceExtension;
    UNREFERENCED_PARAMETER(driver);
    RtlZeroMemory(dev, sizeof(*dev));
    dev->Fdo = fdo;
    dev->Lower = hid->NextDeviceObject;
    dev->Pdo = hid->PhysicalDeviceObject;
    dev->Player = -1;
    KeInitializeSpinLock(&dev->Lock);
    KeInitializeEvent(&dev->IoIdle, NotificationEvent, FALSE);
    InitializeListHead(&dev->Reads);
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static VOID XpUnload(PDRIVER_OBJECT driver)
{
    UNREFERENCED_PARAMETER(driver);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath)
{
    HID_MINIDRIVER_REGISTRATION reg;

    KeInitializeSpinLock(&PlayerLock);
    PlayerBits = 0;
    driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = XpIoctl;
    driver->MajorFunction[IRP_MJ_PNP] = XpPnp;
    driver->MajorFunction[IRP_MJ_POWER] = XpPower;
    driver->DriverExtension->AddDevice = XpAddDevice;
    driver->DriverUnload = XpUnload;

    RtlZeroMemory(&reg, sizeof(reg));
    reg.Revision = HID_REVISION;
    reg.DriverObject = driver;
    reg.RegistryPath = registryPath;
    reg.DeviceExtensionSize = sizeof(XP_DEV);
    reg.DevicesArePolled = FALSE;   /* interrupt IN, not polled */
#if defined(XP_TRACE_E9)
    {
        NTSTATUS st = HidRegisterMinidriver(&reg);
        XPT("driverentry", st, HID_REVISION, sizeof(XP_DEV));
        return st;
    }
#else
    return HidRegisterMinidriver(&reg);
#endif
}

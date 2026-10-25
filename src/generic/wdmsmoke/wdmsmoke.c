/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * wdmsmoke - a disposable WDM function driver that proves the build
 * chain and the basic PnP lifetime (build.md section 6). It attaches to
 * one root-enumerated test devnode, passes every request down, and
 * prints each step with DbgPrint. It claims no USB device and is never
 * released.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */

#include <wdm.h>

typedef struct _SMOKE_EXT {
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT Lower;
    PDEVICE_OBJECT Pdo;
    BOOLEAN Started;
} SMOKE_EXT, *PSMOKE_EXT;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
static NTSTATUS SmokeAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT Pdo);
static VOID SmokeUnload(PDRIVER_OBJECT DriverObject);
static NTSTATUS SmokePnp(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS SmokePower(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS SmokePass(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS SmokeCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = SmokePass;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = SmokeCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = SmokeCreateClose;
    DriverObject->MajorFunction[IRP_MJ_PNP] = SmokePnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = SmokePower;
    DriverObject->DriverExtension->AddDevice = SmokeAddDevice;
    DriverObject->DriverUnload = SmokeUnload;

    DbgPrint("wdmsmoke: DriverEntry\n");
    return STATUS_SUCCESS;
}

static VOID SmokeUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrint("wdmsmoke: Unload\n");
}

static NTSTATUS SmokeAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT fdo;
    PSMOKE_EXT ext;
    NTSTATUS status;

    status = IoCreateDevice(DriverObject, sizeof(SMOKE_EXT), NULL,
                            FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo);
    if (!NT_SUCCESS(status)) {
        DbgPrint("wdmsmoke: IoCreateDevice failed %08X\n", status);
        return status;
    }

    ext = (PSMOKE_EXT)fdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    ext->Self = fdo;
    ext->Pdo = Pdo;
    ext->Lower = IoAttachDeviceToDeviceStack(fdo, Pdo);
    if (ext->Lower == NULL) {
        DbgPrint("wdmsmoke: attach failed\n");
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    fdo->Flags |= DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    DbgPrint("wdmsmoke: AddDevice fdo %p lower %p\n", fdo, ext->Lower);
    return STATUS_SUCCESS;
}

static NTSTATUS SmokeSyncDone(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Sends the IRP down and waits for it; the caller completes it. */
static NTSTATUS SmokeForwardAndWait(PSMOKE_EXT ext, PIRP Irp)
{
    KEVENT done;
    NTSTATUS status;

    KeInitializeEvent(&done, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, SmokeSyncDone, &done, TRUE, TRUE, TRUE);
    status = IoCallDriver(ext->Lower, Irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }
    return status;
}

static NTSTATUS SmokePnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PSMOKE_EXT ext = (PSMOKE_EXT)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_OBJECT lower;
    NTSTATUS status;

    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = SmokeForwardAndWait(ext, Irp);
        if (NT_SUCCESS(status))
            ext->Started = TRUE;
        DbgPrint("wdmsmoke: START_DEVICE %08X\n", status);
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        DbgPrint("wdmsmoke: PnP minor %02X\n", sp->MinorFunction);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_STOP_DEVICE:
        ext->Started = FALSE;
        DbgPrint("wdmsmoke: STOP_DEVICE\n");
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        ext->Started = FALSE;
        DbgPrint("wdmsmoke: SURPRISE_REMOVAL\n");
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE:
        ext->Started = FALSE;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        lower = ext->Lower;
        status = IoCallDriver(lower, Irp);
        IoDetachDevice(lower);
        IoDeleteDevice(DeviceObject);
        DbgPrint("wdmsmoke: REMOVE_DEVICE\n");
        return status;

    default:
        break;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->Lower, Irp);
}

static NTSTATUS SmokePower(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PSMOKE_EXT ext = (PSMOKE_EXT)DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(ext->Lower, Irp);
}

static NTSTATUS SmokePass(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PSMOKE_EXT ext = (PSMOKE_EXT)DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->Lower, Irp);
}

static NTSTATUS SmokeCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

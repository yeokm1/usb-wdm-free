/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original experimental NT2000+ UVC USB transport (see uvcusb.h). Built from
 * USB 2.0, UVC 1.1/1.5 and the published DDK usbdi.h/usbioctl.h/wdm.h
 * declarations; the synchronous-URB and passive completion-worker shape
 * follows this repository's own uac2 uacusb.c. No kit sample, vendor
 * implementation or binary code was used.
 */
#include <wdm.h>
#pragma warning(push)
#pragma warning(disable:4200)
#include <usbdi.h>
#include <usbioctl.h>
#pragma warning(pop)
#include "uvcusb.h"
#include "uvcdesc.h"
#include "uvcneg.h"
#include "uvciso.h"
#include "uvctrace.h"

#define TAG 'uUvU'
#define MAX_SLOTS 6
#define MAX_PACKETS 16

typedef struct UVU_SLOT {
    UVU *Owner;
    PIRP Irp;
    struct _URB_ISOCH_TRANSFER *Urb;
    PUCHAR Buffer;
    ULONG Sequence;
    LONG Phase; /* 0 idle, 1 lower-owned, 2 completed and worker-owned */
    NTSTATUS Status;
} UVU_SLOT;

struct UVU {
    PDEVICE_OBJECT Lower;
    PUCHAR Config;
    ULONG ConfigLength;
    UVC_DESC *Desc;
    unsigned int Speed, Control;
    USBD_CONFIGURATION_HANDLE Configuration;
    USBD_PIPE_HANDLE Pipe;
    UCHAR Interface, Alternate;
    UVC_NEG_RESULT Neg;
    UVC_ISO_PLAN Plan;
    UVU_SLOT Slot[MAX_SLOTS];
    ULONG NextSequence, NextComplete, LastStartFrame, FailedRun;
    NTSTATUS FirstFailStatus;
    BOOLEAN QuirkBandwidth;
    ULONG ReqFormat, ReqFrame; /* last PROBE request (diagnostics) */
    ULONG BandwidthNeed;
    USBD_STATUS FirstFailUsbd;
    BOOLEAN HaveStartFrame, Stop, Running;
    UVC_PAY Pay;
    UVC_ISO_STATS Iso;
    PUCHAR Assembly, Latest;
    ULONG FrameBytes, LatestBytes, LatestSequence;
    KSPIN_LOCK Lock;
    KEVENT Wake, FrameReady, Exited; /* Exited: the worker's last act */
    BOOLEAN WaitThread;               /* WDM 1.10+: also wait the thread object */
    FAST_MUTEX FrameLock;
    PVOID Thread;
    NTSTATUS LastStatus;
};

/* ---- synchronous URBs (PASSIVE) ---- */
static NTSTATUS SyncDone(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    UNREFERENCED_PARAMETER(device); UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}
/* The deadline requests cancellation; only the observed completion lets the
 * IRP, URB and buffers go (same rule as uac2's CallAndDrain). */
static NTSTATUS CallAndDrain(PDEVICE_OBJECT lower, PIRP irp, PKEVENT event)
{
    LARGE_INTEGER timeout;
    NTSTATUS status;
    IoSetCompletionRoutine(irp, SyncDone, event, TRUE, TRUE, TRUE);
    (void)IoCallDriver(lower, irp);
    timeout.QuadPart = -50000000; /* 5 s */
    if (KeWaitForSingleObject(event, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
        (void)IoCancelIrp(irp);
        KeWaitForSingleObject(event, Executive, KernelMode, FALSE, NULL);
        status = irp->IoStatus.Status;
        if (status == STATUS_CANCELLED) status = STATUS_IO_TIMEOUT;
    } else status = irp->IoStatus.Status;
    IoFreeIrp(irp);
    return status;
}
static NTSTATUS UsbRequest(UVU *u, PURB urb)
{
    PIRP irp = IoAllocateIrp(u->Lower->StackSize, FALSE);
    PIO_STACK_LOCATION stack;
    KEVENT event;
    NTSTATUS status;
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    stack = IoGetNextIrpStackLocation(irp);
    RtlZeroMemory(stack, sizeof(*stack));
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = urb;
    status = CallAndDrain(u->Lower, irp, &event);
    if (NT_SUCCESS(status) && !USBD_SUCCESS(urb->UrbHeader.Status))
        status = STATUS_DEVICE_DATA_ERROR;
    return status;
}
static NTSTATUS GetDescriptor(UVU *u, UCHAR type, PVOID buffer, ULONG *length)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
    NTSTATUS status;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    urb.TransferBuffer = buffer; urb.TransferBufferLength = *length;
    urb.DescriptorType = type; urb.Index = 0;
    status = UsbRequest(u, (PURB)&urb);
    *length = urb.TransferBufferLength;
    return status;
}

/* ---- speed (runtime-probed on XP+, inferred on 2000) ---- */
/* ABI facts: WDK 7.1 usbbusif.h USB_BUS_INTERFACE_USBDI_V1 (as in uac2). */
typedef struct USB_SPEED_INTERFACE {
    USHORT Size, Version;
    PVOID BusContext;
    PINTERFACE_REFERENCE Reference;
    PINTERFACE_DEREFERENCE Dereference;
    PVOID OpaqueCallbacks[4];
    BOOLEAN (__stdcall *IsHighSpeed)(PVOID);
} USB_SPEED_INTERFACE;
static int QuerySpeed(UVU *u)
{
    static const GUID id = { 0xb1a96a13,0x3de0,0x4574,{0x9b,1,0xc0,0x8f,0xea,0xb3,0x18,0xd6} };
    USB_SPEED_INTERFACE bus;
    PIRP irp;
    PIO_STACK_LOCATION stack;
    KEVENT event;
    int speed = 0;
    RtlZeroMemory(&bus, sizeof(bus));
    irp = IoAllocateIrp(u->Lower->StackSize, FALSE);
    if (!irp) return 0;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    stack = IoGetNextIrpStackLocation(irp);
    RtlZeroMemory(stack, sizeof(*stack));
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = (LPGUID)&id;
    stack->Parameters.QueryInterface.Size = sizeof(bus);
    stack->Parameters.QueryInterface.Version = 1;
    stack->Parameters.QueryInterface.Interface = (PINTERFACE)&bus;
    if (NT_SUCCESS(CallAndDrain(u->Lower, irp, &event)) &&
        bus.Size >= sizeof(bus) && bus.Version >= 1 && bus.IsHighSpeed && bus.Dereference)
        speed = bus.IsHighSpeed(bus.BusContext) ? UVC_DESC_HIGH_SPEED : UVC_DESC_FULL_SPEED;
    if (bus.Dereference) bus.Dereference(bus.BusContext);
    return speed;
}

/* ---- descriptors and configuration ---- */
static NTSTATUS FindControl(UVU *u)
{
    ULONG o = 0;
    while (o + 2 <= u->ConfigLength && u->Config[o] >= 2 && o + u->Config[o] <= u->ConfigLength) {
        const UCHAR *d = u->Config + o;
        if (d[1] == 4 && d[0] >= 9 && d[5] == 0x0E && d[6] == 0x01 && d[3] == 0) {
            u->Control = d[2]; return STATUS_SUCCESS;
        }
        o += d[0];
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}
static NTSTATUS Parse(UVU *u)
{
    unsigned int i, wide = 0;
    int speed = QuerySpeed(u);
    if (speed) {
        if (UvcDescParse(u->Config, u->ConfigLength, u->Control, speed, u->Desc) != UVC_DESC_OK)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        u->Speed = speed; return STATUS_SUCCESS;
    }
    /* 2000 has no speed interface. A high-bandwidth or >1023-byte isochronous
     * packet only exists at High Speed; otherwise prefer a Full Speed reading
     * and fall back to High Speed. This inference is recorded in the stats. */
    if (UvcDescParse(u->Config, u->ConfigLength, u->Control, UVC_DESC_HIGH_SPEED, u->Desc) == UVC_DESC_OK) {
        for (i = 0; i < u->Desc->AltCount; i++)
            if (u->Desc->Alt[i].Transactions > 1 || u->Desc->Alt[i].PacketBytes > 1023) wide = 1;
        if (wide) { u->Speed = UVC_DESC_HIGH_SPEED; return STATUS_SUCCESS; }
    }
    if (UvcDescParse(u->Config, u->ConfigLength, u->Control, UVC_DESC_FULL_SPEED, u->Desc) == UVC_DESC_OK) {
        u->Speed = UVC_DESC_FULL_SPEED; return STATUS_SUCCESS;
    }
    if (UvcDescParse(u->Config, u->ConfigLength, u->Control, UVC_DESC_HIGH_SPEED, u->Desc) == UVC_DESC_OK) {
        u->Speed = UVC_DESC_HIGH_SPEED; return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}
/* Every interface at alternate 0 (design section 4: USBD may refuse a
 * subset); pipes are opened only on our own interfaces later. */
static NTSTATUS Configure(UVU *u)
{
    struct _URB_SELECT_CONFIGURATION *urb;
    PUSBD_INTERFACE_INFORMATION info;
    ULONG o, size, entry = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes), i;
    NTSTATUS status;
    size = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface);
    for (o = 0; o + 9 <= u->ConfigLength && u->Config[o] >= 2; o += u->Config[o])
        if (u->Config[o + 1] == 4 && u->Config[o + 3] == 0)
            size += entry + u->Config[o + 4] * sizeof(USBD_PIPE_INFORMATION);
    urb = ExAllocatePoolWithTag(NonPagedPool, size, TAG);
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, size);
    urb->Hdr.Length = (USHORT)size;
    urb->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb->ConfigurationDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)u->Config;
    info = &urb->Interface;
    for (o = 0; o + 9 <= u->ConfigLength && u->Config[o] >= 2; o += u->Config[o]) {
        if (u->Config[o + 1] != 4 || u->Config[o + 3] != 0) continue;
        info->Length = (USHORT)(entry + u->Config[o + 4] * sizeof(USBD_PIPE_INFORMATION));
        info->InterfaceNumber = u->Config[o + 2];
        info->AlternateSetting = 0;
        info->NumberOfPipes = u->Config[o + 4];
        for (i = 0; i < info->NumberOfPipes; i++) info->Pipes[i].MaximumTransferSize = PAGE_SIZE;
        info = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)info + info->Length);
    }
    status = UsbRequest(u, (PURB)urb);
    if (NT_SUCCESS(status)) {
        u->Configuration = urb->ConfigurationHandle;
        if (!u->Configuration) status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    ExFreePool(urb);
    return status;
}
static NTSTATUS SelectAlt(UVU *u, UCHAR alternate, ULONG transferBytes, UCHAR endpoint)
{
    struct _URB_SELECT_INTERFACE *urb;
    ULONG pipes = alternate ? 1 : 0;
    ULONG size = FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface) +
        FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) + pipes * sizeof(USBD_PIPE_INFORMATION);
    NTSTATUS status;
    urb = ExAllocatePoolWithTag(NonPagedPool, size, TAG);
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, size);
    urb->Hdr.Length = (USHORT)size; urb->Hdr.Function = URB_FUNCTION_SELECT_INTERFACE;
    urb->ConfigurationHandle = u->Configuration;
    urb->Interface.Length = (USHORT)(size - FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface));
    urb->Interface.InterfaceNumber = u->Interface; urb->Interface.AlternateSetting = alternate;
    urb->Interface.NumberOfPipes = pipes;
    if (pipes) urb->Interface.Pipes[0].MaximumTransferSize = transferBytes;
    status = UsbRequest(u, (PURB)urb);
    if (!NT_SUCCESS(status)) UVHEX("UVU_SELECT_USBD ", urb->Hdr.Status);
    if (NT_SUCCESS(status)) {
        u->Pipe = NULL; u->Alternate = alternate;
        if (pipes) {
            PUSBD_PIPE_INFORMATION p = &urb->Interface.Pipes[0];
            if (p->PipeType != UsbdPipeTypeIsochronous || p->EndpointAddress != endpoint)
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
            else u->Pipe = p->PipeHandle;
        }
    }
    ExFreePool(urb);
    return status;
}

/* ---- PROBE/COMMIT through the portable sequence ---- */
static int ClassXfer(void *context, unsigned int request, unsigned int selector,
    unsigned int iface, unsigned char *buffer, unsigned int bytes)
{
    UVU *u = (UVU *)context;
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST urb;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_CLASS_INTERFACE;
    urb.TransferFlags = (request & 0x80) ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0;
    urb.TransferBuffer = buffer; urb.TransferBufferLength = bytes;
    urb.Request = (UCHAR)request;
    urb.Value = (USHORT)(selector << 8);
    urb.Index = (USHORT)iface;
    if (!NT_SUCCESS(UsbRequest(u, (PURB)&urb))) return -1;
    return (int)urb.TransferBufferLength;
}

/* ---- isochronous stream (worker thread, PASSIVE) ---- */
static VOID Deliver(void *context, const unsigned char *frame, unsigned int bytes, unsigned int still)
{
    UVU *u = (UVU *)context;
    UNREFERENCED_PARAMETER(still);
    if (bytes != u->FrameBytes) return;
    ExAcquireFastMutex(&u->FrameLock);
    RtlCopyMemory(u->Latest, frame, bytes);
    u->LatestBytes = bytes; u->LatestSequence++;
    ExReleaseFastMutex(&u->FrameLock);
    KeSetEvent(&u->FrameReady, IO_NO_INCREMENT, FALSE);
}
static NTSTATUS SlotDone(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    UVU_SLOT *slot = (UVU_SLOT *)context;
    UVU *u = slot->Owner;
    KIRQL old;
    UNREFERENCED_PARAMETER(device);
    KeAcquireSpinLock(&u->Lock, &old);
    slot->Status = irp->IoStatus.Status;
    slot->Phase = 2;
    KeSetEvent(&u->Wake, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&u->Lock, old);
    return STATUS_MORE_PROCESSING_REQUIRED;
}
static NTSTATUS Submit(UVU *u, UVU_SLOT *slot)
{
    ULONG i, n = u->Plan.PacketsPerUrb;
    ULONG urbBytes = FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, IsoPacket) + n * sizeof(USBD_ISO_PACKET_DESCRIPTOR);
    PIO_STACK_LOCATION stack;
    KIRQL old;
    RtlZeroMemory(slot->Urb, urbBytes);
    slot->Urb->Hdr.Length = (USHORT)urbBytes;
    slot->Urb->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    slot->Urb->PipeHandle = u->Pipe;
    slot->Urb->TransferFlags = USBD_START_ISO_TRANSFER_ASAP | USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    slot->Urb->NumberOfPackets = n;
    slot->Urb->TransferBuffer = slot->Buffer;
    slot->Urb->TransferBufferLength = u->Plan.UrbBytes;
    for (i = 0; i < n; i++) slot->Urb->IsoPacket[i].Offset = i * u->Plan.PacketStride;
    slot->Irp = IoAllocateIrp(u->Lower->StackSize, FALSE);
    if (!slot->Irp) return STATUS_INSUFFICIENT_RESOURCES;
    slot->Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    stack = IoGetNextIrpStackLocation(slot->Irp);
    RtlZeroMemory(stack, sizeof(*stack));
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = slot->Urb;
    IoSetCompletionRoutine(slot->Irp, SlotDone, slot, TRUE, TRUE, TRUE);
    KeAcquireSpinLock(&u->Lock, &old);
    if (u->Stop) {
        KeReleaseSpinLock(&u->Lock, old);
        IoFreeIrp(slot->Irp); slot->Irp = NULL;
        return STATUS_DELETE_PENDING;
    }
    slot->Phase = 1; slot->Sequence = u->NextSequence++;
    KeReleaseSpinLock(&u->Lock, old);
    (void)IoCallDriver(u->Lower, slot->Irp);
    return STATUS_SUCCESS;
}
static VOID Consume(UVU *u, UVU_SLOT *slot)
{
    UVC_ISO_PACKET p[MAX_PACKETS];
    ULONG i, n = u->Plan.PacketsPerUrb, frames = u->Speed == UVC_DESC_HIGH_SPEED ? n / 8 : n;
    int ok = NT_SUCCESS(slot->Status), contiguous = 1;
    if (ok) {
        ULONG start = slot->Urb->StartFrame;
        if (u->HaveStartFrame && start != u->LastStartFrame + frames) contiguous = 0;
        u->LastStartFrame = start; u->HaveStartFrame = TRUE;
        for (i = 0; i < n; i++) {
            p[i].Offset = slot->Urb->IsoPacket[i].Offset;
            p[i].Length = slot->Urb->IsoPacket[i].Length;
            p[i].Status = USBD_SUCCESS(slot->Urb->IsoPacket[i].Status) ? 0 : (ULONG)slot->Urb->IsoPacket[i].Status;
        }
    } else u->HaveStartFrame = FALSE;
    (void)UvcIsoFeed(&u->Pay, &u->Iso, slot->Buffer, u->Plan.UrbBytes, u->Plan.PacketCapacity,
        p, ok ? n : 0, ok, contiguous);
}
static VOID Worker(PVOID context)
{
    UVU *u = (UVU *)context;
    ULONG i, outstanding;
    KIRQL old;
    for (;;) {
        BOOLEAN stop, progressed = TRUE;
        KeWaitForSingleObject(&u->Wake, Executive, KernelMode, FALSE, NULL);
        KeAcquireSpinLock(&u->Lock, &old); stop = u->Stop; KeReleaseSpinLock(&u->Lock, old);
        if (stop) break;
        /* Consume completions strictly in submission order. */
        while (progressed) {
            UVU_SLOT *slot = NULL;
            progressed = FALSE;
            /* Oldest completed transfer first. A transfer that the stack holds
             * back does not block the others: StartFrame continuity in Consume
             * reports any hole as loss to the payload core. */
            KeAcquireSpinLock(&u->Lock, &old);
            for (i = 0; i < u->Plan.Urbs; i++)
                if (u->Slot[i].Phase == 2 &&
                    (!slot || (LONG)(u->Slot[i].Sequence - slot->Sequence) < 0)) slot = &u->Slot[i];
            KeReleaseSpinLock(&u->Lock, old);
            if (!slot) break;
            if (!NT_SUCCESS(slot->Status) && NT_SUCCESS(u->FirstFailStatus)) {
                u->FirstFailStatus = slot->Status; u->FirstFailUsbd = slot->Urb->Hdr.Status;
            }
            IoFreeIrp(slot->Irp); slot->Irp = NULL;
            /* A failed or unexpectedly cancelled transfer is fed as loss and
             * resubmitted like any other; only Stop ends the stream. */
            Consume(u, slot);
            slot->Phase = 0; u->NextComplete++;
            /* A gone or stopped device fails every transfer at once; resubmitting
             * forever would spin this thread and starve PnP removal. After a
             * run of whole-transfer failures, stop and keep the status. */
            if (NT_SUCCESS(slot->Status)) u->FailedRun = 0;
            else if (++u->FailedRun >= 8) {
                KeAcquireSpinLock(&u->Lock, &old);
                if (NT_SUCCESS(u->LastStatus)) u->LastStatus = slot->Status;
                u->Stop = TRUE;
                KeReleaseSpinLock(&u->Lock, old);
                break;
            }
            if (!NT_SUCCESS(Submit(u, slot))) {
                KeAcquireSpinLock(&u->Lock, &old);
                if (NT_SUCCESS(u->LastStatus)) u->LastStatus = STATUS_DEVICE_DATA_ERROR;
                u->Stop = TRUE;
                KeReleaseSpinLock(&u->Lock, old);
                break;
            }
            progressed = TRUE;
        }
        KeAcquireSpinLock(&u->Lock, &old); stop = u->Stop; KeReleaseSpinLock(&u->Lock, old);
        if (stop) break; /* includes a resubmission failure set above */
    }
    /* Drain. On 2000 a cancelled isochronous URB on a surprise-removed device
     * was observed never to complete (the hub flushes the PDO only after our
     * REMOVE). Abort the pipe first, which completes every queued transfer,
     * then cancel whatever is still lower-owned and wait for each completion. */
    if (u->Pipe) {
        struct _URB_PIPE_REQUEST abort;
        NTSTATUS status;
        RtlZeroMemory(&abort, sizeof(abort));
        abort.Hdr.Length = sizeof(abort);
        abort.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
        abort.PipeHandle = u->Pipe;
        status = UsbRequest(u, (PURB)&abort);
        UVHEX("UVU_ABORT_PIPE ",status);
    }
    for (;;) {
        outstanding = 0;
        for (i = 0; i < u->Plan.Urbs; i++) {
            KeAcquireSpinLock(&u->Lock, &old);
            if (u->Slot[i].Phase == 1) { outstanding++; KeReleaseSpinLock(&u->Lock, old); IoCancelIrp(u->Slot[i].Irp); }
            else {
                if (u->Slot[i].Phase == 2) { IoFreeIrp(u->Slot[i].Irp); u->Slot[i].Irp = NULL; u->Slot[i].Phase = 0; }
                KeReleaseSpinLock(&u->Lock, old);
            }
        }
        if (!outstanding) break;
        UVHEX("UVU_DRAIN_OUTSTANDING ",outstanding);
        {   /* Re-issue cancellation periodically; only completion frees. */
            LARGE_INTEGER t; t.QuadPart = -1000000; /* 100 ms */
            KeWaitForSingleObject(&u->Wake, Executive, KernelMode, FALSE, &t);
        }
    }
    UVTRACE("UVU_DRAINED\n");
    KeSetEvent(&u->Exited, IO_NO_INCREMENT, FALSE); /* last access to u */
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- public ---- */
NTSTATUS UvuOpen(PDEVICE_OBJECT lower, UVU **out)
{
    UVU *u;
    UCHAR header[9];
    ULONG length = sizeof(header);
    NTSTATUS status;
    *out = NULL;
    if (!lower) return STATUS_INVALID_PARAMETER;
    u = ExAllocatePoolWithTag(NonPagedPool, sizeof(*u), TAG);
    if (!u) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(u, sizeof(*u));
    u->Lower = lower;
    KeInitializeSpinLock(&u->Lock);
    KeInitializeEvent(&u->Wake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&u->FrameReady, SynchronizationEvent, FALSE);
    KeInitializeEvent(&u->Exited, NotificationEvent, FALSE);
    ExInitializeFastMutex(&u->FrameLock);
    u->Desc = ExAllocatePoolWithTag(NonPagedPool, sizeof(UVC_DESC), TAG);
    status = u->Desc ? GetDescriptor(u, USB_CONFIGURATION_DESCRIPTOR_TYPE, header, &length) : STATUS_INSUFFICIENT_RESOURCES;
    if (NT_SUCCESS(status) && (length != sizeof(header) || header[1] != 2)) status = STATUS_DEVICE_DATA_ERROR;
    if (NT_SUCCESS(status)) {
        u->ConfigLength = header[2] | ((ULONG)header[3] << 8);
        if (u->ConfigLength < 9 || u->ConfigLength > 65535) status = STATUS_DEVICE_DATA_ERROR;
    }
    if (NT_SUCCESS(status)) {
        u->Config = ExAllocatePoolWithTag(NonPagedPool, u->ConfigLength, TAG);
        length = u->ConfigLength;
        status = u->Config ? GetDescriptor(u, USB_CONFIGURATION_DESCRIPTOR_TYPE, u->Config, &length) : STATUS_INSUFFICIENT_RESOURCES;
        if (NT_SUCCESS(status) && length != u->ConfigLength) status = STATUS_DEVICE_DATA_ERROR;
    }
    if (NT_SUCCESS(status)) {
        /* Per-camera quirk table, keyed on VID/PID from the device descriptor. */
        UCHAR device[18]; ULONG n = sizeof(device);
        if (NT_SUCCESS(GetDescriptor(u, USB_DEVICE_DESCRIPTOR_TYPE, device, &n)) && n == sizeof(device) &&
            device[8] == 0x6D && device[9] == 0x04 && device[10] == 0x4C && device[11] == 0x09)
            u->QuirkBandwidth = TRUE; /* Logitech Brio 100, 046D:094C */
    }
    if (NT_SUCCESS(status)) status = FindControl(u);
    if (NT_SUCCESS(status)) status = Parse(u);
    if (NT_SUCCESS(status)) status = Configure(u);
    if (!NT_SUCCESS(status)) { u->LastStatus = status; UvuClose(u); return status; }
    *out = u;
    return STATUS_SUCCESS;
}

/* Per-stream allocations (transfer slots, assembly and latest frame) are
 * released at every stop, so the next start may choose another frame size. */
static VOID FreeStream(UVU *u)
{
    ULONG i;
    for (i = 0; i < MAX_SLOTS; i++) {
        if (u->Slot[i].Urb) ExFreePool(u->Slot[i].Urb);
        if (u->Slot[i].Buffer) ExFreePool(u->Slot[i].Buffer);
        u->Slot[i].Urb = NULL; u->Slot[i].Buffer = NULL;
    }
    if (u->Assembly) ExFreePool(u->Assembly);
    if (u->Latest) ExFreePool(u->Latest);
    u->Assembly = u->Latest = NULL;
    u->LatestBytes = 0;
}

const UVC_DESC *UvuDescriptors(UVU *u)
{
    return u ? u->Desc : NULL;
}

NTSTATUS UvuStart(UVU *u, unsigned int kind, unsigned int width, unsigned int height)
{
    unsigned int f;
    if (!u) return STATUS_INVALID_DEVICE_STATE;
    for (f = 0; f < u->Desc->FrameCount; f++)
        if (u->Desc->Frame[f].Kind == kind && u->Desc->Frame[f].Width == width &&
            u->Desc->Frame[f].Height == height) break;
    if (f == u->Desc->FrameCount) return STATUS_NOT_SUPPORTED;
    return UvuStartFrame(u, f);
}

NTSTATUS UvuStartFrame(UVU *u, unsigned int f)
{
    unsigned int kind;
    ULONG limit;
    const UVC_DESC_ALT *alt;
    OBJECT_ATTRIBUTES a;
    HANDLE thread;
    NTSTATUS status;
    ULONG i;
    if (!u || u->Running) return STATUS_INVALID_DEVICE_STATE;
    if (f >= u->Desc->FrameCount) return STATUS_NOT_SUPPORTED;
    FreeStream(u); /* a failed earlier start may have left allocations */
    kind = u->Desc->Frame[f].Kind;
    limit = 0;
    if (u->QuirkBandwidth && kind != UVC_DESC_MJPEG) {
        /* Per-camera quirk (device evidence: uvc-m2 run log). The camera
         * answers its largest payload (3060, a high-bandwidth alternate) for
         * every frame. An uncompressed stream needs only frame bytes x frames
         * per second spread over the (micro)frames plus a 12-byte payload
         * header each; commit that (rounded up to the smallest alternate
         * that carries it) so the camera itself limits its payloads. */
        ULONG interval = u->Desc->Frame[f].DefaultInterval ? u->Desc->Frame[f].DefaultInterval : 1;
        ULONG slots = u->Speed == UVC_DESC_HIGH_SPEED ? 8000 : 1000;
        ULONG perSecond = (10000000 + interval - 1) / interval;
        unsigned int i, best = UVC_DESC_MAX_ALTS;
        /* 32-bit, rounded up per slot (slightly generous; no 64-bit helpers). */
        if (perSecond > 1000) perSecond = 1000;
        u->BandwidthNeed = ((u->Desc->Frame[f].FrameBytes + slots - 1) / slots) * perSecond + 12;
        for (i = 0; i < u->Desc->AltCount; i++)
            if (u->Desc->Alt[i].Stream == u->Desc->Frame[f].Stream && u->Desc->Alt[i].Type == 1 &&
                u->Desc->Alt[i].Capacity >= u->BandwidthNeed &&
                (best == UVC_DESC_MAX_ALTS || u->Desc->Alt[i].Capacity < u->Desc->Alt[best].Capacity)) best = i;
        if (best != UVC_DESC_MAX_ALTS) limit = u->Desc->Alt[best].Capacity;
    }
    u->ReqFormat = u->Desc->Frame[f].Format; u->ReqFrame = u->Desc->Frame[f].Frame;
    /* PROBE/COMMIT only with the interface idle. Run 9 (uvc-m2 record): a
     * stop whose alternate-0 select failed left the camera streaming, and
     * it then answered PROBE with the frame it was already sending. Retry
     * that select now; still failing, refuse to start. (An unconditional
     * select of alternate 0 straight after configuration failed on 2000,
     * run 11, so it is done only when a stop left another alternate.) */
    if (u->Alternate) {
        status = SelectAlt(u, 0, 0, 0);
        UVHEX("UVU_IDLE_ALT0 ", status);
        if (!NT_SUCCESS(status)) return status;
    }
    status = UvcNegotiateLimited(u->Desc, f, 0, limit, ClassXfer, u, &u->Neg);
    UVHEX("UVU_NEG ", status);
    UVHEX("UVU_REQ_FORMAT ", u->ReqFormat); UVHEX("UVU_REQ_FRAME ", u->ReqFrame);
    UVHEX("UVU_DEV_FORMAT ", u->Neg.DeviceControl.Format); UVHEX("UVU_DEV_FRAME ", u->Neg.DeviceControl.Frame);
    UVHEX("UVU_DEV_INTERVAL ", u->Neg.DeviceControl.Interval);
    UVHEX("UVU_DEV_FRAMEBYTES ", u->Neg.DeviceControl.FrameBytes);
    UVHEX("UVU_DEV_PAYLOAD ", u->Neg.DeviceControl.PayloadBytes);
    UVHEX("UVU_RESOLVED ", u->Neg.FrameIndex);
    if (status != UVC_NEG_OK) return STATUS_DEVICE_CONFIGURATION_ERROR;
    /* The camera may answer another frame than the one asked for; streaming
     * it would never fill a READ of the selected size. Refuse instead. */
    if (u->Neg.FrameIndex != f) return STATUS_DEVICE_CONFIGURATION_ERROR;
    alt = &u->Desc->Alt[u->Neg.AltIndex];
    u->Interface = (UCHAR)u->Desc->Stream[alt->Stream].Interface;
    if (!UvcIsoPlan(u->Speed == UVC_DESC_HIGH_SPEED ? UVC_ISO_HIGH_SPEED : UVC_ISO_FULL_SPEED,
            alt->Type, alt->PacketBytes, alt->Transactions, alt->ServiceMicroframes, MAX_SLOTS, &u->Plan) ||
        u->Plan.PacketsPerUrb > MAX_PACKETS) return STATUS_NOT_SUPPORTED;
    u->FrameBytes = u->Desc->Frame[u->Neg.FrameIndex].FrameBytes;
    if (!u->FrameBytes || u->FrameBytes > 16 * 1024 * 1024) return STATUS_NOT_SUPPORTED;
    u->Assembly = ExAllocatePoolWithTag(NonPagedPool, u->FrameBytes, TAG);
    u->Latest = ExAllocatePoolWithTag(NonPagedPool, u->FrameBytes, TAG);
    if (!u->Assembly || !u->Latest) return STATUS_INSUFFICIENT_RESOURCES;
    if (!UvcPayInit(&u->Pay, u->Assembly, u->FrameBytes, u->FrameBytes, Deliver, u)) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < u->Plan.Urbs; i++) {
        UVU_SLOT *s = &u->Slot[i];
        s->Owner = u;
        s->Urb = ExAllocatePoolWithTag(NonPagedPool, FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, IsoPacket) +
            MAX_PACKETS * sizeof(USBD_ISO_PACKET_DESCRIPTOR), TAG);
        s->Buffer = ExAllocatePoolWithTag(NonPagedPool, u->Plan.UrbBytes, TAG);
        if (!s->Urb || !s->Buffer) return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = SelectAlt(u, (UCHAR)alt->Alternate, u->Plan.UrbBytes, (UCHAR)alt->Endpoint);
    if (!NT_SUCCESS(status)) return status;
    u->Stop = FALSE; u->NextSequence = u->NextComplete = 0; u->HaveStartFrame = FALSE;
    /* 98 SE: no OBJ_KERNEL_HANDLE, and no wait on a thread object (it
     * faulted there in usbmidi's stop path); the stop waits for Exited. */
    UNREFERENCED_PARAMETER(a);
    KeClearEvent(&u->Exited);
    u->WaitThread = IoIsWdmVersionAvailable(1, 0x10);
    status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, Worker, u);
    if (!NT_SUCCESS(status)) { (void)SelectAlt(u, 0, 0, 0); return status; }
    if (u->WaitThread)
        status = ObReferenceObjectByHandle(thread, SYNCHRONIZE, NULL, KernelMode, &u->Thread, NULL);
    else { u->Thread = (PVOID)thread; status = STATUS_SUCCESS; } /* marker only */
    ZwClose(thread);
    if (!NT_SUCCESS(status)) {
        /* Cannot join without the object: stop it and give it time to exit. */
        KIRQL old; LARGE_INTEGER t;
        KeAcquireSpinLock(&u->Lock, &old); u->Stop = TRUE; KeReleaseSpinLock(&u->Lock, old);
        KeSetEvent(&u->Wake, IO_NO_INCREMENT, FALSE);
        t.QuadPart = -10000000; KeDelayExecutionThread(KernelMode, FALSE, &t);
        (void)SelectAlt(u, 0, 0, 0); return status;
    }
    u->Running = TRUE;
    for (i = 0; i < u->Plan.Urbs; i++) {
        status = Submit(u, &u->Slot[i]);
        if (!NT_SUCCESS(status)) { u->LastStatus = status; UvuStop(u); return status; }
    }
    return STATUS_SUCCESS;
}

ULONG UvuCopyLatest(UVU *u, PVOID dest, ULONG destBytes, ULONG *sequence, ULONG timeoutMs)
{
    LARGE_INTEGER t;
    ULONG bytes = 0, waited = 0;
    if (!u || !u->Running || !dest || !sequence) return 0;
    for (;;) {
        ExAcquireFastMutex(&u->FrameLock);
        if (u->LatestSequence != *sequence && u->LatestBytes == destBytes) {
            RtlCopyMemory(dest, u->Latest, destBytes);
            bytes = destBytes; *sequence = u->LatestSequence;
        }
        ExReleaseFastMutex(&u->FrameLock);
        if (bytes || waited >= timeoutMs || u->Stop) return bytes;
        t.QuadPart = -500000; /* 50 ms */
        KeWaitForSingleObject(&u->FrameReady, Executive, KernelMode, FALSE, &t);
        waited += 50;
    }
}

VOID UvuStop(UVU *u)
{
    KIRQL old;
    NTSTATUS status;
    if (!u || !u->Running) return;
    UVTRACE("UVU_STOP\n");
    KeAcquireSpinLock(&u->Lock, &old); u->Stop = TRUE; KeReleaseSpinLock(&u->Lock, old);
    KeSetEvent(&u->Wake, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(&u->Exited, Executive, KernelMode, FALSE, NULL);
    if (u->WaitThread) {
        KeWaitForSingleObject(u->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(u->Thread);
    } else {
        LARGE_INTEGER t; t.QuadPart = -100000; /* 10 ms: let it leave our code */
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    UVTRACE("UVU_JOINED\n");
    u->Thread = NULL;
    u->Running = FALSE;
    status = SelectAlt(u, 0, 0, 0);
    UVHEX("UVU_ALT0 ",status);
    KeSetEvent(&u->FrameReady, IO_NO_INCREMENT, FALSE);
    FreeStream(u);
}

VOID UvuClose(UVU *u)
{
    if (!u) return;
    UvuStop(u);
    FreeStream(u);
    if (u->Config) ExFreePool(u->Config);
    if (u->Desc) ExFreePool(u->Desc);
    ExFreePool(u);
}

ULONG UvuLatestSequence(UVU *u)
{
    ULONG sequence;
    if (!u) return 0;
    ExAcquireFastMutex(&u->FrameLock); sequence = u->LatestSequence; ExReleaseFastMutex(&u->FrameLock);
    return sequence;
}

VOID UvuGetStats(UVU *u, UVU_STATS *s)
{
    RtlZeroMemory(s, sizeof(*s));
    if (!u) return;
    s->Frames = u->Pay.Frames; s->Dropped = u->Pay.Dropped;
    s->Urbs = u->Iso.Urbs; s->FailedUrbs = u->Iso.FailedUrbs; s->Gaps = u->Iso.Gaps;
    s->Packets = u->Iso.Packets; s->Empty = u->Iso.Empty; s->Failed = u->Iso.Failed;
    s->Invalid = u->Iso.Invalid;
    s->Alternate = u->Alternate; s->PayloadBytes = u->Neg.Control.PayloadBytes;
    s->FrameBytes = u->FrameBytes; s->Speed = u->Speed;
    s->PacketsPerUrb = u->Plan.PacketsPerUrb; s->Urbs_ = u->Plan.Urbs; s->Stride = u->Plan.PacketStride;
    s->LastStatus = u->LastStatus;
    s->FirstFailStatus = u->FirstFailStatus; s->FirstFailUsbd = (ULONG)u->FirstFailUsbd;
    s->Submitted = u->NextSequence; s->BandwidthNeed = u->BandwidthNeed; s->Quirk = u->QuirkBandwidth;
    s->ReqFormat = u->ReqFormat; s->ReqFrame = u->ReqFrame;
    s->DevFormat = u->Neg.DeviceControl.Format; s->DevFrame = u->Neg.DeviceControl.Frame;
    s->DevInterval = u->Neg.DeviceControl.Interval; s->DevFrameBytes = u->Neg.DeviceControl.FrameBytes;
    s->DevPayload = u->Neg.DeviceControl.PayloadBytes; s->CommitPayload = u->Neg.Control.PayloadBytes;
    if (u->Neg.Requests && u->Neg.FrameIndex < u->Desc->FrameCount) {
        s->ResolvedWidth = u->Desc->Frame[u->Neg.FrameIndex].Width;
        s->ResolvedHeight = u->Desc->Frame[u->Neg.FrameIndex].Height;
    }
    s->Eofs = u->Pay.Eofs; s->FidFlips = u->Pay.FidFlips; s->DropBad = u->Pay.DropBad;
    s->DropSize = u->Pay.DropSize; s->LastDropUsed = u->Pay.LastDropUsed;
    s->ErrorPackets = u->Pay.ErrorPackets; s->Overflows = u->Pay.Overflows;
}

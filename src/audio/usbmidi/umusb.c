/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original USB MIDI 1.0 transport from the USB-IF MIDI 1.0 specification,
 * the USB 2.0 specification and the Windows 2000 DDK USB/WDM headers and
 * documentation. No sample driver source used.
 *
 * One MIDIStreaming function: up to four OUT and four IN bulk/interrupt
 * endpoints, 16 cables each, at most 32 ports per direction.
 * OUT: one passive worker thread fills each OUT transfer round-robin across
 * the endpoint's ports from per-port 4 KiB byte rings (design 6.5) and
 * submits it synchronously; the device's NAK is the flow control (6.6).
 * IN: two requests per IN endpoint in caller-allocated IRPs, resubmitted
 * by a per-pipe DPC after their completion (no recursion, no wait behind
 * OUT); a STALL is cleared by the worker with
 * RESET_PIPE, other errors back off 100 ms (6.7). Only APIs present in the
 * Windows 98 DDK's import libraries are used, for one x86 binary.
 */
#include <wdm.h>
#pragma warning(push)
#pragma warning(disable:4200)
#include <usbdi.h>
#include <usbioctl.h>
#pragma warning(pop)
#include "umcore.h"
#include "umusb.h"

#ifndef USBD_STATUS_DEVICE_GONE                /* XP DDK usb.h; absent in 2000's */
#define USBD_STATUS_DEVICE_GONE ((USBD_STATUS)0xC0007000L)
#endif
#define UM_RING 4096
#define UM_SLOTS 2
#define UM_MAXCFG 16384
#define UM_OUTMAX 512
#define UM_BURST 8      /* synchronous refills in a row before a 1 ms pause */
#define UM_TAG 'uImU'

typedef struct UM_OPORT {
    UM_ENCODER Encoder;
    ULONG Head, Count, Gen;
    BOOLEAN Run, Reset;
    UM_NOTIFY Notify; PVOID Context; LONG Calls;
    UCHAR Ring[UM_RING];
} UM_OPORT;
typedef struct UM_IPORT {
    ULONG Head, Count;
    BOOLEAN Run;
    UM_NOTIFY Notify; PVOID Context; LONG Calls;
    UCHAR Ring[UM_RING];
} UM_IPORT;
struct UM_IPIPE;
typedef struct UM_SLOT {
    struct UM_USB *Owner; struct UM_IPIPE *Pipe;
    PIRP Irp; BOOLEAN Busy;
    struct _URB_BULK_OR_INTERRUPT_TRANSFER Urb;
    UCHAR Buffer[UM_OUTMAX];
} UM_SLOT;
typedef struct UM_IPIPE {
    USBD_PIPE_HANDLE Handle;
    ULONG Size, Cables, Errors;
    UCHAR Port[16];                /* cable -> input port + 1, 0 = none */
    BOOLEAN Stalled, Retry, Dead, LaterSet;
    ULONG Calls;                   /* StartPipe submissions in progress */
    ULONG Burst;                   /* refills after synchronous completions in a row */
    KDPC Refill;                   /* resubmits idle slots after a completion */
    KTIMER Later; KDPC LaterDpc;   /* the same, 1 ms later, after a burst */
    LARGE_INTEGER RetryAt;
    UM_SLOT Slot[UM_SLOTS];
} UM_IPIPE;
typedef struct UM_OPIPE {
    USBD_PIPE_HANDLE Handle;
    ULONG Size, First, Ports, Next;
    BOOLEAN Dead;
    UCHAR Packet[UM_OUTMAX];
    UCHAR Owner[UM_OUTMAX / 4];    /* output port per packet */
    ULONG Gen[UM_OUTMAX / 4];
} UM_OPIPE;
struct UM_USB {
    PDEVICE_OBJECT Lower;
    CCHAR StackSize;
    PVOID Thread;
    KSPIN_LOCK Lock;
    KEVENT Ready, Work, Idle, InIdle, Exited;
    BOOLEAN WaitThread;   /* thread object is waitable (WDM 1.10, 2000 and later) */
    KMUTEX StateMutex;
    ULONG Epoch, Done, InBusy;
    BOOLEAN Stop, Paused, Started;
    UM_USB_INFO Info;
    UM_PORTMAP Map;
    UM_USB_STATS Stats;
    UM_OPIPE Out[UM_MAX_PIPES];
    UM_IPIPE In[UM_MAX_PIPES];
    UM_OPORT *OutPort;             /* Map.Outputs entries */
    UM_IPORT *InPort;              /* Map.Inputs entries */
    WCHAR (*Names)[32];            /* Info.Filters entries */
};

static NTSTATUS Done(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    UNREFERENCED_PARAMETER(device); UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Synchronous URB in an IRP of our own; PASSIVE_LEVEL. A timeout cancels
 * and still waits for the lower driver to give the IRP back. */
static VOID RefillDpc(PKDPC dpc, PVOID context, PVOID a1, PVOID a2);
static VOID LaterDpc(PKDPC dpc, PVOID context, PVOID a1, PVOID a2);

static NTSTATUS Submit(PDEVICE_OBJECT lower, PURB urb, ULONG milliseconds)
{
    PIRP irp = IoAllocateIrp(lower->StackSize, FALSE);
    PIO_STACK_LOCATION stack;
    KEVENT event; LARGE_INTEGER timeout; NTSTATUS wait, status;
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    stack = IoGetNextIrpStackLocation(irp); RtlZeroMemory(stack, sizeof(*stack));
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = urb;
    IoSetCompletionRoutine(irp, Done, &event, TRUE, TRUE, TRUE);
    (void)IoCallDriver(lower, irp);
    timeout.QuadPart = -(LONGLONG)milliseconds * 10000;
    wait = KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, &timeout);
    if (wait == STATUS_TIMEOUT) {
        (void)IoCancelIrp(irp);
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = irp->IoStatus.Status == STATUS_CANCELLED ? STATUS_IO_TIMEOUT : irp->IoStatus.Status;
    } else status = irp->IoStatus.Status;
    if (NT_SUCCESS(status) && !USBD_SUCCESS(urb->UrbHeader.Status)) status = STATUS_DEVICE_DATA_ERROR;
    IoFreeIrp(irp);
    return status;
}

#if defined(UMTRACE) && !defined(_WIN64)
/* Create-path diagnostics on the QEMU debug port (traced test builds only):
 * "cf" lines carry the configuration descriptor the PDO returned, "cs" the
 * step that failed and its status. */
static VOID CfOut(UCHAR v) { __asm { mov al, v } __asm { mov dx, 0xe9 } __asm { out dx, al } }
static VOID CfLine(char a, char b, const UCHAR *p, ULONG n)
{
    static const char hex[] = "0123456789abcdef"; ULONG i;
    CfOut((UCHAR)a); CfOut((UCHAR)b);
    for (i = 0; i < n; i++) { CfOut(' '); CfOut((UCHAR)hex[p[i] >> 4]); CfOut((UCHAR)hex[p[i] & 15]); }
    CfOut(10);
}
static VOID CfDump(const UCHAR *p, ULONG n)
{
    ULONG i;
    for (i = 0; i < n; i += 16) CfLine('c', 'f', p + i, n - i < 16 ? n - i : 16);
}
static VOID CfStep(UCHAR step, NTSTATUS status)
{
    UCHAR b[5]; b[0] = step; b[1] = (UCHAR)(status >> 24); b[2] = (UCHAR)(status >> 16);
    b[3] = (UCHAR)(status >> 8); b[4] = (UCHAR)status; CfLine('c', 's', b, 5);
}
#else
#define CfDump(p, n) ((void)0)
#define CfStep(step, status) ((void)0)
#endif

static NTSTATUS Descriptor(PDEVICE_OBJECT lower, UCHAR kind, UCHAR index, USHORT language,
                           PVOID bytes, ULONG length, PULONG got)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST u; NTSTATUS status;
    RtlZeroMemory(&u, sizeof(u)); u.Hdr.Length = sizeof(u);
    u.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    u.DescriptorType = kind; u.Index = index; u.LanguageId = language;
    u.TransferBuffer = bytes; u.TransferBufferLength = length;
    status = Submit(lower, (PURB)&u, 5000);
    *got = NT_SUCCESS(status) ? u.TransferBufferLength : 0;
    if (*got > length) { *got = 0; status = STATUS_DEVICE_DATA_ERROR; }
    return status;
}

/* String descriptor into UTF-16 units; returns the unit count, 0 on any
 * failure or a malformed descriptor. */
static ULONG String(UM_USB *s, UCHAR index, USHORT language, WCHAR *out, ULONG units)
{
    UCHAR raw[256]; ULONG got, n, i;
    if (!index) return 0;
    if (!NT_SUCCESS(Descriptor(s->Lower, 3, index, language, raw, 255, &got))) return 0;
    if (got < 2 || raw[1] != 3 || raw[0] > got || raw[0] < 2) return 0;
    n = (raw[0] - 2) / 2;
    if (n > units) n = units;
    for (i = 0; i < n; i++) out[i] = (WCHAR)(raw[2 + 2 * i] | (raw[3 + 2 * i] << 8));
    return n;
}

/* Takes the pipe handles of the mapped endpoints from the MIDIStreaming
 * interface's returned pipe list; FALSE if any is missing. */
static BOOLEAN TakePipes(UM_USB *s, const USBD_INTERFACE_INFORMATION *ms, const UM_INTERFACE *iface)
{
    ULONG i, j;
    for (i = 0; i < s->Map.OutPipes + s->Map.InPipes; i++) {
        BOOLEAN in = i >= s->Map.OutPipes;
        ULONG k = in ? i - s->Map.OutPipes : i;
        const UM_ENDPOINT *e = &iface->Endpoint[in ? s->Map.InPipe[k] : s->Map.OutPipe[k]];
        USBD_PIPE_HANDLE handle = NULL;
        for (j = 0; j < ms->NumberOfPipes; j++) {
            const USBD_PIPE_INFORMATION *p = &ms->Pipes[j];
            if (p->EndpointAddress == e->Address && p->PipeHandle &&
                (p->PipeType == UsbdPipeTypeBulk || p->PipeType == UsbdPipeTypeInterrupt))
                handle = p->PipeHandle;
        }
        if (!handle) { CfStep((UCHAR)(0x20 + i), STATUS_DEVICE_CONFIGURATION_ERROR); return FALSE; }
        if (in) s->In[k].Handle = handle;
        else s->Out[k].Handle = handle;
    }
    return TRUE;
}

/* SELECT_INTERFACE for the MIDIStreaming interface at its alternate, on an
 * already selected configuration; takes its pipes. */
static NTSTATUS SelectInterface(UM_USB *s, USBD_CONFIGURATION_HANDLE config,
                                const USBD_INTERFACE_INFORMATION *ms, const UM_INTERFACE *iface)
{
    ULONG size, j; PURB urb; NTSTATUS status;
    size = FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface) + ms->Length;
    urb = ExAllocatePoolWithTag(NonPagedPool, size, UM_TAG);
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, size);
    urb->UrbHeader.Length = (USHORT)size;
    urb->UrbHeader.Function = URB_FUNCTION_SELECT_INTERFACE;
    urb->UrbSelectInterface.ConfigurationHandle = config;
    urb->UrbSelectInterface.Interface.Length = ms->Length;
    urb->UrbSelectInterface.Interface.InterfaceNumber = iface->Number;
    urb->UrbSelectInterface.Interface.AlternateSetting = iface->Alternate;
    urb->UrbSelectInterface.Interface.NumberOfPipes = ms->NumberOfPipes;
    for (j = 0; j < ms->NumberOfPipes; j++)
        urb->UrbSelectInterface.Interface.Pipes[j].MaximumTransferSize = 4096;
    status = Submit(s->Lower, urb, 5000);
    CfStep(0x30, status);
    if (NT_SUCCESS(status) && !TakePipes(s, &urb->UrbSelectInterface.Interface, iface))
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    ExFreePool(urb);
    return status;
}

static NTSTATUS SelectConfiguration(UM_USB *s, PUCHAR config, ULONG length,
                                    const UM_INTERFACE *iface)
{
    UCHAR alt[256], pipes[256], present[256];
    ULONG off, size, count = 0, i, j;
    PURB urb; PUSBD_INTERFACE_INFORMATION info, ms = NULL;
    NTSTATUS status;
    RtlZeroMemory(present, sizeof(present));
    /* Every interface in the descriptor the PDO returned, at alternate 0
     * except the MIDIStreaming one (design 6.2). */
    for (off = 9; off < length; off += config[off]) {
        PUCHAR p = config + off;
        if (p[1] != 4) continue;
        if (!present[p[2]]) {
            if (p[2] == iface->Number ? p[3] != iface->Alternate : p[3] != 0) continue;
            present[p[2]] = 1; alt[p[2]] = p[3]; pipes[p[2]] = p[4]; count++;
        }
    }
    if (!count || !present[iface->Number]) { CfStep(0x0f, (NTSTATUS)count); return STATUS_DEVICE_CONFIGURATION_ERROR; }
    size = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface);
    for (i = 0; i < 256; i++) if (present[i])
        size += FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) + pipes[i] * sizeof(USBD_PIPE_INFORMATION);
    urb = ExAllocatePoolWithTag(NonPagedPool, size, UM_TAG);
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, size);
    urb->UrbHeader.Length = (USHORT)size;
    urb->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb->UrbSelectConfiguration.ConfigurationDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)config;
    info = &urb->UrbSelectConfiguration.Interface;
    for (i = 0; i < 256; i++) if (present[i]) {
        info->Length = (USHORT)(FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) +
                                pipes[i] * sizeof(USBD_PIPE_INFORMATION));
        info->InterfaceNumber = (UCHAR)i; info->AlternateSetting = alt[i];
        info->NumberOfPipes = pipes[i];
        for (j = 0; j < pipes[i]; j++) info->Pipes[j].MaximumTransferSize = 4096;
        if (i == iface->Number) ms = info;
        info = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)info + info->Length);
    }
    status = Submit(s->Lower, urb, 5000);
    CfStep(0x10, status);
#if defined(UMTRACE) && !defined(_WIN64)
    {   /* "ci": per returned interface: number, alternate, pipe count, Length;
         * "cp": per pipe: address, type, handle present, max packet */
        PUSBD_INTERFACE_INFORMATION q = &urb->UrbSelectConfiguration.Interface;
        for (i = 0; i < count; i++) {
            UCHAR t[6];
            t[0] = q->InterfaceNumber; t[1] = q->AlternateSetting; t[2] = (UCHAR)q->NumberOfPipes;
            t[3] = (UCHAR)(q->Length >> 8); t[4] = (UCHAR)q->Length; t[5] = (UCHAR)(q == ms);
            CfLine('c', 'i', t, 6);
            for (j = 0; j < q->NumberOfPipes && j < 8; j++) {
                t[0] = q->Pipes[j].EndpointAddress; t[1] = (UCHAR)q->Pipes[j].PipeType;
                t[2] = (UCHAR)(q->Pipes[j].PipeHandle != NULL);
                t[3] = (UCHAR)(q->Pipes[j].MaximumPacketSize >> 8); t[4] = (UCHAR)q->Pipes[j].MaximumPacketSize;
                t[5] = (UCHAR)q->Pipes[j].Interval;
                CfLine('c', 'p', t, 6);
            }
            if (!q->Length) break;
            q = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)q + q->Length);
        }
    }
#endif
    if (NT_SUCCESS(status) && (!urb->UrbSelectConfiguration.ConfigurationHandle || !ms))
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (NT_SUCCESS(status) && !TakePipes(s, ms, iface)) {
        /* ME's composite parent completed the function's SELECT_CONFIGURATION
         * with a configuration handle but left every pipe entry empty (2026-10-22
         * test log, "Windows ME"); a SELECT_INTERFACE on that handle asks for
         * the MIDIStreaming interface's pipes again. */
        status = SelectInterface(s, urb->UrbSelectConfiguration.ConfigurationHandle, ms, iface);
    }
    ExFreePool(urb);
    return status;
}

static VOID Names(UM_USB *s, const USB_DEVICE_DESCRIPTOR *dev)
{
    WCHAR product[127], jack[127]; UCHAR langs[4]; ULONG got, pn, jn, f;
    USHORT language = 0x0409;
    if (NT_SUCCESS(Descriptor(s->Lower, 3, 0, 0, langs, 4, &got)) && got >= 4 && langs[1] == 3)
        language = (USHORT)(langs[2] | (langs[3] << 8));
    pn = String(s, dev->iProduct, language, product, 126);
    for (f = 0; f < s->Info.Filters; f++) {
        UCHAR index = 0;
        if (f < s->Map.Outputs) index = s->Map.Out[f].Name;
        if (!index && f < s->Map.Inputs) index = s->Map.In[f].Name;
        jn = String(s, index, language, jack, 126);
        UmPortName((const unsigned short *)product, pn, (const unsigned short *)jack, jn,
                   s->Info.Filters > 1, f, (unsigned short *)s->Names[f]);
    }
}

static VOID Free(UM_USB *s)
{
    ULONG i, k;
    for (i = 0; i < UM_MAX_PIPES; i++) for (k = 0; k < UM_SLOTS; k++)
        if (s->In[i].Slot[k].Irp) ExFreePool(s->In[i].Slot[k].Irp);
    if (s->OutPort) ExFreePool(s->OutPort);
    if (s->InPort) ExFreePool(s->InPort);
    if (s->Names) ExFreePool(s->Names);
    ExFreePool(s);
}

NTSTATUS UmUsbCreate(PDEVICE_OBJECT lower, UM_USB **out)
{
    UM_USB *s; NTSTATUS status; USB_DEVICE_DESCRIPTOR dev; UCHAR head[9];
    PUCHAR config = NULL; UM_INTERFACE *iface = NULL; ULONG got, total, i, k;
    PAGED_CODE();
    *out = NULL;
    s = ExAllocatePoolWithTag(NonPagedPool, sizeof(*s), UM_TAG);
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(s, sizeof(*s));
    s->Lower = lower; s->StackSize = lower->StackSize;
    KeInitializeSpinLock(&s->Lock); KeInitializeMutex(&s->StateMutex, 0);
    KeInitializeEvent(&s->Ready, NotificationEvent, FALSE);
    KeInitializeEvent(&s->Exited, NotificationEvent, FALSE);
    s->WaitThread = IoIsWdmVersionAvailable(1, 0x10);
    KeInitializeEvent(&s->Work, SynchronizationEvent, FALSE);
    KeInitializeEvent(&s->Idle, NotificationEvent, TRUE);
    KeInitializeEvent(&s->InIdle, NotificationEvent, TRUE);
    status = Descriptor(lower, 1, 0, 0, &dev, sizeof(dev), &got);
    if (NT_SUCCESS(status) && (got != sizeof(dev) || dev.bDescriptorType != 1)) status = STATUS_DEVICE_DATA_ERROR;
    if (NT_SUCCESS(status)) status = Descriptor(lower, 2, 0, 0, head, 9, &got);
    if (NT_SUCCESS(status) && (got != 9 || head[1] != 2)) status = STATUS_DEVICE_DATA_ERROR;
    total = head[2] | ((ULONG)head[3] << 8);
    CfStep(0, status);
    if (NT_SUCCESS(status)) CfDump(head, 9);
    if (NT_SUCCESS(status) && (total < 9 || total > UM_MAXCFG)) status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (NT_SUCCESS(status)) {
        config = ExAllocatePoolWithTag(NonPagedPool, total, UM_TAG);
        iface = ExAllocatePoolWithTag(NonPagedPool, sizeof(*iface), UM_TAG);
        if (!config || !iface) status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(status)) status = Descriptor(lower, 2, 0, 0, config, total, &got);
    CfStep(1, status);
    if (NT_SUCCESS(status) && got != total) status = STATUS_DEVICE_DATA_ERROR;
    if (NT_SUCCESS(status)) CfDump(config, got);
    if (NT_SUCCESS(status) && !UmFindInterface(config, total, iface)) { status = STATUS_DEVICE_CONFIGURATION_ERROR; CfStep(2, status); }
    if (NT_SUCCESS(status)) {
        s->Info.Filters = UmMapPorts(iface, &s->Map);
        if (!s->Info.Filters) { status = STATUS_DEVICE_CONFIGURATION_ERROR; CfStep(3, status); }
    }
    if (NT_SUCCESS(status)) {
        s->Info.Vendor = dev.idVendor; s->Info.Product = dev.idProduct;
        s->Info.Revision = dev.bcdDevice;
        s->Info.Interface = iface->Number; s->Info.Alternate = iface->Alternate;
        s->Info.Outputs = s->Map.Outputs; s->Info.Inputs = s->Map.Inputs;
        s->Info.OutPipes = (UCHAR)s->Map.OutPipes; s->Info.InPipes = (UCHAR)s->Map.InPipes;
        if (s->Map.Outputs) s->OutPort = ExAllocatePoolWithTag(NonPagedPool, s->Map.Outputs * sizeof(UM_OPORT), UM_TAG);
        if (s->Map.Inputs) s->InPort = ExAllocatePoolWithTag(NonPagedPool, s->Map.Inputs * sizeof(UM_IPORT), UM_TAG);
        s->Names = ExAllocatePoolWithTag(NonPagedPool, s->Info.Filters * sizeof(*s->Names), UM_TAG);
        if ((s->Map.Outputs && !s->OutPort) || (s->Map.Inputs && !s->InPort) || !s->Names)
            status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(status)) {
        if (s->OutPort) RtlZeroMemory(s->OutPort, s->Map.Outputs * sizeof(UM_OPORT));
        if (s->InPort) RtlZeroMemory(s->InPort, s->Map.Inputs * sizeof(UM_IPORT));
        RtlZeroMemory(s->Names, s->Info.Filters * sizeof(*s->Names));
        for (i = 0; i < s->Map.Outputs; i++) UmEncoderInit(&s->OutPort[i].Encoder, s->Map.Out[i].Cable);
        for (i = 0; i < s->Map.OutPipes; i++) {
            const UM_ENDPOINT *e = &iface->Endpoint[s->Map.OutPipe[i]];
            UM_OPIPE *p = &s->Out[i];
            p->Size = e->MaxPacket & ~3u;
            if (p->Size > UM_OUTMAX) p->Size = UM_OUTMAX;
            p->First = s->Map.Outputs;
            for (k = 0; k < s->Map.Outputs; k++) if (s->Map.Out[k].Pipe == i) {
                if (k < p->First) p->First = k;
                p->Ports++;
            }
            if (!i) s->Info.Speed = e->MaxPacket;
        }
        for (i = 0; i < s->Map.InPipes && NT_SUCCESS(status); i++) {
            const UM_ENDPOINT *e = &iface->Endpoint[s->Map.InPipe[i]];
            UM_IPIPE *p = &s->In[i];
            /* A read takes the whole maximum packet (a shorter buffer can
             * fail a full packet); events are decoded four bytes at a time
             * and a tail is ignored. Larger endpoints are refused (Codex
             * round 8). */
            p->Size = e->MaxPacket;
            if (!p->Size || p->Size > UM_OUTMAX) { status = STATUS_NOT_SUPPORTED; break; }
            p->Cables = e->Cables;
            for (k = 0; k < s->Map.Inputs; k++)
                if (s->Map.In[k].Pipe == i) p->Port[s->Map.In[k].Cable] = (UCHAR)(k + 1);
            KeInitializeDpc(&p->Refill, RefillDpc, NULL);
            KeInitializeDpc(&p->LaterDpc, LaterDpc, s);
            KeInitializeTimer(&p->Later);
            for (k = 0; k < UM_SLOTS; k++) {
                p->Slot[k].Owner = s; p->Slot[k].Pipe = p;
                p->Slot[k].Irp = ExAllocatePoolWithTag(NonPagedPool, IoSizeOfIrp(s->StackSize), UM_TAG);
                if (!p->Slot[k].Irp) status = STATUS_INSUFFICIENT_RESOURCES;
            }
            if (!s->Map.OutPipes && !i) s->Info.Speed = e->MaxPacket;
        }
    }
    if (NT_SUCCESS(status)) status = SelectConfiguration(s, config, total, iface);
    if (NT_SUCCESS(status)) Names(s, &dev);
    if (config) ExFreePool(config);
    if (iface) ExFreePool(iface);
    if (!NT_SUCCESS(status)) { Free(s); return status; }
    *out = s;
    return STATUS_SUCCESS;
}

VOID UmUsbGetInfo(UM_USB *s, UM_USB_INFO *info) { *info = s->Info; }

VOID UmUsbFilterName(UM_USB *s, ULONG filter, WCHAR name[32])
{
    if (filter < s->Info.Filters) RtlCopyMemory(name, s->Names[filter], 32 * sizeof(WCHAR));
    else name[0] = 0;
}

BOOLEAN UmUsbHasPort(UM_USB *s, BOOLEAN capture, ULONG port)
{
    return (BOOLEAN)(port < (capture ? s->Map.Inputs : s->Map.Outputs));
}

/* Notify the ports in mask outside the lock; Calls lets Bind(NULL) wait. */
static VOID Notify(UM_USB *s, BOOLEAN capture, ULONG mask)
{
    ULONG i; KIRQL old;
    for (i = 0; mask; i++, mask >>= 1) {
        UM_NOTIFY fn; PVOID context; LONG *calls;
        if (!(mask & 1)) continue;
        KeAcquireSpinLock(&s->Lock, &old);
        if (capture) { fn = s->InPort[i].Notify; context = s->InPort[i].Context; calls = &s->InPort[i].Calls; }
        else { fn = s->OutPort[i].Notify; context = s->OutPort[i].Context; calls = &s->OutPort[i].Calls; }
        if (fn) (*calls)++;
        KeReleaseSpinLock(&s->Lock, old);
        if (fn) {
            fn(context);
            KeAcquireSpinLock(&s->Lock, &old); (*calls)--; KeReleaseSpinLock(&s->Lock, old);
        }
    }
}

static NTSTATUS InDone(PDEVICE_OBJECT device, PIRP irp, PVOID context);

/* Caller holds the lock and has set slot->Busy and counted it in InBusy. */
static VOID PrepareSlot(UM_SLOT *slot)
{
    UM_USB *s = slot->Owner; PIO_STACK_LOCATION stack; PIRP irp = slot->Irp;
    IoInitializeIrp(irp, IoSizeOfIrp(s->StackSize), s->StackSize);
    RtlZeroMemory(&slot->Urb, sizeof(slot->Urb));
    slot->Urb.Hdr.Length = sizeof(slot->Urb);
    slot->Urb.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    slot->Urb.PipeHandle = slot->Pipe->Handle;
    slot->Urb.TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    slot->Urb.TransferBuffer = slot->Buffer;
    slot->Urb.TransferBufferLength = slot->Pipe->Size;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = &slot->Urb;
    IoSetCompletionRoutine(irp, InDone, slot, TRUE, TRUE, TRUE);
}

static NTSTATUS InDone(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    UM_SLOT *slot = context; UM_USB *s = slot->Owner; UM_IPIPE *pipe = slot->Pipe;
    NTSTATUS status = irp->IoStatus.Status; USBD_STATUS usbd = slot->Urb.Hdr.Status;
    ULONG size = slot->Urb.TransferBufferLength, i, mask = 0;
    BOOLEAN resubmit = FALSE, wake = FALSE, stalled = FALSE, dead = FALSE, retry = FALSE; KIRQL old;
    UNREFERENCED_PARAMETER(device);
    if (NT_SUCCESS(status) && !USBD_SUCCESS(usbd)) status = STATUS_DEVICE_DATA_ERROR;
    KeAcquireSpinLock(&s->Lock, &old);
    if (NT_SUCCESS(status)) {
        if (size > pipe->Size) size = 0;
        s->Stats.RxUrbs++; s->Stats.RxBytes += size;
        for (i = 0; i + 4 <= size; i += 4) {
            PUCHAR p = slot->Buffer + i; UCHAR data[3]; ULONG n, j, port;
            if (!(p[0] | p[1] | p[2] | p[3])) continue;       /* padding */
            n = UmDecodePacket(p, 4, (1u << pipe->Cables) - 1, data);
            port = n ? pipe->Port[p[0] >> 4] : 0;
            if (!n) { s->Stats.BadEvents++; continue; }
            if (!port--) continue;                            /* no such port */
            if (!s->InPort[port].Run) continue;
            if (n > UM_RING - s->InPort[port].Count) { s->Stats.Overruns++; continue; }
            for (j = 0; j < n; j++) {
                UM_IPORT *q = &s->InPort[port];
                q->Ring[(q->Head + q->Count) % UM_RING] = data[j]; q->Count++;
            }
            mask |= 1u << port;
        }
        pipe->Errors = 0;
    }
    if (s->Stop || s->Paused || !NT_SUCCESS(status)) {
        if (!s->Stop && !s->Paused) {
            s->Stats.LastStatus = status;
            if (usbd == USBD_STATUS_STALL_PID) { stalled = TRUE; s->Stats.Stalls++; wake = TRUE; }
            else if (status == STATUS_DEVICE_NOT_CONNECTED || usbd == USBD_STATUS_DEVICE_GONE ||
                     status == STATUS_NO_SUCH_DEVICE) dead = TRUE;
            else if (++pipe->Errors > 10) { dead = TRUE; s->Stats.Errors++; }
            else { s->Stats.Errors++; retry = TRUE; wake = TRUE; }
        }
    } else resubmit = TRUE;
    KeReleaseSpinLock(&s->Lock, old);
    if (mask) Notify(s, TRUE, mask);
    /* Never resubmit here: a lower driver completing synchronously would
     * recurse on the stack, and the worker may be blocked in an OUT
     * transfer. The pipe's DPC resubmits idle slots after this routine has
     * returned, holding an InBusy token so a stop cannot free the transport
     * under it (on SMP the DPC can run on two processors at once; the slot
     * lock and the per-insertion tokens cover that). After UM_BURST
     * completions in a row inside our own submission (a lower driver that
     * completes synchronously) the refill waits 1 ms on a timer, so the
     * chain cannot hold the processor at DISPATCH (Codex round m2).
     * The slot goes idle, its recovery state is published and the wakeups
     * are set together under the lock, after the last use of the
     * notifications, so a stop waiting for InIdle cannot free the transport
     * while this routine still uses it (the waiter takes the lock again
     * before it frees anything), and the worker's recovery never runs
     * before the slot is idle (Codex rounds 5-7). */
    KeAcquireSpinLock(&s->Lock, &old);
    slot->Busy = FALSE;
    if (stalled) pipe->Stalled = TRUE;
    if (dead) pipe->Dead = TRUE;
    if (retry) {
        pipe->RetryAt.QuadPart = (LONGLONG)KeQueryInterruptTime() + 1000000;   /* not wall time (Codex round 9) */
        pipe->Retry = TRUE;
    }
    if (resubmit) {
        if (pipe->Calls) pipe->Burst++; else pipe->Burst = 0;
        if (pipe->Burst < UM_BURST) {
            if (KeInsertQueueDpc(&pipe->Refill, s, pipe)) s->InBusy++;   /* the DPC's token */
        } else if (!pipe->LaterSet) {
            LARGE_INTEGER due; due.QuadPart = -10000;                    /* 1 ms */
            pipe->LaterSet = TRUE; s->InBusy++;                          /* the timer DPC's token */
            KeSetTimer(&pipe->Later, due, &pipe->LaterDpc);
        }
    }
    if (wake) KeSetEvent(&s->Work, IO_NO_INCREMENT, FALSE);
    if (!--s->InBusy) KeSetEvent(&s->InIdle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&s->Lock, old);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Start every idle slot of one IN pipe; PASSIVE or DISPATCH. */
static VOID StartPipe(UM_USB *s, UM_IPIPE *pipe)
{
    ULONG k; KIRQL old; PIRP go[UM_SLOTS]; ULONG n = 0;
    KeAcquireSpinLock(&s->Lock, &old);
    /* A pipe waiting out its error delay is restarted by the worker when
     * the delay has passed, not by a sibling's refill (Codex round 8). */
    if (!s->Stop && !s->Paused && !pipe->Dead && !pipe->Stalled && !pipe->Retry)
        for (k = 0; k < UM_SLOTS; k++) if (!pipe->Slot[k].Busy) {
            pipe->Slot[k].Busy = TRUE;
            if (!s->InBusy++) KeClearEvent(&s->InIdle);
            PrepareSlot(&pipe->Slot[k]);
            go[n++] = pipe->Slot[k].Irp;
        }
    pipe->Calls += n;
    KeReleaseSpinLock(&s->Lock, old);
    for (k = 0; k < n; k++) (void)IoCallDriver(s->Lower, go[k]);
    if (n) { KeAcquireSpinLock(&s->Lock, &old); pipe->Calls -= n; KeReleaseSpinLock(&s->Lock, old); }
}

/* The pipe's refill DPC (InDone): start its idle slots, then return the
 * token InDone took when it queued this DPC. DISPATCH_LEVEL. */
static VOID RefillDpc(PKDPC dpc, PVOID context, PVOID a1, PVOID a2)
{
    UM_USB *s = a1; UM_IPIPE *pipe = a2; KIRQL old;
    UNREFERENCED_PARAMETER(dpc); UNREFERENCED_PARAMETER(context);
    StartPipe(s, pipe);
    KeAcquireSpinLock(&s->Lock, &old);
    if (!--s->InBusy) KeSetEvent(&s->InIdle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&s->Lock, old);   /* last access to s */
}

/* The pipe's timer DPC (InDone, after a burst): as RefillDpc. The pipe is
 * found from its DPC; a timer passes no system arguments of ours. */
static VOID LaterDpc(PKDPC dpc, PVOID context, PVOID a1, PVOID a2)
{
    UM_USB *s = context; UM_IPIPE *pipe = CONTAINING_RECORD(dpc, UM_IPIPE, LaterDpc); KIRQL old;
    UNREFERENCED_PARAMETER(a1); UNREFERENCED_PARAMETER(a2);
    KeAcquireSpinLock(&s->Lock, &old);
    pipe->LaterSet = FALSE; pipe->Burst = 0;
    KeReleaseSpinLock(&s->Lock, old);
    StartPipe(s, pipe);
    KeAcquireSpinLock(&s->Lock, &old);
    if (!--s->InBusy) KeSetEvent(&s->InIdle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&s->Lock, old);   /* last access to s */
}

/* Cancel until no IN request is outstanding. A request between its
 * completion and resubmission is caught by the next pass. PASSIVE. */
static VOID CancelIn(UM_USB *s)
{
    for (;;) {
        PIRP irps[UM_MAX_PIPES * UM_SLOTS]; ULONG n = 0, i, k, busy; KIRQL old;
        LARGE_INTEGER wait;
        KeAcquireSpinLock(&s->Lock, &old);
        for (i = 0; i < UM_MAX_PIPES; i++) for (k = 0; k < UM_SLOTS; k++)
            if (s->In[i].Slot[k].Busy) irps[n++] = s->In[i].Slot[k].Irp;
        busy = s->InBusy;
        KeReleaseSpinLock(&s->Lock, old);
        if (!n && !busy) return;   /* no request, no completion holding a token */
        for (i = 0; i < n; i++) (void)IoCancelIrp(irps[i]);
        wait.QuadPart = -500000;   /* 50 ms */
        KeWaitForSingleObject(&s->InIdle, Executive, KernelMode, FALSE, &wait);
    }
}

static NTSTATUS ResetPipe(UM_USB *s, USBD_PIPE_HANDLE handle)
{
    struct _URB_PIPE_REQUEST u;
    RtlZeroMemory(&u, sizeof(u)); u.Hdr.Length = sizeof(u);
    u.Hdr.Function = URB_FUNCTION_RESET_PIPE; u.PipeHandle = handle;
    return Submit(s->Lower, (PURB)&u, 5000);
}

/* Fill one OUT transfer round-robin, a packet per port per round. Lock held. */
static ULONG Fill(UM_USB *s, UM_OPIPE *pipe, ULONG *consumed)
{
    ULONG packets = 0, max = pipe->Size / 4, k;
    BOOLEAN progress = TRUE;
    while (packets < max && progress && pipe->Ports) {
        progress = FALSE;
        for (k = 0; k < pipe->Ports && packets < max; k++) {
            ULONG index = pipe->First + (pipe->Next + k) % pipe->Ports;
            UM_OPORT *q = &s->OutPort[index];
            if (q->Reset) { UmEncoderInit(&q->Encoder, s->Map.Out[index].Cable); q->Reset = FALSE; }
            while (q->Run && q->Count) {
                int r = UmEncodeByte(&q->Encoder, q->Ring[q->Head], pipe->Packet + 4 * packets);
                q->Head = (q->Head + 1) % UM_RING; q->Count--;
                *consumed |= 1u << index;
                if (r > 0) {
                    pipe->Owner[packets] = (UCHAR)index; pipe->Gen[packets] = q->Gen;
                    packets++; progress = TRUE;
                    break;
                }
                if (r < 0) s->Stats.BadEvents++;
            }
        }
        pipe->Next = (pipe->Next + 1) % pipe->Ports;
    }
    return packets;
}

static NTSTATUS Bulk(UM_USB *s, USBD_PIPE_HANDLE handle, PUCHAR bytes, PULONG size)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER u; NTSTATUS status;
    RtlZeroMemory(&u, sizeof(u)); u.Hdr.Length = sizeof(u);
    u.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    u.PipeHandle = handle; u.TransferFlags = 0;
    u.TransferBuffer = bytes; u.TransferBufferLength = *size;
    status = Submit(s->Lower, (PURB)&u, 2000);
    if (status == STATUS_DEVICE_DATA_ERROR && u.Hdr.Status == USBD_STATUS_STALL_PID)
        status = STATUS_DEVICE_PROTOCOL_ERROR;    /* marks a STALL for the caller */
    *size = u.TransferBufferLength;
    return status;
}

static VOID Worker(PVOID context)
{
    UM_USB *s = context; KIRQL old; ULONG i, k, seen;
    /* Windows 98 SE: waiting on a thread object faulted in the stop path
     * (fatal exception 0E on REMOVE, 2026-10-22 test log), so there the
     * stop waits for Exited only. 2000 and later also wait for the thread
     * object, so the image cannot unload under a still-running thread. */
    s->Thread = PsGetCurrentThread();
    if (s->WaitThread) ObReferenceObject(s->Thread);
    KeSetEvent(&s->Ready, IO_NO_INCREMENT, FALSE);
    for (;;) {
        BOOLEAN stop, paused, more = FALSE, retry = FALSE;
        ULONG consumed = 0;
        LARGE_INTEGER now, timeout;
        KeAcquireSpinLock(&s->Lock, &old);
        stop = s->Stop; paused = s->Paused; seen = s->Epoch;
        KeReleaseSpinLock(&s->Lock, old);
        if (stop) break;
        /* IN maintenance: clear stalls, retry after errors. */
        now.QuadPart = (LONGLONG)KeQueryInterruptTime();
        for (i = 0; i < s->Map.InPipes && !paused; i++) {
            UM_IPIPE *p = &s->In[i]; BOOLEAN stalled, due;
            KeAcquireSpinLock(&s->Lock, &old);
            stalled = p->Stalled; due = p->Retry && now.QuadPart >= p->RetryAt.QuadPart;
            if (p->Retry && !due) retry = TRUE;
            KeReleaseSpinLock(&s->Lock, old);
            if (stalled) {
                NTSTATUS r = ResetPipe(s, p->Handle);
                KeAcquireSpinLock(&s->Lock, &old);
                p->Stalled = FALSE;
                if (!NT_SUCCESS(r)) p->Dead = TRUE;
                KeReleaseSpinLock(&s->Lock, old);
            }
            if (stalled || due) {
                /* A failure after the snapshot set a later deadline, or one is
                 * pending beside a stall: keep it (Codex round m3). */
                KeAcquireSpinLock(&s->Lock, &old);
                if (now.QuadPart >= p->RetryAt.QuadPart) p->Retry = FALSE;
                KeReleaseSpinLock(&s->Lock, old);
                StartPipe(s, p);
            }
        }
        /* OUT: one transfer per endpoint per pass. */
        for (i = 0; i < s->Map.OutPipes && !paused; i++) {
            UM_OPIPE *p = &s->Out[i]; ULONG packets, size, kept = 0; NTSTATUS status;
            KeAcquireSpinLock(&s->Lock, &old);
            packets = p->Dead ? 0 : Fill(s, p, &consumed);
            KeReleaseSpinLock(&s->Lock, old);
            if (!packets) continue;
            /* Drop packets of ports stopped since they were taken. */
            KeAcquireSpinLock(&s->Lock, &old);
            for (k = 0; k < packets; k++) {
                UM_OPORT *q = &s->OutPort[p->Owner[k]];
                if (q->Run && q->Gen == p->Gen[k]) {
                    if (kept != k) RtlCopyMemory(p->Packet + 4 * kept, p->Packet + 4 * k, 4);
                    kept++;
                }
            }
            KeReleaseSpinLock(&s->Lock, old);
            if (!kept) continue;
            size = kept * 4;
            status = Bulk(s, p->Handle, p->Packet, &size);
            KeAcquireSpinLock(&s->Lock, &old);
            if (NT_SUCCESS(status) && size == kept * 4) { s->Stats.TxBytes += size; s->Stats.TxUrbs++; }
            else {
                s->Stats.LastStatus = status;
                if (status == STATUS_IO_TIMEOUT) s->Stats.Timeouts++;
                else s->Stats.Errors++;
                if (status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_NO_SUCH_DEVICE) p->Dead = TRUE;
            }
            KeReleaseSpinLock(&s->Lock, old);
            if (status == STATUS_DEVICE_PROTOCOL_ERROR) {
                s->Stats.Stalls++;
                if (!NT_SUCCESS(ResetPipe(s, p->Handle))) p->Dead = TRUE;
            }
        }
        if (consumed) Notify(s, FALSE, consumed);   /* output space */
        KeAcquireSpinLock(&s->Lock, &old);
        for (k = 0; k < s->Map.Outputs && !s->Paused; k++)
            if (s->OutPort[k].Run && s->OutPort[k].Count && !s->Out[s->Map.Out[k].Pipe].Dead) more = TRUE;
        s->Done = seen;
        if (s->Done == s->Epoch) KeSetEvent(&s->Idle, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&s->Lock, old);
        if (more) continue;
        timeout.QuadPart = -1000000;   /* 100 ms while a retry is pending */
        KeWaitForSingleObject(&s->Work, Executive, KernelMode, FALSE, retry ? &timeout : NULL);
    }
    KeAcquireSpinLock(&s->Lock, &old);
    s->Done = s->Epoch; KeSetEvent(&s->Idle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&s->Lock, old);
    KeSetEvent(&s->Exited, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS UmUsbStart(UM_USB *s)
{
    HANDLE thread; NTSTATUS status; ULONG i;
    PAGED_CODE();
    if (s->Started || s->Stop) return STATUS_INVALID_DEVICE_STATE;
    status = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, Worker, s);
    if (!NT_SUCCESS(status)) return status;
    KeWaitForSingleObject(&s->Ready, Executive, KernelMode, FALSE, NULL);
    ZwClose(thread);
    s->Started = TRUE;
    for (i = 0; i < s->Map.InPipes; i++) StartPipe(s, &s->In[i]);
    return STATUS_SUCCESS;
}

#if defined(UMTRACE) && !defined(_WIN64)
static VOID StopMark(UCHAR c)
{   /* one byte per UmUsbStop phase on the QEMU debug port (tests only) */
    UCHAR t[4]; ULONG i; t[0] = 's'; t[1] = c; t[2] = 10; t[3] = 0;
    for (i = 0; t[i]; i++) { UCHAR v = t[i]; __asm { mov al, v } __asm { mov dx, 0xe9 } __asm { out dx, al } }
}
#else
#define StopMark(c) ((void)0)
#endif
VOID UmUsbStop(UM_USB *s)
{
    KIRQL old; ULONG i;
    PAGED_CODE();
    if (!s) return;
    StopMark('1');
    KeWaitForSingleObject(&s->StateMutex, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&s->Lock, &old);
    s->Stop = TRUE;
    for (i = 0; i < s->Map.Outputs; i++) { s->OutPort[i].Run = FALSE; s->OutPort[i].Count = 0; }
    for (i = 0; i < s->Map.Inputs; i++) { s->InPort[i].Run = FALSE; s->InPort[i].Count = 0; }
    KeReleaseSpinLock(&s->Lock, old);
    StopMark('2');
    CancelIn(s);
    StopMark('3');
    if (s->Thread) {
        KeSetEvent(&s->Work, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(&s->Exited, Executive, KernelMode, FALSE, NULL);
        if (s->WaitThread) {
            KeWaitForSingleObject(s->Thread, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(s->Thread);
        } else {
            LARGE_INTEGER t; t.QuadPart = -100000; /* 10 ms: let it leave our code */
            KeDelayExecutionThread(KernelMode, FALSE, &t);
        }
        StopMark('4');
        s->Thread = NULL;
    }
    KeReleaseMutex(&s->StateMutex, FALSE);
    StopMark('5');
}

VOID UmUsbPower(UM_USB *s, BOOLEAN on)
{
    KIRQL old; ULONG i;
    PAGED_CODE();
    KeWaitForSingleObject(&s->StateMutex, Executive, KernelMode, FALSE, NULL);
    if (!s->Stop && s->Started) {
        KeAcquireSpinLock(&s->Lock, &old);
        s->Paused = (BOOLEAN)!on;
        KeReleaseSpinLock(&s->Lock, old);
        if (!on) CancelIn(s);
        else {
            for (i = 0; i < s->Map.InPipes; i++) StartPipe(s, &s->In[i]);
            KeSetEvent(&s->Work, IO_NO_INCREMENT, FALSE);
        }
    }
    KeReleaseMutex(&s->StateMutex, FALSE);
}

VOID UmUsbDestroy(UM_USB *s)
{
    if (s) { UmUsbStop(s); Free(s); }
}

VOID UmUsbGetStats(UM_USB *s, UM_USB_STATS *out)
{
    KIRQL old; KeAcquireSpinLock(&s->Lock, &old); *out = s->Stats; KeReleaseSpinLock(&s->Lock, old);
}

VOID UmUsbBind(UM_USB *s, BOOLEAN capture, ULONG port, UM_NOTIFY notify, PVOID context)
{
    KIRQL old; LONG calls;
    if (!UmUsbHasPort(s, capture, port)) return;
    KeAcquireSpinLock(&s->Lock, &old);
    if (capture) { s->InPort[port].Notify = notify; s->InPort[port].Context = context; }
    else { s->OutPort[port].Notify = notify; s->OutPort[port].Context = context; }
    KeReleaseSpinLock(&s->Lock, old);
    if (notify) return;
    for (;;) {
        LARGE_INTEGER wait;
        KeAcquireSpinLock(&s->Lock, &old);
        calls = capture ? s->InPort[port].Calls : s->OutPort[port].Calls;
        KeReleaseSpinLock(&s->Lock, old);
        if (!calls) break;
        wait.QuadPart = -10000;
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
    }
}

NTSTATUS UmUsbWrite(UM_USB *s, ULONG port, PVOID bytes, ULONG size, PULONG used)
{
    ULONG i, n; KIRQL old; UM_OPORT *q;
    *used = 0;
    if (port >= s->Map.Outputs) return STATUS_INVALID_PARAMETER;
    q = &s->OutPort[port];
    KeAcquireSpinLock(&s->Lock, &old);
    if (s->Stop || s->Out[s->Map.Out[port].Pipe].Dead) {
        /* Gone: bytes are discarded so a client can drain and close. */
        KeReleaseSpinLock(&s->Lock, old); *used = size; return STATUS_SUCCESS;
    }
    n = q->Run ? UM_RING - q->Count : 0;
    if (n >= size) n = size;
    else n &= ~3u;   /* a partial Write accepts a multiple of four bytes */
    for (i = 0; i < n; i++) q->Ring[(q->Head + q->Count + i) % UM_RING] = ((PUCHAR)bytes)[i];
    q->Count += n; *used = n;
    KeReleaseSpinLock(&s->Lock, old);
    if (n) KeSetEvent(&s->Work, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

NTSTATUS UmUsbRead(UM_USB *s, ULONG port, PVOID bytes, ULONG size, PULONG used)
{
    ULONG i, n; KIRQL old; UM_IPORT *q;
    *used = 0;
    if (port >= s->Map.Inputs) return STATUS_INVALID_PARAMETER;
    q = &s->InPort[port];
    KeAcquireSpinLock(&s->Lock, &old);
    n = q->Run && !s->Stop ? q->Count : 0;
    if (n > size) n = size;
    for (i = 0; i < n; i++) ((PUCHAR)bytes)[i] = q->Ring[(q->Head + i) % UM_RING];
    q->Head = (q->Head + n) % UM_RING; q->Count -= n; *used = n;
    KeReleaseSpinLock(&s->Lock, old);
    return STATUS_SUCCESS;
}

NTSTATUS UmUsbState(UM_USB *s, BOOLEAN capture, ULONG port, ULONG state)
{
    KIRQL old; NTSTATUS status = STATUS_SUCCESS; ULONG epoch = 0; BOOLEAN wait = FALSE;
    PAGED_CODE();
    if (!UmUsbHasPort(s, capture, port)) return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&s->StateMutex, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&s->Lock, &old);
    if (s->Stop) {
        /* Removed: only leaving RUN is accepted, so streams can close. */
        if (state == 3) status = STATUS_DEVICE_NOT_READY;
    } else if (capture) {
        UM_IPORT *q = &s->InPort[port];
        q->Run = (BOOLEAN)(state == 3);
        if (state == 0) q->Head = q->Count = 0;
    } else {
        UM_OPORT *q = &s->OutPort[port];
        BOOLEAN was = q->Run;
        q->Run = (BOOLEAN)(state == 3);
        if (state != 3) {
            if (was) q->Gen++;
            if (state == 0) { q->Head = q->Count = 0; q->Reset = TRUE; }
            if (s->Thread) {
                epoch = ++s->Epoch; KeClearEvent(&s->Idle); wait = TRUE;
            }
        }
    }
    KeReleaseSpinLock(&s->Lock, old);
    if (wait) {
        /* Barrier: the worker finishes the pass that may hold this port's
         * bytes; its packets were dropped by the Gen check or already sent. */
        KeSetEvent(&s->Work, IO_NO_INCREMENT, FALSE);
        for (;;) {
            BOOLEAN done;
            KeWaitForSingleObject(&s->Idle, Executive, KernelMode, FALSE, NULL);
            KeAcquireSpinLock(&s->Lock, &old);
            done = (BOOLEAN)((LONG)(s->Done - epoch) >= 0 || s->Stop);
            if (!done) KeClearEvent(&s->Idle);
            KeReleaseSpinLock(&s->Lock, old);
            if (done) break;
            KeSetEvent(&s->Work, IO_NO_INCREMENT, FALSE);
        }
    }
    KeReleaseMutex(&s->StateMutex, FALSE);
    return status;
}

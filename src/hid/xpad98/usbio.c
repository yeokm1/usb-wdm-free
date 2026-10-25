/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * xpad98: the USB side (docs/design/gamepad.md sections 5.1 and 5.5).
 * Descriptors, SELECT_CONFIGURATION of every interface at alternate
 * setting 0, interrupt IN kept armed twice per channel, an OUT queue with
 * one packet in flight per channel, and the XUSB start request. Plain
 * USBD URBs from the DDK's usbdi.h; no import from usbd.sys.
 *
 * Transfers use driver-owned IRPs (pool memory set up with
 * IoInitializeIrp) so they can be resubmitted from their completion
 * routines on every target system. Outstanding counts every transfer the
 * USB stack holds, plus one bias while the device is started; stop drops
 * the bias and waits for zero.
 */
#include "xpad98.h"

static NTSTATUS InComplete(PDEVICE_OBJECT unused, PIRP irp, PVOID context);
static NTSTATUS OutComplete(PDEVICE_OBJECT unused, PIRP irp, PVOID context);
static NTSTATUS CtlComplete(PDEVICE_OBJECT unused, PIRP irp, PVOID context);

/* Implemented in xpad98.c: a queued read the new state can complete. */
PIRP XpTakeReadyReadLocked(XP_DEV *dev);
VOID XpFailReads(XP_DEV *dev, NTSTATUS status);
VOID XpReadOptions(XP_DEV *dev);

static VOID Drop(XP_DEV *dev)
{
    if (InterlockedDecrement(&dev->Outstanding) == 0)
        KeSetEvent(&dev->IoIdle, IO_NO_INCREMENT, FALSE);
}

/* ---- synchronous URBs, PASSIVE_LEVEL only ---- */

static NTSTATUS SyncDone(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    UNREFERENCED_PARAMETER(unused);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS SyncUrb(XP_DEV *dev, PURB urb, ULONG ms)
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
    XPT("urb", urb->UrbHeader.Function, status, urb->UrbHeader.Status);
    if (NT_SUCCESS(status) && !USBD_SUCCESS(urb->UrbHeader.Status))
        status = STATUS_DEVICE_DATA_ERROR;
    IoFreeIrp(irp);
    return status;
}

static NTSTATUS GetDescriptor(XP_DEV *dev, UCHAR type, PVOID buf, ULONG len, ULONG *got)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST u;
    NTSTATUS status;
    RtlZeroMemory(&u, sizeof(u));
    u.Hdr.Length = sizeof(u);
    u.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    u.DescriptorType = type;
    u.TransferBuffer = buf;
    u.TransferBufferLength = len;
    status = SyncUrb(dev, (PURB)&u, 5000);
    *got = NT_SUCCESS(status) ? u.TransferBufferLength : 0;
    return status;
}

static VOID AbortPipe(XP_DEV *dev, USBD_PIPE_HANDLE pipe)
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

static VOID Unconfigure(XP_DEV *dev)
{
    struct _URB_SELECT_CONFIGURATION u;
    RtlZeroMemory(&u, sizeof(u));
    u.Hdr.Length = sizeof(u);
    u.Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    u.ConfigurationDescriptor = NULL;
    (void)SyncUrb(dev, (PURB)&u, 5000);
}

#define IF_SIZE(n) (FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) + (n) * sizeof(USBD_PIPE_INFORMATION))

/* Every interface at alternate setting 0 (design 5.1); returns the pipe
 * handles of the channels' endpoints. */
static NTSTATUS SelectConfig(XP_DEV *dev, PUCHAR cfg, const XP_CONFIG *k, int drivenOnly)
{
    ULONG size = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface);
    struct _URB_SELECT_CONFIGURATION *u;
    PUSBD_INTERFACE_INFORMATION info;
    NTSTATUS status;
    unsigned i, p, c, j, n, eps;
    XU8 idx[XP_MAX_IF];

    n = XpSelectSet(k, drivenOnly, idx, &eps);
    for (j = 0; j < n; j++)
        size += IF_SIZE(k->Iface[idx[j]].NumEndpoints);
    u = ExAllocatePoolWithTag(NonPagedPool, size, XP_TAG);
    if (!u)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(u, size);
    u->Hdr.Length = (USHORT)size;
    u->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    u->ConfigurationDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)cfg;
    info = &u->Interface;
    for (j = 0; j < n; j++) {
        i = idx[j];
        info->Length = (USHORT)IF_SIZE(k->Iface[i].NumEndpoints);
        info->InterfaceNumber = k->Iface[i].Number;
        info->AlternateSetting = 0;
        info->NumberOfPipes = k->Iface[i].NumEndpoints;
        for (p = 0; p < k->Iface[i].NumEndpoints; p++)
            info->Pipes[p].MaximumTransferSize = PAGE_SIZE;
        info = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)info + info->Length);
    }
    XPT("select", size, n, eps);
    status = SyncUrb(dev, (PURB)u, 5000);
    XPT("select-done", status, (ULONG_PTR)u->ConfigurationHandle, u->Hdr.Status);
    if (NT_SUCCESS(status) && !u->ConfigurationHandle)
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (NT_SUCCESS(status)) {
        dev->ConfigHandle = u->ConfigurationHandle;
        info = &u->Interface;
        for (j = 0; j < n; j++) {
            i = idx[j];
            for (c = 0; c < dev->NumChan; c++) {
                XP_CHAN *ch = &dev->Chan[c];
                if (k->ChanIface[c] != i)
                    continue;
                for (p = 0; p < info->NumberOfPipes && p < k->Iface[i].NumEndpoints; p++) {
                    if (info->Pipes[p].EndpointAddress == ch->InAddr)
                        ch->InPipe = info->Pipes[p].PipeHandle;
                    if (info->Pipes[p].EndpointAddress == ch->OutAddr)
                        ch->OutPipe = info->Pipes[p].PipeHandle;
                }
            }
            info = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)info + IF_SIZE(k->Iface[i].NumEndpoints));
        }
        for (c = 0; c < dev->NumChan; c++)
            if (!dev->Chan[c].InPipe || !dev->Chan[c].OutPipe)
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    ExFreePool(u);
    return status;
}

/* ---- driver-owned transfers ---- */

static NTSTATUS XferAlloc(XP_DEV *dev, XP_XFER *x, XP_CHAN *chan)
{
    CCHAR stack = dev->Lower->StackSize;
    USHORT size = IoSizeOfIrp(stack);
    x->Dev = dev;
    x->Chan = chan;
    x->Busy = FALSE;
    x->Parked = FALSE;
    x->Errors = 0;
    x->Irp = ExAllocatePoolWithTag(NonPagedPool, size, XP_TAG);
    if (!x->Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    IoInitializeIrp(x->Irp, size, stack);
    return STATUS_SUCCESS;
}

static VOID XferFree(XP_XFER *x)
{
    if (x->Irp) {
        ExFreePool(x->Irp);
        x->Irp = NULL;
    }
}

static VOID XferSend(XP_XFER *x, PIO_COMPLETION_ROUTINE done)
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

static VOID SendIn(XP_XFER *x)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *u = &x->Urb.Int;
    RtlZeroMemory(u, sizeof(*u));
    u->Hdr.Length = sizeof(*u);
    u->Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    u->PipeHandle = x->Chan->InPipe;
    u->TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    u->TransferBuffer = x->Buf;
    u->TransferBufferLength = XP_PKT_LEN;
    XferSend(x, InComplete);
}

static VOID SendOut(XP_XFER *x, ULONG len)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *u = &x->Urb.Int;
    RtlZeroMemory(u, sizeof(*u));
    u->Hdr.Length = sizeof(*u);
    u->Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    u->PipeHandle = x->Chan->OutPipe;
    u->TransferFlags = USBD_TRANSFER_DIRECTION_OUT;
    u->TransferBuffer = x->Buf;
    u->TransferBufferLength = len;
    XferSend(x, OutComplete);
}

/* The XUSB start request (xpad_start_input): vendor, interface, IN. */
static VOID SendMagic(XP_XFER *x)
{
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *u = &x->Urb.Vendor;
    RtlZeroMemory(u, sizeof(*u));
    u->Hdr.Length = sizeof(*u);
    u->Hdr.Function = URB_FUNCTION_VENDOR_INTERFACE;
    u->TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    u->TransferBuffer = x->Buf;
    u->TransferBufferLength = XP_X360_MAGIC_LEN;
    u->Request = XP_X360_MAGIC_REQ;
    u->Value = XP_X360_MAGIC_VALUE;
    u->Index = XP_X360_MAGIC_INDEX;
    XferSend(x, CtlComplete);
}

/* ---- the OUT queue ---- */

/* Lock held. Queues a packet; returns the OUT length to send now (the
 * caller sends it after releasing the lock), 0 when nothing starts. */
static ULONG QueueOutLocked(XP_DEV *dev, XP_CHAN *ch, const UCHAR *data, ULONG len, BOOLEAN gipSeq)
{
    ULONG slot, i;
    if (len == 0 || len > XP_PKT_LEN || dev->Stopping)
        return 0;
    if (ch->OutCount < XP_OUTQ) {
        slot = (ch->OutHead + ch->OutCount) % XP_OUTQ;
        for (i = 0; i < len; i++)
            ch->OutQ[slot][i] = data[i];
        if (gipSeq)
            ch->OutQ[slot][2] = ch->GipSeq++;
        ch->OutQLen[slot] = (UCHAR)len;
        ch->OutCount++;
    }
    /* A full queue drops the new packet, as a missed LED or rumble is
     * better than an unbounded queue. */
    if (ch->Out.Busy || dev->PoweredDown || ch->OutCount == 0)
        return 0;
    slot = ch->OutHead;
    len = ch->OutQLen[slot];
    for (i = 0; i < len; i++)
        ch->Out.Buf[i] = ch->OutQ[slot][i];
    ch->OutHead = (ch->OutHead + 1) % XP_OUTQ;
    ch->OutCount--;
    ch->Out.Busy = TRUE;
    InterlockedIncrement(&dev->Outstanding);
    return len;
}

VOID XpQueueOut(XP_DEV *dev, XP_CHAN *ch, const UCHAR *data, ULONG len, BOOLEAN gipSeq)
{
    KIRQL irql;
    ULONG n;
    KeAcquireSpinLock(&dev->Lock, &irql);
    n = QueueOutLocked(dev, ch, data, len, gipSeq);
    KeReleaseSpinLock(&dev->Lock, irql);
    if (n)
        SendOut(&ch->Out, n);
}

static NTSTATUS OutComplete(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    XP_XFER *x = context;
    XP_DEV *dev = x->Dev;
    XP_CHAN *ch = x->Chan;
    KIRQL irql;
    ULONG len = 0, i, slot;
    UNREFERENCED_PARAMETER(unused);
    UNREFERENCED_PARAMETER(irp);

    KeAcquireSpinLock(&dev->Lock, &irql);
    /* A failed OUT packet is dropped; the next one is still sent. */
    if (ch->OutCount && !dev->Stopping && !dev->PoweredDown) {
        slot = ch->OutHead;
        len = ch->OutQLen[slot];
        for (i = 0; i < len; i++)
            x->Buf[i] = ch->OutQ[slot][i];
        ch->OutHead = (ch->OutHead + 1) % XP_OUTQ;
        ch->OutCount--;
    } else {
        x->Busy = FALSE;
    }
    KeReleaseSpinLock(&dev->Lock, irql);
    if (len)
        SendOut(x, len);
    else
        Drop(dev);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS CtlComplete(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    XP_XFER *x = context;
    XP_DEV *dev = x->Dev;
    KIRQL irql;
    UNREFERENCED_PARAMETER(unused);
    UNREFERENCED_PARAMETER(irp);
    /* Linux ignores the answer too ("unable to receive magic message"). */
    KeAcquireSpinLock(&dev->Lock, &irql);
    x->Busy = FALSE;
    KeReleaseSpinLock(&dev->Lock, irql);
    Drop(dev);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* ---- family start sequences (design 5.2-5.4) ---- */

static VOID GipInit(XP_DEV *dev, XP_CHAN *ch)
{
    unsigned cursor = 0, len;
    UCHAR pkt[XP_PKT_LEN];
    while ((len = XpGipNextInit(dev->DevDesc.idVendor, dev->DevDesc.idProduct, &cursor, pkt)) != 0)
        XpQueueOut(dev, ch, pkt, len, TRUE);
}

static VOID FamilyInit(XP_DEV *dev)
{
    UCHAR pkt[XP_PKT_LEN];
    unsigned c, len;
    KIRQL irql;
    BOOLEAN magic = FALSE;

    switch (dev->Family) {
    case XP_FAM_X360:
        len = XpX360Led(pkt, XpLedForPlayer(dev->Player < 0 ? 0 : (unsigned)dev->Player));
        XpQueueOut(dev, &dev->Chan[0], pkt, len, FALSE);
        KeAcquireSpinLock(&dev->Lock, &irql);
        if (!dev->Ctl.Busy && !dev->Stopping && !dev->PoweredDown) {
            dev->Ctl.Busy = TRUE;
            InterlockedIncrement(&dev->Outstanding);
            magic = TRUE;
        }
        KeReleaseSpinLock(&dev->Lock, irql);
        if (magic)
            SendMagic(&dev->Ctl);
        break;
    case XP_FAM_X360W:
        /* Ask the receiver to resend connection status for each slot
         * (xpad360w_start_input); the LED follows on connect. */
        for (c = 0; c < dev->NumChan; c++) {
            len = XpX360wPresence(pkt);
            XpQueueOut(dev, &dev->Chan[c], pkt, len, FALSE);
        }
        break;
    case XP_FAM_GIP:
        if (!(dev->Flags & XP_F_DELAYINIT))
            GipInit(dev, &dev->Chan[0]);
        break;
    }
}

/* ---- interrupt IN ---- */

static NTSTATUS InComplete(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    XP_XFER *x = context;
    XP_DEV *dev = x->Dev;
    XP_CHAN *ch = x->Chan;
    NTSTATUS status = irp->IoStatus.Status;
    KIRQL irql;
    BOOLEAN resubmit;
    PIRP read;
    ULONG outLen = 0, outLen2 = 0, ackLen = 0;
    UCHAR ack[XP_PKT_LEN], led[XP_PKT_LEN];
    BOOLEAN delayedInit = FALSE;
    UNREFERENCED_PARAMETER(unused);

    KeAcquireSpinLock(&dev->Lock, &irql);
    if (NT_SUCCESS(status) && USBD_SUCCESS(x->Urb.Int.Hdr.Status)) {
        XP_PAD before = ch->Pad;
        unsigned len = x->Urb.Int.TransferBufferLength, ev = 0, n;
#if defined(XP_TRACE_E9)
        static ULONG traced;
        if (traced < 24) {
            traced++;
            XPTB("in", x->Buf, len > 24 ? 24 : len);
        }
#endif
        if (len > XP_PKT_LEN)
            len = XP_PKT_LEN;
        x->Errors = 0;
        switch (dev->Family) {
        case XP_FAM_X360:
            ev = XpX360Parse(x->Buf, len, &ch->Pad);
            break;
        case XP_FAM_X360W:
            ev = XpX360wParse(x->Buf, len, &ch->Pad);
            if (ev & XP_EV_CONNECT) {
                /* Light the slot's quadrant (xpad_identify_controller). */
                n = XpX360wLed(led, XpLedForPlayer(ch->Index));
                outLen2 = QueueOutLocked(dev, ch, led, n, FALSE);
            }
            break;
        case XP_FAM_GIP:
            ev = XpGipParse(x->Buf, len, &ch->Pad, ack, &n);
            if (ev & XP_EV_ACK)
                ackLen = n;
            if ((ev & XP_EV_ANNOUNCE) && (dev->Flags & XP_F_DELAYINIT) && !ch->DelayedInitDone) {
                ch->DelayedInitDone = TRUE;
                delayedInit = TRUE;
            }
            break;
        }
        if (ackLen)
            outLen = QueueOutLocked(dev, ch, ack, ackLen, FALSE);
        if (!XpPadEqual(&before, &ch->Pad))
            ch->Dirty = TRUE;
    } else if (status != STATUS_CANCELLED) {
        XPT("in-error", status, x->Urb.Int.Hdr.Status, x->Errors);
        x->Errors++;
    }
    resubmit = (BOOLEAN)(!dev->Stopping && !dev->PoweredDown &&
                         status != STATUS_CANCELLED && x->Errors < XP_IN_ERROR_LIMIT);
    if (!resubmit) {
        x->Busy = FALSE;
        x->Parked = (BOOLEAN)!dev->Stopping;
    }
    read = XpTakeReadyReadLocked(dev);
    KeReleaseSpinLock(&dev->Lock, irql);

    if (read)
        IoCompleteRequest(read, IO_NO_INCREMENT);
    /* At most one of these started the idle OUT transfer. */
    if (outLen)
        SendOut(&ch->Out, outLen);
    else if (outLen2)
        SendOut(&ch->Out, outLen2);
    if (delayedInit)
        GipInit(dev, ch);
    if (resubmit)
        SendIn(x);
    else
        Drop(dev);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* ---- start, stop, resume ---- */

VOID XpUsbFree(XP_DEV *dev)
{
    unsigned c, i;
    for (c = 0; c < XP_MAX_CHAN; c++) {
        for (i = 0; i < XP_IN_PER_CHAN; i++)
            XferFree(&dev->Chan[c].In[i]);
        XferFree(&dev->Chan[c].Out);
    }
    XferFree(&dev->Ctl);
}

NTSTATUS XpUsbStart(XP_DEV *dev)
{
    PUCHAR cfg = NULL;
    XP_CONFIG *k = NULL;
    ULONG got;
    NTSTATUS status;
    unsigned c, i, total;
    XP_PAD rest;

    XpUsbFree(dev);
    dev->Known = NULL;
    dev->Family = XP_FAM_NONE;
    dev->NumChan = 0;
    dev->ConfigHandle = NULL;
    RtlZeroMemory(dev->Chan, sizeof(dev->Chan));
    RtlZeroMemory(&dev->Ctl, sizeof(dev->Ctl));

    status = GetDescriptor(dev, USB_DEVICE_DESCRIPTOR_TYPE, &dev->DevDesc, sizeof(dev->DevDesc), &got);
    if (NT_SUCCESS(status) && got != sizeof(dev->DevDesc))
        status = STATUS_DEVICE_DATA_ERROR;
    if (!NT_SUCCESS(status))
        return status;

    cfg = ExAllocatePoolWithTag(NonPagedPool, XP_CFG_MAX, XP_TAG);
    k = ExAllocatePoolWithTag(NonPagedPool, sizeof(*k), XP_TAG);
    if (!cfg || !k) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    status = GetDescriptor(dev, USB_CONFIGURATION_DESCRIPTOR_TYPE, cfg, 9, &got);
    if (!NT_SUCCESS(status))
        goto done;
    total = cfg[2] | ((unsigned)cfg[3] << 8);
    if (got != 9 || total < 9 || total > XP_CFG_MAX) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto done;
    }
    status = GetDescriptor(dev, USB_CONFIGURATION_DESCRIPTOR_TYPE, cfg, total, &got);
    if (!NT_SUCCESS(status))
        goto done;
    XPTB("config", cfg, got > 64 ? 64 : got);
    XPT("parse", got, total, 0);
    if (got != total || XpParseConfig(cfg, got, k) != 0 || !k->AllVendor) {
        /* Not an interface this driver drives, or a composite device with
         * a standard-class function a whole-device match would take away
         * (design 5.1): the start fails as the design says. */
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto done;
    }
    dev->Known = XpFindDevice(dev->DevDesc.idVendor, dev->DevDesc.idProduct);
    if (dev->Known && dev->Known->Family != k->Family) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto done;
    }
    dev->Family = k->Family;
    dev->Flags = dev->Known ? dev->Known->Flags : 0;
    dev->NumChan = k->NumChannels;
    for (c = 0; c < dev->NumChan; c++) {
        dev->Chan[c].Index = (UCHAR)c;
        dev->Chan[c].InAddr = k->ChanIn[c];
        dev->Chan[c].OutAddr = k->ChanOut[c];
    }

    XpReadOptions(dev);
    dev->ReportDescLen = (USHORT)XpBuildReportDescriptor(&dev->Options,
        dev->Family == XP_FAM_X360W ? dev->NumChan : 1, dev->ReportDesc, sizeof(dev->ReportDesc));
    if (!dev->ReportDescLen) {
        status = STATUS_INTERNAL_ERROR;
        goto done;
    }

    status = SelectConfig(dev, cfg, k, 0);
    if (!NT_SUCCESS(status)) {
        /* Every interface at alternate 0 was refused: try again with only
         * the pad's own interfaces (XpSelectSet; xHCI98's endpoint cap). */
        unsigned all, driven;
        XU8 idx[XP_MAX_IF];
        all = XpSelectSet(k, 0, idx, &driven);
        if (XpSelectSet(k, 1, idx, &driven) < all)
            status = SelectConfig(dev, cfg, k, 1);
    }
    if (!NT_SUCCESS(status))
        goto done;

    for (c = 0; c < dev->NumChan && NT_SUCCESS(status); c++) {
        for (i = 0; i < XP_IN_PER_CHAN && NT_SUCCESS(status); i++)
            status = XferAlloc(dev, &dev->Chan[c].In[i], &dev->Chan[c]);
        if (NT_SUCCESS(status))
            status = XferAlloc(dev, &dev->Chan[c].Out, &dev->Chan[c]);
    }
    if (NT_SUCCESS(status))
        status = XferAlloc(dev, &dev->Ctl, &dev->Chan[0]);
    if (!NT_SUCCESS(status))
        goto done;

    XpPadReset(&rest, (XU16)(dev->Family == XP_FAM_GIP ? 1023 : 255));
    for (c = 0; c < dev->NumChan; c++) {
        dev->Chan[c].Pad = rest;
        dev->Chan[c].Dirty = TRUE;      /* the first read reports rest */
    }
    if (dev->Family == XP_FAM_X360 && dev->Player < 0)
        dev->Player = XpAllocPlayer();

    KeClearEvent(&dev->IoIdle);
    dev->Outstanding = 1;               /* the bias, dropped by stop */
    dev->Stopping = FALSE;
    dev->PoweredDown = FALSE;
    dev->Started = TRUE;

    for (c = 0; c < dev->NumChan; c++)
        for (i = 0; i < XP_IN_PER_CHAN; i++) {
            dev->Chan[c].In[i].Busy = TRUE;
            InterlockedIncrement(&dev->Outstanding);
            SendIn(&dev->Chan[c].In[i]);
        }
    FamilyInit(dev);
    status = STATUS_SUCCESS;

done:
    XPT("usbstart", status, dev->Family, dev->ReportDescLen);
    if (cfg)
        ExFreePool(cfg);
    if (k)
        ExFreePool(k);
    if (!NT_SUCCESS(status) && !dev->Started)
        XpUsbFree(dev);
    return status;
}

/* PASSIVE_LEVEL: STOP_DEVICE, SURPRISE_REMOVAL, REMOVE_DEVICE. */
VOID XpUsbStop(XP_DEV *dev, BOOLEAN present)
{
    KIRQL irql;
    LARGE_INTEGER timeout;
    unsigned c, i;

    if (!dev->Started)
        return;
    KeAcquireSpinLock(&dev->Lock, &irql);
    dev->Stopping = TRUE;
    KeReleaseSpinLock(&dev->Lock, irql);

    for (c = 0; c < dev->NumChan; c++) {
        AbortPipe(dev, dev->Chan[c].InPipe);
        AbortPipe(dev, dev->Chan[c].OutPipe);
    }
    Drop(dev);      /* the bias */
    timeout.QuadPart = -20000000;       /* 2 s */
    if (KeWaitForSingleObject(&dev->IoIdle, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
        /* The control request has no pipe to abort. */
        for (c = 0; c < dev->NumChan; c++) {
            for (i = 0; i < XP_IN_PER_CHAN; i++)
                if (dev->Chan[c].In[i].Busy)
                    IoCancelIrp(dev->Chan[c].In[i].Irp);
            if (dev->Chan[c].Out.Busy)
                IoCancelIrp(dev->Chan[c].Out.Irp);
        }
        if (dev->Ctl.Busy)
            IoCancelIrp(dev->Ctl.Irp);
        KeWaitForSingleObject(&dev->IoIdle, Executive, KernelMode, FALSE, NULL);
    }
    XpFailReads(dev, STATUS_DEVICE_NOT_CONNECTED);
    if (present)
        Unconfigure(dev);
    for (c = 0; c < dev->NumChan; c++) {
        dev->Chan[c].InPipe = NULL;
        dev->Chan[c].OutPipe = NULL;
        dev->Chan[c].OutCount = 0;
        for (i = 0; i < XP_IN_PER_CHAN; i++)
            dev->Chan[c].In[i].Parked = FALSE;
    }
    dev->Started = FALSE;
}

/* Any IRQL up to DISPATCH_LEVEL: after the device returns to D0, re-arm
 * the parked IN transfers and run the family's start packets again, as
 * Linux xpad_resume does. */
VOID XpUsbResume(XP_DEV *dev)
{
    KIRQL irql;
    XP_XFER *arm[XP_MAX_CHAN * XP_IN_PER_CHAN];
    ULONG out[XP_MAX_CHAN];
    unsigned n = 0, c, i, slot, j;

    KeAcquireSpinLock(&dev->Lock, &irql);
    if (!dev->Started || dev->Stopping) {
        KeReleaseSpinLock(&dev->Lock, irql);
        return;
    }
    dev->PoweredDown = FALSE;
    for (c = 0; c < dev->NumChan; c++) {
        XP_CHAN *ch = &dev->Chan[c];
        for (i = 0; i < XP_IN_PER_CHAN; i++) {
            XP_XFER *x = &ch->In[i];
            if (!x->Busy) {
                x->Busy = TRUE;
                x->Parked = FALSE;
                x->Errors = 0;
                InterlockedIncrement(&dev->Outstanding);
                arm[n++] = x;
            }
        }
        out[c] = 0;
        if (!ch->Out.Busy && ch->OutCount) {
            slot = ch->OutHead;
            out[c] = ch->OutQLen[slot];
            for (j = 0; j < out[c]; j++)
                ch->Out.Buf[j] = ch->OutQ[slot][j];
            ch->OutHead = (ch->OutHead + 1) % XP_OUTQ;
            ch->OutCount--;
            ch->Out.Busy = TRUE;
            InterlockedIncrement(&dev->Outstanding);
        }
        ch->DelayedInitDone = FALSE;
    }
    KeReleaseSpinLock(&dev->Lock, irql);
    for (i = 0; i < n; i++)
        SendIn(arm[i]);
    for (c = 0; c < dev->NumChan; c++)
        if (out[c])
            SendOut(&dev->Chan[c].Out, out[c]);
    FamilyInit(dev);
}

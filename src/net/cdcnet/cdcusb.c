/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * CDCNET WDM/USB lower edge (ethernet.md sections 4.3, 4.7, 5 and 6).
 * Original code written from the Windows 2000 DDK headers (wdm.h,
 * usbdi.h, usb100.h, usbioctl.h) and the documented URB functions; no DDK
 * sample was read. It owns the USB device for one adapter:
 *
 * - CdcUsbStart probes every configuration (cdcdesc.c), selects the
 *   chosen function's interfaces, runs the protocol's setup (NCM NTB
 *   parameters, RNDIS INITIALIZE), reads the MAC and starts the data path.
 * - The data path is asynchronous: N bulk-IN transfers, one interrupt-IN
 *   transfer and one logical bulk-OUT transfer (payload, then a separate
 *   zero-length packet where ECM/NCM need one) are outstanding. Every
 *   submission goes through one pump per adapter: a completion never
 *   calls IoCallDriver itself, it marks its slot ready and kicks the
 *   pump, which iterates instead of nesting when completions arrive
 *   inline (section 4.7.1).
 * - Each IRP is allocated for one submission and freed by its own
 *   completion routine, which returns STATUS_MORE_PROCESSING_REQUIRED.
 *   Data IRPs are never cancelled by pointer: quiesce closes the gate,
 *   waits for the pump to leave IoCallDriver, then aborts the pipes
 *   (URB_FUNCTION_ABORT_PIPE) and waits for every completion. A timeout
 *   never frees memory a lower driver still owns.
 * - One passive worker thread per adapter runs everything that needs
 *   PASSIVE_LEVEL: control requests for NDIS OIDs, reset, power, error
 *   recovery and RNDIS keepalives. It signals an event before
 *   PsTerminateSystemThread; Windows 98 faults a wait on a thread object
 *   (xHCI98 docs/usb-xhci-info/win98-wdm.md, Oney p.110), so the thread
 *   object is waited on only where WDM 1.10 (Windows 2000) is available.
 */
#include <wdm.h>
#include <usbdi.h>
#include <stdarg.h>
#include "cdcusb.h"

#ifndef USBD_STATUS_DEVICE_GONE
#define USBD_STATUS_DEVICE_GONE ((USBD_STATUS)0xC0007000L) /* later usb.h; absent from the 2000 DDK */
#endif

#define TAG 'uCdC'
#define RX_MAX 8
#define RX_ECM 2048
#define RX_BLOCK 16384            /* NCM NTB and RNDIS transfer limit, each way */
#define CONFIG_MAX 4096
#define CTRL_MAX (RNDIS_CONTROL_MAX + 3)
#define CTRL_TIMEOUT 2000

#define WORK_RECOVER 0x01
#define WORK_FETCH   0x02
#define WORK_REQ     0x04
#define WORK_RESET   0x08
#define WORK_KICK    0x10

#define TX_IDLE  0
#define TX_BUILD 1
#define TX_DATA  2
#define TX_ZLP_READY 3
#define TX_ZLP   4

#define SLOT_RX  0
#define SLOT_INT 1
#define SLOT_TX  2

typedef struct CDC_DEV CDC_DEV;

typedef struct SLOT {
    CDC_DEV *Dev;
    struct _URB_BULK_OR_INTERRUPT_TRANSFER Urb;
    PUCHAR Buffer;
    ULONG Size;
    UCHAR Kind;
    BOOLEAN Busy;
} SLOT;

struct CDC_DEV {
    PDEVICE_OBJECT Lower;
    CDC_CALLBACKS Cb;
    CDC_FUNCTION Fn;
    UCHAR ConfigIndex;
    USBD_CONFIGURATION_HANDLE ConfigHandle;
    USBD_PIPE_HANDLE In, Out, Int;
    USHORT InMax, OutMax;
    BOOLEAN HighSpeed;

    KSPIN_LOCK Lock;              /* everything below up to the worker */
    BOOLEAN Open;                 /* the submission gate */
    BOOLEAN Gone, GoneReported, Suspended, PumpActive, PumpAgain;
    ULONG Outstanding;            /* data IRPs submitted and not completed */
    KEVENT Drained;               /* notification: !Open && !Outstanding */
    SLOT Rx[RX_MAX];
    ULONG RxCount;
    SLOT IntSlot;
    SLOT Tx;
    ULONG TxStage, TxCount, TxCap;
    BOOLEAN TxZlp;
    void *TxPackets[CDC_TX_BATCH];
    ULONG Work;
    BOOLEAN Stop;
    int Req;                      /* the pending OID-driven request, 0 if none */
    ULONG ReqValue, ReqLength;
    UCHAR ReqData[32 * 6];
    BOOLEAN ResetPending;
    ULONG IdleTicks;
    BOOLEAN Connected;
    ULONG Speed;

    /* Worker-only state. */
    KEVENT Wake;                  /* synchronization */
    KEVENT Exited;                /* notification, set by the worker last */
    PVOID ThreadObject;           /* referenced on NT only */
    KEVENT CtrlDone;
    PUCHAR Ctrl;                  /* nonpaged control scratch, CTRL_MAX */
    ULONG Recoveries;
    ULONG RndisId, RndisMaxPackets, RndisMaxTransfer, RndisAlign;
    ULONG KeepaliveFailures;
    NCM_PARAMS Ncm;
    USHORT NcmSeq;
    ULONG Filter, McCount;
    BOOLEAN FilterValid;
    UCHAR Mc[32 * 6];
    ULONG Malformed;
    CDC_NOTIFY_STATE Notify;   /* interrupt-IN reassembly (CdcNotifyFeed) */
};

/* ------------------------------------------------------------------ */
/* Trace: a small formatter (%x %u %d %s %c), no CRT. The trace flavour
 * writes to QEMU's debug console port 0xE9; it is test-only and never
 * released. Checked builds also use DbgPrint. */

static void Out(char c)
{
#if defined(CDC_TRACE_E9)
#if defined(_M_AMD64)
    __outbyte(0xe9, (unsigned char)c);
#else
    __asm { mov dx, 0xe9
            mov al, c
            out dx, al }
#endif
#else
    UNREFERENCED_PARAMETER(c);
#endif
}

void CdcTrace(const char *format, ...)
{
#if defined(CDC_TRACE_E9) || DBG
    char line[160];
    ULONG n = 0;
    va_list args;
    const char *p;
    va_start(args, format);
    for (p = format; *p && n < sizeof(line) - 12; p++) {
        if (*p != '%' || !p[1]) { line[n++] = *p; continue; }
        p++;
        if (*p == 's') {
            const char *s = va_arg(args, const char *);
            while (s && *s && n < sizeof(line) - 12) line[n++] = *s++;
        } else if (*p == 'c') {
            line[n++] = (char)va_arg(args, int);
        } else if (*p == 'x' || *p == 'u' || *p == 'd') {
            ULONG v = va_arg(args, ULONG), base = *p == 'x' ? 16 : 10;
            char digits[11];
            int k = 0;
            if (*p == 'd' && (LONG)v < 0) { line[n++] = '-'; v = (ULONG)-(LONG)v; }
            do { digits[k++] = "0123456789abcdef"[v % base]; v /= base; } while (v && k < 11);
            while (k) line[n++] = digits[--k];
        } else {
            line[n++] = *p;
        }
    }
    va_end(args);
    line[n++] = '\n';
    line[n] = 0;
    {
        ULONG i;
        Out('c'); Out('d'); Out('c'); Out(':'); Out(' ');
        for (i = 0; i < n; i++) Out(line[i]);
    }
#if DBG
    DbgPrint("cdcnet: %s", line);
#endif
#else
    UNREFERENCED_PARAMETER(format);
#endif
}

/* ------------------------------------------------------------------ */
/* Waits. Windows 98 can return an undocumented 0xFFFFFFFF from a wait
 * (Oney p.93): it is never success, and an infinite wait simply waits
 * again. */

static NTSTATUS Wait(PKEVENT event, ULONG ms)
{
    LARGE_INTEGER t;
    NTSTATUS s;
    for (;;) {
        t.QuadPart = -(LONGLONG)ms * 10000;
        s = KeWaitForSingleObject(event, Executive, KernelMode, FALSE, ms ? &t : NULL);
        if (s == STATUS_SUCCESS || s == STATUS_TIMEOUT) return s;
    }
}

static void Sleep(ULONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

/* ------------------------------------------------------------------ */
/* Synchronous URBs, PASSIVE_LEVEL only: Start, Stop and the worker,
 * which never run at the same time for one adapter. */

static NTSTATUS SyncDone(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    UNREFERENCED_PARAMETER(unused);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;  /* the caller frees the IRP */
}

static NTSTATUS Sync(CDC_DEV *d, PURB urb, ULONG timeoutMs)
{
    PIRP irp;
    PIO_STACK_LOCATION stack;
    NTSTATUS status;
    irp = IoAllocateIrp(d->Lower->StackSize, FALSE);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent(&d->CtrlDone, NotificationEvent, FALSE);
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = urb;
    IoSetCompletionRoutine(irp, SyncDone, &d->CtrlDone, TRUE, TRUE, TRUE);
    (void)IoCallDriver(d->Lower, irp);
    if (Wait(&d->CtrlDone, timeoutMs) == STATUS_TIMEOUT) {
        /* Cancellation does not return ownership: completion does. */
        BOOLEAN cancelled = IoCancelIrp(irp);
        CdcTrace("sync: URB %x timed out, cancel %u", urb->UrbHeader.Function, cancelled);
        Wait(&d->CtrlDone, 0);
        CdcTrace("sync: URB %x completed after cancel", urb->UrbHeader.Function);
        status = cancelled && irp->IoStatus.Status == STATUS_CANCELLED ?
                 STATUS_IO_TIMEOUT : irp->IoStatus.Status;
    } else {
        status = irp->IoStatus.Status;
    }
    IoFreeIrp(irp);
    if (NT_SUCCESS(status) && !USBD_SUCCESS(urb->UrbHeader.Status)) status = STATUS_UNSUCCESSFUL;
    return status;
}

static NTSTATUS GetDescriptor(CDC_DEV *d, UCHAR type, UCHAR index, USHORT lang,
                              PVOID buffer, ULONG length, PULONG actual)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
    NTSTATUS status;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    urb.DescriptorType = type; urb.Index = index; urb.LanguageId = lang;
    urb.TransferBuffer = buffer; urb.TransferBufferLength = length;
    status = Sync(d, (PURB)&urb, CTRL_TIMEOUT);
    *actual = NT_SUCCESS(status) ? urb.TransferBufferLength : 0;
    return status;
}

/* A class request to the control interface; data through d->Ctrl. */
static NTSTATUS ClassRequest(CDC_DEV *d, BOOLEAN in, UCHAR request, USHORT value,
                             ULONG length, PULONG actual)
{
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST urb;
    NTSTATUS status;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_CLASS_INTERFACE;
    urb.TransferFlags = in ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0;
    urb.TransferBuffer = length ? d->Ctrl : NULL;
    urb.TransferBufferLength = length;
    urb.Request = request; urb.Value = value; urb.Index = d->Fn.Control;
    status = Sync(d, (PURB)&urb, CTRL_TIMEOUT);
    if (actual) *actual = NT_SUCCESS(status) ? urb.TransferBufferLength : 0;
    return status;
}

static NTSTATUS PipeRequest(CDC_DEV *d, USHORT function, USBD_PIPE_HANDLE pipe)
{
    struct _URB_PIPE_REQUEST urb;
    if (!pipe) return STATUS_SUCCESS;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = function;
    urb.PipeHandle = pipe;
    return Sync(d, (PURB)&urb, CTRL_TIMEOUT);
}

/* ------------------------------------------------------------------ */
/* The data path. */

static NTSTATUS Done(PDEVICE_OBJECT unused, PIRP irp, PVOID context);
static void Pump(CDC_DEV *d);

static BOOLEAN DeviceGone(NTSTATUS status, USBD_STATUS usb)
{
    return status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_NO_SUCH_DEVICE ||
           status == STATUS_DEVICE_REMOVED || usb == USBD_STATUS_DEVICE_GONE;
}

/* Lock held. A failed data transfer while the gate is open closes it and
 * asks the worker to recover; a transfer failing because the gate closed
 * (aborted) does not. */
static void Fault(CDC_DEV *d, NTSTATUS status, USBD_STATUS usb)
{
    if (DeviceGone(status, usb)) d->Gone = TRUE;
    if (d->Open) {
        CdcTrace("fault %x usbd %x", status, usb);
        d->Open = FALSE;
        d->Work |= WORK_RECOVER;
        KeSetEvent(&d->Wake, IO_NO_INCREMENT, FALSE);
    }
}

/* Lock held: wake a quiesce waiting for the last completion. */
static void CheckDrained(CDC_DEV *d)
{
    if (!d->Open && !d->Outstanding && !d->PumpActive) KeSetEvent(&d->Drained, IO_NO_INCREMENT, FALSE);
}

static void Submit(CDC_DEV *d, SLOT *s, ULONG length)
{
    PIRP irp = IoAllocateIrp(d->Lower->StackSize, FALSE);
    PIO_STACK_LOCATION stack;
    KIRQL old;
    if (!irp) {
        /* Retry from the worker's next tick; a transmit fails now. */
        void *packets[CDC_TX_BATCH];
        ULONG count = 0;
        KeAcquireSpinLock(&d->Lock, &old);
        s->Busy = FALSE;
        d->Outstanding--;
        if (s->Kind == SLOT_TX) {
            count = d->TxCount;
            RtlCopyMemory(packets, d->TxPackets, count * sizeof(void *));
            d->TxCount = 0; d->TxStage = TX_IDLE;
            if (count && d->TxZlp) Fault(d, STATUS_INSUFFICIENT_RESOURCES, 0);
        }
        d->Work |= WORK_KICK;
        CheckDrained(d);
        KeReleaseSpinLock(&d->Lock, old);
        if (count) d->Cb.TxDone(d->Cb.Context, packets, count, 0);
        return;
    }
    RtlZeroMemory(&s->Urb, sizeof(s->Urb));
    s->Urb.Hdr.Length = sizeof(s->Urb);
    s->Urb.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    s->Urb.PipeHandle = s->Kind == SLOT_RX ? d->In : s->Kind == SLOT_INT ? d->Int : d->Out;
    s->Urb.TransferFlags = s->Kind == SLOT_TX ? 0 : USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    s->Urb.TransferBuffer = s->Buffer;
    s->Urb.TransferBufferLength = length;
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = &s->Urb;
    IoSetCompletionRoutine(irp, Done, s, TRUE, TRUE, TRUE);
    (void)IoCallDriver(d->Lower, irp);
}

static cdc_u32 AlignUp(cdc_u32 v, cdc_u32 a) { return (v + a - 1) & ~(a - 1); }

/* Build the next bulk-OUT transfer. Called by the pump with TxStage ==
 * TX_BUILD and the lock not held. Returns TRUE if a transfer was queued. */
static BOOLEAN BuildTx(CDC_DEV *d)
{
    void *packets[CDC_TX_BATCH];
    cdc_u32 lengths[CDC_TX_BATCH], offsets[CDC_TX_BATCH];
    cdc_u32 max = 1, count, i, total = 0, ndp = 0;
    PUCHAR b = d->Tx.Buffer;
    BOOLEAN ok = TRUE, zlp = FALSE;
    KIRQL old;

    /* Take no more frames than fit at their largest, so a batch is never
     * split or refused for space once taken. */
    if (d->Fn.Protocol == CDC_PROTO_NCM) {
        cdc_u32 fixed;
        max = d->Ncm.OutMaxDatagrams;
        for (;;) {
            fixed = AlignUp(12, d->Ncm.OutAlignment) + 8 + 4 * (max + 1);
            if (max <= 1 || fixed + max * (CDC_ETH_MAX + d->Ncm.OutDivisor) <= d->TxCap) break;
            max--;
        }
    } else if (d->Fn.Protocol == CDC_PROTO_RNDIS) {
        cdc_u32 each = RNDIS_PACKET_HEADER + CDC_ETH_MAX + (1UL << d->RndisAlign) - 1;
        max = (d->TxCap - 1) / each;
        if (max > d->RndisMaxPackets) max = d->RndisMaxPackets;
        if (!max) max = 1;
    }
    if (max > CDC_TX_BATCH) max = CDC_TX_BATCH;
    count = d->Cb.TxTake(d->Cb.Context, packets, max);
    if (!count) {
        KeAcquireSpinLock(&d->Lock, &old);
        d->TxStage = TX_IDLE;
        KeReleaseSpinLock(&d->Lock, old);
        return FALSE;
    }
    total = 0;
    if (d->Fn.Protocol == CDC_PROTO_ECM) {
        total = d->Cb.TxCopy(d->Cb.Context, packets[0], b, CDC_ETH_MAX);
        if (total < CDC_ETH_MIN) ok = FALSE;
        else if (total < 60) { RtlZeroMemory(b + total, 60 - total); total = 60; }
        zlp = (total % d->OutMax) == 0;
    } else if (d->Fn.Protocol == CDC_PROTO_NCM) {
        /* Copy each frame to its planned place; the plan is recomputed as
         * frames are copied because only the copy yields a length. */
        cdc_u32 used = 0, at = AlignUp(12, d->Ncm.OutAlignment) + 8 + 4 * (count + 1);
        ndp = AlignUp(12, d->Ncm.OutAlignment);
        for (i = 0; i < count && ok; i++) {
            cdc_u32 start = at - (at % d->Ncm.OutDivisor) + d->Ncm.OutRemainder, n;
            if (start < at) start += d->Ncm.OutDivisor;
            if (start >= d->TxCap) { ok = FALSE; break; }
            n = d->Cb.TxCopy(d->Cb.Context, packets[i], b + start,
                             d->TxCap - start > CDC_ETH_MAX ? CDC_ETH_MAX : d->TxCap - start);
            if (n < CDC_ETH_MIN) { ok = FALSE; break; }
            offsets[i] = start; lengths[i] = n; at = start + n; used++;
        }
        if (ok) {
            CdcNcmTxFinish(b, d->NcmSeq++, lengths, offsets, used, ndp, at);
            total = at;
            zlp = (total % d->OutMax) == 0 && total < d->Ncm.OutMaxSize;
        }
    } else {
        for (i = 0; i < count && ok; i++) {
            cdc_u32 n, room = d->TxCap - 1;
            if (total + RNDIS_PACKET_HEADER + CDC_ETH_MIN > room) { ok = FALSE; break; }
            room -= total + RNDIS_PACKET_HEADER;
            n = d->Cb.TxCopy(d->Cb.Context, packets[i], b + total + RNDIS_PACKET_HEADER,
                             room > CDC_ETH_MAX ? CDC_ETH_MAX : room);
            if (n < CDC_ETH_MIN) { ok = FALSE; break; }
            n = CdcRndisTxHeader(b + total, n, d->RndisAlign);
            if (n > d->TxCap - total) { ok = FALSE; break; }
            total += n;
        }
        /* The RNDIS short-packet rule: one zero byte outside the message,
         * never a ZLP, when the transfer would end on a packet boundary. */
        if (ok && total % d->OutMax == 0) {
            if (total < d->TxCap) b[total++] = 0;
            else zlp = TRUE;
        }
    }
    KeAcquireSpinLock(&d->Lock, &old);
    if (!ok || !d->Open) {
        d->TxStage = TX_IDLE;
        KeReleaseSpinLock(&d->Lock, old);
        if (!ok) { d->Malformed++; CdcTrace("tx build failed, %u frames", count); }
        d->Cb.TxDone(d->Cb.Context, packets, count, 0);
        return FALSE;
    }
    RtlCopyMemory(d->TxPackets, packets, count * sizeof(void *));
    d->TxCount = count;
    d->TxZlp = zlp;
    d->TxStage = TX_DATA;
    d->Tx.Busy = TRUE;
    d->Outstanding++;
    KeReleaseSpinLock(&d->Lock, old);
    Submit(d, &d->Tx, total);
    return TRUE;
}

static void Pump(CDC_DEV *d)
{
    KIRQL old;
    BOOLEAN txTried = FALSE;
    KeAcquireSpinLock(&d->Lock, &old);
    if (d->PumpActive) {
        d->PumpAgain = TRUE;
        KeReleaseSpinLock(&d->Lock, old);
        return;
    }
    d->PumpActive = TRUE;
    for (;;) {
        SLOT *s = NULL;
        ULONG i, length = 0;
        BOOLEAN build = FALSE;
        if (d->PumpAgain) { d->PumpAgain = FALSE; txTried = FALSE; }
        if (d->Open) {
            if (d->Int && !d->IntSlot.Busy) { s = &d->IntSlot; length = s->Size; }
            for (i = 0; !s && i < d->RxCount; i++)
                if (!d->Rx[i].Busy) { s = &d->Rx[i]; length = s->Size; }
            if (!s && d->TxStage == TX_ZLP_READY) {
                d->TxStage = TX_ZLP; s = &d->Tx; length = 0;
            } else if (!s && d->TxStage == TX_IDLE && !txTried) {
                d->TxStage = TX_BUILD; build = TRUE;
            }
        }
        if (!s && !build) {
            if (d->PumpAgain) continue;
            break;
        }
        if (s) { s->Busy = TRUE; d->Outstanding++; }
        KeReleaseSpinLock(&d->Lock, old);
        if (s) Submit(d, s, length);
        else if (!BuildTx(d)) txTried = TRUE;
        KeAcquireSpinLock(&d->Lock, &old);
    }
    d->PumpActive = FALSE;
    CheckDrained(d);
    KeReleaseSpinLock(&d->Lock, old);
}

static void Emit(void *context, const cdc_u8 *frame, cdc_u32 length)
{
    CDC_DEV *d = context;
    d->Cb.Receive(d->Cb.Context, frame, length);
}

/* The interrupt-IN transfer size: one wMaxPacketSize (ethernet.md section
 * 6.1), so every packet ends its transfer; 64 when the descriptor gives
 * none or more than the reassembly buffer holds. */
static ULONG IntTransferSize(ULONG maxPacket)
{
    return maxPacket && maxPacket <= 64 ? maxPacket : 64;
}

static void Notification(CDC_DEV *d, PUCHAR b, ULONG length)
{
    KIRQL old;
    int link;
    CDC_NOTIFY list[CDC_NOTIFY_BUF / 8];   /* every notification a completion can hold */
    cdc_u32 count, i, before;
    if (d->Fn.Protocol == CDC_PROTO_RNDIS) {
        /* RESPONSE_AVAILABLE: fetch on the worker. */
        KeAcquireSpinLock(&d->Lock, &old);
        d->Work |= WORK_FETCH;
        KeSetEvent(&d->Wake, IO_NO_INCREMENT, FALSE);
        KeReleaseSpinLock(&d->Lock, old);
        return;
    }
#if defined(CDC_TRACE_E9) || DBG
    /* Trace and checked flavours only: every completion's size and the
     * first bytes where a second notification could start (8, 16). */
    CdcTrace("interrupt %u bytes: %x %x, at 8: %x %x, at 16: %x %x", length, length > 0 ? b[0] : 0,
             length > 1 ? b[1] : 0, length > 9 ? b[8] : 0, length > 9 ? b[9] : 0,
             length > 17 ? b[16] : 0, length > 17 ? b[17] : 0);
#endif
    /* A completion may hold several notifications, or the first part of
     * one longer than a packet (ethernet.md section 6.1): handle each whole
     * one in order. */
    before = d->Notify.Truncated;
    count = CdcNotifyFeed(&d->Notify, b, length, length == d->Fn.IntMax, list, CDC_NOTIFY_BUF / 8);
    if (d->Notify.Truncated != before && d->Notify.Truncated <= 20)
        CdcTrace("notification tail ignored (%u bytes received)", length);
    for (i = 0; i < count; i++) {
        const CDC_NOTIFY n = list[i];
        link = -1;
        if (n.Code == 0x00) {                            /* NETWORK_CONNECTION */
            link = n.Value != 0;
        } else if (n.Code == 0x2a && n.Length >= 8) {    /* CONNECTION_SPEED_CHANGE */
            ULONG down = n.Data[0] | (n.Data[1] << 8) | (n.Data[2] << 16) | ((ULONG)n.Data[3] << 24);
            if (down >= 100) d->Speed = down / 100;
            link = d->Connected;
        }
        if (link < 0) continue;
#if defined(CDC_TRACE_E9) || DBG
        /* Trace and checked flavours only: what the device reported. */
        CdcTrace("notification %x: link %u, %u kb/s", n.Code, link, d->Speed / 10);
#endif
        d->Connected = (BOOLEAN)link;
        d->Cb.Link(d->Cb.Context, link, d->Speed);
    }
}

static NTSTATUS Done(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    SLOT *s = context;
    CDC_DEV *d = s->Dev;
    NTSTATUS status = irp->IoStatus.Status;
    USBD_STATUS usb = s->Urb.Hdr.Status;
    ULONG length = s->Urb.TransferBufferLength;
    KIRQL old;
    UNREFERENCED_PARAMETER(unused);
    IoFreeIrp(irp);
    if (NT_SUCCESS(status) && !USBD_SUCCESS(usb)) status = STATUS_UNSUCCESSFUL;
    if (length > s->Size) length = 0;

    if (s->Kind != SLOT_TX) {
        /* Parse before the slot becomes free: the pump may reuse it. */
        if (NT_SUCCESS(status) && d->Open) {
            d->IdleTicks = 0;
            if (s->Kind == SLOT_INT) Notification(d, s->Buffer, length);
            else if (d->Fn.Protocol == CDC_PROTO_ECM) {
                if (length >= CDC_ETH_MIN && length <= CDC_ETH_MAX) d->Cb.Receive(d->Cb.Context, s->Buffer, length);
                else if (length) d->Malformed++;
            } else if (d->Fn.Protocol == CDC_PROTO_NCM) {
                if (CdcNcmRxParse(s->Buffer, length, Emit, d) < 0) d->Malformed++;
            } else if (length) {
                if (CdcRndisRxParse(s->Buffer, length, Emit, d) < 0) d->Malformed++;
            }
        }
        KeAcquireSpinLock(&d->Lock, &old);
        s->Busy = FALSE;
        d->Outstanding--;
        if (!NT_SUCCESS(status)) Fault(d, status, usb);
        else d->Recoveries = 0;
        CheckDrained(d);
        KeReleaseSpinLock(&d->Lock, old);
    } else {
        void *packets[CDC_TX_BATCH];
        ULONG count = 0;
        KeAcquireSpinLock(&d->Lock, &old);
        s->Busy = FALSE;
        d->Outstanding--;
        if (NT_SUCCESS(status) && d->TxStage == TX_DATA && d->TxZlp && d->Open) {
            d->TxStage = TX_ZLP_READY;   /* the ZLP before any later payload */
        } else {
            count = d->TxCount;
            RtlCopyMemory(packets, d->TxPackets, count * sizeof(void *));
            d->TxCount = 0;
            d->TxStage = TX_IDLE;
            if (!NT_SUCCESS(status)) Fault(d, status, usb);
            else d->Recoveries = 0;
        }
        CheckDrained(d);
        KeReleaseSpinLock(&d->Lock, old);
        if (count) d->Cb.TxDone(d->Cb.Context, packets, count, NT_SUCCESS(status));
    }
    Pump(d);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Close the gate and drain every data IRP. PASSIVE_LEVEL. */
static void Quiesce(CDC_DEV *d)
{
    KIRQL old;
    BOOLEAN idle;
    ULONG waits = 0;
    void *packets[CDC_TX_BATCH];
    ULONG count = 0;
    KeAcquireSpinLock(&d->Lock, &old);
    d->Open = FALSE;
    KeReleaseSpinLock(&d->Lock, old);
    /* No submitter is admitted after the gate closes; wait for one already
     * inside IoCallDriver to return before aborting what is outstanding. */
    for (;;) {
        KeAcquireSpinLock(&d->Lock, &old);
        idle = !d->PumpActive;
        KeReleaseSpinLock(&d->Lock, old);
        if (idle) break;
        Sleep(1);
    }
    for (;;) {
        KeAcquireSpinLock(&d->Lock, &old);
        idle = !d->Outstanding;
        if (!idle) KeClearEvent(&d->Drained);
        KeReleaseSpinLock(&d->Lock, old);
        if (idle) break;
        if (!(waits++ % 4)) {
            (void)PipeRequest(d, URB_FUNCTION_ABORT_PIPE, d->In);
            (void)PipeRequest(d, URB_FUNCTION_ABORT_PIPE, d->Out);
            (void)PipeRequest(d, URB_FUNCTION_ABORT_PIPE, d->Int);
            if (waits > 1) CdcTrace("quiesce: %u still outstanding", d->Outstanding);
        }
        (void)Wait(&d->Drained, 500);
    }
    /* A payload whose ZLP was never sent: its frames left the host. */
    KeAcquireSpinLock(&d->Lock, &old);
    if (d->TxStage == TX_ZLP_READY) {
        count = d->TxCount;
        RtlCopyMemory(packets, d->TxPackets, count * sizeof(void *));
        d->TxCount = 0; d->TxStage = TX_IDLE;
    }
    KeReleaseSpinLock(&d->Lock, old);
    if (count) d->Cb.TxDone(d->Cb.Context, packets, count, 1);
}

static void Reopen(CDC_DEV *d)
{
    KIRQL old;
    KeAcquireSpinLock(&d->Lock, &old);
    if (!d->Gone && !d->Stop && !d->Suspended) d->Open = TRUE;
    KeReleaseSpinLock(&d->Lock, old);
    Pump(d);
}

/* ------------------------------------------------------------------ */
/* Protocol control, worker or Start/Stop context. */

#if defined(CDC_WITH_RNDIS)
static NTSTATUS RndisSend(CDC_DEV *d, ULONG length)
{
    return ClassRequest(d, FALSE, 0x00, 0, length, NULL);  /* SEND_ENCAPSULATED_COMMAND */
}

static void RndisUnsolicited(CDC_DEV *d, RNDIS_REPLY *r)
{
    if (r->Type == RNDIS_MSG_INDICATE) {
        if (r->Status == RNDIS_STATUS_MEDIA_CONNECT || r->Status == RNDIS_STATUS_MEDIA_DISCONNECT) {
            d->Connected = r->Status == RNDIS_STATUS_MEDIA_CONNECT;
            d->Cb.Link(d->Cb.Context, d->Connected, d->Speed);
        }
    } else if (r->Type == RNDIS_MSG_KEEPALIVE) {
        (void)RndisSend(d, CdcRndisKeepaliveReply(d->Ctrl, r->RequestId));
    }
}

/* Send one command already built in d->Ctrl and wait for the completion
 * of the same type and request id. Status indications and device
 * keepalives arriving in between are handled, never taken for it. */
static NTSTATUS RndisCommand(CDC_DEV *d, ULONG length, ULONG expect, ULONG id, RNDIS_REPLY *r)
{
    NTSTATUS status = RndisSend(d, length);
    ULONG tries, got;
    if (!NT_SUCCESS(status)) return status;
    for (tries = 0; tries < 100; tries++) {
        status = ClassRequest(d, TRUE, 0x01, 0, RNDIS_CONTROL_MAX, &got); /* GET_ENCAPSULATED_RESPONSE */
        if (!NT_SUCCESS(status) || CdcRndisParseReply(d->Ctrl, got, r) != CDC_OK) {
            if (DeviceGone(status, 0)) return status;
            Sleep(10);
            continue;
        }
        if (r->Type == expect && (expect == (RNDIS_MSG_RESET | RNDIS_COMPLETION) || r->RequestId == id))
            return r->Status == RNDIS_STATUS_SUCCESS ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        RndisUnsolicited(d, r);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS RndisOid(CDC_DEV *d, ULONG oid, const void *data, ULONG length, int set,
                         PVOID out, ULONG outLength, PULONG got)
{
    RNDIS_REPLY r;
    ULONG id = ++d->RndisId, n;
    NTSTATUS status;
    UCHAR zero[32];
    RtlZeroMemory(&r, sizeof(r));
    if (!set && !length && outLength <= sizeof(zero)) {
        /* A query carries a zeroed buffer of the expected reply size, as
         * Linux's rndis_host does: QEMU's usb-net stalls a query whose
         * buffer offset does not fall inside the message. */
        RtlZeroMemory(zero, sizeof(zero));
        data = zero; length = outLength;
    }
    n = CdcRndisQuery(d->Ctrl, CTRL_MAX, id, oid, data, length, set);
    if (!n) return STATUS_INVALID_PARAMETER;
    status = RndisCommand(d, n, (set ? RNDIS_MSG_SET : RNDIS_MSG_QUERY) | RNDIS_COMPLETION, id, &r);
    CdcTrace("rndis oid %x: %x status %x length %u", oid, status, r.Status, r.InfoLength);
    if (NT_SUCCESS(status) && !set) {
        if (r.InfoLength > outLength) return STATUS_BUFFER_OVERFLOW;
        if (r.InfoLength) RtlCopyMemory(out, r.Info, r.InfoLength);
        *got = r.InfoLength;
    }
    return status;
}

static NTSTATUS RndisInit(CDC_DEV *d, CDC_START *start)
{
    RNDIS_REPLY r;
    ULONG id = ++d->RndisId, got = 0, value;
    UCHAR mac[6];
    NTSTATUS status = RndisCommand(d, CdcRndisInit(d->Ctrl, id, RX_BLOCK),
                                   RNDIS_MSG_INIT | RNDIS_COMPLETION, id, &r);
    if (!NT_SUCCESS(status)) return status;
    if (r.Medium != 0) return STATUS_NOT_SUPPORTED;            /* 802.3 only */
    d->RndisMaxPackets = r.MaxPackets ? r.MaxPackets : 1;
    if (d->RndisMaxPackets > CDC_TX_BATCH) d->RndisMaxPackets = CDC_TX_BATCH;
    d->RndisMaxTransfer = r.MaxTransfer;
    if (d->RndisMaxTransfer > RX_BLOCK) d->RndisMaxTransfer = RX_BLOCK;
    if (d->RndisMaxTransfer < RNDIS_PACKET_HEADER + CDC_ETH_MAX) return STATUS_NOT_SUPPORTED;
    d->RndisAlign = r.AlignShift <= 7 ? r.AlignShift : 0;
    CdcTrace("rndis init: packets %u transfer %u align %u", r.MaxPackets, r.MaxTransfer, r.AlignShift);
    if (!start) return STATUS_SUCCESS;
    /* Wireless RNDIS is out of scope: refuse a 802.11 physical medium. */
    if (NT_SUCCESS(RndisOid(d, 0x00010202, NULL, 0, 0, &value, sizeof(value), &got)) &&
        got == 4 && value == 1) return STATUS_NOT_SUPPORTED;
    status = RndisOid(d, 0x01010101, NULL, 0, 0, mac, 6, &got);  /* PERMANENT_ADDRESS */
    if (NT_SUCCESS(status) && got == 6) RtlCopyMemory(start->PermanentMac, mac, 6);
    status = RndisOid(d, 0x01010102, NULL, 0, 0, mac, 6, &got);  /* CURRENT_ADDRESS */
    if (!NT_SUCCESS(status) || got != 6) {
        RtlCopyMemory(mac, start->PermanentMac, 6);
        status = STATUS_SUCCESS;
    }
    if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) || (mac[0] & 1)) return STATUS_DEVICE_DATA_ERROR;
    RtlCopyMemory(start->Mac, mac, 6);
    if (!(start->PermanentMac[0] | start->PermanentMac[1] | start->PermanentMac[2] |
          start->PermanentMac[3] | start->PermanentMac[4] | start->PermanentMac[5]))
        RtlCopyMemory(start->PermanentMac, mac, 6);
    if (NT_SUCCESS(RndisOid(d, 0x00010114, NULL, 0, 0, &value, sizeof(value), &got)) && got == 4)
        d->Connected = value == 0;                                 /* MEDIA_CONNECT_STATUS */
    if (NT_SUCCESS(RndisOid(d, 0x00010107, NULL, 0, 0, &value, sizeof(value), &got)) && got == 4 && value)
        d->Speed = value;                                          /* LINK_SPEED */
    return STATUS_SUCCESS;
}

static void RndisFetch(CDC_DEV *d)
{
    RNDIS_REPLY r;
    ULONG i, got;
    for (i = 0; i < 8; i++) {
        if (!NT_SUCCESS(ClassRequest(d, TRUE, 0x01, 0, RNDIS_CONTROL_MAX, &got)) ||
            CdcRndisParseReply(d->Ctrl, got, &r) != CDC_OK) return;
        RndisUnsolicited(d, &r);
    }
}
#endif

/* ECM/NCM SET_ETHERNET_PACKET_FILTER bits from NDIS packet filter bits. */
static USHORT CdcFilter(ULONG ndis)
{
    USHORT v = 0;
    if (ndis & 0x20) v |= 0x01;                 /* PROMISCUOUS */
    if (ndis & 0x04) v |= 0x02;                 /* ALL_MULTICAST */
    if (ndis & 0x01) v |= 0x04;                 /* DIRECTED */
    if (ndis & 0x08) v |= 0x08;                 /* BROADCAST */
    if (ndis & 0x02) v |= 0x10;                 /* MULTICAST */
    return v;
}

static NTSTATUS ApplyFilter(CDC_DEV *d)
{
    ULONG filter = d->Filter;
    NTSTATUS status = STATUS_SUCCESS;
    if (!d->FilterValid) return STATUS_SUCCESS;
#if defined(CDC_WITH_RNDIS)
    if (d->Fn.Protocol == CDC_PROTO_RNDIS) {
        if (d->McCount) {
            status = RndisOid(d, 0x01010103, d->Mc, d->McCount * 6, 1, NULL, 0, NULL);
            if (!NT_SUCCESS(status)) filter |= 0x04;  /* all multicast instead */
        }
        return RndisOid(d, 0x0001010e, &filter, 4, 1, NULL, 0, NULL);
    }
#endif
    if (d->Fn.Protocol == CDC_PROTO_NCM && !(d->Fn.NcmCaps & 0x01)) return STATUS_SUCCESS;
    /* Perfect multicast filters when the device has enough of them, else
     * all-multicast (section 4.6). The software filter in the upper edge
     * keeps a device that ignores these requests correct. */
    if ((filter & 0x02) && d->McCount) {
        ULONG slots = d->Fn.McFilters & 0x7fff;
        if (slots >= d->McCount && !(d->Fn.McFilters & 0x8000)) {
            RtlCopyMemory(d->Ctrl, d->Mc, d->McCount * 6);
            if (!NT_SUCCESS(ClassRequest(d, FALSE, 0x40, (USHORT)d->McCount, d->McCount * 6, NULL)))
                filter |= 0x04;
        } else {
            filter |= 0x04;
        }
    }
    status = ClassRequest(d, FALSE, 0x43, CdcFilter(filter), 0, NULL);
    /* A device without filter support stalls the request: not fatal. */
    if (!NT_SUCCESS(status) && !DeviceGone(status, 0)) status = STATUS_SUCCESS;
    return status;
}

/* NCM: parameters before the data interface carries traffic. */
static NTSTATUS NcmSetup(CDC_DEV *d)
{
    ULONG got;
    NTSTATUS status = ClassRequest(d, TRUE, 0x80, 0, 28, &got);      /* GET_NTB_PARAMETERS */
    if (!NT_SUCCESS(status)) return status;
    if (CdcNcmParams(d->Ctrl, got, RX_BLOCK, RX_BLOCK, &d->Ncm) != CDC_OK) return STATUS_NOT_SUPPORTED;
    if (d->Ncm.Formats & 2) {                                        /* NTB32 offered: pick NTB16 */
        status = ClassRequest(d, FALSE, 0x84, 0, 0, NULL);            /* SET_NTB_FORMAT */
        if (!NT_SUCCESS(status)) return status;
    }
    RtlZeroMemory(d->Ctrl, 8);
    d->Ctrl[0] = (UCHAR)d->Ncm.InMaxSize; d->Ctrl[1] = (UCHAR)(d->Ncm.InMaxSize >> 8);
    d->Ctrl[2] = (UCHAR)(d->Ncm.InMaxSize >> 16);
    status = ClassRequest(d, FALSE, 0x86, 0, (d->Fn.NcmCaps & 0x20) ? 8 : 4, NULL); /* SET_NTB_INPUT_SIZE */
    if (!NT_SUCCESS(status)) {
        /* Keep the device's default, which is its dwNtbInMaxSize. */
        CdcTrace("ncm: SET_NTB_INPUT_SIZE failed %x", status);
        if (DeviceGone(status, 0)) return status;
    }
    if (d->Fn.NcmCaps & 0x10) (void)ClassRequest(d, FALSE, 0x8a, 0, 0, NULL);  /* CRC off */
    if (d->Fn.NcmCaps & 0x08) {                                      /* max datagram size */
        if (NT_SUCCESS(ClassRequest(d, TRUE, 0x87, 0, 2, &got)) && got == 2 &&
            (d->Ctrl[0] | (d->Ctrl[1] << 8)) >= CDC_ETH_MAX) {
            d->Ctrl[0] = (UCHAR)CDC_ETH_MAX; d->Ctrl[1] = (UCHAR)(CDC_ETH_MAX >> 8);
            (void)ClassRequest(d, FALSE, 0x88, 0, 2, NULL);
        }
    }
    CdcTrace("ncm: in %u out %u div %u rem %u align %u dg %u", d->Ncm.InMaxSize, d->Ncm.OutMaxSize,
             d->Ncm.OutDivisor, d->Ncm.OutRemainder, d->Ncm.OutAlignment, d->Ncm.OutMaxDatagrams);
    return STATUS_SUCCESS;
}

static ULONG InterfaceSize(ULONG pipes)
{
    return sizeof(USBD_INTERFACE_INFORMATION) + (pipes > 1 ? (pipes - 1) * sizeof(USBD_PIPE_INFORMATION) : 0);
}

static NTSTATUS TakePipes(CDC_DEV *d, PUSBD_INTERFACE_INFORMATION i)
{
    ULONG k;
    for (k = 0; k < i->NumberOfPipes; k++) {
        PUSBD_PIPE_INFORMATION p = &i->Pipes[k];
        if (!p->PipeHandle || !p->MaximumPacketSize) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (p->EndpointAddress == d->Fn.In && p->PipeType == UsbdPipeTypeBulk) {
            d->In = p->PipeHandle; d->InMax = p->MaximumPacketSize;
        } else if (p->EndpointAddress == d->Fn.Out && p->PipeType == UsbdPipeTypeBulk) {
            d->Out = p->PipeHandle; d->OutMax = p->MaximumPacketSize;
        } else if (p->EndpointAddress == d->Fn.Interrupt && p->PipeType == UsbdPipeTypeInterrupt) {
            d->Int = p->PipeHandle;
        } else {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    return STATUS_SUCCESS;
}

/* ECM/NCM: the data interface's alternate with the bulk pair. */
static NTSTATUS SelectDataAlt(CDC_DEV *d, UCHAR alt)
{
    ULONG ds = InterfaceSize(alt ? 2 : 0), size = FIELD_OFFSET(struct _URB_SELECT_INTERFACE, Interface) + ds;
    struct _URB_SELECT_INTERFACE *urb = ExAllocatePoolWithTag(NonPagedPool, size, TAG);
    NTSTATUS status;
    ULONG k;
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, size);
    urb->Hdr.Length = (USHORT)size;
    urb->Hdr.Function = URB_FUNCTION_SELECT_INTERFACE;
    urb->ConfigurationHandle = d->ConfigHandle;
    urb->Interface.Length = (USHORT)ds;
    urb->Interface.InterfaceNumber = d->Fn.Data;
    urb->Interface.AlternateSetting = alt;
    for (k = 0; k < 2; k++) urb->Interface.Pipes[k].MaximumTransferSize = RX_BLOCK;
    status = Sync(d, (PURB)urb, CTRL_TIMEOUT);
    if (NT_SUCCESS(status)) {
        d->In = d->Out = NULL;
        status = TakePipes(d, &urb->Interface);
        if (NT_SUCCESS(status) && alt && (!d->In || !d->Out)) status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    ExFreePool(urb);
    return status;
}

static NTSTATUS SelectConfiguration(CDC_DEV *d, PUCHAR config)
{
    BOOLEAN rndis = d->Fn.Protocol == CDC_PROTO_RNDIS;
    ULONG cp = d->Fn.Interrupt ? 1 : 0, dp = rndis ? 2 : 0;
    ULONG cs = InterfaceSize(cp), ds = InterfaceSize(dp);
    ULONG size = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface) + cs + ds;
    struct _URB_SELECT_CONFIGURATION *urb = ExAllocatePoolWithTag(NonPagedPool, size, TAG);
    PUSBD_INTERFACE_INFORMATION first, second, control, data;
    NTSTATUS status;
    ULONG k;
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, size);
    urb->Hdr.Length = (USHORT)size;
    urb->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb->ConfigurationDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)config;
    /* Only this function's two interfaces, in ascending order. */
    first = &urb->Interface;
    if (d->Fn.Control < d->Fn.Data) {
        control = first; data = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)first + cs);
        second = data;
    } else {
        data = first; control = (PUSBD_INTERFACE_INFORMATION)((PUCHAR)first + ds);
        second = control;
    }
    UNREFERENCED_PARAMETER(second);
    control->Length = (USHORT)cs; control->InterfaceNumber = d->Fn.Control;
    data->Length = (USHORT)ds; data->InterfaceNumber = d->Fn.Data;
    for (k = 0; k < cp; k++) control->Pipes[k].MaximumTransferSize = 64;
    for (k = 0; k < dp; k++) data->Pipes[k].MaximumTransferSize = RX_BLOCK;
    status = Sync(d, (PURB)urb, CTRL_TIMEOUT);
    if (NT_SUCCESS(status) && !urb->ConfigurationHandle) status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (NT_SUCCESS(status)) {
        d->ConfigHandle = urb->ConfigurationHandle;
        status = TakePipes(d, control);
        if (NT_SUCCESS(status)) status = TakePipes(d, data);
        if (NT_SUCCESS(status) && d->Fn.Interrupt && !d->Int) status = STATUS_DEVICE_CONFIGURATION_ERROR;
        if (NT_SUCCESS(status) && rndis && (!d->In || !d->Out)) status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    ExFreePool(urb);
    return status;
}

/* The protocol's own bring-up after the configuration is selected; also
 * the resume path. ECM/NCM end with the data alternate selected. */
static NTSTATUS Bringup(CDC_DEV *d, CDC_START *start)
{
    NTSTATUS status = STATUS_SUCCESS;
#if defined(CDC_WITH_RNDIS)
    if (d->Fn.Protocol == CDC_PROTO_RNDIS) return RndisInit(d, start);
#else
    UNREFERENCED_PARAMETER(start);
#endif
    if (!start) {
        /* Resume or reset: alternate 0 first, so the device restarts. */
        status = SelectDataAlt(d, 0);
        if (!NT_SUCCESS(status)) return status;
    }
    if (d->Fn.Protocol == CDC_PROTO_NCM) {
        status = NcmSetup(d);
        if (!NT_SUCCESS(status)) return status;
    }
    return SelectDataAlt(d, d->Fn.DataAlt);
}

/* ------------------------------------------------------------------ */
/* The worker. */

/* Worker context. The device is gone: drain every data IRP first (abort the
 * pipes, wait for each completion), then report. A gone device's transfers
 * are not always completed by the host controller driver on its own (seen
 * with xHCI98 on Windows 98 SE when the removal was noticed by a control
 * request rather than a data transfer); a send left in flight would hold
 * NDIS's unbind, which waits for every send to complete, so the miniport is
 * never halted and the device's removal never finishes. */
static void ReportGone(CDC_DEV *d)
{
    KIRQL old;
    BOOLEAN report;
    KeAcquireSpinLock(&d->Lock, &old);
    report = d->Gone && !d->GoneReported;
    if (report) d->GoneReported = TRUE;
    KeReleaseSpinLock(&d->Lock, old);
    if (report) {
        CdcTrace("device gone: %u transfers outstanding", d->Outstanding);
        Quiesce(d);
        CdcTrace("device gone: drained");
        d->Connected = FALSE;
        d->Cb.Link(d->Cb.Context, -1, d->Speed);
        CdcTrace("device gone: reported");
    }
}

static void Recover(CDC_DEV *d)
{
    NTSTATUS status = STATUS_SUCCESS;
    Quiesce(d);
    if (d->Gone || d->Suspended) { ReportGone(d); return; }
    if (++d->Recoveries > 30) {
        CdcTrace("recover: giving up after %u attempts", d->Recoveries);
        d->Gone = TRUE;
        ReportGone(d);
        return;
    }
    Sleep(d->Recoveries > 3 ? 200 : 20);
    status = PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->In);
    if (NT_SUCCESS(status)) status = PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->Out);
    if (NT_SUCCESS(status)) status = PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->Int);
    d->Notify.Held = 0;                      /* no partial notification survives a pipe reset */
    CdcTrace("recover %u: reset pipes %x", d->Recoveries, status);
    if (!NT_SUCCESS(status)) {
        if (DeviceGone(status, 0) || d->Recoveries > 3) {
            d->Gone = TRUE;
            ReportGone(d);
            return;
        }
    }
    Reopen(d);
}

static long Power(CDC_DEV *d, BOOLEAN on)
{
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL old;
    if (!on) {
        Quiesce(d);
        d->Notify.Held = 0;                  /* the device may lose power: no fragment survives */
        KeAcquireSpinLock(&d->Lock, &old);
        d->Suspended = TRUE;
        KeReleaseSpinLock(&d->Lock, old);
        return STATUS_SUCCESS;
    }
    KeAcquireSpinLock(&d->Lock, &old);
    d->Suspended = FALSE;
    KeReleaseSpinLock(&d->Lock, old);
    if (d->Gone) return STATUS_SUCCESS;
    /* The device may have lost power: run the protocol's bring-up again,
     * then restore the cached filter and multicast list. */
    status = Bringup(d, NULL);
    if (NT_SUCCESS(status)) status = ApplyFilter(d);
    CdcTrace("resume: %x", status);
    if (!NT_SUCCESS(status)) {
        if (DeviceGone(status, 0)) { CdcTrace("resume: device gone"); d->Gone = TRUE; ReportGone(d); }
        else Recover(d);
        return STATUS_SUCCESS;   /* the adapter still exists; traffic recovers or not */
    }
    d->Recoveries = 0;
    Reopen(d);
    return STATUS_SUCCESS;
}

static long Reset(CDC_DEV *d)
{
    NTSTATUS status;
    Quiesce(d);
    if (d->Gone) { ReportGone(d); return STATUS_SUCCESS; }
    if (d->Suspended) return STATUS_SUCCESS;
    (void)PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->In);
    (void)PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->Out);
    (void)PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->Int);
    d->Notify.Held = 0;
#if defined(CDC_WITH_RNDIS)
    if (d->Fn.Protocol == CDC_PROTO_RNDIS) {
        RNDIS_REPLY r;
        (void)RndisCommand(d, CdcRndisSimple(d->Ctrl, RNDIS_MSG_RESET, 0),
                           RNDIS_MSG_RESET | RNDIS_COMPLETION, 0, &r);
    }
#endif
    status = ApplyFilter(d);
    CdcTrace("reset: %x", status);
    Reopen(d);
    return status;
}

static void Worker(PVOID context)
{
    CDC_DEV *d = context;
    KIRQL old;
    for (;;) {
        ULONG work;
        int req;
        BOOLEAN reset, stop;
        NTSTATUS status = Wait(&d->Wake, 1000);
        KeAcquireSpinLock(&d->Lock, &old);
        work = d->Work; d->Work = 0;
        stop = d->Stop;
        req = (work & WORK_REQ) ? d->Req : 0;
        reset = (work & WORK_RESET) != 0;
        KeReleaseSpinLock(&d->Lock, old);
        if (stop) break;
        if (work & WORK_RECOVER) Recover(d);
        if (reset) {
            long result = Reset(d);
            KeAcquireSpinLock(&d->Lock, &old);
            d->ResetPending = FALSE;
            KeReleaseSpinLock(&d->Lock, old);
            d->Cb.TaskDone(d->Cb.Context, CDC_REQ_RESET, result);
        }
        if (req) {
            long result = STATUS_SUCCESS;
            if (req == CDC_REQ_FILTER) {
                d->Filter = d->ReqValue; d->FilterValid = TRUE;
                if (!d->Gone && !d->Suspended) result = ApplyFilter(d);
            } else if (req == CDC_REQ_MULTICAST) {
                d->McCount = d->ReqLength / 6;
                RtlCopyMemory(d->Mc, d->ReqData, d->ReqLength);
                if (!d->Gone && !d->Suspended) result = ApplyFilter(d);
            } else if (req == CDC_REQ_POWER) {
                result = Power(d, (BOOLEAN)(d->ReqValue == 1));
            }
            if (DeviceGone(result, 0)) {
                CdcTrace("request %d: device gone (%x)", req, result);
                d->Gone = TRUE; ReportGone(d); result = STATUS_SUCCESS;
            }
            KeAcquireSpinLock(&d->Lock, &old);
            d->Req = 0;
            KeReleaseSpinLock(&d->Lock, old);
            d->Cb.TaskDone(d->Cb.Context, req, result);
        }
#if defined(CDC_WITH_RNDIS)
        if (d->Fn.Protocol == CDC_PROTO_RNDIS && !d->Gone && !d->Suspended) {
            if (work & WORK_FETCH) RndisFetch(d);
            /* Host-initiated keepalive after 5 s without device traffic. */
            if (status == STATUS_TIMEOUT && ++d->IdleTicks >= 5) {
                RNDIS_REPLY r;
                ULONG id = ++d->RndisId;
                d->IdleTicks = 0;
                if (NT_SUCCESS(RndisCommand(d, CdcRndisSimple(d->Ctrl, RNDIS_MSG_KEEPALIVE, id),
                                            RNDIS_MSG_KEEPALIVE | RNDIS_COMPLETION, id, &r)))
                    d->KeepaliveFailures = 0;
                else if (++d->KeepaliveFailures == 3)
                    CdcTrace("rndis: three keepalives unanswered");
            }
        }
#else
        UNREFERENCED_PARAMETER(status);
#endif
        if (d->Gone) ReportGone(d);
        Pump(d);   /* retries an IRP allocation that failed */
    }
    KeSetEvent(&d->Exited, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* The interface. */

static void Free(CDC_DEV *d)
{
    ULONG i;
    for (i = 0; i < RX_MAX; i++) if (d->Rx[i].Buffer) ExFreePool(d->Rx[i].Buffer);
    if (d->IntSlot.Buffer) ExFreePool(d->IntSlot.Buffer);
    if (d->Tx.Buffer) ExFreePool(d->Tx.Buffer);
    if (d->Ctrl) ExFreePool(d->Ctrl);
    ExFreePool(d);
}

static void Fail(CDC_START *start, ULONG stage, NTSTATUS status)
{
    start->Failure = stage; start->Status = status;
    CdcTrace("start failed at stage %u, status %x", stage, status);
}

long CdcUsbStart(void *lowerDevice, const CDC_CALLBACKS *callbacks, CDC_START *start, void **device)
{
    CDC_DEV *d;
    PUCHAR config = NULL;
    NTSTATUS status;
    ULONG got, i, configs, best = 0xff;
    USHORT lang = 0x0409;
    HANDLE thread;
    USB_CONFIGURATION_DESCRIPTOR header;

    *device = NULL;
    start->Failure = CDC_FAIL_NONE;
#if !defined(CDC_WITH_RNDIS)
    start->AllowMask &= ~CDC_ALLOW(CDC_PROTO_RNDIS);
#endif
    d = ExAllocatePoolWithTag(NonPagedPool, sizeof(*d), TAG);
    if (!d) { Fail(start, CDC_FAIL_MEMORY, STATUS_INSUFFICIENT_RESOURCES); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(d, sizeof(*d));
    d->Lower = lowerDevice;
    d->Cb = *callbacks;
    KeInitializeSpinLock(&d->Lock);
    KeInitializeEvent(&d->Drained, NotificationEvent, FALSE);
    KeInitializeEvent(&d->Wake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&d->Exited, NotificationEvent, FALSE);
    d->Ctrl = ExAllocatePoolWithTag(NonPagedPool, CTRL_MAX, TAG);
    config = ExAllocatePoolWithTag(NonPagedPool, CONFIG_MAX, TAG);
    if (!d->Ctrl || !config) { Fail(start, CDC_FAIL_MEMORY, STATUS_INSUFFICIENT_RESOURCES); status = STATUS_INSUFFICIENT_RESOURCES; goto fail; }

    /* Device descriptor, then every configuration (section 5.2). */
    status = GetDescriptor(d, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, d->Ctrl, 18, &got);
    if (!NT_SUCCESS(status) || got != 18 || d->Ctrl[0] != 18 || d->Ctrl[1] != 1) {
        Fail(start, CDC_FAIL_DEVICE, NT_SUCCESS(status) ? STATUS_DEVICE_DATA_ERROR : status);
        if (NT_SUCCESS(status)) status = STATUS_DEVICE_DATA_ERROR;
        goto fail;
    }
    configs = d->Ctrl[17];
    CdcTrace("device %x:%x, %u configurations", d->Ctrl[8] | (d->Ctrl[9] << 8),
             d->Ctrl[10] | (d->Ctrl[11] << 8), configs);
    if (configs > 8) configs = 8;
    for (i = 0; i < configs; i++) {
        CDC_FUNCTION f;
        long result;
        status = GetDescriptor(d, USB_CONFIGURATION_DESCRIPTOR_TYPE, (UCHAR)i, 0, &header, sizeof(header), &got);
        if (!NT_SUCCESS(status) || got != sizeof(header) || header.wTotalLength < sizeof(header) ||
            header.wTotalLength > CONFIG_MAX) continue;
        status = GetDescriptor(d, USB_CONFIGURATION_DESCRIPTOR_TYPE, (UCHAR)i, 0, config, header.wTotalLength, &got);
        if (!NT_SUCCESS(status) || got != header.wTotalLength) continue;
        result = CdcProbeConfig(config, got, start->AllowMask, &f);
        CdcTrace("configuration %u: probe %d protocol %u", i, result, result == CDC_OK ? f.Protocol : 0);
        if (result == CDC_OK && (best == 0xff || f.Protocol > d->Fn.Protocol)) { d->Fn = f; best = i; }
    }
    if (best == 0xff) { Fail(start, CDC_FAIL_PROBE, STATUS_NOT_SUPPORTED); status = STATUS_NOT_SUPPORTED; goto fail; }
    d->ConfigIndex = (UCHAR)best;
    status = GetDescriptor(d, USB_CONFIGURATION_DESCRIPTOR_TYPE, d->ConfigIndex, 0, &header, sizeof(header), &got);
    if (NT_SUCCESS(status))
        status = GetDescriptor(d, USB_CONFIGURATION_DESCRIPTOR_TYPE, d->ConfigIndex, 0, config, header.wTotalLength, &got);
    if (NT_SUCCESS(status)) status = SelectConfiguration(d, config);
    CdcTrace("select config %u (value %u) protocol %u control %u data %u alt %u: %x",
             d->ConfigIndex, d->Fn.ConfigValue, d->Fn.Protocol, d->Fn.Control, d->Fn.Data, d->Fn.DataAlt, status);
    if (!NT_SUCCESS(status)) { Fail(start, CDC_FAIL_SELECT, status); goto fail; }
    d->Speed = d->Fn.InMax >= 512 ? 1000000 : 100000;
    d->Connected = TRUE;

    if (d->Fn.Protocol != CDC_PROTO_RNDIS) {
        /* iMACAddress in the device's first language (section 4.8). */
        if (NT_SUCCESS(GetDescriptor(d, USB_STRING_DESCRIPTOR_TYPE, 0, 0, d->Ctrl, 4, &got)) &&
            got >= 4 && d->Ctrl[0] >= 4 && d->Ctrl[1] == 3)
            lang = (USHORT)(d->Ctrl[2] | (d->Ctrl[3] << 8));
        status = GetDescriptor(d, USB_STRING_DESCRIPTOR_TYPE, d->Fn.MacIndex, lang, d->Ctrl, 26, &got);
        if (NT_SUCCESS(status) && CdcParseMacString(d->Ctrl, got, start->Mac) != CDC_OK)
            status = STATUS_DEVICE_DATA_ERROR;
        if (!NT_SUCCESS(status)) { Fail(start, CDC_FAIL_MAC, status); goto fail; }
        RtlCopyMemory(start->PermanentMac, start->Mac, 6);
    }
    status = Bringup(d, start);
    CdcTrace("bring-up %x: pipes in %u out %u bytes per packet (descriptor %u/%u)", status,
             d->InMax, d->OutMax, d->Fn.InMax, d->Fn.OutMax);
    if (!NT_SUCCESS(status)) { Fail(start, CDC_FAIL_PROTOCOL, status); goto fail; }
    CdcTrace("mac %x:%x:%x:%x:%x:%x", start->Mac[0], start->Mac[1], start->Mac[2],
             start->Mac[3], start->Mac[4], start->Mac[5]);

    /* Buffers: every data buffer is nonpaged, owned by its slot. */
    {
        ULONG rxSize = RX_ECM, rxCount = 4;
        if (d->Fn.Protocol == CDC_PROTO_NCM) { rxSize = d->Ncm.InMaxSize; d->TxCap = d->Ncm.OutMaxSize; }
        else if (d->Fn.Protocol == CDC_PROTO_RNDIS) { rxSize = RX_BLOCK; d->TxCap = d->RndisMaxTransfer; }
        else d->TxCap = RX_ECM;
        rxSize = AlignUp(rxSize, d->InMax);
        if (rxSize > RX_BLOCK) rxSize = RX_BLOCK;
        for (i = 0; i < rxCount; i++) {
            d->Rx[i].Dev = d; d->Rx[i].Kind = SLOT_RX; d->Rx[i].Size = rxSize;
            d->Rx[i].Buffer = ExAllocatePoolWithTag(NonPagedPool, rxSize, TAG);
            if (!d->Rx[i].Buffer) break;
        }
        d->RxCount = i;
        d->IntSlot.Dev = d; d->IntSlot.Kind = SLOT_INT; d->IntSlot.Size = IntTransferSize(d->Fn.IntMax);
        d->IntSlot.Buffer = ExAllocatePoolWithTag(NonPagedPool, d->IntSlot.Size, TAG);
        d->Tx.Dev = d; d->Tx.Kind = SLOT_TX; d->Tx.Size = d->TxCap;
        d->Tx.Buffer = ExAllocatePoolWithTag(NonPagedPool, d->TxCap, TAG);
        if (!d->RxCount || !d->IntSlot.Buffer || !d->Tx.Buffer) {
            Fail(start, CDC_FAIL_MEMORY, STATUS_INSUFFICIENT_RESOURCES);
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto fail;
        }
    }
    status = PsCreateSystemThread(&thread, 0, NULL, NULL, NULL, Worker, d);
    if (!NT_SUCCESS(status)) { Fail(start, CDC_FAIL_THREAD, status); goto fail; }
    /* NT: also keep the thread object, to wait for the thread itself. */
    if (IoIsWdmVersionAvailable(1, 0x10) &&
        !NT_SUCCESS(ObReferenceObjectByHandle(thread, 0, NULL, KernelMode, &d->ThreadObject, NULL)))
        d->ThreadObject = NULL;
    ZwClose(thread);
    ExFreePool(config);

    start->Protocol = d->Fn.Protocol;
    start->HighSpeed = d->InMax >= 512;
    start->LinkSpeed = d->Speed;
    start->Connected = d->Connected;
    *device = d;
    Reopen(d);
    return STATUS_SUCCESS;

fail:
    if (config) ExFreePool(config);
    Free(d);
    return status;
}

void CdcUsbStop(void *device)
{
    CDC_DEV *d = device;
    KIRQL old;
    int req;
    if (!d) return;
    KeAcquireSpinLock(&d->Lock, &old);
    d->Stop = TRUE;
    KeSetEvent(&d->Wake, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&d->Lock, old);
    Wait(&d->Exited, 0);
    if (d->ThreadObject) {
        KeWaitForSingleObject(d->ThreadObject, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(d->ThreadObject);
    }
    CdcTrace("stop: worker exited");
    Quiesce(d);
#if defined(CDC_WITH_RNDIS)
    if (d->Fn.Protocol == CDC_PROTO_RNDIS && !d->Gone)
        (void)RndisSend(d, CdcRndisSimple(d->Ctrl, RNDIS_MSG_HALT, ++d->RndisId));
#endif
    /* A request the worker never reached still completes once. */
    KeAcquireSpinLock(&d->Lock, &old);
    req = d->Req; d->Req = 0;
    KeReleaseSpinLock(&d->Lock, old);
    if (req) d->Cb.TaskDone(d->Cb.Context, req, STATUS_SUCCESS);
    if (d->ResetPending) d->Cb.TaskDone(d->Cb.Context, CDC_REQ_RESET, STATUS_SUCCESS);
    CdcTrace("stopped; %u malformed transfers, %u truncated notifications", d->Malformed, d->Notify.Truncated);
    Free(d);
}

void CdcUsbKick(void *device)
{
    Pump(device);
}

int CdcUsbRequest(void *device, int request, cdc_u32 value, const cdc_u8 *data, cdc_u32 length)
{
    CDC_DEV *d = device;
    KIRQL old;
    int queued = 0;
    KeAcquireSpinLock(&d->Lock, &old);
    if (!d->Stop) {
        if (request == CDC_REQ_RESET) {
            if (!d->ResetPending) { d->ResetPending = TRUE; d->Work |= WORK_RESET; queued = 1; }
        } else if (!d->Req && length <= sizeof(d->ReqData)) {
            d->Req = request; d->ReqValue = value; d->ReqLength = length;
            if (length) RtlCopyMemory(d->ReqData, data, length);
            d->Work |= WORK_REQ; queued = 1;
        }
        if (queued) KeSetEvent(&d->Wake, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&d->Lock, old);
    return queued;
}

void CdcUsbSurpriseRemoved(void *device)
{
    CDC_DEV *d = device;
    KIRQL old;
    if (!d) return;
    KeAcquireSpinLock(&d->Lock, &old);
    d->Gone = TRUE;
    d->Open = FALSE;
    d->Work |= WORK_RECOVER;
    KeSetEvent(&d->Wake, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&d->Lock, old);
}

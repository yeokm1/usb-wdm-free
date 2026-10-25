/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * USB Ethernet core, WDM/USB lower edge (ethernet.md sections 3.3, 4.3,
 * 4.7 and 5). Written from the Windows 2000 DDK headers (wdm.h, usbdi.h,
 * usb100.h, usbioctl.h) and the documented URB functions; no DDK sample
 * was read. Derived from cdcnet's cdcusb.c (same author and licence),
 * with the CDC protocols replaced by the netchip.h chip table. Identical
 * copy in src/net/ax88179/ and src/net/rtl815x/.
 *
 * - NetUsbStart reads the device and every configuration descriptor, lets
 *   the chip's Probe choose, selects that configuration and runs the
 *   chip's Start (its register bring-up) through vendor control requests.
 * - The data path is asynchronous: N bulk-IN transfers, one interrupt-IN
 *   transfer and M bulk-OUT transfers (each a batch the chip frames) are
 *   outstanding. Bulk traffic runs only while the link is up. Every
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
 *   PASSIVE_LEVEL: every chip register access (link changes, filters,
 *   power, reset, error recovery and a 2 s link poll). It signals an
 *   event before PsTerminateSystemThread; Windows 98 faults a wait on a
 *   thread object (xHCI98 docs/usb-xhci-info/win98-wdm.md), so the thread
 *   object is waited on only where WDM 1.10 (Windows 2000) is available.
 */
#include <wdm.h>
#include <usbdi.h>
#include <stdarg.h>
#include "netusb.h"

#ifndef USBD_STATUS_DEVICE_GONE
#define USBD_STATUS_DEVICE_GONE ((USBD_STATUS)0xC0007000L) /* later usb.h; absent from the 2000 DDK */
#endif

#define TAG 'tNbU'
#define RX_MAX 8
#define TX_MAX 4
#define RX_LIMIT 32768UL
#define TX_LIMIT 16384UL
#define CONFIG_MAX 4096
#define CONFIGS_MAX 4
#define CTRL_MAX 512
#define CTRL_TIMEOUT 5000

#define WORK_RECOVER 0x01
#define WORK_LINK    0x02
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

typedef struct NET_DEV NET_DEV;

typedef struct SLOT {
    NET_DEV *Dev;
    struct _URB_BULK_OR_INTERRUPT_TRANSFER Urb;
    PUCHAR Buffer;
    ULONG Size;
    UCHAR Kind;
    BOOLEAN Busy;
    /* transmit slots only */
    ULONG Stage, Count;
    BOOLEAN Zlp;
    void *Packets[NET_TX_BATCH];
} SLOT;

struct NET_DEV {
    PDEVICE_OBJECT Lower;
    NET_CALLBACKS Cb;
    const NET_CHIP *Chip;
    NET_PROBE Probe;
    NET_PARAMS Params;
    NET_IO Io;
    union { ULONGLONG Align; UCHAR Bytes[NET_CHIP_STATE]; } ChipState;
    USBD_CONFIGURATION_HANDLE ConfigHandle;
    USBD_PIPE_HANDLE In, Out, Int;
    USHORT InMax, OutMax;
    UCHAR Mac[6];                 /* the address in use */

    KSPIN_LOCK Lock;              /* everything below up to the worker */
    BOOLEAN Open;                 /* the submission gate */
    BOOLEAN LinkUp;               /* bulk traffic allowed */
    BOOLEAN Gone, GoneReported, Suspended, PumpActive, PumpAgain;
    ULONG Outstanding;            /* data IRPs submitted and not completed */
    KEVENT Drained;               /* notification: !Open && !Outstanding */
    SLOT Rx[RX_MAX];
    ULONG RxCount, RxLength;
    SLOT IntSlot;
    SLOT Tx[TX_MAX];
    ULONG TxCount, TxBatch;
    ULONG Work;
    BOOLEAN Stop;
    int Req;                      /* the pending OID-driven request, 0 if none */
    ULONG ReqValue, ReqLength;
    UCHAR ReqData[NC_MULTICAST_MAX * 6];
    BOOLEAN ResetPending;

    /* Worker-only state. */
    KEVENT Wake;                  /* synchronization */
    KEVENT Exited;                /* notification, set by the worker last */
    PVOID ThreadObject;           /* referenced on NT only */
    BOOLEAN Worker;               /* the worker thread exists */
    KEVENT CtrlDone;
    PUCHAR Ctrl;                  /* nonpaged control scratch, CTRL_MAX */
    ULONG Recoveries, Ticks;
    ULONG Filter, McCount;
    BOOLEAN FilterValid;
    UCHAR Mc[NC_MULTICAST_MAX * 6];
    ULONG Speed;                  /* 100 bps units */
    ULONG Malformed, Dropped, FcsStripped;
    BOOLEAN FcsLogged;            /* the anomaly was reported (under Lock) */
};

/* ------------------------------------------------------------------ */
/* Trace: a small formatter (%x %u %d %s %c), no CRT. The trace flavour
 * writes to QEMU's debug console port 0xE9; it is test-only and never
 * released. Checked builds also use DbgPrint. */

static void Out(char c)
{
#if defined(NET_TRACE_E9)
    __asm { mov dx, 0xe9
            mov al, c
            out dx, al }
#else
    UNREFERENCED_PARAMETER(c);
#endif
}

void NetTrace(const char *format, ...)
{
#if defined(NET_TRACE_E9) || DBG
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
        const char *name = NetPackageChip.Name;
        while (*name) Out(*name++);
        Out(':'); Out(' ');
        for (i = 0; i < n; i++) Out(line[i]);
    }
#if DBG
    DbgPrint("%s: %s", NetPackageChip.Name, line);
#endif
#else
    UNREFERENCED_PARAMETER(format);
#endif
}

/* Trace only: the first few receive transfers that held a refused frame,
 * as raw little-endian words (the first 96 bytes and the last 8), so a
 * framing or FCS mismatch on a real chip can be read from the log. */
static void DropSample(const UCHAR *b, ULONG length)
{
#if defined(NET_TRACE_E9) || DBG
    static ULONG samples;
    ULONG i, k, w[6];
    const UCHAR *t;
    if (samples >= 4 || length < 8) return;
    samples++;
    for (k = 0; k < 96 && k < length; k += 24) {
        for (i = 0; i < 6; i++) {
            ULONG at = k + i * 4;
            w[i] = (at + 4 <= length) ? (ULONG)b[at] | ((ULONG)b[at + 1] << 8) |
                   ((ULONG)b[at + 2] << 16) | ((ULONG)b[at + 3] << 24) : 0;
        }
        NetTrace("rx drop %u bytes @%u: %x %x %x %x %x %x", length, k, w[0], w[1], w[2], w[3], w[4], w[5]);
    }
    t = b + length - 8;
    NetTrace("rx drop tail: %x %x",
             (ULONG)t[0] | ((ULONG)t[1] << 8) | ((ULONG)t[2] << 16) | ((ULONG)t[3] << 24),
             (ULONG)t[4] | ((ULONG)t[5] << 8) | ((ULONG)t[6] << 16) | ((ULONG)t[7] << 24));
#else
    UNREFERENCED_PARAMETER(b); UNREFERENCED_PARAMETER(length);
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

static BOOLEAN DeviceGone(NTSTATUS status, USBD_STATUS usb)
{
    return status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_NO_SUCH_DEVICE ||
           status == STATUS_DEVICE_REMOVED || usb == USBD_STATUS_DEVICE_GONE;
}

/* ------------------------------------------------------------------ */
/* Synchronous URBs, PASSIVE_LEVEL only: Start, Open, Stop and the worker,
 * which never run at the same time for one adapter. */

static NTSTATUS SyncDone(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    UNREFERENCED_PARAMETER(unused);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;  /* the caller frees the IRP */
}

static NTSTATUS Sync(NET_DEV *d, PURB urb, ULONG timeoutMs)
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
        Wait(&d->CtrlDone, 0);
        status = cancelled && irp->IoStatus.Status == STATUS_CANCELLED ?
                 STATUS_IO_TIMEOUT : irp->IoStatus.Status;
    } else {
        status = irp->IoStatus.Status;
    }
    IoFreeIrp(irp);
    if (NT_SUCCESS(status) && !USBD_SUCCESS(urb->UrbHeader.Status)) {
        status = DeviceGone(status, urb->UrbHeader.Status) ? STATUS_DEVICE_NOT_CONNECTED : STATUS_UNSUCCESSFUL;
    }
    return status;
}

static NTSTATUS GetDescriptor(NET_DEV *d, UCHAR type, UCHAR index, PVOID buffer, ULONG length,
                              PULONG actual)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
    NTSTATUS status;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    urb.DescriptorType = type; urb.Index = index;
    urb.TransferBuffer = buffer; urb.TransferBufferLength = length;
    status = Sync(d, (PURB)&urb, CTRL_TIMEOUT);
    *actual = NT_SUCCESS(status) ? urb.TransferBufferLength : 0;
    return status;
}

static NTSTATUS PipeRequest(NET_DEV *d, USHORT function, USBD_PIPE_HANDLE pipe)
{
    struct _URB_PIPE_REQUEST urb;
    if (!pipe) return STATUS_SUCCESS;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = function;
    urb.PipeHandle = pipe;
    return Sync(d, (PURB)&urb, CTRL_TIMEOUT);
}

/* NET_IO for the chip: one vendor/device request, data through d->Ctrl. */
static long IoControl(void *context, int in, nc_u8 request, nc_u16 value, nc_u16 index,
                      void *data, nc_u16 length, nc_u16 *actual)
{
    NET_DEV *d = context;
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST urb;
    NTSTATUS status;
    *actual = 0;
    if (d->Gone) return NC_E_IO;
    if (length > CTRL_MAX) return NC_E_MALFORMED;
    if (!in && length) RtlCopyMemory(d->Ctrl, data, length);
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_VENDOR_DEVICE;
    urb.TransferFlags = in ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : 0;
    urb.TransferBuffer = length ? d->Ctrl : NULL;
    urb.TransferBufferLength = length;
    urb.Request = request; urb.Value = value; urb.Index = index;
    status = Sync(d, (PURB)&urb, CTRL_TIMEOUT);
    if (!NT_SUCCESS(status)) {
        if (DeviceGone(status, 0)) d->Gone = TRUE;
        return NC_E_IO;
    }
    if (urb.TransferBufferLength > length) return NC_E_DEVICE;
    if (in && urb.TransferBufferLength) RtlCopyMemory(data, d->Ctrl, urb.TransferBufferLength);
    *actual = (nc_u16)urb.TransferBufferLength;
    return NC_OK;
}

static void IoSleep(void *context, nc_u32 ms)
{
    UNREFERENCED_PARAMETER(context);
    Sleep(ms);
}

static void *ChipState(NET_DEV *d) { return d->ChipState.Bytes; }

static BOOLEAN SameMac(const UCHAR *a, const UCHAR *b)
{
    ULONG i;
    for (i = 0; i < 6; i++) if (a[i] != b[i]) return FALSE;
    return TRUE;
}

/* Start a chip operation with a clean NET_IO status. */
static NET_IO *Io(NET_DEV *d)
{
    d->Io.Status = NC_OK;
    return &d->Io;
}

/* ------------------------------------------------------------------ */
/* The data path. */

static NTSTATUS Done(PDEVICE_OBJECT unused, PIRP irp, PVOID context);
static void Pump(NET_DEV *d);

/* Lock held. A failed data transfer while the gate is open closes it and
 * asks the worker to recover; a transfer failing because the gate closed
 * (aborted) does not. */
static void Fault(NET_DEV *d, NTSTATUS status, USBD_STATUS usb)
{
    if (DeviceGone(status, usb)) d->Gone = TRUE;
    if (d->Open) {
        d->Open = FALSE;
        d->Work |= WORK_RECOVER;
        KeSetEvent(&d->Wake, IO_NO_INCREMENT, FALSE);
    }
}

/* Lock held: wake a quiesce waiting for the last completion. */
static void CheckDrained(NET_DEV *d)
{
    if (!d->Open && !d->Outstanding && !d->PumpActive) KeSetEvent(&d->Drained, IO_NO_INCREMENT, FALSE);
}

static void Submit(NET_DEV *d, SLOT *s, ULONG length)
{
    PIRP irp = IoAllocateIrp(d->Lower->StackSize, FALSE);
    PIO_STACK_LOCATION stack;
    KIRQL old;
    if (!irp) {
        /* Retry from the worker's next tick; a transmit fails now. */
        void *packets[NET_TX_BATCH];
        ULONG count = 0;
        KeAcquireSpinLock(&d->Lock, &old);
        s->Busy = FALSE;
        d->Outstanding--;
        if (s->Kind == SLOT_TX) {
            count = s->Count;
            RtlCopyMemory(packets, s->Packets, count * sizeof(void *));
            s->Count = 0; s->Stage = TX_IDLE;
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
    s->Urb.TransferBuffer = length ? s->Buffer : NULL;
    s->Urb.TransferBufferLength = length;
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = &s->Urb;
    IoSetCompletionRoutine(irp, Done, s, TRUE, TRUE, TRUE);
    (void)IoCallDriver(d->Lower, irp);
}

static nc_u32 AlignUp(nc_u32 v, nc_u32 a) { return (v + a - 1) & ~(a - 1); }

/* Build the next bulk-OUT transfer in slot s. Called by the pump with
 * s->Stage == TX_BUILD and the lock not held. Returns TRUE if a transfer
 * was queued. */
static BOOLEAN BuildTx(NET_DEV *d, SLOT *s)
{
    void *packets[NET_TX_BATCH];
    nc_u32 count, i, total = 0;
    nc_u32 header = d->Params.TxHeader, align = d->Params.TxAlign;
    PUCHAR b = s->Buffer;
    BOOLEAN ok = TRUE;
    int zlp = 0;
    KIRQL old;

    count = d->Cb.TxTake(d->Cb.Context, packets, d->TxBatch);
    if (!count) {
        KeAcquireSpinLock(&d->Lock, &old);
        s->Stage = TX_IDLE;
        KeReleaseSpinLock(&d->Lock, old);
        return FALSE;
    }
    /* TxBatch is sized so that count frames at their largest always fit. */
    for (i = 0; i < count && ok; i++) {
        nc_u32 at = AlignUp(total, align), n;
        if (at + header + NC_ETH_MAX > s->Size) { ok = FALSE; break; }
        n = d->Cb.TxCopy(d->Cb.Context, packets[i], b + at + header, NC_ETH_MAX);
        if (n < NC_ETH_MIN) { ok = FALSE; break; }
        if (n < NC_ETH_PAD) { RtlZeroMemory(b + at + header + n, NC_ETH_PAD - n); n = NC_ETH_PAD; }
        if (at > total) RtlZeroMemory(b + total, at - total);
        d->Chip->TxHeader(ChipState(d), b + at, n, d->OutMax, count == 1);
        total = at + header + n;
    }
    if (ok) zlp = d->Chip->TxEnd(ChipState(d), b, &total, s->Size, d->OutMax);
    KeAcquireSpinLock(&d->Lock, &old);
    if (!ok || !d->Open || !d->LinkUp) {
        s->Stage = TX_IDLE;
        KeReleaseSpinLock(&d->Lock, old);
        if (!ok) { d->Malformed++; NetTrace("tx build failed, %u frames", count); }
        d->Cb.TxDone(d->Cb.Context, packets, count, 0);
        return FALSE;
    }
    RtlCopyMemory(s->Packets, packets, count * sizeof(void *));
    s->Count = count;
    s->Zlp = (BOOLEAN)(zlp != 0);
    s->Stage = TX_DATA;
    s->Busy = TRUE;
    d->Outstanding++;
    KeReleaseSpinLock(&d->Lock, old);
    Submit(d, s, total);
    return TRUE;
}

static void Pump(NET_DEV *d)
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
        SLOT *s = NULL, *build = NULL;
        ULONG i, length = 0;
        if (d->PumpAgain) { d->PumpAgain = FALSE; txTried = FALSE; }
        if (d->Open) {
            if (d->Int && !d->IntSlot.Busy) { s = &d->IntSlot; length = s->Size; }
            for (i = 0; !s && d->LinkUp && i < d->RxCount; i++)
                if (!d->Rx[i].Busy) { s = &d->Rx[i]; length = d->RxLength; }
            for (i = 0; !s && i < d->TxCount; i++)
                if (d->Tx[i].Stage == TX_ZLP_READY) { d->Tx[i].Stage = TX_ZLP; s = &d->Tx[i]; length = 0; }
            /* Transfers on one pipe complete in submission order, so each
             * slot's payload is followed by its own ZLP before the next
             * slot is built only when TxSlots is 1; a chip that needs
             * ZLPs asks for one slot. */
            for (i = 0; !s && !txTried && d->LinkUp && i < d->TxCount; i++)
                if (d->Tx[i].Stage == TX_IDLE) { d->Tx[i].Stage = TX_BUILD; build = &d->Tx[i]; break; }
        }
        if (!s && !build) {
            if (d->PumpAgain) continue;
            break;
        }
        if (s) { s->Busy = TRUE; d->Outstanding++; }
        KeReleaseSpinLock(&d->Lock, old);
        if (s) Submit(d, s, length);
        else if (!BuildTx(d, build)) txTried = TRUE;
        KeAcquireSpinLock(&d->Lock, &old);
    }
    d->PumpActive = FALSE;
    CheckDrained(d);
    KeReleaseSpinLock(&d->Lock, old);
}

static void Emit(void *context, const nc_u8 *frame, nc_u32 length)
{
    NET_DEV *d = context;
    d->Cb.Receive(d->Cb.Context, frame, length);
}

static NTSTATUS Done(PDEVICE_OBJECT unused, PIRP irp, PVOID context)
{
    SLOT *s = context;
    NET_DEV *d = s->Dev;
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
            if (s->Kind == SLOT_INT) {
                int link = d->Chip->Status(ChipState(d), s->Buffer, length);
                if (link >= 0 && (link != 0) != (d->LinkUp != 0)) {
                    KeAcquireSpinLock(&d->Lock, &old);
                    d->Work |= WORK_LINK;
                    KeSetEvent(&d->Wake, IO_NO_INCREMENT, FALSE);
                    KeReleaseSpinLock(&d->Lock, old);
                }
            } else if (length) {
                NET_RX_STATS st = { 0 };
                if (d->Chip->RxParse(ChipState(d), s->Buffer, length, Emit, d, &st) < 0) d->Malformed++;
                else d->Dropped += st.Dropped;
                if (st.Dropped) DropSample(s->Buffer, length);
                if (st.FcsStripped) {
                    /* The chip is configured to deliver frames without
                     * their FCS; the chip core's check stripped it.
                     * Completions run concurrently: the first claims the
                     * report under the lock. */
                    BOOLEAN first;
                    KeAcquireSpinLock(&d->Lock, &old);
                    d->FcsStripped += st.FcsStripped;
                    first = !d->FcsLogged;
                    d->FcsLogged = TRUE;
                    KeReleaseSpinLock(&d->Lock, old);
                    if (first) {
                        NetTrace("anomaly: rx frame arrived with an FCS (configured: none); stripped");
                        d->Cb.Anomaly(d->Cb.Context, NET_ANOMALY_RX_FCS, st.FcsStripped);
                    }
                }
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
        void *packets[NET_TX_BATCH];
        ULONG count = 0;
        KeAcquireSpinLock(&d->Lock, &old);
        s->Busy = FALSE;
        d->Outstanding--;
        if (NT_SUCCESS(status) && s->Stage == TX_DATA && s->Zlp && d->Open) {
            s->Stage = TX_ZLP_READY;   /* the ZLP before any later payload */
        } else {
            count = s->Count;
            RtlCopyMemory(packets, s->Packets, count * sizeof(void *));
            s->Count = 0;
            s->Stage = TX_IDLE;
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
static void Quiesce(NET_DEV *d)
{
    KIRQL old;
    BOOLEAN idle;
    ULONG waits = 0, i;
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
            if (waits > 1) NetTrace("quiesce: %u still outstanding", d->Outstanding);
        }
        (void)Wait(&d->Drained, 500);
    }
    /* A payload whose ZLP was never sent: its frames left the host. */
    for (i = 0; i < d->TxCount; i++) {
        void *packets[NET_TX_BATCH];
        ULONG count = 0;
        SLOT *s = &d->Tx[i];
        KeAcquireSpinLock(&d->Lock, &old);
        if (s->Stage == TX_ZLP_READY) {
            count = s->Count;
            RtlCopyMemory(packets, s->Packets, count * sizeof(void *));
            s->Count = 0; s->Stage = TX_IDLE;
        }
        KeReleaseSpinLock(&d->Lock, old);
        if (count) d->Cb.TxDone(d->Cb.Context, packets, count, 1);
    }
}

static void Reopen(NET_DEV *d)
{
    KIRQL old;
    KeAcquireSpinLock(&d->Lock, &old);
    if (!d->Gone && !d->Stop && !d->Suspended) d->Open = TRUE;
    KeReleaseSpinLock(&d->Lock, old);
    Pump(d);
}

/* ------------------------------------------------------------------ */
/* Chip operations, worker or Start/Open/Stop context. */

static void ReportGone(NET_DEV *d)
{
    KIRQL old;
    BOOLEAN report;
    KeAcquireSpinLock(&d->Lock, &old);
    report = d->Gone && !d->GoneReported;
    if (report) { d->GoneReported = TRUE; d->LinkUp = FALSE; }
    KeReleaseSpinLock(&d->Lock, old);
    if (report) {
        NetTrace("device gone");
        d->Cb.Link(d->Cb.Context, -1, d->Speed);
    }
}

static long ApplyFilter(NET_DEV *d)
{
    long r;
    if (!d->FilterValid || d->Gone || d->Suspended) return NC_OK;
    r = d->Chip->SetFilter(ChipState(d), Io(d), d->Filter, d->Mc, d->McCount);
    if (r < 0) NetTrace("filter %x: %d", d->Filter, r);
    return r;
}

/* Read and apply the link with the data path stopped, then tell NDIS.
 * The caller has quiesced; the caller reopens. */
static void ApplyLink(NET_DEV *d)
{
    NET_LINK link;
    long r;
    KIRQL old;
    BOOLEAN was;
    if (d->Gone || d->Suspended) return;
    link.Up = 0; link.Mbps = 0; link.FullDuplex = 0; link.RxLength = d->RxLength;
    r = d->Chip->Link(ChipState(d), Io(d), &link);
    if (r < 0) {
        NetTrace("link: %d", r);
        link.Up = 0;
    }
    if (link.Up) (void)ApplyFilter(d);
    KeAcquireSpinLock(&d->Lock, &old);
    was = d->LinkUp;
    d->LinkUp = (BOOLEAN)(link.Up != 0);
    if (link.RxLength >= 64 && link.RxLength <= d->Rx[0].Size) d->RxLength = link.RxLength;
    if (link.Mbps) d->Speed = link.Mbps * 10000;
    KeReleaseSpinLock(&d->Lock, old);
    if (was != d->LinkUp)
        NetTrace("link %u, %u Mb/s %s duplex, rx %u", link.Up, link.Mbps,
                 link.FullDuplex ? "full" : "half", d->RxLength);
    /* The upper edge indicates only a change. */
    d->Cb.Link(d->Cb.Context, link.Up ? 1 : 0, d->Speed);
}

static void Recover(NET_DEV *d)
{
    NTSTATUS status;
    Quiesce(d);
    if (d->Gone || d->Suspended) { ReportGone(d); return; }
    if (++d->Recoveries > 30) {
        NetTrace("recover: giving up after %u attempts", d->Recoveries);
        d->Gone = TRUE;
        ReportGone(d);
        return;
    }
    Sleep(d->Recoveries > 3 ? 200 : 20);
    status = PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->In);
    if (NT_SUCCESS(status)) status = PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->Out);
    if (NT_SUCCESS(status)) status = PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->Int);
    NetTrace("recover %u: reset pipes %x", d->Recoveries, status);
    if (!NT_SUCCESS(status) && (DeviceGone(status, 0) || d->Recoveries > 3)) {
        d->Gone = TRUE;
        ReportGone(d);
        return;
    }
    Reopen(d);
}

static void LinkChanged(NET_DEV *d)
{
    Quiesce(d);
    ApplyLink(d);
    if (d->Gone) ReportGone(d);
    Reopen(d);
}

/* The chip's bring-up again (resume), then the address, filter and link. */
static long Restart(NET_DEV *d)
{
    NET_PARAMS p;
    long r = d->Chip->Resume(ChipState(d), Io(d), &d->Probe, &p);
    if (r >= 0 && !SameMac(p.Mac, d->Mac))
        r = d->Chip->SetMac(ChipState(d), Io(d), d->Mac);
    if (r >= 0) r = ApplyFilter(d);
    return r;
}

static long Power(NET_DEV *d, BOOLEAN on)
{
    KIRQL old;
    long r;
    if (!on) {
        Quiesce(d);
        if (!d->Gone) (void)d->Chip->Suspend(ChipState(d), Io(d));
        KeAcquireSpinLock(&d->Lock, &old);
        d->Suspended = TRUE;
        d->LinkUp = FALSE;
        KeReleaseSpinLock(&d->Lock, old);
        return STATUS_SUCCESS;
    }
    KeAcquireSpinLock(&d->Lock, &old);
    d->Suspended = FALSE;
    KeReleaseSpinLock(&d->Lock, old);
    if (d->Gone) return STATUS_SUCCESS;
    /* The device may have lost power: the whole bring-up again (the
     * Realtek and ASIX resume paths both do), then the cached address,
     * filter and the link. */
    r = Restart(d);
    NetTrace("resume: %d", r);
    if (r < 0 && d->Gone) { ReportGone(d); return STATUS_SUCCESS; }
    ApplyLink(d);
    d->Recoveries = 0;
    Reopen(d);
    return STATUS_SUCCESS;
}

static long Reset(NET_DEV *d)
{
    Quiesce(d);
    if (d->Gone) { ReportGone(d); return STATUS_SUCCESS; }
    if (d->Suspended) return STATUS_SUCCESS;
    (void)PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->In);
    (void)PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->Out);
    (void)PipeRequest(d, URB_FUNCTION_RESET_PIPE, d->Int);
    (void)ApplyFilter(d);
    ApplyLink(d);
    NetTrace("reset");
    Reopen(d);
    return STATUS_SUCCESS;
}

static void Worker(PVOID context)
{
    NET_DEV *d = context;
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
            d->Cb.TaskDone(d->Cb.Context, NET_REQ_RESET, result);
        }
        if (req) {
            long result = STATUS_SUCCESS;
            if (req == NET_REQ_FILTER) {
                d->Filter = d->ReqValue; d->FilterValid = TRUE;
                (void)ApplyFilter(d);
            } else if (req == NET_REQ_MULTICAST) {
                d->McCount = d->ReqLength / 6;
                RtlCopyMemory(d->Mc, d->ReqData, d->ReqLength);
                (void)ApplyFilter(d);
            } else if (req == NET_REQ_POWER) {
                result = Power(d, (BOOLEAN)(d->ReqValue == 1));
            }
            KeAcquireSpinLock(&d->Lock, &old);
            d->Req = 0;
            KeReleaseSpinLock(&d->Lock, old);
            d->Cb.TaskDone(d->Cb.Context, req, result);
        }
        if (work & WORK_LINK) {
            LinkChanged(d);
        } else if (status == STATUS_TIMEOUT && !d->Gone && !d->Suspended && d->Open &&
                   (++d->Ticks % 2) == 0) {
            /* A missed or absent link interrupt: ask the chip. */
            if (d->Chip->Poll(ChipState(d), Io(d))) LinkChanged(d);
        }
        if (d->Gone) ReportGone(d);
        Pump(d);   /* retries an IRP allocation that failed */
    }
    KeSetEvent(&d->Exited, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* Configuration. */

static ULONG InterfaceSize(ULONG pipes)
{
    return sizeof(USBD_INTERFACE_INFORMATION) + (pipes > 1 ? (pipes - 1) * sizeof(USBD_PIPE_INFORMATION) : 0);
}

static NTSTATUS SelectConfiguration(NET_DEV *d, PUCHAR config)
{
    ULONG is = InterfaceSize(3);
    ULONG size = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface) + is;
    struct _URB_SELECT_CONFIGURATION *urb = ExAllocatePoolWithTag(NonPagedPool, size, TAG);
    PUSBD_INTERFACE_INFORMATION i;
    NTSTATUS status;
    ULONG k;
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, size);
    urb->Hdr.Length = (USHORT)size;
    urb->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb->ConfigurationDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)config;
    i = &urb->Interface;
    i->Length = (USHORT)is;
    i->InterfaceNumber = d->Probe.Interface;
    i->AlternateSetting = d->Probe.Alternate;
    /* The default maximum is PAGE_SIZE (usbdi.h); ask for the largest
     * buffer either direction uses. */
    for (k = 0; k < 3; k++) i->Pipes[k].MaximumTransferSize = RX_LIMIT > TX_LIMIT ? RX_LIMIT : TX_LIMIT;
    status = Sync(d, (PURB)urb, CTRL_TIMEOUT);
    if (NT_SUCCESS(status) && !urb->ConfigurationHandle) status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (NT_SUCCESS(status)) {
        d->ConfigHandle = urb->ConfigurationHandle;
        for (k = 0; k < i->NumberOfPipes && NT_SUCCESS(status); k++) {
            PUSBD_PIPE_INFORMATION p = &i->Pipes[k];
            if (!p->PipeHandle || !p->MaximumPacketSize) status = STATUS_DEVICE_CONFIGURATION_ERROR;
            else if (p->EndpointAddress == d->Probe.In && p->PipeType == UsbdPipeTypeBulk) {
                d->In = p->PipeHandle; d->InMax = p->MaximumPacketSize;
            } else if (p->EndpointAddress == d->Probe.Out && p->PipeType == UsbdPipeTypeBulk) {
                d->Out = p->PipeHandle; d->OutMax = p->MaximumPacketSize;
            } else if (p->EndpointAddress == d->Probe.Interrupt && p->PipeType == UsbdPipeTypeInterrupt) {
                d->Int = p->PipeHandle;
            } else {
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
            }
        }
        if (NT_SUCCESS(status) && (!d->In || !d->Out || !d->Int)) status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    ExFreePool(urb);
    return status;
}

/* ------------------------------------------------------------------ */
/* The interface. */

static void Free(NET_DEV *d)
{
    ULONG i;
    for (i = 0; i < RX_MAX; i++) if (d->Rx[i].Buffer) ExFreePool(d->Rx[i].Buffer);
    for (i = 0; i < TX_MAX; i++) if (d->Tx[i].Buffer) ExFreePool(d->Tx[i].Buffer);
    if (d->IntSlot.Buffer) ExFreePool(d->IntSlot.Buffer);
    if (d->Ctrl) ExFreePool(d->Ctrl);
    ExFreePool(d);
}

static void Fail(NET_START *start, ULONG stage, NTSTATUS status)
{
    start->Failure = stage; start->Status = status;
    NetTrace("start failed at stage %u, status %x", stage, status);
}

long NetUsbStart(void *lowerDevice, const NET_CALLBACKS *callbacks, NET_START *start, void **device)
{
    NET_DEV *d;
    PUCHAR configs[CONFIGS_MAX];
    ULONG sizes[CONFIGS_MAX];
    UCHAR dev[18];
    NTSTATUS status;
    ULONG got, i, count;
    long r;
    USB_CONFIGURATION_DESCRIPTOR header;

    *device = NULL;
    start->Failure = NET_FAIL_NONE;
    RtlZeroMemory(configs, sizeof(configs));
    d = ExAllocatePoolWithTag(NonPagedPool, sizeof(*d), TAG);
    if (!d) { Fail(start, NET_FAIL_MEMORY, STATUS_INSUFFICIENT_RESOURCES); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(d, sizeof(*d));
    d->Lower = lowerDevice;
    d->Cb = *callbacks;
    d->Chip = &NetPackageChip;
    d->Io.Context = d;
    d->Io.Control = IoControl;
    d->Io.Sleep = IoSleep;
    KeInitializeSpinLock(&d->Lock);
    KeInitializeEvent(&d->Drained, NotificationEvent, FALSE);
    KeInitializeEvent(&d->Wake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&d->Exited, NotificationEvent, FALSE);
    d->Ctrl = ExAllocatePoolWithTag(NonPagedPool, CTRL_MAX, TAG);
    if (!d->Ctrl) { Fail(start, NET_FAIL_MEMORY, STATUS_INSUFFICIENT_RESOURCES); status = STATUS_INSUFFICIENT_RESOURCES; goto fail; }

    /* Device descriptor, then every configuration (section 5.2). */
    status = GetDescriptor(d, USB_DEVICE_DESCRIPTOR_TYPE, 0, d->Ctrl, 18, &got);
    if (!NT_SUCCESS(status) || got != 18 || d->Ctrl[0] != 18 || d->Ctrl[1] != 1) {
        Fail(start, NET_FAIL_DEVICE, NT_SUCCESS(status) ? STATUS_DEVICE_DATA_ERROR : status);
        if (NT_SUCCESS(status)) status = STATUS_DEVICE_DATA_ERROR;
        goto fail;
    }
    RtlCopyMemory(dev, d->Ctrl, 18);
    count = dev[17];
    if (count > CONFIGS_MAX) count = CONFIGS_MAX;
    for (i = 0; i < count; i++) {
        sizes[i] = 0;
        status = GetDescriptor(d, USB_CONFIGURATION_DESCRIPTOR_TYPE, (UCHAR)i, &header, sizeof(header), &got);
        if (!NT_SUCCESS(status) || got != sizeof(header) || header.wTotalLength < sizeof(header) ||
            header.wTotalLength > CONFIG_MAX) continue;
        configs[i] = ExAllocatePoolWithTag(NonPagedPool, header.wTotalLength, TAG);
        if (!configs[i]) continue;
        status = GetDescriptor(d, USB_CONFIGURATION_DESCRIPTOR_TYPE, (UCHAR)i, configs[i], header.wTotalLength, &got);
        if (NT_SUCCESS(status) && got == header.wTotalLength) sizes[i] = got;
    }
    r = d->Chip->Probe(dev, 18, (const nc_u8 *const *)configs, sizes, count, &d->Probe);
    NetTrace("device %x:%x rev %x, %u configurations: probe %d", d->Probe.Vendor, d->Probe.Product,
             d->Probe.Revision, count, r);
    if (r < 0) { Fail(start, NET_FAIL_PROBE, STATUS_NOT_SUPPORTED); status = STATUS_NOT_SUPPORTED; goto fail; }
    status = SelectConfiguration(d, configs[d->Probe.ConfigIndex]);
    NetTrace("select configuration %u (value %u) interface %u: %x", d->Probe.ConfigIndex,
             d->Probe.ConfigValue, d->Probe.Interface, status);
    if (!NT_SUCCESS(status)) { Fail(start, NET_FAIL_SELECT, status); goto fail; }
    for (i = 0; i < CONFIGS_MAX; i++) if (configs[i]) { ExFreePool(configs[i]); configs[i] = NULL; }

    r = d->Chip->Start(ChipState(d), Io(d), &d->Probe, &d->Params);
    NetTrace("chip start: %d, version %x, %u requests", r, d->Params.Version, d->Io.Requests);
    if (d->Params.RxConfig || d->Params.RxConfigRead)
        NetTrace("rx config: wrote %x, read back %x%s", d->Params.RxConfig, d->Params.RxConfigRead,
                 d->Params.RxConfig == d->Params.RxConfigRead ? "" : " (MISMATCH)");
    if (r < 0) {
        Fail(start, NET_FAIL_CHIP, d->Gone ? STATUS_DEVICE_NOT_CONNECTED : STATUS_DEVICE_DATA_ERROR);
        status = STATUS_DEVICE_DATA_ERROR;
        goto fail;
    }

    /* Buffers: every data buffer is nonpaged, owned by its slot. */
    {
        ULONG rxSize = d->Params.RxSize, rxCount = d->Params.RxCount;
        ULONG txSize = d->Params.TxSize, txSlots = d->Params.TxSlots;
        ULONG each = d->Params.TxHeader + NC_ETH_MAX + d->Params.TxAlign - 1;
        if (d->Params.TxAlign == 0 || (d->Params.TxAlign & (d->Params.TxAlign - 1))) d->Params.TxAlign = 1;
        if (rxSize > RX_LIMIT) rxSize = RX_LIMIT;
        if (rxCount > RX_MAX) rxCount = RX_MAX;
        if (txSize > TX_LIMIT) txSize = TX_LIMIT;
        if (txSlots > TX_MAX) txSlots = TX_MAX;
        if (!txSlots) txSlots = 1;
        d->TxBatch = txSize / each;
        if (d->TxBatch > d->Params.TxBatch) d->TxBatch = d->Params.TxBatch;
        if (d->TxBatch > NET_TX_BATCH) d->TxBatch = NET_TX_BATCH;
        if (!d->TxBatch || txSize < d->Params.TxHeader + NC_ETH_MAX || rxSize < 2048) {
            Fail(start, NET_FAIL_CHIP, STATUS_INVALID_PARAMETER);
            status = STATUS_INVALID_PARAMETER;
            goto fail;
        }
        for (i = 0; i < rxCount; i++) {
            d->Rx[i].Dev = d; d->Rx[i].Kind = SLOT_RX; d->Rx[i].Size = rxSize;
            d->Rx[i].Buffer = ExAllocatePoolWithTag(NonPagedPool, rxSize, TAG);
            if (!d->Rx[i].Buffer) break;
        }
        d->RxCount = i;
        d->RxLength = rxSize;
        d->IntSlot.Dev = d; d->IntSlot.Kind = SLOT_INT;
        d->IntSlot.Size = d->Probe.IntMax ? d->Probe.IntMax : 8;
        d->IntSlot.Buffer = ExAllocatePoolWithTag(NonPagedPool, d->IntSlot.Size, TAG);
        for (i = 0; i < txSlots; i++) {
            d->Tx[i].Dev = d; d->Tx[i].Kind = SLOT_TX; d->Tx[i].Size = txSize;
            d->Tx[i].Buffer = ExAllocatePoolWithTag(NonPagedPool, txSize, TAG);
            if (!d->Tx[i].Buffer) break;
        }
        d->TxCount = i;
        if (!d->RxCount || !d->IntSlot.Buffer || !d->TxCount) {
            Fail(start, NET_FAIL_MEMORY, STATUS_INSUFFICIENT_RESOURCES);
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto fail;
        }
        NetTrace("buffers: rx %u x %u, tx %u x %u (%u frames)", d->RxCount, rxSize, d->TxCount,
                 txSize, d->TxBatch);
    }
    RtlCopyMemory(start->DeviceMac, d->Params.Mac, 6);
    start->Vendor = d->Probe.Vendor; start->Product = d->Probe.Product;
    start->Revision = d->Probe.Revision;
    start->Version = d->Params.Version;
    start->Speed = d->Probe.Speed;
    *device = d;
    return STATUS_SUCCESS;

fail:
    for (i = 0; i < CONFIGS_MAX; i++) if (configs[i]) ExFreePool(configs[i]);
    if (d->In && !d->Gone) (void)d->Chip->Stop(ChipState(d), Io(d));
    Free(d);
    return status;
}

long NetUsbOpen(void *device, const nc_u8 mac[6], NET_START *start, int *connected, nc_u32 *speed)
{
    NET_DEV *d = device;
    NTSTATUS status;
    HANDLE thread;
    long r = NC_OK;

    RtlCopyMemory(d->Mac, mac, 6);
    if (!SameMac(mac, d->Params.Mac)) r = d->Chip->SetMac(ChipState(d), Io(d), mac);
    if (r < 0) {
        Fail(start, NET_FAIL_MAC, d->Gone ? STATUS_DEVICE_NOT_CONNECTED : STATUS_DEVICE_DATA_ERROR);
        (void)d->Chip->Stop(ChipState(d), Io(d));
        Free(d);
        return STATUS_DEVICE_DATA_ERROR;
    }
    d->Speed = 1000000;
    ApplyLink(d);          /* reports through Link before NDIS is ready: ignored there */
    status = PsCreateSystemThread(&thread, 0, NULL, NULL, NULL, Worker, d);
    if (!NT_SUCCESS(status)) {
        Fail(start, NET_FAIL_THREAD, status);
        (void)d->Chip->Stop(ChipState(d), Io(d));
        Free(d);
        return status;
    }
    d->Worker = TRUE;
    /* NT: also keep the thread object, to wait for the thread itself. */
    if (IoIsWdmVersionAvailable(1, 0x10) &&
        !NT_SUCCESS(ObReferenceObjectByHandle(thread, 0, NULL, KernelMode, &d->ThreadObject, NULL)))
        d->ThreadObject = NULL;
    ZwClose(thread);
    *connected = d->LinkUp;
    *speed = d->Speed;
    Reopen(d);
    return STATUS_SUCCESS;
}

void NetUsbStop(void *device)
{
    NET_DEV *d = device;
    KIRQL old;
    int req;
    if (!d) return;
    KeAcquireSpinLock(&d->Lock, &old);
    d->Stop = TRUE;
    KeSetEvent(&d->Wake, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&d->Lock, old);
    if (d->Worker) {
        Wait(&d->Exited, 0);
        if (d->ThreadObject) {
            KeWaitForSingleObject(d->ThreadObject, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(d->ThreadObject);
        }
    }
    Quiesce(d);
    if (!d->Gone && !d->Suspended) (void)d->Chip->Stop(ChipState(d), Io(d));
    /* A request the worker never reached still completes once. */
    KeAcquireSpinLock(&d->Lock, &old);
    req = d->Req; d->Req = 0;
    KeReleaseSpinLock(&d->Lock, old);
    if (req) d->Cb.TaskDone(d->Cb.Context, req, STATUS_SUCCESS);
    if (d->ResetPending) d->Cb.TaskDone(d->Cb.Context, NET_REQ_RESET, STATUS_SUCCESS);
    NetTrace("stopped; %u malformed transfers, %u dropped frames, %u with an FCS%s", d->Malformed, d->Dropped,
             d->FcsStripped, d->FcsStripped ? " (anomaly)" : "");
    Free(d);
}

void NetUsbKick(void *device)
{
    Pump(device);
}

int NetUsbRequest(void *device, int request, nc_u32 value, const nc_u8 *data, nc_u32 length)
{
    NET_DEV *d = device;
    KIRQL old;
    int queued = 0;
    KeAcquireSpinLock(&d->Lock, &old);
    if (!d->Stop) {
        if (request == NET_REQ_RESET) {
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

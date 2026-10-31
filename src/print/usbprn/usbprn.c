/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * usbprn.c - USBPRN.SYS, the Windows 98 SE WDM function driver for the USB
 * printer class (USB Device Class Definition for Printing Devices 1.1;
 * docs/design/printing.md section 5).
 *
 * One FDO per printer interface devnode. It selects the printer alternate
 * (prncore.c), answers the class requests GET_DEVICE_ID, GET_PORT_STATUS
 * and SOFT_RESET through the IOCTL_USBPRINT_* codes, sends WriteFile data
 * on the bulk OUT pipe and reads the bulk IN pipe on ReadFile. Each
 * printer gets a port number n, kept in its devnode's hardware key as
 * PortName "USB00n", and the link \DosDevices\USBPRN<n> that
 * USBPRNMN.DLL's port USB00n opens.
 *
 * I/O is synchronous in the caller's thread (the spooler's WritePort
 * waits for it in any case): a transfer is waited for in slices of 1.5 s,
 * with GET_PORT_STATUS read between slices, and given up (abort pipe, or
 * cancel when the abort cannot be sent) when the caller's timeout passes,
 * when the device goes, stops or sleeps. URB IRPs are the driver's own
 * (IoInitializeIrp), so a cancel never races their release.
 *
 * Original code, written from the Windows 98 and 2000 DDK documentation
 * and headers. It uses only routines the 98 DDK's WDM.H declares; the
 * build compiles it against both DDKs' headers and checks the imports.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include <wdm.h>
#include <usbdi.h>
#include <usbdlib.h>
#include "prncore.h"
#include "usbprn.h"

#define UP_TAG              'nrPU'
#define UP_SLICE_MS         1500        /* status poll while a write waits (design 5.1) */
#define UP_CTRL_MS          5000        /* a class request is given up after this */
#define UP_READ_DEFAULT_MS  1000
#define UP_QUIESCE_MS       100         /* STOP / D3: abort again this often until I/O drains */
#define UP_XFER_WANT        16384       /* bulk MaximumTransferSize asked for */
#define UP_CFG_MAX          4096
#define UP_SCAN_BUDGET      8192        /* HKLM\Enum\USB instance keys read at most */

typedef struct UP_DEV {
    PDEVICE_OBJECT  Self, Pdo, Lower;
    KEVENT          RemoveEvent;
    LONG            IoCount;
    KEVENT          WriteLock, ReadLock;    /* one write and one read at a time */
    KEVENT          CtrlLock;               /* one class request on endpoint 0 at a time */
    WORK_QUEUE_ITEM PowerWork;              /* D3: drains I/O off the power path */
    PIRP            PowerIrp;               /* the D3 IRP it then passes down */
    KSPIN_LOCK      Lock;                   /* Removed (set), OpenCount, Writers, DeletePending */
    LONG            OpenCount;
    LONG            Writers;
    BOOLEAN         DeletePending;          /* removed with handles open */
    volatile BOOLEAN Removed, Started, Suspended;
    volatile BOOLEAN Starting;              /* START's own class requests may run */
    BOOLEAN         LinkMade;
    BOOLEAN         PortHeld;               /* PortNumber's bit in g_Live is ours */
    ULONG           Tag;                    /* \Device\UsbPrnF<Tag> */
    ULONG           PortNumber;             /* n of USB00n, 0 before start */

    USB_DEVICE_DESCRIPTOR DevDesc;
    PUSB_CONFIGURATION_DESCRIPTOR Config;
    ULONG           ConfigLength;
    PURB            Select;
    PC_ALT          Alt;
    USBD_PIPE_HANDLE OutPipe, InPipe;
    ULONG           OutXfer, InXfer;        /* MaximumTransferSize the stack gave */

    volatile UCHAR  LastStatus;
    volatile BOOLEAN StatusValid;
    BOOLEAN         ResetOther;             /* SOFT_RESET as 23h (the 1.0 code) */
    BOOLEAN         IdPlainIndex;           /* GET_DEVICE_ID wants wIndex = interface */
    ULONG           WriteMs, ReadMs;
    ULONG           LastWriteBytes;
    BOOLEAN         LastWriteShort;
    char            Description[96];
} UP_DEV;

static KSPIN_LOCK g_LiveLock;
static UCHAR      g_Live[PC_PORT_MAX / 8 + 1];  /* port numbers of started printers */
static KEVENT     g_AssignLock;                 /* one port assignment at a time */
static LONG       g_TagCounter;

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path);
static NTSTATUS UpAddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo);
static NTSTATUS UpDispatch(PDEVICE_OBJECT dev, PIRP irp);
static NTSTATUS UpDispatchPnp(PDEVICE_OBJECT dev, PIRP irp);
static NTSTATUS UpDispatchPower(PDEVICE_OBJECT dev, PIRP irp);
static VOID UpUnload(PDRIVER_OBJECT drv);

/* ---- trace (test flavour only) -------------------------------------------- */

#if defined(UP_TRACE_E9)
/* _outp is the compiler's intrinsic for the x86 OUT instruction. */
int __cdecl _outp(unsigned short port, int value);
#pragma intrinsic(_outp)

/* One line on QEMU's debug console: the text, then up to two numbers in hex. */
static void UpTrace(const char *s, ULONG a, ULONG b, int n)
{
    static const char hex[] = "0123456789abcdef";
    int i, k;
    ULONG v[2];

    v[0] = a;
    v[1] = b;
    for (i = 0; "up: "[i]; i++)
        _outp(0xE9, "up: "[i]);
    for (i = 0; s[i]; i++)
        _outp(0xE9, s[i]);
    for (k = 0; k < n && k < 2; k++) {
        _outp(0xE9, ' ');
        for (i = 28; i >= 0; i -= 4)
            _outp(0xE9, hex[(v[k] >> i) & 15]);
    }
    _outp(0xE9, '\n');
}
#define UPT0(s)         UpTrace(s, 0, 0, 0)
#define UPT1(s, a)      UpTrace(s, (ULONG)(a), 0, 1)
#define UPT2(s, a, b)   UpTrace(s, (ULONG)(a), (ULONG)(b), 2)
#else
#define UPT0(s)         ((void)0)
#define UPT1(s, a)      ((void)0)
#define UPT2(s, a, b)   ((void)0)
#endif

/* ---- small helpers -------------------------------------------------------- */

static NTSTATUS UpComplete(PIRP irp, NTSTATUS st, ULONG info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/* "<prefix><decimal n>" as a counted UNICODE_STRING in buf (cap chars). */
static void UpNameN(UNICODE_STRING *us, WCHAR *buf, ULONG cap, const char *prefix, ULONG n)
{
    char d[12];
    ULONG i = 0, j = 11;

    d[j] = 0;
    do {
        d[--j] = (char)('0' + n % 10);
        n /= 10;
    } while (n && j > 0);
    while (*prefix && i + 1 < cap)
        buf[i++] = (WCHAR)*prefix++;
    while (d[j] && i + 1 < cap)
        buf[i++] = (WCHAR)d[j++];
    buf[i] = 0;
    us->Buffer = buf;
    us->Length = (USHORT)(i * sizeof(WCHAR));
    us->MaximumLength = (USHORT)(cap * sizeof(WCHAR));
}

static BOOLEAN UpAcquire(UP_DEV *d)
{
    InterlockedIncrement(&d->IoCount);
    if (d->Removed) {
        if (InterlockedDecrement(&d->IoCount) == 0)
            KeSetEvent(&d->RemoveEvent, IO_NO_INCREMENT, FALSE);
        return FALSE;
    }
    return TRUE;
}

static void UpRelease(UP_DEV *d)
{
    if (InterlockedDecrement(&d->IoCount) == 0)
        KeSetEvent(&d->RemoveEvent, IO_NO_INCREMENT, FALSE);
}

static void UpLock(KEVENT *m)
{
    KeWaitForSingleObject(m, Executive, KernelMode, FALSE, NULL);
}

/* TRUE when the lock was taken within ms. */
static BOOLEAN UpTryLock(KEVENT *m, ULONG ms)
{
    LARGE_INTEGER t;

    t.QuadPart = -(LONGLONG)ms * 10000;
    return KeWaitForSingleObject(m, Executive, KernelMode, FALSE, &t) != STATUS_TIMEOUT;
}

static void UpUnlock(KEVENT *m)
{
    KeSetEvent(m, IO_NO_INCREMENT, FALSE);
}

/* Takes the lock for a caller's IRP; FALSE when the IRP was cancelled first. */
static BOOLEAN UpLockFor(KEVENT *m, PIRP irp)
{
    while (!UpTryLock(m, UP_SLICE_MS))
        if (irp->Cancel)
            return FALSE;
    return TRUE;
}

/* Whether new transfers may start: started, present, awake. */
static BOOLEAN UpUsable(UP_DEV *d)
{
    return d->Started && !d->Removed && !d->Suspended;
}

/* The interrupt-time clock in 100 ns units: monotonic, not moved by
   changes to the system time. */
static ULONGLONG UpNow(void)
{
    return (ULONGLONG)KeQueryInterruptTime();
}

/* Milliseconds since start (UpNow), as a 64-bit count. */
static ULONGLONG UpElapsedMs(ULONGLONG start)
{
    ULONGLONG now = UpNow();
    return now > start ? (now - start) / 10000 : 0;
}

/* The wait before the next check: a slice, cut at the deadline. */
static ULONG UpSlice(ULONGLONG waited, ULONG limit)
{
    if (limit && waited < limit && limit - waited < UP_SLICE_MS)
        return (ULONG)(limit - waited);
    return UP_SLICE_MS;
}

/* ---- URBs ------------------------------------------------------------------ */

static NTSTATUS UpSignal(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS UpCallLowerSync(UP_DEV *d, PIRP irp)
{
    KEVENT ev;
    NTSTATUS st;

    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, UpSignal, &ev, TRUE, TRUE, TRUE);
    st = IoCallDriver(d->Lower, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    return st;
}

/*
 * A URB request in flight on a driver-owned IRP: the completion routine
 * only signals, so the IRP stays valid (and cancellable) until UpEnd.
 */
typedef struct UP_REQ {
    KEVENT  Event;
    PIRP    Irp;
} UP_REQ;

static NTSTATUS UpBegin(UP_DEV *d, UP_REQ *r, PURB urb)
{
    CCHAR n = d->Lower->StackSize;
    USHORT size = IoSizeOfIrp(n);
    PIO_STACK_LOCATION s;

    r->Irp = (PIRP)ExAllocatePoolWithTag(NonPagedPool, size, UP_TAG);
    if (r->Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    IoInitializeIrp(r->Irp, size, n);
    KeInitializeEvent(&r->Event, NotificationEvent, FALSE);
    s = IoGetNextIrpStackLocation(r->Irp);
    s->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    s->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    s->Parameters.Others.Argument1 = urb;
    IoSetCompletionRoutine(r->Irp, UpSignal, &r->Event, TRUE, TRUE, TRUE);
    (void)IoCallDriver(d->Lower, r->Irp);
    return STATUS_PENDING;
}

/* Waits up to ms (0 = forever); TRUE when the request completed. */
static BOOLEAN UpWait(UP_REQ *r, ULONG ms)
{
    LARGE_INTEGER t;

    if (ms == 0) {
        KeWaitForSingleObject(&r->Event, Executive, KernelMode, FALSE, NULL);
        return TRUE;
    }
    t.QuadPart = -(LONGLONG)ms * 10000;
    return KeWaitForSingleObject(&r->Event, Executive, KernelMode, FALSE, &t) != STATUS_TIMEOUT;
}

/* After completion: the status, and the IRP freed. */
static NTSTATUS UpEnd(UP_REQ *r)
{
    NTSTATUS st = r->Irp->IoStatus.Status;
    ExFreePool(r->Irp);
    r->Irp = NULL;
    return st;
}

static NTSTATUS UpSubmitSync(UP_DEV *d, PURB urb)
{
    UP_REQ r;
    NTSTATUS st = UpBegin(d, &r, urb);

    if (st != STATUS_PENDING)
        return st;
    UpWait(&r, 0);
    return UpEnd(&r);
}

/* The same, cancelled when it has not completed after ms. */
static NTSTATUS UpSubmitBounded(UP_DEV *d, PURB urb, ULONG ms)
{
    UP_REQ r;
    NTSTATUS st = UpBegin(d, &r, urb);

    if (st != STATUS_PENDING)
        return st;
    if (!UpWait(&r, ms)) {
        IoCancelIrp(r.Irp);
        UpWait(&r, 0);
    }
    return UpEnd(&r);
}

static NTSTATUS UpPipeRequest(UP_DEV *d, USBD_PIPE_HANDLE pipe, USHORT function)
{
    struct _URB_PIPE_REQUEST urb;

    if (pipe == NULL)
        return STATUS_SUCCESS;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = function;
    urb.PipeHandle = pipe;
    return UpSubmitBounded(d, (PURB)&urb, UP_CTRL_MS);
}

/* Gives up a transfer in flight on pipe: abort the pipe, and if the abort
   could not be sent or failed, cancel the IRP; then wait for it. */
static void UpGiveUp(UP_DEV *d, UP_REQ *r, USBD_PIPE_HANDLE pipe)
{
    NTSTATUS st = UpPipeRequest(d, pipe, URB_FUNCTION_ABORT_PIPE);

    UPT1("give up: abort pipe (status)", st);
    if (!NT_SUCCESS(st) || !UpWait(r, UP_CTRL_MS)) {
        UPT1("abort did not end the transfer: cancel", st);
        IoCancelIrp(r->Irp);
        UpWait(r, 0);
    }
}

/*
 * A class request on endpoint 0 (function URB_FUNCTION_CLASS_INTERFACE or
 * _OTHER). The buffer must be nonpaged. Cancelled after UP_CTRL_MS.
 * *got receives the bytes moved.
 */
static NTSTATUS UpClass(UP_DEV *d, USHORT function, BOOLEAN in, UCHAR request,
                        USHORT value, USHORT index, PVOID buf, ULONG len, ULONG *got)
{
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST urb;
    NTSTATUS st;

    if (got)
        *got = 0;
    RtlZeroMemory(&urb, sizeof(urb));
    UsbBuildVendorRequest((PURB)&urb, function, sizeof(urb),
                          in ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK) : 0,
                          0, request, value, index, buf, NULL, len, NULL);
    /* Serialised, and not while removed or asleep (UpQuiesce takes it). */
    UpLock(&d->CtrlLock);
    if (d->Removed)
        st = STATUS_DEVICE_NOT_CONNECTED;
    else if (d->Suspended)
        st = STATUS_CANCELLED;
    else if (!d->Started && !d->Starting)
        st = STATUS_DEVICE_NOT_READY;
    else
        st = UpSubmitBounded(d, (PURB)&urb, UP_CTRL_MS);
    UpUnlock(&d->CtrlLock);
    if (got && NT_SUCCESS(st))
        *got = urb.TransferBufferLength <= len ? urb.TransferBufferLength : len;
    return st;
}

static NTSTATUS UpGetDescriptor(UP_DEV *d, UCHAR type, PVOID buf, ULONG len, ULONG *got)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
    NTSTATUS st;

    RtlZeroMemory(&urb, sizeof(urb));
    UsbBuildGetDescriptorRequest((PURB)&urb, sizeof(urb), type, 0, 0, buf, NULL, len, NULL);
    st = UpSubmitSync(d, (PURB)&urb);
    if (got)
        *got = NT_SUCCESS(st) ? (urb.TransferBufferLength <= len ? urb.TransferBufferLength : len) : 0;
    return st;
}

/* ---- class requests (design 5.1) ------------------------------------------ */

static NTSTATUS UpReadStatus(UP_DEV *d, UCHAR *out)
{
    UCHAR *b = (UCHAR *)ExAllocatePoolWithTag(NonPagedPool, 8, UP_TAG);
    ULONG got = 0;
    NTSTATUS st;

    if (b == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    b[0] = 0;
    st = UpClass(d, URB_FUNCTION_CLASS_INTERFACE, TRUE, 1, 0, d->Alt.Interface, b, 1, &got);
    if (NT_SUCCESS(st) && got < 1)
        st = STATUS_DEVICE_DATA_ERROR;
    if (NT_SUCCESS(st)) {
        d->LastStatus = b[0];
        d->StatusValid = TRUE;
        if (out)
            *out = b[0];
    }
    ExFreePool(b);
    return st;
}

/*
 * GET_DEVICE_ID into buf (nonpaged, len >= 2): wIndex carries the interface
 * in the high byte and the alternate in the low byte (spec 4.2.1); for a
 * non-zero interface a failure or an answer under 2 bytes is retried with
 * the plain interface number, which is then kept.
 */
static NTSTATUS UpReadId(UP_DEV *d, PUCHAR buf, ULONG len, ULONG *got)
{
    USHORT spec = (USHORT)((d->Alt.Interface << 8) | d->Alt.Alternate);
    NTSTATUS st;

    *got = 0;
    if (!d->IdPlainIndex) {
        st = UpClass(d, URB_FUNCTION_CLASS_INTERFACE, TRUE, 0, 0, spec, buf, len, got);
        if ((NT_SUCCESS(st) && *got >= 2) || d->Alt.Interface == 0)
            return NT_SUCCESS(st) && *got < 2 ? STATUS_DEVICE_DATA_ERROR : st;
    }
    st = UpClass(d, URB_FUNCTION_CLASS_INTERFACE, TRUE, 0, 0, d->Alt.Interface, buf, len, got);
    if (NT_SUCCESS(st) && *got >= 2) {
        d->IdPlainIndex = TRUE;
        return st;
    }
    return NT_SUCCESS(st) ? STATUS_DEVICE_DATA_ERROR : st;
}

/* SOFT_RESET as 21h, then (on a failure) as 23h, the printer class 1.0 code
   spec 4.2.3 says hosts should expect; the one that worked is kept. */
static NTSTATUS UpSoftReset(UP_DEV *d)
{
    NTSTATUS st;

    if (!d->ResetOther) {
        st = UpClass(d, URB_FUNCTION_CLASS_INTERFACE, FALSE, 2, 0, d->Alt.Interface, NULL, 0, NULL);
        if (NT_SUCCESS(st))
            return st;
    }
    st = UpClass(d, URB_FUNCTION_CLASS_OTHER, FALSE, 2, 0, d->Alt.Interface, NULL, 0, NULL);
    if (NT_SUCCESS(st))
        d->ResetOther = TRUE;
    UPT2("soft reset (status, other)", st, d->ResetOther);
    return st;
}

/* ---- registry: port numbers (design 4.2) ---------------------------------- */

static NTSTATUS UpOpenKey(const WCHAR *path, ACCESS_MASK access, HANDLE *h)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;

    RtlInitUnicodeString(&us, path);
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwOpenKey(h, access, &oa);
}

static NTSTATUS UpOpenRelKey(HANDLE parent, const WCHAR *name, ULONG nameBytes, HANDLE *h)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;

    us.Buffer = (PWSTR)name;
    us.Length = (USHORT)nameBytes;
    us.MaximumLength = (USHORT)nameBytes;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, parent, NULL);
    return ZwOpenKey(h, KEY_READ, &oa);
}

#define UP_PORT_UNREAD  0xFFFFFFFFUL

/* The port number of a "USBnnn" REG_SZ PortName in the key, 0 if there is
   none (or another value), UP_PORT_UNREAD when it could not be read. */
static ULONG UpReadPortName(HANDLE h, PUCHAR buf, ULONG size)
{
    UNICODE_STRING us;
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG got = 0, i, n;
    char s[8];
    const WCHAR *w;
    NTSTATUS st;

    RtlInitUnicodeString(&us, L"PortName");
    st = ZwQueryValueKey(h, &us, KeyValuePartialInformation, buf, size, &got);
    if (st == STATUS_OBJECT_NAME_NOT_FOUND)
        return 0;
    if (st == STATUS_BUFFER_OVERFLOW || st == STATUS_BUFFER_TOO_SMALL)
        return 0;                   /* longer than any "USBnnn" */
    if (!NT_SUCCESS(st))
        return UP_PORT_UNREAD;
    if (v->Type != REG_SZ || v->DataLength < 2 * sizeof(WCHAR))
        return 0;
    w = (const WCHAR *)v->Data;
    n = v->DataLength / sizeof(WCHAR);
    for (i = 0; i < n && i < 7 && w[i]; i++) {
        if (w[i] > 0x7F)
            return 0;
        s[i] = (char)w[i];
    }
    if (i < n && i == 7 && w[i])
        return 0;
    return PcParsePortName(s, i);
}

/*
 * Counts the PortName "USBnnn" values under HKLM\Enum\USB\<id>\<instance>
 * into count[n]. Returns FALSE when the scan could not read the whole tree
 * (the root key, memory, or more than UP_SCAN_BUDGET instance keys).
 */
static BOOLEAN UpScanEnum(UCHAR count[PC_PORT_MAX + 1])
{
    PUCHAR info, val;
    HANDLE h0, h1, h2;
    ULONG i0, i1, got, budget = UP_SCAN_BUDGET;
    const ULONG infoSize = 512, valSize = 64;
    BOOLEAN complete = TRUE;
    NTSTATUS st;

    if (!NT_SUCCESS(UpOpenKey(L"\\Registry\\Machine\\Enum\\USB", KEY_READ, &h0)))
        return FALSE;
    info = (PUCHAR)ExAllocatePoolWithTag(PagedPool, 2 * infoSize + valSize, UP_TAG);
    if (info == NULL) {
        ZwClose(h0);
        return FALSE;
    }
    val = info + 2 * infoSize;
    for (i0 = 0; ; i0++) {
        PKEY_BASIC_INFORMATION k0 = (PKEY_BASIC_INFORMATION)info;
        st = ZwEnumerateKey(h0, i0, KeyBasicInformation, k0, infoSize, &got);
        if (st == STATUS_NO_MORE_ENTRIES)
            break;
        if (!NT_SUCCESS(st) || !NT_SUCCESS(UpOpenRelKey(h0, k0->Name, k0->NameLength, &h1))) {
            complete = FALSE;
            continue;
        }
        for (i1 = 0; ; i1++) {
            PKEY_BASIC_INFORMATION k1 = (PKEY_BASIC_INFORMATION)(info + infoSize);
            ULONG n;
            st = ZwEnumerateKey(h1, i1, KeyBasicInformation, k1, infoSize, &got);
            if (st == STATUS_NO_MORE_ENTRIES)
                break;
            if (budget == 0 || !NT_SUCCESS(st)) {
                complete = FALSE;
                break;
            }
            budget--;
            if (!NT_SUCCESS(UpOpenRelKey(h1, k1->Name, k1->NameLength, &h2))) {
                complete = FALSE;
                continue;
            }
            n = UpReadPortName(h2, val, valSize);
            if (n == UP_PORT_UNREAD)
                complete = FALSE;
            else if (n && count[n] < 255)
                count[n]++;
            ZwClose(h2);
        }
        ZwClose(h1);
        if (budget == 0) {
            complete = FALSE;
            break;
        }
    }
    ExFreePool(info);
    ZwClose(h0);
    return complete;
}

/* Writes PortName "USBnnn" into the devnode's hardware key. */
static NTSTATUS UpWritePortName(UP_DEV *d, ULONG n)
{
    UNICODE_STRING us;
    HANDLE h;
    WCHAR w[8];
    char s[8];
    ULONG i;
    NTSTATUS st;

    st = IoOpenDeviceRegistryKey(d->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &h);
    if (!NT_SUCCESS(st))
        return st;
    PcPortName(n, s);
    for (i = 0; i < 7; i++)
        w[i] = (WCHAR)s[i];
    RtlInitUnicodeString(&us, L"PortName");
    st = ZwSetValueKey(h, &us, 0, REG_SZ, w, 7 * sizeof(WCHAR));
    ZwClose(h);
    return st;
}

/*
 * Picks the port number: the one this devnode's PortName already names,
 * when no started printer holds it and no other devnode names it; else the
 * lowest number no started printer holds and no devnode names (a printer
 * that is unplugged keeps its number). A new number is written into the
 * devnode's hardware key before it is used; if that fails, or the Enum
 * scan was incomplete and there is no number of our own, none is given.
 */
static ULONG UpAssignPort(UP_DEV *d)
{
    UCHAR used[PC_PORT_MAX / 8 + 1], count[PC_PORT_MAX + 1], vbuf[64];
    KIRQL irql;
    HANDLE h;
    ULONG mine = 0, n = 0, i;
    BOOLEAN complete;

    UpLock(&g_AssignLock);
    RtlZeroMemory(count, sizeof(count));
    complete = UpScanEnum(count);
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(d->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &h))) {
        mine = UpReadPortName(h, vbuf, sizeof(vbuf));
        ZwClose(h);
    } else {
        mine = UP_PORT_UNREAD;
    }
    if (mine == UP_PORT_UNREAD) {       /* our own reservation is unknown */
        mine = 0;
        complete = FALSE;
    }
    KeAcquireSpinLock(&g_LiveLock, &irql);
    RtlCopyMemory(used, g_Live, sizeof(used));
    KeReleaseSpinLock(&g_LiveLock, irql);

    /* Our own key is one of the PortNames counted: only a complete scan
       shows that no other devnode names the same number. */
    if (complete && mine && !(used[mine >> 3] & (1u << (mine & 7))) && count[mine] <= 1)
        n = mine;
    if (n == 0 && complete) {
        for (i = 1; i <= PC_PORT_MAX; i++)
            if (count[i])
                used[i >> 3] |= (UCHAR)(1u << (i & 7));
        n = PcLowestFree(used);
        if (n && !NT_SUCCESS(UpWritePortName(d, n)))
            n = 0;
    }
    if (n) {
        KeAcquireSpinLock(&g_LiveLock, &irql);
        g_Live[n >> 3] |= (UCHAR)(1u << (n & 7));
        KeReleaseSpinLock(&g_LiveLock, irql);
    }
    UpUnlock(&g_AssignLock);
    UPT2("port number (assigned, remembered)", n, mine);
    if (!complete)
        UPT0("Enum scan incomplete");
    return n;
}

static void UpFreePort(UP_DEV *d)
{
    KIRQL irql;

    if (!d->PortHeld)
        return;
    d->PortHeld = FALSE;
    KeAcquireSpinLock(&g_LiveLock, &irql);
    g_Live[d->PortNumber >> 3] &= (UCHAR)~(1u << (d->PortNumber & 7));
    KeReleaseSpinLock(&g_LiveLock, irql);
}

static NTSTATUS UpMakeLink(UP_DEV *d)
{
    UNICODE_STRING link, dev;
    WCHAR lb[32], db[32];
    NTSTATUS st;

    UpNameN(&link, lb, 32, "\\DosDevices\\USBPRN", d->PortNumber);
    UpNameN(&dev, db, 32, "\\Device\\UsbPrnF", d->Tag);
    st = IoCreateSymbolicLink(&link, &dev);
    d->LinkMade = NT_SUCCESS(st);
    UPT2("link USBPRN<n> (n, status)", d->PortNumber, st);
    return st;
}

static void UpDropLink(UP_DEV *d)
{
    UNICODE_STRING link;
    WCHAR lb[32];

    if (!d->LinkMade)
        return;
    UpNameN(&link, lb, 32, "\\DosDevices\\USBPRN", d->PortNumber);
    IoDeleteSymbolicLink(&link);
    d->LinkMade = FALSE;
}

/* ---- start and stop ------------------------------------------------------- */

#define UP_IFACE_SIZE(n) (FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) + \
                          (n) * sizeof(USBD_PIPE_INFORMATION))

/* SELECT_CONFIGURATION with the printer interface alone, at its chosen
   alternate; every pipe asks for xfer bytes per transfer. */
static NTSTATUS UpSelect(UP_DEV *d, ULONG xfer)
{
    PUSBD_INTERFACE_INFORMATION info;
    PURB urb;
    ULONG neps = d->Alt.NumEps, size, j;
    NTSTATUS st;

    size = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface) + UP_IFACE_SIZE(neps);
    urb = (PURB)ExAllocatePoolWithTag(NonPagedPool, size, UP_TAG);
    if (urb == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, size);
    UsbBuildSelectConfigurationRequest(urb, (USHORT)size, d->Config);
    info = &urb->UrbSelectConfiguration.Interface;
    info->Length = (USHORT)UP_IFACE_SIZE(neps);
    info->InterfaceNumber = d->Alt.Interface;
    info->AlternateSetting = d->Alt.Alternate;
    info->NumberOfPipes = neps;
    for (j = 0; j < neps; j++)
        info->Pipes[j].MaximumTransferSize = xfer;
    st = UpSubmitSync(d, urb);
    UPT2("select configuration (status, transfer size)", st, xfer);
    if (!NT_SUCCESS(st)) {
        ExFreePool(urb);
        return st;
    }
    d->OutPipe = d->InPipe = NULL;
    for (j = 0; j < info->NumberOfPipes && j < neps; j++) {
        if (info->Pipes[j].EndpointAddress == d->Alt.OutEp) {
            d->OutPipe = info->Pipes[j].PipeHandle;
            d->OutXfer = info->Pipes[j].MaximumTransferSize;
        } else if (d->Alt.InEp && info->Pipes[j].EndpointAddress == d->Alt.InEp) {
            d->InPipe = info->Pipes[j].PipeHandle;
            d->InXfer = info->Pipes[j].MaximumTransferSize;
        }
    }
    if (d->Select)
        ExFreePool(d->Select);
    d->Select = urb;
    if (d->OutPipe == NULL)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS UpStartDevice(UP_DEV *d)
{
    USB_CONFIGURATION_DESCRIPTOR head;
    ULONG total, got = 0;
    PUCHAR id;
    NTSTATUS st;
    int r;

    st = UpGetDescriptor(d, USB_DEVICE_DESCRIPTOR_TYPE, &d->DevDesc, sizeof(d->DevDesc), &got);
    if (NT_SUCCESS(st) && got < sizeof(d->DevDesc))
        st = STATUS_DEVICE_DATA_ERROR;
    if (!NT_SUCCESS(st))
        return st;
    RtlZeroMemory(&head, sizeof(head));
    st = UpGetDescriptor(d, USB_CONFIGURATION_DESCRIPTOR_TYPE, &head, sizeof(head), &got);
    if (!NT_SUCCESS(st))
        return st;
    total = head.wTotalLength;
    if (got < sizeof(head) || total < sizeof(head) || total > UP_CFG_MAX)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (d->Config)
        ExFreePool(d->Config);
    d->Config = (PUSB_CONFIGURATION_DESCRIPTOR)ExAllocatePoolWithTag(NonPagedPool, total, UP_TAG);
    if (d->Config == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(d->Config, total);
    st = UpGetDescriptor(d, USB_CONFIGURATION_DESCRIPTOR_TYPE, d->Config, total, &got);
    if (!NT_SUCCESS(st))
        return st;
    /* The whole descriptor, as long as the first read said, or nothing. */
    if (got != total || d->Config->wTotalLength != total)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    d->ConfigLength = total;

    r = PcSelectAlt((const PC_U8 *)d->Config, d->ConfigLength,
                    PcQuirks(d->DevDesc.idVendor, d->DevDesc.idProduct), &d->Alt);
    UPT2("device (vid:pid, alternate choice)",
         ((ULONG)d->DevDesc.idVendor << 16) | d->DevDesc.idProduct, r);
    UPT2("interface/alternate/protocol, out/in endpoints",
         (d->Alt.Interface << 16) | (d->Alt.Alternate << 8) | d->Alt.Protocol,
         (d->Alt.OutEp << 8) | d->Alt.InEp);
    if (r <= 0)
        return STATUS_DEVICE_CONFIGURATION_ERROR;   /* no printer interface: Code 10 */

    st = UpSelect(d, UP_XFER_WANT);
    if (!NT_SUCCESS(st) && st != STATUS_INSUFFICIENT_RESOURCES)
        st = UpSelect(d, USBD_DEFAULT_MAXIMUM_TRANSFER_SIZE);
    if (!NT_SUCCESS(st))
        return st;
    if (d->OutXfer == 0)
        d->OutXfer = USBD_DEFAULT_MAXIMUM_TRANSFER_SIZE;
    if (d->InXfer == 0)
        d->InXfer = USBD_DEFAULT_MAXIMUM_TRANSFER_SIZE;

    /* The id for the description; a printer that does not answer still prints. */
    d->Starting = TRUE;
    d->Description[0] = 0;
    id = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, PC_ID_MAX + 2, UP_TAG);
    if (id) {
        if (NT_SUCCESS(UpReadId(d, id, PC_ID_MAX + 2, &got))) {
            ULONG n = PcIdLength(id, got);
            PcIdDescribe((const char *)id + 2, n, d->Description, sizeof(d->Description));
        }
        ExFreePool(id);
    }
    if (d->Description[0] == 0)
        PcIdDescribe("", 0, d->Description, sizeof(d->Description));
    (void)UpReadStatus(d, NULL);
    d->Starting = FALSE;

    d->PortNumber = UpAssignPort(d);
    if (d->PortNumber == 0)
        return STATUS_INSUFFICIENT_RESOURCES;       /* no number could be given or kept */
    d->PortHeld = TRUE;
    st = UpMakeLink(d);
    if (!NT_SUCCESS(st)) {
        UpFreePort(d);
        return st;
    }
    d->Started = TRUE;
    return STATUS_SUCCESS;
}

/* Ends I/O in flight: both pipes aborted (the waits see the state). */
static void UpAbortPipes(UP_DEV *d)
{
    (void)UpPipeRequest(d, d->OutPipe, URB_FUNCTION_ABORT_PIPE);
    (void)UpPipeRequest(d, d->InPipe, URB_FUNCTION_ABORT_PIPE);
}

/*
 * After Started or Suspended was changed: aborts the pipes until the write
 * and read in progress (if any) have returned, so none is left using the
 * pipes. Each user checks the state after taking its lock and before every
 * transfer, so no new one starts.
 */
static void UpQuiesce(UP_DEV *d)
{
    if (d->OutPipe == NULL)
        return;
    for (;;) {
        UpAbortPipes(d);
        if (UpTryLock(&d->WriteLock, UP_QUIESCE_MS))
            break;
    }
    for (;;) {
        UpAbortPipes(d);
        if (UpTryLock(&d->ReadLock, UP_QUIESCE_MS))
            break;
    }
    UpLock(&d->CtrlLock);           /* a class request ends within UP_CTRL_MS */
    UpUnlock(&d->CtrlLock);
    UpUnlock(&d->ReadLock);
    UpUnlock(&d->WriteLock);
}

static void UpStopDevice(UP_DEV *d)
{
    d->Started = FALSE;
    UpDropLink(d);
    UpQuiesce(d);
    UpFreePort(d);
}

/* ---- DriverEntry / AddDevice ---------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path)
{
    UNREFERENCED_PARAMETER(path);
    UPT0("DriverEntry");
    KeInitializeSpinLock(&g_LiveLock);
    KeInitializeEvent(&g_AssignLock, SynchronizationEvent, TRUE);
    drv->MajorFunction[IRP_MJ_CREATE] = UpDispatch;
    drv->MajorFunction[IRP_MJ_CLOSE] = UpDispatch;
    drv->MajorFunction[IRP_MJ_CLEANUP] = UpDispatch;
    drv->MajorFunction[IRP_MJ_READ] = UpDispatch;
    drv->MajorFunction[IRP_MJ_WRITE] = UpDispatch;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = UpDispatch;
    drv->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = UpDispatch;
    drv->MajorFunction[IRP_MJ_PNP] = UpDispatchPnp;
    drv->MajorFunction[IRP_MJ_POWER] = UpDispatchPower;
    drv->DriverExtension->AddDevice = UpAddDevice;
    drv->DriverUnload = UpUnload;
    return STATUS_SUCCESS;
}

static VOID UpUnload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
}

static NTSTATUS UpAddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT dev;
    UNICODE_STRING name;
    WCHAR nb[32];
    UP_DEV *d;
    ULONG tag = (ULONG)InterlockedIncrement(&g_TagCounter);
    NTSTATUS st;

    UpNameN(&name, nb, 32, "\\Device\\UsbPrnF", tag);
    st = IoCreateDevice(drv, sizeof(UP_DEV), &name, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
    if (!NT_SUCCESS(st))
        return st;
    d = (UP_DEV *)dev->DeviceExtension;
    RtlZeroMemory(d, sizeof(*d));
    d->Self = dev;
    d->Pdo = pdo;
    d->Tag = tag;
    d->IoCount = 1;
    d->ReadMs = UP_READ_DEFAULT_MS;
    KeInitializeEvent(&d->RemoveEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&d->WriteLock, SynchronizationEvent, TRUE);
    KeInitializeEvent(&d->ReadLock, SynchronizationEvent, TRUE);
    KeInitializeEvent(&d->CtrlLock, SynchronizationEvent, TRUE);
    KeInitializeSpinLock(&d->Lock);
    d->Lower = IoAttachDeviceToDeviceStack(dev, pdo);
    if (d->Lower == NULL) {
        IoDeleteDevice(dev);
        return STATUS_NO_SUCH_DEVICE;
    }
    UPT2("AddDevice (PDO, tag)", pdo, tag);
    dev->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/* ---- write and read (design 5.1) ------------------------------------------- */

/* Why I/O cannot go on, as a status; STATUS_SUCCESS when it can. */
static NTSTATUS UpWhyNot(UP_DEV *d)
{
    if (d->Removed)
        return STATUS_DEVICE_NOT_CONNECTED;
    if (d->Suspended)
        return STATUS_CANCELLED;
    if (!d->Started)
        return STATUS_DEVICE_NOT_READY;
    return STATUS_SUCCESS;
}

/*
 * Sends the buffer on bulk OUT. Each transfer is waited for in slices of
 * UP_SLICE_MS, reading GET_PORT_STATUS between them. A transfer that has
 * not completed when the write timeout has passed since it was sent is
 * given up and the write returns short (success,
 * fewer bytes; the monitor reads why with GET_INFO). Removal returns
 * STATUS_DEVICE_NOT_CONNECTED, sleep or the caller's cancel
 * STATUS_CANCELLED (the cancel is seen within one slice), a stop
 * STATUS_DEVICE_NOT_READY, a failed transfer short after a pipe reset; in
 * every case with the bytes the stack reports moved.
 */
static NTSTATUS UpWrite(UP_DEV *d, PIRP caller, PUCHAR buf, ULONG len, ULONG *done)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *urb;
    NTSTATUS st = STATUS_SUCCESS;
    BOOLEAN shortWrite = FALSE;
    ULONG limit = d->WriteMs;           /* a snapshot: SET_TIMEOUTS may change it */
    ULONGLONG start;

    *done = 0;
    urb = (struct _URB_BULK_OR_INTERRUPT_TRANSFER *)
          ExAllocatePoolWithTag(NonPagedPool, sizeof(*urb), UP_TAG);
    if (urb == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    while (*done < len) {
        ULONG n = PcChunk(len - *done, d->OutXfer), moved;
        ULONGLONG waited;
        BOOLEAN gaveUp = FALSE;
        NTSTATUS xs;
        UP_REQ r;

        st = UpWhyNot(d);
        if (NT_SUCCESS(st) && caller->Cancel)
            st = STATUS_CANCELLED;
        if (!NT_SUCCESS(st))
            break;
        RtlZeroMemory(urb, sizeof(*urb));
        UsbBuildInterruptOrBulkTransferRequest((PURB)urb, sizeof(*urb), d->OutPipe,
                                               buf + *done, NULL, n, 0, NULL);
        start = UpNow();
        st = UpBegin(d, &r, (PURB)urb);
        if (st != STATUS_PENDING)
            break;
        for (;;) {
            waited = UpElapsedMs(start);
            /* Past the deadline, still look once: it may have completed
               while the status was read. */
            if (UpWait(&r, (limit && waited >= limit) ? 1 : UpSlice(waited, limit)))
                break;
            waited = UpElapsedMs(start);
            if (!UpUsable(d) || caller->Cancel || (limit && waited >= limit)) {
                gaveUp = TRUE;
                UpGiveUp(d, &r, d->OutPipe);
                break;
            }
            /* The printer still takes nothing: read why (design 5.1, 7.1);
               not when the caller has cancelled, and check again after
               (a status read takes up to UP_CTRL_MS). */
            if (!caller->Cancel)
                (void)UpReadStatus(d, NULL);
            if (caller->Cancel) {
                gaveUp = TRUE;
                UpGiveUp(d, &r, d->OutPipe);
                break;
            }
        }
        xs = UpEnd(&r);
        /* What the stack reports moved, also for a failed or aborted transfer. */
        moved = urb->TransferBufferLength;
        if (!NT_SUCCESS(xs) && moved > n)
            moved = 0;
        *done += moved < n ? moved : n;
        st = UpWhyNot(d);
        if (NT_SUCCESS(st) && caller->Cancel)
            st = STATUS_CANCELLED;
        if (!NT_SUCCESS(st))
            break;
        if (gaveUp) {
            UPT2("write timed out (moved, status)", *done, d->LastStatus);
            (void)UpReadStatus(d, NULL);
            shortWrite = TRUE;
            break;
        }
        if (!NT_SUCCESS(xs)) {
            UPT2("write failed (status, moved)", xs, *done);
            (void)UpPipeRequest(d, d->OutPipe, URB_FUNCTION_RESET_PIPE);
            (void)UpReadStatus(d, NULL);
            shortWrite = TRUE;
            break;
        }
    }
    ExFreePool(urb);
    d->LastWriteBytes = *done;
    d->LastWriteShort = shortWrite;
    return st;
}

/* One bulk IN transfer, given up after the read timeout (0 bytes then). */
static NTSTATUS UpRead(UP_DEV *d, PIRP caller, PUCHAR buf, ULONG len, ULONG *done)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *urb;
    ULONG n = PcChunk(len, d->InXfer), ms = d->ReadMs ? d->ReadMs : UP_READ_DEFAULT_MS;
    BOOLEAN gaveUp = FALSE;
    UP_REQ r;
    NTSTATUS st, xs;

    *done = 0;
    st = UpWhyNot(d);
    if (!NT_SUCCESS(st))
        return st;
    UPT2("read (bytes, timeout ms)", n, ms);
    urb = (struct _URB_BULK_OR_INTERRUPT_TRANSFER *)
          ExAllocatePoolWithTag(NonPagedPool, sizeof(*urb), UP_TAG);
    if (urb == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(urb, sizeof(*urb));
    UsbBuildInterruptOrBulkTransferRequest((PURB)urb, sizeof(*urb), d->InPipe, buf, NULL, n,
                                           USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK,
                                           NULL);
    st = UpBegin(d, &r, (PURB)urb);
    if (st == STATUS_PENDING) {
        ULONGLONG start = UpNow(), waited;
        for (;;) {
            waited = UpElapsedMs(start);
            if (UpWait(&r, waited >= ms ? 1 : UpSlice(waited, ms)))
                break;
            waited = UpElapsedMs(start);
            if (!UpUsable(d) || caller->Cancel || waited >= ms) {
                gaveUp = TRUE;
                UpGiveUp(d, &r, d->InPipe);
                break;
            }
        }
        xs = UpEnd(&r);
        if (urb->TransferBufferLength <= n)
            *done = urb->TransferBufferLength;
        st = UpWhyNot(d);
        if (NT_SUCCESS(st) && caller->Cancel)
            st = STATUS_CANCELLED;
        if (NT_SUCCESS(st) && !NT_SUCCESS(xs) && !gaveUp) {
            (void)UpPipeRequest(d, d->InPipe, URB_FUNCTION_RESET_PIPE);
            st = xs;
            *done = 0;
        }
        /* A read given up after its timeout is 0 bytes (or what came). */
        UPT2("read done (status, bytes)", st, *done);
    }
    ExFreePool(urb);
    return st;
}

/* ---- dispatch ---------------------------------------------------------------- */

static void UpFillInfo(UP_DEV *d, UP_INFO *o)
{
    ULONG i;

    RtlZeroMemory(o, sizeof(*o));
    o->Version = UP_INFO_VERSION;
    if (d->Started && !d->Removed)
        o->Flags |= UP_F_PRESENT;
    if (d->InPipe)
        o->Flags |= UP_F_BIDI;
    if (d->Alt.Protocol == PC_PROTO_1284_4)
        o->Flags |= UP_F_1284_4;
    if (d->StatusValid)
        o->Flags |= UP_F_STATUS_VALID;
    if (d->LastWriteShort)
        o->Flags |= UP_F_WRITE_SHORT;
    if (d->ResetOther)
        o->Flags |= UP_F_RESET_OTHER;
    o->Vid = d->DevDesc.idVendor;
    o->Pid = d->DevDesc.idProduct;
    o->Interface = d->Alt.Interface;
    o->Alternate = d->Alt.Alternate;
    o->Protocol = d->Alt.Protocol;
    o->LastStatus = d->LastStatus;
    o->PortNumber = d->PortNumber;
    o->LastWriteBytes = d->LastWriteBytes;
    o->WriteMs = d->WriteMs;
    o->ReadMs = d->ReadMs;
    for (i = 0; i + 1 < sizeof(o->Description) && d->Description[i]; i++)
        o->Description[i] = d->Description[i];
}

static NTSTATUS UpIoctl(UP_DEV *d, PIRP irp, ULONG *info)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = s->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = s->Parameters.DeviceIoControl.OutputBufferLength;
    PUCHAR sys = (PUCHAR)irp->AssociatedIrp.SystemBuffer;
    NTSTATUS st;
    ULONG got;

    *info = 0;
    switch (code) {
    case IOCTL_USBPRINT_GET_LPT_STATUS:
        if (outLen < 1)
            return STATUS_BUFFER_TOO_SMALL;
        st = UpReadStatus(d, sys);
        if (NT_SUCCESS(st))
            *info = 1;
        return st;

    case IOCTL_USBPRINT_GET_1284_ID:
        if (outLen < 2)
            return STATUS_BUFFER_TOO_SMALL;
        st = UpReadId(d, sys, outLen < PC_ID_MAX + 2 ? outLen : PC_ID_MAX + 2, &got);
        if (NT_SUCCESS(st))
            *info = got;
        return st;

    case IOCTL_USBPRINT_SOFT_RESET:
        return UpSoftReset(d);

    case IOCTL_USBPRINT_VENDOR_SET_COMMAND:
    case IOCTL_USBPRINT_VENDOR_GET_COMMAND:
        return STATUS_NOT_SUPPORTED;

    case IOCTL_USBPRN_SET_TIMEOUTS:
        if (inLen < sizeof(UP_TIMEOUTS))
            return STATUS_BUFFER_TOO_SMALL;
        d->WriteMs = ((UP_TIMEOUTS *)sys)->WriteMs;
        d->ReadMs = ((UP_TIMEOUTS *)sys)->ReadMs;
        return STATUS_SUCCESS;

    case IOCTL_USBPRN_GET_INFO:
        if (outLen < sizeof(UP_INFO))
            return STATUS_BUFFER_TOO_SMALL;
        UpFillInfo(d, (UP_INFO *)sys);
        *info = sizeof(UP_INFO);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* Whether a create asks to write (FILE_WRITE_DATA); FALSE when unknown. */
static BOOLEAN UpWantsWrite(PIO_STACK_LOCATION s)
{
    if (s->Parameters.Create.SecurityContext == NULL)
        return FALSE;
    return (s->Parameters.Create.SecurityContext->DesiredAccess & FILE_WRITE_DATA) != 0;
}

static NTSTATUS UpDispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    UP_DEV *d = (UP_DEV *)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG info = 0;
    NTSTATUS st;
    KIRQL irql;

    switch (s->MajorFunction) {
    case IRP_MJ_CREATE: {
        /* Admission and the count under the lock removal takes (design 8). */
        BOOLEAN writer = UpWantsWrite(s);
        KeAcquireSpinLock(&d->Lock, &irql);
        if (d->Removed || !d->Started) {
            KeReleaseSpinLock(&d->Lock, irql);
            return UpComplete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
        }
        if (writer && d->Writers) {
            KeReleaseSpinLock(&d->Lock, irql);
            return UpComplete(irp, STATUS_SHARING_VIOLATION, 0);
        }
        if (writer)
            d->Writers++;
        d->OpenCount++;
        KeReleaseSpinLock(&d->Lock, irql);
        if (s->FileObject)
            s->FileObject->FsContext2 = (PVOID)(ULONG)(writer ? 1 : 0);
        UPT2("create (writer, opens)", writer, d->OpenCount);
        return UpComplete(irp, STATUS_SUCCESS, 0);
    }

    case IRP_MJ_CLEANUP:
        return UpComplete(irp, STATUS_SUCCESS, 0);

    case IRP_MJ_CLOSE: {
        /* Runs after removal too: touches nothing below this FDO. */
        BOOLEAN last;
        ULONG tag = d->Tag;
        KeAcquireSpinLock(&d->Lock, &irql);
        if (s->FileObject && s->FileObject->FsContext2 && d->Writers)
            d->Writers--;
        if (d->OpenCount)
            d->OpenCount--;
        last = d->OpenCount == 0 && d->DeletePending;
        KeReleaseSpinLock(&d->Lock, irql);
        UPT2("close (tag, last after removal)", tag, last);
        UpComplete(irp, STATUS_SUCCESS, 0);
        if (last) {
            UPT1("last close after removal: FDO deleted", tag);
            IoDeleteDevice(dev);
        }
        (void)tag;
        return STATUS_SUCCESS;
    }

    case IRP_MJ_SYSTEM_CONTROL:
        if (!UpAcquire(d))
            return UpComplete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
        IoSkipCurrentIrpStackLocation(irp);
        st = IoCallDriver(d->Lower, irp);
        UpRelease(d);
        return st;
    }

    if (!UpAcquire(d))
        return UpComplete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
    if (!d->Started) {
        UpRelease(d);
        return UpComplete(irp, STATUS_DEVICE_NOT_READY, 0);
    }

    switch (s->MajorFunction) {
    case IRP_MJ_WRITE: {
        ULONG done = 0;
        if (d->Alt.Protocol == PC_PROTO_1284_4) {
            st = STATUS_INVALID_DEVICE_REQUEST;     /* 1284.4 only: no raw data (design 5.1) */
            break;
        }
        if (!UpLockFor(&d->WriteLock, irp)) {
            st = STATUS_CANCELLED;
            break;
        }
        st = UpWrite(d, irp, (PUCHAR)irp->AssociatedIrp.SystemBuffer, s->Parameters.Write.Length, &done);
        UpUnlock(&d->WriteLock);
        info = done;
        break;
    }
    case IRP_MJ_READ: {
        ULONG done = 0;
        if (d->InPipe == NULL) {
            st = STATUS_INVALID_DEVICE_REQUEST;     /* unidirectional */
            break;
        }
        if (!UpLockFor(&d->ReadLock, irp)) {
            st = STATUS_CANCELLED;
            break;
        }
        st = UpRead(d, irp, (PUCHAR)irp->AssociatedIrp.SystemBuffer, s->Parameters.Read.Length, &done);
        UpUnlock(&d->ReadLock);
        info = done;
        break;
    }
    case IRP_MJ_DEVICE_CONTROL:
        st = UpIoctl(d, irp, &info);
        break;
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    UpRelease(d);
    return UpComplete(irp, st, info);
}

/* ---- PnP --------------------------------------------------------------------- */

static void UpMarkRemoved(UP_DEV *d)
{
    KIRQL irql;

    KeAcquireSpinLock(&d->Lock, &irql);
    d->Removed = TRUE;
    KeReleaseSpinLock(&d->Lock, irql);
}

static NTSTATUS UpRemove(UP_DEV *d, PIRP irp)
{
    PDEVICE_OBJECT self = d->Self;
    ULONG tag = d->Tag;
    BOOLEAN del;
    NTSTATUS st;
    KIRQL irql;

    UPT1("remove: start (tag)", tag);
    UpMarkRemoved(d);
    UpStopDevice(d);
    UPT0("remove: stopped, waiting for I/O");
    /* AddDevice's reference and this IRP's own; then wait for I/O in flight. */
    UpRelease(d);
    UpRelease(d);
    KeWaitForSingleObject(&d->RemoveEvent, Executive, KernelMode, FALSE, NULL);

    irp->IoStatus.Status = STATUS_SUCCESS;
    IoSkipCurrentIrpStackLocation(irp);
    st = IoCallDriver(d->Lower, irp);
    IoDetachDevice(d->Lower);
    if (d->Select) {
        ExFreePool(d->Select);
        d->Select = NULL;
    }
    if (d->Config) {
        ExFreePool(d->Config);
        d->Config = NULL;
    }
    /* With a handle still open, the FDO goes at its last close (design 8). */
    KeAcquireSpinLock(&d->Lock, &irql);
    del = d->OpenCount == 0;
    if (!del)
        d->DeletePending = TRUE;
    KeReleaseSpinLock(&d->Lock, irql);
    UPT2("removed (tag, deleted now)", tag, del);
    if (del)
        IoDeleteDevice(self);
    (void)tag;
    return st;
}

static NTSTATUS UpDispatchPnp(PDEVICE_OBJECT dev, PIRP irp)
{
    UP_DEV *d = (UP_DEV *)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st;

    if (s->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        /* Also after a surprise removal, which already set Removed. */
        InterlockedIncrement(&d->IoCount);
        return UpRemove(d, irp);
    }
    if (!UpAcquire(d))
        return UpComplete(irp, STATUS_DELETE_PENDING, 0);
    UPT1("PnP minor", s->MinorFunction);

    switch (s->MinorFunction) {
    case IRP_MN_START_DEVICE:
        st = UpCallLowerSync(d, irp);
        if (NT_SUCCESS(st))
            st = UpStartDevice(d);
        UPT2("start (status, port number)", st, d->PortNumber);
        UpRelease(d);
        return UpComplete(irp, st, 0);

    case IRP_MN_SURPRISE_REMOVAL:
        UpMarkRemoved(d);
        UpStopDevice(d);
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_STOP_DEVICE:
        UpStopDevice(d);
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
    st = IoCallDriver(d->Lower, irp);
    UpRelease(d);
    return st;
}

/* ---- power (design 5.2) --------------------------------------------------- */

/*
 * The FDO is the power policy owner, as usbcom's: a sleep S-IRP is turned
 * into D3 before it goes down; S0 into D0 after it comes back, and the
 * S-IRP completes when D0 has. At D3 (in a work item, off the power
 * dispatch path) the pipes are aborted and I/O drained before the D-IRP
 * goes down, so a write in progress returns (STATUS_CANCELLED) and the
 * spooler offers a retry; I/O is admitted again only when a D0 IRP has
 * completed successfully. No selective suspend, no remote wakeup.
 */
static NTSTATUS UpPowerUpDone(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UP_DEV *d = (UP_DEV *)ctx;

    UNREFERENCED_PARAMETER(dev);
    if (irp->PendingReturned)
        IoMarkIrpPending(irp);
    if (NT_SUCCESS(irp->IoStatus.Status))
        d->Suspended = FALSE;
    UPT1("D0 done", irp->IoStatus.Status);
    PoStartNextPowerIrp(irp);
    UpRelease(d);
    return STATUS_SUCCESS;
}

/* The D0 IRP requested for S0 has completed: complete the S-IRP. */
static VOID UpWakeDevDone(PDEVICE_OBJECT dev, UCHAR minor, POWER_STATE state,
                          PVOID ctx, PIO_STATUS_BLOCK iosb)
{
    PIRP sirp = (PIRP)ctx;
    UP_DEV *d = (UP_DEV *)IoGetCurrentIrpStackLocation(sirp)->DeviceObject->DeviceExtension;

    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(minor);
    UNREFERENCED_PARAMETER(state);
    UPT1("D0 for S0 (status)", iosb->Status);
    if (!NT_SUCCESS(iosb->Status))
        sirp->IoStatus.Status = iosb->Status;
    PoStartNextPowerIrp(sirp);
    IoCompleteRequest(sirp, IO_NO_INCREMENT);
    UpRelease(d);
}

static NTSTATUS UpSystemUpDone(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UP_DEV *d = (UP_DEV *)ctx;
    POWER_STATE ps;

    UNREFERENCED_PARAMETER(dev);
    ps.DeviceState = PowerDeviceD0;
    if (NT_SUCCESS(irp->IoStatus.Status) && !d->Removed &&
        NT_SUCCESS(PoRequestPowerIrp(d->Pdo, IRP_MN_SET_POWER, ps, UpWakeDevDone, irp, NULL)))
        return STATUS_MORE_PROCESSING_REQUIRED;     /* completed in UpWakeDevDone */
    /* No D0 IRP: I/O stays refused (STATUS_CANCELLED) until a D0 succeeds. */
    UPT0("no D0 request after S0");
    if (irp->PendingReturned)
        IoMarkIrpPending(irp);
    PoStartNextPowerIrp(irp);
    UpRelease(d);
    return STATUS_SUCCESS;
}

static VOID UpSleepDevDone(PDEVICE_OBJECT dev, UCHAR minor, POWER_STATE state,
                           PVOID ctx, PIO_STATUS_BLOCK iosb)
{
    PIRP sirp = (PIRP)ctx;
    UP_DEV *d = (UP_DEV *)IoGetCurrentIrpStackLocation(sirp)->DeviceObject->DeviceExtension;

    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(minor);
    UNREFERENCED_PARAMETER(state);
    UNREFERENCED_PARAMETER(iosb);
    PoStartNextPowerIrp(sirp);
    IoSkipCurrentIrpStackLocation(sirp);
    PoCallDriver(d->Lower, sirp);
    UpRelease(d);
}

/* D3 (or a sleep S-IRP with no D3 IRP), off the power dispatch path:
   drain I/O, then send the IRP down. */
static VOID UpD3Work(PVOID ctx)
{
    UP_DEV *d = (UP_DEV *)ctx;
    PIRP irp = d->PowerIrp;

    d->PowerIrp = NULL;
    if (d->Started)
        UpQuiesce(d);
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    PoCallDriver(d->Lower, irp);
    UpRelease(d);
}

static NTSTATUS UpDispatchPower(PDEVICE_OBJECT dev, PIRP irp)
{
    UP_DEV *d = (UP_DEV *)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st;

    if (!UpAcquire(d)) {
        PoStartNextPowerIrp(irp);
        return UpComplete(irp, STATUS_DELETE_PENDING, 0);
    }
    if (s->MinorFunction == IRP_MN_SET_POWER && s->Parameters.Power.Type == SystemPowerState) {
        UPT1("set system power", s->Parameters.Power.State.SystemState);
        if (s->Parameters.Power.State.SystemState == PowerSystemWorking) {
            IoMarkIrpPending(irp);
            IoCopyCurrentIrpStackLocationToNext(irp);
            IoSetCompletionRoutine(irp, UpSystemUpDone, d, TRUE, TRUE, TRUE);
            PoCallDriver(d->Lower, irp);            /* released when the S-IRP completes */
            return STATUS_PENDING;
        } else {
            POWER_STATE ps;
            ps.DeviceState = PowerDeviceD3;
            IoMarkIrpPending(irp);
            if (NT_SUCCESS(PoRequestPowerIrp(d->Pdo, IRP_MN_SET_POWER, ps,
                                             UpSleepDevDone, irp, NULL)))
                return STATUS_PENDING;              /* released in UpSleepDevDone */
            /* No D3 IRP: refuse and drain I/O here, then send the S-IRP on. */
            d->Suspended = TRUE;
            d->PowerIrp = irp;
            ExInitializeWorkItem(&d->PowerWork, UpD3Work, d);
            ExQueueWorkItem(&d->PowerWork, DelayedWorkQueue);
            return STATUS_PENDING;                  /* released in UpD3Work */
        }
    }
    if (s->MinorFunction == IRP_MN_SET_POWER && s->Parameters.Power.Type == DevicePowerState) {
        UPT1("set device power", s->Parameters.Power.State.DeviceState);
        if (s->Parameters.Power.State.DeviceState == PowerDeviceD0) {
            IoCopyCurrentIrpStackLocationToNext(irp);
            IoSetCompletionRoutine(irp, UpPowerUpDone, d, TRUE, TRUE, TRUE);
            return PoCallDriver(d->Lower, irp);     /* released in UpPowerUpDone */
        }
        d->Suspended = TRUE;
        IoMarkIrpPending(irp);
        d->PowerIrp = irp;
        ExInitializeWorkItem(&d->PowerWork, UpD3Work, d);
        ExQueueWorkItem(&d->PowerWork, DelayedWorkQueue);
        return STATUS_PENDING;                      /* released in UpD3Work */
    }
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    st = PoCallDriver(d->Lower, irp);
    UpRelease(d);
    return st;
}

/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * The fake operating system of the core tests (fakeos.h). Host-test code
 * only. Identical copy in both packages.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "fakeos.h"
#include "fakewin.h"

#define MAGIC_EVENT  0x45564e54UL
#define MAGIC_THREAD 0x54485244UL
#define MAGIC_LOCK   0x4c4f434bUL
#define MAGIC_IRP    0x49525020UL
#define MAGIC_PACKET 0x50414b54UL
#define MAGIC_FREED  0xdeadbeefUL

FAKE Fake;
void (*FakeSubmit)(PDEVICE_OBJECT, PIRP, PURB);
BOOLEAN (*FakeCancel)(PIRP);
void (*FakeReceive)(PNDIS_PACKET);
void (*FakeSendComplete)(PNDIS_PACKET, NDIS_STATUS);
void (*FakeStatus)(NDIS_STATUS);
void (*FakeSetComplete)(NDIS_STATUS);
void (*FakeResetComplete)(NDIS_STATUS);

static __declspec(thread) long held;
static __declspec(thread) long depth;
static void *account;

static void Account(long *counter, long delta)
{
    if (!account) account = FwLockNew();
    FwLock(account);
    *counter += delta;
    FwUnlock(account);
}

void FakeViolation(const char *what)
{
    Account(&Fake.Violations, 1);
    printf("VIOLATION: %s\n", what);
}

long FakeHeld(void) { return held; }

static void NoLock(const char *where)
{
    if (held) {
        char text[128];
        sprintf(text, "%s with %ld spin lock(s) held", where, held);
        FakeViolation(text);
    }
}

/* ---------------- kernel ---------------- */

PIRP IoAllocateIrp(char stackSize, BOOLEAN charge)
{
    PIRP irp = calloc(1, sizeof(IRP));
    (void)charge;
    if (stackSize < 1) FakeViolation("IoAllocateIrp with no stack location");
    irp->FakeMagic = MAGIC_IRP;
    Account(&Fake.Irps, 1);
    return irp;
}

void IoFreeIrp(PIRP irp)
{
    if (irp->FakeMagic != MAGIC_IRP) { FakeViolation("IoFreeIrp of a freed or foreign IRP"); return; }
    irp->FakeMagic = MAGIC_FREED;
    Account(&Fake.Irps, -1);
    free(irp);
}

NTSTATUS IofCallDriver(PDEVICE_OBJECT device, PIRP irp)
{
    PIO_STACK_LOCATION s = &irp->Next;
    NoLock("IoCallDriver");
    if (irp->FakeMagic != MAGIC_IRP) { FakeViolation("IoCallDriver with a freed IRP"); return STATUS_INVALID_PARAMETER; }
    if (s->MajorFunction != IRP_MJ_INTERNAL_DEVICE_CONTROL ||
        s->Parameters.DeviceIoControl.IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB ||
        !s->CompletionRoutine) {
        FakeViolation("IoCallDriver: not a URB submission with a completion routine");
        return STATUS_INVALID_PARAMETER;
    }
    irp->FakeDevice = device;
    depth++;
    Account(&Fake.Submissions, 1);
    if (depth > Fake.MaxDepth) Account(&Fake.MaxDepth, depth - Fake.MaxDepth);
    FakeSubmit(device, irp, (PURB)s->Parameters.Others.Argument1);
    depth--;
    return STATUS_PENDING;
}

void FakeComplete(PIRP irp, NTSTATUS status, USBD_STATUS usb)
{
    PURB urb = (PURB)irp->Next.Parameters.Others.Argument1;
    NTSTATUS r;
    urb->UrbHeader.Status = usb;
    irp->IoStatus.Status = status;
    r = irp->Next.CompletionRoutine(irp->FakeDevice, irp, irp->Next.Context);
    if (r != STATUS_MORE_PROCESSING_REQUIRED) FakeViolation("completion routine let the IRP go on");
}

BOOLEAN IoCancelIrp(PIRP irp) { return FakeCancel ? FakeCancel(irp) : FALSE; }
BOOLEAN IoIsWdmVersionAvailable(UCHAR major, UCHAR minor)
{
    return (BOOLEAN)(Fake.Wdm110 && major == 1 && minor <= 0x10);
}

void KeInitializeEvent(PKEVENT e, EVENT_TYPE type, BOOLEAN state)
{
    if (e->Magic == MAGIC_EVENT && e->Handle) FwClose(e->Handle);
    e->Magic = MAGIC_EVENT;
    e->Handle = FwEvent(type == NotificationEvent, state);
}

LONG KeSetEvent(PKEVENT e, LONG increment, BOOLEAN wait)
{
    (void)increment; (void)wait;
    if (e->Magic != MAGIC_EVENT) { FakeViolation("KeSetEvent on an uninitialised event"); return 0; }
    FwSet(e->Handle);
    return 0;
}

void KeClearEvent(PKEVENT e)
{
    if (e->Magic != MAGIC_EVENT) { FakeViolation("KeClearEvent on an uninitialised event"); return; }
    FwReset(e->Handle);
}

static unsigned long Ms(PLARGE_INTEGER t)
{
    LONGLONG v;
    if (!t) return FW_INFINITE;
    v = t->QuadPart;
    if (v >= 0) { FakeViolation("absolute timeout"); return 0; }
    return (unsigned long)((-v + 9999) / 10000);
}

NTSTATUS KeWaitForSingleObject(PVOID object, KWAIT_REASON reason, KPROCESSOR_MODE mode,
                               BOOLEAN alertable, PLARGE_INTEGER timeout)
{
    PKEVENT e = object;           /* events and threads share the header */
    (void)reason; (void)mode; (void)alertable;
    NoLock("KeWaitForSingleObject");
    if (e->Magic != MAGIC_EVENT && e->Magic != MAGIC_THREAD) {
        FakeViolation("wait on an unknown object");
        return STATUS_TIMEOUT;
    }
    return FwWait(e->Handle, Ms(timeout)) ? STATUS_TIMEOUT : STATUS_SUCCESS;
}

NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER interval)
{
    (void)mode; (void)alertable;
    NoLock("KeDelayExecutionThread");
    FwSleep(Ms(interval));
    return STATUS_SUCCESS;
}

void KeInitializeSpinLock(PKSPIN_LOCK l)
{
    l->Magic = MAGIC_LOCK;
    l->Lock = FwLockNew();
    l->Owner = 0;
    Account(&Fake.Locks, 1);
}

void KeAcquireSpinLock(PKSPIN_LOCK l, PKIRQL old)
{
    if (l->Magic != MAGIC_LOCK) { FakeViolation("acquire of an uninitialised spin lock"); return; }
    if (l->Owner == FwThreadId()) FakeViolation("spin lock acquired twice by one thread");
    FwLock(l->Lock);
    l->Owner = FwThreadId();
    held++;
    *old = 2;
}

void KeReleaseSpinLock(PKSPIN_LOCK l, KIRQL old)
{
    (void)old;
    if (l->Owner != FwThreadId()) FakeViolation("spin lock released by a thread that does not hold it");
    l->Owner = 0;
    held--;
    FwUnlock(l->Lock);
}

PVOID ExAllocatePoolWithTag(POOL_TYPE type, ULONG size, ULONG tag)
{
    unsigned char *p = malloc(size + 8);
    (void)type; (void)tag;
    if (!p) return NULL;
    memset(p, 0xcd, size + 8);      /* pool memory is not zeroed */
    *(ULONG *)p = MAGIC_PACKET;
    Account(&Fake.Pool, 1);
    return p + 8;
}

void ExFreePool(PVOID p)
{
    unsigned char *b = (unsigned char *)p - 8;
    if (*(ULONG *)b != MAGIC_PACKET) { FakeViolation("ExFreePool of a freed or foreign block"); return; }
    *(ULONG *)b = MAGIC_FREED;
    Account(&Fake.Pool, -1);
    free(b);
}

typedef struct THREAD { ULONG Magic; void *Handle; } THREAD;

NTSTATUS PsCreateSystemThread(PHANDLE handle, ULONG access, PVOID attributes, HANDLE process,
                              PVOID client, void (*start)(PVOID), PVOID context)
{
    THREAD *t = malloc(sizeof(*t));
    (void)access; (void)attributes; (void)process; (void)client;
    t->Magic = MAGIC_THREAD;
    t->Handle = FwThread(start, context);
    Account(&Fake.Threads, 1);
    *handle = t;
    return STATUS_SUCCESS;
}

NTSTATUS PsTerminateSystemThread(NTSTATUS status)
{
    (void)status;
    NoLock("PsTerminateSystemThread");
    FwExitThread();
    return STATUS_SUCCESS;
}

NTSTATUS ObReferenceObjectByHandle(HANDLE handle, ULONG access, PVOID type, KPROCESSOR_MODE mode,
                                   PVOID *object, PVOID info)
{
    THREAD *h = handle, *t = malloc(sizeof(*t));
    (void)access; (void)type; (void)mode; (void)info;
    t->Magic = MAGIC_THREAD;
    t->Handle = FwDup(h->Handle);
    *object = t;
    return STATUS_SUCCESS;
}

void ObfDereferenceObject(PVOID object)
{
    THREAD *t = object;
    FwClose(t->Handle);
    t->Magic = MAGIC_FREED;
    free(t);
}

NTSTATUS ZwClose(HANDLE handle)
{
    THREAD *t = handle;
    FwClose(t->Handle);
    t->Magic = MAGIC_FREED;
    free(t);
    Account(&Fake.Threads, -1);
    return STATUS_SUCCESS;
}

ULONG DbgPrint(const char *format, ...)
{
    (void)format;
    return 0;
}

/* ---------------- NDIS ---------------- */

typedef struct POOL { ULONG Limit, Used; } POOL;

void NdisQueryPacket(PNDIS_PACKET p, PUINT physical, PUINT bufferCount, PNDIS_BUFFER *first, PUINT total)
{
    PNDIS_BUFFER b;
    UINT n = 0, t = 0;
    if (p->FakeMagic != MAGIC_PACKET) FakeViolation("NdisQueryPacket on a freed packet");
    for (b = p->Head; b; b = b->Next) { n++; t += b->Length; }
    if (physical) *physical = n;
    if (bufferCount) *bufferCount = n;
    if (first) *first = p->Head;
    if (total) *total = t;
}

void NdisChainBufferAtFront(PNDIS_PACKET p, PNDIS_BUFFER b) { b->Next = p->Head; p->Head = b; }

void NdisUnchainBufferAtFront(PNDIS_PACKET p, PNDIS_BUFFER *b)
{
    *b = p->Head;
    if (p->Head) { p->Head = p->Head->Next; (*b)->Next = NULL; }
}

NDIS_STATUS NdisAllocateMemory(PVOID *p, UINT length, UINT flags, NDIS_PHYSICAL_ADDRESS highest)
{
    (void)flags; (void)highest;
    *p = malloc(length);
    if (!*p) return NDIS_STATUS_RESOURCES;
    memset(*p, 0xcd, length);
    Account(&Fake.NdisMemory, 1);
    return NDIS_STATUS_SUCCESS;
}

void NdisFreeMemory(PVOID p, UINT length, UINT flags)
{
    (void)length; (void)flags;
    free(p);
    Account(&Fake.NdisMemory, -1);
}

void NdisAllocatePacketPool(PNDIS_STATUS s, PNDIS_HANDLE pool, UINT count, UINT reserved)
{
    POOL *p = calloc(1, sizeof(*p));
    (void)reserved;
    p->Limit = count;
    *pool = p;
    Account(&Fake.PacketPools, 1);
    *s = NDIS_STATUS_SUCCESS;
}

void NdisFreePacketPool(NDIS_HANDLE pool)
{
    POOL *p = pool;
    if (p->Used) FakeViolation("packet pool freed with packets out");
    free(p);
    Account(&Fake.PacketPools, -1);
}

void NdisAllocateBufferPool(PNDIS_STATUS s, PNDIS_HANDLE pool, UINT count)
{
    POOL *p = calloc(1, sizeof(*p));
    p->Limit = count;
    *pool = p;
    Account(&Fake.BufferPools, 1);
    *s = NDIS_STATUS_SUCCESS;
}

void NdisFreeBufferPool(NDIS_HANDLE pool)
{
    POOL *p = pool;
    if (p->Used) FakeViolation("buffer pool freed with buffers out");
    free(p);
    Account(&Fake.BufferPools, -1);
}

void NdisAllocatePacket(PNDIS_STATUS s, PNDIS_PACKET *packet, NDIS_HANDLE pool)
{
    POOL *p = pool;
    if (p->Used >= p->Limit) { *s = NDIS_STATUS_RESOURCES; *packet = NULL; return; }
    *packet = calloc(1, sizeof(NDIS_PACKET));
    (*packet)->FakeMagic = MAGIC_PACKET;
    (*packet)->Pool = p;
    p->Used++;
    Account(&Fake.Packets, 1);
    *s = NDIS_STATUS_SUCCESS;
}

void NdisFreePacket(PNDIS_PACKET packet)
{
    if (packet->FakeMagic != MAGIC_PACKET) { FakeViolation("NdisFreePacket of a freed packet"); return; }
    packet->FakeMagic = MAGIC_FREED;
    ((POOL *)packet->Pool)->Used--;
    Account(&Fake.Packets, -1);
    free(packet);
}

void NdisAllocateBuffer(PNDIS_STATUS s, PNDIS_BUFFER *b, NDIS_HANDLE pool, PVOID va, UINT length)
{
    POOL *p = pool;
    if (p->Used >= p->Limit) { *s = NDIS_STATUS_RESOURCES; *b = NULL; return; }
    *b = calloc(1, sizeof(NDIS_BUFFER));
    (*b)->Va = va; (*b)->Length = length; (*b)->Pool = p;
    p->Used++;
    Account(&Fake.Buffers, 1);
    *s = NDIS_STATUS_SUCCESS;
}

void NdisFreeBuffer(PNDIS_BUFFER b)
{
    ((POOL *)b->Pool)->Used--;
    Account(&Fake.Buffers, -1);
    free(b);
}

void NdisAllocateSpinLock(PNDIS_SPIN_LOCK l) { KeInitializeSpinLock(&l->SpinLock); }
void NdisFreeSpinLock(PNDIS_SPIN_LOCK l)
{
    FwLockFree(l->SpinLock.Lock);
    l->SpinLock.Magic = MAGIC_FREED;
    Account(&Fake.Locks, -1);
}
void NdisAcquireSpinLock(PNDIS_SPIN_LOCK l) { KeAcquireSpinLock(&l->SpinLock, &l->OldIrql); }
void NdisReleaseSpinLock(PNDIS_SPIN_LOCK l) { KeReleaseSpinLock(&l->SpinLock, l->OldIrql); }

static int configOpen;
static NDIS_CONFIGURATION_PARAMETER param;
static WCHAR paramText[16];

void NdisOpenConfiguration(PNDIS_STATUS s, PNDIS_HANDLE h, NDIS_HANDLE config)
{
    (void)config;
    configOpen++;
    *h = &configOpen;
    *s = NDIS_STATUS_SUCCESS;
}

void NdisCloseConfiguration(NDIS_HANDLE h)
{
    (void)h;
    if (configOpen-- <= 0) FakeViolation("configuration closed twice");
}

static int SameName(PNDIS_STRING name, const char *text)
{
    USHORT i, n = (USHORT)(name->Length / 2);
    for (i = 0; i < n; i++) if (!text[i] || name->Buffer[i] != (WCHAR)text[i]) return 0;
    return text[n] == 0;
}

void NdisReadConfiguration(PNDIS_STATUS s, PNDIS_CONFIGURATION_PARAMETER *p, NDIS_HANDLE h,
                           PNDIS_STRING name, NDIS_PARAMETER_TYPE type)
{
    (void)h;
    *s = NDIS_STATUS_FAILURE;
    if (type == NdisParameterString && SameName(name, "FallbackAddress") && Fake.HaveFallback) {
        memcpy(paramText, Fake.Fallback, sizeof(paramText));
        param.ParameterType = NdisParameterString;
        param.ParameterData.StringData.Buffer = paramText;
        param.ParameterData.StringData.Length = 24;
        param.ParameterData.StringData.MaximumLength = 24;
        *p = &param;
        *s = NDIS_STATUS_SUCCESS;
    }
}

void NdisWriteConfiguration(PNDIS_STATUS s, NDIS_HANDLE h, PNDIS_STRING name, PNDIS_CONFIGURATION_PARAMETER p)
{
    (void)h;
    *s = NDIS_STATUS_FAILURE;
    if (SameName(name, "FallbackAddress") && p->ParameterType == NdisParameterString &&
        p->ParameterData.StringData.Length == 24) {
        memset(Fake.Fallback, 0, sizeof(Fake.Fallback));
        memcpy(Fake.Fallback, p->ParameterData.StringData.Buffer, 24);
        Fake.HaveFallback = 1;
        Fake.FallbackWrites++;
        *s = NDIS_STATUS_SUCCESS;
    }
}

void NdisReadNetworkAddress(PNDIS_STATUS s, PVOID *address, PUINT length, NDIS_HANDLE h)
{
    (void)h;
    *s = NDIS_STATUS_FAILURE;
    if (Fake.HaveNetworkAddress) {
        *address = Fake.NetworkAddress;
        *length = 6;
        *s = NDIS_STATUS_SUCCESS;
    }
}

void NdisGetCurrentSystemTime(PLARGE_INTEGER t) { t->QuadPart = (LONGLONG)FwTicks() * 10000; }
void NdisMSleep(ULONG us) { NoLock("NdisMSleep"); FwSleep((us + 999) / 1000); }
void NdisWriteErrorLogEntry(NDIS_HANDLE h, ULONG code, ULONG count, ...)
{
    va_list a;
    ULONG i;
    (void)h;
    Fake.ErrorLogs++;
    Fake.ErrorCode = code;
    Fake.ErrorValue[0] = Fake.ErrorValue[1] = 0;
    va_start(a, count);
    for (i = 0; i < count; i++) {
        ULONG v = va_arg(a, ULONG);
        if (i < 2) Fake.ErrorValue[i] = v;
    }
    va_end(a);
}

void NdisMSetAttributesEx(NDIS_HANDLE h, NDIS_HANDLE context, UINT timeout, ULONG attributes,
                          NDIS_INTERFACE_TYPE type)
{
    (void)h; (void)timeout; (void)type;
    Fake.Context = context;
    Fake.Attributes = attributes;
}

void NdisMGetDeviceProperty(NDIS_HANDLE h, PDEVICE_OBJECT *pdo, PDEVICE_OBJECT *fdo,
                            PDEVICE_OBJECT *next, PVOID raw, PVOID translated)
{
    (void)h; (void)raw; (void)translated;
    *pdo = *fdo = *next = Fake.Lower;
}

void NdisMIndicateReceivePacket(NDIS_HANDLE h, PPNDIS_PACKET packets, UINT count)
{
    UINT i;
    (void)h;
    NoLock("NdisMIndicateReceivePacket");
    for (i = 0; i < count; i++) FakeReceive(packets[i]);
}

void NdisMIndicateStatus(NDIS_HANDLE h, NDIS_STATUS status, PVOID buffer, UINT length)
{
    (void)h; (void)buffer; (void)length;
    NoLock("NdisMIndicateStatus");
    if (FakeStatus) FakeStatus(status);
}

void NdisMIndicateStatusComplete(NDIS_HANDLE h) { (void)h; NoLock("NdisMIndicateStatusComplete"); }

void NdisMSendComplete(NDIS_HANDLE h, PNDIS_PACKET p, NDIS_STATUS status)
{
    (void)h;
    NoLock("NdisMSendComplete");
    FakeSendComplete(p, status);
}

void NdisMSetInformationComplete(NDIS_HANDLE h, NDIS_STATUS status)
{
    (void)h;
    NoLock("NdisMSetInformationComplete");
    if (FakeSetComplete) FakeSetComplete(status);
}

void NdisMResetComplete(NDIS_HANDLE h, NDIS_STATUS status, BOOLEAN addressing)
{
    (void)h; (void)addressing;
    NoLock("NdisMResetComplete");
    if (FakeResetComplete) FakeResetComplete(status);
}

void NdisMInitializeWrapper(PNDIS_HANDLE wrapper, PVOID driver, PVOID path, PVOID unused)
{
    (void)driver; (void)path; (void)unused;
    *wrapper = &Fake;
}

NDIS_STATUS NdisMRegisterMiniport(NDIS_HANDLE wrapper, NDIS_MINIPORT_CHARACTERISTICS *chars, UINT size)
{
    (void)wrapper;
    if (size != sizeof(*chars)) return NDIS_STATUS_FAILURE;
    Fake.Chars = *chars;
    return NDIS_STATUS_SUCCESS;
}

void NdisTerminateWrapper(NDIS_HANDLE wrapper, PVOID unused) { (void)wrapper; (void)unused; }

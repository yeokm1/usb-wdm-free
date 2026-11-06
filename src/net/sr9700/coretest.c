/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Core tests: netcore.c and netusb.c, compiled unchanged against the fake
 * operating system of shim/ (ethernet.md section 10.3), with a fake USB
 * device and a fake chip table. The fake completion backend completes
 * transfers inline (inside IoCallDriver), later (from this thread), with
 * failure, during cancel (ABORT_PIPE) and with the device gone; the fake
 * NDIS receiver returns packets at once or holds them across halt. Every
 * accepted send must complete exactly once, every IRP, pool block, packet
 * and buffer must be freed by halt, and fakeos.c checks that no USB
 * submission, NDIS indication or wait happens under a spin lock.
 *
 * The chip here is a test double, not the AX88179 or RTL815x core (those
 * have their own host tests); this file is identical in both packages.
 * It runs real threads (the driver's worker), so it waits with bounded
 * polls. Host-test code only.
 */
#include <stdio.h>
#include <stdlib.h>
#include "fakeos.h"
#include "fakewin.h"
#include "netusb.h"

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path);

static unsigned long assertions;
static void Check(int ok, const char *expression, int line)
{
    assertions++;
    if (!ok) { printf("FAIL %d: %s\n", line, expression); exit(1); }
}
#define CHECK(x) Check((x) != 0, #x, __LINE__)
#define UNTIL(x, ms) do { unsigned long t0_ = FwTicks(); \
    while (!(x) && FwTicks() - t0_ < (ms)) FwSleep(1); Check((x) != 0, #x, __LINE__); } while (0)

/* ---------------- the fake chip ---------------- */

typedef struct CHIP_MODEL {
    int TxSlots, Zlp, FailStart, BadMac;
    int Starts, Resumes, Suspends, Stops, SetMacs, Links, Filters;
    nc_u32 Filter, FilterCount;
    int LinkApplied;
    nc_u32 Fcs;                 /* RxParse reports this many frames per transfer as FCS-stripped */
    nc_u32 Mbps;                /* link speed Link reports; 0: 100 */
} CHIP_MODEL;
static CHIP_MODEL chip;

static long CProbe(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
                   const nc_u32 *size, nc_u32 count, NET_PROBE *probe)
{
    nc_u32 i;
    if (NcParseDevice(device, deviceSize, probe) != NC_OK) return NC_E_MALFORMED;
    for (i = 0; i < count; i++)
        if (config[i] && NcFindVendorInterface(config[i], size[i], 1, probe) == NC_OK) {
            probe->ConfigIndex = (nc_u8)i;
            return NC_OK;
        }
    return NC_E_UNSUPPORTED;
}

static long CStart(void *s, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *p)
{
    (void)s; (void)probe;
    chip.Starts++;
    if (chip.FailStart) return NC_E_DEVICE;
    NcIoRead(io, 0x11, 0, 0, p->Mac, 6);
    p->RxSize = 2048; p->RxCount = 4;
    p->TxSize = 4096; p->TxSlots = (nc_u32)chip.TxSlots; p->TxBatch = 4;
    p->TxHeader = 4; p->TxAlign = 4; p->Version = 0x7e57;
    chip.LinkApplied = 0;
    return io->Status;
}

static long CResume(void *s, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *p)
{
    chip.Resumes++;
    chip.Starts--;
    return CStart(s, io, probe, p);
}

static long CSetMac(void *s, NET_IO *io, const nc_u8 mac[6])
{
    (void)s;
    chip.SetMacs++;
    NcIoWrite(io, 0x12, 0, 0, mac, 6);
    return io->Status;
}

static long CLink(void *s, NET_IO *io, NET_LINK *l)
{
    nc_u8 up = 0;
    (void)s;
    chip.Links++;
    NcIoRead(io, 0x20, 0, 0, &up, 1);
    if (io->Status < 0) return io->Status;
    l->Up = up; l->Mbps = up ? (chip.Mbps ? chip.Mbps : 100) : 0; l->FullDuplex = up; l->RxLength = 2048;
    chip.LinkApplied = up;
    return NC_OK;
}

static int CPoll(void *s, NET_IO *io)
{
    nc_u8 up = 0;
    (void)s;
    NcIoRead(io, 0x20, 0, 0, &up, 1);
    return io->Status >= 0 && (up != 0) != (chip.LinkApplied != 0);
}

static long CSetFilter(void *s, NET_IO *io, nc_u32 filter, const nc_u8 *mc, nc_u32 count)
{
    nc_u8 b = (nc_u8)filter;
    (void)s; (void)mc;
    chip.Filters++;
    chip.Filter = filter; chip.FilterCount = count;
    NcIoWrite(io, 0x30, 0, 0, &b, 1);
    return io->Status;
}

static long CStop(void *s, NET_IO *io) { (void)s; (void)io; chip.Stops++; return NC_OK; }
static long CSuspend(void *s, NET_IO *io) { (void)s; (void)io; chip.Suspends++; chip.LinkApplied = 0; return NC_OK; }
static int CStatus(void *s, const nc_u8 *d, nc_u32 n) { (void)s; return n < 1 ? -1 : (d[0] & 1); }

/* Transfer format of the double: [length lo, hi][frame] ..., length 0 ends. */
static long CRxParse(void *s, const nc_u8 *d, nc_u32 n, NET_EMIT emit, void *context, NET_RX_STATS *st)
{
    nc_u32 pass, at, frames = 0;
    (void)s;
    for (pass = 0; pass < 2; pass++) {
        at = 0; frames = 0;
        while (n - at >= 2) {
            nc_u32 len = d[at] | (d[at + 1] << 8);
            if (!len) break;
            if (len > n - at - 2) return NC_E_MALFORMED;
            if (pass) emit(context, d + at + 2, len);
            frames++;
            at += 2 + len;
        }
    }
    st->Frames = frames; st->Dropped = 0;
    st->FcsStripped = chip.Fcs;
    return (long)frames;
}

static void CTxHeader(void *s, nc_u8 *h, nc_u32 len, nc_u32 maxPacket, int single)
{
    (void)s; (void)maxPacket;
    h[0] = (nc_u8)len; h[1] = (nc_u8)(len >> 8); h[2] = 0xa5; h[3] = (nc_u8)single;
}

static int CTxEnd(void *s, nc_u8 *b, nc_u32 *total, nc_u32 cap, nc_u32 maxPacket)
{
    (void)s; (void)b; (void)cap;
    return chip.Zlp && *total % maxPacket == 0;
}

const NET_CHIP NetPackageChip = {
    "coretest", "coretest double", 1,
    CProbe, CStart, CSetMac, CLink, CPoll, CSetFilter, CStop, CSuspend, CResume,
    CStatus, CRxParse, CTxHeader, CTxEnd
};

/* ---------------- the fake USB device ---------------- */

#define P_IN 0
#define P_OUT 1
#define P_INT 2

typedef struct PIPE {
    UCHAR Address;
    PIRP Head, Tail;
    int Count;
    ULONG MaxTransfer;
} PIPE;

typedef struct DEVICE {
    void *Lock;
    PIPE Pipe[3];
    int Gone, LinkUp;
    UCHAR Mac[6], SetMac[6];
    int InlineRx;                 /* complete this many RX submissions inline */
    int Aborts, Vendor;
    int OutFail;                  /* complete the next OUT with failure */
    UCHAR Sent[64][4096];
    ULONG SentLength[64];
    int SentCount;
} DEVICE;
static DEVICE dev;
static DEVICE_OBJECT lowerDevice;

static const UCHAR deviceDescriptor[18] = { 18,1,0,2,0,0,0,64, 0x34,0x12, 0x78,0x56, 0,1, 0,0,0, 1 };
static const UCHAR configDescriptor[39] = {
    9,2,39,0,1,1,0,0x80,50,
    9,4,0,0,3,0xff,0xff,0,0,
    7,5,0x81,2,0x00,0x02,0,
    7,5,0x02,2,0x00,0x02,0,
    7,5,0x83,3,8,0,8
};

static void Enqueue(PIPE *p, PIRP irp)
{
    irp->FakeNext = NULL;
    if (p->Tail) p->Tail->FakeNext = irp; else p->Head = irp;
    p->Tail = irp;
    p->Count++;
}

static PIRP Dequeue(PIPE *p)
{
    PIRP irp = p->Head;
    if (irp) {
        p->Head = irp->FakeNext;
        if (!p->Head) p->Tail = NULL;
        p->Count--;
    }
    return irp;
}

static PIRP Take(int pipe)
{
    PIRP irp;
    FwLock(dev.Lock);
    irp = Dequeue(&dev.Pipe[pipe]);
    FwUnlock(dev.Lock);
    return irp;
}

static int Queued(int pipe)
{
    int n;
    FwLock(dev.Lock);
    n = dev.Pipe[pipe].Count;
    FwUnlock(dev.Lock);
    return n;
}

static struct _URB_BULK_OR_INTERRUPT_TRANSFER *Bulk(PIRP irp)
{
    return &((PURB)irp->Next.Parameters.Others.Argument1)->UrbBulkOrInterruptTransfer;
}

static UCHAR frame0[1514];

/* An RX transfer carrying one frame to the adapter's address. */
static ULONG OneFrame(UCHAR *b, ULONG cap)
{
    ULONG n = 60;
    CHECK(cap >= n + 2);
    b[0] = (UCHAR)n; b[1] = 0;
    memcpy(b + 2, dev.Mac, 6);
    memset(b + 8, 0x22, n - 6);
    return n + 2;
}

static void Submit(PDEVICE_OBJECT device, PIRP irp, PURB urb)
{
    NTSTATUS gone = STATUS_DEVICE_NOT_CONNECTED;
    CHECK(device == &lowerDevice);
    switch (urb->UrbHeader.Function) {
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE: {
        struct _URB_CONTROL_DESCRIPTOR_REQUEST *u = &urb->UrbControlDescriptorRequest;
        const UCHAR *src = u->DescriptorType == 1 ? deviceDescriptor : configDescriptor;
        ULONG n = u->DescriptorType == 1 ? 18 : sizeof(configDescriptor);
        if (dev.Gone) { FakeComplete(irp, gone, 0); return; }
        CHECK(u->DescriptorType == 1 || (u->DescriptorType == 2 && u->Index == 0));
        if (n > u->TransferBufferLength) n = u->TransferBufferLength;
        memcpy(u->TransferBuffer, src, n);
        u->TransferBufferLength = n;
        FakeComplete(irp, STATUS_SUCCESS, 0);
        return;
    }
    case URB_FUNCTION_SELECT_CONFIGURATION: {
        struct _URB_SELECT_CONFIGURATION *u = &urb->UrbSelectConfiguration;
        int k;
        CHECK(memcmp(u->ConfigurationDescriptor, configDescriptor, sizeof(configDescriptor)) == 0);
        CHECK(u->Interface.InterfaceNumber == 0 && u->Interface.AlternateSetting == 0);
        CHECK(u->Interface.Length >= sizeof(USBD_INTERFACE_INFORMATION) + 2 * sizeof(USBD_PIPE_INFORMATION));
        u->Interface.NumberOfPipes = 3;
        for (k = 0; k < 3; k++) {
            PUSBD_PIPE_INFORMATION p = &u->Interface.Pipes[k];
            p->EndpointAddress = dev.Pipe[k].Address;
            p->MaximumPacketSize = (USHORT)(k == P_INT ? 8 : 512);
            p->PipeType = k == P_INT ? UsbdPipeTypeInterrupt : UsbdPipeTypeBulk;
            p->PipeHandle = &dev.Pipe[k];
            dev.Pipe[k].MaxTransfer = p->MaximumTransferSize;
        }
        u->ConfigurationHandle = &dev;
        FakeComplete(irp, STATUS_SUCCESS, 0);
        return;
    }
    case URB_FUNCTION_VENDOR_DEVICE: {
        struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *u = &urb->UrbControlVendorClassRequest;
        UCHAR *b = u->TransferBuffer;
        int in = (u->TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
        dev.Vendor++;
        if (dev.Gone) { FakeComplete(irp, gone, 0); return; }
        if (in) CHECK(u->TransferFlags & USBD_SHORT_TRANSFER_OK);
        switch (u->Request) {
        case 0x11: CHECK(in && u->TransferBufferLength == 6); memcpy(b, dev.Mac, 6); break;
        case 0x12: CHECK(!in && u->TransferBufferLength == 6); memcpy(dev.SetMac, b, 6); break;
        case 0x20: CHECK(in && u->TransferBufferLength == 1); b[0] = (UCHAR)dev.LinkUp; break;
        case 0x30: CHECK(!in && u->TransferBufferLength == 1); break;
        default: CHECK(0);
        }
        FakeComplete(irp, STATUS_SUCCESS, 0);
        return;
    }
    case URB_FUNCTION_ABORT_PIPE: {
        PIPE *p = urb->UrbPipeRequest.PipeHandle;
        PIRP q;
        dev.Aborts++;
        /* Complete every queued transfer of the pipe as cancelled, inline
         * (inside the abort's IoCallDriver, as a host controller may). */
        while ((q = Take((int)(p - dev.Pipe))) != NULL)
            FakeComplete(q, STATUS_CANCELLED, USBD_STATUS_CANCELED);
        FakeComplete(irp, dev.Gone ? gone : STATUS_SUCCESS, 0);
        return;
    }
    case URB_FUNCTION_RESET_PIPE:
        FakeComplete(irp, dev.Gone ? gone : STATUS_SUCCESS, 0);
        return;
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER: {
        struct _URB_BULK_OR_INTERRUPT_TRANSFER *u = &urb->UrbBulkOrInterruptTransfer;
        PIPE *p = u->PipeHandle;
        int k = (int)(p - dev.Pipe);
        CHECK(k >= 0 && k < 3);
        CHECK(u->TransferBufferLength <= p->MaxTransfer);
        if (k == P_OUT) CHECK(!(u->TransferFlags & USBD_TRANSFER_DIRECTION_IN));
        else CHECK((u->TransferFlags & (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK)) ==
                   (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK));
        if (dev.Gone) { FakeComplete(irp, gone, (USBD_STATUS)0xC0007000L); return; }
        if (k == P_IN && dev.InlineRx > 0) {
            dev.InlineRx--;
            u->TransferBufferLength = OneFrame(u->TransferBuffer, u->TransferBufferLength);
            FakeComplete(irp, STATUS_SUCCESS, 0);
            return;
        }
        FwLock(dev.Lock);
        Enqueue(p, irp);
        FwUnlock(dev.Lock);
        return;
    }
    default:
        CHECK(0);
    }
}

static BOOLEAN Cancel(PIRP irp)
{
    int k;
    for (k = 0; k < 3; k++) {
        PIPE *p = &dev.Pipe[k];
        PIRP prev = NULL, q;
        FwLock(dev.Lock);
        for (q = p->Head; q; prev = q, q = q->FakeNext) {
            if (q != irp) continue;
            if (prev) prev->FakeNext = q->FakeNext; else p->Head = q->FakeNext;
            if (p->Tail == q) p->Tail = prev;
            p->Count--;
            FwUnlock(dev.Lock);
            FakeComplete(irp, STATUS_CANCELLED, USBD_STATUS_CANCELED);
            return TRUE;
        }
        FwUnlock(dev.Lock);
    }
    return FALSE;
}

/* Complete the oldest transfer of a pipe; data for IN, captured for OUT. */
static void CompleteIn(int pipe, const UCHAR *data, ULONG n)
{
    PIRP irp = Take(pipe);
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *u;
    CHECK(irp != NULL);
    u = Bulk(irp);
    CHECK(n <= u->TransferBufferLength);
    memcpy(u->TransferBuffer, data, n);
    u->TransferBufferLength = n;
    FakeComplete(irp, STATUS_SUCCESS, 0);
}

static ULONG CompleteOut(NTSTATUS status)
{
    PIRP irp = Take(P_OUT);
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *u;
    ULONG n;
    CHECK(irp != NULL);
    u = Bulk(irp);
    n = u->TransferBufferLength;
    CHECK(dev.SentCount < 64);
    if (n) memcpy(dev.Sent[dev.SentCount], u->TransferBuffer, n);
    dev.SentLength[dev.SentCount++] = n;
    FakeComplete(irp, status, NT_SUCCESS(status) ? 0 : (USBD_STATUS)0xC0000004L);
    return n;
}

static void Interrupt(int up)
{
    UCHAR b[8] = { 0 };
    b[0] = (UCHAR)up;
    dev.LinkUp = up;
    UNTIL(Queued(P_INT) == 1, 2000);
    CompleteIn(P_INT, b, 8);
}

/* ---------------- the fake NDIS protocol ---------------- */

typedef struct NDIS_SIDE {
    int Hold;
    PNDIS_PACKET Held[128];
    int HeldCount;
    int Received, Resources;
    UCHAR Last[1514];
    UINT LastLength;
    int Connects, Disconnects, SetCompletes, ResetCompletes;
} NDIS_SIDE;
static NDIS_SIDE nd;
static void *ndLock;

static void Receive(PNDIS_PACKET p)
{
    PNDIS_BUFFER b;
    PVOID va;
    UINT len, total;
    NdisQueryPacket(p, NULL, NULL, &b, &total);
    NdisQueryBuffer(b, &va, &len);
    CHECK(len == total && p->HeaderSize == 14);
    FwLock(ndLock);
    nd.Received++;
    memcpy(nd.Last, va, len);
    nd.LastLength = len;
    if (p->Status == NDIS_STATUS_RESOURCES) {
        nd.Resources++;               /* copied; the miniport keeps it */
        FwUnlock(ndLock);
        return;
    }
    if (nd.Hold) {
        CHECK(nd.HeldCount < 128);
        nd.Held[nd.HeldCount++] = p;
        FwUnlock(ndLock);
        return;
    }
    FwUnlock(ndLock);
    Fake.Chars.ReturnPacketHandler(Fake.Context, p);   /* reentrant return */
}

static void ReturnHeld(void)
{
    int i, n;
    PNDIS_PACKET list[128];
    FwLock(ndLock);
    n = nd.HeldCount;
    memcpy(list, nd.Held, n * sizeof(list[0]));
    nd.HeldCount = 0;
    FwUnlock(ndLock);
    for (i = 0; i < n; i++) Fake.Chars.ReturnPacketHandler(Fake.Context, list[i]);
}

/* Test packets, owned here: a frame split into up to three buffers. */
typedef struct TPACKET {
    NDIS_PACKET P;
    NDIS_BUFFER B[3];
    UCHAR Data[1600];
    UINT Length;
    int Completions;
    NDIS_STATUS Status;
} TPACKET;
static TPACKET tp[256];

static void SendComplete(PNDIS_PACKET p, NDIS_STATUS status)
{
    TPACKET *t = (TPACKET *)p;
    CHECK(t >= tp && t < tp + 256);
    FwLock(ndLock);
    t->Completions++;
    t->Status = status;
    FwUnlock(ndLock);
}

static void StatusInd(NDIS_STATUS s)
{
    if (s == NDIS_STATUS_MEDIA_CONNECT) nd.Connects++;
    else if (s == NDIS_STATUS_MEDIA_DISCONNECT) nd.Disconnects++;
    else CHECK(0);
}

static void SetDone(NDIS_STATUS s) { CHECK(s == NDIS_STATUS_SUCCESS); nd.SetCompletes++; }
static void ResetDone(NDIS_STATUS s) { CHECK(s == NDIS_STATUS_SUCCESS); nd.ResetCompletes++; }

static PNDIS_PACKET MakePacket(int i, UINT length, const UCHAR *dst)
{
    TPACKET *t = &tp[i];
    UINT k, cut1 = length / 3, cut2 = 2 * length / 3;
    memset(t, 0, sizeof(*t));
    t->P.FakeMagic = 0x50414b54UL;
    t->Length = length;
    memcpy(t->Data, dst, 6);
    for (k = 6; k < length; k++) t->Data[k] = (UCHAR)(i * 7 + k);
    /* three fragments, the middle one possibly empty */
    t->B[0].Va = t->Data; t->B[0].Length = cut1;
    t->B[1].Va = t->Data + cut1; t->B[1].Length = cut2 - cut1;
    t->B[2].Va = t->Data + cut2; t->B[2].Length = length - cut2;
    t->B[0].Next = &t->B[1]; t->B[1].Next = &t->B[2];
    t->P.Head = &t->B[0];
    return &t->P;
}

/* ---------------- the adapter's life ---------------- */

static NDIS_HANDLE ctx;

static void Fresh(void)
{
    memset(&Fake, 0, sizeof(Fake));
    memset(&chip, 0, sizeof(chip));
    chip.TxSlots = 2;
    if (dev.Lock) FwLockFree(dev.Lock);
    memset(&dev, 0, sizeof(dev));
    dev.Lock = FwLockNew();
    dev.Pipe[P_IN].Address = 0x81; dev.Pipe[P_OUT].Address = 0x02; dev.Pipe[P_INT].Address = 0x83;
    dev.Mac[0] = 0x00; dev.Mac[1] = 0x11; dev.Mac[2] = 0x22; dev.Mac[3] = 0x33; dev.Mac[4] = 0x44; dev.Mac[5] = 0x55;
    memset(&nd, 0, sizeof(nd));
    memset(tp, 0, sizeof(tp));
    lowerDevice.StackSize = 2;
    Fake.Lower = &lowerDevice;
    Fake.Wdm110 = 1;
    FakeSubmit = Submit; FakeCancel = Cancel; FakeReceive = Receive;
    FakeSendComplete = SendComplete; FakeStatus = StatusInd;
    FakeSetComplete = SetDone; FakeResetComplete = ResetDone;
}

static NDIS_STATUS Init(void)
{
    DRIVER_OBJECT driver;
    UNICODE_STRING path;
    NDIS_STATUS error, s;
    UINT chosen = 99;
    NDIS_MEDIUM media[2] = { NdisMedium802_5, NdisMedium802_3 };
    CHECK(DriverEntry(&driver, &path) == STATUS_SUCCESS);
    s = Fake.Chars.InitializeHandler(&error, &chosen, media, 2, (NDIS_HANDLE)&Fake, (NDIS_HANDLE)&Fake);
    if (s == NDIS_STATUS_SUCCESS) {
        CHECK(chosen == 1);
        CHECK(Fake.Attributes & NDIS_ATTRIBUTE_DESERIALIZE);
        ctx = Fake.Context;
    }
    return s;
}

static ULONG QueryUlong(NDIS_OID oid)
{
    ULONG v = 0, w = 0, n = 0;
    CHECK(Fake.Chars.QueryInformationHandler(ctx, oid, &v, sizeof(v), &w, &n) == NDIS_STATUS_SUCCESS);
    CHECK(w == sizeof(v));
    return v;
}

static void Query6(NDIS_OID oid, UCHAR *out)
{
    ULONG w = 0, n = 0;
    CHECK(Fake.Chars.QueryInformationHandler(ctx, oid, out, 6, &w, &n) == NDIS_STATUS_SUCCESS && w == 6);
}

static NDIS_STATUS SetUlong(NDIS_OID oid, ULONG v)
{
    ULONG r = 0, n = 0;
    return Fake.Chars.SetInformationHandler(ctx, oid, &v, sizeof(v), &r, &n);
}

static void Halt(void)
{
    Fake.Chars.HaltHandler(ctx);
}

static void Balanced(void)
{
    CHECK(Fake.Violations == 0);
    CHECK(Fake.Irps == 0);
    CHECK(Fake.Pool == 0);
    CHECK(Fake.NdisMemory == 0);
    CHECK(Fake.Packets == 0 && Fake.Buffers == 0);
    CHECK(Fake.PacketPools == 0 && Fake.BufferPools == 0);
    CHECK(Queued(P_IN) == 0 && Queued(P_OUT) == 0 && Queued(P_INT) == 0);
}

static void LinkUp(void)
{
    int connects = nd.Connects;
    Interrupt(1);
    UNTIL(nd.Connects == connects + 1, 3000);
    UNTIL(Queued(P_IN) == 4 && Queued(P_INT) == 1, 3000);
    CHECK(QueryUlong(OID_GEN_MEDIA_CONNECT_STATUS) == NdisMediaStateConnected);
    CHECK(QueryUlong(OID_GEN_LINK_SPEED) == 1000000);
}

static void SetFilter(ULONG filter)
{
    int done = nd.SetCompletes;
    NDIS_STATUS s = SetUlong(OID_GEN_CURRENT_PACKET_FILTER, filter);
    CHECK(s == NDIS_STATUS_PENDING);
    UNTIL(nd.SetCompletes == done + 1, 3000);
    CHECK(chip.Filter == filter);
}

/* ---------------- scenarios ---------------- */

static void Lifecycle(int wdm110)
{
    Fresh();
    Fake.Wdm110 = wdm110;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    CHECK(chip.Starts == 1 && chip.SetMacs == 0);
    CHECK(dev.Pipe[P_IN].MaxTransfer >= 2048 && dev.Pipe[P_OUT].MaxTransfer >= 4096);
    UNTIL(Queued(P_INT) == 1, 2000);
    CHECK(Queued(P_IN) == 0);                         /* no bulk traffic before link */
    CHECK(QueryUlong(OID_GEN_MEDIA_CONNECT_STATUS) == NdisMediaStateDisconnected);
    LinkUp();
    Halt();
    CHECK(chip.Stops == 1);
    Balanced();
}

/* OID_GEN_LINK_SPEED in 100 b/s units for every speed a chip reports,
 * up to 10 Gb/s (100000000, within a ULONG). */
static void LinkSpeeds(void)
{
    static const nc_u32 mbps[] = { 10, 100, 1000, 2500, 5000, 10000, 1000 };
    unsigned i;
    Fresh();
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    UNTIL(Queued(P_INT) == 1, 2000);
    for (i = 0; i < sizeof(mbps) / sizeof(mbps[0]); i++) {
        int connects = nd.Connects, disconnects = nd.Disconnects;
        chip.Mbps = mbps[i];
        Interrupt(1);
        UNTIL(nd.Connects == connects + 1, 3000);
        CHECK(QueryUlong(OID_GEN_LINK_SPEED) == mbps[i] * 10000);
        Interrupt(0);
        UNTIL(nd.Disconnects == disconnects + 1, 3000);
        CHECK(QueryUlong(OID_GEN_LINK_SPEED) == mbps[i] * 10000);   /* the last speed stays */
    }
    Halt();
    Balanced();
}

static void ReceivePath(void)
{
    UCHAR b[2048];
    ULONG at = 0, i;
    static const UCHAR mc1[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    static const UCHAR mc2[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x02 };
    UCHAR list[12];
    ULONG r = 0, n = 0;
    int done;
    struct { const UCHAR *dst; UCHAR fill; ULONG len; } f[6];
    static const UCHAR bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const UCHAR other[6] = { 0x00, 0x99, 0x99, 0x99, 0x99, 0x99 };

    Fresh();
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    LinkUp();
    SetFilter(NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST | NDIS_PACKET_TYPE_MULTICAST);
    memcpy(list, mc1, 6);
    memcpy(list + 6, mc1, 6);
    done = nd.SetCompletes;
    CHECK(Fake.Chars.SetInformationHandler(ctx, OID_802_3_MULTICAST_LIST, list, 6, &r, &n) == NDIS_STATUS_PENDING);
    UNTIL(nd.SetCompletes == done + 1, 3000);
    CHECK(chip.FilterCount == 1);
    CHECK(Fake.Chars.SetInformationHandler(ctx, OID_802_3_MULTICAST_LIST, list, 7, &r, &n) == NDIS_STATUS_INVALID_LENGTH);

    f[0].dst = dev.Mac; f[0].len = 60;
    f[1].dst = bcast; f[1].len = 200;
    f[2].dst = other; f[2].len = 100;     /* someone else's: dropped */
    f[3].dst = mc1; f[3].len = 1514;
    f[4].dst = mc2; f[4].len = 80;        /* not in the list: dropped */
    f[5].dst = dev.Mac; f[5].len = 10;    /* runt: dropped */
    for (i = 0; i < 6; i++) {
        b[at] = (UCHAR)f[i].len; b[at + 1] = (UCHAR)(f[i].len >> 8);
        memset(b + at + 2, (int)(0x40 + i), f[i].len);
        memcpy(b + at + 2, f[i].dst, f[i].len < 6 ? f[i].len : 6);
        at += 2 + f[i].len;
    }
    CHECK(at <= sizeof(b));
    CompleteIn(P_IN, b, at);
    CHECK(nd.Received == 3 && nd.LastLength == 1514 && nd.Last[6] == 0x43);
    UNTIL(Queued(P_IN) == 4, 2000);                 /* reposted */
    CHECK(QueryUlong(OID_GEN_RCV_OK) == 3);
    /* A malformed transfer emits nothing and does not stop receive. */
    b[0] = 0xff; b[1] = 0x07;
    CompleteIn(P_IN, b, at);
    CHECK(nd.Received == 3);
    UNTIL(Queued(P_IN) == 4, 2000);
    /* Promiscuous lets everything through. */
    SetFilter(NDIS_PACKET_TYPE_PROMISCUOUS);
    b[0] = (UCHAR)f[0].len; b[1] = 0;
    CompleteIn(P_IN, b, at);
    CHECK(nd.Received == 3 + 5);
    /* A frame that came with its FCS (the chip core stripped it) is an
     * anomaly: one error log entry per adapter, not one per transfer. */
    CHECK(Fake.ErrorLogs == 0);
    chip.Fcs = 2;
    CompleteIn(P_IN, b, at);
    CHECK(nd.Received == 3 + 10);
    CHECK(Fake.ErrorLogs == 1 && Fake.ErrorCode == NDIS_ERROR_CODE_HARDWARE_FAILURE);
    CHECK(Fake.ErrorValue[0] == NET_ANOMALY_RX_FCS && Fake.ErrorValue[1] == 2);
    UNTIL(Queued(P_IN) == 4, 2000);
    CompleteIn(P_IN, b, at);
    CHECK(Fake.ErrorLogs == 1);
    chip.Fcs = 0;
    Halt();
    Balanced();
}

static void InlineStorm(void)
{
    UCHAR b[64];
    Fresh();
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    LinkUp();
    SetFilter(NDIS_PACKET_TYPE_DIRECTED);
    Fake.MaxDepth = 0;
    dev.InlineRx = 5000;
    /* One later completion starts a long run of inline completions: the
     * pump must iterate, never nest IoCallDriver (section 4.7.1). */
    CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    UNTIL(dev.InlineRx == 0, 10000);
    CHECK(nd.Received == 5001);
    CHECK(Fake.MaxDepth == 1);
    UNTIL(Queued(P_IN) == 4, 2000);
    Halt();
    Balanced();
}

static void HoldAndResources(void)
{
    UCHAR b[64];
    int i;
    Fresh();
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    LinkUp();
    SetFilter(NDIS_PACKET_TYPE_DIRECTED);
    nd.Hold = 1;
    for (i = 0; i < 70; i++) {
        UNTIL(Queued(P_IN) > 0, 2000);
        CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    }
    /* 64 packets, RESOURCES below 8 free: 56 owned by the protocol, the
     * rest indicated with RESOURCES and reclaimed at once. */
    CHECK(nd.Received == 70 && nd.HeldCount == 56 && nd.Resources == 14);
    CHECK(QueryUlong(OID_GEN_RCV_NO_BUFFER) == 0);
    ReturnHeld();
    nd.Hold = 0;
    /* Halt waits for packets still owned by the protocol. */
    nd.Hold = 1;
    for (i = 0; i < 3; i++) {
        UNTIL(Queued(P_IN) > 0, 2000);
        CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    }
    CHECK(nd.HeldCount == 3);
    {
        void *t = FwThread((void (*)(void *))Fake.Chars.HaltHandler, ctx);
        CHECK(FwWait(t, 300) == 1);                   /* still waiting */
        ReturnHeld();
        CHECK(FwWait(t, 5000) == 0);
        FwClose(t);
    }
    Balanced();
}

static void SendPath(void)
{
    PNDIS_PACKET list[80];
    int i, completed;
    static const UCHAR peer[6] = { 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
    UINT lengths[10] = { 14, 59, 60, 61, 100, 508, 1000, 1513, 1514, 600 };

    Fresh();
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    /* Before link: every send fails at once. */
    list[0] = MakePacket(0, 100, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 1);
    CHECK(tp[0].Completions == 1 && tp[0].Status == NDIS_STATUS_FAILURE);
    LinkUp();

    for (i = 0; i < 10; i++) list[i] = MakePacket(i, lengths[i], peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 10);
    /* Two slots of two frames each in flight; complete until done. */
    for (completed = 0; completed < 20 && !tp[9].Completions; completed++) {
        UNTIL(Queued(P_OUT) > 0, 2000);
        CompleteOut(STATUS_SUCCESS);
    }
    {
        /* Unpack what reached the wire: header, frame (padded to 60),
         * the next header at a multiple of 4. */
        int t, f = 0;
        for (t = 0; t < dev.SentCount; t++) {
            ULONG at = 0, n = dev.SentLength[t];
            while (at < n) {
                UINT len = dev.Sent[t][at] | (dev.Sent[t][at + 1] << 8);
                UINT want = tp[f].Length < 60 ? 60 : tp[f].Length;
                CHECK(dev.Sent[t][at + 2] == 0xa5 && len == want);
                CHECK(memcmp(dev.Sent[t] + at + 4, tp[f].Data, tp[f].Length) == 0);
                if (tp[f].Length < 60) CHECK(dev.Sent[t][at + 4 + tp[f].Length] == 0);
                at += 4 + len;
                if (at < n) at = (at + 3) & ~3UL;
                f++;
            }
            CHECK(at == n);
        }
        CHECK(f == 10);
    }
    for (i = 0; i < 10; i++) CHECK(tp[i].Completions == 1 && tp[i].Status == NDIS_STATUS_SUCCESS);
    CHECK(QueryUlong(OID_GEN_XMIT_OK) == 10);

    /* A burst beyond the queue: everything completes exactly once. */
    for (i = 0; i < 80; i++) list[i] = MakePacket(100 + i, 200, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 80);
    dev.SentCount = 0;
    for (completed = 0; completed < 200; completed++) {
        int all = 1;
        for (i = 0; i < 80; i++) if (!tp[100 + i].Completions) all = 0;
        if (all) break;
        UNTIL(Queued(P_OUT) > 0, 2000);
        CompleteOut(STATUS_SUCCESS);
        dev.SentCount = 0;
    }
    {
        int ok = 0, failed = 0;
        for (i = 0; i < 80; i++) {
            CHECK(tp[100 + i].Completions == 1);
            if (tp[100 + i].Status == NDIS_STATUS_SUCCESS) ok++; else failed++;
        }
        CHECK(failed > 0 && ok >= 64);                /* the queue holds 64 */
    }

    /* A failed transfer fails its packets once, and traffic recovers. */
    for (i = 0; i < 4; i++) list[i] = MakePacket(200 + i, 300, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 4);
    UNTIL(Queued(P_OUT) == 2, 2000);
    CompleteOut(STATUS_UNSUCCESSFUL);
    UNTIL(tp[200].Completions == 1 && tp[201].Completions == 1, 3000);
    CHECK(tp[200].Status == NDIS_STATUS_FAILURE);
    UNTIL(tp[202].Completions == 1 && tp[203].Completions == 1, 5000);   /* aborted, or sent after recovery */
    UNTIL(Queued(P_IN) == 4 && Queued(P_INT) == 1, 5000);
    list[0] = MakePacket(210, 300, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 1);
    UNTIL(Queued(P_OUT) == 1, 2000);
    CompleteOut(STATUS_SUCCESS);
    CHECK(tp[210].Completions == 1 && tp[210].Status == NDIS_STATUS_SUCCESS);

    /* Link down: the queue is flushed, bulk traffic stops, sends fail. */
    for (i = 0; i < 6; i++) list[i] = MakePacket(220 + i, 300, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 6);
    UNTIL(Queued(P_OUT) == 2, 2000);
    Interrupt(0);
    UNTIL(nd.Disconnects == 1, 3000);
    UNTIL(Queued(P_IN) == 0 && Queued(P_OUT) == 0, 3000);
    for (i = 0; i < 6; i++) CHECK(tp[220 + i].Completions == 1);
    list[0] = MakePacket(230, 300, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 1);
    CHECK(tp[230].Completions == 1 && tp[230].Status == NDIS_STATUS_FAILURE);
    Halt();
    Balanced();
}

static void ZeroLengthPackets(void)
{
    PNDIS_PACKET list[2];
    static const UCHAR peer[6] = { 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
    Fresh();
    chip.TxSlots = 1;
    chip.Zlp = 1;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    LinkUp();
    /* 4 + 508 = 512: the payload needs a ZLP before anything else. */
    list[0] = MakePacket(0, 508, peer);
    list[1] = MakePacket(1, 300, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 1);
    UNTIL(Queued(P_OUT) == 1, 2000);
    Fake.Chars.SendPacketsHandler(ctx, list + 1, 1);
    CHECK(CompleteOut(STATUS_SUCCESS) == 512);
    CHECK(tp[0].Completions == 0);                    /* not until its ZLP is out */
    UNTIL(Queued(P_OUT) == 1, 2000);
    CHECK(CompleteOut(STATUS_SUCCESS) == 0);          /* the ZLP, before packet 1 */
    CHECK(tp[0].Completions == 1 && tp[0].Status == NDIS_STATUS_SUCCESS);
    UNTIL(Queued(P_OUT) == 1, 2000);
    CHECK(CompleteOut(STATUS_SUCCESS) == 304);
    CHECK(tp[1].Completions == 1);
    Halt();
    Balanced();
}

static void PowerAndReset(void)
{
    int done;
    UCHAR b[64];
    Fresh();
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    LinkUp();
    SetFilter(NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST);
    /* D3: everything drained before the OID completes. */
    done = nd.SetCompletes;
    CHECK(SetUlong(OID_PNP_SET_POWER, NdisDeviceStateD3) == NDIS_STATUS_PENDING);
    UNTIL(nd.SetCompletes == done + 1, 5000);
    CHECK(chip.Suspends == 1);
    CHECK(Queued(P_IN) == 0 && Queued(P_OUT) == 0 && Queued(P_INT) == 0);
    /* D0: the bring-up again, the filter again, the link, traffic. */
    done = nd.SetCompletes;
    chip.Filters = 0;
    CHECK(SetUlong(OID_PNP_SET_POWER, NdisDeviceStateD0) == NDIS_STATUS_PENDING);
    UNTIL(nd.SetCompletes == done + 1, 5000);
    CHECK(chip.Resumes == 1 && chip.Filters >= 1);
    CHECK(chip.Filter == (NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST));
    UNTIL(Queued(P_IN) == 4 && Queued(P_INT) == 1, 3000);
    CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    CHECK(nd.Received == 1);
    /* Reset: pends, completes once, traffic back. */
    {
        BOOLEAN addressing = TRUE;
        CHECK(Fake.Chars.ResetHandler(&addressing, ctx) == NDIS_STATUS_PENDING);
        CHECK(!addressing);
        UNTIL(nd.ResetCompletes == 1, 5000);
        UNTIL(Queued(P_IN) == 4 && Queued(P_INT) == 1, 3000);
    }
    Halt();
    Balanced();
}

static void SurpriseRemoval(void)
{
    PNDIS_PACKET list[4];
    static const UCHAR peer[6] = { 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
    int i;
    Fresh();
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    LinkUp();
    for (i = 0; i < 4; i++) list[i] = MakePacket(i, 400, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 4);
    UNTIL(Queued(P_OUT) == 2, 2000);
    /* Unplugged: what is queued fails as gone, so do all later requests. */
    dev.Gone = 1;
    {
        PIRP irp;
        int k;
        for (k = 0; k < 3; k++)
            while ((irp = Take(k)) != NULL) FakeComplete(irp, STATUS_DEVICE_NOT_CONNECTED, (USBD_STATUS)0xC0007000L);
    }
    UNTIL(nd.Disconnects == 1, 5000);
    for (i = 0; i < 4; i++) UNTIL(tp[i].Completions == 1, 3000);
    list[0] = MakePacket(10, 400, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 1);
    CHECK(tp[10].Completions == 1 && tp[10].Status == NDIS_STATUS_FAILURE);
    CHECK(SetUlong(OID_GEN_CURRENT_PACKET_FILTER, NDIS_PACKET_TYPE_DIRECTED) == NDIS_STATUS_SUCCESS);
    Halt();
    CHECK(chip.Stops == 0);                           /* nothing sent to a gone device */
    Balanced();
}

static void Addresses(void)
{
    UCHAR cur[6], perm[6];
    static const UCHAR laa[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9a };
    /* NetworkAddress wins and is programmed into the chip. */
    Fresh();
    memcpy(Fake.NetworkAddress, laa, 6);
    Fake.HaveNetworkAddress = 1;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    Query6(OID_802_3_CURRENT_ADDRESS, cur);
    Query6(OID_802_3_PERMANENT_ADDRESS, perm);
    CHECK(memcmp(cur, laa, 6) == 0 && memcmp(perm, dev.Mac, 6) == 0);
    CHECK(chip.SetMacs == 1 && memcmp(dev.SetMac, laa, 6) == 0);
    Halt();
    Balanced();
    /* A multicast NetworkAddress is ignored. */
    Fresh();
    Fake.NetworkAddress[0] = 0x03;
    Fake.HaveNetworkAddress = 1;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    Query6(OID_802_3_CURRENT_ADDRESS, cur);
    CHECK(memcmp(cur, dev.Mac, 6) == 0 && chip.SetMacs == 0);
    Halt();
    Balanced();
    /* No valid chip address: a locally administered one, generated once,
     * stored, and reused next time. */
    Fresh();
    memset(dev.Mac, 0, 6);
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    Query6(OID_802_3_CURRENT_ADDRESS, cur);
    CHECK((cur[0] & 3) == 2 && (cur[0] | cur[1] | cur[2] | cur[3] | cur[4] | cur[5]));
    CHECK(Fake.FallbackWrites == 1 && chip.SetMacs == 1 && memcmp(dev.SetMac, cur, 6) == 0);
    Halt();
    Balanced();
    {
        WCHAR saved[16];
        memcpy(saved, Fake.Fallback, sizeof(saved));
        Fresh();
        memset(dev.Mac, 0, 6);
        memcpy(Fake.Fallback, saved, sizeof(saved));
        Fake.HaveFallback = 1;
        CHECK(Init() == NDIS_STATUS_SUCCESS);
        Query6(OID_802_3_CURRENT_ADDRESS, perm);
        CHECK(memcmp(perm, cur, 6) == 0 && Fake.FallbackWrites == 0);
        Halt();
        Balanced();
    }
}

static void Failures(void)
{
    Fresh();
    chip.FailStart = 1;
    CHECK(Init() == NDIS_STATUS_ADAPTER_NOT_FOUND);
    Balanced();
    Fresh();
    dev.Gone = 1;
    CHECK(Init() == NDIS_STATUS_ADAPTER_NOT_FOUND);
    Balanced();
}

static void Oids(void)
{
    UCHAR buffer[512];
    ULONG w, n, v;
    ULONGLONG wide;
    Fresh();
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_GEN_SUPPORTED_LIST, buffer, 4, &w, &n) ==
          NDIS_STATUS_INVALID_LENGTH && n > 4 && w == 0);
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_GEN_SUPPORTED_LIST, buffer, n, &w, &n) ==
          NDIS_STATUS_SUCCESS && w % 4 == 0);
    CHECK(QueryUlong(OID_GEN_MAXIMUM_FRAME_SIZE) == 1500 && QueryUlong(OID_GEN_MAXIMUM_TOTAL_SIZE) == 1514);
    CHECK(QueryUlong(OID_802_3_MAXIMUM_LIST_SIZE) == 32);
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_GEN_XMIT_OK, &wide, 8, &w, &n) == NDIS_STATUS_SUCCESS && w == 8);
    CHECK(Fake.Chars.QueryInformationHandler(ctx, 0x12345678, &v, 4, &w, &n) == NDIS_STATUS_NOT_SUPPORTED);
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_GEN_VENDOR_DESCRIPTION, buffer, sizeof(buffer), &w, &n) ==
          NDIS_STATUS_SUCCESS && w == sizeof("coretest double"));
    CHECK(SetUlong(OID_GEN_CURRENT_PACKET_FILTER, 0x10000) == NDIS_STATUS_NOT_SUPPORTED);
    CHECK(SetUlong(OID_GEN_CURRENT_LOOKAHEAD, 1501) == NDIS_STATUS_INVALID_DATA);
    CHECK(SetUlong(OID_GEN_CURRENT_LOOKAHEAD, 256) == NDIS_STATUS_SUCCESS);
    {
        ULONG r, need;
        USHORT tiny = 0;
        CHECK(Fake.Chars.SetInformationHandler(ctx, OID_GEN_CURRENT_PACKET_FILTER, &tiny, 2, &r, &need) ==
              NDIS_STATUS_INVALID_LENGTH && need == 4);
    }
    Halt();
    Balanced();
}

int main(void)
{
    int round;
    ndLock = FwLockNew();
    Lifecycle(1);
    Lifecycle(0);                 /* Windows 98: no wait on the thread object */
    LinkSpeeds();
    ReceivePath();
    InlineStorm();
    HoldAndResources();
    SendPath();
    ZeroLengthPackets();
    PowerAndReset();
    SurpriseRemoval();
    Addresses();
    Failures();
    Oids();
    for (round = 0; round < 20; round++) Lifecycle(round & 1);
    printf("PASS: %lu USB Ethernet core checks (netcore.c, netusb.c on the fake backend)\n", assertions);
    return 0;
}

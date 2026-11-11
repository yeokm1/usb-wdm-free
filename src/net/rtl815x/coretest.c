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
    nc_u32 Offload, LsoMax;     /* what Start reports */
    nc_u32 RxSum;               /* RxChecksum's answer */
    int Offloads;               /* TxOffload calls */
    NET_TX_INFO LastInfo;       /* the last TxOffload request */
    long Seen[2];               /* the settings the last Start read (NcSetting) */
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
    (void)s;
    chip.Starts++;
    chip.Seen[0] = NcSetting(probe, 0);
    chip.Seen[1] = NcSetting(probe, 1);
    if (chip.FailStart) return NC_E_DEVICE;
    NcIoRead(io, 0x11, 0, 0, p->Mac, 6);
    p->RxSize = 2048; p->RxCount = 4;
    p->TxSize = 4096; p->TxSlots = (nc_u32)chip.TxSlots; p->TxBatch = 4;
    p->TxHeader = 4; p->TxAlign = 4; p->Version = 0x7e57;
    p->Offload = chip.Offload; p->LsoMax = chip.LsoMax;
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

/* Offloads: the request is recorded and marked in the header (bit 7 of
 * its fourth byte). */
static void CTxOffload(void *s, nc_u8 *h, const NET_TX_INFO *info)
{
    (void)s;
    chip.Offloads++;
    chip.LastInfo = *info;
    h[3] |= 0x80;
}

static nc_u32 CRxChecksum(void *s, const nc_u8 *frame) { (void)s; (void)frame; return chip.RxSum; }

/* Two Advanced-tab keywords, as a package's table lists them. */
static const NET_SETTING CSettings[] = {
    { "*TestSpeed", 0, 3, { 0, 6, 2500 } },
    { "TestFlag", 1, 2, { 0, 1 } },
};

const NET_CHIP NetPackageChip = {
    "coretest", "coretest double", 1,
    CProbe, CStart, CSetMac, CLink, CPoll, CSetFilter, CStop, CSuspend, CResume,
    CStatus, CRxParse, CTxHeader, CTxEnd, 0, CTxOffload, CRxChecksum,
    CSettings, 2
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
    int Inject;                       /* frames to complete from inside the next receive indications */
    PNDIS_PACKET Held[128];
    int HeldCount;
    int Received, Resources;
    UCHAR Last[1514];
    UINT LastLength;
    ULONG LastChecksum;           /* the last packet's checksum per-packet information */
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
    if (nd.Inject > 0) {
        /* A bulk-IN completion while the timer's routine indicates. */
        UCHAR f[64];
        nd.Inject--;
        if (Queued(P_IN)) CompleteIn(P_IN, f, OneFrame(f, sizeof(f)));
    }
    FwLock(ndLock);
    nd.Received++;
    memcpy(nd.Last, va, len);
    nd.LastLength = len;
    nd.LastChecksum = (ULONG)(ULONG_PTR)NDIS_PER_PACKET_INFO_FROM_PACKET(p, TcpIpChecksumPacketInfo);
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
    UCHAR Data[4200];
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

    /* A burst of 80, more than the 64 the queue once held: every packet
     * completes exactly once, and none fails for want of room. */
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
        CHECK(failed == 0 && ok == 80);               /* no limit on the queue */
    }

    /* A failed transfer fails its packets once, and traffic recovers. */
    for (i = 0; i < 4; i++) list[i] = MakePacket(200 + i, 1514, peer);
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

/* 98 SE and ME: what the USB completions bring is indicated and completed
 * only from the NDIS timer, in order; nothing from a completion routine
 * (the fake counts that as a violation); Halt cancels the timer, completes
 * the queued sends and takes back the receives it never indicated. */

/* ---------------- transfers filled by bytes, and task offload ---------------- */

/* Transfers are filled by bytes up to the chip's frame cap (TxBatch 4 in
 * 4096 bytes behind 4-byte headers): four short frames share one
 * transfer, three full ones take two. */
static void FillByBytes(void)
{
    PNDIS_PACKET list[8];
    static const UCHAR peer[6] = { 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
    int i;
    Fresh();
    chip.TxSlots = 1;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    LinkUp();
    for (i = 0; i < 4; i++) list[i] = MakePacket(i, 100, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 4);
    UNTIL(Queued(P_OUT) == 1, 2000);
    CHECK(CompleteOut(STATUS_SUCCESS) == 4 * 104);
    for (i = 0; i < 4; i++) UNTIL(tp[i].Completions == 1 && tp[i].Status == NDIS_STATUS_SUCCESS, 2000);
    for (i = 0; i < 6; i++) list[i] = MakePacket(10 + i, 60 + i, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 6);      /* six: four, then two (the frame cap) */
    UNTIL(Queued(P_OUT) == 1, 2000);
    CHECK(CompleteOut(STATUS_SUCCESS) == 200 + 67);
    UNTIL(Queued(P_OUT) == 1, 2000);
    CHECK(CompleteOut(STATUS_SUCCESS) == 68 + 69);
    for (i = 0; i < 3; i++) list[i] = MakePacket(20 + i, 1514, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 3);      /* two fit 4096 bytes, the third alone */
    UNTIL(Queued(P_OUT) == 1, 2000);
    CHECK(CompleteOut(STATUS_SUCCESS) == 1518 + 2 + 1518);
    UNTIL(Queued(P_OUT) == 1, 2000);
    CHECK(CompleteOut(STATUS_SUCCESS) == 1518);
    for (i = 0; i < 3; i++) UNTIL(tp[20 + i].Completions == 1 && tp[20 + i].Status == NDIS_STATUS_SUCCESS, 2000);
    CHECK(chip.Offloads == 0);
    Halt();
    Balanced();
}

/* A TCP/IPv4 frame: Ethernet II, a 20-byte IP header, a 20-byte TCP
 * header at 34; the per-packet large-send and checksum values given. */
static PNDIS_PACKET TcpPacket(int i, UINT length, ULONG mss, ULONG checksum, UCHAR protocol)
{
    static const UCHAR peer[6] = { 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
    PNDIS_PACKET p = MakePacket(i, length, peer);
    TPACKET *t = &tp[i];
    t->Data[12] = 0x08; t->Data[13] = 0x00; t->Data[14] = 0x45; t->Data[23] = protocol;
    t->Data[34 + 12] = 0x50;
    NDIS_PER_PACKET_INFO_FROM_PACKET(p, TcpLargeSendPacketInfo) = (PVOID)(ULONG_PTR)mss;
    NDIS_PER_PACKET_INFO_FROM_PACKET(p, TcpIpChecksumPacketInfo) = (PVOID)(ULONG_PTR)checksum;
    return p;
}

#define CS_TCP 0x05UL       /* Transmit: V4 and TCP */
#define CS_UDP 0x09UL       /* V4 and UDP */
#define CS_IP  0x11UL       /* V4 and IP */

static NDIS_STATUS SetTaskList(int checksum, int lso, ULONG encapsulation)
{
    union { UCHAR b[256]; ULONG align; } u;
    NDIS_TASK_OFFLOAD_HEADER *h = (NDIS_TASK_OFFLOAD_HEADER *)u.b;
    ULONG at = sizeof(*h), r = 0, n = 0;
    NDIS_TASK_OFFLOAD *last = NULL;
    memset(&u, 0, sizeof(u));
    h->Version = NDIS_TASK_OFFLOAD_VERSION; h->Size = sizeof(*h);
    h->EncapsulationFormat.Encapsulation = (NDIS_ENCAPSULATION)encapsulation;
    h->EncapsulationFormat.EncapsulationHeaderSize = 14;
    if (checksum) {
        NDIS_TASK_OFFLOAD *t = (NDIS_TASK_OFFLOAD *)(u.b + at);
        NDIS_TASK_TCP_IP_CHECKSUM *c = (NDIS_TASK_TCP_IP_CHECKSUM *)t->TaskBuffer;
        h->OffsetFirstTask = at;
        t->Version = NDIS_TASK_OFFLOAD_VERSION; t->Size = sizeof(*t); t->Task = TcpIpChecksumNdisTask;
        t->TaskBufferLength = sizeof(*c);
        c->V4Transmit.TcpChecksum = 1; c->V4Transmit.IpChecksum = 1;
        c->V4Receive.TcpChecksum = 1; c->V4Receive.UdpChecksum = 1; c->V4Receive.IpChecksum = 1;
        last = t;
        at += (FIELD_OFFSET(NDIS_TASK_OFFLOAD, TaskBuffer) + sizeof(*c) + 3) & ~3UL;
    }
    if (lso) {
        NDIS_TASK_OFFLOAD *t = (NDIS_TASK_OFFLOAD *)(u.b + at);
        NDIS_TASK_TCP_LARGE_SEND *l = (NDIS_TASK_TCP_LARGE_SEND *)t->TaskBuffer;
        if (last) last->OffsetNextTask = at - (ULONG)((UCHAR *)last - u.b); else h->OffsetFirstTask = at;
        t->Version = NDIS_TASK_OFFLOAD_VERSION; t->Size = sizeof(*t); t->Task = TcpLargeSendNdisTask;
        t->TaskBufferLength = sizeof(*l);
        l->MaxOffLoadSize = 4092 - 134; l->MinSegmentCount = 2;
        at += (FIELD_OFFSET(NDIS_TASK_OFFLOAD, TaskBuffer) + sizeof(*l) + 3) & ~3UL;
    }
    return Fake.Chars.SetInformationHandler(ctx, OID_TCP_TASK_OFFLOAD, u.b, at, &r, &n);
}

static void TaskOffload(void)
{
    union { UCHAR b[256]; ULONG align; } u;
    NDIS_TASK_OFFLOAD_HEADER *h = (NDIS_TASK_OFFLOAD_HEADER *)u.b;
    NDIS_TASK_OFFLOAD *t;
    NDIS_TASK_TCP_IP_CHECKSUM *c;
    NDIS_TASK_TCP_LARGE_SEND *l;
    ULONG w = 0, n = 0;
    UCHAR b[2048];
    PNDIS_PACKET p;
    int links;

    Fresh();
    chip.TxSlots = 1;
    chip.Offload = NC_OFFLOAD_TX_IP4 | NC_OFFLOAD_TX_TCP4 | NC_OFFLOAD_LSO4 | NC_OFFLOAD_RX4;
    chip.LsoMax = 8000;                              /* capped to one buffer: 4096 - 4 */
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    LinkUp();
    SetFilter(NDIS_PACKET_TYPE_DIRECTED);

    /* Discovery: listed last in the supported list here, and only here. */
    {
        NDIS_OID list[64];
        ULONG k, found = 0;
        CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_GEN_SUPPORTED_LIST, list, sizeof(list), &w, &n) ==
              NDIS_STATUS_SUCCESS && w % sizeof(NDIS_OID) == 0);
        for (k = 0; k < w / sizeof(NDIS_OID); k++) if (list[k] == OID_TCP_TASK_OFFLOAD) found++;
        CHECK(found == 1);
    }
    /* The query: the header back with a checksum task and a large-send task. */
    memset(&u, 0, sizeof(u));
    h->Version = NDIS_TASK_OFFLOAD_VERSION; h->Size = sizeof(*h);
    h->EncapsulationFormat.Encapsulation = IEEE_802_3_Encapsulation;
    h->EncapsulationFormat.EncapsulationHeaderSize = 14;
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_TCP_TASK_OFFLOAD, u.b, 8, &w, &n) ==
          NDIS_STATUS_INVALID_LENGTH && n == sizeof(*h));
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_TCP_TASK_OFFLOAD, u.b, sizeof(*h) + 8, &w, &n) ==
          NDIS_STATUS_INVALID_LENGTH && n > sizeof(*h) + 8);
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_TCP_TASK_OFFLOAD, u.b, sizeof(u.b), &w, &n) ==
          NDIS_STATUS_SUCCESS && w > sizeof(*h));
    CHECK(h->OffsetFirstTask == sizeof(*h) && h->Version == NDIS_TASK_OFFLOAD_VERSION);
    t = (NDIS_TASK_OFFLOAD *)(u.b + h->OffsetFirstTask);
    c = (NDIS_TASK_TCP_IP_CHECKSUM *)t->TaskBuffer;
    CHECK(t->Task == TcpIpChecksumNdisTask && t->TaskBufferLength == sizeof(*c) && t->OffsetNextTask);
    CHECK(c->V4Transmit.TcpChecksum && c->V4Transmit.IpChecksum && !c->V4Transmit.UdpChecksum);
    CHECK(c->V4Receive.TcpChecksum && c->V4Receive.UdpChecksum && c->V4Receive.IpChecksum);
    CHECK(!c->V6Transmit.TcpChecksum && !c->V6Receive.TcpChecksum);
    t = (NDIS_TASK_OFFLOAD *)((UCHAR *)t + t->OffsetNextTask);
    l = (NDIS_TASK_TCP_LARGE_SEND *)t->TaskBuffer;
    CHECK(t->Task == TcpLargeSendNdisTask && t->OffsetNextTask == 0);
    CHECK(l->MaxOffLoadSize == 4092 - 134 && l->MinSegmentCount == 2 && l->TcpOptions && l->IpOptions);
    CHECK((UCHAR *)l + sizeof(*l) <= u.b + w);
    h->EncapsulationFormat.Encapsulation = (NDIS_ENCAPSULATION)0;
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_TCP_TASK_OFFLOAD, u.b, sizeof(u.b), &w, &n) ==
          NDIS_STATUS_NOT_SUPPORTED);

    /* Not yet enabled: a send asking for a checksum fails, alone. */
    p = TcpPacket(0, 200, 0, CS_TCP, 6);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(tp[0].Completions == 1, 2000);
    CHECK(tp[0].Status == NDIS_STATUS_FAILURE && Queued(P_OUT) == 0);

    /* Sets: a bad encapsulation and a task not offered are refused. */
    CHECK(SetTaskList(1, 1, 0) == NDIS_STATUS_NOT_SUPPORTED);
    CHECK(SetTaskList(1, 1, IEEE_802_3_Encapsulation) == NDIS_STATUS_SUCCESS);

    /* A TCP checksum: the chip is asked, the TCP header at 34. */
    p = TcpPacket(1, 200, 0, CS_TCP, 6);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(Queued(P_OUT) == 1, 2000);
    CHECK(CompleteOut(STATUS_SUCCESS) == 204 && (dev.Sent[0][3] & 0x80));
    CHECK(chip.LastInfo.Flags == NC_TX_TCP4 && chip.LastInfo.Transport == 34);
    UNTIL(tp[1].Completions == 1 && tp[1].Status == NDIS_STATUS_SUCCESS, 2000);
    /* a short frame with an offload is not padded (the chip pads it);
     * a plain one is */
    p = TcpPacket(22, 54, 0, CS_TCP, 6);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(Queued(P_OUT) == 1, 2000);
    CHECK(CompleteOut(STATUS_SUCCESS) == 4 + 54);
    UNTIL(tp[22].Completions == 1 && tp[22].Status == NDIS_STATUS_SUCCESS, 2000);
    p = TcpPacket(23, 54, 0, 0, 6);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(Queued(P_OUT) == 1, 2000);
    CHECK(CompleteOut(STATUS_SUCCESS) == 4 + 60);
    UNTIL(tp[23].Completions == 1 && tp[23].Status == NDIS_STATUS_SUCCESS, 2000);
    /* the IPv4 header checksum alone */
    p = TcpPacket(2, 98, 0, CS_IP, 1);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(Queued(P_OUT) == 1, 2000);
    CompleteOut(STATUS_SUCCESS);
    CHECK(chip.LastInfo.Flags == NC_TX_IP4);
    UNTIL(tp[2].Completions == 1 && tp[2].Status == NDIS_STATUS_SUCCESS, 2000);
    /* UDP was not offered: refused; a TCP request on a UDP frame too */
    p = TcpPacket(3, 98, 0, CS_UDP, 17);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(tp[3].Completions == 1, 2000);
    CHECK(tp[3].Status == NDIS_STATUS_FAILURE);
    p = TcpPacket(4, 98, 0, CS_TCP, 17);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(tp[4].Completions == 1, 2000);
    CHECK(tp[4].Status == NDIS_STATUS_FAILURE);

    /* A large send: alone in its transfer, the MSS passed, and the
     * per-packet value becomes the payload sent (3000 - 34 - 20). */
    p = TcpPacket(5, 3000, 1460, 0, 6);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(Queued(P_OUT) == 1, 2000);
    CHECK(CompleteOut(STATUS_SUCCESS) == 3004);
    CHECK(chip.LastInfo.Flags == NC_TX_LSO4 && chip.LastInfo.Mss == 1460 && chip.LastInfo.Transport == 34);
    UNTIL(tp[5].Completions == 1 && tp[5].Status == NDIS_STATUS_SUCCESS, 2000);
    CHECK((ULONG)(ULONG_PTR)NDIS_PER_PACKET_INFO_FROM_PACKET(&tp[5].P, TcpLargeSendPacketInfo) == 2946);
    /* longer than one buffer holds, or a long frame that is no large send: refused at once */
    {
        PNDIS_PACKET two[2];
        two[0] = TcpPacket(6, 4093, 1460, 0, 6);
        two[1] = TcpPacket(12, 100, 0, 0, 6);
        Fake.Chars.SendPacketsHandler(ctx, two, 2);
        CHECK(tp[6].Completions == 1 && tp[6].Status == NDIS_STATUS_FAILURE);
        UNTIL(Queued(P_OUT) == 1, 2000);             /* the next one is not failed with it */
        CHECK(CompleteOut(STATUS_SUCCESS) == 104);
        UNTIL(tp[12].Completions == 1 && tp[12].Status == NDIS_STATUS_SUCCESS, 2000);
    }
    p = TcpPacket(7, 1515, 0, CS_TCP, 6);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    CHECK(tp[7].Completions == 1 && tp[7].Status == NDIS_STATUS_FAILURE);
    /* a large send not on TCP */
    p = TcpPacket(8, 3000, 1460, 0, 17);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(tp[8].Completions == 1, 2000);
    CHECK(tp[8].Status == NDIS_STATUS_FAILURE);

    /* Receive: what the chip verified and TCP/IP enabled, as succeeded. */
    chip.RxSum = NC_RX_IP_OK | NC_RX_TCP_OK;
    UNTIL(Queued(P_IN) == 4, 2000);
    CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    CHECK(nd.Received == 1 && nd.LastChecksum == 0x28);       /* TCP (bit 3) and IP (bit 5) succeeded */
    chip.RxSum = NC_RX_IP_OK | NC_RX_UDP_OK;
    UNTIL(Queued(P_IN) == 4, 2000);
    CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    CHECK(nd.Received == 2 && nd.LastChecksum == 0x30);
    chip.RxSum = 0;
    UNTIL(Queued(P_IN) == 4, 2000);
    CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    CHECK(nd.Received == 3 && nd.LastChecksum == 0);

    /* A large send whose transfer fails sent nothing. */
    p = TcpPacket(9, 3000, 1460, 0, 6);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(Queued(P_OUT) == 1, 2000);
    links = chip.Links;
    CompleteOut(STATUS_UNSUCCESSFUL);
    UNTIL(tp[9].Completions == 1, 3000);
    CHECK(tp[9].Status == NDIS_STATUS_FAILURE);
    CHECK(NDIS_PER_PACKET_INFO_FROM_PACKET(&tp[9].P, TcpLargeSendPacketInfo) == NULL);
    UNTIL(chip.Links > links, 5000);                   /* recovered */
    UNTIL(Queued(P_IN) == 4 && Queued(P_INT) == 1, 5000);

    /* One refused request in a batch fails alone: the frames before and
     * after it share the transfer. */
    {
        PNDIS_PACKET three[3];
        three[0] = TcpPacket(13, 100, 0, 0, 6);
        three[1] = TcpPacket(14, 100, 0, CS_TCP, 17);   /* a TCP request on UDP */
        three[2] = TcpPacket(15, 120, 0, 0, 6);
        Fake.Chars.SendPacketsHandler(ctx, three, 3);
        UNTIL(Queued(P_OUT) == 1, 2000);
        CHECK(tp[14].Completions == 1 && tp[14].Status == NDIS_STATUS_FAILURE);
        CHECK(CompleteOut(STATUS_SUCCESS) == 104 + 124);
        UNTIL(tp[13].Completions == 1 && tp[13].Status == NDIS_STATUS_SUCCESS, 2000);
        UNTIL(tp[15].Completions == 1 && tp[15].Status == NDIS_STATUS_SUCCESS, 2000);
    }
    {
        /* every send of a batch (four, the frame cap) refused: the one
         * queued after them still goes, with no further send to kick it */
        PNDIS_PACKET five[5];
        int k;
        for (k = 0; k < 4; k++) five[k] = TcpPacket(30 + k, 100, 0, CS_UDP, 17);
        five[4] = TcpPacket(34, 100, 0, 0, 6);
        Fake.Chars.SendPacketsHandler(ctx, five, 5);
        for (k = 0; k < 4; k++) CHECK(tp[30 + k].Completions == 1 && tp[30 + k].Status == NDIS_STATUS_FAILURE);
        CHECK(Queued(P_OUT) == 1);                     /* within the same call */
        CHECK(CompleteOut(STATUS_SUCCESS) == 104);
        UNTIL(tp[34].Completions == 1 && tp[34].Status == NDIS_STATUS_SUCCESS, 2000);
    }

    /* Turned off while a large send waits behind a transfer: that send
     * fails alone, the plain one after it goes. */
    {
        PNDIS_PACKET q[2];
        p = TcpPacket(19, 200, 0, 0, 6);
        Fake.Chars.SendPacketsHandler(ctx, &p, 1);
        UNTIL(Queued(P_OUT) == 1, 2000);              /* the one slot is busy */
        q[0] = TcpPacket(20, 3000, 1460, 0, 6);
        q[1] = TcpPacket(21, 200, 0, 0, 6);
        Fake.Chars.SendPacketsHandler(ctx, q, 2);
        CHECK(SetTaskList(1, 0, IEEE_802_3_Encapsulation) == NDIS_STATUS_SUCCESS);   /* no large send */
        CompleteOut(STATUS_SUCCESS);
        UNTIL(tp[20].Completions == 1, 2000);
        CHECK(tp[20].Status == NDIS_STATUS_FAILURE);
        CHECK(NDIS_PER_PACKET_INFO_FROM_PACKET(&tp[20].P, TcpLargeSendPacketInfo) == NULL);
        UNTIL(Queued(P_OUT) == 1, 2000);
        CHECK(CompleteOut(STATUS_SUCCESS) == 204);
        UNTIL(tp[21].Completions == 1 && tp[21].Status == NDIS_STATUS_SUCCESS, 2000);
        UNTIL(tp[19].Completions == 1 && tp[19].Status == NDIS_STATUS_SUCCESS, 2000);
    }

    /* Disabled (a header and no task): nothing reported or asked. */
    {
        NDIS_TASK_OFFLOAD_HEADER off;
        ULONG r = 0, need = 0;
        memset(&off, 0, sizeof(off));
        off.Version = NDIS_TASK_OFFLOAD_VERSION; off.Size = sizeof(off);
        off.EncapsulationFormat.Encapsulation = IEEE_802_3_Encapsulation;
        off.EncapsulationFormat.EncapsulationHeaderSize = 14;
        CHECK(Fake.Chars.SetInformationHandler(ctx, OID_TCP_TASK_OFFLOAD, &off, sizeof(off), &r, &need) ==
              NDIS_STATUS_SUCCESS);
    }
    chip.RxSum = NC_RX_IP_OK | NC_RX_TCP_OK;
    CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    CHECK(nd.Received == 4);
    CHECK(nd.LastChecksum == 0);
    p = TcpPacket(10, 200, 0, CS_TCP, 6);
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(tp[10].Completions == 1, 2000);
    CHECK(tp[10].Status == NDIS_STATUS_FAILURE);
    p = TcpPacket(11, 200, 0, 0, 6);                 /* a plain frame still goes */
    Fake.Chars.SendPacketsHandler(ctx, &p, 1);
    UNTIL(Queued(P_OUT) == 1, 2000);
    CompleteOut(STATUS_SUCCESS);
    UNTIL(tp[11].Completions == 1 && tp[11].Status == NDIS_STATUS_SUCCESS, 2000);
    Halt();
    Balanced();

    /* A chip without the IPv4 header checksum: a set asking for it is refused. */
    Fresh();
    chip.Offload = NC_OFFLOAD_TX_TCP4 | NC_OFFLOAD_RX4;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    CHECK(SetTaskList(1, 0, IEEE_802_3_Encapsulation) == NDIS_STATUS_NOT_SUPPORTED);
    CHECK(SetTaskList(0, 1, IEEE_802_3_Encapsulation) == NDIS_STATUS_NOT_SUPPORTED);   /* no large send */
    Halt();
    Balanced();

    /* A chip with no offload, and 98 SE/ME: no task offload at all. */
    Fresh();
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    {
        NDIS_OID list[64];
        ULONG k;
        CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_GEN_SUPPORTED_LIST, list, sizeof(list), &w, &n) ==
              NDIS_STATUS_SUCCESS);
        for (k = 0; k < w / sizeof(NDIS_OID); k++) CHECK(list[k] != OID_TCP_TASK_OFFLOAD);
    }
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_TCP_TASK_OFFLOAD, u.b, sizeof(u.b), &w, &n) ==
          NDIS_STATUS_NOT_SUPPORTED);
    Halt();
    Balanced();
    Fresh();
    Fake.Wdm110 = 0;
    chip.Offload = NC_OFFLOAD_TX_TCP4 | NC_OFFLOAD_LSO4 | NC_OFFLOAD_RX4;
    chip.LsoMax = 4092;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    h->EncapsulationFormat.Encapsulation = IEEE_802_3_Encapsulation;
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_TCP_TASK_OFFLOAD, u.b, sizeof(u.b), &w, &n) ==
          NDIS_STATUS_NOT_SUPPORTED);
    CHECK(SetTaskList(1, 1, IEEE_802_3_Encapsulation) == NDIS_STATUS_NOT_SUPPORTED);
    {
        NDIS_OID list[64];
        ULONG k;
        CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_GEN_SUPPORTED_LIST, list, sizeof(list), &w, &n) ==
              NDIS_STATUS_SUCCESS);
        for (k = 0; k < w / sizeof(NDIS_OID); k++) CHECK(list[k] != OID_TCP_TASK_OFFLOAD);
    }
    Halt();
    Balanced();
}

static void Deferred98(void)
{
    UCHAR b[2048];
    PNDIS_PACKET list[8];
    static const UCHAR peer[6] = { 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
    ULONG at = 0, i;
    int completed, sets;

    Fresh();
    Fake.Wdm110 = 0;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    CHECK(Fake.Timer != NULL && Fake.TimerSets == 0);
    LinkUp();
    SetFilter(NDIS_PACKET_TYPE_DIRECTED);
    CHECK(Fake.TimerSets == 0);                      /* link and filter: nothing queued */

    /* Three frames in one transfer: queued, the timer armed once. */
    for (i = 0; i < 3; i++) {
        ULONG len = 100 + i;
        b[at] = (UCHAR)len; b[at + 1] = 0;
        memset(b + at + 2, (int)(0x60 + i), len);
        memcpy(b + at + 2, dev.Mac, 6);
        at += 2 + len;
    }
    CompleteIn(P_IN, b, at);
    CHECK(nd.Received == 0 && Fake.TimerSets == 1);
    CHECK(QueryUlong(OID_GEN_RCV_OK) == 3);
    UNTIL(Queued(P_IN) == 4, 2000);
    CompleteIn(P_IN, b, at);                         /* still armed: not set again */
    CHECK(nd.Received == 0 && Fake.TimerSets == 1);
    CHECK(FakeRunTimer() == 1);
    CHECK(nd.Received == 6 && nd.Resources == 0);
    CHECK(nd.LastLength == 102 && nd.Last[6] == 0x62);   /* in order: the last one last */
    CHECK(FakeRunTimer() == 0);

    /* Sends: completed by the timer, once each, with their status. */
    for (i = 0; i < 4; i++) list[i] = MakePacket((int)i, 300, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 4);
    for (completed = 0; completed < 100; completed++) {
        int done = 0, after = 0;
        for (i = 0; i < 4; i++) done += tp[i].Completions;
        if (done == 4) break;
        if (Queued(P_OUT)) {
            CompleteOut(STATUS_SUCCESS);
            for (i = 0; i < 4; i++) after += tp[i].Completions;
            CHECK(after == done);                     /* never from the completion */
        } else if (!FakeRunTimer()) {
            FwSleep(10);
        }
    }
    for (i = 0; i < 4; i++) CHECK(tp[i].Completions == 1 && tp[i].Status == NDIS_STATUS_SUCCESS);
    CHECK(QueryUlong(OID_GEN_XMIT_OK) == 4);

    /* A send that fails at once (bad length) completes from the timer too. */
    list[0] = MakePacket(10, 10, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 1);
    CHECK(tp[10].Completions == 0);
    CHECK(FakeRunTimer() == 1);
    CHECK(tp[10].Completions == 1 && tp[10].Status == NDIS_STATUS_FAILURE);

    /* Halt with receives and sends still queued: the sends complete once,
     * the receives are never indicated, the timer is cancelled while set,
     * and the pool is whole again (Balanced; Halt would wait forever for a
     * receive left behind). */
    UNTIL(Queued(P_IN) == 4, 2000);
    CompleteIn(P_IN, b, at);
    for (i = 0; i < 2; i++) list[i] = MakePacket(20 + (int)i, 300, peer);
    Fake.Chars.SendPacketsHandler(ctx, list, 2);
    UNTIL(Queued(P_OUT) > 0, 2000);
    while (Queued(P_OUT)) CompleteOut(STATUS_SUCCESS);
    CHECK(Fake.Timer->Set);
    sets = Fake.TimerSets;
    Halt();
    CHECK(nd.Received == 6);
    CHECK(tp[20].Completions == 1 && tp[21].Completions == 1);
    CHECK(Fake.TimerCancels == 2 && Fake.TimerSetAtCancel && Fake.TimerSets == sets);
    CHECK(Fake.Timer == NULL);                        /* freed with the adapter, not set */
    Balanced();
}

/* 98 SE and ME: receives completed while the timer's routine is
 * indicating are delivered by that same run; the timer is never set from
 * inside its own routine (the fake counts that as a violation), and is not
 * set again once the run ends with nothing queued. */
static void DeferredReentry98(void)
{
    UCHAR b[64];
    Fresh();
    Fake.Wdm110 = 0;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    LinkUp();
    SetFilter(NDIS_PACKET_TYPE_DIRECTED);
    CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    CHECK(Fake.TimerSets == 1);
    nd.Inject = 3;
    CHECK(FakeRunTimer() == 1);
    CHECK(nd.Received == 4 && nd.Inject == 0);
    CHECK(Fake.TimerSets == 1 && !Fake.Timer->Set);
    {
        PNDIS_MINIPORT_TIMER first = Fake.Timer;
        CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));      /* armed anew after the run: the other timer */
        CHECK(Fake.TimerSets == 2 && Fake.Timer != first && Fake.Timer->Set);
        CHECK(FakeRunTimer() == 1 && nd.Received == 5);
        UNTIL(Queued(P_IN) > 0, 2000);
        CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));      /* and back to the first */
        CHECK(Fake.TimerSets == 3 && Fake.Timer == first && FakeRunTimer() == 1 && nd.Received == 6);
    }
    Halt();
    Balanced();
}

/* 98 SE and ME: Halt with more queued receives than one indication's
 * batch and no queued send (17), and with the whole pool queued (64):
 * every one goes back to the pool. */
static void DeferredHalt98(ULONG frames)
{
    UCHAR b[64];
    ULONG i;
    Fresh();
    Fake.Wdm110 = 0;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    LinkUp();
    SetFilter(NDIS_PACKET_TYPE_DIRECTED);
    for (i = 0; i < frames; i++) {
        UNTIL(Queued(P_IN) > 0, 2000);
        CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    }
    CHECK(nd.Received == 0 && QueryUlong(OID_GEN_RCV_OK) == frames);
    CHECK(Fake.TimerSets == 1 && Fake.Timer->Set);
    {
        void *t = FwThread((void (*)(void *))Fake.Chars.HaltHandler, ctx);
        CHECK(FwWait(t, 5000) == 0);                  /* returns: nothing left out */
        FwClose(t);
    }
    CHECK(nd.Received == 0);
    CHECK(Fake.TimerCancels == 2 && Fake.TimerSetAtCancel && Fake.Timer == NULL);
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

/* 98 SE and ME: a receive queued for the timer when a sleep request
 * comes is taken back, never indicated, and the timer cancelled, before
 * the request completes; at D0 the timer serves again. */
static void Power98(void)
{
    UCHAR b[64];
    int done;
    Fresh();
    Fake.Wdm110 = 0;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    LinkUp();
    SetFilter(NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST);
    CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    CHECK(nd.Received == 0 && Fake.Timer->Set);
    done = nd.SetCompletes;
    CHECK(SetUlong(OID_PNP_SET_POWER, NdisDeviceStateD3) == NDIS_STATUS_PENDING);
    UNTIL(nd.SetCompletes == done + 1, 5000);
    CHECK(!Fake.Timer->Set && Fake.TimerSetAtCancel);
    CHECK(FakeRunTimer() == 0 && nd.Received == 0);   /* taken back, not indicated */
    {
        /* A send while asleep fails at once, from SendPackets itself. */
        static const UCHAR peer[6] = { 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
        PNDIS_PACKET one = MakePacket(30, 10, peer);
        Fake.Chars.SendPacketsHandler(ctx, &one, 1);
        CHECK(tp[30].Completions == 1 && tp[30].Status == NDIS_STATUS_FAILURE);
    }
    done = nd.SetCompletes;
    CHECK(SetUlong(OID_PNP_SET_POWER, NdisDeviceStateD0) == NDIS_STATUS_PENDING);
    UNTIL(nd.SetCompletes == done + 1, 5000);
    UNTIL(Queued(P_IN) == 4 && Queued(P_INT) == 1, 3000);
    CompleteIn(P_IN, b, OneFrame(b, sizeof(b)));
    CHECK(nd.Received == 0 && Fake.Timer->Set);
    CHECK(FakeRunTimer() == 1 && nd.Received == 1);
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
    for (i = 0; i < 4; i++) list[i] = MakePacket(i, 1514, peer);
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

static void Keyword(const char *name, const char *text)
{
    Fake.Keywords[Fake.KeywordCount].Name = name;
    Fake.Keywords[Fake.KeywordCount].Text = text;
    Fake.KeywordCount++;
}

/* The Advanced tab's keywords (ethernet.md section 4.10): read before the
 * chip starts, decimal text or an integer, anything else the default;
 * on NT the checksum keywords narrow what TCP/IP is offered. */
static void Settings(void)
{
    union { UCHAR b[256]; ULONG align; } u;
    NDIS_TASK_OFFLOAD_HEADER *h = (NDIS_TASK_OFFLOAD_HEADER *)u.b;
    NDIS_TASK_OFFLOAD *t;
    NDIS_TASK_TCP_IP_CHECKSUM *c;
    ULONG w = 0, n = 0;
    int round, done;

    for (round = 0; round < 2; round++) {    /* NT, then 98 SE */
        Fresh();
        Fake.Wdm110 = !round;
        CHECK(Init() == NDIS_STATUS_SUCCESS);
        CHECK(chip.Seen[0] == 0 && chip.Seen[1] == 1);          /* none written: the defaults */
        Halt();
        Balanced();

        Fresh();
        Fake.Wdm110 = !round;
        Keyword("*TestSpeed", "2500");
        Keyword("TestFlag", "0");
        CHECK(Init() == NDIS_STATUS_SUCCESS);
        CHECK(chip.Seen[0] == 2500 && chip.Seen[1] == 0 && Fake.KeywordReads >= 2);
        Halt();
        Balanced();
    }

    /* Malformed or not offered: the default. */
    Fresh();
    Keyword("*TestSpeed", "25x0");
    Keyword("TestFlag", "7");
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    CHECK(chip.Seen[0] == 0 && chip.Seen[1] == 1);
    Halt();
    Balanced();
    Fresh();
    Keyword("*TestSpeed", "");
    Keyword("TestFlag", "-1");
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    CHECK(chip.Seen[0] == 0 && chip.Seen[1] == 1);
    Halt();
    Balanced();
    Fresh();
    Keyword("*TestSpeed", "1000");                       /* a number, but not one this chip offers */
    Fake.Keywords[1].Name = "TestFlag";                  /* an integer-typed value (a DWORD) */
    Fake.Keywords[1].IsInteger = 1;
    Fake.Keywords[1].Integer = 0;
    Fake.KeywordCount = 2;
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    CHECK(chip.Seen[0] == 0 && chip.Seen[1] == 0);
    /* A power cycle's restart reads them from the probe again. */
    done = nd.SetCompletes;
    CHECK(SetUlong(OID_PNP_SET_POWER, NdisDeviceStateD3) == NDIS_STATUS_PENDING);
    UNTIL(nd.SetCompletes == done + 1, 5000);
    Fake.Keywords[1].Integer = 1;                        /* a later registry change waits for a restart */
    done = nd.SetCompletes;
    CHECK(SetUlong(OID_PNP_SET_POWER, NdisDeviceStateD0) == NDIS_STATUS_PENDING);
    UNTIL(nd.SetCompletes == done + 1, 5000);
    CHECK(chip.Resumes == 1);
    CHECK(chip.Seen[0] == 0 && chip.Seen[1] == 0);
    Halt();
    Balanced();

    /* The checksum keywords (NT): TCP's off, the IP header's and UDP's left on. */
    Fresh();
    chip.Offload = NC_OFFLOAD_TX_IP4 | NC_OFFLOAD_TX_TCP4 | NC_OFFLOAD_TX_UDP4;
    Keyword("*TCPChecksumOffloadIPv4", "2");             /* receive only: not offered, so off */
    Keyword("*UDPChecksumOffloadIPv4", "3");
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    memset(&u, 0, sizeof(u));
    h->Version = NDIS_TASK_OFFLOAD_VERSION; h->Size = sizeof(*h);
    h->EncapsulationFormat.Encapsulation = IEEE_802_3_Encapsulation;
    h->EncapsulationFormat.EncapsulationHeaderSize = 14;
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_TCP_TASK_OFFLOAD, u.b, sizeof(u.b), &w, &n) ==
          NDIS_STATUS_SUCCESS);
    t = (NDIS_TASK_OFFLOAD *)(u.b + h->OffsetFirstTask);
    c = (NDIS_TASK_TCP_IP_CHECKSUM *)t->TaskBuffer;
    CHECK(t->Task == TcpIpChecksumNdisTask);
    CHECK(!c->V4Transmit.TcpChecksum && c->V4Transmit.IpChecksum && c->V4Transmit.UdpChecksum);
    Halt();
    Balanced();
    /* All three off: nothing to offer. */
    Fresh();
    chip.Offload = NC_OFFLOAD_TX_IP4 | NC_OFFLOAD_TX_TCP4 | NC_OFFLOAD_TX_UDP4;
    Keyword("*IPChecksumOffloadIPv4", "0");
    Keyword("*TCPChecksumOffloadIPv4", "0");
    Keyword("*UDPChecksumOffloadIPv4", "0");
    CHECK(Init() == NDIS_STATUS_SUCCESS);
    CHECK(Fake.Chars.QueryInformationHandler(ctx, OID_TCP_TASK_OFFLOAD, u.b, sizeof(u.b), &w, &n) ==
          NDIS_STATUS_NOT_SUPPORTED);
    Halt();
    Balanced();
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
    FillByBytes();
    TaskOffload();
    Deferred98();
    DeferredReentry98();
    DeferredHalt98(17);
    DeferredHalt98(64);
    ZeroLengthPackets();
    PowerAndReset();
    Power98();
    SurpriseRemoval();
    Addresses();
    Settings();
    Failures();
    Oids();
    for (round = 0; round < 20; round++) Lifecycle(round & 1);
    printf("PASS: %lu USB Ethernet core checks (netcore.c, netusb.c on the fake backend)\n", assertions);
    return 0;
}

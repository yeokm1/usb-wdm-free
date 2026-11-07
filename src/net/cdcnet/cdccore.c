/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * cdccore - host test of CDCNET's NDIS upper edge (cdcnet.c, unchanged)
 * on the fake operating system of shim\ (the shared core's, ethernet.md
 * section 10.3), with a fake lower edge in place of cdcusb.c: the test
 * calls the callbacks cdcnet.c registers as the USB completions would.
 * Host-test code only.
 *
 * What it proves (ethernet.md section 4.4, "Upcalls on 98 SE and ME"):
 * - every accepted send is marked NDIS_STATUS_PENDING;
 * - NT (WDM 1.10): receives are indicated and sends completed at once;
 * - 98 SE / ME: nothing is indicated or completed from a "completion";
 *   the NDIS timer's routine does it, in order; Halt cancels the timer,
 *   completes the queued sends and takes back the receives it never
 *   indicated, with 3, 17 and 64 of them queued.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fakeos.h"
#include "fakewin.h"
#include "cdcusb.h"

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path);

static unsigned long assertions;
static void Check(int ok, const char *expression, int line)
{
    assertions++;
    if (!ok) { printf("FAIL %d: %s\n", line, expression); exit(1); }
}
#define CHECK(x) Check((x) != 0, #x, __LINE__)

/* ---------------- the fake lower edge ---------------- */

static CDC_CALLBACKS cb;
static int starts, stops, kicks;
static int queuePower;                          /* CdcUsbRequest queues a power request */
static int setCompletes;
static const cdc_u8 mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

long CdcUsbStart(void *lowerDevice, const CDC_CALLBACKS *callbacks, CDC_START *start, void **device)
{
    (void)lowerDevice;
    cb = *callbacks;
    memcpy(start->Mac, mac, 6);
    memcpy(start->PermanentMac, mac, 6);
    start->Protocol = CDC_PROTO_RNDIS;
    start->HighSpeed = 1;
    start->LinkSpeed = 1000000;
    start->Connected = 1;
    *device = &cb;
    starts++;
    return 0;
}

void CdcUsbStop(void *device) { (void)device; stops++; }
void CdcUsbKick(void *device) { (void)device; kicks++; }
int CdcUsbRequest(void *device, int request, cdc_u32 value, const cdc_u8 *data, cdc_u32 length)
{
    (void)device; (void)value; (void)data; (void)length;
    /* A queued power request completes when the test calls TaskDone, as
     * the worker would; everything else is done at once. */
    return request == CDC_REQ_POWER && queuePower;
}
void CdcUsbSurpriseRemoved(void *device) { (void)device; }
void CdcTrace(const char *format, ...) { (void)format; }

/* ---------------- the NDIS side ---------------- */

static int inject;                              /* frames to deliver from inside the next indications */
static void Frame(UCHAR fill, UINT length);
static int timing;                              /* inside FakeRunTimer, or Halt */
static int received, completed, early;
static UCHAR lastFrame[1514];
static UINT lastLength;
static NDIS_HANDLE ctx;

static void Receive(PNDIS_PACKET p)
{
    PNDIS_BUFFER b;
    PVOID va;
    UINT len, total;
    if (!Fake.Wdm110 && !timing) early++;
    if (inject > 0) { inject--; Frame(0x70, 90); }   /* a bulk-IN completion during the run */
    NdisQueryPacket(p, NULL, NULL, &b, &total);
    NdisQueryBuffer(b, &va, &len);
    CHECK(len == total && p->HeaderSize == 14);
    memcpy(lastFrame, va, len);
    lastLength = len;
    received++;
    if (p->Status != NDIS_STATUS_RESOURCES) Fake.Chars.ReturnPacketHandler(Fake.Context, p);
}

typedef struct TPACKET {
    NDIS_PACKET P;
    NDIS_BUFFER B;
    UCHAR Data[1514];
    int Completions;
    NDIS_STATUS Status;
} TPACKET;
static TPACKET tp[128];

static void SendComplete(PNDIS_PACKET p, NDIS_STATUS status)
{
    TPACKET *t = (TPACKET *)p;
    CHECK(t >= tp && t < tp + 128);
    if (!Fake.Wdm110 && !timing) early++;
    t->Completions++;
    t->Status = status;
    completed++;
}

static PNDIS_PACKET MakePacket(int i, UINT length)
{
    TPACKET *t = &tp[i];
    UINT k;
    memset(t, 0, sizeof(*t));
    t->P.FakeMagic = 0x50414b54UL;
    for (k = 0; k < length; k++) t->Data[k] = (UCHAR)(i * 3 + k);
    t->B.Va = t->Data;
    t->B.Length = length;
    t->P.Head = &t->B;
    t->P.Status = NDIS_STATUS_SUCCESS;
    return &t->P;
}

static void SetDone(NDIS_STATUS status) { CHECK(status == NDIS_STATUS_SUCCESS); setCompletes++; }

static NDIS_STATUS SetPower(ULONG state)
{
    ULONG r = 0, n = 0;
    return Fake.Chars.SetInformationHandler(ctx, OID_PNP_SET_POWER, &state, 4, &r, &n);
}

static int RunTimer(void)
{
    int r;
    timing++;
    r = FakeRunTimer();
    timing--;
    return r;
}

static void Fresh(int wdm110)
{
    static DEVICE_OBJECT lower;
    DRIVER_OBJECT driver;
    UNICODE_STRING path;
    NDIS_STATUS error, s;
    UINT chosen = 99;
    NDIS_MEDIUM media[1] = { NdisMedium802_3 };
    ULONG filter = NDIS_PACKET_TYPE_DIRECTED, r = 0, n = 0;
    memset(&Fake, 0, sizeof(Fake));
    memset(tp, 0, sizeof(tp));
    received = completed = early = setCompletes = inject = 0;
    queuePower = 0;
    FakeSetComplete = SetDone;
    Fake.Wdm110 = wdm110;
    Fake.Lower = &lower;
    FakeReceive = Receive;
    FakeSendComplete = SendComplete;
    CHECK(DriverEntry(&driver, &path) == STATUS_SUCCESS);
    s = Fake.Chars.InitializeHandler(&error, &chosen, media, 1, (NDIS_HANDLE)&Fake, (NDIS_HANDLE)&Fake);
    CHECK(s == NDIS_STATUS_SUCCESS && chosen == 0);
    ctx = Fake.Context;
    CHECK((Fake.Timer != NULL) == !wdm110);    /* the timer exists on 98 SE/ME only */
    CHECK(Fake.Chars.SetInformationHandler(ctx, OID_GEN_CURRENT_PACKET_FILTER, &filter, 4, &r, &n) ==
          NDIS_STATUS_SUCCESS);
}

static void Frame(UCHAR fill, UINT length)
{
    UCHAR f[1514];
    memset(f, fill, length);
    memcpy(f, mac, 6);
    cb.Receive(cb.Context, f, length);          /* as a bulk-IN completion would */
}

/* The lower edge's transmit path: take what is queued, "send" it, and
 * report it done, as a bulk-OUT completion would. */
static int Transmit(int success)
{
    void *packets[CDC_TX_BATCH];
    cdc_u8 buffer[1514];
    cdc_u32 n, i;
    int total = 0;
    while ((n = cb.TxTake(cb.Context, packets, CDC_TX_BATCH)) != 0) {
        for (i = 0; i < n; i++) CHECK(cb.TxCopy(cb.Context, packets[i], buffer, sizeof(buffer)) > 0);
        cb.TxDone(cb.Context, packets, n, success);
        total += (int)n;
    }
    return total;
}

static void Balanced(void)
{
    CHECK(Fake.Violations == 0);
    CHECK(Fake.NdisMemory == 0);
    CHECK(Fake.Packets == 0 && Fake.Buffers == 0);
    CHECK(Fake.PacketPools == 0 && Fake.BufferPools == 0);
}

static void Nt(void)
{
    PNDIS_PACKET list[4];
    int i;
    Fresh(1);
    Frame(0x41, 100);
    CHECK(received == 1 && lastLength == 100);  /* at once */
    for (i = 0; i < 4; i++) list[i] = MakePacket(i, 200);
    Fake.Chars.SendPacketsHandler(ctx, list, 4);
    for (i = 0; i < 4; i++) CHECK(tp[i].P.Status == NDIS_STATUS_PENDING && tp[i].Completions == 0);
    CHECK(Transmit(1) == 4);
    for (i = 0; i < 4; i++) CHECK(tp[i].Completions == 1 && tp[i].Status == NDIS_STATUS_SUCCESS);
    CHECK(Fake.TimerSets == 0);
    Fake.Chars.HaltHandler(ctx);
    CHECK(stops == starts);
    Balanced();
}

static void Win98(void)
{
    PNDIS_PACKET list[8];
    int i;
    Fresh(0);
    Frame(0x51, 100);
    Frame(0x52, 101);
    Frame(0x53, 102);
    CHECK(received == 0 && Fake.TimerSets == 1);
    CHECK(RunTimer() == 1);
    CHECK(received == 3 && lastLength == 102 && lastFrame[6] == 0x53);   /* in order */
    CHECK(RunTimer() == 0);

    for (i = 0; i < 4; i++) list[i] = MakePacket(i, 300);
    Fake.Chars.SendPacketsHandler(ctx, list, 4);
    for (i = 0; i < 4; i++) CHECK(tp[i].P.Status == NDIS_STATUS_PENDING);
    CHECK(Transmit(1) == 4);
    CHECK(completed == 0);                      /* never from the completion */
    CHECK(RunTimer() == 1);
    for (i = 0; i < 4; i++) CHECK(tp[i].Completions == 1 && tp[i].Status == NDIS_STATUS_SUCCESS);

    /* A send refused at once (bad length) completes from the timer too. */
    list[0] = MakePacket(10, 10);
    Fake.Chars.SendPacketsHandler(ctx, list, 1);
    CHECK(tp[10].Completions == 0);
    CHECK(RunTimer() == 1);
    CHECK(tp[10].Completions == 1 && tp[10].Status == NDIS_STATUS_FAILURE);

    /* A failed transfer: failure, once. */
    list[0] = MakePacket(11, 300);
    Fake.Chars.SendPacketsHandler(ctx, list, 1);
    CHECK(Transmit(0) == 1);
    CHECK(RunTimer() == 1);
    CHECK(tp[11].Completions == 1 && tp[11].Status == NDIS_STATUS_FAILURE);

    /* Halt with a receive and sends queued. */
    Frame(0x54, 80);
    for (i = 0; i < 2; i++) list[i] = MakePacket(20 + i, 300);
    Fake.Chars.SendPacketsHandler(ctx, list, 2);
    CHECK(Transmit(1) == 2);
    CHECK(Fake.Timer->Set);
    CHECK(early == 0);
    timing++;                                   /* Halt may complete sends itself */
    Fake.Chars.HaltHandler(ctx);
    timing--;
    CHECK(received == 3);
    CHECK(tp[20].Completions == 1 && tp[21].Completions == 1);
    CHECK(Fake.TimerCancels == 2 && Fake.TimerSetAtCancel && Fake.Timer == NULL);
    Balanced();
}

/* Halt with more queued receives than one deferred indication (17) and
 * with the whole pool queued (64), no send queued: all go back. */
static void Win98Halt(int frames)
{
    int i;
    Fresh(0);
    for (i = 0; i < frames; i++) Frame((UCHAR)i, 64);
    CHECK(received == 0 && Fake.TimerSets == 1);
    {
        void *t = FwThread((void (*)(void *))Fake.Chars.HaltHandler, ctx);
        CHECK(FwWait(t, 5000) == 0);              /* returns: nothing left out */
        FwClose(t);
    }
    CHECK(received == 0);
    CHECK(Fake.TimerCancels == 2 && Fake.TimerSetAtCancel && Fake.Timer == NULL);
    Balanced();
}

/* 98 SE and ME: a receive queued for the timer when a sleep request
 * comes is taken back, never indicated, and the timer cancelled, before
 * the request completes (queued, and at once); at D0 the timer serves
 * again. */
static void Win98Power(void)
{
    Fresh(0);
    queuePower = 1;
    Frame(0x61, 100);
    CHECK(received == 0 && Fake.Timer->Set);
    CHECK(SetPower(NdisDeviceStateD3) == NDIS_STATUS_PENDING);
    cb.TaskDone(cb.Context, CDC_REQ_POWER, 0);  /* the worker, after its drain */
    CHECK(setCompletes == 1);
    CHECK(!Fake.Timer->Set && Fake.TimerSetAtCancel);
    CHECK(RunTimer() == 0 && received == 0);
    Frame(0x62, 100);                           /* late: queued, no timer while asleep */
    CHECK(!Fake.Timer->Set);
    {
        /* A send accepted while asleep stays queued; the restart's
         * transmit completes it before the worker's TaskDone: from the
         * timer still, never from that completion. */
        PNDIS_PACKET one = MakePacket(40, 200);
        Fake.Chars.SendPacketsHandler(ctx, &one, 1);
        CHECK(tp[40].Completions == 0);
        CHECK(SetPower(NdisDeviceStateD0) == NDIS_STATUS_PENDING);
        CHECK(Transmit(1) == 1 && tp[40].Completions == 0);
    }
    cb.TaskDone(cb.Context, CDC_REQ_POWER, 0);
    CHECK(setCompletes == 2 && Fake.Timer->Set);  /* armed for what waited */
    CHECK(RunTimer() == 1 && received == 1 && lastFrame[6] == 0x62);
    CHECK(tp[40].Completions == 1 && tp[40].Status == NDIS_STATUS_SUCCESS);
    /* The synchronous path (request not queued). */
    queuePower = 0;
    Frame(0x63, 100);
    CHECK(Fake.Timer->Set);
    CHECK(SetPower(NdisDeviceStateD3) == NDIS_STATUS_SUCCESS);
    CHECK(!Fake.Timer->Set && RunTimer() == 0 && received == 1);
    CHECK(SetPower(NdisDeviceStateD0) == NDIS_STATUS_SUCCESS);
    Frame(0x64, 100);
    CHECK(RunTimer() == 1 && received == 2);
    /* The synchronous path with the timer's routine already due (the
     * cancel fails): the request pends, and the routine completes it
     * without indicating what was queued. */
    Frame(0x65, 100);
    Fake.TimerCancelFails = 1;
    CHECK(SetPower(NdisDeviceStateD3) == NDIS_STATUS_PENDING);
    Fake.TimerCancelFails = 0;
    CHECK(setCompletes == 2);
    CHECK(RunTimer() == 1);
    CHECK(setCompletes == 3 && received == 2);
    CHECK(SetPower(NdisDeviceStateD0) == NDIS_STATUS_SUCCESS);
    CHECK(early == 0);
    Fake.Chars.HaltHandler(ctx);
    Balanced();
}

/* The send queue has no limit: a burst of 80, more than the 64 it once
 * held, is accepted whole and completes, every send once and in success;
 * a long array kicks the pump as it goes. A link down fails what is
 * queued, once, and a send while down fails at once. */
static void Queue(void)
{
    PNDIS_PACKET list[80];
    int i, kicked, ok = 0;
    Fresh(1);
    for (i = 0; i < 80; i++) list[i] = MakePacket(i, 200);
    kicked = kicks;
    Fake.Chars.SendPacketsHandler(ctx, list, 80);
    CHECK(kicks - kicked >= 10);
    for (i = 0; i < 80; i++) CHECK(tp[i].Completions == 0);
    CHECK(Transmit(1) == 80);
    for (i = 0; i < 80; i++) if (tp[i].Completions == 1 && tp[i].Status == NDIS_STATUS_SUCCESS) ok++;
    CHECK(ok == 80);
    for (i = 0; i < 3; i++) list[i] = MakePacket(90 + i, 200);
    Fake.Chars.SendPacketsHandler(ctx, list, 3);
    cb.Link(cb.Context, 0, 0);                  /* the lower edge: link down (recovery, sleep) */
    for (i = 0; i < 3; i++) CHECK(tp[90 + i].Completions == 1 && tp[90 + i].Status == NDIS_STATUS_FAILURE);
    CHECK(Transmit(1) == 0);
    list[0] = MakePacket(95, 200);
    Fake.Chars.SendPacketsHandler(ctx, list, 1);
    CHECK(tp[95].Completions == 1 && tp[95].Status == NDIS_STATUS_FAILURE);
    cb.Link(cb.Context, 1, 1000000);
    list[0] = MakePacket(96, 200);
    Fake.Chars.SendPacketsHandler(ctx, list, 1);
    CHECK(Transmit(1) == 1 && tp[96].Completions == 1 && tp[96].Status == NDIS_STATUS_SUCCESS);
    Fake.Chars.HaltHandler(ctx);
    Balanced();
}

/* 98 SE and ME: frames that arrive while the timer's routine indicates
 * are delivered by that same run; the timer is not set from inside it. */
static void Win98Reentry(void)
{
    Fresh(0);
    inject = 0;
    Frame(0x71, 100);
    CHECK(Fake.TimerSets == 1);
    inject = 3;
    CHECK(RunTimer() == 1);
    CHECK(received == 4 && inject == 0 && Fake.TimerSets == 1 && !Fake.Timer->Set);
    Frame(0x72, 100);
    CHECK(Fake.TimerSets == 2 && RunTimer() == 1 && received == 5);
    Fake.Chars.HaltHandler(ctx);
    Balanced();
}

int main(void)
{
    Nt();
    Queue();
    Win98();
    Win98Halt(17);
    Win98Halt(64);
    Win98Power();
    Win98Reentry();
    printf("PASS: %lu CDCNET upper-edge checks (cdcnet.c on the fake backend)\n", assertions);
    return 0;
}

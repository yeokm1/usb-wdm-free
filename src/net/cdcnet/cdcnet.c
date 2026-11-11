/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * CDCNET NDIS upper edge (ethernet.md sections 4.3-4.8). Original code
 * written from the Windows 2000 DDK network guide and ndis.h (NDIS 5.0,
 * x86) and the WDK 7.1 ndis.h (NDIS 5.1, x64); no DDK sample was read.
 *
 * A deserialized NDIS 5 miniport with a WDM lower edge (cdcusb.c):
 * - every packet accepted by SendPackets completes once through
 *   NdisMSendComplete, from the transmit path or a flush; the queue has
 *   no limit (a full queue never fails a send), and a packet while the
 *   link is down or the adapter gone or halting completes with failure at
 *   once;
 * - received frames are copied into a preallocated packet pool and
 *   indicated; a SUCCESS indication transfers ownership until
 *   ReturnPacket, a RESOURCES indication (pool low) is reclaimed on
 *   return using the status saved before the call;
 * - OIDs that need the device (filter, multicast, power) and reset pend
 *   and complete from the lower edge's passive worker.
 *
 * The x86 NDIS 5.0 build only (the NDIS 5.1 x64 build has none of it,
 * ethernet.md section 4.4, as the shared core netcore.c): every accepted
 * send is marked NDIS_STATUS_PENDING, the adapter's lock is the kernel
 * spin lock inside its NDIS_SPIN_LOCK, and on 98 SE and ME (at run time)
 * no receive indication or send completion is made from a USB completion:
 * both are queued and made from an NDIS timer (Upcalls).
 */
#include <ndis.h>
#include "cdcusb.h"

#define TX_LIMIT 64          /* OID_GEN_TRANSMIT_BUFFER_SPACE only: the queue itself is not bounded */
#define TX_KICK 8            /* frames queued between kicks of the pump within one call */
#define RX_PACKETS 64
#define RX_LOW 8
#define RX_STRIDE 1536
#define MC_MAX 32
#define VERSION_MAJOR 0
#define VERSION_MINOR 1
#define RX_BATCH 16                /* 98 SE/ME: receives per deferred indication, at most */

#if !defined(NDIS51_MINIPORT)
#define CDC_DEFER 1
#else
#define CDC_DEFER 0
#endif

typedef struct ADAPTER {
    NDIS_HANDLE Handle;
    void *Usb;
    NDIS_SPIN_LOCK Lock;
    CDC_START Start;
    ULONG Filter, Lookahead, LinkSpeed;
    BOOLEAN Connected, Gone, Halting, Ready;
    BOOLEAN LinkKnown;             /* the lower edge has called Link */
    UCHAR Multicast[MC_MAX][6];
    ULONG MulticastCount;
    PNDIS_PACKET TxHead, TxTail;   /* linked through MiniportReserved */
    ULONG TxQueued;
    ULONG GoneSends, GoneOids;     /* trace only: NDIS calls after the device went */
    NDIS_HANDLE PacketPool, BufferPool;
    PUCHAR RxStorage;
    PNDIS_PACKET RxFree[RX_PACKETS];
    ULONG RxFreeCount;
    LONG RxOut;
    ULONG PowerState;
    ULONG TxOk, TxError, RxOk, RxError, RxNoBuffer;
    ULONG TxBytes, RxBytes;
#if CDC_DEFER
    /* 98 SE and ME (Defer): what the USB completions would have indicated
     * or completed waits here for Upcalls, under Lock. RxPend is a ring of
     * packets taken from RxFree and not yet indicated (it cannot overflow:
     * there are RX_PACKETS packets); DoneHead/DoneTail lists completed
     * sends through NEXT, their status in DONE_STATUS. */
    BOOLEAN Defer;
    BOOLEAN UpcallArmed;           /* the timer is set and its routine has not started */
    BOOLEAN UpcallRunning;         /* Upcalls is running */
    BOOLEAN UpcallStop;            /* Halt: the timer is never set again */
    BOOLEAN UpcallPaused;          /* asleep (D1-D3): no timer, nothing indicated */
    BOOLEAN SleepPending;          /* a sleep request Upcalls completes when it ends */
    NDIS_MINIPORT_TIMER UpcallTimer[2];  /* used in turn: never the one whose routine may still be ending */
    ULONG UpcallNext;              /* the timer the next ArmUpcalls sets */
    PNDIS_PACKET RxPend[RX_PACKETS];
    ULONG RxPendHead, RxPendCount;
    PNDIS_PACKET DoneHead, DoneTail;
    ULONG UpcallRuns;              /* trace */
#endif
} ADAPTER;

#define NEXT(p) (*(PNDIS_PACKET *)&(p)->MiniportReserved[0])

#if CDC_DEFER
/* The adapter's lock is taken from the lower edge's USB completions and
 * from NDIS's own calls. On Windows 98 SE NdisAcquireSpinLock did not keep
 * those completions out of a queue (2026-11-07-rtl8157-hardware.md), so
 * the kernel spin lock inside NDIS_SPIN_LOCK is taken directly: on NT that
 * is what NdisAcquireSpinLock does. */
#define CdcLock(a) KeAcquireSpinLock(&(a)->Lock.SpinLock, &(a)->Lock.OldIrql)
#define CdcUnlock(a) KeReleaseSpinLock(&(a)->Lock.SpinLock, (a)->Lock.OldIrql)
#define DONE_STATUS(p) (*(NDIS_STATUS *)&(p)->MiniportReserved[sizeof(PVOID)])

/* Windows 98 SE: under sustained TCP, receive indications and send
 * completions made straight from the USB completions stalled the 98
 * TCP/IP, and from an NDIS timer they did not
 * (docs/testing/2026-11-07-98se-tcp.md). So on 98 SE and ME the
 * completions only queue (Receive, TxDone) and arm the timer, and Upcalls
 * makes the calls. NT makes them at once. Called with the lock held. */
static void ArmUpcalls(ADAPTER *a)
{
    if (a->UpcallArmed || a->UpcallStop || a->UpcallPaused) return;
    a->UpcallArmed = TRUE;
    /* The other timer from the last one set: a routine that has just
     * cleared UpcallArmed may not have returned yet, and its own timer is
     * not set again (see Upcalls). */
    NdisMSetTimer(&a->UpcallTimer[a->UpcallNext], 0);
    a->UpcallNext ^= 1;
}

/* Complete the queued sends and indicate the queued receives, in order,
 * until both queues are empty. With indicate FALSE (Halt) the receives go
 * back to the pool unindicated. */
static void Deliver(ADAPTER *a, BOOLEAN indicate)
{
    PNDIS_PACKET rx[RX_BATCH], done;
    ULONG n, i, taken;
    for (;;) {
        CdcLock(a);
        done = a->DoneHead;
        a->DoneHead = a->DoneTail = NULL;
        n = 0;
        while (n < RX_BATCH && a->RxPendCount) {
            rx[n++] = a->RxPend[a->RxPendHead];
            a->RxPendHead = (a->RxPendHead + 1) % RX_PACKETS;
            a->RxPendCount--;
        }
        taken = n;
        if (!indicate) {
            for (i = 0; i < n; i++) a->RxFree[a->RxFreeCount++] = rx[i];
            a->RxOut -= (LONG)n;
            n = 0;
        }
        CdcUnlock(a);
        if (!done && !taken) return;
        while (done) {
            PNDIS_PACKET next = NEXT(done);
            NdisMSendComplete(a->Handle, done, DONE_STATUS(done));
            done = next;
        }
        if (n) NdisMIndicateReceivePacket(a->Handle, rx, n);
    }
}

static VOID Upcalls(PVOID s1, PVOID context, PVOID s2, PVOID s3)
{
    ADAPTER *a = context;
    BOOLEAN indicate, sleep = FALSE, more;
    UNREFERENCED_PARAMETER(s1);
    UNREFERENCED_PARAMETER(s2);
    UNREFERENCED_PARAMETER(s3);
    /* UpcallArmed stays TRUE until this routine ends with both queues
     * empty, so ArmUpcalls never sets the timer while it runs: what
     * arrives meanwhile is delivered by the loop here. Setting a timer
     * again from inside its own routine is not relied on: on 98 SE/ME an
     * occasional stall of both directions, and a halt that waited for
     * ever, both fit such a set being lost (UpcallArmed left TRUE). */
    CdcLock(a);
    a->UpcallRunning = TRUE;
    a->UpcallRuns++;
    CdcUnlock(a);
    for (;;) {
        CdcLock(a);
        indicate = !a->Halting && !a->UpcallPaused;
        CdcUnlock(a);
        Deliver(a, indicate);
        CdcLock(a);
        more = a->RxPendCount || a->DoneHead;
        if (!more) {
            a->UpcallArmed = FALSE;
            a->UpcallRunning = FALSE;
            sleep = a->SleepPending;
            a->SleepPending = FALSE;
        }
        CdcUnlock(a);
        if (!more) break;
    }
    if (sleep) {
        /* The sleep request Set pended for this routine (SleepNow). */
        Deliver(a, FALSE);
        NdisMSetInformationComplete(a->Handle, NDIS_STATUS_SUCCESS);
    }
}

/* Halt (halt TRUE), or a sleep request before it completes: the timer is
 * cancelled, as the 2000 DDK's OID_PNP_SET_POWER page asks before a sleep
 * state, and never set again until ResumeUpcalls; a routine already
 * started or due runs to its end, indicating nothing once Halting or
 * UpcallPaused is set; then the queued sends complete and the unindicated
 * receives go back to the pool. PASSIVE_LEVEL (Halt, the worker). */
static void StopUpcalls(ADAPTER *a, BOOLEAN halt)
{
    BOOLEAN cancelled = FALSE, busy;
    ULONG waited = 0;
    CdcLock(a);
    if (halt) a->UpcallStop = TRUE; else a->UpcallPaused = TRUE;
    CdcUnlock(a);
    {
        BOOLEAN c0 = FALSE, c1 = FALSE;   /* only the timer last set can be set */
        NdisMCancelTimer(&a->UpcallTimer[0], &c0);
        NdisMCancelTimer(&a->UpcallTimer[1], &c1);
        cancelled = (BOOLEAN)(c0 || c1);
    }
    CdcLock(a);
    if (cancelled) a->UpcallArmed = FALSE;
    busy = a->UpcallArmed || a->UpcallRunning;
    CdcUnlock(a);
    while (busy) {
        NdisMSleep(1000);
        if (++waited % 5000 == 0) CdcTrace("upcall routine still due");
        CdcLock(a);
        busy = a->UpcallArmed || a->UpcallRunning;
        CdcUnlock(a);
    }
    Deliver(a, FALSE);
}

/* A sleep request Set completes itself (the request could not be queued
 * to the worker, so there is no passive context to wait in): delivery
 * stops as in StopUpcalls. TRUE when a routine already started or due
 * must end first: Set then pends the request and that routine completes
 * it (Upcalls); FALSE when it is done here. */
static BOOLEAN SleepNow(ADAPTER *a)
{
    BOOLEAN cancelled = FALSE, busy;
    CdcLock(a);
    a->UpcallPaused = TRUE;
    CdcUnlock(a);
    {
        BOOLEAN c0 = FALSE, c1 = FALSE;   /* only the timer last set can be set */
        NdisMCancelTimer(&a->UpcallTimer[0], &c0);
        NdisMCancelTimer(&a->UpcallTimer[1], &c1);
        cancelled = (BOOLEAN)(c0 || c1);
    }
    CdcLock(a);
    if (cancelled) a->UpcallArmed = FALSE;
    busy = a->UpcallArmed || a->UpcallRunning;
    if (busy) a->SleepPending = TRUE;
    CdcUnlock(a);
    if (!busy) Deliver(a, FALSE);
    return busy;
}

/* Back at D0: deferred delivery again, for anything queued meanwhile. */
static void ResumeUpcalls(ADAPTER *a)
{
    CdcLock(a);
    a->UpcallPaused = FALSE;
    if (a->RxPendCount || a->DoneHead) ArmUpcalls(a);
    CdcUnlock(a);
}
#else
#define CdcLock(a) NdisAcquireSpinLock(&(a)->Lock)
#define CdcUnlock(a) NdisReleaseSpinLock(&(a)->Lock)
#endif

/* Live adapters: from a successful Initialize to the start of Halt. On
 * Windows 98 SE under NUSB's EHCI stack, NDIS called the set handler
 * (OID_PNP_SET_POWER) after Halt had freed the adapter; Query and Set
 * check their context here first and never touch a halted one. */
#define LIVE_MAX 16
static ADAPTER *live[LIVE_MAX];
static NDIS_SPIN_LOCK liveLock;
static ULONG staleCalls;

static void LiveAdd(ADAPTER *a)
{
    ULONG i;
    NdisAcquireSpinLock(&liveLock);
    for (i = 0; i < LIVE_MAX; i++) if (!live[i]) { live[i] = a; break; }
    NdisReleaseSpinLock(&liveLock);
}

static void LiveDrop(ADAPTER *a)
{
    ULONG i;
    NdisAcquireSpinLock(&liveLock);
    for (i = 0; i < LIVE_MAX; i++) if (live[i] == a) live[i] = NULL;
    NdisReleaseSpinLock(&liveLock);
}

/* TRUE when context is a live adapter. A full table (more than LIVE_MAX
 * adapters) is not checked: every context then counts as live. */
static BOOLEAN Live(void *context, const char *what, NDIS_OID oid)
{
    ULONG i, used = 0;
    BOOLEAN found = FALSE;
    NdisAcquireSpinLock(&liveLock);
    for (i = 0; i < LIVE_MAX; i++) {
        if (live[i]) used++;
        if (live[i] == context) found = TRUE;
    }
    NdisReleaseSpinLock(&liveLock);
    if (found || used == LIVE_MAX) return TRUE;
    if (++staleCalls <= 20) CdcTrace("%s oid %x on a halted adapter: refused", what, oid);
    return FALSE;
}

static NDIS_OID supported[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE, OID_GEN_MAXIMUM_LOOKAHEAD, OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_LINK_SPEED, OID_GEN_TRANSMIT_BUFFER_SPACE, OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION, OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD, OID_GEN_DRIVER_VERSION, OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_MAC_OPTIONS, OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS,
    OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER, OID_GEN_DIRECTED_BYTES_XMIT, OID_GEN_DIRECTED_BYTES_RCV,
    OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS, OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE, OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION, OID_802_3_XMIT_MORE_COLLISIONS,
    OID_PNP_CAPABILITIES, OID_PNP_QUERY_POWER, OID_PNP_SET_POWER
};

/* ---------------- callbacks from the lower edge ---------------- */

static void Receive(void *context, const cdc_u8 *frame, cdc_u32 length)
{
    ADAPTER *a = context;
    PNDIS_PACKET packet = NULL;
    PNDIS_BUFFER buffer;
    PVOID storage;
    UINT size;
    ULONG filter, i;
    BOOLEAN accept = FALSE, low = FALSE;
    NDIS_STATUS saved;

    CdcLock(a);
    filter = a->Filter;
    if (a->Halting || !filter) {
        CdcUnlock(a);
        return;
    }
    /* Software filter: a device that ignores the CDC filter requests
     * still delivers only what NDIS asked for. */
    if (filter & NDIS_PACKET_TYPE_PROMISCUOUS) accept = TRUE;
    else if (frame[0] & 1) {
        BOOLEAN broadcast = TRUE;
        for (i = 0; i < 6; i++) if (frame[i] != 0xff) broadcast = FALSE;
        if (broadcast) accept = (filter & NDIS_PACKET_TYPE_BROADCAST) != 0;
        else if (filter & NDIS_PACKET_TYPE_ALL_MULTICAST) accept = TRUE;
        else if (filter & NDIS_PACKET_TYPE_MULTICAST)
            for (i = 0; i < a->MulticastCount; i++)
                if (NdisEqualMemory(frame, a->Multicast[i], 6)) { accept = TRUE; break; }
    } else {
        accept = (filter & NDIS_PACKET_TYPE_DIRECTED) && NdisEqualMemory(frame, a->Start.Mac, 6);
    }
    if (accept) {
        if (a->RxFreeCount) {
            packet = a->RxFree[--a->RxFreeCount];
            low = a->RxFreeCount < RX_LOW;
            a->RxOut++;
            a->RxOk++;
            a->RxBytes += length;
        } else {
            a->RxNoBuffer++;
        }
    }
    CdcUnlock(a);
    if (!packet) return;

    NdisQueryPacket(packet, NULL, NULL, &buffer, NULL);
    NdisQueryBuffer(buffer, &storage, &size);
    NdisMoveMemory(storage, frame, length);
    NdisAdjustBufferLength(buffer, length);
    NdisRecalculatePacketCounts(packet);
    NDIS_SET_PACKET_HEADER_SIZE(packet, 14);
#if CDC_DEFER
    if (a->Defer) {
        /* Indicated by Upcalls, always with NDIS_STATUS_SUCCESS: the
         * RESOURCES path below needs the indication inside this call. */
        NDIS_SET_PACKET_STATUS(packet, NDIS_STATUS_SUCCESS);
        CdcLock(a);
        a->RxPend[(a->RxPendHead + a->RxPendCount++) % RX_PACKETS] = packet;
        ArmUpcalls(a);
        CdcUnlock(a);
        return;
    }
#endif
    saved = low ? NDIS_STATUS_RESOURCES : NDIS_STATUS_SUCCESS;
    NDIS_SET_PACKET_STATUS(packet, saved);
    NdisMIndicateReceivePacket(a->Handle, &packet, 1);
    /* The status saved before the call decides ownership, never the
     * status after it (NDIS 5 receive indication contract). */
    if (saved == NDIS_STATUS_RESOURCES) {
        CdcLock(a);
        a->RxFree[a->RxFreeCount++] = packet;
        a->RxOut--;
        CdcUnlock(a);
    }
}

static VOID ReturnPacket(NDIS_HANDLE context, PNDIS_PACKET packet)
{
    ADAPTER *a = context;
    CdcLock(a);
    a->RxFree[a->RxFreeCount++] = packet;
    a->RxOut--;
    CdcUnlock(a);
}

static cdc_u32 TxTake(void *context, void **packets, cdc_u32 max)
{
    ADAPTER *a = context;
    cdc_u32 n = 0;
    CdcLock(a);
    while (n < max && a->TxHead) {
        PNDIS_PACKET p = a->TxHead;
        a->TxHead = NEXT(p);
        if (!a->TxHead) a->TxTail = NULL;
        a->TxQueued--;
        packets[n++] = p;
    }
    CdcUnlock(a);
    return n;
}

static cdc_u32 TxCopy(void *context, void *packet, cdc_u8 *bytes, cdc_u32 capacity)
{
    PNDIS_BUFFER b;
    UINT total, copied = 0;
    UNREFERENCED_PARAMETER(context);
    NdisQueryPacket((PNDIS_PACKET)packet, NULL, NULL, &b, &total);
    if (total < CDC_ETH_MIN || total > capacity) return 0;
    while (b) {
        PVOID address;
        UINT size;
        NdisQueryBuffer(b, &address, &size);
        if (size > total - copied || (size && !address)) return 0;
        if (size) NdisMoveMemory(bytes + copied, address, size);
        copied += size;
        NdisGetNextBuffer(b, &b);
    }
    return copied == total ? total : 0;
}

static void TxDone(void *context, void **packets, cdc_u32 count, int success)
{
    ADAPTER *a = context;
    cdc_u32 i;
    for (i = 0; i < count; i++) {
        PNDIS_PACKET p = packets[i];
        UINT size;
        NdisQueryPacket(p, NULL, NULL, NULL, &size);
        CdcLock(a);
        if (success) { a->TxOk++; a->TxBytes += size; } else a->TxError++;
#if CDC_DEFER
        /* Queued for Upcalls, except asleep: no USB completion runs then
         * (the worker drained them), so a send SendPackets or a flush
         * fails completes at once, below. */
        if (a->Defer && !a->UpcallPaused) {
            NEXT(p) = NULL;
            DONE_STATUS(p) = success ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE;
            if (a->DoneTail) NEXT(a->DoneTail) = p; else a->DoneHead = p;
            a->DoneTail = p;
            ArmUpcalls(a);
            CdcUnlock(a);
            continue;
        }
#endif
        CdcUnlock(a);
        NdisMSendComplete(a->Handle, p, success ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE);
    }
}

/* Complete every queued send with failure (gone, halting). */
static void Flush(ADAPTER *a)
{
    void *packets[CDC_TX_BATCH];
    cdc_u32 n;
    while ((n = TxTake(a, packets, CDC_TX_BATCH)) != 0) TxDone(a, packets, n, 0);
}

static void Link(void *context, int connected, cdc_u32 speed)
{
    ADAPTER *a = context;
    BOOLEAN changed;
    CdcLock(a);
    if (connected < 0) a->Gone = TRUE;
    if (speed) a->LinkSpeed = speed;
    changed = a->Connected != (connected > 0);
    a->LinkKnown = TRUE;
    a->Connected = connected > 0;
    CdcUnlock(a);
    if (connected <= 0) Flush(a);
    /* No indication before MiniportInitialize has returned. */
    if (changed && a->Ready) {
#if defined(CDC_TRACE_E9) || DBG
        CdcTrace("indicate %s", connected > 0 ? "MEDIA_CONNECT" : "MEDIA_DISCONNECT");
#endif
        NdisMIndicateStatus(a->Handle, connected > 0 ? NDIS_STATUS_MEDIA_CONNECT :
                            NDIS_STATUS_MEDIA_DISCONNECT, NULL, 0);
        NdisMIndicateStatusComplete(a->Handle);
    }
}

static void TaskDone(void *context, int request, long status)
{
    ADAPTER *a = context;
    if (request == CDC_REQ_RESET) {
        /* A reset that could not bring the device back after a failed
         * resume (cdcusb.c Rebuild) is reported as failed; any other
         * completes as before. */
        NdisMResetComplete(a->Handle, status == CDC_E_BROKEN ? NDIS_STATUS_HARD_ERRORS : NDIS_STATUS_SUCCESS,
                           FALSE);
        return;
    }
#if CDC_DEFER
    /* 98 SE and ME: before a sleep request completes, nothing may be left
     * for the timer to indicate (D0 resumed in Set, before the worker). */
    if (a->Defer && request == CDC_REQ_POWER && a->PowerState != NdisDeviceStateD0) StopUpcalls(a, FALSE);
#endif
    /* A filter or multicast request the device refused still stands in
     * the software filter, which keeps delivery correct; a power change
     * always completes, the adapter recovering or reporting itself gone. */
    NdisMSetInformationComplete(a->Handle, NDIS_STATUS_SUCCESS);
}

/* ---------------- NDIS handlers ---------------- */

static NDIS_STATUS Query(NDIS_HANDLE context, NDIS_OID oid, PVOID buffer,
                         ULONG length, PULONG written, PULONG needed)
{
    ADAPTER *a = context;
    ULONG value = 0, size = sizeof(value);
    ULONGLONG wide = 0;
    USHORT version = 0x0500;
    PVOID data = &value;
    NDIS_PNP_CAPABILITIES pm;
    static CHAR description[] = "usb-wdm-free CDCNET (CDC-ECM, CDC-NCM, RNDIS)";
    BOOLEAN counter = FALSE;
    *written = 0; *needed = 0;
    if (!Live(a, "query", oid)) return NDIS_STATUS_ADAPTER_REMOVED;
    switch (oid) {
    case OID_GEN_SUPPORTED_LIST: data = supported; size = sizeof(supported); break;
    case OID_GEN_HARDWARE_STATUS: value = NdisHardwareStatusReady; break;
    case OID_GEN_MEDIA_SUPPORTED: case OID_GEN_MEDIA_IN_USE: value = NdisMedium802_3; break;
    case OID_GEN_MAXIMUM_LOOKAHEAD: case OID_GEN_MAXIMUM_FRAME_SIZE: value = 1500; break;
    case OID_GEN_MAXIMUM_TOTAL_SIZE: case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE: value = CDC_ETH_MAX; break;
    case OID_GEN_LINK_SPEED: value = a->LinkSpeed; break;
    case OID_GEN_TRANSMIT_BUFFER_SPACE: value = TX_LIMIT * CDC_ETH_MAX; break;
    case OID_GEN_RECEIVE_BUFFER_SPACE: value = RX_PACKETS * CDC_ETH_MAX; break;
    case OID_GEN_VENDOR_ID:
        value = a->Start.PermanentMac[0] | (a->Start.PermanentMac[1] << 8) | (a->Start.PermanentMac[2] << 16);
        break;
    case OID_GEN_VENDOR_DESCRIPTION: data = description; size = sizeof(description); break;
    case OID_GEN_VENDOR_DRIVER_VERSION: value = (VERSION_MAJOR << 16) | VERSION_MINOR; break;
    case OID_GEN_CURRENT_PACKET_FILTER: value = a->Filter; break;
    case OID_GEN_CURRENT_LOOKAHEAD: value = a->Lookahead; break;
    case OID_GEN_DRIVER_VERSION: data = &version; size = sizeof(version); break;
    case OID_GEN_MAC_OPTIONS:
        value = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                NDIS_MAC_OPTION_NO_LOOPBACK;
        break;
    case OID_GEN_MEDIA_CONNECT_STATUS:
        value = a->Connected ? NdisMediaStateConnected : NdisMediaStateDisconnected; break;
    case OID_GEN_MAXIMUM_SEND_PACKETS: value = CDC_TX_BATCH; break;
    case OID_GEN_XMIT_OK: wide = a->TxOk; counter = TRUE; break;
    case OID_GEN_RCV_OK: wide = a->RxOk; counter = TRUE; break;
    case OID_GEN_XMIT_ERROR: wide = a->TxError; counter = TRUE; break;
    case OID_GEN_RCV_ERROR: wide = a->RxError; counter = TRUE; break;
    case OID_GEN_RCV_NO_BUFFER: wide = a->RxNoBuffer; counter = TRUE; break;
    case OID_GEN_DIRECTED_BYTES_XMIT: wide = a->TxBytes; counter = TRUE; break;
    case OID_GEN_DIRECTED_BYTES_RCV: wide = a->RxBytes; counter = TRUE; break;
    case OID_802_3_PERMANENT_ADDRESS: data = a->Start.PermanentMac; size = 6; break;
    case OID_802_3_CURRENT_ADDRESS: data = a->Start.Mac; size = 6; break;
    case OID_802_3_MULTICAST_LIST: data = a->Multicast; size = a->MulticastCount * 6; break;
    case OID_802_3_MAXIMUM_LIST_SIZE: value = MC_MAX; break;
    case OID_802_3_RCV_ERROR_ALIGNMENT: case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS: counter = TRUE; break;
    case OID_PNP_CAPABILITIES:
        /* Power management aware, with no wake-up (section 4.7). */
        NdisZeroMemory(&pm, sizeof(pm));
        pm.WakeUpCapabilities.MinMagicPacketWakeUp = NdisDeviceStateUnspecified;
        pm.WakeUpCapabilities.MinPatternWakeUp = NdisDeviceStateUnspecified;
        pm.WakeUpCapabilities.MinLinkChangeWakeUp = NdisDeviceStateUnspecified;
        data = &pm; size = sizeof(pm);
        break;
    case OID_PNP_QUERY_POWER: size = 0; break;
    default: return NDIS_STATUS_NOT_SUPPORTED;
    }
    if (counter) {
        /* 64-bit when the caller has room for it, else 32-bit. */
        if (length >= sizeof(ULONGLONG)) { data = &wide; size = sizeof(ULONGLONG); }
        else { value = (ULONG)wide; data = &value; size = sizeof(ULONG); }
    }
    if (length < size) { *needed = size; return NDIS_STATUS_INVALID_LENGTH; }
    if (size) NdisMoveMemory(buffer, data, size);
    *written = size;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS Set(NDIS_HANDLE context, NDIS_OID oid, PVOID buffer,
                       ULONG length, PULONG read, PULONG needed)
{
    ADAPTER *a = context;
    ULONG value;
    *read = 0; *needed = 0;
    if (!Live(a, "set", oid)) { *read = length; return NDIS_STATUS_SUCCESS; }
    if (a->Gone && ++a->GoneOids <= 20) CdcTrace("gone: set oid %x", oid);
    switch (oid) {
    case OID_802_3_MULTICAST_LIST:
        if (length % 6) { *needed = (length / 6 + 1) * 6; return NDIS_STATUS_INVALID_LENGTH; }
        if (length > sizeof(a->Multicast)) return NDIS_STATUS_MULTICAST_FULL;
        CdcLock(a);
        if (length) NdisMoveMemory(a->Multicast, buffer, length);
        a->MulticastCount = length / 6;
        CdcUnlock(a);
        *read = length;
        if (a->Gone || !CdcUsbRequest(a->Usb, CDC_REQ_MULTICAST, 0, buffer, length))
            return NDIS_STATUS_SUCCESS;
        return NDIS_STATUS_PENDING;
    case OID_GEN_CURRENT_PACKET_FILTER:
    case OID_GEN_CURRENT_LOOKAHEAD:
    case OID_PNP_SET_POWER:
        if (length < sizeof(value)) { *needed = sizeof(value); return NDIS_STATUS_INVALID_LENGTH; }
        NdisMoveMemory(&value, buffer, sizeof(value));
        *read = sizeof(value);
        if (oid == OID_GEN_CURRENT_LOOKAHEAD) {
            if (value > 1500) return NDIS_STATUS_INVALID_DATA;
            a->Lookahead = value;
            return NDIS_STATUS_SUCCESS;
        }
        if (oid == OID_PNP_SET_POWER) {
            a->PowerState = value;
#if CDC_DEFER
            /* 98 SE and ME: deferred delivery is back before the worker
             * reopens the device, so no USB completion of the restart
             * reaches NDIS directly. */
            if (a->Defer && value == NdisDeviceStateD0) ResumeUpcalls(a);
#endif
            if (a->Gone || !CdcUsbRequest(a->Usb, CDC_REQ_POWER, value == NdisDeviceStateD0 ? 1 : 0, NULL, 0)) {
#if CDC_DEFER
                /* Completed here, or by a routine still due (SleepNow). */
                if (a->Defer && value != NdisDeviceStateD0 && SleepNow(a)) return NDIS_STATUS_PENDING;
#endif
                return NDIS_STATUS_SUCCESS;
            }
            return NDIS_STATUS_PENDING;
        }
        if (value & ~(NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST |
                      NDIS_PACKET_TYPE_MULTICAST | NDIS_PACKET_TYPE_ALL_MULTICAST |
                      NDIS_PACKET_TYPE_PROMISCUOUS))
            return NDIS_STATUS_NOT_SUPPORTED;
        CdcLock(a);
        a->Filter = value;
        CdcUnlock(a);
        if (a->Gone || !CdcUsbRequest(a->Usb, CDC_REQ_FILTER, value, NULL, 0))
            return NDIS_STATUS_SUCCESS;
        return NDIS_STATUS_PENDING;
    case OID_GEN_PROTOCOL_OPTIONS:
    case OID_GEN_NETWORK_LAYER_ADDRESSES:
    case OID_GEN_TRANSPORT_HEADER_OFFSET:
        *read = length;
        return NDIS_STATUS_SUCCESS;
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static VOID SendPackets(NDIS_HANDLE context, PPNDIS_PACKET packets, UINT count)
{
    ADAPTER *a = context;
    UINT i;
    for (i = 0; i < count; i++) {
        PNDIS_PACKET p = packets[i];
        UINT size;
        BOOLEAN queued = FALSE;
#if CDC_DEFER
        /* Pending until NdisMSendComplete (the deserialized rules allow
         * the Status member to be set). Windows 98 SE's NDIS reads it: a
         * packet left at another value is handed back to the protocol
         * while it is still queued here, and reused. */
        NDIS_SET_PACKET_STATUS(p, NDIS_STATUS_PENDING);
#endif
        NdisQueryPacket(p, NULL, NULL, NULL, &size);
        CdcLock(a);
        /* No limit on the queue: a deserialized miniport keeps every valid
         * send until it can go (NDIS 5 rules); the packets themselves are
         * the queue (MiniportReserved), so nothing is allocated. Only a
         * gone, halting or disconnected adapter or a bad length fails one. */
        if (!a->Gone && !a->Halting && a->Connected && size >= CDC_ETH_MIN && size <= CDC_ETH_MAX) {
            NEXT(p) = NULL;
            if (a->TxTail) NEXT(a->TxTail) = p; else a->TxHead = p;
            a->TxTail = p;
            a->TxQueued++;
            queued = TRUE;
        }
        CdcUnlock(a);
        if (!queued) {
            void *one = p;
            if (a->Gone) {
                ULONG n = ++a->GoneSends;
                if (n == 1 || n == 10 || n == 100 || n == 1000 || n % 10000 == 0)
                    CdcTrace("gone: %u sends failed", n);
            }
            TxDone(a, &one, 1, 0);
        } else if (!((i + 1) % TX_KICK)) {
            CdcUsbKick(a->Usb);   /* a long array starts going out at once */
        }
    }
    CdcUsbKick(a->Usb);
}

static NDIS_STATUS Reset(PBOOLEAN addressing, NDIS_HANDLE context)
{
    ADAPTER *a = context;
    *addressing = FALSE;
    CdcTrace("reset%s", a->Gone ? " (gone)" : "");
    if (a->Gone || !CdcUsbRequest(a->Usb, CDC_REQ_RESET, 0, NULL, 0)) return NDIS_STATUS_SUCCESS;
    return NDIS_STATUS_PENDING;
}

static void FreeAdapter(ADAPTER *a)
{
    ULONG i;
    for (i = 0; i < a->RxFreeCount; i++) {
        PNDIS_BUFFER b;
        NdisUnchainBufferAtFront(a->RxFree[i], &b);
        if (b) NdisFreeBuffer(b);
        NdisFreePacket(a->RxFree[i]);
    }
    if (a->BufferPool) NdisFreeBufferPool(a->BufferPool);
    if (a->PacketPool) NdisFreePacketPool(a->PacketPool);
    if (a->RxStorage) NdisFreeMemory(a->RxStorage, RX_PACKETS * RX_STRIDE, 0);
    NdisFreeSpinLock(&a->Lock);
    NdisFreeMemory(a, sizeof(*a), 0);
}

static VOID Halt(NDIS_HANDLE context)
{
    ADAPTER *a = context;
    ULONG waited = 0;
    CdcTrace("halt: begin");
    LiveDrop(a);
    CdcLock(a);
    a->Halting = TRUE;
    CdcUnlock(a);
    CdcUsbStop(a->Usb);          /* no USB activity or callback after this */
    a->Usb = NULL;
    Flush(a);
#if CDC_DEFER
    if (a->Defer) {
        StopUpcalls(a, TRUE);
        CdcTrace("halt: %u upcall runs", a->UpcallRuns);
    }
#endif
    /* Every indicated packet comes back before its pool is freed. */
    while (a->RxOut > 0) {
        NdisMSleep(10000);
        if (++waited % 500 == 0) CdcTrace("halt: %d receive packets still out", a->RxOut);
    }
    CdcTrace("halted: tx %u/%u rx %u nobuf %u", a->TxOk, a->TxError, a->RxOk, a->RxNoBuffer);
    FreeAdapter(a);
}

#if defined(NDIS51_MINIPORT)
static VOID PnPEvent(NDIS_HANDLE context, NDIS_DEVICE_PNP_EVENT event, PVOID buffer, ULONG length)
{
    ADAPTER *a = context;
    UNREFERENCED_PARAMETER(buffer);
    UNREFERENCED_PARAMETER(length);
    if (event == NdisDevicePnPEventSurpriseRemoved) {
        CdcLock(a);
        a->Gone = TRUE;
        CdcUnlock(a);
        CdcUsbSurpriseRemoved(a->Usb);
        Flush(a);
    }
}

static VOID Shutdown(NDIS_HANDLE context)
{
    UNREFERENCED_PARAMETER(context);  /* may run at high IRQL: touch nothing */
}
#endif

/* The Advanced tab's NetworkAddress (ethernet.md 4.8, 4.10): a unicast
 * address for an NCM function that can take it (cdcusb.c Bringup). */
static void ReadAddress(NDIS_HANDLE config, CDC_START *start)
{
    NDIS_HANDLE handle;
    NDIS_STATUS status;
    PVOID address = NULL;
    UINT length = 0;
    start->HaveWantMac = 0;
    NdisOpenConfiguration(&status, &handle, config);
    if (status != NDIS_STATUS_SUCCESS) return;
    NdisReadNetworkAddress(&status, &address, &length, handle);
    if (status == NDIS_STATUS_SUCCESS && length == 6 && !(((PUCHAR)address)[0] & 1)) {
        PUCHAR m = address;
        if (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) {
            NdisMoveMemory(start->WantMac, address, 6);
            start->HaveWantMac = 1;
        }
    }
    NdisCloseConfiguration(handle);
}

static unsigned ReadProtocol(NDIS_HANDLE config)
{
    NDIS_HANDLE handle;
    NDIS_STATUS status;
    PNDIS_CONFIGURATION_PARAMETER p;
    NDIS_STRING name = NDIS_STRING_CONST("Protocol");
    unsigned mask = CDC_ALLOW(CDC_PROTO_ECM) | CDC_ALLOW(CDC_PROTO_NCM) | CDC_ALLOW(CDC_PROTO_RNDIS);
    NdisOpenConfiguration(&status, &handle, config);
    if (status != NDIS_STATUS_SUCCESS) { CdcTrace("NdisOpenConfiguration %x", status); return mask; }
    NdisReadConfiguration(&status, &p, handle, &name, NdisParameterInteger);
    CdcTrace("Protocol parameter: status %x type %u value %u", status,
             status == NDIS_STATUS_SUCCESS ? p->ParameterType : 0,
             status == NDIS_STATUS_SUCCESS ? p->ParameterData.IntegerData : 0);
    /* 0 automatic (NCM, then ECM, then RNDIS), 1 RNDIS, 2 ECM, 3 NCM. */
    if (status == NDIS_STATUS_SUCCESS && p->ParameterData.IntegerData >= 1 &&
        p->ParameterData.IntegerData <= 3)
        mask = CDC_ALLOW(p->ParameterData.IntegerData);
    NdisCloseConfiguration(handle);
    return mask;
}

static NDIS_STATUS Initialize(PNDIS_STATUS error, PUINT chosen, PNDIS_MEDIUM media, UINT count,
                              NDIS_HANDLE handle, NDIS_HANDLE config)
{
    ADAPTER *a;
    UINT i;
    PDEVICE_OBJECT pdo = NULL, fdo = NULL, lower = NULL;
    NDIS_STATUS status;
    NDIS_PHYSICAL_ADDRESS highest = NDIS_PHYSICAL_ADDRESS_CONST(-1, -1);
    CDC_CALLBACKS cb;
    *error = NDIS_STATUS_SUCCESS;
    for (i = 0; i < count; i++) if (media[i] == NdisMedium802_3) break;
    if (i == count) return NDIS_STATUS_UNSUPPORTED_MEDIA;
    *chosen = i;
    status = NdisAllocateMemory((PVOID *)&a, sizeof(*a), 0, highest);
    if (status != NDIS_STATUS_SUCCESS) return status;
    NdisZeroMemory(a, sizeof(*a));
    a->Handle = handle;
    a->Lookahead = 1500;
    a->PowerState = NdisDeviceStateD0;
    NdisAllocateSpinLock(&a->Lock);
#if CDC_DEFER
    KeInitializeSpinLock(&a->Lock.SpinLock);   /* 98 SE: NDIS need not initialise it */
#endif

    /* Receive pool: packets, buffers and storage, all before USB starts. */
    NdisAllocatePacketPool(&status, &a->PacketPool, RX_PACKETS, 4 * sizeof(PVOID));
    if (status == NDIS_STATUS_SUCCESS) NdisAllocateBufferPool(&status, &a->BufferPool, RX_PACKETS);
    if (status == NDIS_STATUS_SUCCESS)
        status = NdisAllocateMemory((PVOID *)&a->RxStorage, RX_PACKETS * RX_STRIDE, 0, highest);
    for (i = 0; status == NDIS_STATUS_SUCCESS && i < RX_PACKETS; i++) {
        PNDIS_PACKET p;
        PNDIS_BUFFER b;
        NdisAllocatePacket(&status, &p, a->PacketPool);
        if (status != NDIS_STATUS_SUCCESS) break;
        NdisAllocateBuffer(&status, &b, a->BufferPool, a->RxStorage + i * RX_STRIDE, CDC_ETH_MAX);
        if (status != NDIS_STATUS_SUCCESS) { NdisFreePacket(p); break; }
        NdisChainBufferAtFront(p, b);
        a->RxFree[a->RxFreeCount++] = p;
    }
    if (status != NDIS_STATUS_SUCCESS) { FreeAdapter(a); return NDIS_STATUS_RESOURCES; }

    NdisMSetAttributesEx(handle, a, 0,
                         NDIS_ATTRIBUTE_DESERIALIZE
#if defined(NDIS51_MINIPORT)
                         | NDIS_ATTRIBUTE_SURPRISE_REMOVE_OK
#endif
                         , NdisInterfaceInternal);
#if CDC_DEFER
    /* 98 SE and ME: the WDM 1.10 query fails there. The timer is set only
     * once NDIS can send or set a filter, which is after Initialize
     * returns, so no failure path below has one to cancel. */
    a->Defer = !IoIsWdmVersionAvailable(1, 0x10);
    if (a->Defer) {
        NdisMInitializeTimer(&a->UpcallTimer[0], handle, Upcalls, a);
        NdisMInitializeTimer(&a->UpcallTimer[1], handle, Upcalls, a);
    }
#endif
    NdisMGetDeviceProperty(handle, &pdo, &fdo, &lower, NULL, NULL);
    if (!pdo || !fdo || !lower) { FreeAdapter(a); return NDIS_STATUS_ADAPTER_NOT_FOUND; }

    a->Start.AllowMask = ReadProtocol(config);
    ReadAddress(config, &a->Start);
    cb.Context = a; cb.Receive = Receive; cb.TxTake = TxTake; cb.TxCopy = TxCopy;
    cb.TxDone = TxDone; cb.Link = Link; cb.TaskDone = TaskDone;
    if (CdcUsbStart(lower, &cb, &a->Start, &a->Usb) < 0) {
        CdcTrace("initialize failed: stage %u status %x", a->Start.Failure, a->Start.Status);
        NdisWriteErrorLogEntry(handle, NDIS_ERROR_CODE_ADAPTER_NOT_FOUND, 2,
                               a->Start.Failure, (ULONG)a->Start.Status);
        FreeAdapter(a);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }
    if (a->Start.HaveWantMac && !a->Start.AddressSet) {
        /* ECM and RNDIS define no way to set it; an NCM function may not
         * offer it or may refuse it (section 4.10). */
        CdcTrace("NetworkAddress not taken by this function (protocol %u)", a->Start.Protocol);
        NdisWriteErrorLogEntry(handle, NDIS_ERROR_CODE_UNSUPPORTED_CONFIGURATION, 1, 0x4e41UL);
    }
    CdcLock(a);
    /* Link may have been called already (the worker runs from
     * CdcUsbStart on): the latest state stands over the snapshot. */
    if (!a->LinkKnown) a->Connected = a->Start.Connected != 0;
    if (!a->LinkSpeed) a->LinkSpeed = a->Start.LinkSpeed;
    a->Ready = TRUE;
    CdcUnlock(a);
    LiveAdd(a);
#if CDC_DEFER
    CdcTrace("initialized: protocol %u, %s speed, link %u%s", a->Start.Protocol,
             a->Start.HighSpeed ? "high" : "full", a->Connected, a->Defer ? ", upcalls deferred" : "");
#else
    CdcTrace("initialized: protocol %u, %s speed, link %u", a->Start.Protocol,
             a->Start.HighSpeed ? "high" : "full", a->Connected);
#endif
    return NDIS_STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    NDIS_HANDLE wrapper = NULL;
    NDIS_MINIPORT_CHARACTERISTICS chars;
    NDIS_STATUS status;
    NdisMInitializeWrapper(&wrapper, driver, path, NULL);
    if (!wrapper) return STATUS_INSUFFICIENT_RESOURCES;
    NdisAllocateSpinLock(&liveLock);
    NdisZeroMemory(&chars, sizeof(chars));
#if defined(NDIS51_MINIPORT)
    chars.MajorNdisVersion = 5; chars.MinorNdisVersion = 1;
    chars.PnPEventNotifyHandler = PnPEvent;
    chars.AdapterShutdownHandler = Shutdown;
#else
    chars.MajorNdisVersion = 5; chars.MinorNdisVersion = 0;
#endif
    chars.InitializeHandler = Initialize;
    chars.HaltHandler = Halt;
    chars.QueryInformationHandler = Query;
    chars.SetInformationHandler = Set;
    chars.ResetHandler = Reset;
    chars.ReturnPacketHandler = ReturnPacket;
    chars.SendPacketsHandler = SendPackets;
    status = NdisMRegisterMiniport(wrapper, &chars, sizeof(chars));
    CdcTrace("DriverEntry: register %x", status);
    if (status != NDIS_STATUS_SUCCESS) NdisTerminateWrapper(wrapper, NULL);
    return status;
}

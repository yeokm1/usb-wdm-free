/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * USB Ethernet core, NDIS 5.0 upper edge (ethernet.md sections 4.3-4.8).
 * Written from the Windows 2000 DDK network guide and ndis.h; no DDK
 * sample was read. Derived from cdcnet's cdcnet.c (same author and
 * licence), with the address policy of section 4.8 added. Identical copy
 * in src/net/ax88179/ and src/net/rtl815x/ (see netchip.h).
 *
 * A deserialized NDIS 5 miniport with a WDM lower edge (netusb.c):
 * - every packet accepted by SendPackets completes once through
 *   NdisMSendComplete, from the transmit path or a flush; the queue has
 *   no limit (a full queue never fails a send), and a packet while the
 *   link is down or the adapter gone or halting completes with failure at
 *   once;
 * - received frames are copied into a preallocated packet pool and
 *   indicated up to NET_RX_BATCH at a time per bulk-IN transfer; a SUCCESS
 *   indication transfers ownership until ReturnPacket, a RESOURCES
 *   indication (pool low, one packet alone) is reclaimed on return using
 *   the status saved before the call;
 * - on 98 SE and ME (the x86 NDIS 5.0 build, at run time) no receive
 *   indication or send completion is made from a USB completion: both
 *   are queued and made from an NDIS timer (Upcalls, ethernet.md section
 *   4.4);
 * - on NT, task offload (ethernet.md section 4.9): the checksums and the
 *   large send a chip offers (NET_START.Offload) are answered on
 *   OID_TCP_TASK_OFFLOAD, and each send's per-packet request goes to the
 *   chip; 98 SE and ME read no per-packet information;
 * - OIDs that need the device (filter, multicast, power) and reset pend
 *   and complete from the lower edge's passive worker;
 * - the address: the Advanced tab's NetworkAddress if valid, else the
 *   chip's own, else a locally administered address generated once and
 *   kept in the driver key as FallbackAddress (section 4.8);
 * - the Advanced tab's other keywords (section 4.10): the chip's own
 *   (NET_CHIP.Settings), read once before the chip starts and handed to
 *   it in NET_START.Settings, and on NT the transmit checksum keywords,
 *   which narrow NET_START.Offload.
 */
#include <ndis.h>
#include "netusb.h"

#define TX_LIMIT 64          /* OID_GEN_TRANSMIT_BUFFER_SPACE only: the queue itself is not bounded */
#define TX_KICK 8            /* frames queued between kicks of the pump within one call */
#define RX_PACKETS 64
#define RX_LOW 8
#define RX_STRIDE 1536

/* The 98 SE / ME upcall queue (Upcalls) exists in the x86 NDIS 5.0 build
 * only; the NDIS 5.1 (x64) build has none of it. */
#if !defined(NDIS51_MINIPORT)
#define NET_DEFER 1
#define DEFERRING(a) ((a)->Defer)
#else
#define NET_DEFER 0
#define DEFERRING(a) FALSE
#endif

typedef struct ADAPTER {
    NDIS_HANDLE Handle;
    void *Usb;
    NDIS_SPIN_LOCK Lock;
    NET_START Start;
    UCHAR Mac[6], PermanentMac[6];
    ULONG Filter, Lookahead, LinkSpeed;
    BOOLEAN Connected, Gone, Halting, Ready;
    BOOLEAN LinkKnown;             /* the lower edge has called Link */
    UCHAR Multicast[NC_MULTICAST_MAX][6];
    ULONG MulticastCount;
    PNDIS_PACKET TxHead, TxTail;   /* linked through MiniportReserved */
    ULONG TxQueued;
    NDIS_HANDLE PacketPool, BufferPool;
    PUCHAR RxStorage;
    PNDIS_PACKET RxFree[RX_PACKETS];
    ULONG RxFreeCount;
    LONG RxOut;
    ULONG PowerState;
    ULONG TxOk, TxError, RxOk, RxError, RxNoBuffer;
    ULONG TxBytes, RxBytes;
    ULONG RxCopied;                /* trace: frames indicated with NDIS_STATUS_RESOURCES */
    ULONG TxPeak;                  /* trace: the longest the send queue has been */
    /* Task offload (section 4.9), NT only: PerPacket is set where the
     * packets carry per-packet information (not on 98 SE and ME, whose
     * TCP/IP never asks). TaskTx (NC_TX_*) and TaskRx (NC_RX_*_OK) are
     * what TCP/IP enabled, under Lock. */
    BOOLEAN PerPacket;
    ULONG TaskTx, TaskRx;
    long Settings[NC_SETTINGS_MAX];  /* the chip's keywords (section 4.10), in its table's order */
    ULONG OffloadAllowed;          /* NC_OFFLOAD_* the checksum keywords leave on */
    ULONG TxLarge, RxChecked;      /* trace: large sends; frames given a checksum result */
#if NET_DEFER
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

/* The core's lock is taken from the lower edge's USB completions, which
 * run under the kernel's dispatch level, and from NDIS's own calls. Windows
 * 98 SE's NDIS spin lock does not keep those completions out (the send
 * queue and the receive pool were corrupted under load), so the kernel spin
 * lock inside NDIS_SPIN_LOCK is taken directly: on NT that is what
 * NdisAcquireSpinLock does. */
#define CoreLock(a) KeAcquireSpinLock(&(a)->Lock.SpinLock, &(a)->Lock.OldIrql)
#define CoreUnlock(a) KeReleaseSpinLock(&(a)->Lock.SpinLock, (a)->Lock.OldIrql)

#define NEXT(p) (*(PNDIS_PACKET *)&(p)->MiniportReserved[0])
#define DONE_STATUS(p) (*(NDIS_STATUS *)&(p)->MiniportReserved[sizeof(PVOID)])
/* While a send is queued, the same second slot holds its length. */
#define QUEUED_SIZE(p) (*(ULONG *)&(p)->MiniportReserved[sizeof(PVOID)])
#define LARGE_SEND(p) NDIS_PER_PACKET_INFO_FROM_PACKET(p, TcpLargeSendPacketInfo)
#define CHECKSUM_INFO(p) NDIS_PER_PACKET_INFO_FROM_PACKET(p, TcpIpChecksumPacketInfo)
#define ETH_HEADER 14
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17
/* Headroom in a large send for the IP and TCP headers with options. */
#define LSO_HEADERS (ETH_HEADER + 60 + 60)

#if NET_DEFER
/* Windows 98 SE and ME. Under sustained TCP, receive indications and send
 * completions made straight from the USB completions stalled or faulted
 * the 98 TCP/IP (fatal exceptions 0E in MSTCP, streams stalling within
 * seconds), with the core's own state intact; the same calls made from an
 * NDIS timer did not (docs/testing/2026-11-07-98se-tcp.md). So on these
 * systems the completions only queue (Receive, TxDone) and arm the timer,
 * and Upcalls makes the calls. NT makes them at once, as before. Called
 * with the lock held. */
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
    PNDIS_PACKET rx[NET_RX_BATCH], done;
    ULONG n, i, taken;
    for (;;) {
        CoreLock(a);
        done = a->DoneHead;
        a->DoneHead = a->DoneTail = NULL;
        n = 0;
        while (n < NET_RX_BATCH && a->RxPendCount) {
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
        CoreUnlock(a);
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
    CoreLock(a);
    a->UpcallRunning = TRUE;
    a->UpcallRuns++;
    CoreUnlock(a);
    for (;;) {
        CoreLock(a);
        indicate = !a->Halting && !a->UpcallPaused;
        CoreUnlock(a);
        Deliver(a, indicate);
        CoreLock(a);
        more = a->RxPendCount || a->DoneHead;
        if (!more) {
            a->UpcallArmed = FALSE;
            a->UpcallRunning = FALSE;
            sleep = a->SleepPending;
            a->SleepPending = FALSE;
        }
        CoreUnlock(a);
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
    CoreLock(a);
    if (halt) a->UpcallStop = TRUE; else a->UpcallPaused = TRUE;
    CoreUnlock(a);
    {
        BOOLEAN c0 = FALSE, c1 = FALSE;   /* only the timer last set can be set */
        NdisMCancelTimer(&a->UpcallTimer[0], &c0);
        NdisMCancelTimer(&a->UpcallTimer[1], &c1);
        cancelled = (BOOLEAN)(c0 || c1);
    }
    CoreLock(a);
    if (cancelled) a->UpcallArmed = FALSE;
    busy = a->UpcallArmed || a->UpcallRunning;
    CoreUnlock(a);
    while (busy) {
        NdisMSleep(1000);
        if (++waited % 5000 == 0) NetTrace("upcall routine still due");
        CoreLock(a);
        busy = a->UpcallArmed || a->UpcallRunning;
        CoreUnlock(a);
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
    CoreLock(a);
    a->UpcallPaused = TRUE;
    CoreUnlock(a);
    {
        BOOLEAN c0 = FALSE, c1 = FALSE;   /* only the timer last set can be set */
        NdisMCancelTimer(&a->UpcallTimer[0], &c0);
        NdisMCancelTimer(&a->UpcallTimer[1], &c1);
        cancelled = (BOOLEAN)(c0 || c1);
    }
    CoreLock(a);
    if (cancelled) a->UpcallArmed = FALSE;
    busy = a->UpcallArmed || a->UpcallRunning;
    if (busy) a->SleepPending = TRUE;
    CoreUnlock(a);
    if (!busy) Deliver(a, FALSE);
    return busy;
}

/* Back at D0: deferred delivery again, for anything queued meanwhile. */
static void ResumeUpcalls(ADAPTER *a)
{
    CoreLock(a);
    a->UpcallPaused = FALSE;
    if (a->RxPendCount || a->DoneHead) ArmUpcalls(a);
    CoreUnlock(a);
}
#endif

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
    OID_PNP_CAPABILITIES, OID_PNP_QUERY_POWER, OID_PNP_SET_POWER,
    OID_TCP_TASK_OFFLOAD            /* last: listed only where task offload is answered (QueryTasks) */
};

/* ---------------- callbacks from the lower edge ---------------- */

/* Indicate what is gathered, in order. Every packet in it was given
 * NDIS_STATUS_SUCCESS: NDIS returns each through ReturnPacket. */
static void ReceiveFlush(ADAPTER *a, NET_RX_GATHER *g)
{
    if (!g->Count) return;
    NdisMIndicateReceivePacket(a->Handle, (PPNDIS_PACKET)g->Packets, g->Count);
    g->Count = 0;
}

static void Receive(void *context, NET_RX_GATHER *g, const nc_u8 *frame, nc_u32 length,
                    nc_u32 checksum)
{
    ADAPTER *a = context;
    PNDIS_PACKET packet = NULL;
    PNDIS_BUFFER buffer;
    PVOID storage;
    UINT size;
    ULONG filter, i;
    BOOLEAN accept = FALSE, low = FALSE;
    NDIS_TCP_IP_CHECKSUM_PACKET_INFO info;

    if (length < NC_ETH_MIN || length > NC_ETH_MAX) return;
    info.Value = 0;
    CoreLock(a);
    /* Only what the chip verified and TCP/IP enabled is reported; a
     * failure is never reported, so TCP/IP checks such a frame itself. */
    checksum &= a->TaskRx;
    if (checksum & NC_RX_IP_OK) info.Receive.NdisPacketIpChecksumSucceeded = 1;
    if (checksum & NC_RX_TCP_OK) info.Receive.NdisPacketTcpChecksumSucceeded = 1;
    if (checksum & NC_RX_UDP_OK) info.Receive.NdisPacketUdpChecksumSucceeded = 1;
    filter = a->Filter;
    if (a->Halting || !filter) {
        CoreUnlock(a);
        return;
    }
    /* Software filter: the chips' 64-bin multicast hash and always-on
     * broadcast let through more than NDIS asked for. */
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
        accept = (filter & NDIS_PACKET_TYPE_DIRECTED) && NdisEqualMemory(frame, a->Mac, 6);
    }
    if (accept) {
        if (a->RxFreeCount) {
            packet = a->RxFree[--a->RxFreeCount];
            low = a->RxFreeCount < RX_LOW;
            a->RxOut++;
            a->RxOk++;
            a->RxBytes += length;
            if (info.Value) a->RxChecked++;
#if defined(NET_TRACE_E9)
            if (!(a->RxOk % 8192))
                NetTrace("rx: %u ok, %u free, %d out, %u no buffer, %u copied, %u checked; tx %u/%u queued %u peak %u, %u large%s",
                         a->RxOk, a->RxFreeCount, a->RxOut, a->RxNoBuffer, a->RxCopied, a->RxChecked, a->TxOk,
                         a->TxError, a->TxQueued, a->TxPeak, a->TxLarge, DEFERRING(a) ? "; deferred" : "");
#endif
        } else {
            a->RxNoBuffer++;
        }
    }
    CoreUnlock(a);
    if (!packet) return;

    NdisQueryPacket(packet, NULL, NULL, &buffer, NULL);
    NdisQueryBuffer(buffer, &storage, &size);
    NdisMoveMemory(storage, frame, length);
    NdisAdjustBufferLength(buffer, length);
    NdisRecalculatePacketCounts(packet);
    NDIS_SET_PACKET_HEADER_SIZE(packet, 14);
    /* Every packet of the pool is set, as it is reused (NT only). */
    if (a->PerPacket) CHECKSUM_INFO(packet) = (PVOID)(ULONG_PTR)info.Value;
#if NET_DEFER
    if (a->Defer) {
        /* Indicated by Upcalls, always with NDIS_STATUS_SUCCESS: the
         * RESOURCES path below needs the indication inside this call. */
        NDIS_SET_PACKET_STATUS(packet, NDIS_STATUS_SUCCESS);
        CoreLock(a);
        a->RxPend[(a->RxPendHead + a->RxPendCount++) % RX_PACKETS] = packet;
        ArmUpcalls(a);
        CoreUnlock(a);
        return;
    }
#endif
    if (low) {
        /* Pool low: what is gathered goes first, in order, then this
         * packet alone with NDIS_STATUS_RESOURCES, which the protocols
         * copy during the call; it is reclaimed at once. The status set
         * before the call decides ownership, never the status after it
         * (NDIS 5 receive indication contract). */
        ReceiveFlush(a, g);
        a->RxCopied++;              /* trace only; an unlocked count */
        NDIS_SET_PACKET_STATUS(packet, NDIS_STATUS_RESOURCES);
        NdisMIndicateReceivePacket(a->Handle, &packet, 1);
        CoreLock(a);
        a->RxFree[a->RxFreeCount++] = packet;
        a->RxOut--;
        CoreUnlock(a);
        return;
    }
    NDIS_SET_PACKET_STATUS(packet, NDIS_STATUS_SUCCESS);
    g->Packets[g->Count++] = packet;
    if (g->Count == NET_RX_BATCH) ReceiveFlush(a, g);
}

static void ReceiveEnd(void *context, NET_RX_GATHER *g)
{
    ReceiveFlush(context, g);
}

static VOID ReturnPacket(NDIS_HANDLE context, PNDIS_PACKET packet)
{
    ADAPTER *a = context;
    CoreLock(a);
    a->RxFree[a->RxFreeCount++] = packet;
    a->RxOut--;
    CoreUnlock(a);
}

static nc_u32 TxTake(void *context, void **packets, nc_u32 max, nc_u32 room, nc_u32 header,
                     nc_u32 align)
{
    ADAPTER *a = context;
    nc_u32 n = 0, used = 0;
    CoreLock(a);
    while (n < max && a->TxHead) {
        PNDIS_PACKET p = a->TxHead;
        nc_u32 size = QUEUED_SIZE(p), at = (used + align - 1) & ~(align - 1);
        if (size < NC_ETH_PAD) size = NC_ETH_PAD;
        /* The first always goes (SendPackets admits no packet longer than
         * a transfer holds); the rest only while they fit. */
        if (n && (at + header + size > room || at + header + size < at)) break;
        used = at + header + size;
        a->TxHead = NEXT(p);
        if (!a->TxHead) a->TxTail = NULL;
        a->TxQueued--;
        packets[n++] = p;
    }
    CoreUnlock(a);
    return n;
}

/* The offloads a send asks for (2000 DDK network guide sections 9.5 and
 * 9.7), from its per-packet information and its headers as copied.
 * Returns FALSE when the packet asks for one this adapter cannot do. On a
 * large send the per-packet value becomes the TCP payload bytes, which is
 * what is sent once the transfer completes (TxDone resets it on failure). */
static BOOLEAN TxOffloads(ADAPTER *a, PNDIS_PACKET p, const nc_u8 *f, nc_u32 total, NET_TX_INFO *info)
{
    ULONG mss = (ULONG)(ULONG_PTR)LARGE_SEND(p);
    NDIS_TCP_IP_CHECKSUM_PACKET_INFO ci;
    nc_u32 ip, transport, tcp, enabled;
    ci.Value = (ULONG)(ULONG_PTR)CHECKSUM_INFO(p);
    if (!mss && !ci.Transmit.NdisPacketChecksumV4) return total <= NC_ETH_MAX;
    CoreLock(a);
    enabled = a->TaskTx;
    CoreUnlock(a);
    /* IPv4 behind an Ethernet II header (the 802.3 encapsulation TCP/IP
     * was told). */
    if (total < ETH_HEADER + 20 || f[12] != 0x08 || f[13] != 0x00 || (f[14] >> 4) != 4) return FALSE;
    ip = (f[14] & 0x0f) * 4;
    transport = ETH_HEADER + ip;
    if (ip < 20 || transport > total) return FALSE;
    if (mss) {
        if (!(enabled & NC_TX_LSO4) || f[23] != IP_PROTO_TCP || mss > total) return FALSE;
        if (transport + 20 > total) return FALSE;
        tcp = (f[transport + 12] >> 4) * 4;
        if (tcp < 20 || transport + tcp > total) return FALSE;
        info->Flags = NC_TX_LSO4;
        info->Mss = mss;
        info->Transport = transport;
        LARGE_SEND(p) = (PVOID)(ULONG_PTR)(total - transport - tcp);
        return TRUE;
    }
    if (total > NC_ETH_MAX) return FALSE;
    if (ci.Transmit.NdisPacketTcpChecksum) {
        if (!(enabled & NC_TX_TCP4) || f[23] != IP_PROTO_TCP) return FALSE;
        info->Flags |= NC_TX_TCP4;
    } else if (ci.Transmit.NdisPacketUdpChecksum) {
        if (!(enabled & NC_TX_UDP4) || f[23] != IP_PROTO_UDP) return FALSE;
        info->Flags |= NC_TX_UDP4;
    }
    if (ci.Transmit.NdisPacketIpChecksum) {
        if (!(enabled & NC_TX_IP4)) return FALSE;
        info->Flags |= NC_TX_IP4;
    }
    info->Transport = transport;
    return TRUE;
}

static nc_u32 TxCopy(void *context, void *packet, nc_u8 *bytes, nc_u32 capacity, NET_TX_INFO *info)
{
    ADAPTER *a = context;
    PNDIS_BUFFER b;
    UINT total, copied = 0;
    NdisQueryPacket((PNDIS_PACKET)packet, NULL, NULL, &b, &total);
    if (total < NC_ETH_MIN || total > capacity) return 0;
    while (b) {
        PVOID address;
        UINT size;
        NdisQueryBuffer(b, &address, &size);
        if (size > total - copied || (size && !address)) return 0;
        if (size) NdisMoveMemory(bytes + copied, address, size);
        copied += size;
        NdisGetNextBuffer(b, &b);
    }
    if (copied != total) return 0;
    if (!a->PerPacket) return total <= NC_ETH_MAX ? total : 0;
    if (!TxOffloads(a, (PNDIS_PACKET)packet, bytes, total, info)) return 0;
    if (info->Flags & NC_TX_LSO4) a->TxLarge++;      /* trace only; an unlocked count */
    return total;
}

static void TxDone(void *context, void **packets, nc_u32 count, int success)
{
    ADAPTER *a = context;
    nc_u32 i;
    for (i = 0; i < count; i++) {
        PNDIS_PACKET p = packets[i];
        UINT size;
        NdisQueryPacket(p, NULL, NULL, NULL, &size);
        /* A large send that failed sent no payload (section 9.7). */
        if (!success && a->PerPacket && LARGE_SEND(p)) LARGE_SEND(p) = NULL;
        CoreLock(a);
        if (success) { a->TxOk++; a->TxBytes += size; } else a->TxError++;
#if NET_DEFER
        /* Queued for Upcalls, except asleep: no USB completion runs then
         * (the worker drained them), so a send SendPackets or a flush
         * fails completes at once, below. */
        if (a->Defer && !a->UpcallPaused) {
            NEXT(p) = NULL;
            DONE_STATUS(p) = success ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE;
            if (a->DoneTail) NEXT(a->DoneTail) = p; else a->DoneHead = p;
            a->DoneTail = p;
            ArmUpcalls(a);
            CoreUnlock(a);
            continue;
        }
#endif
        CoreUnlock(a);
        NdisMSendComplete(a->Handle, p, success ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE);
    }
}

/* Complete every queued send with failure (gone, halting, link down). */
static void Flush(ADAPTER *a)
{
    void *packets[NET_TX_BATCH];
    nc_u32 n;
    while ((n = TxTake(a, packets, NET_TX_BATCH, 0xffffffffUL, 0, 1)) != 0) TxDone(a, packets, n, 0);
}

static void Link(void *context, int connected, nc_u32 speed)
{
    ADAPTER *a = context;
    BOOLEAN changed;
    CoreLock(a);
    if (connected < 0) a->Gone = TRUE;
    if (speed) a->LinkSpeed = speed;
    changed = a->Connected != (connected > 0);
    a->LinkKnown = TRUE;
    a->Connected = connected > 0;
    CoreUnlock(a);
    if (connected <= 0) Flush(a);
    /* No indication before MiniportInitialize has returned. */
    if (changed && a->Ready) {
        NdisMIndicateStatus(a->Handle, connected > 0 ? NDIS_STATUS_MEDIA_CONNECT :
                            NDIS_STATUS_MEDIA_DISCONNECT, NULL, 0);
        NdisMIndicateStatusComplete(a->Handle);
    }
}

/* Once per adapter, from a USB completion: written to the system's error
 * log, as a release build has no trace. */
static void Anomaly(void *context, nc_u32 code, nc_u32 value)
{
    ADAPTER *a = context;
    NdisWriteErrorLogEntry(a->Handle, NDIS_ERROR_CODE_HARDWARE_FAILURE, 2, (ULONG)code, (ULONG)value);
}

static void TaskDone(void *context, int request, long status)
{
    ADAPTER *a = context;
    UNREFERENCED_PARAMETER(status);
    if (request == NET_REQ_RESET) {
        NdisMResetComplete(a->Handle, NDIS_STATUS_SUCCESS, FALSE);
        return;
    }
#if NET_DEFER
    /* 98 SE and ME: before a sleep request completes, nothing may be left
     * for the timer to indicate (D0 resumed in Set, before the worker). */
    if (a->Defer && request == NET_REQ_POWER && a->PowerState != NdisDeviceStateD0) StopUpcalls(a, FALSE);
#endif
    /* A filter the device refused still stands in the software filter,
     * which keeps delivery correct; a power change always completes, the
     * adapter recovering or reporting itself gone. */
    NdisMSetInformationComplete(a->Handle, NDIS_STATUS_SUCCESS);
}

/* ---------------- task offload (section 4.9) ---------------- */

/* 2000 DDK network guide chapter 9: TCP/IP queries OID_TCP_TASK_OFFLOAD
 * with an NDIS_TASK_OFFLOAD_HEADER, then sets the tasks it enables (a set
 * with no task disables them all). IPv4 over Ethernet II only. */
#define TASK_OFFSET ((ULONG)FIELD_OFFSET(NDIS_TASK_OFFLOAD, TaskBuffer))
#define TASK_ROUND(n) (((n) + 3) & ~3UL)

static BOOLEAN TaskHeaderOk(const NDIS_TASK_OFFLOAD_HEADER *h)
{
    return h->Version == NDIS_TASK_OFFLOAD_VERSION && h->Size == sizeof(*h) &&
           h->EncapsulationFormat.Encapsulation == IEEE_802_3_Encapsulation &&
           h->EncapsulationFormat.EncapsulationHeaderSize == ETH_HEADER;
}

static NDIS_STATUS QueryTasks(ADAPTER *a, PVOID buffer, ULONG length, PULONG written, PULONG needed)
{
    NDIS_TASK_OFFLOAD_HEADER h;
    ULONG offload = a->Start.Offload, at, size, csum = 0;
    PUCHAR out = buffer;
    if (!a->PerPacket || !offload) return NDIS_STATUS_NOT_SUPPORTED;
    if (length < sizeof(h)) { *needed = sizeof(h); return NDIS_STATUS_INVALID_LENGTH; }
    NdisMoveMemory(&h, buffer, sizeof(h));
    if (!TaskHeaderOk(&h)) return NDIS_STATUS_NOT_SUPPORTED;
    if (offload & (NC_OFFLOAD_TX_IP4 | NC_OFFLOAD_TX_TCP4 | NC_OFFLOAD_TX_UDP4 | NC_OFFLOAD_RX4))
        csum = TASK_ROUND(TASK_OFFSET + sizeof(NDIS_TASK_TCP_IP_CHECKSUM));
    size = sizeof(h) + csum;
    if (offload & NC_OFFLOAD_LSO4) size += TASK_ROUND(TASK_OFFSET + sizeof(NDIS_TASK_TCP_LARGE_SEND));
    if (size == sizeof(h)) return NDIS_STATUS_NOT_SUPPORTED;
    if (length < size) { *needed = size; return NDIS_STATUS_INVALID_LENGTH; }
    NdisZeroMemory(out, size);
    h.OffsetFirstTask = sizeof(h);
    NdisMoveMemory(out, &h, sizeof(h));
    at = sizeof(h);
    if (csum) {
        PNDIS_TASK_OFFLOAD t = (PNDIS_TASK_OFFLOAD)(out + at);
        PNDIS_TASK_TCP_IP_CHECKSUM c = (PNDIS_TASK_TCP_IP_CHECKSUM)t->TaskBuffer;
        t->Version = NDIS_TASK_OFFLOAD_VERSION;
        t->Size = sizeof(NDIS_TASK_OFFLOAD);
        t->Task = TcpIpChecksumNdisTask;
        t->TaskBufferLength = sizeof(NDIS_TASK_TCP_IP_CHECKSUM);
        t->OffsetNextTask = size > at + csum ? csum : 0;
        c->V4Transmit.IpOptionsSupported = 1;
        c->V4Transmit.TcpOptionsSupported = 1;
        c->V4Transmit.TcpChecksum = (offload & NC_OFFLOAD_TX_TCP4) != 0;
        c->V4Transmit.UdpChecksum = (offload & NC_OFFLOAD_TX_UDP4) != 0;
        c->V4Transmit.IpChecksum = (offload & NC_OFFLOAD_TX_IP4) != 0;
        if (offload & NC_OFFLOAD_RX4) {
            c->V4Receive.IpOptionsSupported = 1;
            c->V4Receive.TcpOptionsSupported = 1;
            c->V4Receive.TcpChecksum = 1;
            c->V4Receive.UdpChecksum = 1;
            c->V4Receive.IpChecksum = 1;
        }
        at += csum;
    }
    if (offload & NC_OFFLOAD_LSO4) {
        PNDIS_TASK_OFFLOAD t = (PNDIS_TASK_OFFLOAD)(out + at);
        PNDIS_TASK_TCP_LARGE_SEND l = (PNDIS_TASK_TCP_LARGE_SEND)t->TaskBuffer;
        t->Version = NDIS_TASK_OFFLOAD_VERSION;
        t->Size = sizeof(NDIS_TASK_OFFLOAD);
        t->Task = TcpLargeSendNdisTask;
        t->TaskBufferLength = sizeof(NDIS_TASK_TCP_LARGE_SEND);
        t->OffsetNextTask = 0;
        l->Version = 0;
        l->MaxOffLoadSize = a->Start.LsoMax - LSO_HEADERS;   /* user data, any header options */
        l->MinSegmentCount = 2;
        l->TcpOptions = TRUE;
        l->IpOptions = TRUE;
    }
    *written = size;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS SetTasks(ADAPTER *a, PVOID buffer, ULONG length, PULONG read, PULONG needed)
{
    NDIS_TASK_OFFLOAD_HEADER h;
    ULONG offload = a->Start.Offload, at, tx = 0, rx = 0, guard = 0;
    PUCHAR in = buffer;
    if (!a->PerPacket || !offload) return NDIS_STATUS_NOT_SUPPORTED;
    if (length < sizeof(h)) { *needed = sizeof(h); return NDIS_STATUS_INVALID_LENGTH; }
    NdisMoveMemory(&h, buffer, sizeof(h));
    if (!TaskHeaderOk(&h)) return NDIS_STATUS_NOT_SUPPORTED;
    for (at = h.OffsetFirstTask; at; ) {
        NDIS_TASK_OFFLOAD t;
        if (++guard > 8 || at < sizeof(h) || at > length || length - at < TASK_OFFSET)
            return NDIS_STATUS_INVALID_DATA;
        NdisMoveMemory(&t, in + at, TASK_OFFSET);
        if (t.TaskBufferLength > length - at - TASK_OFFSET) return NDIS_STATUS_INVALID_DATA;
        if (t.Task == TcpIpChecksumNdisTask) {
            NDIS_TASK_TCP_IP_CHECKSUM c;
            if (t.TaskBufferLength < sizeof(c)) return NDIS_STATUS_INVALID_DATA;
            NdisMoveMemory(&c, in + at + TASK_OFFSET, sizeof(c));
            if (c.V4Transmit.TcpChecksum) tx |= NC_TX_TCP4;
            if (c.V4Transmit.UdpChecksum) tx |= NC_TX_UDP4;
            if (c.V4Transmit.IpChecksum) tx |= NC_TX_IP4;
            if (c.V4Receive.TcpChecksum) rx |= NC_RX_TCP_OK;
            if (c.V4Receive.UdpChecksum) rx |= NC_RX_UDP_OK;
            if (c.V4Receive.IpChecksum) rx |= NC_RX_IP_OK;
            if (c.V6Transmit.TcpChecksum || c.V6Transmit.UdpChecksum) return NDIS_STATUS_NOT_SUPPORTED;
        } else if (t.Task == TcpLargeSendNdisTask) {
            if (t.TaskBufferLength < sizeof(NDIS_TASK_TCP_LARGE_SEND)) return NDIS_STATUS_INVALID_DATA;
            tx |= NC_TX_LSO4;
        } else {
            return NDIS_STATUS_NOT_SUPPORTED;
        }
        if (!t.OffsetNextTask) break;
        if (t.OffsetNextTask > length - at) return NDIS_STATUS_INVALID_DATA;
        at += t.OffsetNextTask;
    }
    /* Only what was offered. */
    if (((tx & NC_TX_TCP4) && !(offload & NC_OFFLOAD_TX_TCP4)) ||
        ((tx & NC_TX_UDP4) && !(offload & NC_OFFLOAD_TX_UDP4)) ||
        ((tx & NC_TX_IP4) && !(offload & NC_OFFLOAD_TX_IP4)) ||
        ((tx & NC_TX_LSO4) && !(offload & NC_OFFLOAD_LSO4)) ||
        (rx && !(offload & NC_OFFLOAD_RX4)))
        return NDIS_STATUS_NOT_SUPPORTED;
    CoreLock(a);
    a->TaskTx = tx;
    a->TaskRx = rx;
    CoreUnlock(a);
    NetTrace("task offload: tx %x rx %x", tx, rx);
    *read = length;
    return NDIS_STATUS_SUCCESS;
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
    BOOLEAN counter = FALSE;
    *written = 0; *needed = 0;
    switch (oid) {
    case OID_GEN_SUPPORTED_LIST:
        data = supported;
        size = sizeof(supported);
        if (!a->PerPacket || !a->Start.Offload) size -= sizeof(NDIS_OID);
        break;
    case OID_GEN_HARDWARE_STATUS: value = NdisHardwareStatusReady; break;
    case OID_GEN_MEDIA_SUPPORTED: case OID_GEN_MEDIA_IN_USE: value = NdisMedium802_3; break;
    case OID_GEN_MAXIMUM_LOOKAHEAD: case OID_GEN_MAXIMUM_FRAME_SIZE: value = 1500; break;
    case OID_GEN_MAXIMUM_TOTAL_SIZE: case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE: value = NC_ETH_MAX; break;
    case OID_GEN_LINK_SPEED: value = a->LinkSpeed; break;
    case OID_GEN_TRANSMIT_BUFFER_SPACE: value = TX_LIMIT * NC_ETH_MAX; break;
    case OID_GEN_RECEIVE_BUFFER_SPACE: value = RX_PACKETS * NC_ETH_MAX; break;
    case OID_GEN_VENDOR_ID:
        value = a->PermanentMac[0] | (a->PermanentMac[1] << 8) | (a->PermanentMac[2] << 16);
        break;
    case OID_GEN_VENDOR_DESCRIPTION:
        data = (PVOID)NetPackageChip.Description;
        size = 0;
        while (NetPackageChip.Description[size]) size++;
        size++;
        break;
    case OID_GEN_VENDOR_DRIVER_VERSION: value = NetPackageChip.DriverVersion; break;
    case OID_GEN_CURRENT_PACKET_FILTER: value = a->Filter; break;
    case OID_GEN_CURRENT_LOOKAHEAD: value = a->Lookahead; break;
    case OID_GEN_DRIVER_VERSION: data = &version; size = sizeof(version); break;
    case OID_GEN_MAC_OPTIONS:
        value = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                NDIS_MAC_OPTION_NO_LOOPBACK;
        break;
    case OID_GEN_MEDIA_CONNECT_STATUS:
        value = a->Connected ? NdisMediaStateConnected : NdisMediaStateDisconnected; break;
    case OID_GEN_MAXIMUM_SEND_PACKETS: value = NET_TX_BATCH; break;
    case OID_GEN_XMIT_OK: wide = a->TxOk; counter = TRUE; break;
    case OID_GEN_RCV_OK: wide = a->RxOk; counter = TRUE; break;
    case OID_GEN_XMIT_ERROR: wide = a->TxError; counter = TRUE; break;
    case OID_GEN_RCV_ERROR: wide = a->RxError; counter = TRUE; break;
    case OID_GEN_RCV_NO_BUFFER: wide = a->RxNoBuffer; counter = TRUE; break;
    case OID_GEN_DIRECTED_BYTES_XMIT: wide = a->TxBytes; counter = TRUE; break;
    case OID_GEN_DIRECTED_BYTES_RCV: wide = a->RxBytes; counter = TRUE; break;
    case OID_802_3_PERMANENT_ADDRESS: data = a->PermanentMac; size = 6; break;
    case OID_802_3_CURRENT_ADDRESS: data = a->Mac; size = 6; break;
    case OID_802_3_MULTICAST_LIST: data = a->Multicast; size = a->MulticastCount * 6; break;
    case OID_802_3_MAXIMUM_LIST_SIZE: value = NC_MULTICAST_MAX; break;
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
    case OID_TCP_TASK_OFFLOAD: return QueryTasks(a, buffer, length, written, needed);
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
    switch (oid) {
    case OID_802_3_MULTICAST_LIST:
        if (length % 6) { *needed = (length / 6 + 1) * 6; return NDIS_STATUS_INVALID_LENGTH; }
        if (length > sizeof(a->Multicast)) return NDIS_STATUS_MULTICAST_FULL;
        CoreLock(a);
        if (length) NdisMoveMemory(a->Multicast, buffer, length);
        a->MulticastCount = length / 6;
        CoreUnlock(a);
        *read = length;
        if (a->Gone || !NetUsbRequest(a->Usb, NET_REQ_MULTICAST, 0, buffer, length))
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
#if NET_DEFER
            /* 98 SE and ME: deferred delivery is back before the worker
             * reopens the device, so no USB completion of the restart
             * reaches NDIS directly. */
            if (a->Defer && value == NdisDeviceStateD0) ResumeUpcalls(a);
#endif
            if (a->Gone || !NetUsbRequest(a->Usb, NET_REQ_POWER, value == NdisDeviceStateD0 ? 1 : 0, NULL, 0)) {
#if NET_DEFER
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
        CoreLock(a);
        a->Filter = value;
        CoreUnlock(a);
        if (a->Gone || !NetUsbRequest(a->Usb, NET_REQ_FILTER, value, NULL, 0))
            return NDIS_STATUS_SUCCESS;
        return NDIS_STATUS_PENDING;
    case OID_TCP_TASK_OFFLOAD:
        return SetTasks(a, buffer, length, read, needed);
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
        UINT size, limit;
        BOOLEAN queued = FALSE;
        /* Pending until NdisMSendComplete (the deserialized rules allow
         * the Status member to be set). Windows 98 SE's NDIS reads it: a
         * packet left at another value is handed back to the protocol
         * while it is still queued here, and reused. */
        NDIS_SET_PACKET_STATUS(p, NDIS_STATUS_PENDING);
        NdisQueryPacket(p, NULL, NULL, NULL, &size);
        /* A large send (section 4.9) may be as long as one transmit
         * buffer holds; anything else one frame. */
        limit = NC_ETH_MAX;
        if (a->PerPacket && a->Start.LsoMax && LARGE_SEND(p)) limit = a->Start.LsoMax;
        CoreLock(a);
        /* No limit on the queue: a deserialized miniport keeps every valid
         * send until it can go (NDIS 5 rules); the packets themselves are
         * the queue (MiniportReserved), so nothing is allocated. Only a
         * gone, halting or disconnected adapter or a bad length fails one. */
        if (!a->Gone && !a->Halting && a->Connected && size >= NC_ETH_MIN && size <= limit) {
            NEXT(p) = NULL;
            QUEUED_SIZE(p) = size;
            if (a->TxTail) NEXT(a->TxTail) = p; else a->TxHead = p;
            a->TxTail = p;
            a->TxQueued++;
            if (a->TxQueued > a->TxPeak) a->TxPeak = a->TxQueued;
            queued = TRUE;
        }
        CoreUnlock(a);
        if (!queued) {
            void *one = p;
            TxDone(a, &one, 1, 0);
        } else if (!((i + 1) % TX_KICK)) {
            NetUsbKick(a->Usb);   /* a long array starts going out at once */
        }
    }
    NetUsbKick(a->Usb);
}

static NDIS_STATUS Reset(PBOOLEAN addressing, NDIS_HANDLE context)
{
    ADAPTER *a = context;
    *addressing = FALSE;
    if (a->Gone || !NetUsbRequest(a->Usb, NET_REQ_RESET, 0, NULL, 0)) return NDIS_STATUS_SUCCESS;
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
    NetTrace("halt: begin, %d receive packets out", a->RxOut);
    CoreLock(a);
    a->Halting = TRUE;
    CoreUnlock(a);
    NetUsbStop(a->Usb);          /* no USB activity or callback after this */
    a->Usb = NULL;
    Flush(a);
#if NET_DEFER
    if (a->Defer) {
        StopUpcalls(a, TRUE);
        NetTrace("halt: %u upcall runs", a->UpcallRuns);
    }
#endif
    /* Every indicated packet comes back before its pool is freed. */
    while (a->RxOut > 0) {
        NdisMSleep(10000);
        if (++waited % 500 == 0) NetTrace("halt: %d receive packets still out", a->RxOut);
    }
    NetTrace("halted: tx %u/%u rx %u nobuf %u", a->TxOk, a->TxError, a->RxOk, a->RxNoBuffer);
    FreeAdapter(a);
}

#if defined(NDIS51_MINIPORT)
/* NDIS 5.1 (the x64 build, ethernet.md section 4.2), as cdcnet's: a
 * surprise removal fails what is queued, lets nothing new in, and closes
 * the lower edge's gate. */
static VOID PnPEvent(NDIS_HANDLE context, NDIS_DEVICE_PNP_EVENT event, PVOID buffer, ULONG length)
{
    ADAPTER *a = context;
    UNREFERENCED_PARAMETER(buffer);
    UNREFERENCED_PARAMETER(length);
    if (event == NdisDevicePnPEventSurpriseRemoved) {
        CoreLock(a);
        a->Gone = TRUE;
        CoreUnlock(a);
        NetUsbSurpriseRemoved(a->Usb);
        Flush(a);
    }
}

static VOID Shutdown(NDIS_HANDLE context)
{
    UNREFERENCED_PARAMETER(context);  /* may run at high IRQL: touch nothing */
}
#endif

/* ---------------- the address (section 4.8) ---------------- */

static BOOLEAN ValidMac(const UCHAR *m)
{
    return !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0;
}

static int Hex(WCHAR c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

static void ChooseAddress(ADAPTER *a, NDIS_HANDLE config)
{
    NDIS_HANDLE handle = NULL;
    NDIS_STATUS status;
    PVOID address = NULL;
    UINT length = 0, i;
    NDIS_STRING name = NDIS_STRING_CONST("FallbackAddress");
    PNDIS_CONFIGURATION_PARAMETER p;
    UCHAR fallback[6];
    BOOLEAN haveFallback = FALSE;

    NdisMoveMemory(a->PermanentMac, a->Start.DeviceMac, 6);
    NdisMoveMemory(a->Mac, a->Start.DeviceMac, 6);
    NdisOpenConfiguration(&status, &handle, config);
    if (status != NDIS_STATUS_SUCCESS) handle = NULL;

    /* 1. NetworkAddress from the Advanced tab. */
    if (handle) {
        NdisReadNetworkAddress(&status, &address, &length, handle);
        if (status == NDIS_STATUS_SUCCESS && length == 6 && ValidMac(address)) {
            NdisMoveMemory(a->Mac, address, 6);
            NetTrace("address: NetworkAddress");
            if (!ValidMac(a->PermanentMac)) NdisMoveMemory(a->PermanentMac, a->Mac, 6);
            NdisCloseConfiguration(handle);
            return;
        }
    }
    /* 2. The chip's own. */
    if (ValidMac(a->Start.DeviceMac)) {
        if (handle) NdisCloseConfiguration(handle);
        return;
    }
    /* 3. A locally administered address, generated once and kept. */
    if (handle) {
        NdisReadConfiguration(&status, &p, handle, &name, NdisParameterString);
        if (status == NDIS_STATUS_SUCCESS && p->ParameterType == NdisParameterString &&
            p->ParameterData.StringData.Length == 24) {
            haveFallback = TRUE;
            for (i = 0; i < 6 && haveFallback; i++) {
                int hi = Hex(p->ParameterData.StringData.Buffer[2 * i]);
                int lo = Hex(p->ParameterData.StringData.Buffer[2 * i + 1]);
                if (hi < 0 || lo < 0) haveFallback = FALSE;
                else fallback[i] = (UCHAR)(hi * 16 + lo);
            }
            if (haveFallback && (!ValidMac(fallback) || !(fallback[0] & 2))) haveFallback = FALSE;
        }
    }
    if (!haveFallback) {
        LARGE_INTEGER t;
        ULONG seed;
        NdisGetCurrentSystemTime(&t);
        seed = t.LowPart ^ t.HighPart ^ (ULONG)(ULONG_PTR)a;
        for (i = 0; i < 6; i++) {
            seed = seed * 1103515245UL + 12345UL;
            fallback[i] = (UCHAR)(seed >> 16);
        }
        fallback[0] = (UCHAR)((fallback[0] & 0xfc) | 0x02);
        if (handle) {
            WCHAR text[12];
            NDIS_CONFIGURATION_PARAMETER value;
            for (i = 0; i < 6; i++) {
                text[2 * i] = L"0123456789ABCDEF"[fallback[i] >> 4];
                text[2 * i + 1] = L"0123456789ABCDEF"[fallback[i] & 15];
            }
            NdisZeroMemory(&value, sizeof(value));
            value.ParameterType = NdisParameterString;
            value.ParameterData.StringData.Buffer = text;
            value.ParameterData.StringData.Length = sizeof(text);
            value.ParameterData.StringData.MaximumLength = sizeof(text);
            NdisWriteConfiguration(&status, handle, &name, &value);
            NetTrace("address: generated fallback, saved %x", status);
        }
    }
    NdisMoveMemory(a->Mac, fallback, 6);
    NdisMoveMemory(a->PermanentMac, fallback, 6);
    if (handle) NdisCloseConfiguration(handle);
}

/* ---------------- the Advanced tab's keywords (section 4.10) ---------------- */

/* One keyword as a number: the tab writes decimal text on every system;
 * a DWORD (integer type) is taken as it is. FALSE when it is missing or
 * not a plain decimal number. */
static BOOLEAN ReadKeyword(NDIS_HANDLE handle, const char *keyword, long *value)
{
    WCHAR text[40];
    NDIS_STRING name;
    NDIS_STATUS status;
    PNDIS_CONFIGURATION_PARAMETER p;
    UINT i, n;
    for (n = 0; keyword[n] && n < 39; n++) text[n] = (WCHAR)(UCHAR)keyword[n];
    text[n] = 0;
    name.Buffer = text;
    name.Length = (USHORT)(n * sizeof(WCHAR));
    name.MaximumLength = (USHORT)(sizeof(text));
    NdisReadConfiguration(&status, &p, handle, &name, NdisParameterString);
    if (status != NDIS_STATUS_SUCCESS) return FALSE;
    if (p->ParameterType == NdisParameterInteger || p->ParameterType == NdisParameterHexInteger) {
        if (p->ParameterData.IntegerData > 0x7fffffffUL) return FALSE;
        *value = (long)p->ParameterData.IntegerData;
        return TRUE;
    }
    if (p->ParameterType != NdisParameterString) return FALSE;
    n = p->ParameterData.StringData.Length / sizeof(WCHAR);
    while (n && p->ParameterData.StringData.Buffer[n - 1] == 0) n--;   /* a counted terminator */
    if (!n || n > 9) return FALSE;
    *value = 0;
    for (i = 0; i < n; i++) {
        WCHAR c = p->ParameterData.StringData.Buffer[i];
        if (c < L'0' || c > L'9') return FALSE;
        *value = *value * 10 + (c - L'0');
    }
    return TRUE;
}

/* The transmit checksum keywords (NT): 1 (Tx) or 3 (Rx and Tx) leave the
 * task on, 0 or 2 take it off; missing or anything else: on. */
static ULONG ChecksumKeyword(NDIS_HANDLE handle, const char *keyword, ULONG bits)
{
    long v;
    if (!ReadKeyword(handle, keyword, &v) || v < 0 || v > 3) return bits;
    NetTrace("setting %s = %d", keyword, v);
    return (v & 1) ? bits : 0;
}

static void ReadSettings(ADAPTER *a, NDIS_HANDLE config)
{
    NDIS_HANDLE handle = NULL;
    NDIS_STATUS status;
    UINT i, k;
    const NET_SETTING *s = NetPackageChip.Settings;
    UINT count = s ? NetPackageChip.SettingCount : 0;
    if (count > NC_SETTINGS_MAX) count = NC_SETTINGS_MAX;
    a->OffloadAllowed = (ULONG)-1;
    for (i = 0; i < count; i++) a->Settings[i] = s[i].Default;
    NdisOpenConfiguration(&status, &handle, config);
    if (status != NDIS_STATUS_SUCCESS) return;
    for (i = 0; i < count; i++) {
        long v;
        if (!ReadKeyword(handle, s[i].Name, &v)) {
            NetTrace("setting %s: none or malformed, default %d", s[i].Name, s[i].Default);
            continue;
        }
        for (k = 0; k < s[i].Count && k < NC_SETTING_VALUES; k++) if (s[i].Values[k] == v) break;
        if (k == s[i].Count || k == NC_SETTING_VALUES) {
            NetTrace("setting %s = %d: not offered, default %d", s[i].Name, v, s[i].Default);
            continue;
        }
        a->Settings[i] = v;
        NetTrace("setting %s = %d", s[i].Name, v);
    }
    if (a->PerPacket) {         /* NT: 98 SE and ME have no offload */
        a->OffloadAllowed &= ~(ULONG)(NC_OFFLOAD_TX_IP4 | NC_OFFLOAD_TX_TCP4 | NC_OFFLOAD_TX_UDP4);
        a->OffloadAllowed |= ChecksumKeyword(handle, "*IPChecksumOffloadIPv4", NC_OFFLOAD_TX_IP4);
        a->OffloadAllowed |= ChecksumKeyword(handle, "*TCPChecksumOffloadIPv4", NC_OFFLOAD_TX_TCP4);
        a->OffloadAllowed |= ChecksumKeyword(handle, "*UDPChecksumOffloadIPv4", NC_OFFLOAD_TX_UDP4);
    }
    NdisCloseConfiguration(handle);
}

static NDIS_STATUS Initialize(PNDIS_STATUS error, PUINT chosen, PNDIS_MEDIUM media, UINT count,
                              NDIS_HANDLE handle, NDIS_HANDLE config)
{
    ADAPTER *a;
    UINT i;
    PDEVICE_OBJECT pdo = NULL, fdo = NULL, lower = NULL;
    NDIS_STATUS status;
    NDIS_PHYSICAL_ADDRESS highest = NDIS_PHYSICAL_ADDRESS_CONST(-1, -1);
    NET_CALLBACKS cb;
    int connected = 0;
    nc_u32 speed = 0;
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
    KeInitializeSpinLock(&a->Lock.SpinLock);   /* 98 SE: NDIS need not initialise it */

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
        NdisAllocateBuffer(&status, &b, a->BufferPool, a->RxStorage + i * RX_STRIDE, NC_ETH_MAX);
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
#if NET_DEFER
    /* 98 SE and ME: the WDM 1.10 query fails there (as in netusb.c). The
     * timer is set only once NDIS can send or set a filter, which is after
     * Initialize returns, so no failure path below has one to cancel. */
    a->Defer = !IoIsWdmVersionAvailable(1, 0x10);
    if (a->Defer) {
        NdisMInitializeTimer(&a->UpcallTimer[0], handle, Upcalls, a);
        NdisMInitializeTimer(&a->UpcallTimer[1], handle, Upcalls, a);
    }
#endif
    /* NT's packets carry per-packet information (section 4.9); 98 SE's
     * and ME's are not read for it. */
    a->PerPacket = !DEFERRING(a);
    NdisMGetDeviceProperty(handle, &pdo, &fdo, &lower, NULL, NULL);
    if (!pdo || !fdo || !lower) { FreeAdapter(a); return NDIS_STATUS_ADAPTER_NOT_FOUND; }

    cb.Context = a; cb.Receive = Receive; cb.ReceiveEnd = ReceiveEnd; cb.TxTake = TxTake; cb.TxCopy = TxCopy;
    cb.TxDone = TxDone; cb.Link = Link; cb.TaskDone = TaskDone; cb.Anomaly = Anomaly;
    ReadSettings(a, config);
    a->Start.Settings = NetPackageChip.SettingCount ? a->Settings : NULL;
    if (NetUsbStart(lower, &cb, &a->Start, &a->Usb) < 0) {
        NetTrace("initialize failed: stage %u status %x", a->Start.Failure, a->Start.Status);
        NdisWriteErrorLogEntry(handle, NDIS_ERROR_CODE_ADAPTER_NOT_FOUND, 2,
                               a->Start.Failure, (ULONG)a->Start.Status);
        FreeAdapter(a);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }
    if (a->Start.Offload & ~a->OffloadAllowed)
        NetTrace("offload %x, keywords leave %x", a->Start.Offload, a->Start.Offload & a->OffloadAllowed);
    a->Start.Offload &= a->OffloadAllowed;
    ChooseAddress(a, config);
    if (NetUsbOpen(a->Usb, a->Mac, &a->Start, &connected, &speed) < 0) {
        NetTrace("open failed: stage %u status %x", a->Start.Failure, a->Start.Status);
        NdisWriteErrorLogEntry(handle, NDIS_ERROR_CODE_ADAPTER_NOT_FOUND, 2,
                               a->Start.Failure, (ULONG)a->Start.Status);
        a->Usb = NULL;           /* NetUsbOpen freed it */
        FreeAdapter(a);
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }
    CoreLock(a);
    /* Link may have been called already (the worker runs from
     * NetUsbOpen on): the latest state stands over the snapshot. */
    if (!a->LinkKnown) a->Connected = connected != 0;
    if (speed) a->LinkSpeed = speed;
    a->Ready = TRUE;
    CoreUnlock(a);
    NetTrace("initialized: %x:%x chip %x, %s speed USB, link %u, %x:%x:%x:%x:%x:%x",
             a->Start.Vendor, a->Start.Product, a->Start.Version,
             a->Start.Speed == NC_SPEED_SUPER ? "super" : a->Start.Speed == NC_SPEED_HIGH ? "high" : "full",
             a->Connected, a->Mac[0], a->Mac[1], a->Mac[2], a->Mac[3], a->Mac[4], a->Mac[5]);
    return NDIS_STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    NDIS_HANDLE wrapper = NULL;
    NDIS_MINIPORT_CHARACTERISTICS chars;
    NDIS_STATUS status;
    NdisMInitializeWrapper(&wrapper, driver, path, NULL);
    if (!wrapper) return STATUS_INSUFFICIENT_RESOURCES;
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
    NetTrace("DriverEntry: register %x, at %x", status, (ULONG)(ULONG_PTR)DriverEntry);
    if (status != NDIS_STATUS_SUCCESS) NdisTerminateWrapper(wrapper, NULL);
    return status;
}

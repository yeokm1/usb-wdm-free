/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * The fake operating system under netusb.c and netcore.c in the core
 * tests (ethernet.md section 10.3): kernel, USB and NDIS services with
 * accounting and rule checks, and hooks the test fills in. Host-test code
 * only. Identical copy in both packages.
 *
 * Rules checked on every call (each breach counts in Fake.Violations and
 * is printed):
 * - no USB submission, NDIS indication or completion, wait or delay while
 *   the calling thread holds a spin lock (ethernet.md 4.7.1);
 * - no spin lock acquired twice by one thread, none released by another;
 * - a completion routine returns STATUS_MORE_PROCESSING_REQUIRED (the
 *   driver owns and frees its IRPs); an IRP is freed exactly once;
 * - a packet, buffer or pool block is freed exactly once;
 * - with Wdm110 0 (98 SE, ME), no receive indication or send completion
 *   from inside a USB completion routine: those come from the NDIS timer.
 */
#ifndef FAKEOS_H
#define FAKEOS_H

#include "ndis.h"
#include "usbdi.h"

typedef struct FAKE {
    /* accounting: allocations not yet freed */
    long Irps, Pool, NdisMemory, Packets, Buffers, PacketPools, BufferPools, Locks, Threads;
    long Violations;
    long Depth, MaxDepth;             /* IoCallDriver nesting, any thread */
    long Submissions;
    int Wdm110;                       /* IoIsWdmVersionAvailable(1, 0x10) */
    /* registry */
    unsigned char NetworkAddress[6];
    int HaveNetworkAddress;
    WCHAR Fallback[16];
    int HaveFallback, FallbackWrites;
    /* NDIS state */
    NDIS_MINIPORT_CHARACTERISTICS Chars;
    NDIS_HANDLE Context;              /* from NdisMSetAttributesEx */
    ULONG Attributes;
    PDEVICE_OBJECT Lower;
    /* NdisWriteErrorLogEntry: entries, and the last one's code and first two values */
    int ErrorLogs;
    ULONG ErrorCode, ErrorValue[2];
    /* NDIS timer: the one initialised last (NULL once the memory that
     * holds it is freed); NdisMSetTimer, routine and cancel counts; the
     * Set state when it was last cancelled */
    PNDIS_MINIPORT_TIMER Timer;
    PNDIS_MINIPORT_TIMER Timers[4];   /* every timer initialised (NULL once its memory is freed) */
    long TimerSets, TimerRuns, TimerCancels;
    int TimerSetAtCancel;
    int TimerCancelFails;             /* NdisMCancelTimer fails on a set timer: its DPC is due */
} FAKE;

extern FAKE Fake;

/* Hooks the test provides. */
extern void (*FakeSubmit)(PDEVICE_OBJECT device, PIRP irp, PURB urb);
extern BOOLEAN (*FakeCancel)(PIRP irp);
extern void (*FakeReceive)(PNDIS_PACKET packet);
extern void (*FakeSendComplete)(PNDIS_PACKET packet, NDIS_STATUS status);
extern void (*FakeStatus)(NDIS_STATUS status);
extern void (*FakeSetComplete)(NDIS_STATUS status);
extern void (*FakeResetComplete)(NDIS_STATUS status);

/* Complete an IRP the device model holds: sets the URB and IRP status
 * and runs the driver's completion routine on the calling thread. */
void FakeComplete(PIRP irp, NTSTATUS status, USBD_STATUS usb);
void FakeViolation(const char *what);
/* Locks the calling thread holds. */
long FakeHeld(void);
/* Run the NDIS timer's routine on the calling thread if it is set; 1 if
 * it ran. */
int FakeRunTimer(void);

#endif

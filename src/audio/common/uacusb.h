/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original bounded UAC 2.0 USB transport (any function uacdesc.c parses
 * and uacplan.c can plan). Include wdm.h first.
 */
#ifndef UACUSB_H
#define UACUSB_H
#include "uacdesc.h"
#include "uacplan.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The trace flavour only (build.cmd trace): one line to QEMU's debug
 * console port 0xE9 (uacusb.c). UAC_TRACE((tag, a, b, c)). */
#if defined(UAC_TRACE_E9)
void UacTrace(const char *tag, ULONG a, ULONG b, ULONG c);
#define UAC_TRACE(x) UacTrace x
#else
#define UAC_TRACE(x)
#endif
typedef struct UAC_DEV UAC_DEV;
typedef struct UAC_USB UAC_USB;
/* Playback: fill exactly bytes (silence is valid). Capture: bytes of whole
 * PCM frames received, in order. Both run on the stream's passive worker. */
typedef void (*UAC_USB_FILL)(PVOID context, PUCHAR destination, ULONG bytes);
typedef void (*UAC_USB_CAPTURE)(PVOID context, const UCHAR *source, ULONG bytes);
typedef void (*UAC_USB_PROGRESS)(PVOID context, ULONG bytes, ULONG packetErrors);
typedef struct UAC_USB_STATS {
    ULONG SubmittedBytes, SubmittedNonzeroBytes, CompletedBytes, DataUrbs, FeedbackUrbs;
    ULONG PacketErrors, GoodFeedback, BadFeedback, Cancelled, LastUsbStatus;
    ULONG LastFeedbackValue, LastFeedbackLength, FeedbackKeepalives;
    NTSTATUS LastStatus;
    ULONG Running, HighSpeed, ClockHz;
    ULONG MuteKnown, Mute, VolumeKnown;
    LONG Volume256Db;
    NTSTATUS FeatureStatus;
    /* Schedule diagnostics (build 6): frame numbers as the bus driver
     * reports them, and what the feedback IN packets came back as. */
    ULONG StartCurrentFrame, FbFirstStartFrame, FbLastStartFrame, DataFirstStartFrame;
    ULONG FbFourBytes, FbZeroLength, FbUrbFailed;
    ULONG FbCode[4], FbCodeCount[4];
    /* Build 8: every data packet's status, with the bytes of the
     * successful ones, so a bus that reports packets it never ran can be
     * told from one that reports them failed. */
    ULONG DataPktOk, DataPktOkBytes, DataUrbFailed;
    ULONG DataCode[4], DataCodeCount[4];
    /* Build 10: the stream's format and route. */
    ULONG Capture, Channels, Bits, Subslot, Interface, Alternate, Sync, ClockId;
    ULONG CapturedBytes;
    /* Build 19 (docs/design/uac2.md 19.3): Uac2QueueMs as read
     * (UAC_QUEUE_NONE when nothing usable was), the milliseconds in force,
     * the data URBs kept queued and one data URB's span. */
    ULONG QueueRead, QueueMs, DataTransfers, TransferUs;
    /* Build 20 (19.6): Uac2Realtime as read (UAC_QUEUE_NONE when nothing
     * usable was), whether it is on, and the priority the worker set (0
     * when it set none; KeSetPriorityThread's own answer is not kept). */
    ULONG RealtimeRead, Realtime, WorkerPriority;
    /* The worker's demotions from real time for a window busy past
     * UAC_DUTY_PERCENT (19.6, build 23): 0 in normal running. While
     * demoted, WorkerPriority reads the priority it was created at. */
    ULONG WorkerThrottles;
    /* Build 25 (docs/design/uac2.md 20.5), words 65 to 93, written by
     * uac2wdm.cpp (the transport leaves them 0): the render line's
     * volume as the driver found it at start. VolLayout is 25 when they
     * are filled. Slots 0 master, 1 and 2 logical channels 1 and 2; bit
     * n of VolRangeRead / VolStartKnown: slot n's RANGE was read / its
     * CUR was read at start. VolMin is the first subrange's MIN (the
     * silence value read as its next step), VolMax the last's MAX, VolRes
     * the smallest RES, VolTop the highest step, VolOffered the top the
     * driver offers, all 1/256 dB. */
    ULONG VolLayout, VolUnit, VolMode, VolChannels, VolAboveZero, VolLowered;
    ULONG VolRangeRead, VolStartKnown;
    ULONG VolCount[3];
    LONG VolMin[3], VolMax[3], VolRes[3], VolTop[3], VolOffered[3], VolStartCur[3];
    /* Design 25.7, word 94: 1 when the stream stopped polling feedback
     * after UAC_FB_RESTART_LIMIT refusals in a row (UAC_FB_OFF). */
    ULONG FeedbackOff;
    /* Design 26.13, words 95 to 97: the resyncs at explicit start frames
     * (after a URB refused for its start frame, or one lost whole on frames
     * behind the bus), the bus frame the last one read, and how many of
     * them a URB behind the bus began. */
    ULONG Resyncs, ResyncFrame, ResyncBehind;
} UAC_USB_STATS;

#define UAC_USB_RATES 16

/* Device: PASSIVE. Open copies the descriptor view, learns the bus speed
 * and selects the configuration (every interface at alternate 0). Rates
 * lists the sample rates the terminal's clock path can run (selector
 * inputs included), ascending; discrete ranges as given, continuous ones
 * as the common rates they contain. Close requires every stream destroyed.
 */
NTSTATUS UacUsbOpen(PDEVICE_OBJECT lower, PDEVICE_OBJECT pdo, const UAC_DESC *desc, UAC_DEV **out);
NTSTATUS UacUsbRates(UAC_DEV *dev, UAC_BYTE terminal, ULONG *rates, ULONG max, ULONG *count);
int UacUsbHighSpeed(UAC_DEV *dev);
#ifdef UAC_V1
/* uac1, PASSIVE: the USBDI bus interface's speed answer for the device
 * below lower; STATUS_NOT_SUPPORTED when there is none (no answer, or a
 * version 0 interface). UacUsbOpen takes the speed from desc->Speed. */
NTSTATUS UacUsbBusSpeed(PDEVICE_OBJECT lower, BOOLEAN *high);
#endif
void UacUsbClose(UAC_DEV *dev);
/* PASSIVE. One class request to the function, under the device lock that
 * also orders the clock requests: setup is the 8 bytes uacctl.c's
 * UacCtlSetup made (interface recipient only; wLength is the buffer).
 * *length in: wLength; out: the bytes moved. */
NTSTATUS UacUsbClassRequest(UAC_DEV *dev, const UCHAR *setup, PVOID bytes, ULONG *length);
/* PASSIVE, before UacUsbOpen too: GET_DESCRIPTOR from the device below
 * lower (type, index, language), at most 5 s, then cancelled and drained
 * as every request here; *got the bytes moved (0 on failure). */
NTSTATUS UacUsbDescriptor(PDEVICE_OBJECT lower, UCHAR type, UCHAR index, USHORT language,
                          PVOID buffer, USHORT size, ULONG *got);

/* Stream: one direction, one format, one rate. Create plans the alternate
 * (UacPlanPick) and the queue (UacPlanQueue: queueMs as UacPlanQueueMs
 * gave it for queueRead, bounded by the client's ring of ringBytes) and
 * changes nothing on the device. Start sets the clock
 * path (selector inputs, frequency; a running stream on the same clock
 * keeps its rate: a different rate is STATUS_DEVICE_BUSY), selects the
 * alternate and runs the worker. Stop closes repost, cancels every owned
 * IRP, waits for all completions and the thread, and returns the interface
 * to alternate zero; it never frees on a wait timeout. Callbacks run on
 * the worker until Stop returns. Stop must run before forwarding PnP
 * STOP/SURPRISE_REMOVE/REMOVE or D3.
 */
NTSTATUS UacUsbCreate(UAC_DEV *dev, int capture, UAC_BYTE terminal, ULONG channels,
    ULONG subslot, ULONG bits, ULONG config, ULONG rate,
    UAC_USB_FILL fill, UAC_USB_CAPTURE captured, UAC_USB_PROGRESS progress,
    PVOID context, ULONG queueRead, ULONG queueMs, ULONG ringBytes, UAC_USB **out);
/* PASSIVE, before Start: Uac2Realtime's raw reading for this stream
 * (UacPlanRealtime decides); the worker applies it as it starts. */
void UacUsbSetRealtime(UAC_USB *state, ULONG raw);
NTSTATUS UacUsbStart(UAC_USB *state);
void UacUsbStop(UAC_USB *state);
void UacUsbDestroy(UAC_USB *state);
void UacUsbGetStats(UAC_USB *state, UAC_USB_STATS *stats);
#ifdef __cplusplus
}
#endif
#endif

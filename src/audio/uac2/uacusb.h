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
} UAC_USB_STATS;

#define UAC_USB_RATES 16

/* Device: PASSIVE. Open copies the descriptor view, learns the bus speed
 * and selects the configuration (every interface at alternate 0). Rates
 * lists the sample rates the terminal's clock path can run (selector
 * inputs included), ascending; discrete ranges as given, continuous ones
 * as the common rates they contain. Close requires every stream destroyed.
 */
NTSTATUS UacUsbOpen(PDEVICE_OBJECT lower, const UAC_DESC *desc, UAC_DEV **out);
NTSTATUS UacUsbRates(UAC_DEV *dev, UAC_BYTE terminal, ULONG *rates, ULONG max, ULONG *count);
int UacUsbHighSpeed(UAC_DEV *dev);
void UacUsbClose(UAC_DEV *dev);

/* Stream: one direction, one format, one rate. Create plans the alternate
 * (UacPlanPick) and changes nothing on the device. Start sets the clock
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
    PVOID context, UAC_USB **out);
NTSTATUS UacUsbStart(UAC_USB *state);
void UacUsbStop(UAC_USB *state);
void UacUsbDestroy(UAC_USB *state);
void UacUsbGetStats(UAC_USB *state, UAC_USB_STATS *stats);
#ifdef __cplusplus
}
#endif
#endif

/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Experimental NT2000+ UVC USB transport for the M2 miniport (PASSIVE only).
 * Descriptors, configuration, PROBE/COMMIT, one isochronous IN stream fed
 * through the portable walker and payload core into a latest-frame buffer.
 * No bulk streaming, still images, controls or Windows 98 claim yet.
 */
#ifndef UVCUSB_H
#define UVCUSB_H
#include <wdm.h>
typedef struct UVU UVU;
typedef struct UVU_STATS {
    ULONG Frames, Dropped, Urbs, FailedUrbs, Gaps, Packets, Empty, Failed, Invalid;
    ULONG Alternate, PayloadBytes, FrameBytes, Speed, PacketsPerUrb, Urbs_, Stride;
    NTSTATUS LastStatus, FirstFailStatus;
    ULONG FirstFailUsbd, Submitted, BandwidthNeed, Quirk;
    /* PROBE: requested bFormatIndex/bFrameIndex, the device's answer, and
     * the descriptor frame it resolved to; payload-core diagnostics. */
    ULONG ReqFormat, ReqFrame, DevFormat, DevFrame, DevInterval, DevFrameBytes, DevPayload;
    ULONG CommitPayload, ResolvedWidth, ResolvedHeight;
    ULONG Eofs, FidFlips, DropBad, DropSize, LastDropUsed, ErrorPackets, Overflows;
} UVU_STATS;
/* Open the camera below `lower` (ConfigInfo->PhysicalDeviceObject): read
 * descriptors, parse the video function, select the configuration with
 * every interface at alternate 0. PASSIVE_LEVEL. */
NTSTATUS UvuOpen(PDEVICE_OBJECT lower, UVU **out);
/* Negotiate the uncompressed frame of this kind/size (UVC_DESC_YUY2 etc.)
 * at its default interval, choose the alternate, select it and start the
 * isochronous stream with its own passive thread. PASSIVE_LEVEL. */
NTSTATUS UvuStart(UVU *u, unsigned int kind, unsigned int width, unsigned int height);
/* Same, for descriptor frame index f (UVC_DESC.Frame[f]). */
NTSTATUS UvuStartFrame(UVU *u, unsigned int f);
/* The parsed descriptor model, valid from UvuOpen until UvuClose. */
struct UVC_DESC;
const struct UVC_DESC *UvuDescriptors(UVU *u);
/* Copy the newest complete frame newer than *sequence, waiting up to
 * timeoutMs. Returns bytes copied (0 on timeout, stop or size mismatch)
 * and updates *sequence. PASSIVE_LEVEL. */
ULONG UvuCopyLatest(UVU *u, PVOID dest, ULONG destBytes, ULONG *sequence, ULONG timeoutMs);
/* Stop the stream: abort the pipe, drain every transfer, join the thread,
 * select alternate 0 and release the per-stream buffers. The device stays
 * open for another start. Idempotent. PASSIVE_LEVEL. */
VOID UvuStop(UVU *u);
VOID UvuClose(UVU *u);
VOID UvuGetStats(UVU *u, UVU_STATS *stats);
/* Sequence number of the newest complete frame (it only grows; kept across
 * a stop). A stream session's baseline for UvuCopyLatest and drop counts. */
ULONG UvuLatestSequence(UVU *u);
#endif

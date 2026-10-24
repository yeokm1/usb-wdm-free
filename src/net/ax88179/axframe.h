/* SPDX-License-Identifier: GPL-2.0-or-later
 * ASIX AX88179/178A USB 3.0/2.0 to Gigabit Ethernet Devices
 * Copyright (C) 2011-2013 ASIX
 * Copyright (C) 2026 Yeo Kheng Meng
 * Framing semantics adapted from Linux v6.12 ax88179_178a.c.
 * Original bounded caller-buffer interface; no USB, NDIS or allocation.
 */
#ifndef AXFRAME_H
#define AXFRAME_H
#define AX_FRAME_MAX_AGGREGATE 65536UL
#define AX_FRAME_MAX_HEADERS 2048U
#define AX_FRAME_MAX_ETHERNET 8189U
typedef struct AX_FRAME_STATS {
    unsigned int Headers, Delivered, Dropped, Dummy, ChecksumErrors;
    unsigned int FcsStripped;   /* AxFrameReceiveAuto: records that ended in their FCS */
} AX_FRAME_STATS;
typedef void (*AX_FRAME_RECEIVE)(void *context, const unsigned char *frame,
    unsigned int bytes, unsigned long rawStatus);

/* Validate the COMPLETE aggregate before any delivery. RX frames exclude
 * the 2-byte device alignment pseudo-header; length excludes alignment pad.
 * CRC/drop/runt/over-limit frames are counted and omitted. Checksum bits
 * are reported, never a claim that the packet has a validated checksum.
 * maxFrame includes Ethernet header and payload, not FCS; range14..8189.
 * The upper value is a wire-field safety limit, not a hardware jumbo-frame
 * capability claim. Caller must configure RX to omit FCS; this code neither
 * programs that mode nor strips an optionally forwarded Ethernet FCS.
 * Caller serializes, retains immutable input until return and consumes/copies
 * synchronously in callback. Callback must not mutate input or reenter.
 * On malformed input return0, zero stats and make no callbacks.
 */
int AxFrameReceive(const unsigned char *aggregate, unsigned int bytes,
    unsigned int maxFrame, AX_FRAME_RECEIVE receive, void *context,
    AX_FRAME_STATS *stats);
/* Explicit physical-FCS mode: each frame includes a four-byte Ethernet
 * FCS after payload. Verify IEEE CRC32, drop bad-FCS frames, strip FCS
 * before callback. maxFrame still excludes FCS. Never guess the mode. */
int AxFrameReceiveFcs(const unsigned char *aggregate, unsigned int bytes,
    unsigned int maxFrame, AX_FRAME_RECEIVE receive, void *context,
    AX_FRAME_STATS *stats);
/* The driver's mode is "no FCS" (what the chip delivers with the
 * driver's receive configuration, as Linux, which never strips one).
 * This entry adds a safety net for a record that does end in its FCS
 * (the 2026-10-21 host-side run saw that): when the last four bytes are
 * the IEEE CRC32 of the rest they are stripped and counted in
 * FcsStripped, which the driver logs; any other record is delivered
 * whole. A frame without an FCS whose last four bytes equal its CRC
 * (2^-32) loses those bytes. */
int AxFrameReceiveAuto(const unsigned char *aggregate, unsigned int bytes,
    unsigned int maxFrame, AX_FRAME_RECEIVE receive, void *context,
    AX_FRAME_STATS *stats);

/* Encode one ordinary Ethernet frame, no TSO/checksum/VLAN offload. Uses
 * the ACTUAL selected endpoint maximum packet size (8/16/32/64/512/1024),
 * never bcdUSB as a proxy. Header padding flag handles exact packet multiples;
 * no extra USB ZLP or software padding is emitted here. Source/destination
 * must not overlap. Failure leaves destination unchanged and written=0.
 */
int AxFrameTransmit(const unsigned char *frame, unsigned int bytes,
    unsigned int maxFrame, unsigned int maxPacket, unsigned char *output,
    unsigned int capacity, unsigned int *written);
#endif

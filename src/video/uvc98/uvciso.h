/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original isochronous-IN packet walker for the UVC payload core.
 * No OS/USB I/O or allocation. UVC 1.1 section 2.4.3.3: each isochronous
 * (micro)frame transfer carries one complete payload with its own header.
 */
#ifndef UVCISO_H
#define UVCISO_H
#include "uvcpay.h"

/* Portable copy of one completed packet descriptor (USBD_ISO_PACKET_DESCRIPTOR
 * Offset/Length/Status). Length is the actual received byte count; Status 0
 * is success. Any nonzero status is treated as loss, never as data. */
typedef struct UVC_ISO_PACKET {
    unsigned long Offset, Length, Status;
} UVC_ISO_PACKET;

typedef struct UVC_ISO_STATS {
    unsigned long Urbs, FailedUrbs, Gaps, Packets, Empty, Fed, Failed, Invalid;
} UVC_ISO_STATS;

/* Feed one completed isochronous IN transfer to the payload core, in packet
 * order. buffer/bufferBytes is the transfer buffer; maxPacket is the selected
 * alternate's per-(micro)frame capacity (0 = only the buffer bound applies).
 * contiguous=0 means packets were missed since the previous transfer (a
 * resubmission gap): loss is reported before this transfer's data.
 * urbOk=0 means the transfer failed as a whole: loss is reported and no packet
 * data is trusted. A failed, out-of-bounds or oversized packet reports loss and
 * is skipped; an empty successful packet is ignored, as the payload core
 * requires. Returns 0 (and changes nothing) on invalid arguments, else 1.
 * Caller serializes with all other use of the same UVC_PAY.
 */
int UvcIsoFeed(UVC_PAY *pay, UVC_ISO_STATS *stats,
    const unsigned char *buffer, unsigned long bufferBytes, unsigned long maxPacket,
    const UVC_ISO_PACKET *packets, unsigned int count, int urbOk, int contiguous);

/* Transfer shape for one selected isochronous alternate (docs/design/uvc.md
 * section 5.4): Full Speed 16 packets per URB, 3 URBs; High Speed 8 packets
 * per URB, up to 6 URBs; maxUrbs further limits the count (host-controller
 * capacity is a parameter, not a constant). Every packet, high-bandwidth
 * High Speed (2 or 3 transactions) included, is laid out back to back at its
 * capacity: USBD's packet length is the distance to the next offset, so the
 * stride must equal the capacity. Only every-(micro)frame endpoints
 * (bInterval 1) are planned: anything else, a bulk alternate, an unknown
 * speed or maxUrbs 0 is refused. Arguments are UVC_DESC_ALT field values
 * (Type, PacketBytes, Transactions, ServiceMicroframes), so the planner does
 * not depend on the descriptor model's storage. Returns 1 and fills plan,
 * or 0 and leaves plan unchanged. */
typedef struct UVC_ISO_PLAN {
    unsigned int PacketsPerUrb, Urbs, PacketCapacity, PacketStride;
    unsigned long UrbBytes, UrbMicroseconds;
} UVC_ISO_PLAN;
#define UVC_ISO_FULL_SPEED 1
#define UVC_ISO_HIGH_SPEED 2
int UvcIsoPlan(unsigned int speed, unsigned int type, unsigned int packetBytes,
    unsigned int transactions, unsigned int serviceMicroframes,
    unsigned int maxUrbs, UVC_ISO_PLAN *plan);

#endif

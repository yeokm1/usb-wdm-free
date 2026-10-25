/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original implementation; see uvciso.h for the contract.
 */
#include "uvciso.h"

static void Count(unsigned long *value)
{
    if (*value != (unsigned long)-1) ++*value;
}

int UvcIsoPlan(unsigned int speed, unsigned int type, unsigned int packetBytes,
    unsigned int transactions, unsigned int serviceMicroframes,
    unsigned int maxUrbs, UVC_ISO_PLAN *plan)
{
    UVC_ISO_PLAN p;
    if (!plan || type != 1 || !maxUrbs || !packetBytes ||
        transactions < 1 || transactions > 3) return 0;
    if (speed == UVC_ISO_FULL_SPEED) {
        /* bInterval 1 at Full Speed: one packet every 1 ms frame (8 uframes). */
        if (serviceMicroframes != 8 || transactions != 1 || packetBytes > 1023) return 0;
        p.PacketsPerUrb = 16; p.Urbs = 3; p.UrbMicroseconds = 16000;
        p.PacketCapacity = packetBytes; p.PacketStride = packetBytes;
    } else if (speed == UVC_ISO_HIGH_SPEED) {
        if (serviceMicroframes != 1 || packetBytes > 1024) return 0;
        p.PacketsPerUrb = 8; p.Urbs = 6; p.UrbMicroseconds = 1000;
        p.PacketCapacity = packetBytes * transactions;
        /* Back to back at their capacity, high bandwidth too. USBD gives an
         * isochronous packet no length of its own: its length is the
         * distance to the next packet's offset (the last one's, to the
         * buffer's end). A 4096-byte stride (one packet per page, the
         * earlier layout) made every high-bandwidth packet 4096 bytes long
         * to the host controller driver, and xHCI98 refuses a packet longer
         * than the endpoint's payload (uvcsim legs, 2026-10-23). A packet
         * that crosses a page costs xHCI98 a second TRB instead. */
        p.PacketStride = p.PacketCapacity;
    } else return 0;
    if (p.Urbs > maxUrbs) p.Urbs = maxUrbs;
    p.UrbBytes = (unsigned long)p.PacketsPerUrb * p.PacketStride;
    *plan = p;
    return 1;
}

int UvcIsoFeed(UVC_PAY *pay, UVC_ISO_STATS *stats,
    const unsigned char *buffer, unsigned long bufferBytes, unsigned long maxPacket,
    const UVC_ISO_PACKET *packets, unsigned int count, int urbOk, int contiguous)
{
    unsigned int i;
    if (!pay || !pay->Buffer || !stats) return 0;
    if (urbOk && count && (!packets || !buffer)) return 0;
    Count(&stats->Urbs);
    if (!contiguous) { Count(&stats->Gaps); UvcPayLost(pay); }
    if (!urbOk) { Count(&stats->FailedUrbs); UvcPayLost(pay); return 1; }
    for (i = 0; i < count; i++) {
        const UVC_ISO_PACKET *p = &packets[i];
        Count(&stats->Packets);
        if (p->Status) { Count(&stats->Failed); UvcPayLost(pay); continue; }
        if (!p->Length) { Count(&stats->Empty); continue; }
        if (p->Offset > bufferBytes || p->Length > bufferBytes - p->Offset ||
            (maxPacket && p->Length > maxPacket)) {
            Count(&stats->Invalid); UvcPayLost(pay); continue;
        }
        Count(&stats->Fed);
        UvcPayFeed(pay, buffer + p->Offset, (unsigned int)p->Length);
    }
    return 1;
}

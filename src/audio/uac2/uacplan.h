/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original stream planning over a parsed UAC 2.0 function (uacdesc.h):
 * which formats a direction offers, which alternate carries a format at
 * a rate and bus speed, and the payload one service interval needs.
 * USB Audio 2.0 sections 4.9-4.10 and the Format Type I document; USB 2.0
 * section 5.6 (isochronous) and 9.6.6 (bInterval). No Windows types.
 */
#ifndef UACPLAN_H
#define UACPLAN_H
#include "uacdesc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UAC_PLAN_FORMATS 16

/* One PCM format a direction offers: channels, subslot bytes, valid bits,
 * and the USB-streaming terminal its alternates serve. */
typedef struct UAC_FORMAT {
    UAC_BYTE Channels, Subslot, Bits, Terminal;
    unsigned int ChannelConfig;   /* the first such alternate's bmChannelConfig */
} UAC_FORMAT;

/* The WAVEFORMATEXTENSIBLE dwChannelMask for a UAC 2.0 bmChannelConfig:
 * the spatial positions FL..TBR (bits 0-17) are the same bits in both;
 * 0 (unassigned) unless exactly channels positions are named there. */
unsigned int UacPlanChannelMask(unsigned int config, unsigned int channels);

/* Distinct formats of the playback (capture == 0) or capture alternates,
 * in descriptor order. Alternates a stream cannot run are left out: an
 * async OUT without an explicit feedback endpoint (implicit feedback is
 * not implemented), or a High Speed bInterval above 4 / Full Speed above
 * 1. Returns the count (0..max). */
unsigned int UacPlanFormats(const UAC_DESC *desc, int capture, int highSpeed,
                            UAC_FORMAT *out, unsigned int max);

/* Payload bytes one service interval can need at rate: ceil(rate *
 * period) frames, plus one frame of margin when the device's clock runs
 * free of the host's (headroom: async endpoints, whose feedback or data
 * rate may run fast), period 2^(interval-1) microframes (High Speed) or
 * frames (Full Speed). 0 for arguments outside UacRateInitialize's limits. */
unsigned int UacPlanPacketBytes(unsigned int rate, int highSpeed,
                                unsigned int interval, unsigned int frameBytes,
                                int headroom);

/* The alternate (index into desc->Alt) carrying exactly channels, subslot
 * and bits in the given direction, on the given USB-streaming terminal
 * (0: any), whose endpoint can hold UacPlanPacketBytes at rate; the
 * smallest such endpoint wins. Returns UAC_OK or UAC_NO_AUDIO. */
/* config: the format's bmChannelConfig (an alternate with another layout
 * is not taken), or UAC_PLAN_ANY_CONFIG. */
#define UAC_PLAN_ANY_CONFIG 0xffffffffU
int UacPlanPick(const UAC_DESC *desc, int capture, unsigned int terminal,
                unsigned int channels, unsigned int subslot, unsigned int bits,
                unsigned int config, unsigned int rate, int highSpeed, unsigned int *index);

/* The first terminal (descriptor order) a direction's runnable alternates
 * serve: the one a single wave pin represents. 0 if none. */
unsigned int UacPlanTerminal(const UAC_DESC *desc, int capture, int highSpeed);

/* Every USB-streaming terminal a direction's runnable alternates serve,
 * each once, in descriptor order, at most max: the endpoints a device
 * UacPlanQuirks lists offers one wave filter each (docs/design/uac2.md
 * section 15.3). out[0] is UacPlanTerminal's. Returns the count. */
unsigned int UacPlanTerminals(const UAC_DESC *desc, int capture, int highSpeed,
                              unsigned int *out, unsigned int max);

/* Every endpoint of a direction, not the first only (section 15.3 item 5). */
#define UAC_QUIRK_ENDPOINTS 0x1U

/* The quirks of a device by its idVendor and idProduct: 0 for a device
 * the table does not list. The ids are the boards Linux's sound/usb
 * mixer_maps.c (commit c339f401) names as carrying the Realtek ALC4080;
 * facts only. */
unsigned int UacPlanQuirks(unsigned int vid, unsigned int pid);

/* wMaxPacketSize as payload capacity: bits 10..0 times 1 + bits 12..11. */
unsigned int UacPlanCapacity(unsigned int maxPacket);

/* Sync type of an alternate's data endpoint: 1 async, 2 adaptive,
 * 3 synchronous, 0 none. */
#define UacPlanSync(alt) (((alt)->Attributes >> 2) & 3)

#ifdef __cplusplus
}
#endif
#endif

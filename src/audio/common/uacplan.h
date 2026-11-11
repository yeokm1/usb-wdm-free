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
#ifdef UAC_V1
    /* uac1 (docs/design/uac2.md 26.12): Subslot and Bits are what the pin
     * offers (2 and 16 for a converted format); the wire's are kept here
     * with the alternates' wFormatTag. */
    UAC_BYTE FormatTag, WireSubslot, WireBits;
#endif
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

/* UacPlanFormats for the alternates of one USB-streaming terminal only (0:
 * every terminal), so another terminal's formats never crowd max out (an
 * on-board codec's several outputs, docs/design/uac2.md section 15). */
unsigned int UacPlanFormatsFor(const UAC_DESC *desc, int capture, int highSpeed,
                               unsigned int terminal, UAC_FORMAT *out, unsigned int max);

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
#ifdef UAC_V1
/* USB Audio 1.0 (uac1): whether the alternate's own rate list holds rate
 * (discrete) or its continuous range contains it. UacPlanPick takes only
 * such an alternate. */
int UacPlanHasRate(const UAC_ALT *alt, unsigned int rate);
/* uac1: UacPlanPick's bits may carry the alternate's wFormatTag in bits
 * 8 to 15 (UAC_PLAN_BITS); 0 there means PCM. Only an alternate of that
 * tag is taken (PCM8, A-law and mu-law share 1-byte 8-bit subframes). */
#define UAC_PLAN_BITS(tag, bits) ((unsigned int)(bits) | ((unsigned int)(tag) << 8))
/* uac1 (26.12): one direction's formats with the rates each runs at
 * (rates[i * stride + k], counts[i]), put in the order a pin offers them:
 * by uacconv.h's rank, stable (native PCM first, as found), a converted
 * format keeping only the rates no earlier format of the same pin view
 * (channels, Subslot, Bits, ChannelConfig, terminal) carries, and a
 * format left with no rate removed. Returns the formats kept. */
unsigned int UacPlanOffer(UAC_FORMAT *formats, unsigned long *counts, unsigned long *rates,
                          unsigned int stride, unsigned int count);
#endif

/* The first terminal (descriptor order) a direction's runnable alternates
 * serve: the one a single wave pin represents. 0 if none. */
unsigned int UacPlanTerminal(const UAC_DESC *desc, int capture, int highSpeed);

/* Every USB-streaming terminal a direction's runnable alternates serve,
 * each once, in descriptor order, at most max: the endpoints a device
 * UacPlanQuirks lists offers one wave filter each (docs/design/uac2.md
 * section 15.3). out[0] is UacPlanTerminal's. Returns the count. */
unsigned int UacPlanTerminals(const UAC_DESC *desc, int capture, int highSpeed,
                              unsigned int *out, unsigned int max);

/* The endpoints of a direction (section 15.3 items 2 and 3): the
 * terminals of UacPlanTerminals, in that order, when every streaming
 * interface's runnable alternates, of either direction, link one terminal
 * and no terminal is linked from two interfaces; else only the first
 * terminal, as before.
 * Returns the count. */
unsigned int UacPlanEndpoints(const UAC_DESC *desc, int capture, int highSpeed,
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

/* The stream queue (docs/design/uac2.md section 19): Uac2QueueMs's
 * default and range, the most data URBs kept queued, and the raw value
 * that stands for "nothing usable was read". */
#define UAC_QUEUE_DEFAULT_MS 32U
#define UAC_QUEUE_MIN_MS 16U
#define UAC_QUEUE_MAX_MS 128U
#define UAC_QUEUE_MAX_URBS 32U
#define UAC_QUEUE_NONE 0xFFFFFFFFUL

/* Uac2QueueMs in force for a raw reading: UAC_QUEUE_NONE gives the
 * default, anything else is clamped to the range (19.3). */
unsigned int UacPlanQueueMs(unsigned long raw);

/* Uac2Realtime (docs/design/uac2.md 19.6): 1 runs the stream's worker at
 * LOW_REALTIME_PRIORITY, 0 leaves it at the system thread's priority (the
 * wake's IO_SOUND_INCREMENT still applies). UAC_QUEUE_NONE (nothing usable
 * read) gives the default, 1; any other value is 0 for 0, else 1. */
#define UAC_REALTIME_DEFAULT 1U
unsigned int UacPlanRealtime(unsigned long raw);

/* The worker's fairness rule (19.6, build 23). The worker measures, in
 * interrupt time, how long it runs between its waits (busy) in windows of
 * at least UAC_DUTY_WINDOW_100NS. A window busy for UAC_DUTY_PERCENT or
 * more of its length demotes a real-time worker to the priority it was
 * created at for the next window; a window below it restores real time.
 * It never sleeps, so a working stream keeps refilling; demoted, it shares
 * the processor with the normal threads, the one that asks Stop among
 * them. Busy time is measured, not inferred from whether a wait blocked,
 * so no wake racing a wait can hide a loop. At tick granularity (10 to
 * 15.6 ms before Windows 8) a short pass is counted only when a tick falls
 * inside it, which samples the busy fraction; a normal stream (a pass well
 * under a millisecond, at most about 1.25 a millisecond) stays far below
 * the threshold. UacPlanDuty: the window's length and busy time so far;
 * returns the new demoted state (0 or 1) once the window is long enough,
 * else the current one; *closed 1 when the window ends now. */
#define UAC_DUTY_WINDOW_MS 200U
#define UAC_DUTY_PERCENT 60U
#define UAC_DUTY_SHORT_WAIT_US 50U
unsigned int UacPlanDuty(unsigned __int64 elapsed, unsigned __int64 busy,
                         unsigned __int64 window, unsigned int demoted,
                         unsigned int *closed);
/* Build 24: a demoted window always ends in real time again (one window
 * demoted at most, then measured afresh), and every interval is in
 * performance-counter units: window is UAC_DUTY_WINDOW_MS of them, and a
 * wait shorter than UAC_DUTY_SHORT_WAIT_US counts as busy (an immediate
 * return, not a block). UacPlanCounts: n microseconds or milliseconds in
 * counter units at frequency hz (at least 1). */
unsigned __int64 UacPlanCounts(unsigned __int64 hz, unsigned long n,
                               unsigned long perSecond);

/* Data URBs kept queued for a stream (19.4): *packets per URB (unchanged,
 * section 4.4) and *spanUs, one URB's span; ceil(queueMs / span), lowered
 * so the queue stays at least one URB below ringBytes (0: no ring bound).
 * One URB's bytes: bytesPerSecond over the span, rounded up, plus an
 * eighth (the fastest feedback uacrate.c accepts) and one frameBytes per
 * packet, at most capacity bytes a packet (0: no such cap). Never below 2
 * or above UAC_QUEUE_MAX_URBS. queueMs is clamped as UacPlanQueueMs;
 * interval is bInterval (0 taken as 1). */
unsigned int UacPlanQueue(int highSpeed, unsigned int interval, unsigned int queueMs,
                          unsigned long bytesPerSecond, unsigned int frameBytes,
                          unsigned int capacity, unsigned long ringBytes,
                          unsigned int *packets, unsigned long *spanUs);

/* A lost service interval (docs/design/uac2.md section 25). Windows'
 * USB stack completes an isochronous URB whose every packet failed with
 * USBD_STATUS_ISOCH_REQUEST_FAILED (usb.h: "returned by HCD if all packets
 * in an iso transfer complete with an error"), each packet carrying its
 * own status (not accessed by the controller, late). The pipe stays
 * usable: the next URBs are scheduled again. UacPlanIsoLost: 1 when a
 * completion is such a URB (that URB status, packets > 0 and every one of
 * them failed), else 0. The caller excludes cancellation. */
#define UAC_ISO_LOST_STATUS 0xC0000B00UL
unsigned int UacPlanIsoLost(unsigned long urbStatus, unsigned int packets, unsigned int failed);
/* How many lost data URBs in a row a stream rides out before it stops:
 * those covering UAC_LOST_LIMIT_MS at spanUs each (rounded up), at least
 * 2; spanUs 0 is taken as 1 ms. */
#define UAC_LOST_LIMIT_MS 1000U
unsigned int UacPlanLostLimit(unsigned long spanUs);

/* Design 25.7: a feedback URB (only feedback) that comes back with
 * USBD_STATUS_BAD_START_FRAME. run is how many came back so in a row
 * before this one. UAC_FB_RESUBMIT: pass it over and resubmit the slot
 * (ASAP), the last good value kept, while fewer than UAC_FB_RESTART_LIMIT
 * did; UAC_FB_OFF at the limit: stop polling feedback for the rest of the
 * stream, which goes on at the last good or nominal cadence (a stack that
 * keeps refusing has halted the pipe; the next start selects the alternate
 * afresh); UAC_FB_NONE for any other status (the rules before). */
#define UAC_ISO_BAD_START_STATUS 0xC0000A00UL
/* Design 25.8: a data URB the stack refused as a whole for its start frame
 * (that URB status, packets > 0) is a lost interval as 25.3's whole-URB
 * failure is, under the same run limit. 1 when it is, else 0. */
unsigned int UacPlanDataRefused(unsigned long urbStatus, unsigned int packets);
#define UAC_FB_RESTART_LIMIT 8U
#define UAC_FB_NONE 0U
#define UAC_FB_RESUBMIT 1U
#define UAC_FB_OFF 2U
unsigned int UacPlanFbRefused(unsigned long urbStatus, unsigned int run);
/* Design 26.13: after a URB refused for its start frame, once no slot is
 * with the stack, every slot of a pipe is submitted at an explicit frame:
 * slot k (in submission order) at UacPlanResyncFrame(current, k, frames) =
 * current + UAC_RESYNC_LEAD + k * frames, wrapping as a 32-bit frame
 * number; ASAP after that. UacPlanSpanFrames: the 1 ms frames a URB of
 * spanUs covers, rounded up, at least 1. UAC_RESYNC_LIMIT resyncs with no
 * good data URB between them end the stream. */
#define UAC_RESYNC_LEAD 8UL
#define UAC_RESYNC_LIMIT 4U
/* The longest the worker holds the stream's slots waiting for the stack
 * to give back the last one; past it the stream ends. */
#define UAC_RESYNC_WAIT_MS 1000U
unsigned long UacPlanResyncFrame(unsigned long current, unsigned long index, unsigned long frames);
unsigned long UacPlanSpanFrames(unsigned long spanUs);
/* Design 26.13: 1 when a URB's frames (start, frames long) ended more
 * than slack frames before the frame current (for a data URB lost whole:
 * the bus frame read as it was submitted, so the stack placed it behind
 * the bus: its ASAP chain lags, as 98 SE's own UHCI's does after a stall
 * until it refuses a start frame); the frame numbers compared as 32-bit
 * and wrapping. 0 when they end later, or the lag is over 2^31. */
unsigned int UacPlanBehind(unsigned long current, unsigned long start, unsigned long frames,
                           unsigned long slack);
/* Design 26.13: a stream's recovery state, which each retired data URB
 * updates (UacPlanDataDone) and each resync reads (UacPlanResyncStart).
 * ResyncRun: resyncs since the last data URB that carried audio; LagSuspect:
 * the bus frame is read before each ASAP data submission; Resync: a resync
 * is wanted (no idle slot is submitted until it runs). */
typedef struct UAC_RECOVERY {
    unsigned int ResyncRun, LagSuspect, Resync;
} UAC_RECOVERY;
/* What a data URB came back as. AUDIO: successful, and for capture at least
 * one PCM frame delivered (render: any successful URB); EMPTY: a successful
 * capture URB that delivered none (98 SE's own UHCI returns a recording's
 * URBs placed behind the bus so); REFUSED: refused whole for its start
 * frame (UacPlanDataRefused); LOST: lost whole (UacPlanIsoLost); OTHER:
 * anything else (a stop, a cancellation, any other failure). */
#define UAC_DATA_AUDIO 0U
#define UAC_DATA_EMPTY 1U
#define UAC_DATA_REFUSED 2U
#define UAC_DATA_LOST 3U
#define UAC_DATA_OTHER 4U
/* behind: for LOST or EMPTY, UacPlanBehind of the frame read before its
 * submission (0 when none was read). Only AUDIO clears ResyncRun and
 * LagSuspect; REFUSED wants a resync; LOST and EMPTY arm LagSuspect and,
 * behind the bus with no resync wanted yet, want one. Returns 1 when this
 * URB's lag began the resync, else 0. */
unsigned int UacPlanDataDone(UAC_RECOVERY *r, unsigned int kind, unsigned int behind);
/* At a resync, with the bus frame in hand: 0 when UAC_RESYNC_LIMIT resyncs
 * already ran with no AUDIO URB since (the stream ends); else 1, the resync
 * counted and no longer wanted. */
unsigned int UacPlanResyncStart(UAC_RECOVERY *r);
/* Design 26.13: the worker sends its frame requests and does not wait for
 * them; one is out at a time and its storage is the stream's until it is
 * back. UacPlanFrameCancel: 1 when the worker cancels the one out (once): at
 * a stop, or a submission-frame request out past UAC_FRAME_WAIT_MS. A
 * resync's request is bounded by the hold's UAC_RESYNC_WAIT_MS instead. */
#define UAC_FRAME_WAIT_MS 100U
unsigned int UacPlanFrameCancel(unsigned int stop, unsigned int cancelled, unsigned int forResync,
                                unsigned long ageMs);
/* An idle data slot the worker submits ASAP. lagSuspect: LagSuspect; taken:
 * a submission-frame request came back this pass (the frame is its, or none
 * if it failed); out: a request is with the stack; cancelled: that one was
 * cancelled. SUBMIT: submit with no frame; FRAME: submit with the frame
 * taken; ASK: send a request and hold the slot; HOLD: hold the slot. */
#define UAC_GATE_SUBMIT 0U
#define UAC_GATE_FRAME 1U
#define UAC_GATE_ASK 2U
#define UAC_GATE_HOLD 3U
unsigned int UacPlanDataGate(unsigned int lagSuspect, unsigned int taken, unsigned int out,
                             unsigned int cancelled);
/* A wanted resync with no slot out. taken: the resync's own request came
 * back this pass; out: a request is with the stack. RUN: resync on the
 * frame taken (a failed request ends the stream); ASK: send the resync's
 * request; WAIT: wait for the one out. */
#define UAC_RSY_RUN 0U
#define UAC_RSY_ASK 1U
#define UAC_RSY_WAIT 2U
unsigned int UacPlanResyncGate(unsigned int taken, unsigned int out);
/* 1 when a resync is still wanted (resync) elapsedMs after the hold
 * began, past UAC_RESYNC_WAIT_MS: the stream ends, whatever is out or has
 * just come back. */
unsigned int UacPlanHoldExpired(unsigned int resync, unsigned long elapsedMs);
/* The resync's frame, read by a request that was out ageMs before the
 * worker took it (the stack read it somewhere in that time). 1 with *ahead
 * = frame + ageMs (1 ms frames, 32-bit wrap), at or past the bus's frame
 * when it is taken, when ageMs is at most UAC_FRAME_FRESH_MS; else 0: read
 * afresh. Bounded so, the last slot of the largest queue lies at most
 * UAC_FRAME_FRESH_MS + UAC_RESYNC_LEAD + 31 x 8 frames past the bus when
 * taken, inside the stack's USBD_ISO_START_FRAME_RANGE (1024). */
#define UAC_FRAME_FRESH_MS 64U
unsigned int UacPlanResyncAhead(unsigned long frame, unsigned long ageMs, unsigned long *ahead);
/* 1 when the worker may exit: stopped, no slot and no frame request with
 * the stack (their storage is freed only after it). */
unsigned int UacPlanWorkerExit(unsigned int stop, unsigned int slotsOut, unsigned int frameOut);

/* Section 25: 1 when a device's PDO belongs to Windows 2000's USB 1.1
 * hub driver, whose devices run at Full or Low Speed: the driver object's
 * name (count UTF-16 units, no terminator needed) is "\Driver\usbhub",
 * compared without case, whole. 2000's USB 2.0 hub driver is usbhub20. */
unsigned int UacPlanUsb11Hub(const unsigned short *name, unsigned int count);

/* Sync type of an alternate's data endpoint: 1 async, 2 adaptive,
 * 3 synchronous, 0 none. */
#define UacPlanSync(alt) (((alt)->Attributes >> 2) & 3)

#ifdef __cplusplus
}
#endif
#endif

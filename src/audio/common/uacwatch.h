/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original feedback deadline predicate, independent of Windows types.
 * Inputs MUST be the same monotonic100ns clock domain, never wall time.
 */
#ifndef UACWATCH_H
#define UACWATCH_H
#ifdef _MSC_VER
typedef unsigned __int64 UAC_WATCH_TICK;
#else
typedef unsigned long long UAC_WATCH_TICK;
#endif
#define UAC_FEEDBACK_TIMEOUT_100NS 2000000u
/* Preserve the existing strict >200ms boundary. A backward observation
 * is not elapsed time. Current interrupt time cannot normally go backward;
 * do not turn an unsigned underflow into an immediate false timeout.
 */
static int UacFeedbackExpired(UAC_WATCH_TICK now,UAC_WATCH_TICK last)
{
    return now>=last && now-last>UAC_FEEDBACK_TIMEOUT_100NS;
}
/* Before the first valid rate of a stream the deadline is 1 s, not 200 ms:
 * a device may answer its feedback endpoint with nothing for a while after
 * SET_INTERFACE, and a bus that buffers isochronous IN before delivering
 * (QEMU usb-redir: 120 packets here) or a guest whose clock jumps after a
 * stall makes 200 ms of guest time too short. The PCM planner runs at the
 * nominal rate until then, so the longer start costs nothing. Once a valid
 * rate has arrived the 200 ms rule above applies unchanged.
 */
#define UAC_FEEDBACK_START_TIMEOUT_100NS 10000000u
static int UacFeedbackLate(UAC_WATCH_TICK now,UAC_WATCH_TICK last,int active)
{
    if(active) return UacFeedbackExpired(now,last);
    return now>=last && now-last>UAC_FEEDBACK_START_TIMEOUT_100NS;
}
/* docs/design/uac2.md section 25: the same deadlines in bus time.
 * unusableUs is the service time covered by the feedback URBs that came
 * back (not cancelled) without a usable packet since the last usable one.
 * A time in which the stack held every URB and none came back, or the
 * guest's clock jumped, is not counted against the device. Stale past
 * 200 ms once the stream has had a valid value, 1 s before. */
static int UacFeedbackStale(unsigned long unusableUs,int active)
{
    return unusableUs>(active?UAC_FEEDBACK_TIMEOUT_100NS:UAC_FEEDBACK_START_TIMEOUT_100NS)/10u;
}
/* One feedback packet's service time in microseconds: at High Speed
 * 125 << (bInterval - 1) (bInterval 1 to 4: one packet a serviced
 * microframe, one a frame at 4), at Full Speed one frame. */
static unsigned long UacFeedbackPacketUs(int highSpeed,unsigned int interval)
{
    if(!highSpeed) return 1000u;
    if(interval<1) interval=1;
    if(interval>4) interval=4;
    return 125ul<<(interval-1);
}
/* Both bus-time counts: unusableUs as above; quietUs the service time of
 * the data URBs that came back since the last usable feedback packet
 * (feedback held, or coming back unusable, while data plays). Stale when
 * either is. */
static int UacFeedbackStaleBus(unsigned long unusableUs,unsigned long quietUs,int active)
{
    return UacFeedbackStale(unusableUs>quietUs?unusableUs:quietUs,active);
}
/* The deadline as applied: stale only when both the bus time (above) and
 * the interrupt time since the last usable packet (or the stream's start),
 * UacFeedbackLate, are past it. Bus time alone overcounts where a stack
 * returns URBs faster than their span (98 SE under NUSB: zero-length
 * feedback at a stream's start came back several times faster than its
 * packets' service time and ended the stream within 0.3 s); interrupt time
 * alone counts a time in which the stack held every URB (7 under its own
 * EHCI). The earlier of the two never decides. */
static int UacFeedbackStaleBoth(UAC_WATCH_TICK now,UAC_WATCH_TICK last,unsigned long unusableUs,
                                unsigned long quietUs,int active)
{
    return UacFeedbackLate(now,last,active) && UacFeedbackStaleBus(unusableUs,quietUs,active);
}
/* Whether a data URB that came back as completion number stamp counts
 * toward quietUs, the last usable feedback having come back as number
 * from: only one that came back after it (the worker retires feedback
 * first, so a backlog of older data is retired after newer feedback).
 * Modulo 2^32. */
static int UacFeedbackAfter(unsigned long stamp,unsigned long from)
{
    return (long)(stamp-from)>0;
}
/* unusableUs after a URB of n packets came back unusable, saturated. */
static unsigned long UacFeedbackAdd(unsigned long unusableUs,unsigned long n,unsigned long packetUs)
{
    unsigned long add=n*packetUs;
    if(packetUs&&add/packetUs!=n) return 0xfffffffful;
    return unusableUs>0xfffffffful-add?0xfffffffful:unusableUs+add;
}
#endif

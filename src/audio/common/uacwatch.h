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
#endif

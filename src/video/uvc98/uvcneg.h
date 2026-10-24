/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original PROBE/COMMIT negotiation sequence (UVC 1.1 section 4.3.1.1;
 * docs/design/uvc.md section 5.2) over the bounded descriptor model and
 * control codec. No OS or USB I/O: the caller supplies one class-request
 * callback, which the kernel adapter maps to a control transfer.
 */
#ifndef UVCNEG_H
#define UVCNEG_H
#include "uvcdesc.h"

#define UVC_NEG_SET_CUR 0x01
#define UVC_NEG_GET_CUR 0x81
#define UVC_NEG_PROBE 1   /* VS_PROBE_CONTROL selector */
#define UVC_NEG_COMMIT 2  /* VS_COMMIT_CONTROL selector */

/* One VideoStreaming interface class request. request is SET_CUR/GET_CUR,
 * selector PROBE/COMMIT, iface the VS interface number. For SET_CUR the
 * buffer holds bytes to send; for GET_CUR the callback fills it. Return the
 * number of bytes actually transferred, or a negative value on failure. */
typedef int (*UVC_NEG_XFER)(void *context, unsigned int request,
    unsigned int selector, unsigned int iface, unsigned char *buffer,
    unsigned int bytes);

#define UVC_NEG_OK 0
#define UVC_NEG_ARG 1       /* invalid arguments or descriptor/frame index */
#define UVC_NEG_IO 2        /* a class request failed or was short */
#define UVC_NEG_REJECTED 3  /* the device's PROBE answer cannot be used */

typedef struct UVC_NEG_RESULT {
    UVC_CTRL Control;          /* the decoded control that was committed */
    UVC_CTRL DeviceControl;    /* the device's own PROBE answer */
    unsigned int FrameIndex;   /* index into desc->Frame[] */
    unsigned int AltIndex;     /* index into desc->Alt[] */
    unsigned int Requests;     /* class requests issued (for tests/logs) */
} UVC_NEG_RESULT;

/* Negotiate desc->Frame[frameIndex] at interval (100 ns units; 0 = the
 * frame's default). Sequence: SET_CUR(PROBE) with format/frame/interval and
 * the fixed-interval hint, GET_CUR(PROBE), validate the answer with
 * UvcDescSelect (same stream, a known frame, a listed interval, exact
 * uncompressed size, an alternate that carries the payload), then
 * SET_CUR(COMMIT) with exactly the bytes the device answered. Nothing is
 * committed unless every earlier step succeeded. On failure result is
 * zeroed except Requests. */
int UvcNegotiate(const UVC_DESC *desc, unsigned int frameIndex,
    unsigned long interval, UVC_NEG_XFER xfer, void *context,
    UVC_NEG_RESULT *result);
/* Same, with a per-camera payload limit (0 = none): when the device's PROBE
 * answer asks more than payloadLimit bytes per (micro)frame, the committed
 * bytes carry payloadLimit instead, and the alternate is chosen for it. This
 * is a quirk for cameras that answer their largest payload for every frame
 * (docs/design/uvc.md section 5.2); the device's own answer is kept in
 * result->DeviceControl. */
int UvcNegotiateLimited(const UVC_DESC *desc, unsigned int frameIndex,
    unsigned long interval, unsigned long payloadLimit, UVC_NEG_XFER xfer,
    void *context, UVC_NEG_RESULT *result);
#endif

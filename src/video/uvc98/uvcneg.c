/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original implementation; see uvcneg.h for the contract.
 */
#include "uvcneg.h"

static void Zero(void *p, unsigned int n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

int UvcNegotiate(const UVC_DESC *desc, unsigned int frameIndex,
    unsigned long interval, UVC_NEG_XFER xfer, void *context,
    UVC_NEG_RESULT *result)
{
    return UvcNegotiateLimited(desc, frameIndex, interval, 0, xfer, context, result);
}

int UvcNegotiateLimited(const UVC_DESC *desc, unsigned int frameIndex,
    unsigned long interval, unsigned long payloadLimit, UVC_NEG_XFER xfer,
    void *context, UVC_NEG_RESULT *result)
{
    UVC_CTRL device;
    unsigned char probe[48], answer[48];
    unsigned int size, iface, frame, alt, requests = 0;
    const UVC_DESC_FRAME *f;
    UVC_CTRL c, got;
    int n;
    if (!result) return UVC_NEG_ARG;
    Zero(result, sizeof(*result));
    if (!desc || !desc->Valid || !xfer || frameIndex >= desc->FrameCount)
        return UVC_NEG_ARG;
    f = &desc->Frame[frameIndex];
    if (f->Stream >= desc->StreamCount) return UVC_NEG_ARG;
    size = UvcCtrlSize(desc->Revision);
    if (!size || size > sizeof(probe)) return UVC_NEG_ARG;
    if (!interval) interval = f->DefaultInterval;
    if (!UvcDescInterval(desc, frameIndex, interval)) return UVC_NEG_ARG;
    iface = desc->Stream[f->Stream].Interface;
    /* Unsupported and negotiable fields stay zero (operational model). */
    Zero(&c, sizeof(c));
    c.Hint = 1; /* dwFrameInterval fixed */
    c.Format = f->Format; c.Frame = f->Frame; c.Interval = interval;
    if (!UvcCtrlEncode(desc->Revision, &c, probe, size)) return UVC_NEG_ARG;
    ++requests;
    n = xfer(context, UVC_NEG_SET_CUR, UVC_NEG_PROBE, iface, probe, size);
    if (n != (int)size) { result->Requests = requests; return UVC_NEG_IO; }
    Zero(answer, sizeof(answer));
    ++requests;
    n = xfer(context, UVC_NEG_GET_CUR, UVC_NEG_PROBE, iface, answer, size);
    if (n != (int)size) { result->Requests = requests; return UVC_NEG_IO; }
    if (!UvcCtrlDecode(desc->Revision, answer, size, &got)) {
        result->Requests = requests; return UVC_NEG_REJECTED;
    }
    device = got;
    if (payloadLimit && got.PayloadBytes > payloadLimit) {
        /* Commit the limited payload; re-encode so COMMIT carries it. */
        got.PayloadBytes = payloadLimit;
        if (!UvcCtrlEncode(desc->Revision, &got, answer, size)) {
            result->Requests = requests; return UVC_NEG_REJECTED;
        }
    }
    if (!UvcDescSelect(desc, f->Stream, &got, &frame, &alt) ||
        desc->Alt[alt].Stream != f->Stream) {
        result->Requests = requests; return UVC_NEG_REJECTED;
    }
    ++requests;
    n = xfer(context, UVC_NEG_SET_CUR, UVC_NEG_COMMIT, iface, answer, size);
    if (n != (int)size) { result->Requests = requests; return UVC_NEG_IO; }
    result->Control = got; result->DeviceControl = device;
    result->FrameIndex = frame; result->AltIndex = alt;
    result->Requests = requests;
    return UVC_NEG_OK;
}

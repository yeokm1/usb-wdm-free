/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * M2 stream formats built from the camera's own descriptors (KS data ranges
 * from published ks.h/ksmedia.h). The camera's uncompressed YUY2 frames, and
 * for each of them an RGB24 (BI_RGB, bottom-up) format the driver converts
 * from YUY2 (design uvc.md 6): VfW on 98 SE and 2000 has no YUY2 decoder.
 */
#ifndef UVCFMT_H
#define UVCFMT_H
#include <strmini.h>
#include <ksmedia.h>
#include "uvcdesc.h"
#define UVFMT_MAX 64 /* UVFMT_MAX / 2 frames, each as RGB24 and as YUY2 */
#define UVFMT_MAX_BYTES (640u * 480u * 2u) /* bounds the camera (YUY2) frame */
#define UVFMT_YUY2 0
#define UVFMT_RGB24 1
typedef struct UVFMT_ENTRY {
    KS_DATARANGE_VIDEO Range;
    KS_DATAFORMAT_VIDEOINFOHEADER Format;
    unsigned int Kind;               /* UVFMT_YUY2 or UVFMT_RGB24 */
    unsigned int Width, Height, FrameIndex;
    unsigned long FrameBytes;        /* bytes the pin delivers per frame */
    unsigned long SourceBytes;       /* bytes of the camera's YUY2 frame */
    unsigned long Interval;
} UVFMT_ENTRY;
typedef struct UVFMT {
    unsigned int Count;
    UVFMT_ENTRY Entry[UVFMT_MAX];
    PKSDATAFORMAT Ranges[UVFMT_MAX]; /* HW_STREAM_INFORMATION.StreamFormatsArray */
} UVFMT;
/* Every YUY2 frame of the first stream up to UVFMT_MAX_BYTES, one fixed
 * (default) interval each, at most UVFMT_MAX / 2 frames: first an RGB24
 * entry per frame, then the YUY2 entry per frame, in descriptor order.
 * RGB24 comes first so that a client which takes the first range gets a
 * format every 98 SE and 2000 system can draw. Returns the count. */
unsigned int UvfBuild(const UVC_DESC *desc, UVFMT *table);
/* Index of the table entry this requested format names, or -1. The frame
 * rate is not matched: the camera's committed interval is delivered. */
int UvfMatch(const UVFMT *table, const KSDATAFORMAT *format);
/* SRB_GET_DATA_INTERSECTION over the table (same buffer rules as M0/M1). */
NTSTATUS UvfIntersect(const UVFMT *table, PHW_STREAM_REQUEST_BLOCK srb);
/* Bytes of one RGB24 DIB row (DWORD aligned). */
unsigned long UvfRgbStride(unsigned int width);
/* YUY2 (BT.601 studio range, top-down) to a bottom-up RGB24 DIB (B,G,R),
 * integer only. width even; dst holds UvfRgbStride(width) * height bytes. */
void UvfYuy2ToRgb24(const unsigned char *src, unsigned char *dst,
    unsigned int width, unsigned int height);
#endif

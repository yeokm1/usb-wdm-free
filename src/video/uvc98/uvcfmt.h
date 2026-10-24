/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * M2 stream formats built from the camera's own descriptors (KS data ranges
 * from published ks.h/ksmedia.h). Uncompressed YUY2 frames only for now.
 */
#ifndef UVCFMT_H
#define UVCFMT_H
#include <strmini.h>
#include <ksmedia.h>
#include "uvcdesc.h"
#define UVFMT_MAX 16
#define UVFMT_MAX_BYTES (640u * 480u * 2u) /* bounds the per-frame buffers */
typedef struct UVFMT_ENTRY {
    KS_DATARANGE_VIDEO Range;
    KS_DATAFORMAT_VIDEOINFOHEADER Format;
    unsigned int Width, Height, FrameIndex;
    unsigned long FrameBytes, Interval;
} UVFMT_ENTRY;
typedef struct UVFMT {
    unsigned int Count;
    UVFMT_ENTRY Entry[UVFMT_MAX];
    PKSDATAFORMAT Ranges[UVFMT_MAX]; /* HW_STREAM_INFORMATION.StreamFormatsArray */
} UVFMT;
/* Every YUY2 frame of the first stream up to UVFMT_MAX_BYTES, one fixed
 * (default) interval each, in descriptor order. Returns the count. */
unsigned int UvfBuild(const UVC_DESC *desc, UVFMT *table);
/* Index of the table entry this requested format names exactly, or -1. */
int UvfMatch(const UVFMT *table, const KSDATAFORMAT *format);
/* SRB_GET_DATA_INTERSECTION over the table (same buffer rules as M0/M1). */
NTSTATUS UvfIntersect(const UVFMT *table, PHW_STREAM_REQUEST_BLOCK srb);
#endif

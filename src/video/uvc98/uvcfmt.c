/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original implementation; see uvcfmt.h. Field use follows the M0/M1
 * single-format metadata in uvcmini.c, repeated per camera frame.
 */
#include "uvcfmt.h"

#define YUY2_FOURCC 0x32595559
static const GUID typeVideo = { STATIC_KSDATAFORMAT_TYPE_VIDEO };
static const GUID videoInfo = { STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO };
static const GUID yuy2 = { YUY2_FOURCC, 0, 0x10, {0x80,0,0,0xaa,0,0x38,0x9b,0x71} };

static int Same(const GUID *a, const GUID *b)
{
    const ULONG *x = (const ULONG *)a, *y = (const ULONG *)b;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}

static void Fill(UVFMT_ENTRY *e)
{
    KS_DATAFORMAT_VIDEOINFOHEADER *f = &e->Format;
    KS_DATARANGE_VIDEO *r = &e->Range;
    ULONG fps = e->Interval ? (10000000 + e->Interval / 2) / e->Interval : 1;
    RtlZeroMemory(f, sizeof(*f));
    f->DataFormat.FormatSize = sizeof(*f);
    f->DataFormat.SampleSize = e->FrameBytes;
    f->DataFormat.MajorFormat = typeVideo;
    f->DataFormat.SubFormat = yuy2;
    f->DataFormat.Specifier = videoInfo;
    f->VideoInfoHeader.rcSource.right = e->Width;
    f->VideoInfoHeader.rcSource.bottom = e->Height;
    f->VideoInfoHeader.rcTarget = f->VideoInfoHeader.rcSource;
    f->VideoInfoHeader.dwBitRate = e->FrameBytes * 8 * fps;
    f->VideoInfoHeader.AvgTimePerFrame = e->Interval;
    f->VideoInfoHeader.bmiHeader.biSize = sizeof(KS_BITMAPINFOHEADER);
    f->VideoInfoHeader.bmiHeader.biWidth = e->Width;
    f->VideoInfoHeader.bmiHeader.biHeight = e->Height;
    f->VideoInfoHeader.bmiHeader.biPlanes = 1;
    f->VideoInfoHeader.bmiHeader.biBitCount = 16;
    f->VideoInfoHeader.bmiHeader.biCompression = YUY2_FOURCC;
    f->VideoInfoHeader.bmiHeader.biSizeImage = e->FrameBytes;
    RtlZeroMemory(r, sizeof(*r));
    r->DataRange = f->DataFormat;
    r->DataRange.FormatSize = sizeof(*r);
    r->bFixedSizeSamples = TRUE;
    r->ConfigCaps.guid = videoInfo;
    r->ConfigCaps.InputSize.cx = e->Width;
    r->ConfigCaps.InputSize.cy = e->Height;
    r->ConfigCaps.MinCroppingSize = r->ConfigCaps.InputSize;
    r->ConfigCaps.MaxCroppingSize = r->ConfigCaps.InputSize;
    r->ConfigCaps.CropGranularityX = r->ConfigCaps.CropGranularityY = 1;
    r->ConfigCaps.CropAlignX = r->ConfigCaps.CropAlignY = 1;
    r->ConfigCaps.MinOutputSize = r->ConfigCaps.InputSize;
    r->ConfigCaps.MaxOutputSize = r->ConfigCaps.InputSize;
    r->ConfigCaps.OutputGranularityX = r->ConfigCaps.OutputGranularityY = 1;
    r->ConfigCaps.MinFrameInterval = r->ConfigCaps.MaxFrameInterval = e->Interval;
    r->ConfigCaps.MinBitsPerSecond = r->ConfigCaps.MaxBitsPerSecond = f->VideoInfoHeader.dwBitRate;
    r->VideoInfoHeader = f->VideoInfoHeader;
}

unsigned int UvfBuild(const UVC_DESC *d, UVFMT *t)
{
    unsigned int i;
    RtlZeroMemory(t, sizeof(*t));
    if (!d || !d->Valid) return 0;
    for (i = 0; i < d->FrameCount && t->Count < UVFMT_MAX; i++) {
        const UVC_DESC_FRAME *fr = &d->Frame[i];
        UVFMT_ENTRY *e = &t->Entry[t->Count];
        if (fr->Stream != 0 || fr->Kind != UVC_DESC_YUY2 || !fr->FrameBytes ||
            fr->FrameBytes > UVFMT_MAX_BYTES || !fr->DefaultInterval) continue;
        e->Width = fr->Width; e->Height = fr->Height; e->FrameIndex = i;
        e->FrameBytes = fr->FrameBytes; e->Interval = fr->DefaultInterval;
        Fill(e);
        t->Ranges[t->Count] = (PKSDATAFORMAT)&e->Range;
        t->Count++;
    }
    return t->Count;
}

static int Rect(const RECT *rect, unsigned int w, unsigned int h)
{
    return !rect->left && !rect->top &&
        ((!rect->right && !rect->bottom) || (rect->right == (LONG)w && rect->bottom == (LONG)h));
}

int UvfMatch(const UVFMT *t, const KSDATAFORMAT *input)
{
    const KS_DATAFORMAT_VIDEOINFOHEADER *f = (const KS_DATAFORMAT_VIDEOINFOHEADER *)input;
    unsigned int i;
    if (!t || !input || input->FormatSize < sizeof(*f) ||
        !Same(&input->MajorFormat, &typeVideo) || !Same(&input->SubFormat, &yuy2) ||
        !Same(&input->Specifier, &videoInfo)) return -1;
    for (i = 0; i < t->Count; i++) {
        const UVFMT_ENTRY *e = &t->Entry[i];
        if ((!input->SampleSize || input->SampleSize == e->FrameBytes) &&
            Rect(&f->VideoInfoHeader.rcSource, e->Width, e->Height) &&
            Rect(&f->VideoInfoHeader.rcTarget, e->Width, e->Height) &&
            f->VideoInfoHeader.bmiHeader.biSize == sizeof(KS_BITMAPINFOHEADER) &&
            f->VideoInfoHeader.bmiHeader.biWidth == (LONG)e->Width &&
            f->VideoInfoHeader.bmiHeader.biHeight == (LONG)e->Height &&
            f->VideoInfoHeader.bmiHeader.biPlanes == 1 &&
            f->VideoInfoHeader.bmiHeader.biBitCount == 16 &&
            f->VideoInfoHeader.bmiHeader.biCompression == YUY2_FOURCC &&
            f->VideoInfoHeader.bmiHeader.biSizeImage == e->FrameBytes &&
            f->VideoInfoHeader.AvgTimePerFrame == (REFERENCE_TIME)e->Interval) return (int)i;
    }
    return -1;
}

NTSTATUS UvfIntersect(const UVFMT *t, PHW_STREAM_REQUEST_BLOCK srb)
{
    PSTREAM_DATA_INTERSECT_INFO info = srb->CommandData.IntersectInfo;
    PKSDATARANGE r;
    const UVFMT_ENTRY *e = NULL;
    unsigned int i;
    if (!t || !info || info->StreamNumber || !(r = info->DataRange) ||
        r->FormatSize < sizeof(KSDATARANGE)) return STATUS_INVALID_PARAMETER;
    if (!Same(&r->MajorFormat, &typeVideo) || !Same(&r->SubFormat, &yuy2) ||
        !Same(&r->Specifier, &videoInfo)) return STATUS_NO_MATCH;
    for (i = 0; i < t->Count && !e; i++) {
        if (r->FormatSize >= sizeof(KS_DATARANGE_VIDEO)) {
            PKS_DATARANGE_VIDEO v = (PKS_DATARANGE_VIDEO)r;
            if (v->VideoInfoHeader.bmiHeader.biWidth == (LONG)t->Entry[i].Width &&
                v->VideoInfoHeader.bmiHeader.biHeight == (LONG)t->Entry[i].Height &&
                v->VideoInfoHeader.bmiHeader.biCompression == YUY2_FOURCC &&
                v->VideoInfoHeader.AvgTimePerFrame == (REFERENCE_TIME)t->Entry[i].Interval)
                e = &t->Entry[i];
        } else e = &t->Entry[i]; /* a bare range takes the first frame */
    }
    if (!e) return STATUS_NO_MATCH;
    if (info->SizeOfDataFormatBuffer == sizeof(ULONG) && info->DataFormatBuffer) {
        *(PULONG)info->DataFormatBuffer = sizeof(e->Format);
        srb->ActualBytesTransferred = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    srb->ActualBytesTransferred = sizeof(e->Format);
    if (info->SizeOfDataFormatBuffer < sizeof(e->Format)) return STATUS_BUFFER_TOO_SMALL;
    if (!info->DataFormatBuffer) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(info->DataFormatBuffer, &e->Format, sizeof(e->Format));
    return STATUS_SUCCESS;
}

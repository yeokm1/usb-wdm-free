/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original implementation; see uvcfmt.h. Field use follows the M0/M1
 * single-format metadata in uvcmini.c, repeated per camera frame. The
 * RGB24 subtype is DirectShow's MEDIASUBTYPE_RGB24 (MSVC 6.0 UUIDS.H, line
 * 193); the YUY2 to RGB conversion is the ITU-R BT.601 studio-range matrix
 * in 8.8 fixed point.
 */
#include "uvcfmt.h"

#define YUY2_FOURCC 0x32595559
static const GUID typeVideo = { STATIC_KSDATAFORMAT_TYPE_VIDEO };
static const GUID videoInfo = { STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO };
static const GUID yuy2 = { YUY2_FOURCC, 0, 0x10, {0x80,0,0,0xaa,0,0x38,0x9b,0x71} };
/* e436eb7d-524f-11ce-9f53-0020af0ba770, MEDIASUBTYPE_RGB24 */
static const GUID rgb24 = { 0xe436eb7d, 0x524f, 0x11ce, {0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70} };

static int Same(const GUID *a, const GUID *b)
{
    const ULONG *x = (const ULONG *)a, *y = (const ULONG *)b;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}
/* GUID_NULL is the KS wildcard for major format, subformat and specifier
 * (ks.h, KSDATAFORMAT_TYPE_WILDCARD and its siblings). */
static int Null(const GUID *a)
{
    const ULONG *x = (const ULONG *)a;
    return !x[0] && !x[1] && !x[2] && !x[3];
}
static int SameOrAny(const GUID *want, const GUID *have)
{ return Null(want) || Same(want, have); }

unsigned long UvfRgbStride(unsigned int width)
{ return ((unsigned long)width * 3u + 3u) & ~3ul; }

static void Fill(UVFMT_ENTRY *e)
{
    KS_DATAFORMAT_VIDEOINFOHEADER *f = &e->Format;
    KS_DATARANGE_VIDEO *r = &e->Range;
    ULONG fps = e->Interval ? (10000000 + e->Interval / 2) / e->Interval : 1;
    RtlZeroMemory(f, sizeof(*f));
    f->DataFormat.FormatSize = sizeof(*f);
    f->DataFormat.SampleSize = e->FrameBytes;
    f->DataFormat.MajorFormat = typeVideo;
    f->DataFormat.SubFormat = e->Kind == UVFMT_RGB24 ? rgb24 : yuy2;
    f->DataFormat.Specifier = videoInfo;
    f->VideoInfoHeader.rcSource.right = e->Width;
    f->VideoInfoHeader.rcSource.bottom = e->Height;
    f->VideoInfoHeader.rcTarget = f->VideoInfoHeader.rcSource;
    f->VideoInfoHeader.dwBitRate = e->FrameBytes * 8 * fps;
    f->VideoInfoHeader.AvgTimePerFrame = e->Interval;
    f->VideoInfoHeader.bmiHeader.biSize = sizeof(KS_BITMAPINFOHEADER);
    f->VideoInfoHeader.bmiHeader.biWidth = e->Width;
    f->VideoInfoHeader.bmiHeader.biHeight = e->Height; /* RGB24: bottom-up */
    f->VideoInfoHeader.bmiHeader.biPlanes = 1;
    f->VideoInfoHeader.bmiHeader.biBitCount = e->Kind == UVFMT_RGB24 ? 24 : 16;
    f->VideoInfoHeader.bmiHeader.biCompression = e->Kind == UVFMT_RGB24 ? KS_BI_RGB : YUY2_FOURCC;
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

static void Add(UVFMT *t, const UVC_DESC_FRAME *fr, unsigned int i, unsigned int kind)
{
    UVFMT_ENTRY *e = &t->Entry[t->Count];
    e->Kind = kind;
    e->Width = fr->Width; e->Height = fr->Height; e->FrameIndex = i;
    e->SourceBytes = fr->FrameBytes;
    e->FrameBytes = kind == UVFMT_RGB24 ? UvfRgbStride(fr->Width) * fr->Height : fr->FrameBytes;
    e->Interval = fr->DefaultInterval;
    Fill(e);
    t->Ranges[t->Count] = (PKSDATAFORMAT)&e->Range;
    t->Count++;
}

static int Usable(const UVC_DESC_FRAME *fr)
{
    /* YUY2 is 2 bytes a pixel in pairs: an odd width has no exact layout. */
    return fr->Stream == 0 && fr->Kind == UVC_DESC_YUY2 && fr->FrameBytes &&
        fr->FrameBytes <= UVFMT_MAX_BYTES && fr->DefaultInterval &&
        fr->Width && fr->Height && !(fr->Width & 1) &&
        fr->FrameBytes == (unsigned long)fr->Width * fr->Height * 2u;
}

unsigned int UvfBuild(const UVC_DESC *d, UVFMT *t)
{
    unsigned int i, kind, n;
    RtlZeroMemory(t, sizeof(*t));
    if (!d || !d->Valid) return 0;
    /* RGB24 first, then YUY2; the same frames for both, so neither kind
     * can take the other's room (UVFMT_MAX / 2 each). */
    for (kind = UVFMT_RGB24 + 1; kind-- > 0;)
        for (i = n = 0; i < d->FrameCount && n < UVFMT_MAX / 2; i++)
            if (Usable(&d->Frame[i])) { Add(t, &d->Frame[i], i, kind); n++; }
    return t->Count;
}

static int Rect(const RECT *rect, unsigned int w, unsigned int h)
{
    return !rect->left && !rect->top &&
        ((!rect->right && !rect->bottom) || (rect->right == (LONG)w && rect->bottom == (LONG)h));
}

static int Kind(const GUID *sub, ULONG compression, unsigned int kind)
{
    return kind == UVFMT_RGB24 ? Same(sub, &rgb24) && compression == KS_BI_RGB :
        Same(sub, &yuy2) && compression == YUY2_FOURCC;
}

int UvfMatch(const UVFMT *t, const KSDATAFORMAT *input)
{
    const KS_DATAFORMAT_VIDEOINFOHEADER *f = (const KS_DATAFORMAT_VIDEOINFOHEADER *)input;
    unsigned int i;
    if (!t || !input || input->FormatSize < sizeof(*f) ||
        !Same(&input->MajorFormat, &typeVideo) || !Same(&input->Specifier, &videoInfo)) return -1;
    for (i = 0; i < t->Count; i++) {
        const UVFMT_ENTRY *e = &t->Entry[i];
        const KS_BITMAPINFOHEADER *b = &f->VideoInfoHeader.bmiHeader;
        if (Kind(&input->SubFormat, b->biCompression, e->Kind) &&
            (!input->SampleSize || input->SampleSize == e->FrameBytes) &&
            Rect(&f->VideoInfoHeader.rcSource, e->Width, e->Height) &&
            Rect(&f->VideoInfoHeader.rcTarget, e->Width, e->Height) &&
            b->biSize == sizeof(KS_BITMAPINFOHEADER) &&
            b->biWidth == (LONG)e->Width && b->biHeight == (LONG)e->Height &&
            b->biPlanes == 1 && b->biBitCount == (e->Kind == UVFMT_RGB24 ? 24 : 16) &&
            (b->biSizeImage == e->FrameBytes || (!b->biSizeImage && e->Kind == UVFMT_RGB24)))
            return (int)i;
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
    if (!SameOrAny(&r->MajorFormat, &typeVideo) || !SameOrAny(&r->Specifier, &videoInfo))
        return STATUS_NO_MATCH;
    for (i = 0; i < t->Count && !e; i++) {
        const UVFMT_ENTRY *c = &t->Entry[i];
        if (!SameOrAny(&r->SubFormat, &c->Format.DataFormat.SubFormat)) continue;
        if (r->FormatSize >= sizeof(KS_DATARANGE_VIDEO) && Same(&r->Specifier, &videoInfo)) {
            PKS_DATARANGE_VIDEO v = (PKS_DATARANGE_VIDEO)r;
            if (v->VideoInfoHeader.bmiHeader.biWidth == (LONG)c->Width &&
                v->VideoInfoHeader.bmiHeader.biHeight == (LONG)c->Height &&
                v->VideoInfoHeader.bmiHeader.biCompression ==
                    c->Format.VideoInfoHeader.bmiHeader.biCompression)
                e = c;
        } else e = c; /* a bare or wildcard range takes the first match */
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

static unsigned char Clamp(int v)
{ return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

void UvfYuy2ToRgb24(const unsigned char *src, unsigned char *dst,
    unsigned int width, unsigned int height)
{
    unsigned long stride = UvfRgbStride(width);
    unsigned int x, y;
    for (y = 0; y < height; y++) {
        const unsigned char *s = src + (unsigned long)y * width * 2u;
        unsigned char *d = dst + (unsigned long)(height - 1 - y) * stride;
        for (x = 0; x + 1 < width; x += 2, s += 4) {
            int u = s[1] - 128, v = s[3] - 128;
            int rv = 409 * v + 128, gv = -100 * u - 208 * v + 128, bv = 516 * u + 128;
            int c0 = 298 * (s[0] - 16), c1 = 298 * (s[2] - 16);
            *d++ = Clamp((c0 + bv) >> 8); *d++ = Clamp((c0 + gv) >> 8); *d++ = Clamp((c0 + rv) >> 8);
            *d++ = Clamp((c1 + bv) >> 8); *d++ = Clamp((c1 + gv) >> 8); *d++ = Clamp((c1 + rv) >> 8);
        }
        for (x = width * 3u; x < stride; x++) *d++ = 0; /* row padding */
    }
}

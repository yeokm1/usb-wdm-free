/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original parser from UVC core and uncompressed/MJPEG payload 3.1;
 * USB 2.0 standard interface/endpoint fields. No copied implementation.
 */
#include "uvcdesc.h"
static unsigned int R16(const unsigned char *p)
{ return p[0] | ((unsigned int)p[1] << 8); }
static unsigned long R32(const unsigned char *p)
{ return (unsigned long)R16(p) | ((unsigned long)R16(p + 2) << 16); }
static int Equal(const unsigned char *a, const unsigned char *b, unsigned int n)
{ unsigned int i; for (i = 0; i < n; ++i) if (a[i] != b[i]) return 0; return 1; }

/* UVC Uncompressed1.5 Table2-1/section2.3 gives format IDs/subsampling and
 * tight wire dimensions. Published Microsoft YUV docs give NV12/I420 order;
 * Linux M420 format documentation gives alternating twoY/oneUV row groups.
 * Original arithmetic; no reference implementation copied. Audit anchors:
 * docs/testing/2026-10-21-uvc-layouts.md. */
int UvcDescLayout(unsigned int kind, unsigned int w, unsigned int h,
    UVC_DESC_LAYOUT *out)
{
    UVC_DESC_LAYOUT v = { 0 };
    unsigned long pixels;
    if (!out || !w || !h || w > 65535U || h > 65535U || (w & 1)) return 0;
    if (kind != UVC_DESC_YUY2 && kind != UVC_DESC_NV12 &&
        kind != UVC_DESC_I420 && kind != UVC_DESC_M420) return 0;
    v.Kind = kind; v.MacroWidth = 2;
    pixels = (unsigned long)w * h; /* bounded WORD product */
    if (kind == UVC_DESC_YUY2) {
        if (pixels > 0xffffffffUL / 2) return 0;
        v.BitsPerPixel = 16; v.MacroHeight = 1; v.PlaneCount = 1;
        v.FrameBytes = pixels * 2;
        v.Plane[0].Role = UVC_PLANE_YUYV; v.Plane[0].Rows = h;
        v.Plane[0].RowBytes = v.Plane[0].Stride = (unsigned long)w * 2;
    } else {
        if ((h & 1) || pixels > 0xffffffffUL - pixels / 2) return 0;
        v.BitsPerPixel = 12; v.MacroHeight = 2;
        v.FrameBytes = pixels + pixels / 2;
        v.Plane[0].Role = UVC_PLANE_Y; v.Plane[0].Rows = h;
        v.Plane[0].RowBytes = v.Plane[0].Stride = w;
        v.Plane[1].Offset = pixels; v.Plane[1].Rows = h / 2;
        if (kind == UVC_DESC_NV12) {
            v.PlaneCount = 2; v.Plane[1].Role = UVC_PLANE_UV;
            v.Plane[1].RowBytes = v.Plane[1].Stride = w;
        } else if (kind == UVC_DESC_I420) {
            v.PlaneCount = 3; v.Plane[1].Role = UVC_PLANE_U;
            v.Plane[1].RowBytes = v.Plane[1].Stride = w / 2;
            v.Plane[2] = v.Plane[1]; v.Plane[2].Role = UVC_PLANE_V;
            v.Plane[2].Offset = pixels + pixels / 4;
        } else {
            unsigned int i;
            v.PlaneCount = 3;
            for (i = 0; i < 3; ++i) {
                v.Plane[i].Role = i == 0 ? UVC_PLANE_Y_EVEN :
                    i == 1 ? UVC_PLANE_Y_ODD : UVC_PLANE_UV;
                v.Plane[i].Offset = (unsigned long)i * w;
                v.Plane[i].Rows = h / 2; v.Plane[i].RowBytes = w;
                v.Plane[i].Stride = (unsigned long)w * 3;
            }
        }
    }
    *out = v; return 1;
}
int UvcDescRow(unsigned int kind, unsigned int w, unsigned int h,
    unsigned int plane, unsigned int row, unsigned long bytes,
    unsigned long *offset, unsigned long *rowBytes)
{
    UVC_DESC_LAYOUT v;
    unsigned long o;
    if (!offset || !rowBytes || offset == rowBytes ||
        !UvcDescLayout(kind, w, h, &v) || plane >= v.PlaneCount ||
        row >= v.Plane[plane].Rows || bytes < v.FrameBytes) return 0;
    o = v.Plane[plane].Offset + row * v.Plane[plane].Stride;
    if (o > bytes || v.Plane[plane].RowBytes > bytes - o) return 0;
    *offset = o; *rowBytes = v.Plane[plane].RowBytes; return 1;
}

/* The complete standard-descriptor walk is validated before any later walk. */
static int Config(const unsigned char *b, unsigned int n, unsigned int *total)
{
    unsigned int o, p, size, actual = 0, expected = 0, have = 0, interfaces = 0;
    unsigned char seen[256] = { 0 };
    if (!b || n < 9 || b[0] != 9 || b[1] != 2) return 0;
    *total = R16(b + 2);
    if (*total < 9 || *total > n) return 0;
    for (o = 9; o < *total; o += size) {
        if (*total - o < 2 || (size = b[o]) < 2 || size > *total - o) return 0;
        if (b[o + 1] == 4) {
            if (size < 9 || (have && actual != expected)) return 0;
            /* Reject duplicate interface/alternate pairs without a large bitmap. */
            for (p = 9; p < o; p += b[p])
                if (b[p + 1] == 4 && b[p + 2] == b[o + 2] && b[p + 3] == b[o + 3]) return 0;
            if (!seen[b[o + 2]]) { seen[b[o + 2]] = 1; ++interfaces; }
            expected = b[o + 4]; actual = 0; have = 1;
        } else if (b[o + 1] == 5) {
            if (size < 7 || !have) return 0;
            ++actual;
        } else if (b[o + 1] == 11 && size < 8) return 0;
        else if (b[o + 1] == 2) return 0;
    }
    if ((have && actual != expected) || interfaces != b[4]) return 0;
    /* Every interface must have alternate zero. */
    for (o = 9; o < *total; o += b[o]) if (b[o + 1] == 4 && b[o + 3] == 0) seen[b[o + 2]] = 0;
    for (o = 0; o < 256; ++o) if (seen[o]) return 0;
    return 1;
}
static unsigned int Interface(const unsigned char *b, unsigned int n,
    unsigned int number, unsigned int alternate)
{
    unsigned int o;
    for (o = 9; o < n; o += b[o])
        if (b[o + 1] == 4 && b[o + 2] == number && b[o + 3] == alternate) return o;
    return 0;
}
static unsigned int EndInterface(const unsigned char *b, unsigned int n, unsigned int o)
{
    o += b[o];
    while (o < n && b[o + 1] != 4 && b[o + 1] != 11) o += b[o];
    return o;
}
static int Block(const unsigned char *b, unsigned int start, unsigned int size,
    unsigned int limit)
{
    unsigned int o, end;
    if (size < b[start] || size > limit - start) return 0;
    end = start + size;
    for (o = start; o < end; o += b[o])
        if (b[o + 1] != 0x24 || b[o] < 3 || b[o] > end - o) return 0;
    return o == end;
}
int UvcDescInterval(const UVC_DESC *d, unsigned int f, unsigned long interval)
{
    const UVC_DESC_FRAME *v;
    const unsigned char *p;
    unsigned int i;
    unsigned long low, high, step;
    if (!d || !d->Valid || f >= d->FrameCount || !interval) return 0;
    v = &d->Frame[f]; p = d->Bytes + v->IntervalOffset;
    if (v->IntervalCount) {
        for (i = 0; i < v->IntervalCount; ++i) if (R32(p + 4 * i) == interval) return 1;
        return 0;
    }
    low = R32(p); high = R32(p + 4); step = R32(p + 8);
    return interval >= low && interval <= high && step && (interval - low) % step == 0;
}
static int Frame(const unsigned char *b, unsigned int o, unsigned int stream,
    unsigned int format, unsigned int kind, UVC_DESC *d)
{
    UVC_DESC_FRAME v = { 0 };
    unsigned int count, size = b[o], i;
    unsigned long last = 0, val, low, high, step;
    if (size < 26 || !b[o + 3]) return UVC_DESC_BAD;
    count = b[o + 25];
    if (size != 26 + (count ? 4 * count : 12)) return UVC_DESC_BAD;
    v.Stream = stream; v.Format = format; v.Frame = b[o + 3]; v.Kind = kind;
    v.Width = R16(b + o + 5); v.Height = R16(b + o + 7);
    v.DescriptorMaxBytes = R32(b + o + 17);
    v.DefaultInterval = R32(b + o + 21); v.IntervalCount = count; v.IntervalOffset = o + 26;
    if (!v.Width || !v.Height || !v.DefaultInterval || R32(b + o + 9) > R32(b + o + 13)) return UVC_DESC_BAD;
    if (count) {
        unsigned int found = 0;
        for (i = 0; i < count; ++i) {
            val = R32(b + o + 26 + i * 4);
            if (!val || val <= last) return UVC_DESC_BAD;
            if (val == v.DefaultInterval) found = 1;
            last = val;
        }
        if (!found) return UVC_DESC_BAD;
    } else {
        low = R32(b + o + 26); high = R32(b + o + 30); step = R32(b + o + 34);
        if (!low || high < low || !step || v.DefaultInterval < low ||
            v.DefaultInterval > high || (v.DefaultInterval - low) % step) return UVC_DESC_BAD;
    }
    if (kind && kind != UVC_DESC_MJPEG) {
        UVC_DESC_LAYOUT layout;
        if (!UvcDescLayout(kind, v.Width, v.Height, &layout)) return UVC_DESC_BAD;
        v.FrameBytes = layout.FrameBytes;
    }
    if (!kind) return UVC_DESC_OK;
    if (d->FrameCount == UVC_DESC_MAX_FRAMES) return UVC_DESC_LIMIT;
    d->Frame[d->FrameCount++] = v;
    return UVC_DESC_OK;
}
static int Stream(const unsigned char *b, unsigned int n, unsigned int s,
    unsigned int vcStart, unsigned int vcEnd, UVC_DESC *d)
{
    static const unsigned char guid[4][16] = {
        {0x59,0x55,0x59,0x32,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71},
        {0x4e,0x56,0x31,0x32,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71},
        {0x49,0x34,0x32,0x30,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71},
        {0x4d,0x34,0x32,0x30,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    static const unsigned int kinds[4] = {UVC_DESC_YUY2,UVC_DESC_NV12,UVC_DESC_I420,UVC_DESC_M420};
    unsigned int start, end, h, o, csEnd, formats = 0, format = 0, subtype = 0;
    unsigned int kind = 0, expected = 0, frames = 0, def = 0, haveDefault = 0, ret;
    unsigned int terminal = 0, i;
    unsigned char formatSeen[256] = { 0 }, frameSeen[256] = { 0 };
    UVC_DESC_STREAM *v = &d->Stream[s];
    start = Interface(b, n, v->Interface, 0);
    if (!start || b[start + 5] != 14 || b[start + 6] != 2) return UVC_DESC_BAD;
    end = EndInterface(b, n, start); h = start + b[start];
    if (h >= end || b[h + 1] != 0x24 || b[h] < 13 || b[h + 2] != 1 ||
        !b[h + 3] || !b[h + 12] || b[h] != 13 + b[h + 3] * b[h + 12]) return UVC_DESC_BAD;
    if (!Block(b, h, R16(b + h + 4), end)) return UVC_DESC_BAD;
    csEnd = h + R16(b + h + 4);
    v->Endpoint = b[h + 6]; v->Terminal = b[h + 8]; v->StillMethod = b[h + 9];
    if (!(v->Endpoint & 0x80) || (v->Endpoint & 0x70) || !(v->Endpoint & 15) || v->StillMethod > 3) return UVC_DESC_BAD;
    for (o = vcStart; o < vcEnd; o += b[o])
        if (b[o + 2] == 3 && b[o] >= 9 && b[o + 3] == v->Terminal && R16(b + o + 4) == 0x101) ++terminal;
    if (terminal != 1) return UVC_DESC_BAD;
    for (o = h + b[h]; o < csEnd; o += b[o]) {
        unsigned int t = b[o + 2];
        /* Format subtypes (UVC 1.5 Table A-6): uncompressed 4 and MJPEG 6 are
         * parsed; MPEG-2 TS 0x0A, DV 0x0C, frame-based 0x10, stream-based 0x12,
         * H.264 0x13, H.264 simulcast 0x15, VP8 0x16 and VP8 simulcast 0x18
         * count toward bNumFormats and are skipped with their frames. */
        if (t == 4 || t == 6 || t == 10 || t == 12 || t == 16 || t == 18 ||
            t == 0x13 || t == 0x15 || t == 0x16 || t == 0x18) {
            if (format && (frames != expected || !haveDefault)) return UVC_DESC_BAD;
            if ((t == 4 && b[o] < 27) || (t == 6 && b[o] < 11) || b[o] < 5) return UVC_DESC_BAD;
            format = b[o + 3]; expected = b[o + 4]; frames = 0; haveDefault = 0; subtype = t; kind = 0;
            if (!format || format > b[h + 3] || formatSeen[format]) return UVC_DESC_BAD;
            formatSeen[format] = 1; ++formats;
            for (i = 0; i < 256; ++i) frameSeen[i] = 0;
            if (t == 4 || t == 6) {
                if (!expected) return UVC_DESC_BAD;
                def = b[o + (t == 4 ? 22 : 6)];
                if (t == 6) kind = UVC_DESC_MJPEG;
                else for (i = 0; i < 4; ++i) if (Equal(b + o + 5, guid[i], 16)) {
                    if (b[o + 21] != (i ? 12 : 16)) return UVC_DESC_BAD;
                    kind = kinds[i]; break;
                }
            } else { expected = 0; def = 0; haveDefault = 1; }
        } else if (t == 5 || t == 7) {
            if ((t == 5 && subtype != 4) || (t == 7 && subtype != 6) || b[o] < 4) return UVC_DESC_BAD;
            if (b[o + 3] > expected || frameSeen[b[o + 3]]) return UVC_DESC_BAD;
            frameSeen[b[o + 3]] = 1; ++frames;
            if (b[o + 3] == def) haveDefault = 1;
            ret = Frame(b, o, s, format, kind, d); if (ret) return ret;
        } else if (t == 1 || t == 2) return UVC_DESC_BAD;
    }
    if (!format || frames != expected || !haveDefault || formats != b[h + 3]) return UVC_DESC_BAD;
    /* No class-specific descriptors may escape the header's declared block. */
    for (o = csEnd; o < end; o += b[o]) if (b[o + 1] == 0x24) return UVC_DESC_BAD;
    return UVC_DESC_OK;
}
static int Alts(const unsigned char *b, unsigned int n, unsigned int s, UVC_DESC *d)
{
    unsigned int o, p, end, count = 0, seen, type = 0;
    UVC_DESC_ALT v;
    for (o = 9; o < n; o += b[o]) if (b[o + 1] == 4 && b[o + 2] == d->Stream[s].Interface) {
        if (b[o + 5] != 14 || b[o + 6] != 2) return UVC_DESC_BAD;
        end = EndInterface(b, n, o); seen = 0;
        for (p = o + b[o]; p < end; p += b[p]) if (b[p + 1] == 5 && b[p + 2] == d->Stream[s].Endpoint) {
            unsigned int max = R16(b + p + 4);
            if (seen++) return UVC_DESC_BAD;
            v.Stream = s; v.Alternate = b[o + 3]; v.Endpoint = b[p + 2]; v.Type = b[p + 3] & 3;
            v.PacketBytes = max & 2047; v.Transactions = 1 + ((max >> 11) & 3); v.Interval = b[p + 6];
            if ((max & 0xe000) || !v.PacketBytes || v.Transactions > 3 || (v.Type != 1 && v.Type != 2)) return UVC_DESC_BAD;
            if (v.Type == 2) {
                if (v.Alternate || v.Transactions != 1 ||
                    (d->Speed == UVC_DESC_HIGH_SPEED ? v.PacketBytes != 512 :
                    (v.PacketBytes != 8 && v.PacketBytes != 16 && v.PacketBytes != 32 && v.PacketBytes != 64))) return UVC_DESC_BAD;
                v.ServiceMicroframes = 0;
            } else {
                if (!v.Interval || v.Interval > 16 || !v.Alternate || v.PacketBytes > (d->Speed == UVC_DESC_HIGH_SPEED ? 1024U : 1023U) ||
                    (d->Speed == UVC_DESC_FULL_SPEED && v.Transactions != 1)) return UVC_DESC_BAD;
                /* USB 2.0 Table 9-14 forbids undersized high-bandwidth packets. */
                if ((v.Transactions == 2 && v.PacketBytes < 513) ||
                    (v.Transactions == 3 && v.PacketBytes < 683)) return UVC_DESC_BAD;
                v.ServiceMicroframes = (1U << (v.Interval - 1)) * (d->Speed == UVC_DESC_FULL_SPEED ? 8U : 1U);
            }
            v.Capacity = v.PacketBytes * v.Transactions;
            if (type && type != v.Type) return UVC_DESC_BAD;
            type = v.Type;
            if (d->AltCount == UVC_DESC_MAX_ALTS) return UVC_DESC_LIMIT;
            d->Alt[d->AltCount++] = v; ++count;
        }
    }
    return count ? UVC_DESC_OK : UVC_DESC_BAD;
}
int UvcDescParse(const unsigned char *b, unsigned int length,
    unsigned int control, unsigned int speed, UVC_DESC *d)
{
    unsigned int n, start, end, h, vcEnd, o, i, j, ret, iadFirst = 0, iadCount = 0;
    if (!d) return UVC_DESC_BAD;
    /* Avoid a model-sized temporary on a future kernel caller's stack. */
    for (i = 0; i < sizeof(*d); ++i) ((unsigned char *)d)[i] = 0;
    if (control > 255 || (speed != 1 && speed != 2)) return UVC_DESC_UNSUPPORTED;
    if (!Config(b, length, &n)) return UVC_DESC_BAD;
    start = Interface(b, n, control, 0);
    if (!start || b[start + 5] != 14 || b[start + 6] != 1) return UVC_DESC_BAD;
    for (o = 9; o < n; o += b[o]) if (b[o + 1] == 11) {
        unsigned int first = b[o + 2], count = b[o + 3];
        if (!count || first + count > 256) return UVC_DESC_BAD;
        if (control >= first && control - first < count) {
            if (iadCount || b[o + 4] != 14 || b[o + 5] != 3 || control != first) return UVC_DESC_BAD;
            iadFirst = first; iadCount = count;
        }
    }
    end = EndInterface(b, n, start); h = start + b[start];
    if (h >= end || b[h + 1] != 0x24 || b[h] < 12 || b[h + 2] != 1 || b[h] != 12 + b[h + 11]) return UVC_DESC_BAD;
    d->Revision = R16(b + h + 3);
    if (!UvcCtrlSize(d->Revision)) return UVC_DESC_UNSUPPORTED;
    if (!Block(b, h, R16(b + h + 5), end) || !b[h + 11]) return UVC_DESC_BAD;
    vcEnd = h + R16(b + h + 5);
    for (o = vcEnd; o < end; o += b[o]) if (b[o + 1] == 0x24) return UVC_DESC_BAD;
    d->StreamCount = b[h + 11];
    if (d->StreamCount > UVC_DESC_MAX_STREAMS) return UVC_DESC_LIMIT;
    d->Bytes = b; d->Length = n; d->Control = control; d->Speed = speed;
    for (i = 0; i < d->StreamCount; ++i) {
        unsigned int number = b[h + 12 + i];
        if (number == control || (iadCount && (number < iadFirst || number - iadFirst >= iadCount))) return UVC_DESC_BAD;
        /* A collection reference cannot steal a stream assigned to another IAD. */
        for (o = 9; o < n; o += b[o]) if (b[o + 1] == 11 && number >= b[o + 2] &&
            number - b[o + 2] < b[o + 3] &&
            (!iadCount || b[o + 2] != iadFirst || b[o + 3] != iadCount)) return UVC_DESC_BAD;
        /* Also reject collection aliasing on older configurations without IADs. */
        for (o = 9; o < n; o += b[o]) if (b[o + 1] == 4 && b[o + 2] != control &&
            b[o + 3] == 0 && b[o + 5] == 14 && b[o + 6] == 1) {
            unsigned int q = o + b[o], k;
            if (q < EndInterface(b, n, o) && b[q + 1] == 0x24 && b[q] >= 12 &&
                b[q + 2] == 1 && b[q] == 12 + b[q + 11])
                for (k = 0; k < b[q + 11]; ++k) if (b[q + 12 + k] == number) return UVC_DESC_BAD;
        }
        for (j = 0; j < i; ++j) if (d->Stream[j].Interface == number) return UVC_DESC_BAD;
        d->Stream[i].Interface = number;
        ret = Stream(b, n, i, h, vcEnd, d); if (ret) return ret;
        ret = Alts(b, n, i, d); if (ret) return ret;
    }
    if (!d->FrameCount) return UVC_DESC_UNSUPPORTED;
    d->Valid = 1; return UVC_DESC_OK;
}
int UvcDescSelect(const UVC_DESC *d, unsigned int s, const UVC_CTRL *c,
    unsigned int *frame, unsigned int *alt)
{
    unsigned int i, f = UVC_DESC_MAX_FRAMES, a = UVC_DESC_MAX_ALTS, capacity = 0xffffffffU;
    if (!d || !d->Valid || !c || !frame || !alt || s >= d->StreamCount || !c->FrameBytes || c->PayloadBytes < 2) return 0;
    for (i = 0; i < d->FrameCount; ++i)
        if (d->Frame[i].Stream == s && d->Frame[i].Format == c->Format && d->Frame[i].Frame == c->Frame) { f = i; break; }
    if (f == UVC_DESC_MAX_FRAMES || !UvcDescInterval(d, f, c->Interval) ||
        (d->Frame[f].Kind != UVC_DESC_MJPEG && c->FrameBytes != d->Frame[f].FrameBytes)) return 0;
    for (i = 0; i < d->AltCount; ++i) if (d->Alt[i].Stream == s &&
        (d->Alt[i].Type == 2 || d->Alt[i].Capacity >= c->PayloadBytes) && d->Alt[i].Capacity < capacity) {
        capacity = d->Alt[i].Capacity; a = i;
    }
    if (a == UVC_DESC_MAX_ALTS) return 0;
    *frame = f; *alt = a; return 1;
}

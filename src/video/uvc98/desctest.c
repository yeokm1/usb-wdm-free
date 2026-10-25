/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Explicitly synthetic fixtures, not descriptors captured from a camera.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "uvcdesc.h"
static unsigned long checks;
static void Check(int value, unsigned int line, const char *expression)
{ ++checks; if (!value) { printf("FAIL line %u: %s\n", line, expression); exit(1); } }
#define CHECK(x) Check((x), __LINE__, #x)
static unsigned char fixture[8192], changed[8192];
static unsigned int used, vc, vs, format1, frame1, format2, frame2, ep, iad;
static UVC_DESC model;
static unsigned int Add(const unsigned char *p, unsigned int n)
{ unsigned int o = used; memcpy(fixture + used, p, n); used += n; return o; }
static void W16(unsigned char *p, unsigned int v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void W32(unsigned char *p, unsigned long v)
{ W16(p, (unsigned int)(v & 65535UL)); W16(p + 2, (unsigned int)(v >> 16)); }
static void Fixture(void)
{
    static const unsigned char config[] = {9,2,0,0,3,1,0,0x80,50};
    static const unsigned char association[] = {8,11,0,2,14,3,0,0};
    static const unsigned char control[] = {9,4,0,0,0,14,1,0,0};
    static const unsigned char header[] = {13,0x24,1,0x10,1,22,0,0,0,0,0,1,1};
    static const unsigned char terminal[] = {9,0x24,3,2,1,1,0,1,0};
    static const unsigned char stream[] = {9,4,1,0,0,14,2,0,0};
    static const unsigned char input[] = {15,0x24,1,2,0,0,0x81,0,2,0,0,0,1,0,0};
    static const unsigned char yuy2[] = {27,0x24,4,1,1,0x59,0x55,0x59,0x32,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71,16,1,0,0,0,0};
    static const unsigned char mjpeg[] = {11,0x24,6,2,1,0,1,0,0,0,0};
    static const unsigned char alternate[] = {9,4,1,1,1,14,2,0,0};
    static const unsigned char endpoint[] = {7,5,0x81,1,0,0x14,1}; /* 1024 x3 */
    static const unsigned char foreign[] = {9,4,2,0,1,3,0,0,0,7,5,0x82,3,8,0,10};
    unsigned char frame[38] = { 0 };
    used = 0; Add(config, sizeof(config)); iad = Add(association, sizeof(association));
    Add(control, sizeof(control)); vc = Add(header, sizeof(header)); Add(terminal, sizeof(terminal));
    Add(stream, sizeof(stream)); vs = Add(input, sizeof(input)); format1 = Add(yuy2, sizeof(yuy2));
    frame[0] = 34; frame[1] = 0x24; frame[2] = 5; frame[3] = 1;
    W16(frame + 5, 640); W16(frame + 7, 480); W32(frame + 9, 73728000UL);
    W32(frame + 13, 147456000UL); W32(frame + 17, 614400UL);
    W32(frame + 21, 333333UL); frame[25] = 2;
    W32(frame + 26, 333333UL); W32(frame + 30, 666666UL);
    frame1 = Add(frame, 34); format2 = Add(mjpeg, sizeof(mjpeg));
    frame[0] = 38; frame[2] = 7; frame[25] = 0;
    W32(frame + 26, 333333UL); W32(frame + 30, 999999UL); W32(frame + 34, 333333UL);
    frame2 = Add(frame, 38); W16(fixture + vs + 4, used - vs);
    Add(alternate, sizeof(alternate)); ep = Add(endpoint, sizeof(endpoint));
    Add(foreign, sizeof(foreign)); W16(fixture + 2, used);
}
static void Bad(unsigned int o, unsigned int value)
{
    memcpy(changed, fixture, used); changed[o] = (unsigned char)value;
    CHECK(UvcDescParse(changed, used, 0, 2, &model) != UVC_DESC_OK);
    CHECK(!model.Valid);
}
static void Codec(unsigned char *guardEnd)
{
    static const unsigned int rev[] = {0x100,0x110,0x150};
    unsigned char in[48], out[50];
    UVC_CTRL c;
    unsigned int r, n, i, k;
    CHECK(sizeof(unsigned long) == 4);
    for (r = 0; r < 3; ++r) {
        n = UvcCtrlSize(rev[r]); CHECK(n == (r == 0 ? 26U : r == 1 ? 34U : 48U));
        for (k = 0; k < 256; ++k) {
            for (i = 0; i < n; ++i) in[i] = (unsigned char)(k + i * 17);
            memcpy(guardEnd - n, in, n);
            CHECK(UvcCtrlDecode(rev[r], guardEnd - n, n, &c));
            memset(out, 0xcc, sizeof(out));
            CHECK(UvcCtrlEncode(rev[r], &c, out + 1, n));
            CHECK(out[0] == 0xcc && out[n + 1] == 0xcc && !memcmp(in, out + 1, n));
        }
        for (i = 0; i < n; ++i) CHECK(!UvcCtrlDecode(rev[r], guardEnd - i, i, &c));
        CHECK(!UvcCtrlDecode(rev[r], in, n + 1, &c));
        CHECK(UvcCtrlDecode(rev[r], in, n, &c)); c.Format = 256;
        memset(out, 0xaa, sizeof(out)); CHECK(!UvcCtrlEncode(rev[r], &c, out, n));
        CHECK(out[0] == 0xaa && out[n - 1] == 0xaa);
    }
    CHECK(!UvcCtrlSize(0x120)); CHECK(!UvcCtrlSize(0x200));
    CHECK(!UvcCtrlDecode(0x110, NULL, 34, &c)); CHECK(!UvcCtrlDecode(0x110, in, 34, NULL));
    /* Independent byte offsets, not merely encode/decode round trips. */
    memset(in, 0, sizeof(in));
    in[0] = 0x34; in[1] = 0x12; in[2] = 7; in[3] = 8;
    in[4] = 0x15; in[5] = 0x16; in[6] = 0x05;
    in[18] = 0x00; in[19] = 0x60; in[20] = 0x09;
    in[22] = 0x00; in[23] = 0x0c;
    in[26] = 0x78; in[27] = 0x56; in[28] = 0x34; in[29] = 0x12;
    in[30] = 3; in[31] = 4; in[32] = 2; in[33] = 5; in[47] = 0xa5;
    CHECK(UvcCtrlDecode(0x150, in, 48, &c));
    CHECK(c.Hint == 0x1234 && c.Format == 7 && c.Frame == 8 && c.Interval == 333333UL);
    CHECK(c.FrameBytes == 614400UL && c.PayloadBytes == 3072 && c.Clock == 0x12345678UL);
    CHECK(c.Framing == 3 && c.Preferred == 4 && c.Minimum == 2 && c.Maximum == 5 && c.Tail[13] == 0xa5);
    {
        static const unsigned char expected[14] = {
            0x12,10,0x5a,4,0x43,0x65,0x23,0x01,0x67,0x45,0xab,0x89,0xef,0xcd
        };
        UVC_CTRL15 fields, decoded, bad, empty;
        UVC_CTRL saved;
        memset(&fields,0,sizeof(fields)); memset(&empty,0,sizeof(empty));
        CHECK(UvcCtrl15Get(0x150,&c,&decoded) && decoded.Layout[3] == 0xa500);
        fields.Usage=0x12; fields.BitDepthLuma=10; fields.Settings=0x5a;
        fields.MaxReferencesPlus1=4; fields.RateControlModes=0x6543;
        fields.Layout[0]=0x0123; fields.Layout[1]=0x4567;
        fields.Layout[2]=0x89ab; fields.Layout[3]=0xcdef;
        CHECK(UvcCtrl15Set(0x150,&c,&fields));
        CHECK(!memcmp(c.Tail,expected,sizeof(expected)));
        CHECK(UvcCtrlEncode(0x150,&c,out+1,48));
        CHECK(!memcmp(out+1+34,expected,sizeof(expected)));
        CHECK(UvcCtrl15Get(0x150,&c,&decoded) && !memcmp(&fields,&decoded,sizeof(fields)));
        saved=c;
        for(i=0;i<9;i++) {
            bad=fields;
            if(i==0) bad.Usage=256;
            if(i==1) bad.BitDepthLuma=256;
            if(i==2) bad.Settings=256;
            if(i==3) bad.MaxReferencesPlus1=256;
            if(i==4) bad.RateControlModes=65536;
            if(i>=5) bad.Layout[i-5]=65536;
            CHECK(!UvcCtrl15Set(0x150,&c,&bad));
            CHECK(!memcmp(&c,&saved,sizeof(c)));
        }
        CHECK(!UvcCtrl15Set(0x110,&c,&fields) && !memcmp(&c,&saved,sizeof(c)));
        memset(&decoded,0xcc,sizeof(decoded));
        CHECK(!UvcCtrl15Get(0x100,&c,&decoded) && !memcmp(&decoded,&empty,sizeof(empty)));
        CHECK(!UvcCtrl15Get(0x150,NULL,&decoded));
        CHECK(!UvcCtrl15Get(0x150,&c,NULL));
        CHECK(!UvcCtrl15Set(0x150,NULL,&fields));
        CHECK(!UvcCtrl15Set(0x150,&c,NULL));
    }
}
static void Limits(void)
{
    unsigned char fmt[27], frm[30], alternate[16];
    unsigned int i;
    Fixture(); memcpy(changed, fixture, used);
    memmove(changed + vc + 21, changed + vc + 13, used - vc - 13);
    changed[vc] = 21; changed[vc + 11] = 9; W16(changed + vc + 5, 30);
    for (i = 0; i < 9; ++i) changed[vc + 12 + i] = (unsigned char)(i + 1);
    W16(changed + 2, used + 8);
    CHECK(UvcDescParse(changed, used + 8, 0, 2, &model) == UVC_DESC_LIMIT && !model.Valid);
    /* The 129th otherwise valid supported frame exceeds fixed storage. */
    Fixture(); memcpy(fmt, fixture + format1, 27); memcpy(frm, fixture + frame1, 30);
    memcpy(alternate, fixture + ep - 9, 16);
    fixture[4] = 2; fixture[vs] = 14; fixture[vs + 3] = 1;
    used = vs + 14; fmt[4] = 129; Add(fmt, 27); frm[0] = 30; frm[25] = 1;
    for (i = 1; i <= 129; ++i) { frm[3] = (unsigned char)i; Add(frm, 30); }
    W16(fixture + vs + 4, used - vs); Add(alternate, 16); W16(fixture + 2, used);
    CHECK(UvcDescParse(fixture, used, 0, 2, &model) == UVC_DESC_LIMIT && !model.Valid);
    /* Distinct standard alternate pairs; reject model exhaustion, never wrap. */
    Fixture(); memcpy(alternate, fixture + ep - 9, 16); used = ep - 9; fixture[4] = 2;
    for (i = 1; i <= 129; ++i) { alternate[3] = (unsigned char)i; Add(alternate, 16); }
    W16(fixture + 2, used);
    CHECK(UvcDescParse(fixture, used, 0, 2, &model) == UVC_DESC_LIMIT && !model.Valid);
}
/* UVC 1.5 shapes. The MJPEG format and its frame are replaced by each
 * standard format the parser skips, with one frame of that format's own
 * frame subtype where it has one (UVC 1.5 Table A-6; descriptor lengths
 * from the 1.5 payload documents). The YUY2 frame stays; the skipped
 * format counts toward bNumFormats. A reserved format subtype is refused.
 * Then a 1.5 VC graph: bcdUVC 0x0150 with a 13-byte encoding unit. */
static unsigned int Replace(const unsigned char *fmt, unsigned int fmtSize,
    const unsigned char *frm, unsigned int frmSize)
{
    unsigned int n = format2, tail = used - (frame2 + 38);
    memcpy(changed, fixture, format2);
    memcpy(changed + n, fmt, fmtSize); n += fmtSize;
    if (frmSize) { memcpy(changed + n, frm, frmSize); n += frmSize; }
    memcpy(changed + n, fixture + frame2 + 38, tail);
    W16(changed + vs + 4, n - vs); n += tail; W16(changed + 2, n);
    return n;
}
static void Uvc15(void)
{
    /* format subtype, format length, frame subtype, frame length, and the
     * frame's offsets of wWidth, dwDefaultFrameInterval, the interval
     * count and the first interval (0: no frame). Layouts: Frame Based
     * payload 1.5 Tables 3-1/3-2 (26 + 4n, dwBytesPerLine at 22), H.264
     * payload 1.5 (44 + 4n), VP8 payload 1.5 (31 + 4n), MPEG-2 TS 1.5
     * Table 3-1 (23 bytes with guidStrideFormat), DV (9), stream-based (24). */
    static const unsigned char kinds[][8] = {
        {0x0a, 23, 0, 0, 0, 0, 0, 0}, {0x0c, 9, 0, 0, 0, 0, 0, 0},
        {0x10, 28, 0x11, 30, 5, 17, 21, 26}, {0x12, 24, 0, 0, 0, 0, 0, 0},
        {0x13, 52, 0x14, 48, 4, 39, 43, 44}, {0x15, 52, 0x14, 48, 4, 39, 43, 44},
        {0x16, 13, 0x17, 35, 4, 26, 30, 31}, {0x18, 13, 0x17, 35, 4, 26, 30, 31}};
    static const unsigned char reserved[] = {0x0e, 0x0f, 0x19, 0x20};
    unsigned char fmt[64], frm[64];
    unsigned int i, n;
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        Fixture(); W16(fixture + vc + 3, 0x150);
        memset(fmt, 0, sizeof(fmt)); memset(frm, 0, sizeof(frm));
        fmt[0] = kinds[i][1]; fmt[1] = 0x24; fmt[2] = kinds[i][0]; fmt[3] = 2;
        if (kinds[i][2]) {
            fmt[4] = 1;                          /* bNumFrameDescriptors */
            if (kinds[i][0] == 0x10) fmt[22] = 1; /* bDefaultFrameIndex */
            else fmt[5] = 1;                     /* H.264/VP8 bDefaultFrameIndex */
            frm[0] = kinds[i][3]; frm[1] = 0x24; frm[2] = kinds[i][2]; frm[3] = 1;
            W16(frm + kinds[i][4], 640); W16(frm + kinds[i][4] + 2, 480);
            W32(frm + kinds[i][5], 333333UL); frm[kinds[i][6]] = 1;
            W32(frm + kinds[i][7], 333333UL);
        } else if (kinds[i][0] == 0x0a) {
            fmt[4] = 0; fmt[5] = 188; fmt[6] = 188; /* bDataOffset, bPacketLength, bStrideLength (MPEG-2 TS 1.5 Table 3-2) */
        } else if (kinds[i][0] == 0x0c) {
            W32(fmt + 4, 144000UL);              /* dwMaxVideoFrameBufferSize, SD 525-60 */
        } else {
            W32(fmt + 20, 1024UL);               /* stream-based dwPacketLength */
        }
        n = Replace(fmt, fmt[0], frm, frm[0]);
        CHECK(UvcDescParse(changed, n, 0, 2, &model) == UVC_DESC_OK);
        CHECK(model.Valid && model.Revision == 0x150 && model.FrameCount == 1);
        CHECK(model.Frame[0].Kind == UVC_DESC_YUY2 && model.Frame[0].Format == 1);
    }
    for (i = 0; i < sizeof(reserved); ++i) {
        Fixture();
        memset(fmt, 0, sizeof(fmt)); fmt[0] = 11; fmt[1] = 0x24; fmt[2] = reserved[i]; fmt[3] = 2;
        n = Replace(fmt, 11, frm, 0);
        CHECK(UvcDescParse(changed, n, 0, 2, &model) != UVC_DESC_OK && !model.Valid);
    }
    /* Encoding unit (UVC 1.5 Table 3-9) after the output terminal. */
    {
        static const unsigned char eu[13] = {13,0x24,7,4,1,0,3,0,0,0,0,0,0};
        unsigned int at = vc + 13 + 9;
        Fixture(); W16(fixture + vc + 3, 0x150);
        memcpy(changed, fixture, at); memcpy(changed + at, eu, 13);
        memcpy(changed + at + 13, fixture + at, used - at);
        W16(changed + vc + 5, 22 + 13); W16(changed + 2, used + 13);
        CHECK(UvcDescParse(changed, used + 13, 0, 2, &model) == UVC_DESC_OK && model.FrameCount == 2);
    }
}
static void Layouts(unsigned char *end)
{
    static const unsigned int kind[] = {UVC_DESC_YUY2,UVC_DESC_NV12,UVC_DESC_I420,UVC_DESC_M420};
    static const unsigned char fourcc[4][4] = {{'Y','U','Y','2'},{'N','V','1','2'},{'I','4','2','0'},{'M','4','2','0'}};
    /* Independent 4x4 row offsets and sizes, in row-sequence order. */
    static const unsigned long offsets[4][8] = {
        {0,8,16,24,0,0,0,0}, {0,4,8,12,16,20,0,0},
        {0,4,8,12,16,18,20,22}, {0,12,4,16,8,20,0,0}};
    static const unsigned int roles[4][3] = {
        {UVC_PLANE_YUYV,0,0},{UVC_PLANE_Y,UVC_PLANE_UV,0},
        {UVC_PLANE_Y,UVC_PLANE_U,UVC_PLANE_V},
        {UVC_PLANE_Y_EVEN,UVC_PLANE_Y_ODD,UVC_PLANE_UV}};
    UVC_DESC_LAYOUT v, saved, *guard;
    unsigned int k,p,row,i,n,f,a;
    unsigned long o,z,expectedSize;
    unsigned char covered[32];
    UVC_CTRL c = {0};
    guard = (UVC_DESC_LAYOUT *)(end - sizeof(UVC_DESC_LAYOUT));
    for (k=0;k<4;++k) {
        CHECK(UvcDescLayout(kind[k],4,4,guard)); v=*guard;
        CHECK(v.FrameBytes == (k ? 24UL : 32UL));
        CHECK(v.BitsPerPixel == (k ? 12U : 16U));
        CHECK(v.MacroWidth==2 && v.MacroHeight==(k ? 2U : 1U));
        memset(covered,0,sizeof(covered)); n=0;
        for(p=0;p<v.PlaneCount;++p) {
            CHECK(v.Plane[p].Role==roles[k][p]);
            for(row=0;row<v.Plane[p].Rows;++row) {
                CHECK(UvcDescRow(kind[k],4,4,p,row,v.FrameBytes,&o,&z));
                expectedSize = k==0 ? 8UL : k==2 && p ? 2UL : 4UL;
                CHECK(o==offsets[k][n++] && z==expectedSize);
                for(i=0;i<z;++i) { CHECK(o+i < v.FrameBytes); CHECK(!covered[o+i]); covered[o+i]=1; }
            }
            o=123; z=456;
            CHECK(!UvcDescRow(kind[k],4,4,p,row,v.FrameBytes,&o,&z)); CHECK(o==123 && z==456);
        }
        for(i=0;i<v.FrameBytes;++i) CHECK(covered[i]);
        CHECK(!UvcDescRow(kind[k],4,4,v.PlaneCount,0,v.FrameBytes,&o,&z));
        CHECK(!UvcDescRow(kind[k],4,4,0,0,v.FrameBytes-1,&o,&z));
        CHECK(!UvcDescRow(kind[k],4,4,0,0,v.FrameBytes,&o,&o));
        saved=v;
        CHECK(!UvcDescLayout(kind[k],3,4,&v) && !memcmp(&v,&saved,sizeof(v)));
        CHECK(!UvcDescLayout(kind[k],0,4,&v)); CHECK(!UvcDescLayout(kind[k],4,0,&v));
        CHECK(!UvcDescLayout(kind[k],65536,4,&v)); CHECK(!UvcDescLayout(kind[k],4,65536,&v));
        CHECK(!UvcDescLayout(kind[k],65534,65534,&v));
        CHECK(UvcDescLayout(kind[k],2,2,&v) && v.FrameBytes==(k?6UL:8UL));
        CHECK(UvcDescLayout(kind[k],4,3,&v)==(k==0));
        if(k) {
            CHECK(UvcDescLayout(kind[k],65534,43690,&v));
            CHECK(v.FrameBytes==4294770690UL);
            CHECK(UvcDescLayout(kind[k],65534,43692,&v) && v.FrameBytes==4294967292UL);
            p=v.PlaneCount-1; row=v.Plane[p].Rows-1;
            CHECK(UvcDescRow(kind[k],65534,43692,p,row,v.FrameBytes,&o,&z));
            CHECK(o+z==4294967292UL);
            CHECK(!UvcDescLayout(kind[k],65534,43694,&v));
        }
        Fixture(); memcpy(fixture+format1+5,fourcc[k],4); fixture[format1+21]=(unsigned char)(k?12:16);
        W16(fixture+vc+3,0x150);
        CHECK(UvcDescParse(fixture,used,0,2,&model)==0 && model.FrameCount==2);
        CHECK(model.Frame[0].Kind==kind[k] && model.Frame[0].FrameBytes==(k?460800UL:614400UL));
        c.Format=1; c.Frame=1; c.Interval=333333; c.PayloadBytes=3072; c.FrameBytes=model.Frame[0].FrameBytes;
        CHECK(UvcDescSelect(&model,0,&c,&f,&a) && f==0);
        c.FrameBytes++; CHECK(!UvcDescSelect(&model,0,&c,&f,&a));
        Bad(format1+21,k?16:12);
        Bad(frame1+5,0x81); /* odd width */
        if(k) Bad(frame1+7,0xe1); /* odd height */
        /* Exact complete GUID: same FOURCC with each tail byte damaged is unknown. */
        for(i=4;i<16;++i) {
            memcpy(changed,fixture,used); changed[format1+5+i]^=1;
            CHECK(UvcDescParse(changed,used,0,2,&model)==0 && model.FrameCount==1 && model.Frame[0].Kind==UVC_DESC_MJPEG);
        }
        for(i=0;i<used;++i) {
            memcpy(end-i,fixture,i);
            CHECK(UvcDescParse(end-i,i,0,2,&model)!=0 && !model.Valid);
        }
        memcpy(end-used,fixture,used);
        CHECK(UvcDescParse(end-used,used,0,2,&model)==0);
    }
    CHECK(!UvcDescLayout(UVC_DESC_MJPEG,4,4,&v));
    CHECK(!UvcDescLayout(99,4,4,&v)); CHECK(!UvcDescLayout(UVC_DESC_NV12,4,4,NULL));
    CHECK(!UvcDescRow(UVC_DESC_NV12,4,4,0,0,24,NULL,&z));
    CHECK(!UvcDescRow(UVC_DESC_NV12,4,4,0,0,24,&o,NULL));
}
int main(void)
{
    SYSTEM_INFO info;
    unsigned char *pages, *end;
    DWORD old;
    unsigned int i, j, f, a, oldsize;
    UVC_CTRL c = { 0 };
    GetSystemInfo(&info);
    pages = (unsigned char *)VirtualAlloc(NULL, info.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(pages != NULL); CHECK(VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old));
    end = pages + info.dwPageSize;
    Codec(end); Fixture(); CHECK(used < info.dwPageSize);
    CHECK(UvcDescParse(fixture, used, 0, 2, &model) == 0);
    CHECK(model.Valid && model.Length == used && model.StreamCount == 1 && model.FrameCount == 2 && model.AltCount == 1);
    CHECK(model.Frame[0].Kind == UVC_DESC_YUY2 && model.Frame[0].FrameBytes == 614400UL);
    CHECK(model.Frame[1].Kind == UVC_DESC_MJPEG && model.Frame[1].IntervalCount == 0);
    CHECK(model.Alt[0].Capacity == 3072 && model.Alt[0].Transactions == 3 && model.Alt[0].ServiceMicroframes == 1);
    CHECK(UvcDescInterval(&model, 0, 333333) && UvcDescInterval(&model, 0, 666666));
    CHECK(!UvcDescInterval(&model, 0, 444444) && !UvcDescInterval(&model, 1, 444444));
    CHECK(UvcDescInterval(&model, 1, 999999) && !UvcDescInterval(&model, 1, 1333332));
    c.Format = 1; c.Frame = 1; c.Interval = 333333; c.FrameBytes = 614400; c.PayloadBytes = 3072;
    CHECK(UvcDescSelect(&model, 0, &c, &f, &a) && f == 0 && a == 0);
    c.PayloadBytes = 3073; CHECK(!UvcDescSelect(&model, 0, &c, &f, &a)); CHECK(c.PayloadBytes == 3073);
    c.PayloadBytes = 3072; c.FrameBytes--; CHECK(!UvcDescSelect(&model, 0, &c, &f, &a));
    c.Format = 2; c.FrameBytes = 12345; CHECK(UvcDescSelect(&model, 0, &c, &f, &a) && f == 1);
    CHECK(UvcDescParse(fixture, used, 2, 2, &model) != 0); /* HID is never video. */
    CHECK(UvcDescParse(fixture, used, 0, 1, &model) != 0); /* HS endpoint on FS link. */
    CHECK(UvcDescParse(fixture, used, 0, 3, &model) == UVC_DESC_UNSUPPORTED);
    Bad(vc + 5, 21); Bad(vc + 12, 2); Bad(vc + 12, 0); Bad(iad + 3, 1);
    Bad(vs + 4, 255); Bad(vs + 3, 3); Bad(vs + 12, 2); Bad(vs + 8, 99); Bad(vs + 6, 1);
    Bad(format1 + 4, 2); Bad(format1 + 22, 2); Bad(format2 + 3, 1);
    Bad(format2 + 3, 3); Bad(frame1 + 3, 2);
    Bad(frame1 + 3, 0); Bad(frame1 + 5, 0x81); Bad(frame1 + 25, 3);
    Bad(frame1 + 21, 1);
    /* Explicit all-zero continuous step, reversed range and duplicate interval. */
    memcpy(changed, fixture, used); W32(changed + frame2 + 34, 0);
    CHECK(UvcDescParse(changed, used, 0, 2, &model) != 0);
    memcpy(changed, fixture, used); W32(changed + frame2 + 30, 1);
    CHECK(UvcDescParse(changed, used, 0, 2, &model) != 0);
    memcpy(changed, fixture, used); W32(changed + frame1 + 30, 333333);
    CHECK(UvcDescParse(changed, used, 0, 2, &model) != 0);
    memcpy(changed, fixture, used); W16(changed + frame1 + 5, 65534); W16(changed + frame1 + 7, 65535);
    CHECK(UvcDescParse(changed, used, 0, 2, &model) != 0); /* frame-byte multiplication overflow */
    memcpy(changed, fixture, used); changed[iad + 2] = 1; changed[iad + 3] = 1;
    CHECK(UvcDescParse(changed, used, 0, 2, &model) != 0); /* VS assigned to another function */
    memcpy(changed, fixture, used); changed[4] = 4;
    memcpy(changed + used, fixture + iad + 8, 22); /* second VC interface + header */
    changed[used + 2] = 3; W16(changed + 2, used + 22);
    CHECK(UvcDescParse(changed, used + 22, 0, 2, &model) != 0); /* another VC claims same VS */
    Bad(ep + 5, 0x1c); Bad(ep + 6, 0); Bad(ep + 6, 17); Bad(ep + 2, 0x82);
    memcpy(changed, fixture, used); W16(changed + ep + 4, 0x1200);
    CHECK(UvcDescParse(changed, used, 0, 2, &model) != 0); /* 3x512 forbidden */
    memcpy(changed, fixture, used); W16(changed + ep + 4, 0x0a00);
    CHECK(UvcDescParse(changed, used, 0, 2, &model) != 0); /* 2x512 forbidden */
    memcpy(changed, fixture, used); changed[ep + 6] = 4;
    CHECK(UvcDescParse(changed, used, 0, 2, &model) == 0 && model.Alt[0].ServiceMicroframes == 8);
    /* Every truncation is placed immediately before an inaccessible page. */
    for (i = 0; i < used; ++i) {
        memcpy(end - i, fixture, i);
        CHECK(UvcDescParse(end - i, i, 0, 2, &model) != 0 && !model.Valid);
    }
    for (i = 0; i < used; ++i) for (j = 1; j <= 128; j <<= 1) {
        memcpy(changed, fixture, used); changed[i] ^= (unsigned char)j;
        memcpy(end - used, changed, used);
        if (UvcDescParse(end - used, used, 0, 2, &model) == 0) {
            CHECK(model.Valid && model.Length <= used && model.FrameCount <= UVC_DESC_MAX_FRAMES && model.AltCount <= UVC_DESC_MAX_ALTS);
            for (f = 0; f < model.FrameCount; ++f) CHECK(model.Frame[f].Stream == 0 && UvcDescInterval(&model, f, model.Frame[f].DefaultInterval));
        } else CHECK(!model.Valid);
    }
    /* A longer transport buffer never expands wTotalLength. */
    memcpy(changed, fixture, used); memset(changed + used, 0xff, 4);
    CHECK(UvcDescParse(changed, used + 4, 0, 2, &model) == 0 && model.Length == used);
    /* Valid full-speed bulk camera, reusing the same format model. */
    oldsize = used; fixture[vs - 9 + 4] = 1;
    memmove(fixture + ep - 9, fixture + ep, used - ep); used -= 9; ep -= 9;
    fixture[ep + 3] = 2; W16(fixture + ep + 4, 64); fixture[ep + 6] = 0; W16(fixture + 2, used);
    CHECK(UvcDescParse(fixture, used, 0, 1, &model) == 0);
    CHECK(model.Alt[0].Type == 2 && model.Alt[0].Capacity == 64 && model.Alt[0].ServiceMicroframes == 0);
    c.Format = 2; c.Frame = 1; c.FrameBytes = 12345; c.PayloadBytes = 65536;
    CHECK(UvcDescSelect(&model, 0, &c, &f, &a)); CHECK(c.PayloadBytes == 65536);
    CHECK(oldsize > used); Limits(); Uvc15(); Layouts(end); VirtualFree(pages, 0, MEM_RELEASE);
    printf("PASS: %lu synthetic descriptor/codec checks including guarded truncations and mutations\n", checks);
    return 0;
}

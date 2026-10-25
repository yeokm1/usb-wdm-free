/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original little-endian UVC probe/commit serialization, no device I/O.
 */
#include "uvcctrl.h"
static unsigned int R16(const unsigned char *p)
{ return p[0] | ((unsigned int)p[1] << 8); }
static unsigned long R32(const unsigned char *p)
{ return (unsigned long)R16(p) | ((unsigned long)R16(p + 2) << 16); }
static void W16(unsigned char *p, unsigned int v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void W32(unsigned char *p, unsigned long v)
{ W16(p, (unsigned int)(v & 65535UL)); W16(p + 2, (unsigned int)(v >> 16)); }
static int Fits32(unsigned long v) { return ((v >> 16) >> 16) == 0; }
unsigned int UvcCtrlSize(unsigned int revision)
{
    if (revision == 0x100) return 26;
    if (revision == 0x110) return 34;
    if (revision == 0x150) return 48;
    return 0;
}
int UvcCtrlDecode(unsigned int revision, const unsigned char *p,
    unsigned int bytes, UVC_CTRL *c)
{
    UVC_CTRL empty = { 0 };
    unsigned int i, n = UvcCtrlSize(revision);
    if (!c) return 0;
    *c = empty;
    if (!p || !n || bytes != n) return 0;
    c->Hint = R16(p); c->Format = p[2]; c->Frame = p[3];
    c->Interval = R32(p + 4); c->KeyRate = R16(p + 8);
    c->PFrameRate = R16(p + 10); c->Quality = R16(p + 12);
    c->Window = R16(p + 14); c->Delay = R16(p + 16);
    c->FrameBytes = R32(p + 18); c->PayloadBytes = R32(p + 22);
    if (n >= 34) {
        c->Clock = R32(p + 26); c->Framing = p[30];
        c->Preferred = p[31]; c->Minimum = p[32]; c->Maximum = p[33];
    }
    if (n == 48) for (i = 0; i < 14; ++i) c->Tail[i] = p[34 + i];
    return 1;
}
int UvcCtrlEncode(unsigned int revision, const UVC_CTRL *c,
    unsigned char *p, unsigned int bytes)
{
    unsigned int i, n = UvcCtrlSize(revision);
    if (!c || !p || !n || bytes != n || c->Hint > 65535 ||
        c->Format > 255 || c->Frame > 255 || c->KeyRate > 65535 ||
        c->PFrameRate > 65535 || c->Quality > 65535 || c->Window > 65535 ||
        c->Delay > 65535 || c->Framing > 255 || c->Preferred > 255 ||
        c->Minimum > 255 || c->Maximum > 255 || !Fits32(c->Interval) ||
        !Fits32(c->FrameBytes) || !Fits32(c->PayloadBytes) || !Fits32(c->Clock)) return 0;
    for (i = 0; i < n; ++i) p[i] = 0;
    W16(p, c->Hint); p[2] = (unsigned char)c->Format; p[3] = (unsigned char)c->Frame;
    W32(p + 4, c->Interval); W16(p + 8, c->KeyRate);
    W16(p + 10, c->PFrameRate); W16(p + 12, c->Quality);
    W16(p + 14, c->Window); W16(p + 16, c->Delay);
    W32(p + 18, c->FrameBytes); W32(p + 22, c->PayloadBytes);
    if (n >= 34) {
        W32(p + 26, c->Clock); p[30] = (unsigned char)c->Framing;
        p[31] = (unsigned char)c->Preferred; p[32] = (unsigned char)c->Minimum;
        p[33] = (unsigned char)c->Maximum;
    }
    if (n == 48) for (i = 0; i < 14; ++i) p[34 + i] = c->Tail[i];
    return 1;
}

int UvcCtrl15Get(unsigned int revision, const UVC_CTRL *c, UVC_CTRL15 *f)
{
    UVC_CTRL15 empty = { 0 };
    unsigned int i;
    if (!f) return 0;
    *f = empty;
    if (revision != 0x150 || !c) return 0;
    f->Usage = c->Tail[0]; f->BitDepthLuma = c->Tail[1];
    f->Settings = c->Tail[2]; f->MaxReferencesPlus1 = c->Tail[3];
    f->RateControlModes = R16(c->Tail + 4);
    for (i = 0; i < 4; ++i) f->Layout[i] = R16(c->Tail + 6 + 2 * i);
    return 1;
}

int UvcCtrl15Set(unsigned int revision, UVC_CTRL *c, const UVC_CTRL15 *f)
{
    unsigned char tail[14];
    unsigned int i;
    if (revision != 0x150 || !c || !f || f->Usage > 255 ||
        f->BitDepthLuma > 255 || f->Settings > 255 ||
        f->MaxReferencesPlus1 > 255 || f->RateControlModes > 65535) return 0;
    for (i = 0; i < 4; ++i) if (f->Layout[i] > 65535) return 0;
    tail[0] = (unsigned char)f->Usage; tail[1] = (unsigned char)f->BitDepthLuma;
    tail[2] = (unsigned char)f->Settings; tail[3] = (unsigned char)f->MaxReferencesPlus1;
    W16(tail + 4, f->RateControlModes);
    for (i = 0; i < 4; ++i) W16(tail + 6 + 2 * i, f->Layout[i]);
    for (i = 0; i < 14; ++i) c->Tail[i] = tail[i];
    return 1;
}

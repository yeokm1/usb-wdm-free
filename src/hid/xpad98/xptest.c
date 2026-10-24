/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host tests of the xpad98 portable cores (xusb.c, gip.c, devtab.c,
 * xpcfg.c, report.c). Built by test.ps1 with the repository's MSVC 6.0.
 *
 *   xptest            run every test; exit 0 only if all pass
 *   xptest ids SET    print the INF ids of SET (x360, x360w, gip), one
 *                     USB\VID_vvvv&PID_pppp per line, for infchk.ps1
 *
 * The configuration descriptors here are synthetic, shaped as design 5.1
 * expects each family; M0's captured descriptors are to replace them.
 */
#pragma warning(disable:4127)  /* the CHECK macro's while (0) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "xpcore.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ------------------------------------------------------------------ */
/* A small HID report descriptor reader, independent of report.c, to   */
/* check what a HID parser would see.                                  */

typedef struct FIELD {
    unsigned ReportId, BitOffset, Size, Page, Usage, Const, Null;
    long LogMin, LogMax;
} FIELD;

typedef struct HIDINFO {
    unsigned Collections, Depth, TopUsage[8];
    unsigned ReportBits[8];     /* by report id 0..4 */
    unsigned NumFields;
    FIELD Field[160];
    int Error;
} HIDINFO;

static long ItemValue(const XU8 *d, unsigned size, int sign)
{
    unsigned long v = 0;
    unsigned i;
    for (i = 0; i < size; i++)
        v |= (unsigned long)d[i] << (8 * i);
    if (sign && size && size < 4 && (v & (1UL << (8 * size - 1))))
        v |= ~0UL << (8 * size);
    return (long)v;
}

static void HidRead(const XU8 *d, unsigned len, HIDINFO *h)
{
    unsigned pos = 0, page = 0, rsize = 0, rcount = 0, rid = 0;
    long lmin = 0, lmax = 0;
    unsigned usages[32], nu = 0, umin = 0, umax = 0, hasRange = 0;
    memset(h, 0, sizeof(*h));
    while (pos < len) {
        XU8 pre = d[pos];
        unsigned size = (pre & 3) == 3 ? 4 : (pre & 3);
        unsigned tag = pre & 0xFC;
        const XU8 *data = d + pos + 1;
        long sv, uv;
        if (pos + 1 + size > len) { h->Error = 1; return; }
        sv = ItemValue(data, size, 1);
        uv = ItemValue(data, size, 0);
        switch (tag) {
        case 0x04: page = (unsigned)uv; break;
        case 0x14: lmin = sv; break;
        case 0x24: lmax = sv; break;
        case 0x34: case 0x44: case 0x54: case 0x64: break;  /* physical, unit */
        case 0x74: rsize = (unsigned)uv; break;
        case 0x94: rcount = (unsigned)uv; break;
        case 0x84: rid = (unsigned)uv; if (rid >= 8) h->Error = 1; break;
        case 0x08: if (nu < 32) usages[nu++] = (unsigned)uv; break;
        case 0x18: umin = (unsigned)uv; hasRange = 1; break;
        case 0x28: umax = (unsigned)uv; hasRange = 1; break;
        case 0xA0:
            if (h->Depth == 0) {
                if (h->Collections < 8 && nu)
                    h->TopUsage[h->Collections] = usages[0];
                h->Collections++;
            }
            h->Depth++;
            nu = 0; hasRange = 0;
            break;
        case 0xC0:
            if (h->Depth == 0) h->Error = 1; else h->Depth--;
            break;
        case 0x80: {
            unsigned i;
            for (i = 0; i < rcount; i++) {
                FIELD *f;
                if (h->NumFields >= 160) { h->Error = 1; return; }
                f = &h->Field[h->NumFields++];
                f->ReportId = rid;
                f->BitOffset = h->ReportBits[rid];
                f->Size = rsize;
                f->Page = page;
                f->Const = (unsigned)(uv & 1);
                f->Null = (unsigned)((uv >> 6) & 1);
                f->LogMin = lmin;
                f->LogMax = lmax;   /* a 4-byte item is not sign-extended */
                if (hasRange)
                    f->Usage = umin + i <= umax ? umin + i : umax;
                else if (nu)
                    f->Usage = usages[i < nu ? i : nu - 1];
                else
                    f->Usage = 0;
                h->ReportBits[rid] += rsize;
            }
            nu = 0; hasRange = 0;
            break;
        }
        default:
            h->Error = 2;
            break;
        }
        pos += 1 + size;
    }
    if (h->Depth != 0) h->Error = 3;
}

static unsigned long Bits(const XU8 *r, unsigned off, unsigned size)
{
    unsigned long v = 0;
    unsigned i;
    for (i = 0; i < size; i++)
        if (r[(off + i) / 8] & (1u << ((off + i) % 8)))
            v |= 1UL << i;
    return v;
}

/* Value of the field with page/usage in report id rid; -1 if absent. */
/* An axis expectation on the 0..65535 scale as the report carries it. */
#define AX(x) ((x) < 0 ? (long)(x) : (long)(x) >> 1)

static long FieldValue(const HIDINFO *h, unsigned rid, unsigned page,
                       unsigned usage, const XU8 *report)
{
    unsigned i;
    const XU8 *body = rid ? report + 1 : report;
    for (i = 0; i < h->NumFields; i++) {
        const FIELD *f = &h->Field[i];
        if (f->ReportId == rid && f->Page == page && f->Usage == usage && !f->Const) {
            long v = (long)Bits(body, f->BitOffset, f->Size);
            /* A signed field reads as its offset from Logical Minimum, so
             * the checks below keep the 0..65535 scale of WinMM. */
            if (f->LogMin < 0) {
                if (f->Size < 32 && (v & (1L << (f->Size - 1))))
                    v -= 1L << f->Size;
                v -= f->LogMin;
            }
            return v;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */

static void TestX360(void)
{
    XU8 p[20];
    XP_PAD pad;
    memset(p, 0, sizeof(p));
    XpPadReset(&pad, 255);
    p[0] = 0x00; p[1] = 0x14;
    p[2] = 0x01 | 0x08 | 0x10 | 0x80;   /* up, right, Start, RS */
    p[3] = 0x10 | 0x80 | 0x02 | 0x04;   /* A, Y, RB, Guide */
    p[4] = 200; p[5] = 7;
    p[6] = 0xFF; p[7] = 0x7F;           /* LX 32767 */
    p[8] = 0xFF; p[9] = 0x7F;           /* LY 32767 (up) */
    p[10] = 0x00; p[11] = 0x80;         /* RX -32768 */
    p[12] = 0x00; p[13] = 0x80;         /* RY -32768 (down) */
    CHECK(XpX360Parse(p, 20, &pad) == XP_EV_DATA);
    CHECK(pad.Dpad == (XP_D_UP | XP_D_RIGHT));
    CHECK(pad.Buttons == (XP_B_START | XP_B_RS | XP_B_A | XP_B_Y | XP_B_RB | XP_B_GUIDE));
    CHECK(pad.LT == 200 && pad.RT == 7 && pad.TriggerMax == 255);
    CHECK(pad.LX == 32767 && pad.LY == -32768);
    CHECK(pad.RX == -32768 && pad.RY == 32767);
    /* Not pad data, or too short: untouched. */
    p[0] = 0x01;
    CHECK(XpX360Parse(p, 20, &pad) == 0);
    p[0] = 0x00;
    CHECK(XpX360Parse(p, 13, &pad) == 0);
    CHECK(pad.LT == 200);
    /* Builders. */
    {
        XU8 o[16];
        CHECK(XpX360Led(o, XpLedForPlayer(0)) == 3 && o[0] == 1 && o[1] == 3 && o[2] == 2);
        CHECK(XpLedForPlayer(5) == 3);
        CHECK(XpX360Rumble(o, 0x11, 0x22) == 8 && o[1] == 8 && o[3] == 0x11 && o[4] == 0x22);
    }
}

static void TestX360w(void)
{
    XU8 p[29], o[12];
    XP_PAD pad;
    unsigned ev;
    XpPadReset(&pad, 255);
    memset(p, 0, sizeof(p));
    /* Connect: 08 80 */
    p[0] = 0x08; p[1] = 0x80;
    ev = XpX360wParse(p, 2, &pad);
    CHECK(ev == XP_EV_CONNECT && pad.Present == 1);
    CHECK(XpX360wParse(p, 2, &pad) == 0);       /* no change, no event */
    /* Pad data: 00 01 00 f0 | 00 13 <xusb body> */
    memset(p, 0, sizeof(p));
    p[1] = 0x01; p[3] = 0xF0;
    p[4] = 0x00; p[5] = 0x13;
    p[6] = 0x02;                /* down */
    p[7] = 0x20;                /* B */
    p[8] = 255;                 /* LT */
    ev = XpX360wParse(p, 29, &pad);
    CHECK(ev == XP_EV_DATA);
    CHECK(pad.Dpad == XP_D_DOWN && pad.Buttons == XP_B_B && pad.LT == 255);
    CHECK(pad.LY == -1);        /* ~0 */
    CHECK(XpX360wParse(p, 17, &pad) == 0);      /* too short for a body */
    /* Disconnect: 08 00 resets to centred. */
    memset(p, 0, sizeof(p));
    p[0] = 0x08;
    ev = XpX360wParse(p, 2, &pad);
    CHECK(ev == XP_EV_DISCONNECT && pad.Present == 0 && pad.Buttons == 0 && pad.LT == 0 && pad.LY == 0);
    /* Data while absent is ignored. */
    memset(p, 0, sizeof(p));
    p[1] = 0x01; p[7] = 0x20;
    CHECK(XpX360wParse(p, 29, &pad) == 0 && pad.Buttons == 0);
    CHECK(XpX360wPresence(o) == 12 && o[0] == 0x08 && o[2] == 0x0F && o[3] == 0xC0 && o[11] == 0);
    CHECK(XpX360wLed(o, 6) == 12 && o[2] == 0x08 && o[3] == 0x46);
    CHECK(XpX360wPowerOff(o) == 12 && o[2] == 0x08 && o[3] == 0xC0 && o[0] == 0);
    CHECK(XpX360wRumble(o, 1, 2) == 12 && o[1] == 1 && o[5] == 1 && o[6] == 2);
}

static unsigned InitSeq(XU16 vid, XU16 pid, XU8 cmds[16])
{
    unsigned cursor = 0, n = 0, len;
    XU8 pkt[XP_PKT_LEN];
    while ((len = XpGipNextInit(vid, pid, &cursor, pkt)) != 0 && n < 16) {
        cmds[n++] = pkt[0];
        if (pkt[0] == 0x05 && pkt[3] == 0x0f) cmds[n - 1] = 0x55;  /* s_init */
        if (pkt[0] == 0x09 && pkt[8] == 0x1D) cmds[n - 1] = 0x91;  /* rumble begin */
        if (pkt[0] == 0x09 && pkt[8] == 0x00) cmds[n - 1] = 0x90;  /* rumble end */
        if (pkt[0] == 0x4d) cmds[n - 1] = 0x4d;
        CHECK(pkt[2] == 0 || pkt[0] == 0x4d);    /* byte 2 is the caller's sequence */
    }
    return n;
}

static void TestGip(void)
{
    XU8 c[16], p[64], ack[XP_PKT_LEN];
    unsigned n, ackLen, ev;
    XP_PAD pad;

    n = InitSeq(0x045E, 0x0B12, c);         /* Series X|S */
    CHECK(n == 3 && c[0] == 0x05 && c[1] == 0x0a && c[2] == 0x06);
    n = InitSeq(0x045E, 0x02EA, c);         /* One S */
    CHECK(n == 4 && c[0] == 0x05 && c[1] == 0x55 && c[2] == 0x0a && c[3] == 0x06);
    n = InitSeq(0x045E, 0x0B00, c);         /* Elite 2 */
    CHECK(n == 5 && c[1] == 0x55 && c[2] == 0x4d);
    n = InitSeq(0x0E6F, 0x0165, c);         /* Titanfall 2: Hori ack-id first */
    CHECK(n == 4 && c[0] == 0x01 && c[1] == 0x05);
    n = InitSeq(0x24C6, 0x543A, c);         /* PowerA: rumble begin, end last */
    CHECK(n == 5 && c[3] == 0x91 && c[4] == 0x90);
    {
        unsigned cursor = 0;
        XU8 pkt[XP_PKT_LEN];
        CHECK(XpGipNextInit(0x045E, 0x0B12, &cursor, pkt) == 5);
        CHECK(pkt[0] == 0x05 && pkt[1] == 0x20 && pkt[3] == 0x01 && pkt[4] == 0x00);
        CHECK(XpGipNextInit(0x045E, 0x0B12, &cursor, pkt) == 7);
        CHECK(pkt[0] == 0x0a && pkt[4] == 0 && pkt[5] == 1 && pkt[6] == 0x14);
    }

    XpPadReset(&pad, 1023);
    memset(p, 0, sizeof(p));
    p[0] = 0x20; p[1] = 0x00; p[2] = 0x05; p[3] = 0x0e;
    p[4] = 0x04 | 0x10 | 0x40;          /* Menu, A, X */
    p[5] = 0x08 | 0x10 | 0x40;          /* right, LB, LS */
    p[6] = 0xFF; p[7] = 0x03;           /* LT 1023 */
    p[8] = 0x00; p[9] = 0x02;           /* RT 512 */
    p[10] = 0x34; p[11] = 0x12;         /* LX 0x1234 */
    p[12] = 0x00; p[13] = 0x00;         /* LY 0 -> -1 */
    p[14] = 0x01; p[15] = 0x80;         /* RX -32767 */
    p[16] = 0xFF; p[17] = 0xFF;         /* RY -1 -> 0 */
    ev = XpGipParse(p, 18, &pad, ack, &ackLen);
    CHECK(ev == XP_EV_DATA && ackLen == 0);
    CHECK(pad.Buttons == (XP_B_START | XP_B_A | XP_B_X | XP_B_LB | XP_B_LS));
    CHECK(pad.Dpad == XP_D_RIGHT);
    CHECK(pad.LT == 1023 && pad.RT == 512 && pad.TriggerMax == 1023);
    CHECK(pad.LX == 0x1234 && pad.LY == -1 && pad.RX == -32767 && pad.RY == 0);
    CHECK(XpGipParse(p, 17, &pad, ack, &ackLen) == 0);

    /* Guide with the ack request: 07 30 SS 02 01 5b */
    memset(p, 0, sizeof(p));
    p[0] = 0x07; p[1] = 0x30; p[2] = 0x2A; p[3] = 0x02; p[4] = 0x01; p[5] = 0x5B;
    ev = XpGipParse(p, 6, &pad, ack, &ackLen);
    CHECK(ev == (XP_EV_DATA | XP_EV_ACK));
    CHECK(ackLen == 13 && ack[0] == 0x01 && ack[1] == 0x20 && ack[2] == 0x2A &&
          ack[3] == 0x09 && ack[5] == 0x07 && ack[6] == 0x20 && ack[7] == 0x02);
    CHECK(pad.Buttons & XP_B_GUIDE);
    /* Guide survives an input report, which carries no Guide bit. */
    memset(p, 0, sizeof(p));
    p[0] = 0x20;
    XpGipParse(p, 18, &pad, ack, &ackLen);
    CHECK((pad.Buttons & XP_B_GUIDE) && !(pad.Buttons & XP_B_A));
    /* Guide release without the ack flag. */
    memset(p, 0, sizeof(p));
    p[0] = 0x07; p[1] = 0x20;
    ev = XpGipParse(p, 6, &pad, ack, &ackLen);
    CHECK(ev == XP_EV_DATA && ackLen == 0 && !(pad.Buttons & XP_B_GUIDE));
    /* Announce, and unknown commands. */
    memset(p, 0, sizeof(p));
    p[0] = 0x02; p[1] = 0x20;
    CHECK(XpGipParse(p, 32, &pad, ack, &ackLen) == XP_EV_ANNOUNCE);
    p[0] = 0x03;
    CHECK(XpGipParse(p, 32, &pad, ack, &ackLen) == 0);
    CHECK(XpGipParse(p, 3, &pad, ack, &ackLen) == 0);
}

static void TestDevtab(void)
{
    unsigned i, n = XpDeviceCount(), x360 = 0, w = 0, gip = 0;
    for (i = 0; i < n; i++) {
        const XP_DEVICE *d = XpDeviceAt(i);
        if (i) {
            const XP_DEVICE *p = XpDeviceAt(i - 1);
            CHECK(((XU32)p->Vid << 16 | p->Pid) < ((XU32)d->Vid << 16 | d->Pid));
        }
        CHECK(d->Name && d->Name[0]);
        CHECK(d->Family >= XP_FAM_X360 && d->Family <= XP_FAM_GIP);
        CHECK(XpFindDevice(d->Vid, d->Pid) == d);
        if (d->Family == XP_FAM_X360) x360++;
        if (d->Family == XP_FAM_X360W) w++;
        if (d->Family == XP_FAM_GIP) gip++;
    }
    CHECK(w == 3);
    CHECK(x360 > 100 && gip > 50);
    CHECK(XpFindDevice(0x045E, 0x0719) && XpFindDevice(0x045E, 0x0719)->Family == XP_FAM_X360W);
    CHECK(XpFindDevice(0x045E, 0x0B12) && XpFindDevice(0x045E, 0x0B12)->Family == XP_FAM_GIP);
    CHECK(XpFindDevice(0x2DC8, 0x3106) && XpFindDevice(0x2DC8, 0x3106)->Family == XP_FAM_X360);
    CHECK(XpFindDevice(0x045E, 0x0202) == 0);       /* original Xbox: not planned */
    CHECK(XpFindDevice(0x0000, 0x0000) == 0 && XpFindDevice(0xFFFF, 0xFFFF) == 0);
    CHECK((XpFindDevice(0x0738, 0x4A01)->Flags & XP_F_TRIGBTN) != 0);
    CHECK((XpFindDevice(0x366C, 0x0005)->Flags & XP_F_DELAYINIT) != 0);
    CHECK((XpFindDevice(0x31E3, 0x1100)->Flags & XP_F_NOINF) != 0);
    CHECK(XpX360cCount() == 25);
    for (i = 0; i < XpX360cCount(); i++) {
        XU16 v, p, v2, p2;
        unsigned j;
        const XP_DEVICE *d;
        XpX360cAt(i, &v, &p);
        CHECK(XpIsX360c(v, p));
        for (j = 0; j < i; j++) {
            XpX360cAt(j, &v2, &p2);
            CHECK(v != v2 || p != p2);
        }
        d = XpFindDevice(v, p);
        CHECK(!d || d->Family == XP_FAM_X360);
    }
    CHECK(!XpIsX360c(0x2DC8, 0x3106));
}

/* ---- configuration descriptors ---- */

typedef struct CB { XU8 b[512]; unsigned n; } CB;

static void Cfg(CB *c) { memset(c, 0, sizeof(*c)); c->n = 9; c->b[0] = 9; c->b[1] = 2; c->b[4] = 0; c->b[5] = 1; c->b[7] = 0xA0; c->b[8] = 250; }
static void If(CB *c, XU8 num, XU8 alt, XU8 neps, XU8 cl, XU8 sc, XU8 pr)
{
    XU8 *d = c->b + c->n;
    d[0] = 9; d[1] = 4; d[2] = num; d[3] = alt; d[4] = neps; d[5] = cl; d[6] = sc; d[7] = pr; d[8] = 0;
    c->n += 9;
    if (alt == 0) c->b[4]++;
}
static void Ep(CB *c, XU8 addr, XU8 attr, XU16 mps, XU8 interval)
{
    XU8 *d = c->b + c->n;
    d[0] = 7; d[1] = 5; d[2] = addr; d[3] = attr; d[4] = (XU8)mps; d[5] = (XU8)(mps >> 8); d[6] = interval;
    c->n += 7;
}
static void Vend(CB *c, XU8 type, XU8 len)
{
    XU8 *d = c->b + c->n;
    unsigned i;
    d[0] = len; d[1] = type;
    for (i = 2; i < len; i++) d[i] = (XU8)i;
    c->n += len;
}
static void Done(CB *c) { c->b[2] = (XU8)c->n; c->b[3] = (XU8)(c->n >> 8); }

static void TestConfig(void)
{
    CB c;
    XP_CONFIG k;
    unsigned i;

    /* Wired 360 pad: pad, headset, unknown, security. */
    Cfg(&c);
    If(&c, 0, 0, 2, 0xFF, 0x5D, 0x01); Vend(&c, 0x21, 17);
    Ep(&c, 0x81, 3, 32, 4); Ep(&c, 0x01, 3, 32, 8);
    If(&c, 1, 0, 4, 0xFF, 0x5D, 0x03); Vend(&c, 0x21, 27);
    Ep(&c, 0x82, 3, 32, 2); Ep(&c, 0x02, 3, 32, 4); Ep(&c, 0x83, 3, 32, 64); Ep(&c, 0x03, 3, 32, 16);
    If(&c, 2, 0, 1, 0xFF, 0x5D, 0x02); Vend(&c, 0x21, 9);
    Ep(&c, 0x84, 3, 32, 16);
    If(&c, 3, 0, 0, 0xFF, 0xFD, 0x13); Vend(&c, 0x41, 6);
    Done(&c);
    CHECK(XpParseConfig(c.b, c.n, &k) == 0);
    CHECK(k.NumInterfaces == 4 && k.AllVendor && k.Family == XP_FAM_X360);
    CHECK(k.NumChannels == 1 && k.ChanIface[0] == 0 && k.ChanIn[0] == 0x81 && k.ChanOut[0] == 0x01);
    CHECK(k.Iface[1].NumEndpoints == 4 && k.Iface[3].NumEndpoints == 0);
    CHECK(k.ConfigValue == 1);
    {
        /* SELECT_CONFIGURATION sets: all four interfaces hold 7 endpoints
         * (over xHCI98 3.1.0.0's 4 per device); the pad's own, 2. */
        XU8 idx[XP_MAX_IF];
        unsigned eps, n;
        n = XpSelectSet(&k, 0, idx, &eps);
        CHECK(n == 4 && eps == 7 && idx[0] == 0 && idx[3] == 3);
        n = XpSelectSet(&k, 1, idx, &eps);
        CHECK(n == 1 && eps == 2 && idx[0] == 0);
    }
    /* Truncated, bad total, endpoint count mismatch. */
    CHECK(XpParseConfig(c.b, c.n - 1, &k) == XP_CFG_EBAD);
    c.b[2]--;
    CHECK(XpParseConfig(c.b, c.n, &k) == XP_CFG_EBAD);
    c.b[2]++;
    c.b[9 + 4] = 3;     /* interface 0 declares 3 endpoints */
    CHECK(XpParseConfig(c.b, c.n, &k) == XP_CFG_EBAD);
    c.b[9 + 4] = 2;
    c.b[9 + 9 + 17] = 0;    /* zero-length descriptor */
    CHECK(XpParseConfig(c.b, c.n, &k) == XP_CFG_EBAD);

    /* Receiver: four slots of pad + headset interfaces. */
    Cfg(&c);
    for (i = 0; i < 4; i++) {
        If(&c, (XU8)(2 * i), 0, 2, 0xFF, 0x5D, 0x81); Vend(&c, 0x21, 20);
        Ep(&c, (XU8)(0x81 + 2 * i), 3, 32, 1); Ep(&c, (XU8)(0x01 + 2 * i), 3, 32, 8);
        If(&c, (XU8)(2 * i + 1), 0, 2, 0xFF, 0x5D, 0x82); Vend(&c, 0x21, 20);
        Ep(&c, (XU8)(0x82 + 2 * i), 3, 32, 2); Ep(&c, (XU8)(0x02 + 2 * i), 3, 32, 4);
    }
    Done(&c);
    CHECK(XpParseConfig(c.b, c.n, &k) == 0);
    CHECK(k.Family == XP_FAM_X360W && k.NumChannels == 4 && k.NumInterfaces == 8);
    {
        XU8 idx[XP_MAX_IF];
        unsigned eps, n = XpSelectSet(&k, 1, idx, &eps);
        /* the four pad slots only, still 8 endpoints: over xHCI98 3.1.0.0's cap */
        CHECK(n == 4 && idx[0] == 0 && idx[1] == 2 && idx[3] == 6 && eps == 8);
    }
    CHECK(k.ChanIface[3] == 6 && k.ChanIn[3] == 0x87 && k.ChanOut[3] == 0x07);

    /* GIP: data, audio with alternate settings, a third interface. */
    Cfg(&c);
    If(&c, 0, 0, 2, 0xFF, 0x47, 0xD0);
    Ep(&c, 0x02, 3, 64, 4); Ep(&c, 0x82, 3, 64, 4);
    If(&c, 1, 0, 0, 0xFF, 0x47, 0xD0);
    If(&c, 1, 1, 2, 0xFF, 0x47, 0xD0);
    Ep(&c, 0x03, 1, 228, 1); Ep(&c, 0x83, 1, 64, 1);
    If(&c, 2, 0, 0, 0xFF, 0x47, 0xD0);
    If(&c, 2, 1, 2, 0xFF, 0x47, 0xD0);
    Ep(&c, 0x04, 2, 64, 0); Ep(&c, 0x84, 2, 64, 0);
    Done(&c);
    CHECK(XpParseConfig(c.b, c.n, &k) == 0);
    CHECK(k.Family == XP_FAM_GIP && k.NumChannels == 1 && k.NumInterfaces == 3);
    CHECK(k.ChanIn[0] == 0x82 && k.ChanOut[0] == 0x02);
    CHECK(k.Iface[1].NumEndpoints == 0 && k.Iface[2].NumEndpoints == 0);

    /* GIP triple on interface 1 only: not driven. */
    Cfg(&c);
    If(&c, 1, 0, 2, 0xFF, 0x47, 0xD0);
    Ep(&c, 0x02, 3, 64, 4); Ep(&c, 0x82, 3, 64, 4);
    Done(&c);
    CHECK(XpParseConfig(c.b, c.n, &k) == XP_CFG_ENOPAD);

    /* A composite keyboard + XInput: parsed, AllVendor false. */
    Cfg(&c);
    If(&c, 0, 0, 1, 0x03, 0x01, 0x01);
    Ep(&c, 0x81, 3, 8, 10);
    If(&c, 1, 0, 2, 0xFF, 0x5D, 0x01);
    Ep(&c, 0x82, 3, 32, 4); Ep(&c, 0x02, 3, 32, 8);
    Done(&c);
    CHECK(XpParseConfig(c.b, c.n, &k) == 0);
    CHECK(!k.AllVendor && k.Family == XP_FAM_X360 && k.ChanIface[0] == 1);

    /* A pad interface whose endpoints are bulk: not driven. */
    Cfg(&c);
    If(&c, 0, 0, 2, 0xFF, 0x5D, 0x01);
    Ep(&c, 0x81, 2, 64, 0); Ep(&c, 0x01, 2, 64, 0);
    Done(&c);
    CHECK(XpParseConfig(c.b, c.n, &k) == XP_CFG_ENOPAD);

    /* Not a configuration descriptor. */
    c.b[1] = 1;
    CHECK(XpParseConfig(c.b, c.n, &k) == XP_CFG_EBAD);
}

/* ---- report descriptor and reports ---- */

static void CheckDescriptor(const XP_OPTIONS *o, unsigned cols)
{
    XU8 d[XP_REPORT_DESC_MAX];
    unsigned n = XpBuildReportDescriptor(o, cols, d, sizeof(d)), i, axes = 0, buttons = 0, hats = 0;
    HIDINFO h;
    XP_LAYOUT lay;
    XpLayoutOf(o, &lay);
    CHECK(n > 0);
    HidRead(d, n, &h);
    CHECK(h.Error == 0);
    CHECK(h.Collections == cols);
    for (i = 0; i < cols && i < 8; i++)
        CHECK(h.TopUsage[i] == o->TopUsage);
    for (i = 0; i <= cols; i++) {
        unsigned rid = cols > 1 ? i : 0;
        if (cols > 1 && i == 0) { CHECK(h.ReportBits[0] == 0); continue; }
        if (cols == 1 && i > 0) break;
        CHECK(h.ReportBits[rid] == lay.ReportLen * 8);
    }
    for (i = 0; i < h.NumFields; i++) {
        const FIELD *f = &h.Field[i];
        if (f->ReportId > 1) continue;      /* collection 1 suffices */
        if (f->Const) continue;
        /* 98 SE's parser reads a value field as signed in its report size:
         * the logical range must fit that (2026-10-23 98 SE leg). */
        if (f->Page == 1)
            CHECK(f->LogMin >= -(1L << (f->Size - 1)) && f->LogMax <= (1L << (f->Size - 1)) - 1);
        if (f->Page == 1 && f->Usage >= 0x30 && f->Usage <= 0x35) {
            CHECK(f->Size == 16 && f->LogMin == 0 && f->LogMax == 32767);
            CHECK(f->Usage == lay.AxisUsage[axes]);
            axes++;
        } else if (f->Page == 1 && f->Usage == 0x39) {
            CHECK(f->Size == 4 && f->LogMax == 7 && f->Null);
            hats++;
        } else if (f->Page == 9) {
            CHECK(f->Size == 1 && f->Usage == buttons + 1);
            buttons++;
        } else {
            CHECK(0);
        }
    }
    CHECK(axes == lay.NumAxes && hats == 1 && buttons == lay.NumButtons);
}

static void TestDescriptors(void)
{
    XP_OPTIONS o;
    unsigned layout, tb, g, top, cols;
    for (layout = 0; layout < 3; layout++)
        for (tb = 0; tb < 2; tb++)
            for (g = 0; g < 2; g++)
                for (top = 4; top <= 5; top++)
                    for (cols = 1; cols <= 4; cols++) {
                        XpOptionsUnset(&o);
                        o.Layout = layout; o.TriggerButtons = tb; o.GuideButton = g; o.TopUsage = top;
                        XpOptionsResolve(&o, 0);
                        CheckDescriptor(&o, cols);
                    }
    XpOptionsUnset(&o);
    XpOptionsResolve(&o, 0);
    CHECK(XpBuildReportDescriptor(&o, 0, 0, 0) == 0);
    CHECK(XpBuildReportDescriptor(&o, 5, 0, 0) == 0);
    {
        XU8 small[20];
        CHECK(XpBuildReportDescriptor(&o, 1, small, sizeof(small)) == 0);
    }
}

static void TestOptions(void)
{
    XP_OPTIONS o;
    XpOptionsUnset(&o);
    XpOptionsResolve(&o, 0);
    CHECK(o.Layout == 0 && o.TriggerButtons == 0 && o.TriggerThreshold == 30 &&
          o.TopUsage == 5 && o.GuideButton == 1 && o.Deadzone == 0);
    XpOptionsUnset(&o);
    o.Layout = 1;
    XpOptionsResolve(&o, 0);
    CHECK(o.TriggerButtons == 1);
    XpOptionsUnset(&o);
    XpOptionsResolve(&o, XP_F_TRIGBTN);
    CHECK(o.TriggerButtons == 1 && o.Layout == 0);
    XpOptionsUnset(&o);
    o.Layout = 7; o.TriggerButtons = 9; o.TriggerThreshold = 999; o.TopUsage = 3;
    o.GuideButton = 0; o.Deadzone = 99999;
    XpOptionsResolve(&o, 0);
    CHECK(o.Layout == 0 && o.TriggerButtons == 1 && o.TriggerThreshold == 254 &&
          o.TopUsage == 5 && o.GuideButton == 0 && o.Deadzone == 32767);
}

static void TestHat(void)
{
    CHECK(XpHatOf(0) == 8);
    CHECK(XpHatOf(XP_D_UP) == 0);
    CHECK(XpHatOf(XP_D_UP | XP_D_RIGHT) == 1);
    CHECK(XpHatOf(XP_D_RIGHT) == 2);
    CHECK(XpHatOf(XP_D_DOWN | XP_D_RIGHT) == 3);
    CHECK(XpHatOf(XP_D_DOWN) == 4);
    CHECK(XpHatOf(XP_D_DOWN | XP_D_LEFT) == 5);
    CHECK(XpHatOf(XP_D_LEFT) == 6);
    CHECK(XpHatOf(XP_D_UP | XP_D_LEFT) == 7);
    CHECK(XpHatOf(XP_D_UP | XP_D_DOWN) == 8);
    CHECK(XpHatOf(XP_D_LEFT | XP_D_RIGHT | XP_D_UP) == 0);
}

static void TestReports(void)
{
    XP_OPTIONS o;
    XP_PAD pad;
    XU8 d[XP_REPORT_DESC_MAX], r[XP_REPORT_MAX];
    HIDINFO h;
    unsigned n, rn;

    /* Layout 0 on a 360 pad: rest state. */
    XpOptionsUnset(&o);
    XpOptionsResolve(&o, 0);
    n = XpBuildReportDescriptor(&o, 1, d, sizeof(d));
    HidRead(d, n, &h);
    XpPadReset(&pad, 255);
    rn = XpBuildReport(&o, &pad, 0, r, sizeof(r));
    CHECK(rn == 13);
    CHECK(FieldValue(&h, 0, 1, 0x30, r) == AX(32768));
    CHECK(FieldValue(&h, 0, 1, 0x31, r) == AX(32768));
    CHECK(FieldValue(&h, 0, 1, 0x32, r) == AX(32768));
    CHECK(FieldValue(&h, 0, 1, 0x33, r) == AX(32768));
    CHECK(FieldValue(&h, 0, 1, 0x34, r) == AX(32768));
    CHECK(FieldValue(&h, 0, 1, 0x35, r) == AX(-1));
    CHECK(FieldValue(&h, 0, 1, 0x39, r) == 8);
    CHECK(FieldValue(&h, 0, 9, 1, r) == 0);
    /* Full LT: 32768 + 255*128; full RT: 32768 - 255*128. */
    pad.LT = 255;
    XpBuildReport(&o, &pad, 0, r, sizeof(r));
    CHECK(FieldValue(&h, 0, 1, 0x32, r) == AX(32768 + 255 * 128));
    pad.LT = 0; pad.RT = 255;
    XpBuildReport(&o, &pad, 0, r, sizeof(r));
    CHECK(FieldValue(&h, 0, 1, 0x32, r) == AX(32768 - 255 * 128));
    /* 10-bit: * 32. */
    pad.TriggerMax = 1023; pad.LT = 1023; pad.RT = 0;
    XpBuildReport(&o, &pad, 0, r, sizeof(r));
    CHECK(FieldValue(&h, 0, 1, 0x32, r) == AX(32768 + 1023 * 32));
    /* Sticks: extremes, Y up is 0. */
    XpPadReset(&pad, 255);
    pad.LX = -32768; pad.LY = -32768; pad.RX = 32767; pad.RY = 32767;
    XpBuildReport(&o, &pad, 0, r, sizeof(r));
    CHECK(FieldValue(&h, 0, 1, 0x30, r) == AX(0) && FieldValue(&h, 0, 1, 0x31, r) == AX(0));
    CHECK(FieldValue(&h, 0, 1, 0x33, r) == AX(65535) && FieldValue(&h, 0, 1, 0x34, r) == AX(65535));
    /* Buttons in design order, Guide 11. */
    XpPadReset(&pad, 255);
    pad.Buttons = XP_B_A | XP_B_Y | XP_B_START | XP_B_RS | XP_B_GUIDE;
    pad.Dpad = XP_D_DOWN | XP_D_LEFT;
    XpBuildReport(&o, &pad, 0, r, sizeof(r));
    CHECK(FieldValue(&h, 0, 9, 1, r) == 1 && FieldValue(&h, 0, 9, 2, r) == 0);
    CHECK(FieldValue(&h, 0, 9, 4, r) == 1 && FieldValue(&h, 0, 9, 8, r) == 1);
    CHECK(FieldValue(&h, 0, 9, 10, r) == 1 && FieldValue(&h, 0, 9, 11, r) == 1);
    CHECK(FieldValue(&h, 0, 9, 12, r) == -1);
    CHECK(FieldValue(&h, 0, 1, 0x39, r) == 5);
    CHECK((r[10] & 0xF0) == 0);         /* hat padding */
    CHECK((r[12] & 0xF8) == 0);         /* button padding (11 buttons) */

    /* Layout 2 with trigger buttons, report ids (receiver slot 3). */
    XpOptionsUnset(&o);
    o.Layout = 2; o.TriggerButtons = 1; o.TriggerThreshold = 30;
    XpOptionsResolve(&o, 0);
    n = XpBuildReportDescriptor(&o, 4, d, sizeof(d));
    HidRead(d, n, &h);
    XpPadReset(&pad, 255);
    pad.LT = 31; pad.RT = 30;
    rn = XpBuildReport(&o, &pad, 3, r, sizeof(r));
    CHECK(rn == 1 + 12 + 1 + 2 && r[0] == 3);
    CHECK(FieldValue(&h, 3, 1, 0x32, r) == AX((31L * 65535) / 255));
    CHECK(FieldValue(&h, 3, 1, 0x35, r) == AX((30L * 65535) / 255));
    CHECK(FieldValue(&h, 3, 9, 12, r) == 1);    /* LT 31 > 30 */
    CHECK(FieldValue(&h, 3, 9, 13, r) == 0);    /* RT 30 is not > 30 */
    pad.LT = 255;
    XpBuildReport(&o, &pad, 3, r, sizeof(r));
    CHECK(FieldValue(&h, 3, 1, 0x32, r) == AX(65535));

    /* Layout 1: right stick on Z/Rz, trigger buttons by default. */
    XpOptionsUnset(&o);
    o.Layout = 1;
    XpOptionsResolve(&o, 0);
    n = XpBuildReportDescriptor(&o, 1, d, sizeof(d));
    HidRead(d, n, &h);
    XpPadReset(&pad, 1023);
    pad.RX = -100; pad.RY = 100; pad.RT = 1023;
    rn = XpBuildReport(&o, &pad, 0, r, sizeof(r));
    CHECK(rn == 11);
    CHECK(FieldValue(&h, 0, 1, 0x32, r) == AX(32668) && FieldValue(&h, 0, 1, 0x35, r) == AX(32868));
    CHECK(FieldValue(&h, 0, 1, 0x33, r) == AX(-1));
    CHECK(FieldValue(&h, 0, 9, 13, r) == 1 && FieldValue(&h, 0, 9, 12, r) == 0);

    /* Guide off: triggers become buttons 11, 12. */
    XpOptionsUnset(&o);
    o.GuideButton = 0; o.TriggerButtons = 1;
    XpOptionsResolve(&o, 0);
    n = XpBuildReportDescriptor(&o, 1, d, sizeof(d));
    HidRead(d, n, &h);
    XpPadReset(&pad, 255);
    pad.Buttons = XP_B_GUIDE; pad.LT = 255;
    XpBuildReport(&o, &pad, 0, r, sizeof(r));
    CHECK(FieldValue(&h, 0, 9, 11, r) == 1 && FieldValue(&h, 0, 9, 12, r) == 0);
    CHECK(FieldValue(&h, 0, 9, 13, r) == -1);

    /* Deadzone: inside cut to centre, outside untouched. */
    XpOptionsUnset(&o);
    o.Deadzone = 8000;
    XpOptionsResolve(&o, 0);
    n = XpBuildReportDescriptor(&o, 1, d, sizeof(d));
    HidRead(d, n, &h);
    XpPadReset(&pad, 255);
    pad.LX = 5000; pad.LY = -6000;      /* radius 7810 */
    pad.RX = 6000; pad.RY = 6000;       /* radius 8485 */
    XpBuildReport(&o, &pad, 0, r, sizeof(r));
    CHECK(FieldValue(&h, 0, 1, 0x30, r) == AX(32768) && FieldValue(&h, 0, 1, 0x31, r) == AX(32768));
    CHECK(FieldValue(&h, 0, 1, 0x33, r) == AX(38768) && FieldValue(&h, 0, 1, 0x34, r) == AX(38768));
    pad.LX = -32768; pad.LY = -32768;
    o.Deadzone = 32767;
    XpBuildReport(&o, &pad, 0, r, sizeof(r));
    CHECK(FieldValue(&h, 0, 1, 0x30, r) == AX(0));

    /* Buffer too small. */
    CHECK(XpBuildReport(&o, &pad, 0, r, 12) == 0);
    CHECK(XpBuildReport(&o, &pad, 1, r, 13) == 0);
}

/* The end-to-end path a driver takes: packet, parser, report. */
/* 98 SE's joyhid refuses Rx/Ry (2026-10-23 legs): on 9x the right stick
 * moves to Rz/Slider (layout 0), Slider/Dial (layout 2); NT keeps Rx/Ry.
 * The descriptor and the report agree on the new usages. */
static void TestWin9xUsages(void)
{
    static const XU8 nt0[5] = { 0x30, 0x31, 0x32, 0x33, 0x34 };
    static const XU8 w90[5] = { 0x30, 0x31, 0x32, 0x35, 0x36 };
    static const XU8 nt2[6] = { 0x30, 0x31, 0x33, 0x34, 0x32, 0x35 };
    static const XU8 w92[6] = { 0x30, 0x31, 0x36, 0x37, 0x32, 0x35 };
    static const XU8 l1[4] = { 0x30, 0x31, 0x32, 0x35 };
    XP_OPTIONS o;
    XP_LAYOUT lay;
    XP_PAD pad;
    XU8 d[XP_REPORT_DESC_MAX], r[XP_REPORT_MAX];
    HIDINFO h;
    unsigned layout, w, i, n;
    for (layout = 0; layout < 3; layout++)
        for (w = 0; w < 2; w++) {
            const XU8 *want = layout == 0 ? (w ? w90 : nt0) : layout == 2 ? (w ? w92 : nt2) : l1;
            XpOptionsUnset(&o);
            o.Layout = layout;
            o.Win9x = w;
            XpOptionsResolve(&o, 0);
            XpLayoutOf(&o, &lay);
            for (i = 0; i < lay.NumAxes; i++)
                CHECK(lay.AxisUsage[i] == want[i]);
            n = XpBuildReportDescriptor(&o, 1, d, sizeof(d));
            CHECK(n > 0);
            HidRead(d, n, &h);
            CHECK(h.Error == 0);
            for (i = 0; i < h.NumFields; i++)
                if (h.Field[i].Page == 1 && !h.Field[i].Const)
                    CHECK(h.Field[i].Usage != 0x33 || !w);
            /* the right stick's X reaches its usage in the report */
            XpPadReset(&pad, 255);
            pad.RX = 32767;
            n = XpBuildReport(&o, &pad, 0, r, sizeof(r));
            CHECK(n == lay.ReportLen);
            if (layout != 1)
                CHECK(FieldValue(&h, 0, 1, layout == 0 ? (w ? 0x35 : 0x33) : (w ? 0x36 : 0x33), r) == AX(65535));
        }
}

static void TestPipeline(void)
{
    XP_OPTIONS o;
    XP_PAD pad;
    XU8 d[XP_REPORT_DESC_MAX], r[XP_REPORT_MAX], p[20], ack[XP_PKT_LEN];
    HIDINFO h;
    unsigned n, ackLen;

    XpOptionsUnset(&o);
    XpOptionsResolve(&o, 0);
    n = XpBuildReportDescriptor(&o, 1, d, sizeof(d));
    HidRead(d, n, &h);
    XpPadReset(&pad, 1023);
    memset(p, 0, sizeof(p));
    p[0] = 0x20; p[4] = 0x20; p[5] = 0x01; p[8] = 0xFF; p[9] = 0x03;
    p[12] = 0xFF; p[13] = 0x7F;         /* LY raw 32767 (up) */
    CHECK(XpGipParse(p, 18, &pad, ack, &ackLen) == XP_EV_DATA);
    XpBuildReport(&o, &pad, 0, r, sizeof(r));
    CHECK(FieldValue(&h, 0, 9, 2, r) == 1);             /* B */
    CHECK(FieldValue(&h, 0, 1, 0x39, r) == 0);          /* up */
    CHECK(FieldValue(&h, 0, 1, 0x32, r) == AX(32768 - 1023 * 32));
    CHECK(FieldValue(&h, 0, 1, 0x31, r) == AX(0));          /* stick up */
}

static int PrintIds(const char *set)
{
    unsigned i, n = XpDeviceCount(), fam;
    if (!strcmp(set, "x360")) fam = XP_FAM_X360;
    else if (!strcmp(set, "x360w")) fam = XP_FAM_X360W;
    else if (!strcmp(set, "gip")) fam = XP_FAM_GIP;
    else return 2;
    for (i = 0; i < n; i++) {
        const XP_DEVICE *d = XpDeviceAt(i);
        if (d->Family != fam || (d->Flags & XP_F_NOINF) || XpIsX360c(d->Vid, d->Pid))
            continue;
        printf("USB\\VID_%04X&PID_%04X\n", d->Vid, d->Pid);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "ids"))
        return PrintIds(argv[2]);
    TestX360();
    TestX360w();
    TestGip();
    TestDevtab();
    TestConfig();
    TestOptions();
    TestHat();
    TestDescriptors();
    TestReports();
    TestPipeline();
    TestWin9xUsages();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

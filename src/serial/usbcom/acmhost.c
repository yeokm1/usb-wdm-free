/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * acmhost.c - host tests for acmcore.c, built with the package's own
 * MSVC 6.0 compiler as a Win32 console program (hosttest.ps1).
 *
 * The descriptors below are written by hand for these tests from the
 * CDC 1.2 layouts; they imitate the shapes named in serial.md 5.2 and 6.1
 * (an Uno R3-like single CDC device, a Pico-like IAD composite, TinyUSB-like
 * dual CDC) and are not copied from any device or other project.
 * Every descriptor walk also runs with its buffer ending exactly at a
 * PAGE_NOACCESS page, so any read past the given length faults the test.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "acmcore.h"

static int g_fail, g_checks;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- guarded buffers ------------------------------------------------ */

static unsigned char *g_page;

static void GuardInit(void)
{
    DWORD old;
    g_page = (unsigned char *)VirtualAlloc(NULL, 2 * 65536, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_READWRITE);
    if (!g_page || !VirtualProtect(g_page + 65536, 65536, PAGE_NOACCESS, &old)) {
        printf("FAIL: guard page setup\n");
        ExitProcess(2);
    }
}

/* Copies data so that it ends at the no-access page. */
static const unsigned char *Guard(const unsigned char *d, unsigned long len)
{
    unsigned char *p = g_page + 65536 - len;
    memcpy(p, d, len);
    return p;
}

static int Find(const unsigned char *d, unsigned long len, unsigned long q,
                AC_FUNC *f, int max)
{
    return AcFindFunctions(Guard(d, len), len, q, f, max);
}

/* ---- descriptor builder ------------------------------------------- */

typedef struct B { unsigned char b[1024]; unsigned long n; } B;

static void Put(B *b, const unsigned char *d, unsigned long n)
{
    memcpy(b->b + b->n, d, n);
    b->n += n;
}
static void Cfg(B *b, unsigned char numIf)
{
    unsigned char d[9] = { 9, 2, 0, 0, 0, 1, 0, 0x80, 50 };
    d[4] = numIf;
    b->n = 0;
    Put(b, d, 9);
}
static void Fix(B *b) { b->b[2] = (unsigned char)b->n; b->b[3] = (unsigned char)(b->n >> 8); }
static void Iad(B *b, unsigned char first, unsigned char count)
{
    unsigned char d[8] = { 8, 0x0B, 0, 0, 2, 2, 0, 0 };
    d[2] = first; d[3] = count;
    Put(b, d, 8);
}
static void If(B *b, unsigned char num, unsigned char alt, unsigned char eps,
               unsigned char cls, unsigned char sub, unsigned char prot)
{
    unsigned char d[9] = { 9, 4, 0, 0, 0, 0, 0, 0, 0 };
    d[2] = num; d[3] = alt; d[4] = eps; d[5] = cls; d[6] = sub; d[7] = prot;
    Put(b, d, 9);
}
static void Ep(B *b, unsigned char addr, unsigned char attr, unsigned short max,
               unsigned char ival)
{
    unsigned char d[7] = { 7, 5, 0, 0, 0, 0, 0 };
    d[2] = addr; d[3] = attr; d[4] = (unsigned char)max;
    d[5] = (unsigned char)(max >> 8); d[6] = ival;
    Put(b, d, 7);
}
static void Header(B *b) { unsigned char d[5] = { 5, 0x24, 0x00, 0x10, 0x01 }; Put(b, d, 5); }
static void CallMgmt(B *b, unsigned char data)
{
    unsigned char d[5] = { 5, 0x24, 0x01, 0x00, 0 };
    d[4] = data;
    Put(b, d, 5);
}
static void Acm(B *b, unsigned char caps) { unsigned char d[4] = { 4, 0x24, 0x02, 0 }; d[3] = caps; Put(b, d, 4); }
static void Union(B *b, unsigned char ctl, unsigned char sub)
{
    unsigned char d[5] = { 5, 0x24, 0x06, 0, 0 };
    d[3] = ctl; d[4] = sub;
    Put(b, d, 5);
}

/* A CDC-ACM pair: comm `c` with notify `nep`, data `c+1` with bulk eps. */
static void Pair(B *b, unsigned char c, unsigned char nep, unsigned char in,
                 unsigned char out)
{
    If(b, c, 0, 1, 2, 2, 1);
    Header(b); CallMgmt(b, (unsigned char)(c + 1)); Acm(b, 0x06);
    Union(b, c, (unsigned char)(c + 1));
    Ep(b, nep, 3, 8, 16);
    If(b, (unsigned char)(c + 1), 0, 2, 0x0A, 0, 0);
    Ep(b, out, 2, 64, 0);
    Ep(b, in, 2, 64, 0);
}

static void TestDescriptors(void)
{
    B b;
    AC_FUNC f[AC_MAX_FUNCS];
    AC_IFACE ifs[16];
    int n;

    /* Uno R3-like: one pair. */
    Cfg(&b, 2); Pair(&b, 0, 0x82, 0x83, 0x04); Fix(&b);
    n = Find(b.b, b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 1);
    CHECK(f[0].CommIf == 0 && f[0].DataIf == 1 && f[0].DataAlt == 0);
    CHECK(f[0].NotifyEp == 0x82 && f[0].NotifyMax == 8 && f[0].NotifyInterval == 16);
    CHECK(f[0].BulkIn == 0x83 && f[0].BulkOut == 0x04 && f[0].InMax == 64 && f[0].OutMax == 64);
    CHECK(f[0].Caps == 0x06 && f[0].Flags == AC_F_UNION);
    CHECK(AcListInterfaces(b.b, b.n, ifs, 16) == 2 && ifs[1].Class == 0x0A);

    /* Pico-like composite: IAD, pair, then a vendor reset interface. */
    Cfg(&b, 3); Iad(&b, 0, 2); Pair(&b, 0, 0x81, 0x82, 0x02);
    If(&b, 2, 0, 0, 0xFF, 0, 1); Fix(&b);
    n = Find(b.b, b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].CommIf == 0 && f[0].DataIf == 1);
    CHECK(AcListInterfaces(b.b, b.n, ifs, 16) == 3);

    /* Function PDO view (xHCI98 / usbccgp): only interfaces 2 and 3. */
    Cfg(&b, 2); Pair(&b, 2, 0x83, 0x84, 0x04); Fix(&b);
    n = Find(b.b, b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].CommIf == 2 && f[0].DataIf == 3);

    /* Dual CDC (TinyUSB-like): two ports. */
    Cfg(&b, 4); Iad(&b, 0, 2); Pair(&b, 0, 0x81, 0x82, 0x02);
    Iad(&b, 2, 2); Pair(&b, 2, 0x83, 0x84, 0x04); Fix(&b);
    n = Find(b.b, b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 2 && f[0].DataIf == 1 && f[1].CommIf == 2 && f[1].DataIf == 3);
    CHECK(f[1].BulkIn == 0x84 && f[1].BulkOut == 0x04);
    CHECK(Find(b.b, b.n, 0, f, 1) == 1);

    /* Union reversed (names data as control): swap. */
    Cfg(&b, 2);
    If(&b, 0, 0, 1, 2, 2, 1); Header(&b); Acm(&b, 2); Union(&b, 1, 0);
    Ep(&b, 0x81, 3, 16, 10);
    If(&b, 1, 0, 2, 0x0A, 0, 0); Ep(&b, 0x82, 2, 64, 0); Ep(&b, 0x02, 2, 64, 0);
    Fix(&b);
    n = Find(b.b, b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].DataIf == 1 && f[0].Flags == (AC_F_UNION | AC_F_SWAPPED));

    /* No Union: Call Management names interface 2; interface 1 is HID. */
    Cfg(&b, 3);
    If(&b, 0, 0, 1, 2, 2, 1); Header(&b); CallMgmt(&b, 2); Acm(&b, 2);
    Ep(&b, 0x81, 3, 8, 10);
    If(&b, 1, 0, 1, 3, 0, 0); Ep(&b, 0x85, 3, 8, 10);
    If(&b, 2, 0, 2, 0x0A, 0, 0); Ep(&b, 0x82, 2, 64, 0); Ep(&b, 0x02, 2, 64, 0);
    Fix(&b);
    n = Find(b.b, b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].DataIf == 2 && f[0].Flags == AC_F_CALLMGMT);

    /* Neither: the next interface. Data at alt 1 only. */
    Cfg(&b, 2);
    If(&b, 0, 0, 1, 2, 2, 0); Header(&b); Ep(&b, 0x81, 3, 8, 10);
    If(&b, 1, 0, 0, 0x0A, 0, 0);
    If(&b, 1, 1, 2, 0x0A, 0, 0); Ep(&b, 0x82, 2, 512, 0); Ep(&b, 0x01, 2, 512, 0);
    Fix(&b);
    n = Find(b.b, b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].DataIf == 1 && f[0].DataAlt == 1 && f[0].Flags == AC_F_NEXT);
    CHECK(f[0].InMax == 512 && f[0].Caps == 0);

    /* Combined control and data in one interface. */
    Cfg(&b, 1);
    If(&b, 0, 0, 3, 2, 2, 1); Header(&b); Acm(&b, 2);
    Ep(&b, 0x81, 3, 8, 10); Ep(&b, 0x82, 2, 64, 0); Ep(&b, 0x03, 2, 64, 0);
    Fix(&b);
    n = Find(b.b, b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].DataIf == 0 && f[0].Flags == AC_F_COMBINED);
    CHECK(f[0].BulkIn == 0x82 && f[0].BulkOut == 0x03 && f[0].NotifyEp == 0x81);

    /* Union names an interface that is not this one; quirk ignores Union. */
    Cfg(&b, 3);
    If(&b, 0, 0, 1, 2, 2, 1); Header(&b); CallMgmt(&b, 2); Union(&b, 0, 1);
    Ep(&b, 0x81, 3, 8, 10);
    If(&b, 1, 0, 0, 0x0A, 0, 0);
    If(&b, 2, 0, 2, 0x0A, 0, 0); Ep(&b, 0x82, 2, 64, 0); Ep(&b, 0x02, 2, 64, 0);
    Fix(&b);
    n = Find(b.b, b.n, AC_Q_NO_UNION_NORMAL, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].DataIf == 2 && f[0].Flags == AC_F_CALLMGMT);
    /* Without the quirk the Union's interface 1 has no endpoints: combined
       is impossible (no bulk in comm), so the function is dropped. */
    n = Find(b.b, b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 0);
    CHECK(Find(b.b, b.n, AC_Q_IGNORE_DEVICE, f, AC_MAX_FUNCS) == 0);

    /* NO_CAP_LINE clears D1. */
    Cfg(&b, 2); Pair(&b, 0, 0x82, 0x83, 0x04); Fix(&b);
    n = Find(b.b, b.n, AC_Q_NO_CAP_LINE, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].Caps == 0x04);

    /* CDC-ECM (subclass 06) is not a serial port. */
    Cfg(&b, 2);
    If(&b, 0, 0, 1, 2, 6, 0); Header(&b); Union(&b, 0, 1); Ep(&b, 0x81, 3, 16, 10);
    If(&b, 1, 0, 0, 0x0A, 0, 0);
    If(&b, 1, 1, 2, 0x0A, 0, 0); Ep(&b, 0x82, 2, 64, 0); Ep(&b, 0x02, 2, 64, 0);
    Fix(&b);
    CHECK(Find(b.b, b.n, 0, f, AC_MAX_FUNCS) == 0);

    /* Malformed. */
    Cfg(&b, 2); Pair(&b, 0, 0x82, 0x83, 0x04); Fix(&b);
    CHECK(Find(b.b, b.n - 1, 0, f, AC_MAX_FUNCS) == -1);     /* total > len */
    b.b[9] = 0;                                                  /* bLength 0 */
    CHECK(Find(b.b, b.n, 0, f, AC_MAX_FUNCS) == -1);
    b.b[9] = 200;                                                /* overruns */
    CHECK(Find(b.b, b.n, 0, f, AC_MAX_FUNCS) == -1);
    CHECK(AcFindFunctions(NULL, 0, 0, f, 1) == -1);
    {
        unsigned char tiny[9] = { 9, 2, 9, 0, 0, 1, 0, 0x80, 50 };
        CHECK(Find(tiny, 9, 0, f, AC_MAX_FUNCS) == 0);
        tiny[2] = 8;
        CHECK(Find(tiny, 9, 0, f, AC_MAX_FUNCS) == -1);
        CHECK(Find(tiny, 8, 0, f, AC_MAX_FUNCS) == -1);
    }
}

/* Random mutations: no fault (guard page), sane results. */
static void TestFuzz(void)
{
    B base;
    unsigned long seed = 12345, i;
    AC_FUNC f[AC_MAX_FUNCS];
    AC_IFACE ifs[64];

    Cfg(&base, 4); Iad(&base, 0, 2); Pair(&base, 0, 0x81, 0x82, 0x02);
    Iad(&base, 2, 2); Pair(&base, 2, 0x83, 0x84, 0x04); Fix(&base);
    for (i = 0; i < 200000; i++) {
        unsigned char m[1024];
        unsigned long len = base.n, k, edits;
        int n, j;
        memcpy(m, base.b, base.n);
        seed = seed * 1103515245 + 12345;
        edits = 1 + (seed >> 16) % 4;
        for (k = 0; k < edits; k++) {
            seed = seed * 1103515245 + 12345;
            m[(seed >> 8) % base.n] = (unsigned char)(seed >> 20);
        }
        seed = seed * 1103515245 + 12345;
        if ((seed >> 16) % 4 == 0)
            len = 1 + (seed >> 4) % base.n;
        n = Find(m, len, 0, f, AC_MAX_FUNCS);
        g_checks++;
        if (n < -1 || n > AC_MAX_FUNCS) { g_fail++; printf("FAIL fuzz count %d\n", n); }
        for (j = 0; j < n; j++) {
            if (!(f[j].BulkIn & 0x80) || (f[j].BulkOut & 0x80) ||
                f[j].InMax == 0 || f[j].OutMax == 0) {
                g_fail++;
                printf("FAIL fuzz endpoints at %lu\n", i);
            }
        }
        (void)AcListInterfaces(Guard(m, len), len, ifs, 64);
    }
}

static void TestLine(void)
{
    AC_U8 lc[7];
    AC_DCB d, r;
    AC_LINE line;

    AcEncodeLineCoding(115200, 0, 0, 8, lc);
    CHECK(lc[0] == 0x00 && lc[1] == 0xC2 && lc[2] == 0x01 && lc[3] == 0x00);
    CHECK(lc[4] == 0 && lc[5] == 0 && lc[6] == 8);
    AcEncodeLineCoding(3000000, 2, 4, 7, lc);
    CHECK(lc[0] == 0xC0 && lc[1] == 0xC6 && lc[2] == 0x2D && lc[4] == 2 && lc[5] == 4 && lc[6] == 7);

    CHECK(AcMapBaud(0xFF10) == 110 && AcMapBaud(0xFF16) == 9600);
    CHECK(AcMapBaud(0xFF1B) == 38400 && AcMapBaud(0xFF1F) == 56000);
    CHECK(AcMapBaud(0xFF23) == 128000 && AcMapBaud(0xFF27) == 256000);
    CHECK(AcMapBaud(0xFF19) == 0 && AcMapBaud(0xFF1A) == 0);
    CHECK(AcMapBaud(1200) == 1200 && AcMapBaud(921600) == 921600 && AcMapBaud(0) == 0);
    CHECK(sizeof(AC_DCB) == 48);

    AcDefaultDcb(&d, 4096);
    AcDcbToLine(&d, &line);
    CHECK(line.Rate == 9600 && line.DataBits == 8 && line.Dtr == 1 && line.Rts == 1);
    CHECK(d.XoffLim == 1024 && d.XonChar == 0x11 && d.XoffChar == 0x13);

    r = d;
    r.BaudRate = 0xFF13; r.ByteSize = 7; r.Parity = 2; r.StopBits = 2;
    r.BitMask = AC_fBinary | AC_fDTRDisable;
    CHECK(AcApplyDcb(&d, &r, AC_mBaudRate | AC_mByteSize | AC_mParity | AC_mStopBits | AC_mBitMask) == 0);
    AcDcbToLine(&d, &line);
    CHECK(line.Rate == 1200 && line.DataBits == 7 && line.Parity == 2 && line.StopBits == 2);
    CHECK(line.Dtr == 0 && line.Rts == 1);

    r.BaudRate = 0xFF19;
    CHECK(AcApplyDcb(&d, &r, AC_mBaudRate) == AC_IE_BAUDRATE && d.BaudRate == 0xFF13);
    r.BaudRate = 0;
    CHECK(AcApplyDcb(&d, &r, AC_mBaudRate) == AC_IE_BAUDRATE);
    r.BaudRate = 9600; r.ByteSize = 4;
    CHECK(AcApplyDcb(&d, &r, AC_mBaudRate | AC_mByteSize) == AC_IE_BYTESIZE && d.BaudRate == 0xFF13);
    r.ByteSize = 16;
    CHECK(AcApplyDcb(&d, &r, AC_mByteSize) == 0 && d.ByteSize == 16);
    r.Parity = 5;
    CHECK(AcApplyDcb(&d, &r, AC_mParity) == AC_IE_DEFAULT);
    r.StopBits = 3;
    CHECK(AcApplyDcb(&d, &r, AC_mStopBits) == AC_IE_DEFAULT);
    r.BaudRate = 57600; r.ByteSize = 99;            /* unselected field ignored */
    CHECK(AcApplyDcb(&d, &r, AC_mBaudRate) == 0 && d.BaudRate == 57600 && d.ByteSize == 16);
}

static void TestNotify(void)
{
    AC_U8 n[32] = {
        0xA1, 0x20, 0, 0, 0, 0, 2, 0, 0x03, 0x00,       /* SERIAL_STATE if 0: DCD|DSR */
        0xA1, 0x00, 1, 0, 0, 0, 0, 0,                   /* NETWORK_CONNECTION */
        0xA1, 0x20, 0, 0, 2, 0, 2, 0, 0x08, 0x00        /* SERIAL_STATE if 2 */
    };
    AC_U16 st = 0xFFFF;
    AC_U32 ev = 0, er = 0;
    AC_U8 msr;

    CHECK(AcParseNotify(n, 28, 0, &st) == 1 && st == 0x0003);
    CHECK(AcParseNotify(n, 28, 2, &st) == 1 && st == 0x0008);
    {
        AC_U32 off = 0;
        AC_U8 two[20] = { 0xA1, 0x20, 0, 0, 0, 0, 2, 0, 0x10, 0,
                          0xA1, 0x20, 0, 0, 0, 0, 2, 0, 0x01, 0 };
        CHECK(AcNextNotify(two, 20, &off, 0, &st) == 1 && st == 0x10 && off == 10);
        CHECK(AcNextNotify(two, 20, &off, 0, &st) == 1 && st == 0x01 && off == 20);
        CHECK(AcNextNotify(two, 20, &off, 0, &st) == 0);
        off = 0;
        CHECK(AcNextNotify(two, 19, &off, 0, &st) == 1 && AcNextNotify(two, 19, &off, 0, &st) == 0);
    }
    CHECK(AcParseNotify(n, 9, 0, &st) == 0);             /* truncated */
    CHECK(AcParseNotify(n, 7, 0, &st) == 0);
    n[0] = 0x21;
    CHECK(AcParseNotify(n, 28, 2, &st) == 0);            /* stops at garbage */
    n[0] = 0xA1; n[6] = 0xFF; n[7] = 0xFF;
    CHECK(AcParseNotify(n, 28, 0, &st) == 0);            /* huge wLength */

    msr = AcStateToMsr(0, AC_SS_DCD | AC_SS_DSR, &ev, &er);
    CHECK(msr == (AC_MSR_CTS | AC_MSR_DSR | AC_MSR_DCD | AC_MSR_DDSR | AC_MSR_DDCD));
    CHECK((ev & (AC_EV_DSR | AC_EV_RLSD | AC_EV_DSRS | AC_EV_RLSDS | AC_EV_CTSS)) ==
          (AC_EV_DSR | AC_EV_RLSD | AC_EV_DSRS | AC_EV_RLSDS | AC_EV_CTSS) && er == 0);
    ev = er = 0;
    msr = AcStateToMsr(AC_SS_DCD | AC_SS_DSR, AC_SS_DCD | AC_SS_DSR | AC_SS_RING, &ev, &er);
    CHECK(msr == (AC_MSR_CTS | AC_MSR_DSR | AC_MSR_DCD | AC_MSR_RI) && ev == AC_EV_RING);
    ev = er = 0;
    msr = AcStateToMsr(AC_SS_RING, 0, &ev, &er);
    CHECK(msr == (AC_MSR_CTS | AC_MSR_TERI) && ev == AC_EV_RingTe);
    ev = er = 0;
    msr = AcStateToMsr(0, AC_SS_BREAK | AC_SS_FRAMING | AC_SS_PARITY | AC_SS_OVERRUN, &ev, &er);
    CHECK(msr == AC_MSR_CTS && ev == (AC_EV_BREAK | AC_EV_ERR));
    CHECK(er == (AC_CE_BREAK | AC_CE_FRAME | AC_CE_RXPARITY | AC_CE_OVERRUN));
    CHECK(AcMsrToModemStatus(AC_MSR_CTS | AC_MSR_DCD | AC_MSR_DDCD) == (AC_MS_CTS_ON | AC_MS_RLSD_ON));
    CHECK(AcMsrToModemStatus(AC_MSR_DSR | AC_MSR_RI) == (AC_MS_DSR_ON | AC_MS_RING_ON));
}

typedef struct QS { AC_U32 addr, size, count, get, put; AC_QUEUE q; AC_U8 buf[16]; } QS;

static void QInit(QS *s, AC_U32 size)
{
    s->addr = (AC_U32)s->buf; s->size = size; s->count = s->get = s->put = 0;
    s->q.Addr = &s->addr; s->q.Size = &s->size; s->q.Count = &s->count;
    s->q.Get = &s->get; s->q.Put = &s->put;
}

static void TestQueues(void)
{
    QS rx, tx;
    AC_DCB d;
    AC_FLOW fl;
    AC_U32 ev, act, n;
    AC_U8 out[32];
    const AC_U8 data[] = "ABCDEFGHIJ";

    memset(&fl, 0, sizeof(fl));
    AcDefaultDcb(&d, 8);
    QInit(&rx, 8);
    ev = act = 0;
    n = AcRxPut(&rx.q, &d, &fl, data, 10, &ev, &act);
    CHECK(n == 8 && rx.count == 8 && rx.put == 0 && ev == AC_EV_RXCHAR && act == 0);
    n = AcRxGet(&rx.q, &d, &fl, out, 3, &act);
    CHECK(n == 3 && memcmp(out, "ABC", 3) == 0 && rx.count == 5 && rx.get == 3);
    n = AcRxPut(&rx.q, &d, &fl, data + 8, 2, &ev, &act);   /* wraps */
    CHECK(n == 2 && rx.put == 2 && rx.count == 7);
    n = AcRxGet(&rx.q, &d, &fl, out, 32, &act);
    CHECK(n == 7 && memcmp(out, "DEFGHIJ", 7) == 0 && rx.count == 0);
    CHECK(AcQueuePeek(&rx.q, out) == 0);

    /* Event char, null strip. */
    d.EvtChar1 = 'X'; d.BitMask |= AC_fNullStrip;
    ev = 0;
    n = AcRxPut(&rx.q, &d, &fl, (const AC_U8 *)"a\0X", 3, &ev, &act);
    CHECK(n == 3 && rx.count == 2 && (ev & AC_EV_RXFLAG));
    CHECK(AcQueuePeek(&rx.q, out) == 1 && out[0] == 'a');
    AcQueueClear(&rx.q);
    CHECK(rx.count == 0 && rx.get == 0 && rx.put == 0);

    /* Output XON/XOFF: XOFF holds the queue, XON releases and asks for TX. */
    d.BitMask = AC_fBinary | AC_fOutX;
    QInit(&tx, 16);
    CHECK(AcTxPut(&tx.q, (const AC_U8 *)"hello", 5) == 5);
    ev = act = 0;
    n = AcRxPut(&rx.q, &d, &fl, (const AC_U8 *)"\x13z", 2, &ev, &act);
    CHECK(n == 2 && rx.count == 1 && (fl.Status & AC_fXoffHold));
    CHECK(AcTxPull(&tx.q, &fl, out, 32) == 0 && tx.count == 5);
    fl.Immediate = '!'; fl.Status |= AC_fTximmed;
    CHECK(AcTxPull(&tx.q, &fl, out, 32) == 1 && out[0] == '!');   /* immediate ignores hold */
    n = AcRxPut(&rx.q, &d, &fl, (const AC_U8 *)"\x11", 1, &ev, &act);
    CHECK(n == 1 && !(fl.Status & AC_fXoffHold) && (act & AC_RX_KICK_TX));
    n = AcTxPull(&tx.q, &fl, out, 3);
    CHECK(n == 3 && memcmp(out, "hel", 3) == 0);
    CHECK(AcTxPull(&tx.q, &fl, out, 0) == 0);
    n = AcTxPull(&tx.q, &fl, out, 32);
    CHECK(n == 2 && memcmp(out, "lo", 2) == 0 && tx.count == 0);

    /* Input XON/XOFF: XOFF owed when free space < XoffLim, XON when count < XonLim. */
    AcQueueClear(&rx.q);
    memset(&fl, 0, sizeof(fl));
    d.BitMask = AC_fBinary | AC_fInX; d.XoffLim = 3; d.XonLim = 2;
    act = 0;
    AcRxPut(&rx.q, &d, &fl, (const AC_U8 *)"12345", 5, &ev, &act);
    CHECK(!(fl.Status & AC_fXoffSent) && act == 0);           /* 3 free: not < 3 */
    AcRxPut(&rx.q, &d, &fl, (const AC_U8 *)"6", 1, &ev, &act);
    CHECK((fl.Status & AC_fXoffSent) && fl.FlowPending && fl.FlowChar == 0x13 && (act & AC_RX_KICK_TX));
    CHECK(AcTxPut(&tx.q, (const AC_U8 *)"q", 1) == 1);
    fl.Immediate = '!'; fl.Status |= AC_fTximmed;
    n = AcTxPull(&tx.q, &fl, out, 32);
    CHECK(n == 3 && out[0] == 0x13 && out[1] == '!' && out[2] == 'q' && !fl.FlowPending);
    act = 0;
    AcRxGet(&rx.q, &d, &fl, out, 4, &act);                    /* 2 left: not < 2 */
    CHECK((fl.Status & AC_fXoffSent) && act == 0);
    AcRxGet(&rx.q, &d, &fl, out, 1, &act);
    CHECK(!(fl.Status & AC_fXoffSent) && fl.FlowPending && fl.FlowChar == 0x11 && (act & AC_RX_KICK_TX));

    /* A bad queue is refused, not written. */
    rx.addr = 0;
    CHECK(AcRxPut(&rx.q, &d, &fl, data, 3, &ev, &act) == 0);
    rx.addr = (AC_U32)rx.buf; rx.get = 99;
    CHECK(AcRxGet(&rx.q, &d, &fl, out, 3, &act) == 0 && AcTxPut(&rx.q, data, 1) == 0);
}

static void TestPortNames(void)
{
    AC_U8 used[32];
    char s[8];

    CHECK(AcParsePortName("COM3", 4) == 3 && AcParsePortName("com255", 6) == 255);
    CHECK(AcParsePortName("Com12", 5) == 12);
    CHECK(AcParsePortName("COM0", 4) == 0 && AcParsePortName("COM03", 5) == 0);
    CHECK(AcParsePortName("COM256", 6) == 0 && AcParsePortName("LPT1", 4) == 0);
    CHECK(AcParsePortName("COM", 3) == 0 && AcParsePortName("COM1x", 5) == 0);
    CHECK(AcParsePortName("COM1000", 7) == 0);
    CHECK(AcFormatPortName(3, s) == 4 && strcmp(s, "COM3") == 0);
    CHECK(AcFormatPortName(42, s) == 5 && strcmp(s, "COM42") == 0);
    CHECK(AcFormatPortName(255, s) == 6 && strcmp(s, "COM255") == 0);
    CHECK(AcFormatPortName(100, s) == 6 && strcmp(s, "COM100") == 0);

    memset(used, 0, sizeof(used));
    used[0] = (1 << 1) | (1 << 2) | (1 << 3);                  /* COM1-3 used */
    CHECK(AcChoosePort(used, 0, 3) == 4);
    CHECK(AcChoosePort(used, 7, 3) == 7);                      /* remembered */
    CHECK(AcChoosePort(used, 3, 3) == 4);                      /* remembered taken */
    CHECK(AcChoosePort(used, 0, 5) == 5);
    CHECK(AcChoosePort(used, 300, 3) == 4);
    memset(used, 0xFF, sizeof(used));
    CHECK(AcChoosePort(used, 9, 3) == 0);
    used[31] &= 0x7F;                                          /* COM255 free */
    CHECK(AcChoosePort(used, 0, 3) == 255);
}

static int FmtIs(const char *want, const char *fmt, AC_U32 a, AC_U32 b, int n, AC_U32 cap)
{
    char buf[64];
    AC_U32 v[2], len;
    memset(buf, 'Z', sizeof(buf));
    v[0] = a; v[1] = b;
    len = AcFormat(buf, cap, fmt, v, n);
    return len == strlen(want) && strcmp(buf, want) == 0 && buf[cap] == 'Z';
}

static void TestFormat(void)
{
    static const AC_U16 wide[] = { 'C', 'O', 'M', '3', 0x263A, 0 };
    CHECK(FmtIs("port 3 is COM12", "port %lu is COM%lu", 3, 12, 2, 40));
    CHECK(FmtIs("st c000000d", "st %08lx", 0xC000000D, 0, 1, 40));
    CHECK(FmtIs("st 0000002a", "st %08lx", 0x2A, 0, 1, 40));
    CHECK(FmtIs("[  -5|7F]", "[%4d|%X]", (AC_U32)-5, 127, 2, 40));
    CHECK(FmtIs("[-005]", "[%04d]", (AC_U32)-5, 0, 1, 40));
    CHECK(FmtIs("a=COM5 b=0", "a=%s b=%u", (AC_U32)"COM5", 0, 1, 40));
    CHECK(FmtIs("(null)", "%s", 0, 0, 1, 40));
    CHECK(FmtIs("COM3?", "%w", (AC_U32)wide, 0, 1, 40));
    CHECK(FmtIs("100% x", "100%% %c", 'x', 0, 1, 40));
    CHECK(FmtIs("abcd", "abcdefgh", 0, 0, 0, 5));      /* truncated, NUL kept */
    CHECK(FmtIs("ff", "%x", 255, 0, 1, 3));
    CHECK(FmtIs("trail", "trail%", 0, 0, 0, 40));
    CHECK(FmtIs("", "x", 0, 0, 0, 1));
}

int main(void)
{
    GuardInit();
    TestDescriptors();
    TestFuzz();
    TestLine();
    TestNotify();
    TestQueues();
    TestPortNames();
    TestFormat();
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

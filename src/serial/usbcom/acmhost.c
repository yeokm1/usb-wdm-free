/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * acmhost.c - host tests for acmcore.c and chipcore.c, built with the package's own
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
#include "chipcore.h"

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

/* ---- chip modules (chipcore.c, design 6.3) ------------------------------ */

/*
 * The expected requests below are written by hand from the facts design
 * 6.3 lists (Linux v6.12 drivers/usb/serial; facts only). The FTDI divisor
 * values are the ones the FTDI divisor rule gives (3 MHz / divisor with
 * eighths; 12 MHz for H parts with bit 17).
 */

static void Dev(AC_U8 d[18], AC_U16 vid, AC_U16 pid, AC_U16 bcdUsb, AC_U16 bcdDev,
                AC_U8 cls, AC_U8 mp0, AC_U8 serial)
{
    memset(d, 0, 18);
    d[0] = 18; d[1] = 1;
    d[2] = (AC_U8)bcdUsb; d[3] = (AC_U8)(bcdUsb >> 8);
    d[4] = cls; d[7] = mp0;
    d[8] = (AC_U8)vid; d[9] = (AC_U8)(vid >> 8);
    d[10] = (AC_U8)pid; d[11] = (AC_U8)(pid >> 8);
    d[12] = (AC_U8)bcdDev; d[13] = (AC_U8)(bcdDev >> 8);
    d[16] = serial;
}

/* AcChipRx with the newest state of the transfer (the tests of one change). */
static AC_U32 Rx1(AC_CHIP *c, AC_U8 *b, AC_U32 n, AC_U16 *st, int *rep)
{
    AC_U16 states[64];
    int ns;
    AC_U32 r = AcChipRx(c, b, n, states, &ns, 64);
    *rep = ns;
    *st = (AC_U16)(ns ? states[ns - 1] : 0);
    return r;
}

/* Runs one step; returns the result and checks nothing. */
static int Step(AC_CHIP *c, AC_U32 op, AC_U32 value, const AC_U8 *data, AC_U32 *step,
                int ok, const AC_U8 *in, AC_U32 inLen, AC_SETUP *s, AC_U16 *st)
{
    memset(s, 0xCC, sizeof(*s));
    return AcChipStep(c, op, value, data, step, ok, in, inLen, s, st);
}

static int Is(const AC_SETUP *s, AC_U8 type, AC_U8 req, AC_U16 value, AC_U16 index, AC_U16 len)
{
    return s->Type == type && s->Request == req && s->Value == value && s->Index == index &&
           s->Length == len;
}

static void Lc7(AC_U8 lc[8], AC_U32 rate, AC_U8 stop, AC_U8 par, AC_U8 bits)
{
    memset(lc, 0, 8);
    AcEncodeLineCoding(rate, stop, par, bits, lc);
}

static void TestChipIds(void)
{
    CHECK(AcChipFamily(0x0403, 0x6015) == AC_FAM_FTDI);
    CHECK(AcChipFamily(0x0403, 0x6011) == AC_FAM_FTDI);
    CHECK(AcChipFamily(0x0403, 0x6001) == AC_FAM_FTDI);
    CHECK(AcChipFamily(0x067B, 0x2303) == AC_FAM_PL2303);
    CHECK(AcChipFamily(0x067B, 0x23F3) == AC_FAM_PL2303);
    CHECK(AcChipFamily(0x10C4, 0xEA70) == AC_FAM_CP210X);
    CHECK(AcChipFamily(0x10C4, 0xEA71) == AC_FAM_CP210X);
    CHECK(AcChipFamily(0x2341, 0x0043) == AC_FAM_CDC);
    CHECK(AcChipFamily(0x0403, 0x6016) == AC_FAM_CDC);
}

static void TestFtdi(void)
{
    AC_CHIP c;
    AC_SETUP s;
    AC_U8 d[18], lc[8], rx[200];
    AC_U32 step, n, i;
    AC_U16 st = 0;
    int r, rep;

    /* Divisors: 3 MHz / (n + eighths), the sub-divisor codes, the two
       special values; the H parts' 12 MHz form with bit 17. */
    CHECK(AcFtdi232bmDivisor(9600, 48000000) == 0x4138);       /* 312.5 */
    CHECK(AcFtdi232bmDivisor(115200, 48000000) == 0x001A);     /* 26 */
    CHECK(AcFtdi232bmDivisor(300, 48000000) == 0x2710);        /* 10000 */
    CHECK(AcFtdi232bmDivisor(3000000, 48000000) == 0);
    CHECK(AcFtdi232bmDivisor(2000000, 48000000) == 1);
    CHECK(AcFtdi232bmDivisor(57600, 48000000) == 0xC034);      /* 52.125 */
    CHECK(AcFtdi232bmDivisor(38400, 48000000) == 0xC04E);      /* 78.125 */
    CHECK(AcFtdi2232hDivisor(115200, 120000000) == 0x2C068);   /* 104.125, 12 MHz */
    CHECK(AcFtdi2232hDivisor(12000000, 120000000) == 0x20000);
    CHECK(AcFtdi232amDivisor(9600, 48000000) == 0x4138);       /* .5 is 0x4000 on the AM */

    /* FT-X, one port: channel 1. */
    Dev(d, 0x0403, 0x6015, 0x0200, 0x1000, 0, 64, 3);
    AcChipSetup(&c, AC_FAM_FTDI, d, 0, 64);
    CHECK(c.Part == AC_FT_X && c.Channel == 1);
    CHECK(c.Limits.MaxBaud == 3000000 && c.Limits.MinBaud == 184);
    CHECK(c.Limits.DataBits == ((1UL << 7) | (1UL << 8)) && c.Limits.StopBits == 0x05);
    step = 0;
    CHECK(Step(&c, AC_OP_INIT, 0, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    step = 0;
    r = Step(&c, AC_OP_OPEN, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(r == AC_STEP_SEND && Is(&s, 0x40, 0, 0, 1, 0));
    Lc7(lc, 9600, 0, 0, 8);
    step = 0;
    r = Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(r == AC_STEP_SEND && Is(&s, 0x40, 4, 0x0008, 1, 0));
    step = 1;
    r = Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(r == AC_STEP_SEND && Is(&s, 0x40, 3, 0x4138, 0x0001, 0));
    step = 2;
    CHECK(Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    Lc7(lc, 19200, 2, 2, 7);                         /* 7E2 */
    step = 0;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 4, 0x1207, 1, 0));
    step = 0;
    r = Step(&c, UCOP_CONTROL, 3, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(r == AC_STEP_SEND && Is(&s, 0x40, 1, 0x0303, 1, 0));
    step = 0;
    Step(&c, UCOP_CONTROL, 1, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 1, 0x0301, 1, 0));
    step = 0;
    Step(&c, UCOP_BREAK, 0xFFFF, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 4, 0x5207, 1, 0));            /* 7E2 + break */
    step = 0;
    Step(&c, UCOP_BREAK, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 4, 0x1207, 1, 0));
    /* Flow: one mode for both directions. */
    step = 0;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS | UCIF_FLOW_RTS, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 2, 0, 0x0101, 0));
    step = 0;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 2, 0, 0x0001, 0));
    step = 1;                                        /* off: the lines again */
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 1, 0x0301, 1, 0));
    step = 2;
    CHECK(Step(&c, UCOP_FLOW, UCIF_FLOW_CTS, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    step = 0;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS | UCIF_FLOW_RTS, NULL, &step, 1, NULL, 0, &s, &st);
    step = 1;                                        /* on: the chip drives RTS */
    CHECK(Step(&c, UCOP_FLOW, 3, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    /* Close: flow off, DTR and RTS low, no break. */
    step = 0;
    Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 2, 0, 1, 0));
    step = 1;
    Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 1, 0x0300, 1, 0));
    step = 2;
    Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 4, 0x1207, 1, 0));
    step = 3;
    CHECK(Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);

    /* FT4232H, third interface: channel 3; the H divisor's high bits. */
    Dev(d, 0x0403, 0x6011, 0x0200, 0x0800, 0, 64, 0);
    AcChipSetup(&c, AC_FAM_FTDI, d, 2, 512);
    CHECK(c.Part == AC_FT_4232H && c.Channel == 3 && c.Limits.MaxBaud == 12000000);
    Lc7(lc, 115200, 0, 0, 8);
    step = 1;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 3, 0xC068, 0x0203, 0));
    Lc7(lc, 300, 0, 0, 8);                           /* below 1200: the 3 MHz form */
    step = 1;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 3, 0x2710, 0x0003, 0));

    /* FT232BM (QEMU's model): channel 0, the divisor's high bits alone. */
    Dev(d, 0x0403, 0x6001, 0x0200, 0x0400, 0, 8, 0);
    AcChipSetup(&c, AC_FAM_FTDI, d, 0, 64);
    CHECK(c.Part == AC_FT_BM && c.Channel == 0);
    Lc7(lc, 38400, 0, 0, 8);
    step = 1;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 3, 0xC04E, 0x0000, 0));
    /* 0x0200 without a serial number: the latency timer decides. */
    Dev(d, 0x0403, 0x6001, 0x0200, 0x0200, 0, 8, 0);
    AcChipSetup(&c, AC_FAM_FTDI, d, 0, 64);
    CHECK(c.Part == AC_FT_AM);
    step = 0;
    CHECK(Step(&c, AC_OP_INIT, 0, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_SEND &&
          Is(&s, 0xC0, 0x0A, 0, 0, 1));
    rx[0] = 16;
    step = 1;
    CHECK(Step(&c, AC_OP_INIT, 0, NULL, &step, 1, rx, 1, &s, &st) == AC_STEP_DONE && c.Part == AC_FT_BM);
    AcChipSetup(&c, AC_FAM_FTDI, d, 0, 64);
    step = 1;
    Step(&c, AC_OP_INIT, 0, NULL, &step, 0, NULL, 0, &s, &st);
    CHECK(c.Part == AC_FT_AM);
    Dev(d, 0x0403, 0x6001, 0x0200, 0x0200, 0, 8, 3);  /* with a serial number: AM */
    AcChipSetup(&c, AC_FAM_FTDI, d, 0, 64);
    step = 0;
    CHECK(Step(&c, AC_OP_INIT, 0, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE && c.Part == AC_FT_AM);

    /* Receive framing: two status bytes per 64-byte packet stripped. */
    Dev(d, 0x0403, 0x6015, 0x0200, 0x1000, 0, 64, 3);
    AcChipSetup(&c, AC_FAM_FTDI, d, 0, 64);
    rx[0] = 0x31; rx[1] = 0x60;                      /* CTS, DSR */
    for (i = 0; i < 62; i++) rx[2 + i] = (AC_U8)('a' + i % 26);
    rx[64] = 0x31; rx[65] = 0x60; rx[66] = 'Z';
    n = Rx1(&c, rx, 67, &st, &rep);
    CHECK(n == 63 && rx[0] == 'a' && rx[61] == (AC_U8)('a' + 61 % 26) && rx[62] == 'Z');
    CHECK(rep && st == AC_SS_DSR);
    rx[0] = 0x31; rx[1] = 0x60; rx[2] = 'q';
    n = Rx1(&c, rx, 3, &st, &rep);
    CHECK(n == 1 && rx[0] == 'q' && !rep);          /* no change */
    rx[0] = 0x01; rx[1] = 0x60;                      /* status only, CTS and DSR off */
    n = Rx1(&c, rx, 2, &st, &rep);
    CHECK(n == 0 && rep && st == AC_SS_NOCTS);
    rx[0] = 0x91; rx[1] = 0x6C; rx[2] = 'x';          /* CTS DCD; parity, framing */
    n = Rx1(&c, rx, 3, &st, &rep);
    CHECK(n == 1 && rep && st == (AC_SS_DCD | AC_SS_PARITY | AC_SS_FRAMING));
    rx[0] = 0x91; rx[1] = 0x72; rx[2] = 0;            /* break with its NUL, overrun */
    n = Rx1(&c, rx, 3, &st, &rep);
    CHECK(n == 1 && rep && st == (AC_SS_DCD | AC_SS_BREAK | AC_SS_OVERRUN));
    rx[0] = 0x91; rx[1] = 0x70; rx[2] = 'y';          /* BI without a NUL: no break */
    n = Rx1(&c, rx, 3, &st, &rep);
    CHECK(n == 1 && !rep);
    rx[0] = 0x91; rx[1] = 0x7E;                       /* errors in a status-only packet */
    n = Rx1(&c, rx, 2, &st, &rep);
    CHECK(n == 0 && !rep);
    rx[0] = 0x91;                                     /* a 1-byte packet is dropped */
    n = Rx1(&c, rx, 1, &st, &rep);
    CHECK(n == 0 && !rep);
    /* A fall and rise of CTS within one transfer: both passed on, in order. */
    {
        AC_U16 states[4];
        int ns;
        rx[0] = 0x91; rx[1] = 0x60; rx[2] = 'a';
        for (i = 3; i < 64; i++) rx[i] = 'b';
        rx[64] = 0x81; rx[65] = 0x60; rx[66] = 'c';             /* CTS off */
        rx[67] = 0x91; rx[68] = 0x60;                             /* the next packet, CTS on */
        n = AcChipRx(&c, rx, 64, states, &ns, 4);
        n = AcChipRx(&c, rx + 64, 3, states, &ns, 4);
        CHECK(ns == 1 && states[0] == (AC_SS_DCD | AC_SS_NOCTS));
        rx[0] = 0x81; rx[1] = 0x60; rx[2] = 'x';
        for (i = 3; i < 64; i++) rx[i] = 'y';
        rx[64] = 0x91; rx[65] = 0x60; rx[66] = 'z';
        rx[67] = 0x81; rx[68] = 0x60; rx[69] = 'w';
        AcChipSetup(&c, AC_FAM_FTDI, d, 0, 64);
        n = AcChipRx(&c, rx, 64 + 3, states, &ns, 4);
        CHECK(ns == 2 && states[0] == (AC_SS_DCD | AC_SS_NOCTS) && states[1] == AC_SS_DCD && n == 63);
        /* More changes than room: the last slot holds the newest. */
        AcChipSetup(&c, AC_FAM_FTDI, d, 0, 3);
        rx[0] = 0x91; rx[1] = 0x60; rx[2] = 'q';
        n = AcChipRx(&c, rx, 3, states, &ns, 1);
        CHECK(ns == 1 && states[0] == AC_SS_DCD && n == 1);
        rx[0] = 0x81; rx[1] = 0x60; rx[2] = 'a'; rx[3] = 0x91; rx[4] = 0x60; rx[5] = 'b';
        rx[6] = 0x81; rx[7] = 0x60; rx[8] = 'c';
        n = AcChipRx(&c, rx, 9, states, &ns, 1);
        CHECK(ns == 1 && states[0] == (AC_SS_DCD | AC_SS_NOCTS) && n == 3);
    }
    /* Stripping alone leaves the state untouched. */
    AcChipSetup(&c, AC_FAM_FTDI, d, 0, 64);
    {
        AC_U16 last = c.LastState;
        AC_U8 fl = c.Flags;
        rx[0] = 0x01; rx[1] = 0x60; rx[2] = 'k'; rx[3] = 'l';
        n = AcChipStrip(&c, rx, 4);
        CHECK(n == 2 && rx[0] == 'k' && rx[1] == 'l' && c.LastState == last && c.Flags == fl);
    }
    /* High Speed: 512-byte packets. */
    AcChipSetup(&c, AC_FAM_FTDI, d, 0, 512);
    memset(rx, 0, sizeof(rx));
    rx[0] = 0x11; rx[1] = 0x60;
    for (i = 2; i < 100; i++) rx[i] = (AC_U8)i;
    n = Rx1(&c, rx, 100, &st, &rep);
    CHECK(n == 98 && rx[0] == 2 && rx[97] == 99);
}

static void TestPl2303(void)
{
    AC_CHIP c;
    AC_SETUP s;
    AC_U8 d[18], lc[8], in[2], buf[10];
    AC_U32 step, off;
    AC_U16 st = 0;
    int r, i;

    /* HX (USB 1.1, 0x0300): the eleven-step start; wValue register, wIndex value. */
    Dev(d, 0x067B, 0x2303, 0x0110, 0x0300, 0, 64, 0);
    AcChipSetup(&c, AC_FAM_PL2303, d, 0, 64);
    CHECK(c.Part == AC_PL_HX && c.Limits.MaxBaud == 6000000);
    CHECK(c.Limits.StopBits == 0x07 && c.Limits.DataBits == 0x1E0);
    step = 0;
    r = Step(&c, AC_OP_INIT, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(r == AC_STEP_SEND && step == 1 && Is(&s, 0xC0, 0x01, 0x8484, 0, 1));
    {
        /* The whole start sequence (Linux pl2303_startup): reads as
           (0xC0, 0x01, register, 0, 1), writes as (0x40, 0x01, register, value). */
        static const unsigned short seq[11][3] = {
            { 0xC0, 0x8484, 0 }, { 0x40, 0x0404, 0 }, { 0xC0, 0x8484, 0 }, { 0xC0, 0x8383, 0 },
            { 0xC0, 0x8484, 0 }, { 0x40, 0x0404, 1 }, { 0xC0, 0x8484, 0 }, { 0xC0, 0x8383, 0 },
            { 0x40, 0x0000, 1 }, { 0x40, 0x0001, 0 }, { 0x40, 0x0002, 0x44 } };
        for (i = 1; i <= 11; i++) {
            step = (AC_U32)i;
            r = Step(&c, AC_OP_INIT, 0, NULL, &step, 1, in, 1, &s, &st);
            CHECK(r == AC_STEP_SEND && step == (AC_U32)i);
            CHECK(Is(&s, (AC_U8)seq[i - 1][0], 0x01, seq[i - 1][1], seq[i - 1][2],
                     (AC_U16)(seq[i - 1][0] == 0xC0 ? 1 : 0)));
        }
    }
    step = 12;
    CHECK(Step(&c, AC_OP_INIT, 0, NULL, &step, 1, in, 1, &s, &st) == AC_STEP_DONE);
    step = 0;
    Step(&c, AC_OP_OPEN, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 0x01, 8, 0, 0));
    step = 1;
    Step(&c, AC_OP_OPEN, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 0x01, 9, 0, 0));
    /* Line: the CDC layout, a direct rate and a divisor one. */
    Lc7(lc, 9600, 1, 3, 7);
    step = 0;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x21, 0x20, 0, 0, 7) && s.Data[0] == 0x80 && s.Data[1] == 0x25 && s.Data[2] == 0 &&
          s.Data[3] == 0 && s.Data[4] == 1 && s.Data[5] == 3 && s.Data[6] == 7);
    Lc7(lc, 100000, 0, 0, 8);
    step = 0;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(s.Data[0] == 240 && s.Data[1] == 4 && s.Data[2] == 0 && s.Data[3] == 0x80);
    step = 0;
    Step(&c, UCOP_CONTROL, 2, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x21, 0x22, 2, 0, 0));
    step = 0;
    Step(&c, UCOP_BREAK, 0xFFFF, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x21, 0x23, 0xFFFF, 0, 0));
    /* Flow: read register 0 as 0x80, write it back with 0x60. */
    step = 0;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS | UCIF_FLOW_RTS, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0xC0, 0x01, 0x80, 0, 1));
    in[0] = 0x01;
    step = 1;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS | UCIF_FLOW_RTS, NULL, &step, 1, in, 1, &s, &st);
    CHECK(Is(&s, 0x40, 0x01, 0, 0x61, 0));
    in[0] = 0x61;                                    /* one direction only: off */
    step = 0;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS, NULL, &step, 1, NULL, 0, &s, &st);
    step = 1;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS, NULL, &step, 1, in, 1, &s, &st);
    CHECK(Is(&s, 0x40, 0x01, 0, 0x01, 0));
    step = 1;                                        /* a failed read writes nothing */
    CHECK(Step(&c, UCOP_FLOW, UCIF_FLOW_CTS, NULL, &step, 0, NULL, 0, &s, &st) == AC_STEP_DONE);
    step = 0;
    Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x21, 0x23, 0, 0, 0));
    step = 1;
    Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x21, 0x22, 0, 0, 0));
    step = 2;
    CHECK(Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    CHECK(!AcChipResetsPipes(&c) && !AcChipPolls(&c) && AcChipBreak(&c));
    /* The class requests go to wIndex 0 even when the bulk endpoints are
       on interface 1 (the endpoint hack layout). */
    AcChipSetup(&c, AC_FAM_PL2303, d, 1, 64);
    Lc7(lc, 9600, 0, 0, 8);
    step = 0;
    Step(&c, AC_OP_OPEN, 0, NULL, &step, 1, NULL, 0, &s, &st);
    step = 0;
    CHECK(Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st) == AC_STEP_SEND &&
          Is(&s, 0x21, 0x20, 0, 0, 7));
    step = 1;
    CHECK(Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    /* The same line again is not sent (the part loses bytes on a rewrite),
       nor a rate that encodes the same; a new open sends it afresh. */
    step = 0;
    CHECK(Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    Lc7(lc, 100100, 0, 0, 8);
    step = 0;
    CHECK(Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st) == AC_STEP_SEND);
    step = 1;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    Lc7(lc, 100200, 0, 0, 8);
    step = 0;
    CHECK(Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    Lc7(lc, 100200, 0, 0, 7);
    step = 0;
    CHECK(Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st) == AC_STEP_SEND);
    step = 1;                                        /* a failed write is not remembered */
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 0, NULL, 0, &s, &st);
    step = 0;
    CHECK(Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st) == AC_STEP_SEND);
    step = 0;
    Step(&c, UCOP_CONTROL, 3, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x21, 0x22, 3, 0, 0));
    step = 0;
    Step(&c, UCOP_BREAK, 1, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x21, 0x23, 0xFFFF, 0, 0));
    /* The rate field: every rate the part might be given decodes back
       within 3 % (the divisor's step at the top of its range). */
    {
        AC_U32 r, dec, m, e;
        AC_U8 f[4];
        int bad = 0;
        for (r = 75; r <= 6000000 && !bad; r += r / 97 + 1) {
            AcPl2303Rate(&c, r, f);
            if (!(f[3] & 0x80)) {
                dec = f[0] | ((AC_U32)f[1] << 8) | ((AC_U32)f[2] << 16);
            } else {
                m = f[0] | ((AC_U32)(f[1] & 1) << 8);
                e = (f[1] >> 1) & 7;
                dec = (384000000UL / m) >> (2 * e);
            }
            if (dec * 100 < r * 97 || dec * 100 > r * 103) { bad = 1; printf("  PL2303 rate %lu -> %lu\n", r, dec); }
        }
        CHECK(!bad);
    }
    /* The interrupt transfer's UART state. */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xA1; buf[1] = 0x20; buf[8] = 0x83;      /* DCD DSR CTS */
    off = 0;
    CHECK(AcChipNotify(&c, buf, 10, &off, &st) == 1 && st == (AC_SS_DCD | AC_SS_DSR));
    CHECK(AcChipNotify(&c, buf, 10, &off, &st) == 0);
    buf[8] = 0x14;                                    /* break, framing; CTS off */
    off = 0;
    CHECK(AcChipNotify(&c, buf, 10, &off, &st) == 1 && st == (AC_SS_BREAK | AC_SS_FRAMING | AC_SS_NOCTS));
    off = 0;
    CHECK(AcChipNotify(&c, buf, 8, &off, &st) == 0);

    /* TA (USB 2.0, 0x0300, register 0x8080 reads): the other divisor form. */
    Dev(d, 0x067B, 0x2303, 0x0200, 0x0300, 0, 64, 0);
    AcChipSetup(&c, AC_FAM_PL2303, d, 0, 64);
    CHECK(c.Part == AC_PL_PROBE);
    step = 0;
    Step(&c, AC_OP_INIT, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(step == 0 && Is(&s, 0xC0, 0x01, 0x8080, 0, 1));
    step = 1;
    r = Step(&c, AC_OP_INIT, 0, NULL, &step, 1, in, 1, &s, &st);
    CHECK(c.Part == AC_PL_TA && r == AC_STEP_SEND && Is(&s, 0xC0, 0x01, 0x8484, 0, 1));
    {
        AC_U32 r, dec, m, e;
        AC_U8 f[4];
        int bad = 0;
        for (r = 46; r <= 6000000 && !bad; r += r / 89 + 1) {
            AcPl2303Rate(&c, r, f);
            if (f[3] & 0x80) {
                m = f[0] | ((AC_U32)(f[1] & 7) << 8);
                e = ((f[1] >> 4) & 0x0E) | (f[2] & 1);
                dec = (384000000UL / m) >> e;
                if (dec * 100 < r * 98 || dec * 100 > r * 102) { bad = 1; printf("  TA rate %lu -> %lu\n", r, dec); }
            }
        }
        CHECK(!bad);
    }
    Lc7(lc, 100000, 0, 0, 8);
    step = 0;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(s.Data[0] == 0x80 && s.Data[1] == 0x07 && s.Data[2] == 1 && s.Data[3] == 0x80);
    /* The same part whose 0x8080 read fails is a G (GT). */
    AcChipSetup(&c, AC_FAM_PL2303, d, 0, 64);
    step = 1;
    CHECK(Step(&c, AC_OP_INIT, 0, NULL, &step, 0, NULL, 0, &s, &st) == AC_STEP_DONE && c.Part == AC_PL_G);

    /* G series (GC): no start sequence, requests 0x80/0x81, direct rates. */
    Dev(d, 0x067B, 0x23A3, 0x0200, 0x0100, 0, 64, 0);
    AcChipSetup(&c, AC_FAM_PL2303, d, 0, 64);
    CHECK(c.Part == AC_PL_G && c.Limits.MaxBaud == 12000000);
    step = 0;
    CHECK(Step(&c, AC_OP_INIT, 0, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    step = 0;
    Step(&c, AC_OP_OPEN, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x40, 0x80, 7, 3, 0));
    step = 1;
    CHECK(Step(&c, AC_OP_OPEN, 0, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    Lc7(lc, 100000, 0, 0, 8);
    step = 0;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(s.Data[0] == 0xA0 && s.Data[1] == 0x86 && s.Data[2] == 0x01 && s.Data[3] == 0);
    step = 0;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS | UCIF_FLOW_RTS, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0xC0, 0x81, 0x0A, 0, 1));
    in[0] = 0x1C;
    step = 1;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS | UCIF_FLOW_RTS, NULL, &step, 1, in, 1, &s, &st);
    CHECK(Is(&s, 0x40, 0x80, 0x0A, 0x18, 0));

    /* The H (class 02 or an 8-byte EP0): the pipes are reset at open. */
    Dev(d, 0x067B, 0x2303, 0x0110, 0x0202, 0, 8, 0);
    AcChipSetup(&c, AC_FAM_PL2303, d, 0, 64);
    CHECK(c.Part == AC_PL_H && c.Limits.MaxBaud == 1228800 && AcChipResetsPipes(&c));
    step = 11;
    Step(&c, AC_OP_INIT, 0, NULL, &step, 1, in, 1, &s, &st);
    CHECK(Is(&s, 0x40, 0x01, 0x0002, 0x24, 0));
    step = 0;
    CHECK(Step(&c, AC_OP_OPEN, 0, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
}

static void TestCp210x(void)
{
    AC_CHIP c;
    AC_SETUP s;
    AC_U8 d[18], lc[8], in[16];
    AC_U32 step;
    AC_U16 st = 0;
    int r;

    Dev(d, 0x10C4, 0xEA70, 0x0200, 0x0100, 0, 64, 0);
    /* CP2105, second interface: 8N1 / N-O-E from 2400 to 921600, no break. */
    AcChipSetup(&c, AC_FAM_CP210X, d, 1, 64);
    step = 0;
    r = Step(&c, AC_OP_INIT, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(r == AC_STEP_SEND && Is(&s, 0xC0, 0xFF, 0x370B, 1, 1));
    in[0] = AC_CP_2105;
    step = 1;
    CHECK(Step(&c, AC_OP_INIT, 0, NULL, &step, 1, in, 1, &s, &st) == AC_STEP_DONE);
    CHECK(c.Part == AC_CP_2105 && c.Limits.MinBaud == 2400 && c.Limits.MaxBaud == 921600);
    CHECK(c.Limits.DataBits == (1UL << 8) && c.Limits.StopBits == 1 && c.Limits.Parity == 7);
    CHECK(!AcChipBreak(&c) && AcChipPolls(&c));
    CHECK(AcChipRate(&c, 300) == 2400);
    CHECK(AcCheckLine(&c.Limits, 9600, 7, 0, 0) == AC_IE_BYTESIZE);
    CHECK(AcCheckLine(&c.Limits, 9600, 8, 3, 0) == AC_IE_DEFAULT);
    CHECK(AcCheckLine(&c.Limits, 1200, 8, 0, 0) == AC_IE_BAUDRATE);
    CHECK(AcCheckLine(&c.Limits, 115200, 8, 2, 0) == 0);

    /* CP2105, first interface: the actual-rate rule, 5/6 bits to 921600. */
    AcChipSetup(&c, AC_FAM_CP210X, d, 0, 64);
    step = 1;
    Step(&c, AC_OP_INIT, 0, NULL, &step, 1, in, 1, &s, &st);
    CHECK(c.Limits.MaxBaud == 2000000 && c.Limits.Max56Baud == 921600 && AcChipBreak(&c));
    CHECK(AcCheckLine(&c.Limits, 1000000, 6, 0, 0) == AC_IE_BAUDRATE);
    CHECK(AcCheckLine(&c.Limits, 921600, 5, 0, 2) == 0);
    CHECK(AcCheckLine(&c.Limits, 1000000, 8, 0, 0) == 0);
    CHECK(AcCheckLine(&c.Limits, 9600, 8, 0, 1) == AC_IE_DEFAULT);   /* 1.5 refused */
    step = 0;
    r = Step(&c, AC_OP_OPEN, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(r == AC_STEP_SEND && Is(&s, 0x41, 0x00, 1, 0, 0));
    Lc7(lc, 115200, 2, 1, 7);
    step = 0;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x41, 0x1E, 0, 0, 4) && s.Data[0] == 0xB8 && s.Data[1] == 0xC2 && s.Data[2] == 0x01 &&
          s.Data[3] == 0);                           /* 48 MHz / (2 * 208) = 115384 */
    step = 1;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x41, 0x03, 0x0712, 0, 0));
    step = 0;
    Step(&c, UCOP_CONTROL, 3, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x41, 0x07, 0x0303, 0, 0));
    step = 1;
    CHECK(Step(&c, UCOP_CONTROL, 3, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    step = 0;
    Step(&c, UCOP_BREAK, 0xFFFF, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x41, 0x05, 1, 0, 0));
    /* Flow: the two directions in their own fields of the 16-byte block. */
    memset(in, 0, sizeof(in));
    in[0] = 0x51; in[4] = 0x43;                      /* DTR active + DSR bits; RTS active + auto Tx/Rx */
    step = 0;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0xC1, 0x14, 0, 0, 16));
    step = 1;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS, NULL, &step, 1, in, 16, &s, &st);
    CHECK(Is(&s, 0x41, 0x13, 0, 0, 16) && s.Data[0] == 0x09 && s.Data[4] == 0x40);
    step = 0;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS | UCIF_FLOW_RTS, NULL, &step, 1, NULL, 0, &s, &st);
    step = 1;
    Step(&c, UCOP_FLOW, UCIF_FLOW_CTS | UCIF_FLOW_RTS, NULL, &step, 1, in, 16, &s, &st);
    CHECK(s.Data[0] == 0x09 && s.Data[4] == 0x80);
    step = 0;
    Step(&c, UCOP_FLOW, UCIF_FLOW_RTS, NULL, &step, 1, NULL, 0, &s, &st);
    step = 1;
    Step(&c, UCOP_FLOW, UCIF_FLOW_RTS, NULL, &step, 1, in, 16, &s, &st);
    CHECK(s.Data[0] == 0x01 && s.Data[4] == 0x80);
    /* With RTS in flow mode the control lines go through SET_FLOW. */
    step = 0;
    Step(&c, UCOP_CONTROL, 1, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0xC1, 0x14, 0, 0, 16));
    in[0] = 0x09; in[4] = 0x80;
    step = 1;
    Step(&c, UCOP_CONTROL, 1, NULL, &step, 1, in, 16, &s, &st);
    CHECK(Is(&s, 0x41, 0x13, 0, 0, 16) && s.Data[0] == 0x09 && s.Data[4] == 0x00);   /* RTS low */
    /* Close: DTR and RTS inactive, no handshake (through the flow block),
       purge, disable; then poll. */
    in[0] = 0x09; in[4] = 0x83;
    step = 0;
    Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0xC1, 0x14, 0, 0, 16));
    step = 1;
    Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, in, 16, &s, &st);
    CHECK(Is(&s, 0x41, 0x13, 0, 0, 16) && s.Data[0] == 0x00 && s.Data[4] == 0x00);
    step = 2;
    Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(step == 2 && Is(&s, 0x41, 0x12, 0x000F, 0, 0));
    step = 1;                                        /* a failed GET_FLOW: on to the purge */
    Step(&c, AC_OP_CLOSE, 0, NULL, &step, 0, NULL, 0, &s, &st);
    CHECK(step == 2 && Is(&s, 0x41, 0x12, 0x000F, 0, 0));
    step = 3;
    Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x41, 0x00, 0, 0, 0));
    step = 4;
    CHECK(Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    step = 0;
    Step(&c, AC_OP_OPEN, 0, NULL, &step, 1, NULL, 0, &s, &st);   /* forgets the last state */
    step = 0;
    Step(&c, AC_OP_POLL, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0xC1, 0x08, 0, 0, 1));
    in[0] = 0xB3;                                    /* DCD DSR CTS, DTR RTS */
    step = 1;
    CHECK(Step(&c, AC_OP_POLL, 0, NULL, &step, 1, in, 1, &s, &st) == (AC_STEP_DONE | AC_STEP_STATE) &&
          st == (AC_SS_DCD | AC_SS_DSR));
    step = 1;
    CHECK(Step(&c, AC_OP_POLL, 0, NULL, &step, 1, in, 1, &s, &st) == AC_STEP_DONE);
    in[0] = 0x40;                                    /* RI, CTS off */
    step = 1;
    CHECK(Step(&c, AC_OP_POLL, 0, NULL, &step, 1, in, 1, &s, &st) == (AC_STEP_DONE | AC_STEP_STATE) &&
          st == (AC_SS_RING | AC_SS_NOCTS));
    step = 1;
    CHECK(Step(&c, AC_OP_POLL, 0, NULL, &step, 0, NULL, 0, &s, &st) == AC_STEP_DONE);

    /* CP2108: the AN205 table below 1 Mbit/s. */
    Dev(d, 0x10C4, 0xEA71, 0x0200, 0x0100, 0, 64, 0);
    AcChipSetup(&c, AC_FAM_CP210X, d, 3, 64);
    in[0] = AC_CP_2108;
    step = 1;
    Step(&c, AC_OP_INIT, 0, NULL, &step, 1, in, 1, &s, &st);
    CHECK(c.Limits.MaxBaud == 2000000 && c.Limits.MinBaud == 300 && c.Limits.Max56Baud == 0);
    CHECK(AcChipRate(&c, 115200) == 115200 && AcChipRate(&c, 117029) == 128000);
    CHECK(AcChipRate(&c, 4801) == 4800 && AcChipRate(&c, 1500000) == 1500000);
    CHECK(AcChipRate(&c, 921599) == 921600 && AcChipRate(&c, 3000000) == 2000000);
    Lc7(lc, 9600, 0, 0, 8);
    step = 1;
    Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x41, 0x03, 0x0800, 3, 0));
    /* A part number that does not read: the general limits. */
    AcChipSetup(&c, AC_FAM_CP210X, d, 0, 64);
    step = 1;
    Step(&c, AC_OP_INIT, 0, NULL, &step, 0, NULL, 0, &s, &st);
    CHECK(c.Part == 0 && c.Limits.MaxBaud == 2000000);
}

static void TestCdcSteps(void)
{
    AC_CHIP c;
    AC_SETUP s;
    AC_U8 d[18], lc[8], buf[16];
    AC_U32 step, off;
    AC_U16 st = 0, i;
    int rep;

    Dev(d, 0x2341, 0x0043, 0x0110, 0x0001, 2, 8, 0);
    AcChipSetup(&c, AC_FAM_CDC, d, 2, 64);
    CHECK(c.Limits.MinBaud == 1 && c.Limits.MaxBaud == 0xFFFFFFFFUL && (c.Limits.DataBits & (1UL << 16)));
    CHECK(AcCheckLine(&c.Limits, 300, 5, 4, 1) == 0 && AcCheckLine(&c.Limits, 300, 9, 0, 0) == AC_IE_BYTESIZE);
    Lc7(lc, 115200, 0, 0, 8);
    step = 0;
    CHECK(Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st) == AC_STEP_SEND &&
          Is(&s, 0x21, 0x20, 0, 2, 7) && memcmp(s.Data, lc, 7) == 0);
    step = 1;
    CHECK(Step(&c, UCOP_LINE_CODING, 0, lc, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    step = 0;
    Step(&c, UCOP_CONTROL, 7, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x21, 0x22, 3, 2, 0));
    step = 0;
    Step(&c, UCOP_BREAK, 0xFFFF, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x21, 0x23, 0xFFFF, 2, 0));
    step = 0;
    Step(&c, AC_OP_CLOSE, 0, NULL, &step, 1, NULL, 0, &s, &st);
    CHECK(Is(&s, 0x21, 0x22, 0, 2, 0));
    for (i = 0; i < 4; i++) {
        static const AC_U32 ops[4] = { UCOP_FLOW, AC_OP_INIT, AC_OP_OPEN, AC_OP_POLL };
        step = 0;
        CHECK(Step(&c, ops[i], 3, NULL, &step, 1, NULL, 0, &s, &st) == AC_STEP_DONE);
    }
    CHECK(!AcChipPolls(&c) && !AcChipResetsPipes(&c) && AcChipBreak(&c));
    CHECK(Rx1(&c, buf, 16, &st, &rep) == 16 && !rep);
    /* A device's SERIAL_STATE is masked to D0-D6: bit 15 is the chips'. */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xA1; buf[1] = 0x20; buf[4] = 2; buf[6] = 2; buf[8] = 0x03; buf[9] = 0x80;
    off = 0;
    CHECK(AcChipNotify(&c, buf, 10, &off, &st) == 1 && st == 0x0003);
}

static void TestVendorPorts(void)
{
    B b;
    AC_FUNC f[AC_MAX_FUNCS];
    int n;

    /* FT4232H-like: four vendor interfaces, one bulk pair each. */
    Cfg(&b, 4);
    for (n = 0; n < 4; n++) {
        If(&b, (unsigned char)n, 0, 2, 0xFF, 0xFF, 0xFF);
        Ep(&b, (unsigned char)(0x81 + 2 * n), 2, 512, 0);
        Ep(&b, (unsigned char)(0x02 + 2 * n), 2, 512, 0);
    }
    Fix(&b);
    n = AcFindVendorPorts(Guard(b.b, b.n), b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 4 && f[3].CommIf == 3 && f[3].DataIf == 3 && f[3].BulkIn == 0x87 && f[3].BulkOut == 0x08);
    CHECK(f[0].InMax == 512 && f[0].NotifyEp == 0 && f[0].Flags == AC_F_COMBINED);
    CHECK(f[0].Caps == (AC_CAP_LINE | AC_CAP_BREAK));
    CHECK(AcChipFindPorts(AC_FAM_FTDI, Guard(b.b, b.n), b.n, f, 2) == 2);
    /* The filtered configuration of one split function (xHCI98 &MI_02). */
    Cfg(&b, 1);
    If(&b, 2, 0, 2, 0xFF, 0xFF, 0xFF);
    Ep(&b, 0x85, 2, 512, 0);
    Ep(&b, 0x06, 2, 512, 0);
    Fix(&b);
    n = AcFindVendorPorts(Guard(b.b, b.n), b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].CommIf == 2);
    /* PL2303: interrupt, bulk OUT, bulk IN in one interface. */
    Cfg(&b, 1);
    If(&b, 0, 0, 3, 0xFF, 0, 0);
    Ep(&b, 0x81, 3, 10, 1);
    Ep(&b, 0x02, 2, 64, 0);
    Ep(&b, 0x83, 2, 64, 0);
    Fix(&b);
    n = AcFindVendorPorts(Guard(b.b, b.n), b.n, 1, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].NotifyEp == 0x81 && f[0].NotifyMax == 10 && f[0].BulkIn == 0x83);
    /* The endpoint hack: the interrupt endpoint in interface 0, bulk in 1. */
    Cfg(&b, 2);
    If(&b, 0, 0, 1, 0xFF, 0, 0);
    Ep(&b, 0x81, 3, 10, 1);
    If(&b, 1, 0, 2, 0xFF, 0, 0);
    Ep(&b, 0x02, 2, 64, 0);
    Ep(&b, 0x83, 2, 64, 0);
    Fix(&b);
    n = AcFindVendorPorts(Guard(b.b, b.n), b.n, 1, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].CommIf == 1 && f[0].NotifyEp == 0x81);
    n = AcFindVendorPorts(Guard(b.b, b.n), b.n, 0, f, AC_MAX_FUNCS);
    CHECK(n == 1 && f[0].NotifyEp == 0);
    /* No bulk pair: no port; malformed: -1. */
    Cfg(&b, 1);
    If(&b, 0, 0, 1, 0xFF, 0, 0);
    Ep(&b, 0x81, 2, 64, 0);
    Fix(&b);
    CHECK(AcFindVendorPorts(Guard(b.b, b.n), b.n, 0, f, AC_MAX_FUNCS) == 0);
    b.b[2] = (unsigned char)(b.n + 1);
    CHECK(AcFindVendorPorts(Guard(b.b, b.n), b.n, 0, f, AC_MAX_FUNCS) == -1);
}

static void TestCtsBits(void)
{
    AC_U32 ev = 0, err = 0;
    AC_U8 msr;
    AC_DCB dcb;

    msr = AcStateToMsr(0, AC_SS_NOCTS | AC_SS_DSR, &ev, &err);
    CHECK(!(msr & AC_MSR_CTS) && (msr & AC_MSR_DCTS) && (msr & AC_MSR_DSR) && (ev & AC_EV_CTS));
    ev = 0;
    msr = AcStateToMsr(AC_SS_NOCTS, 0, &ev, &err);
    CHECK((msr & AC_MSR_CTS) && (msr & AC_MSR_DCTS) && (ev & AC_EV_CTS));
    ev = 0;
    msr = AcStateToMsr(AC_SS_DSR, AC_SS_DSR, &ev, &err);
    CHECK((msr & AC_MSR_CTS) && !(msr & AC_MSR_DCTS) && !(ev & AC_EV_CTS));
    /* The state events follow the lines: CTS off with a DSR change, and
       CTS rising alone. */
    ev = 0;
    AcStateToMsr(AC_SS_NOCTS, AC_SS_NOCTS | AC_SS_DSR, &ev, &err);
    CHECK((ev & AC_EV_DSRS) && !(ev & AC_EV_CTSS));
    ev = 0;
    AcStateToMsr(AC_SS_NOCTS, 0, &ev, &err);
    CHECK((ev & AC_EV_CTSS) && (ev & AC_EV_CTS) && !(ev & AC_EV_DSRS));
    AcDefaultDcb(&dcb, 4096);
    CHECK(AcDcbFlow(&dcb) == 0);
    dcb.BitMask |= AC_fOutXCTSFlow;
    CHECK(AcDcbFlow(&dcb) == UCIF_FLOW_CTS);
    dcb.BitMask |= AC_fRTSFlow;
    CHECK(AcDcbFlow(&dcb) == (UCIF_FLOW_CTS | UCIF_FLOW_RTS));
    /* A CTS hold holds XON/XOFF and TransmitCommChar bytes too. */
    {
        QS tx;
        AC_FLOW fl;
        AC_U8 out[8];
        memset(&fl, 0, sizeof(fl));
        QInit(&tx, 8);
        AcTxPut(&tx.q, (const AC_U8 *)"ab", 2);
        fl.FlowPending = 1; fl.FlowChar = 0x13;
        fl.Immediate = 'Z'; fl.Status = AC_fTximmed | AC_fCtsHold;
        CHECK(AcTxPull(&tx.q, &fl, out, 8) == 0 && fl.FlowPending && (fl.Status & AC_fTximmed));
        fl.Status &= ~AC_fCtsHold;
        CHECK(AcTxPull(&tx.q, &fl, out, 8) == 4 && out[0] == 0x13 && out[1] == 'Z' && out[2] == 'a');
    }
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
    TestChipIds();
    TestFtdi();
    TestPl2303();
    TestCp210x();
    TestCdcSteps();
    TestVendorPorts();
    TestCtsBits();
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

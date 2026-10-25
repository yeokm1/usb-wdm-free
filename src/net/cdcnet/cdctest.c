/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host tests for CDCNET's pure parsers (ethernet.md sections 3.3, 8 and
 * 10.3): the descriptor probe, NCM NTB16 and RNDIS. Every fixture is
 * synthetic, built here from the published field layouts; none is a
 * capture of QEMU or of a real device. Truncated inputs end at an
 * inaccessible page, so any read past the caller's size faults.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cdcproto.h"
#pragma warning(disable: 4127) /* do { } while (0) test macros */

static unsigned long assertions, mutations;
static void Check(int ok, const char *expression, int line)
{
    assertions++;
    if (!ok) { printf("FAIL %d: %s\n", line, expression); exit(1); }
}
#define CHECK(x) Check((x), #x, __LINE__)

static unsigned char *guard;
static DWORD page;

/* Copy n bytes so that they end exactly at the inaccessible page. */
static unsigned char *Edge(const void *data, unsigned long n)
{
    unsigned char *p = guard + page - n;
    memcpy(p, data, n);
    return p;
}

static unsigned long seed = 12345;
static unsigned long Rand(void) { seed = seed * 1103515245UL + 12345UL; return (seed >> 8) & 0xffffff; }

/* ---------------- descriptors ---------------- */

/* QEMU-like ECM configuration (value 1): control 0 with Header, Union,
 * Ethernet; interrupt IN; data 1 alt 0 empty, alt 1 bulk pair. */
static unsigned char ecm[] = {
    9,2,0,0,2,1,0,0x80,50,
    9,4,0,0,1,2,6,0,0,
    5,0x24,0,0x10,1,
    5,0x24,6,0,1,
    13,0x24,15,3,0,0,0,0,0xea,5,0,0,0,
    7,5,0x81,3,16,0,10,
    9,4,1,0,0,10,0,0,0,
    9,4,1,1,2,10,0,0,0,
    7,5,0x82,2,64,0,0,
    7,5,0x03,2,64,0,0
};

/* RNDIS (value 2): control 02/02/FF with Header/CM/ACM/Union, data alt 0. */
static unsigned char rndis[] = {
    9,2,0,0,2,2,0,0x80,50,
    9,4,0,0,1,2,2,0xff,0,
    5,0x24,0,0x10,1,
    5,0x24,1,0,1,
    4,0x24,2,0,
    5,0x24,6,0,1,
    7,5,0x81,3,8,0,32,
    9,4,1,0,2,10,0,0,0,
    7,5,0x82,2,64,0,0,
    7,5,0x03,2,64,0,0
};

/* NCM at High Speed with an NCM functional descriptor, data alt 1 NTB. */
static unsigned char ncm[] = {
    9,2,0,0,2,1,0,0x80,50,
    9,4,0,0,1,2,0x0d,0,0,
    5,0x24,0,0x10,1,
    5,0x24,6,0,1,
    13,0x24,15,4,0,0,0,0,0xea,5,0,0,0,
    6,0x24,0x1a,0,1,0x21,
    7,5,0x83,3,16,0,9,
    9,4,1,0,0,10,0,1,0,
    9,4,1,1,2,10,0,1,0,
    7,5,0x81,2,0,2,0,
    7,5,0x02,2,0,2,0
};

/* Android-like: an IAD-grouped RNDIS E0/01/03 without a Union, plus an
 * unrelated vendor interface 2 (ADB-like) after it. */
static unsigned char android[] = {
    9,2,0,0,3,1,0,0x80,50,
    8,0x0b,0,2,0xe0,1,3,0,
    9,4,0,0,1,0xe0,1,3,0,
    5,0x24,0,0x10,1,
    7,5,0x81,3,8,0,9,
    9,4,1,0,2,10,0,0,0,
    7,5,0x82,2,0,2,0,
    7,5,0x02,2,0,2,0,
    9,4,2,0,2,0xff,0x42,1,0,
    7,5,0x83,2,0,2,0,
    7,5,0x04,2,0,2,0
};

static void Fix(unsigned char *b, unsigned long n) { b[2] = (unsigned char)n; b[3] = (unsigned char)(n >> 8); }

static long Probe(const unsigned char *b, unsigned long n, unsigned mask, CDC_FUNCTION *f)
{
    memset(f, 0xcc, sizeof(*f));
    return CdcProbeConfig(Edge(b, n), n, mask, f);
}

static void MutateAll(const unsigned char *base, unsigned long n, unsigned mask)
{
    unsigned char *copy = malloc(n);
    CDC_FUNCTION f;
    unsigned long i, bit;
    for (i = 0; i < n; i++) {
        for (bit = 0; bit < 8; bit++) {
            memcpy(copy, base, n); copy[i] ^= (unsigned char)(1 << bit);
            (void)Probe(copy, n, mask, &f); mutations++;
        }
        memcpy(copy, base, n); copy[i] = 0; (void)Probe(copy, n, mask, &f); mutations++;
        memcpy(copy, base, n); copy[i] = 0xff; (void)Probe(copy, n, mask, &f); mutations++;
    }
    for (i = 0; i < n; i++) CHECK(Probe(base, i, mask, &f) < 0);  /* truncation */
    free(copy);
}

static void TestDescriptors(void)
{
    CDC_FUNCTION f;
    unsigned all = CDC_ALLOW(CDC_PROTO_ECM) | CDC_ALLOW(CDC_PROTO_NCM) | CDC_ALLOW(CDC_PROTO_RNDIS);
    unsigned char copy[256];
    unsigned long n;

    Fix(ecm, sizeof(ecm)); Fix(rndis, sizeof(rndis)); Fix(ncm, sizeof(ncm)); Fix(android, sizeof(android));
    CHECK(Probe(ecm, sizeof(ecm), all, &f) == CDC_OK);
    CHECK(f.Protocol == CDC_PROTO_ECM && f.ConfigValue == 1 && f.Control == 0 && f.Data == 1);
    CHECK(f.DataAlt == 1 && f.DataAltZero && f.In == 0x82 && f.Out == 3 && f.Interrupt == 0x81);
    CHECK(f.InMax == 64 && f.OutMax == 64 && f.IntMax == 16 && f.MacIndex == 3 && f.MaxSegment == 1514);
    CHECK(Probe(ecm, sizeof(ecm), CDC_ALLOW(CDC_PROTO_RNDIS), &f) == CDC_E_UNSUPPORTED);

    CHECK(Probe(rndis, sizeof(rndis), all, &f) == CDC_OK);
    CHECK(f.Protocol == CDC_PROTO_RNDIS && f.ConfigValue == 2 && f.Data == 1 && f.DataAlt == 0);
    CHECK(f.HasUnion && f.Interrupt == 0x81 && f.MacIndex == 0);
    CHECK(Probe(rndis, sizeof(rndis), CDC_ALLOW(CDC_PROTO_ECM) | CDC_ALLOW(CDC_PROTO_NCM), &f) == CDC_E_UNSUPPORTED);

    CHECK(Probe(ncm, sizeof(ncm), all, &f) == CDC_OK);
    CHECK(f.Protocol == CDC_PROTO_NCM && f.InMax == 512 && f.OutMax == 512 && f.NcmCaps == 0x21);
    CHECK(f.In == 0x81 && f.Out == 2 && f.Interrupt == 0x83 && f.DataAlt == 1);

    CHECK(Probe(android, sizeof(android), all, &f) == CDC_OK);
    CHECK(f.Protocol == CDC_PROTO_RNDIS && !f.HasUnion && f.Control == 0 && f.Data == 1);
    CHECK(f.In == 0x82 && f.Out == 2 && f.NumInterfaces == 3);

    /* ECM + NCM in one configuration: NCM is preferred, ECM if NCM barred. */
    n = sizeof(ecm) + sizeof(ncm) - 9;
    {
        static unsigned char big[512];
        unsigned long i;
        memcpy(big, ecm, sizeof(ecm));
        memcpy(big + sizeof(ecm), ncm + 9, sizeof(ncm) - 9);
        /* renumber the NCM function to interfaces 2 and 3 */
        for (i = sizeof(ecm); i < n; i += big[i]) {
            if (big[i + 1] == 4) big[i + 2] += 2;
            if (big[i + 1] == 0x24 && big[i + 2] == 6) { big[i + 3] += 2; big[i + 4] += 2; }
            if (big[i + 1] == 5 && (big[i + 2] & 0x0f) <= 3) big[i + 2] += 4;
        }
        big[4] = 4; Fix(big, n);
        CHECK(Probe(big, n, all, &f) == CDC_OK && f.Protocol == CDC_PROTO_NCM && f.Control == 2 && f.Data == 3);
        CHECK(Probe(big, n, CDC_ALLOW(CDC_PROTO_ECM), &f) == CDC_OK && f.Protocol == CDC_PROTO_ECM && f.Control == 0);
        MutateAll(big, n, all);
    }

    /* Contradictions. */
    memcpy(copy, ecm, sizeof(ecm)); copy[27] = 7;            /* union slave missing */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_CONFIG);
    memcpy(copy, ecm, sizeof(ecm)); copy[26] = 1;            /* union master not self */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_CONFIG);
    memcpy(copy, ecm, sizeof(ecm)); copy[36] = 0x00; copy[37] = 0x04; /* wMaxSegmentSize 1024 */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_CONFIG);
    memcpy(copy, ecm, sizeof(ecm)); copy[31] = 0;            /* no iMACAddress */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_CONFIG);
    memcpy(copy, ecm, sizeof(ecm)); copy[68] = 0x04;         /* two bulk OUT */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_CONFIG);
    memcpy(copy, ecm, sizeof(ecm)); copy[60] = 0;            /* data alt 1 -> duplicate alt 0 */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_MALFORMED);
    memcpy(copy, ecm, sizeof(ecm)); copy[44] = 2;            /* interrupt is bulk */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_CONFIG);
    memcpy(copy, ecm, sizeof(ecm)); copy[2]++;               /* wTotalLength mismatch */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_MALFORMED);
    memcpy(copy, ecm, sizeof(ecm)); copy[9] = 1;             /* bLength below 2 */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_MALFORMED);
    memcpy(copy, ecm, sizeof(ecm)); copy[9] = 200;           /* bLength past end */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_MALFORMED);
    memcpy(copy, ecm, sizeof(ecm)); copy[45] = 0; copy[46] = 0; /* interrupt wMaxPacketSize 0 */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_CONFIG);
    memcpy(copy, ncm, sizeof(ncm)); copy[43] = 0x99;         /* NCM descriptor -> unknown subtype */
    CHECK(Probe(copy, sizeof(ncm), all, &f) == CDC_E_CONFIG);
    memcpy(copy, ecm, sizeof(ecm)); copy[53] = 0x02;         /* data class -> 02 */
    CHECK(Probe(copy, sizeof(ecm), all, &f) == CDC_E_CONFIG);

    MutateAll(ecm, sizeof(ecm), all);
    MutateAll(rndis, sizeof(rndis), all);
    MutateAll(ncm, sizeof(ncm), all);
    MutateAll(android, sizeof(android), all);
    {
        unsigned long i, k;
        for (i = 0; i < 20000; i++) {
            unsigned long len = 9 + Rand() % 200;
            for (k = 0; k < len; k++) copy[k] = (unsigned char)Rand();
            copy[0] = 9; copy[1] = 2; Fix(copy, len);
            (void)Probe(copy, len, all, &f); mutations++;
        }
    }
}

static void TestMac(void)
{
    static const char good[] = "5254A0b0C0d1";
    unsigned char s[26], mac[6];
    int i;
    s[0] = 26; s[1] = 3;
    for (i = 0; i < 12; i++) { s[2 + 2 * i] = good[i]; s[3 + 2 * i] = 0; }
    CHECK(CdcParseMacString(Edge(s, 26), 26, mac) == CDC_OK);
    CHECK(mac[0] == 0x52 && mac[1] == 0x54 && mac[2] == 0xa0 && mac[5] == 0xd1);
    for (i = 0; i < 26; i++) CHECK(CdcParseMacString(Edge(s, i), i, mac) < 0);
    s[4] = '3'; CHECK(CdcParseMacString(Edge(s, 26), 26, mac) == CDC_E_CONFIG); /* multicast */
    s[4] = '2';
    s[2] = 'G'; CHECK(CdcParseMacString(Edge(s, 26), 26, mac) == CDC_E_MALFORMED);
    s[2] = '5'; s[3] = 1; CHECK(CdcParseMacString(Edge(s, 26), 26, mac) == CDC_E_MALFORMED);
    for (i = 0; i < 12; i++) { s[2 + 2 * i] = '0'; s[3 + 2 * i] = 0; }
    CHECK(CdcParseMacString(Edge(s, 26), 26, mac) == CDC_E_CONFIG);         /* zero */
    for (i = 0; i < 12; i++) s[2 + 2 * i] = 'F';
    CHECK(CdcParseMacString(Edge(s, 26), 26, mac) == CDC_E_CONFIG);         /* broadcast */
    s[0] = 28; CHECK(CdcParseMacString(Edge(s, 26), 26, mac) == CDC_E_MALFORMED);
}

/* ---------------- NCM ---------------- */

typedef struct SINK { unsigned long Count, Bytes; unsigned char Last[CDC_ETH_MAX]; unsigned long LastLen; } SINK;
static void Emit(void *context, const cdc_u8 *frame, cdc_u32 length)
{
    SINK *s = context;
    CHECK(length >= CDC_ETH_MIN && length <= CDC_ETH_MAX);
    s->Count++; s->Bytes += length;
    memcpy(s->Last, frame, length); s->LastLen = length;
}

static void Params(unsigned char *b, unsigned long inMax, unsigned outDiv, unsigned outRem,
                   unsigned outAlign, unsigned long outMax, unsigned maxDg)
{
    memset(b, 0, 28);
    b[0] = 28; b[2] = 1;
    b[4] = (unsigned char)inMax; b[5] = (unsigned char)(inMax >> 8); b[6] = (unsigned char)(inMax >> 16);
    b[8] = 4; b[12] = 4;
    b[16] = (unsigned char)outMax; b[17] = (unsigned char)(outMax >> 8); b[18] = (unsigned char)(outMax >> 16);
    b[20] = (unsigned char)outDiv; b[21] = (unsigned char)(outDiv >> 8);
    b[22] = (unsigned char)outRem; b[23] = (unsigned char)(outRem >> 8);
    b[24] = (unsigned char)outAlign; b[25] = (unsigned char)(outAlign >> 8);
    b[26] = (unsigned char)maxDg; b[27] = (unsigned char)(maxDg >> 8);
}

static unsigned long Ntb(unsigned char *b, NCM_PARAMS *p, unsigned long frames, unsigned long size, unsigned long cap)
{
    cdc_u32 lengths[NCM_TX_DATAGRAMS], offsets[NCM_TX_DATAGRAMS], ndp, total, used, i, k;
    for (i = 0; i < frames && i < NCM_TX_DATAGRAMS; i++) lengths[i] = size + i;
    memset(b, 0xee, cap);
    used = CdcNcmTxPlan(p, lengths, i, cap, offsets, &ndp, &total);
    for (i = 0; i < used; i++) {
        CHECK(offsets[i] % p->OutDivisor == p->OutRemainder);
        for (k = 0; k < lengths[i]; k++) b[offsets[i] + k] = (unsigned char)(i + k);
    }
    if (used) CdcNcmTxFinish(b, 7, lengths, offsets, used, ndp, total);
    /* Every byte outside NTH, NDP and the datagrams is zero: nothing stale. */
    for (i = 12; i < total; i++) {
        int payload = 0;
        for (k = 0; k < used; k++) if (i >= offsets[k] && i < offsets[k] + lengths[k]) payload = 1;
        if (!payload && (i < ndp || i >= ndp + 8 + 4 * (used + 1))) CHECK(b[i] == 0);
    }
    return used ? total : 0;
}

static void TestNcm(void)
{
    unsigned char raw[28], ntb[16384], copy[16384];
    NCM_PARAMS p;
    SINK sink;
    unsigned long total, i, j;

    Params(raw, 16384, 4, 0, 4, 16384, 0);
    CHECK(CdcNcmParams(Edge(raw, 28), 28, 16384, 16384, &p) == CDC_OK);
    CHECK(p.OutMaxDatagrams == NCM_TX_DATAGRAMS && p.InMaxSize == 16384);
    for (i = 0; i < 28; i++) CHECK(CdcNcmParams(Edge(raw, i), i, 16384, 16384, &p) < 0);
    Params(raw, 1024, 4, 0, 4, 16384, 0);
    CHECK(CdcNcmParams(raw, 28, 16384, 16384, &p) == CDC_E_CONFIG);
    Params(raw, 16384, 4, 0, 4, 16384, 0); raw[2] = 2;           /* NTB32 only */
    CHECK(CdcNcmParams(raw, 28, 16384, 16384, &p) == CDC_E_UNSUPPORTED);
    Params(raw, 65536 * 4, 0, 7, 3, 65536 * 4, 0);               /* zero divisor, bad alignment */
    CHECK(CdcNcmParams(raw, 28, 16384, 8192, &p) == CDC_OK);
    CHECK(p.OutDivisor == 4 && p.OutRemainder == 0 && p.OutAlignment == 4);
    CHECK(p.InMaxSize == 16384 && p.OutMaxSize == 8192);

    /* Round trips at several layouts. */
    {
        static const unsigned layouts[][3] = { {4,0,4}, {4,2,4}, {16,14,8}, {32,0,16}, {512,2,64} };
        for (j = 0; j < sizeof(layouts) / sizeof(layouts[0]); j++) {
            Params(raw, 16384, layouts[j][0], layouts[j][1], layouts[j][2], 16384, 0);
            CHECK(CdcNcmParams(raw, 28, 16384, 16384, &p) == CDC_OK);
            for (i = 1; i <= NCM_TX_DATAGRAMS; i += 3) {
                total = Ntb(ntb, &p, i, 60, sizeof(ntb));
                CHECK(total > 0);
                memset(&sink, 0, sizeof(sink));
                CHECK(CdcNcmRxParse(Edge(ntb, total), total, Emit, &sink) == (long)i);
                CHECK(sink.Count == i && sink.LastLen == 60 + i - 1 && sink.Last[5] == (unsigned char)(i - 1 + 5));
            }
        }
    }
    /* Capacity limits the plan: 1514-byte frames, 8 KiB block. */
    Params(raw, 16384, 4, 0, 4, 8192, 0);
    CHECK(CdcNcmParams(raw, 28, 16384, 16384, &p) == CDC_OK);
    {
        cdc_u32 lengths[8], offsets[8], ndp, t;
        for (i = 0; i < 8; i++) lengths[i] = 1514;
        CHECK(CdcNcmTxPlan(&p, lengths, 8, 16384, offsets, &ndp, &t) == 5 && t <= 8192);
        lengths[0] = 13;
        CHECK(CdcNcmTxPlan(&p, lengths, 8, 16384, offsets, &ndp, &t) == 0);
    }

    Params(raw, 16384, 4, 0, 4, 16384, 0);
    CHECK(CdcNcmParams(raw, 28, 16384, 16384, &p) == CDC_OK);
    total = Ntb(ntb, &p, 3, 100, sizeof(ntb));
    for (i = 0; i < total; i++) CHECK(CdcNcmRxParse(Edge(ntb, i), i, Emit, &sink) < 0);
    /* Header and table damage. NTH at 0, NDP at 12. */
#define BAD(stmt) do { memcpy(copy, ntb, total); stmt; memset(&sink, 0, sizeof(sink)); \
        CHECK(CdcNcmRxParse(Edge(copy, total), total, Emit, &sink) < 0 && sink.Count == 0); } while (0)
    BAD(copy[0] = 'X');                                   /* NTH signature */
    BAD(copy[4] = 16);                                    /* header length */
    BAD(copy[8] = (unsigned char)(total + 1); copy[9] = (unsigned char)((total + 1) >> 8)); /* block > transfer */
    BAD(copy[10] = 13);                                   /* misaligned NDP */
    BAD(copy[10] = 0);                                    /* no NDP */
    BAD(copy[10] = 0xfc; copy[11] = 0xff);                /* NDP offset wraps */
    BAD(copy[15] = '1');                                  /* CRC NDP "NCM1" */
    BAD(copy[12] = 'X');                                  /* NDP signature */
    BAD(copy[16] = 12);                                   /* NDP length below 16 */
    BAD(copy[16] = 18);                                   /* NDP length not /4 */
    BAD(copy[18] = 12);                                   /* NDP links to itself: cycle */
    BAD(copy[18] = 13);                                   /* misaligned next NDP */
    BAD(copy[20] = 4; copy[21] = 0);                      /* datagram inside NTH */
    BAD(copy[20] = 16; copy[21] = 0);                     /* datagram over NDP */
    BAD(copy[20] = 0xff; copy[21] = 0xff);                /* datagram offset wraps */
    BAD(copy[22] = 0xff; copy[23] = 0xff);                /* datagram length wraps */
    BAD(copy[22] = 13; copy[23] = 0);                     /* runt */
    BAD(copy[22] = 0xeb; copy[23] = 0x05);                /* 1515 bytes */
    BAD(copy[16] = 16; copy[24] = 1; copy[26] = 60);      /* table without terminator */
    /* A second NDP that loops back to the first. */
    BAD(copy[18] = 12; copy[19] = 0);
    /* Datagram count limit: one table with 257 entries is refused. */
    {
        static unsigned char big[4096];
        unsigned long n = 12 + 8 + 4 * 258 + 16;
        memset(big, 0, sizeof(big));
        big[0] = 'N'; big[1] = 'C'; big[2] = 'M'; big[3] = 'H'; big[4] = 12;
        big[8] = (unsigned char)n; big[9] = (unsigned char)(n >> 8); big[10] = 12;
        big[12] = 'N'; big[13] = 'C'; big[14] = 'M'; big[15] = '0';
        { unsigned long t = 8 + 4 * 258; big[16] = (unsigned char)t; big[17] = (unsigned char)(t >> 8); }
        for (i = 0; i < 257; i++) { big[20 + 4 * i] = (unsigned char)(n - 16); big[21 + 4 * i] = (unsigned char)((n - 16) >> 8); big[22 + 4 * i] = 14; }
        CHECK(CdcNcmRxParse(big, n, Emit, &sink) < 0);
        big[20 + 4 * 256] = 0; big[22 + 4 * 256] = 0;  /* 256 entries: accepted */
        memset(&sink, 0, sizeof(sink));
        CHECK(CdcNcmRxParse(big, n, Emit, &sink) == 256 && sink.Count == 256);
    }
    /* Two chained tables are accepted. */
    {
        unsigned char two[512];
        unsigned long n = 12 + 16 + 16 + 60 + 60;
        memset(two, 0, sizeof(two));
        memcpy(two, "NCMH", 4); two[4] = 12; two[8] = (unsigned char)n; two[10] = 12;
        memcpy(two + 12, "NCM0", 4); two[16] = 16; two[18] = 28; two[20] = 44; two[22] = 60;
        memcpy(two + 28, "NCM0", 4); two[32] = 16; two[36] = 104; two[38] = 60;
        memset(&sink, 0, sizeof(sink));
        CHECK(CdcNcmRxParse(Edge(two, n), n, Emit, &sink) == 2 && sink.Count == 2);
        two[18] = 12;  /* second table points back to the first */
        memcpy(two + 12, "NCM0", 4); two[30] = 12;
        memset(&sink, 0, sizeof(sink));
        CHECK(CdcNcmRxParse(Edge(two, n), n, Emit, &sink) < 0 && sink.Count == 0);
    }
    for (i = 0; i < total; i++) {
        for (j = 0; j < 8; j++) {
            memcpy(copy, ntb, total); copy[i] ^= (unsigned char)(1 << j);
            memset(&sink, 0, sizeof(sink));
            if (CdcNcmRxParse(Edge(copy, total), total, Emit, &sink) < 0) CHECK(sink.Count == 0);
            mutations++;
        }
    }
    for (i = 0; i < 50000; i++) {
        unsigned long n = 12 + Rand() % 300, k;
        for (k = 0; k < n; k++) copy[k] = (unsigned char)(Rand() % 4 ? Rand() % 64 : Rand());
        memcpy(copy, "NCMH", 4); copy[4] = 12; copy[8] = (unsigned char)n; copy[9] = 0;
        copy[10] = (unsigned char)((Rand() % 16) * 4); copy[11] = 0;
        if (Rand() % 2) memcpy(copy + (copy[10] < n - 4 ? copy[10] : 0), "NCM0", 4);
        memset(&sink, 0, sizeof(sink));
        if (CdcNcmRxParse(Edge(copy, n), n, Emit, &sink) < 0) CHECK(sink.Count == 0);
        mutations++;
    }
}

/* ---------------- RNDIS ---------------- */

static void Put32(unsigned char *p, unsigned long v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

static unsigned long Packet(unsigned char *b, unsigned long frame, unsigned long shift)
{
    unsigned long i, n;
    for (i = 0; i < frame; i++) b[44 + i] = (unsigned char)(i ^ frame);
    n = CdcRndisTxHeader(b, frame, shift);
    return n;
}

static void TestRndis(void)
{
    unsigned char m[8192], copy[8192];
    RNDIS_REPLY r;
    SINK sink;
    unsigned long n, i, j, k;
    static const unsigned long oid = 0x01010101;

    CHECK(CdcRndisInit(m, 5, 16384) == 24 && m[0] == 2 && m[4] == 24 && m[8] == 5 && m[12] == 1 && m[21] == 0x40);
    n = CdcRndisQuery(m, sizeof(m), 6, oid, "\x01\x02\x03\x04", 4, 1);
    CHECK(n == 32 && m[0] == 5 && m[4] == 32 && m[16] == 4 && m[20] == 20 && m[28] == 1 && m[31] == 4);
    CHECK(CdcRndisQuery(m, 30, 6, oid, "\x01\x02\x03\x04", 4, 0) == 0);
    CHECK(CdcRndisSimple(m, RNDIS_MSG_KEEPALIVE, 9) == 12 && m[0] == 8 && m[8] == 9);
    CHECK(CdcRndisKeepaliveReply(m, 9) == 16 && m[3] == 0x80 && m[8] == 9);

    /* INIT_CMPLT */
    memset(m, 0, 52);
    Put32(m, 0x80000002); Put32(m + 4, 52); Put32(m + 8, 1); Put32(m + 16, 1);
    Put32(m + 32, 1); Put32(m + 36, 1600); Put32(m + 40, 2);
    CHECK(CdcRndisParseReply(Edge(m, 52), 52, &r) == CDC_OK && r.Type == 0x80000002 && r.MaxPackets == 1 && r.MaxTransfer == 1600 && r.AlignShift == 2);
    for (i = 0; i < 52; i++) CHECK(CdcRndisParseReply(Edge(m, i), i, &r) < 0);
    Put32(m + 16, 2); CHECK(CdcRndisParseReply(m, 52, &r) == CDC_E_UNSUPPORTED);
    /* QUERY_CMPLT with a 6-byte address at offset 16 (byte 24). */
    memset(m, 0, 64);
    Put32(m, 0x80000004); Put32(m + 4, 30); Put32(m + 8, 7); Put32(m + 16, 6); Put32(m + 20, 16);
    m[24] = 0x02; m[29] = 0x66;
    CHECK(CdcRndisParseReply(Edge(m, 30), 30, &r) == CDC_OK && r.InfoLength == 6 && r.Info[0] == 2 && r.Info[5] == 0x66);
    Put32(m + 20, 15); CHECK(CdcRndisParseReply(m, 30, &r) < 0);          /* overlaps header */
    Put32(m + 20, 17); CHECK(CdcRndisParseReply(m, 30, &r) < 0);          /* runs past message */
    Put32(m + 20, 0xfffffff8); CHECK(CdcRndisParseReply(m, 30, &r) < 0);  /* wraps */
    Put32(m + 20, 16); Put32(m + 16, 0xffffffff); CHECK(CdcRndisParseReply(m, 30, &r) < 0);
    Put32(m + 16, 6); Put32(m + 4, 31); CHECK(CdcRndisParseReply(m, 30, &r) < 0); /* longer than transfer */
    /* INDICATE_STATUS between a request and its response. */
    memset(m, 0, 64);
    Put32(m, 7); Put32(m + 4, 20); Put32(m + 8, RNDIS_STATUS_MEDIA_CONNECT);
    CHECK(CdcRndisParseReply(Edge(m, 20), 20, &r) == CDC_OK && r.Type == 7 && r.Status == RNDIS_STATUS_MEDIA_CONNECT && !r.Info);
    Put32(m + 12, 4); Put32(m + 16, 12); Put32(m + 4, 24);
    CHECK(CdcRndisParseReply(Edge(m, 24), 24, &r) == CDC_OK && r.InfoLength == 4);
    Put32(m + 16, 11); CHECK(CdcRndisParseReply(m, 24, &r) < 0);
    memset(m, 0, 16); Put32(m, 8); Put32(m + 4, 12); Put32(m + 8, 0x55);
    CHECK(CdcRndisParseReply(Edge(m, 12), 12, &r) == CDC_OK && r.Type == 8 && r.RequestId == 0x55);
    memset(m, 0, 16); Put32(m, 0x80000005); Put32(m + 4, 16); Put32(m + 8, 3); Put32(m + 12, 0xc0000001);
    CHECK(CdcRndisParseReply(Edge(m, 16), 16, &r) == CDC_OK && r.RequestId == 3 && r.Status == 0xc0000001);
    Put32(m, 0x80000099); CHECK(CdcRndisParseReply(m, 16, &r) == CDC_E_UNSUPPORTED);

    /* Data: frames 14..1514, each header+frame, round trip. */
    for (i = 14; i <= 1514; i += 50) {
        for (k = 0; k <= 3; k++) {
            n = Packet(m, i, k);
            CHECK(n >= 44 + i && (k == 0 || n % (1UL << k) == 0));
            memset(&sink, 0, sizeof(sink));
            CHECK(CdcRndisRxParse(Edge(m, n), n, Emit, &sink) == 1 && sink.Count == 1 && sink.LastLen == i);
            CHECK(sink.Last[0] == (unsigned char)(0 ^ i));
        }
    }
    /* 84-byte frame -> 128-byte message (an exact 64-byte multiple). */
    CHECK(Packet(m, 84, 0) == 128);
    /* Zero tails: 1 byte, shorter than, equal to and longer than 44. */
    {
        static const unsigned long tails[] = { 1, 7, 43, 44, 45, 100, 511 };
        for (j = 0; j < sizeof(tails) / sizeof(tails[0]); j++) {
            n = Packet(m, 84, 0);
            memset(m + n, 0, tails[j]);
            memset(&sink, 0, sizeof(sink));
            CHECK(CdcRndisRxParse(Edge(m, n + tails[j]), n + tails[j], Emit, &sink) == 1 && sink.Count == 1);
            m[n + tails[j] - 1] = 1;                                     /* non-zero tail */
            memset(&sink, 0, sizeof(sink));
            CHECK(CdcRndisRxParse(Edge(m, n + tails[j]), n + tails[j], Emit, &sink) < 0 && sink.Count == 0);
        }
    }
    /* Three messages in one transfer. */
    n = Packet(m, 60, 2); n += Packet(m + n, 1514, 2); n += Packet(m + n, 100, 2);
    memset(&sink, 0, sizeof(sink));
    CHECK(CdcRndisRxParse(Edge(m, n), n, Emit, &sink) == 3 && sink.Count == 3 && sink.LastLen == 100);
    /* Damage, with nothing emitted from the damaged transfer. */
#define RBAD(stmt) do { memcpy(copy, m, n); stmt; memset(&sink, 0, sizeof(sink)); \
        CHECK(CdcRndisRxParse(Edge(copy, n), n, Emit, &sink) < 0 && sink.Count == 0); } while (0)
    n = Packet(m, 60, 0); n += Packet(m + n, 60, 0);
    RBAD(Put32(copy + 104, 9));                         /* second type wrong */
    RBAD(Put32(copy + 4, 43));                          /* message below header */
    RBAD(Put32(copy + 4, (unsigned long)n + 1));        /* message beyond transfer */
    RBAD(Put32(copy + 8, 35));                          /* data inside header */
    RBAD(Put32(copy + 8, 37));                          /* data runs out of message */
    RBAD(Put32(copy + 8, 0xfffffff8));                  /* offset wraps */
    RBAD(Put32(copy + 12, 61));                         /* length past message */
    RBAD(Put32(copy + 12, 0xffffffff));                 /* length wraps */
    RBAD(Put32(copy + 12, 13));                         /* runt */
    RBAD(Put32(copy + 16, 36); Put32(copy + 20, 100));  /* OOB past message */
    RBAD(Put32(copy + 28, 0x7fffffff); Put32(copy + 32, 4)); /* PPI offset wild */
    for (i = 0; i < n; i++) CHECK(CdcRndisRxParse(Edge(m, i), i, Emit, &sink) <= 1);
    for (i = 0; i < n; i++) {
        for (j = 0; j < 8; j++) {
            memcpy(copy, m, n); copy[i] ^= (unsigned char)(1 << j);
            memset(&sink, 0, sizeof(sink));
            if (CdcRndisRxParse(Edge(copy, n), n, Emit, &sink) < 0) CHECK(sink.Count == 0);
            (void)CdcRndisParseReply(Edge(copy, n), n, &r);
            mutations += 2;
        }
    }
    for (i = 0; i < 50000; i++) {
        unsigned long len = 8 + Rand() % 400;
        for (k = 0; k < len; k++) copy[k] = (unsigned char)(Rand() % 3 ? Rand() % 48 : Rand());
        Put32(copy, Rand() % 2 ? 1 : (Rand() % 9) | (Rand() % 2 ? 0x80000000 : 0));
        Put32(copy + 4, Rand() % 2 ? len : Rand() % 512);
        memset(&sink, 0, sizeof(sink));
        if (CdcRndisRxParse(Edge(copy, len), len, Emit, &sink) < 0) CHECK(sink.Count == 0);
        if (CdcRndisParseReply(Edge(copy, len), len, &r) == CDC_OK && r.Info)
            CHECK(r.Info >= guard + page - len && r.Info + r.InfoLength <= guard + page);
        mutations += 2;
    }
}

/* ---------------- notifications ---------------- */

static const unsigned char conn0[8] = { 0xa1, 0x00, 0, 0, 0, 0, 0, 0 };
static const unsigned char conn1[8] = { 0xa1, 0x00, 1, 0, 0, 0, 0, 0 };
static const unsigned char speed[16] = { 0xa1, 0x2a, 0, 0, 0, 0, 8, 0,
                                         0x00, 0x6a, 0x9c, 0x1c, 0x00, 0x6a, 0x9c, 0x1c }; /* 480000000 */

static unsigned long Feed(CDC_NOTIFY_STATE *st, const unsigned char *b, unsigned long n, int full,
                          CDC_NOTIFY *out)
{
    /* Each completion ends exactly at the guard page. */
    return CdcNotifyFeed(st, n ? Edge(b, n) : b, n, full, out, 8);
}

static void TestNotify(void)
{
    unsigned char b[64];
    CDC_NOTIFY n[8];
    CDC_NOTIFY_STATE st;
    cdc_u32 off, i, k;
    /* One notification; the walk then ends. */
    off = 0;
    CHECK(CdcNotifyNext(Edge(conn1, 8), 8, &off, &n[0]) == 1 && off == 8);
    CHECK(n[0].Code == 0 && n[0].Value == 1 && n[0].Length == 0);
    CHECK(CdcNotifyNext(Edge(conn1, 8), 8, &off, &n[0]) == 0);
    /* Coalesced SPEED_CHANGE + NETWORK_CONNECTION 0 (what xHCI98 delivered
     * in one 24-byte completion, 2026-10-23). */
    memset(&st, 0, sizeof(st));
    memcpy(b, speed, 16); memcpy(b + 16, conn0, 8);
    CHECK(Feed(&st, b, 24, 0, n) == 2 && st.Truncated == 0 && st.Held == 0);
    CHECK(n[0].Code == 0x2a && n[0].Length == 8 && n[0].Data[1] == 0x6a);
    CHECK(n[1].Code == 0 && n[1].Value == 0);
    /* CONN0 + CONN1: both, in order. */
    memcpy(b, conn0, 8); memcpy(b + 8, conn1, 8);
    CHECK(Feed(&st, b, 16, 0, n) == 2 && n[0].Value == 0 && n[1].Value == 1);
    /* A truncated tail after a whole notification: the first is kept, the
     * tail dropped and counted. */
    memcpy(b, conn1, 8); memcpy(b + 8, speed, 5);
    CHECK(Feed(&st, b, 13, 0, n) == 1 && n[0].Value == 1 && st.Truncated == 1 && st.Held == 0);
    /* wLength beyond the buffer. */
    memcpy(b, speed, 16); b[6] = 0x20;
    CHECK(Feed(&st, b, 16, 0, n) == 0 && st.Truncated == 2);
    off = 0;
    CHECK(CdcNotifyNext(Edge(b, 16), 16, &off, &n[0]) == CDC_E_MALFORMED && off == 16);
    /* A header that is not a class notification ends the walk. */
    memcpy(b, conn1, 8); b[8] = 0x21; memset(b + 9, 0, 7);
    CHECK(Feed(&st, b, 16, 0, n) == 1 && st.Truncated == 3);
    /* An 8-byte endpoint: the 16-byte SPEED_CHANGE arrives as two full
     * packets and is reassembled; a lone full CONN packet is whole. */
    memset(&st, 0, sizeof(st));
    CHECK(Feed(&st, speed, 8, 1, n) == 0 && st.Held == 8 && st.Truncated == 0);
    CHECK(Feed(&st, speed + 8, 8, 1, n) == 1 && st.Held == 0 && n[0].Code == 0x2a &&
          n[0].Data[3] == 0x1c);
    CHECK(Feed(&st, conn0, 8, 1, n) == 1 && n[0].Value == 0 && st.Held == 0);
    /* A held head followed by a short packet that does not complete it. */
    CHECK(Feed(&st, speed, 8, 1, n) == 0 && st.Held == 8);
    CHECK(Feed(&st, speed + 8, 4, 0, n) == 0 && st.Held == 0 && st.Truncated == 1);
    /* A zero-length completion drops a held head. */
    CHECK(Feed(&st, speed, 8, 1, n) == 0 && st.Held == 8);
    CHECK(Feed(&st, b, 0, 0, n) == 0 && st.Held == 0 && st.Truncated == 2);
    /* Eight connections in one 64-byte completion; more never overflow out[]. */
    memset(&st, 0, sizeof(st));
    for (i = 0; i < 8; i++) { memcpy(b + 8 * i, conn1, 8); b[8 * i + 2] = (unsigned char)(i & 1); }
    CHECK(Feed(&st, b, 64, 1, n) == 8);
    for (i = 0; i < 8; i++) CHECK(n[i].Value == (i & 1));
    /* A smaller output limit stops early and writes nothing past it. */
    {
        CDC_NOTIFY lim[4];
        memset(&st, 0, sizeof(st));
        memset(lim, 0x5a, sizeof(lim));
        CHECK(CdcNotifyFeed(&st, Edge(b, 64), 64, 1, lim, 3) == 3);
        CHECK(lim[3].Code == 0x5a && lim[3].Length == 0x5a5a);
        CHECK(st.Truncated == 1);                 /* the five left over are counted */
        CHECK(CdcNotifyFeed(&st, Edge(b, 64), 64, 1, lim, 0) == 0);
    }
    /* A held tail never disturbs the notifications returned with it (Codex,
     * round 1): SPEED_CHANGE, an unknown notification with wLength 25, then
     * 9..15 bytes of a cut header in one full 64-byte packet. */
    for (k = 9; k < 16; k++) {
        unsigned char full[64];
        memset(full, 0, sizeof(full));
        memcpy(full, speed, 16);
        full[16] = 0xa1; full[17] = 0x7f; full[22] = (unsigned char)(48 - k - 8);
        memcpy(full + 64 - k, speed, k);
        memset(&st, 0, sizeof(st));
        CHECK(Feed(&st, full, 64, 1, n) == 2 && st.Held == k && st.Truncated == 0);
        CHECK(n[0].Code == 0x2a && memcmp(n[0].Data, speed + 8, 8) == 0);
        CHECK(n[1].Code == 0x7f && n[1].Length == 48 - k - 8);
        CHECK(Feed(&st, speed + k, 16 - k, 0, n) == 1 && n[0].Code == 0x2a && st.Held == 0);
        CHECK(memcmp(n[0].Data, speed + 8, 8) == 0);
    }
    /* The walker itself on every length of a coalesced pair, each input
     * ending at the guard page. */
    memcpy(b, speed, 16); memcpy(b + 16, conn0, 8);
    for (k = 0; k <= 24; k++) {
        const unsigned char *e = Edge(b, k);
        long r;
        off = 0; i = 0;
        while ((r = CdcNotifyNext(e, k, &off, &n[0])) > 0) {
            CHECK(n[0].Data + n[0].Length <= e + k);
            i++;
        }
        CHECK(off == k && i == (k >= 24 ? 2u : k >= 16 ? 1u : 0u));
    }
    /* Every truncation and every byte of a coalesced pair, mutated: bounded. */
    memcpy(b, speed, 16); memcpy(b + 16, conn0, 8);
    for (k = 0; k <= 24; k++) {
        int full;
        for (full = 0; full < 2; full++) {
            memset(&st, 0, sizeof(st));
            i = Feed(&st, b, k, full, n);
            CHECK(i <= 2 && st.Held < CDC_NOTIFY_HELD);
            CHECK(st.Held == ((full && k && k != 16 && k != 24) ? k % 16 : 0));
        }
    }
    for (k = 0; k < 24 * 8; k++) {
        unsigned char m[24];
        memcpy(m, b, 24); m[k / 8] ^= (unsigned char)(1 << (k % 8));
        memset(&st, 0, sizeof(st));
        i = Feed(&st, m, 24, 0, n);
        CHECK(i <= 3);
        while (i--) CHECK(n[i].Data + n[i].Length <= st.Buf + 24);
        mutations++;
    }
}

int main(void)
{
    SYSTEM_INFO sys;
    DWORD previous;
    GetSystemInfo(&sys);
    page = 3 * sys.dwPageSize;   /* room for 8 KiB inputs before the guard */
    guard = VirtualAlloc(NULL, page + sys.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(guard != NULL);
    CHECK(VirtualProtect(guard + page, sys.dwPageSize, PAGE_NOACCESS, &previous) != 0);
    TestDescriptors();
    TestMac();
    TestNcm();
    TestRndis();
    TestNotify();
    printf("PASS %lu assertions, %lu mutated or random inputs; synthetic descriptor/NCM/RNDIS fixtures\n",
           assertions, mutations);
    return 0;
}

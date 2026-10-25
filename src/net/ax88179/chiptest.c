/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host tests for the AX179F chip core (axchip.c) and the shared pure
 * helpers (netchip.c), against a register-file model of the device. The
 * expected request sequences are written by hand from Linux
 * ax88179_178a.c at f47d2236 (ax88179_reset, link_reset, set_multicast,
 * suspend, unbind); the model is synthetic, not a capture. The one
 * physical datum used is the captured SuperSpeed descriptor set in
 * testdata/ax179 and the medium/queue values the physical host run
 * recorded (docs/testing/2026-10-21-ax88179-physical.md).
 *
 * usage: chiptest <package source directory>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "axchip.h"
#include "axframe.h"
#pragma warning(disable: 4127)

static unsigned long assertions;
static void Check(int ok, const char *expression, int line)
{
    assertions++;
    if (!ok) { printf("FAIL %d: %s\n", line, expression); exit(1); }
}
#define CHECK(x) Check((x), #x, __LINE__)

/* ---------------- the device model ---------------- */

typedef struct ENTRY {
    int In;
    nc_u8 Request;
    nc_u16 Value, Index, Length;
    nc_u8 Data[8];
} ENTRY;

typedef struct MOCK {
    nc_u8 Mac[256];
    nc_u16 Phy[32];
    nc_u16 Eeprom43;
    int EepromStall;
    nc_u32 FifoBusy;            /* FIFO reads still to answer "full" */
    nc_u32 FailAt;              /* 1-based request that fails; 0 none */
    nc_u32 ShortAt;             /* 1-based request answered short */
    ENTRY Log[512];
    nc_u32 Count;
    nc_u32 Slept;
} MOCK;

static long MockControl(void *context, int in, nc_u8 request, nc_u16 value, nc_u16 index,
                        void *data, nc_u16 length, nc_u16 *actual)
{
    MOCK *m = context;
    ENTRY *e;
    nc_u8 *b = data;
    nc_u32 n = ++m->Count;
    CHECK(n <= 512);
    e = &m->Log[n - 1];
    e->In = in; e->Request = request; e->Value = value; e->Index = index; e->Length = length;
    memset(e->Data, 0, sizeof(e->Data));
    if (!in) memcpy(e->Data, data, length < 8 ? length : 8);
    if (m->FailAt == n) return -5;
    *actual = length;
    if (m->ShortAt == n) { *actual = 0; return 0; }
    switch (request) {
    case AX_ACCESS_MAC:
        CHECK(value + length <= 256);
        CHECK(index == length);          /* the Linux helpers pass size as index */
        if (in) memcpy(b, m->Mac + value, length); else memcpy(m->Mac + value, b, length);
        break;
    case AX_ACCESS_PHY:
        CHECK(value == AX88179_PHY_ID && index < 32 && length == 2);
        if (in) { b[0] = (nc_u8)m->Phy[index]; b[1] = (nc_u8)(m->Phy[index] >> 8); }
        else m->Phy[index] = (nc_u16)(b[0] | (b[1] << 8));
        break;
    case AX_ACCESS_EEPROM:
        CHECK(in && value == 0x43 && index == 1 && length == 2);
        if (m->EepromStall) return -32;
        b[0] = (nc_u8)m->Eeprom43; b[1] = (nc_u8)(m->Eeprom43 >> 8);
        break;
    case AX_FIFO_STATUS_CMD:
        CHECK(in && value == 0x8c && index == 0 && length == 4);
        memset(b, 0, 4);
        if (m->FifoBusy) { m->FifoBusy--; b[3] = 0x40; }
        break;
    default:
        CHECK(0);
    }
    return 0;
}

static void MockSleep(void *context, nc_u32 ms)
{
    ((MOCK *)context)->Slept += ms;
}

static void Fresh(MOCK *m, NET_IO *io)
{
    static const nc_u8 mac[6] = { 0x00, 0x0e, 0xc6, 0x12, 0x34, 0x56 };
    memset(m, 0, sizeof(*m));
    memcpy(m->Mac + AX_NODE_ID, mac, 6);
    m->Phy[MII_BMCR] = BMCR_ANENABLE;
    memset(io, 0, sizeof(*io));
    io->Context = m;
    io->Control = MockControl;
    io->Sleep = MockSleep;
}

/* One expected request. */
static nc_u32 at;
static void Expect(MOCK *m, int in, nc_u8 request, nc_u16 value, nc_u16 index, nc_u16 length,
                   const char *bytes)
{
    ENTRY *e;
    CHECK(at < m->Count);
    e = &m->Log[at++];
    CHECK(e->In == in && e->Request == request && e->Value == value && e->Index == index &&
          e->Length == length);
    if (bytes) {
        nc_u32 i;
        for (i = 0; i < length && i < 8; i++) {
            unsigned v;
            sscanf(bytes + 3 * i, "%2x", &v);
            CHECK(e->Data[i] == v);
        }
    }
}
#define W(r, v, i, l, s) Expect(&m, 0, r, v, i, l, s)
#define R(r, v, i, l) Expect(&m, 1, r, v, i, l, NULL)

static nc_u8 state[NET_CHIP_STATE];
#define SETTLE_REQUESTS 15     /* AxStart's settle cycle */
static const NET_PROBE superProbe = { 0, 1, 0, 0, 1, 0x82, 0x03, 0x81, 1024, 1024, 8, 11,
                                      NC_SPEED_SUPER, 0x0b95, 0x1790, 0x0100 };

/* ---------------- Start ---------------- */

static void StartSequence(int eeprom)
{
    MOCK m;
    NET_IO io;
    NET_PARAMS p;
    Fresh(&m, &io);
    m.Eeprom43 = (nc_u16)eeprom;
    m.Mac[AX_PHYPWR_RSTCTL] = 0;
    CHECK(sizeof(AX_STATE) <= NET_CHIP_STATE);
    CHECK(NetPackageChip.Start(state, &io, &superProbe, &p) == NC_OK);
    at = 0;
    /* The settle cycle: a short run with this configuration (gigabit
     * medium, wake on magic packet armed), then stopped as Stop does. */
    W(1, 0x26, 2, 2, "00 00");
    W(1, 0x26, 2, 2, "20 00");
    W(1, 0x33, 1, 1, "03");
    W(1, 0x2e, 5, 5, "07 4f 00 12 ff");
    W(1, 0x55, 1, 1, "34");
    W(1, 0x54, 1, 1, "52");
    W(1, 0x34, 1, 1, "00");
    W(1, 0x35, 1, 1, "00");
    W(1, 0x0b, 2, 2, "aa 03");
    W(1, 0x24, 1, 1, "64");
    W(1, 0x22, 2, 2, "3b 01");
    W(1, 0x22, 2, 2, "3b 00");
    W(1, 0x0b, 2, 2, "00 00");
    W(1, 0x33, 1, 1, "00");
    W(1, 0x26, 2, 2, "00 00");
    CHECK(at == SETTLE_REQUESTS);
    /* The bring-up. */
    W(1, 0x26, 2, 2, "00 00");
    W(1, 0x26, 2, 2, "20 00");
    W(1, 0x33, 1, 1, "03");
    R(4, 0x43, 1, 2);
    if (eeprom != 0xffff && (eeprom & 0x0100)) {
        R(1, 0x33, 1, 1);
        W(1, 0x33, 1, 1, "0b");
        R(1, 0x26, 2, 2);
        W(1, 0x26, 2, 2, "20 10");
    }
    R(1, 0x10, 6, 6);
    W(1, 0x2e, 5, 5, "07 4f 00 12 ff");
    W(1, 0x55, 1, 1, "34");
    W(1, 0x54, 1, 1, "52");
    W(1, 0x34, 1, 1, "00");
    W(1, 0x35, 1, 1, "00");
    W(1, 0x0b, 2, 2, "aa 03");
    R(1, 0x0b, 2, 2);
    W(1, 0x24, 1, 1, "64");
    W(1, 0x22, 2, 2, "33 01");
    W(2, 3, 0x1f, 2, "03 00");
    W(2, 3, 0x19, 2, "46 32");
    W(2, 3, 0x1f, 2, "00 00");
    W(2, 3, 0x0d, 2, "07 00");
    W(2, 3, 0x0e, 2, "3c 00");
    W(2, 3, 0x0d, 2, "07 40");
    W(2, 3, 0x0e, 2, "00 00");
    R(2, 3, 0x00, 2);
    W(2, 3, 0x00, 2, "00 12");
    CHECK(at == m.Count);
    CHECK(m.Slept == 900 + 700);
    CHECK(p.Mac[0] == 0x00 && p.Mac[5] == 0x56);
    CHECK(p.RxSize == 26624 && p.TxBatch == 1 && p.TxHeader == 8 && p.TxSlots == 4);
    CHECK(p.TxSize >= 8 + NC_ETH_MAX);
    CHECK(((AX_STATE *)state)->RxLength == 20480);
    /* The receive mode is written and read back for the log. */
    CHECK(p.RxConfig == 0x03aa && p.RxConfigRead == 0x03aa);
}

static void StartFailures(void)
{
    MOCK m;
    NET_IO io;
    NET_PARAMS p;
    nc_u32 n, total;
    /* An absent EEPROM word (the read stalls) is not fatal, as in Linux. */
    Fresh(&m, &io);
    m.EepromStall = 1;
    CHECK(NetPackageChip.Start(state, &io, &superProbe, &p) == NC_OK);
    total = m.Count;
    /* Every other failing request stops the sequence: nothing after it. */
    for (n = 1; n <= total; n++) {
        if (n == SETTLE_REQUESTS + 4) continue;   /* the EEPROM read */
        Fresh(&m, &io);
        m.EepromStall = 1;
        m.FailAt = n;
        CHECK(NetPackageChip.Start(state, &io, &superProbe, &p) < 0);
        CHECK(m.Count == n);
        Fresh(&m, &io);
        m.EepromStall = 1;
        m.ShortAt = n;
        CHECK(NetPackageChip.Start(state, &io, &superProbe, &p) == NC_E_DEVICE);
        CHECK(m.Count == n);
    }
}

/* ---------------- Link ---------------- */

static void LinkCase(nc_u16 phy, nc_u8 usb, const char *queue, const char *medium,
                     nc_u32 mbps, nc_u32 rxLength)
{
    MOCK m;
    NET_IO io;
    NET_PARAMS p;
    NET_LINK l;
    Fresh(&m, &io);
    CHECK(NetPackageChip.Start(state, &io, &superProbe, &p) == NC_OK);
    m.Count = 0;
    m.Phy[GMII_PHY_PHYSR] = phy;
    m.Mac[AX_PHYSICAL_LINK_STATUS] = usb;
    CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK);
    at = 0;
    W(1, 0x0b, 2, 2, "00 00");
    W(1, 0x0b, 2, 2, "aa 03");
    R(0x81, 0x8c, 0, 4);
    R(1, 0x02, 1, 1);
    R(2, 3, 0x11, 2);
    if (!queue) {
        CHECK(at == m.Count && !l.Up && l.RxLength == 20480);
        CHECK(NetPackageChip.Poll(state, &io) == 0);
        return;
    }
    W(1, 0x2e, 5, 5, queue);
    W(1, 0x22, 2, 2, medium);
    CHECK(at == m.Count);
    CHECK(l.Up && l.Mbps == mbps && l.RxLength == rxLength);
    CHECK(l.FullDuplex == ((phy & GMII_PHY_PHYSR_FULL) != 0));
    CHECK(NetPackageChip.Poll(state, &io) == 0);
    m.Phy[GMII_PHY_PHYSR] = 0;
    CHECK(NetPackageChip.Poll(state, &io) == 1);
}

static void Links(void)
{
    /* The physical run: 1000 FD at SuperSpeed gave medium 013B and queue
     * 07 4F 00 12 FF with a 20480-byte receive buffer. */
    LinkCase(0x8000 | 0x2000 | 0x400, 0x04, "07 4f 00 12 ff", "3b 01", 1000, 20480);
    LinkCase(0x8000 | 0x2000 | 0x400, 0x02, "07 20 03 16 ff", "3b 01", 1000, 24576);
    LinkCase(0x8000 | 0x2000 | 0x400, 0x00, "07 cc 4c 18 08", "3b 01", 1000, 26624);
    LinkCase(0x8000 | 0x400, 0x04, "07 4f 00 12 ff", "39 01", 1000, 20480);
    LinkCase(0x4000 | 0x2000 | 0x400, 0x04, "07 ae 07 18 ff", "32 03", 100, 26624);
    LinkCase(0x4000 | 0x2000 | 0x400, 0x02, "07 ae 07 18 ff", "32 03", 100, 26624);
    LinkCase(0x4000 | 0x400, 0x00, "07 cc 4c 18 08", "30 03", 100, 26624);
    LinkCase(0x2000 | 0x400, 0x04, "07 cc 4c 18 08", "32 01", 10, 26624);
    LinkCase(0x0400, 0x02, "07 cc 4c 18 08", "30 01", 10, 26624);
    LinkCase(0x8000 | 0x2000, 0x04, NULL, NULL, 0, 0);
    LinkCase(0, 0x04, NULL, NULL, 0, 0);
    {
        /* TX FIFO full for longer than the bound: no configuration this
         * round, link reported down, counted. */
        MOCK m;
        NET_IO io;
        NET_PARAMS p;
        NET_LINK l;
        Fresh(&m, &io);
        CHECK(NetPackageChip.Start(state, &io, &superProbe, &p) == NC_OK);
        m.Count = 0; m.Slept = 0;
        m.FifoBusy = 100;
        m.Phy[GMII_PHY_PHYSR] = 0xa400;
        CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK);
        CHECK(!l.Up && m.Count == 30 && m.Slept == 90 && ((AX_STATE *)state)->FifoBusy == 1);
        /* Busy twice, then free: configured. */
        m.Count = 0;
        m.FifoBusy = 2;
        CHECK(NetPackageChip.Link(state, &io, &l) == NC_OK);
        CHECK(l.Up && m.Count == 9 + 4);
    }
}

/* ---------------- filters ---------------- */

static void Filters(void)
{
    MOCK m;
    NET_IO io;
    NET_PARAMS p;
    static const nc_u8 groups[4][6] = {
        { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 },   /* bin 31 */
        { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 },   /* bin 62 */
        { 0x01, 0x00, 0x5e, 0x7f, 0xff, 0xfa },   /* bin 43 */
        { 0x33, 0x33, 0xff, 0x12, 0x34, 0x56 },   /* bin 2 */
    };
    nc_u8 many[65 * 6];
    /* Bins computed independently (zlib CRC-32, bit-reversed, top six bits). */
    CHECK((NcEtherCrc(groups[0], 6) >> 26) == 31);
    CHECK((NcEtherCrc(groups[1], 6) >> 26) == 62);
    CHECK((NcEtherCrc(groups[2], 6) >> 26) == 43);
    CHECK((NcEtherCrc(groups[3], 6) >> 26) == 2);
    CHECK(NcEtherCrc((const nc_u8 *)"123456789", 9) == 0x9b63d02cUL);
    CHECK(NcFcs((const nc_u8 *)"123456789", 9) == 0xcbf43926UL);

    Fresh(&m, &io);
    CHECK(NetPackageChip.Start(state, &io, &superProbe, &p) == NC_OK);
    m.Count = 0;
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_DIRECTED | NC_FILTER_BROADCAST |
                                   NC_FILTER_MULTICAST, &groups[0][0], 4) == NC_OK);
    at = 0;
    /* bins 31, 62, 43, 2: bytes 3 (bit 7), 7 (bit 6), 5 (bit 3), 0 (bit 2) */
    W(1, 0x16, 8, 8, "04 00 00 80 00 08 00 40");
    W(1, 0x0b, 2, 2, "98 02");
    CHECK(at == m.Count);

    m.Count = 0;
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_DIRECTED | NC_FILTER_MULTICAST, NULL, 0) == NC_OK);
    at = 0;
    W(1, 0x0b, 2, 2, "88 02");
    CHECK(at == m.Count);

    m.Count = 0;
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_ALL_MULTICAST | NC_FILTER_MULTICAST,
                                   &groups[0][0], 4) == NC_OK);
    at = 0;
    W(1, 0x0b, 2, 2, "8a 02");
    CHECK(at == m.Count);

    memset(many, 0x01, sizeof(many));
    m.Count = 0;
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_MULTICAST, many, 65) == NC_OK);
    at = 0;
    W(1, 0x0b, 2, 2, "8a 02");
    CHECK(at == m.Count);

    m.Count = 0;
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_PROMISCUOUS | NC_FILTER_MULTICAST,
                                   &groups[0][0], 4) == NC_OK);
    at = 0;
    W(1, 0x0b, 2, 2, "89 02");
    CHECK(at == m.Count);
    CHECK(((AX_STATE *)state)->RxCtl == 0x0289);

    m.Count = 0;
    m.FailAt = 1;
    io.Status = 0;
    CHECK(NetPackageChip.SetFilter(state, &io, NC_FILTER_DIRECTED, NULL, 0) < 0);
    CHECK(((AX_STATE *)state)->RxCtl == 0x0289);   /* unchanged on failure */
}

/* ---------------- stop, suspend, resume, MAC ---------------- */

static void Lifecycle(void)
{
    MOCK m;
    NET_IO io;
    NET_PARAMS p;
    static const nc_u8 laa[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    Fresh(&m, &io);
    CHECK(NetPackageChip.Start(state, &io, &superProbe, &p) == NC_OK);

    m.Count = 0;
    CHECK(NetPackageChip.SetMac(state, &io, laa) == NC_OK);
    at = 0;
    W(1, 0x10, 6, 6, "02 11 22 33 44 55");
    CHECK(at == m.Count);

    m.Count = 0;
    m.Mac[AX_MEDIUM_STATUS_MODE] = 0x3b; m.Mac[AX_MEDIUM_STATUS_MODE + 1] = 0x01;
    m.Mac[AX_PHYPWR_RSTCTL] = 0x20; m.Mac[AX_PHYPWR_RSTCTL + 1] = 0x00;
    CHECK(NetPackageChip.Suspend(state, &io) == NC_OK);
    at = 0;
    R(1, 0x22, 2, 2);
    W(1, 0x22, 2, 2, "3b 00");
    R(1, 0x26, 2, 2);
    W(1, 0x26, 2, 2, "30 00");
    W(1, 0x33, 1, 1, "00");
    W(1, 0x0b, 2, 2, "00 00");
    CHECK(at == m.Count);

    m.Count = 0;
    m.Mac[AX_MEDIUM_STATUS_MODE] = 0x3b; m.Mac[AX_MEDIUM_STATUS_MODE + 1] = 0x01;
    CHECK(NetPackageChip.Stop(state, &io) == NC_OK);
    at = 0;
    R(1, 0x22, 2, 2);
    W(1, 0x22, 2, 2, "3b 00");
    W(1, 0x0b, 2, 2, "00 00");
    W(1, 0x33, 1, 1, "00");
    W(1, 0x26, 2, 2, "00 00");
    CHECK(at == m.Count);

    /* Resume is the whole reset sequence again. */
    m.Count = 0;
    CHECK(NetPackageChip.Resume(state, &io, &superProbe, &p) == NC_OK);
    CHECK(m.Count == SETTLE_REQUESTS + 23);   /* settle, 22, the RX_CTL read-back */
}

/* ---------------- interrupt, receive, transmit ---------------- */

static nc_u32 emitted, emittedBytes;
static nc_u8 lastFrame[2048];
static void Emit(void *context, const nc_u8 *frame, nc_u32 length)
{
    (void)context;
    emitted++;
    emittedBytes += length;
    memcpy(lastFrame, frame, length);
}

static nc_u32 Put32(nc_u8 *b, nc_u32 v)
{
    b[0] = (nc_u8)v; b[1] = (nc_u8)(v >> 8); b[2] = (nc_u8)(v >> 16); b[3] = (nc_u8)(v >> 24);
    return 4;
}

/* An aggregate in the current firmware's layout: each packet is 2 bytes
 * of alignment pseudo header, the frame, its FCS, padded to 8; then one
 * metadata entry and one dummy entry per packet; then the trailer. */
static nc_u32 Aggregate(nc_u8 *b, const nc_u32 *lengths, nc_u32 count, int badFcs)
{
    nc_u32 at = 0, i, meta;
    nc_u32 entries[16];
    for (i = 0; i < count; i++) {
        nc_u32 n = lengths[i], k, len = 2 + n + (badFcs < 0 ? 0 : 4);
        b[at] = b[at + 1] = 0;
        for (k = 0; k < n; k++) b[at + 2 + k] = (nc_u8)(k == 0 ? 0x02 : (i * 31 + k));
        if (badFcs >= 0)
            Put32(b + at + 2 + n, NcFcs(b + at + 2, n) ^ (badFcs && i == count - 1 ? 1 : 0));
        entries[i] = len << 16;
        at += len;
        while (at & 7) b[at++] = 0;
    }
    meta = at;
    for (i = 0; i < count; i++) {
        at += Put32(b + at, entries[i]);
        at += Put32(b + at, 0x80000000UL);     /* dummy: length 0, drop bit */
    }
    at += Put32(b + at, (2 * count) | (meta << 16));
    return at;
}

static void Datapath(void)
{
    nc_u8 b[8192], hdr[8], interrupt[8] = { 0 };
    NET_RX_STATS st;
    nc_u32 lengths[3] = { 60, 1514, 342 }, n, len;
    CHECK(NetPackageChip.Status(state, interrupt, 4) == -1);
    CHECK(NetPackageChip.Status(state, interrupt, 8) == 0);
    interrupt[2] = 0x01;
    CHECK(NetPackageChip.Status(state, interrupt, 8) == 1);

    n = Aggregate(b, lengths, 3, 0);
    emitted = emittedBytes = 0;
    CHECK(NetPackageChip.RxParse(state, b, n, Emit, NULL, &st) == 3);
    CHECK(emitted == 3 && emittedBytes == 60 + 1514 + 342 && st.Dropped == 0);
    CHECK(st.FcsStripped == 3);       /* the safety net fired, counted */
    CHECK(lastFrame[0] == 0x02);

    /* Per-frame FCS: a record whose last 4 bytes are not its FCS is
     * delivered whole (the chip drops real CRC errors itself). */
    n = Aggregate(b, lengths, 3, 1);
    emitted = emittedBytes = 0;
    CHECK(NetPackageChip.RxParse(state, b, n, Emit, NULL, &st) == 3);
    CHECK(emitted == 3 && emittedBytes == 60 + 1514 + 342 + 4 && st.Dropped == 0);
    CHECK(st.FcsStripped == 2);

    /* The configured mode, no FCS (seen on 98 SE, 2026-10-22): frames
     * exact, the safety net silent. */
    n = Aggregate(b, lengths, 3, -1);
    emitted = emittedBytes = 0;
    CHECK(NetPackageChip.RxParse(state, b, n, Emit, NULL, &st) == 3);
    CHECK(emitted == 3 && emittedBytes == 60 + 1514 + 342 && st.Dropped == 0);
    CHECK(st.FcsStripped == 0);

    /* Malformed: trailer offset past the data: nothing emitted. */
    n = Aggregate(b, lengths, 3, 0);
    b[n - 2] = 0xff;
    emitted = 0;
    CHECK(NetPackageChip.RxParse(state, b, n, Emit, NULL, &st) == NC_E_MALFORMED);
    CHECK(emitted == 0);
    /* Every truncation of a good aggregate either parses whole or is
     * refused with nothing emitted. */
    n = Aggregate(b, lengths, 3, 0);
    for (len = 0; len < n; len++) {
        long r;
        emitted = 0;
        r = NetPackageChip.RxParse(state, b, len, Emit, NULL, &st);
        CHECK(r < 0 ? emitted == 0 : (nc_u32)r == emitted);
    }

    /* TX header: length, and the padding flag exactly at packet multiples. */
    for (len = NC_ETH_PAD; len <= NC_ETH_MAX; len++) {
        nc_u32 mp;
        for (mp = 64; mp <= 1024; mp *= 2) {
            nc_u32 flags;
            if (mp == 128 || mp == 256) continue;
            NetPackageChip.TxHeader(state, hdr, len, mp, 1);
            CHECK((hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((nc_u32)hdr[3] << 24)) == len);
            flags = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | ((nc_u32)hdr[7] << 24);
            CHECK(flags == (((len + 8) % mp) ? 0 : 0x80008000UL));
        }
    }
    n = 100;
    CHECK(NetPackageChip.TxEnd(state, b, &n, sizeof(b), 512) == 0 && n == 100);
}

/* ---------------- probe ---------------- */

static nc_u32 Load(const char *dir, const char *name, nc_u8 *b, nc_u32 cap)
{
    char path[512];
    FILE *f;
    nc_u32 n;
    sprintf(path, "%s\\testdata\\ax179\\%s", dir, name);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    n = (nc_u32)fread(b, 1, cap, f);
    fclose(f);
    return n;
}

static void Probes(const char *dir)
{
    nc_u8 dev[64], cfg[256], copy[256];
    nc_u32 dn = Load(dir, "device.bin", dev, sizeof(dev));
    nc_u32 cn = Load(dir, "config0.bin", cfg, sizeof(cfg));
    const nc_u8 *configs[1];
    nc_u32 sizes[1], i;
    NET_PROBE pr;
    configs[0] = cfg; sizes[0] = cn;
    CHECK(dn == 18 && cn == 57);
    memset(&pr, 0, sizeof(pr));
    CHECK(NetPackageChip.Probe(dev, dn, configs, sizes, 1, &pr) == NC_OK);
    CHECK(pr.Vendor == 0x0b95 && pr.Product == 0x1790 && pr.Revision == 0x0100);
    CHECK(pr.ConfigIndex == 0 && pr.ConfigValue == 1 && pr.Interface == 0);
    CHECK(pr.In == 0x82 && pr.Out == 0x03 && pr.Interrupt == 0x81);
    CHECK(pr.InMax == 1024 && pr.OutMax == 1024 && pr.IntMax == 8 && pr.Speed == NC_SPEED_SUPER);

    /* The same configuration as a USB 2.0 device would give it: no
     * endpoint companions, 512-byte bulk endpoints. Synthetic. */
    {
        static const nc_u8 hs[] = {
            9,2,39,0,1,1,0,0xa0,0x3e,
            9,4,0,0,3,0xff,0xff,0,4,
            7,5,0x81,3,8,0,11,
            7,5,0x82,2,0x00,0x02,0,
            7,5,0x03,2,0x00,0x02,0
        };
        configs[0] = hs; sizes[0] = sizeof(hs);
        CHECK(NetPackageChip.Probe(dev, dn, configs, sizes, 1, &pr) == NC_OK);
        CHECK(pr.Speed == NC_SPEED_HIGH && pr.InMax == 512);
        memcpy(copy, hs, sizeof(hs));
        copy[14] = 0x02; copy[15] = 0x0d;               /* a CDC-NCM-like interface */
        configs[0] = copy;
        CHECK(NetPackageChip.Probe(dev, dn, configs, sizes, 1, &pr) == NC_E_UNSUPPORTED);
        memcpy(copy, hs, sizeof(hs));
        copy[2] = 200;                                  /* wTotalLength beyond the data */
        CHECK(NetPackageChip.Probe(dev, dn, configs, sizes, 1, &pr) == NC_E_MALFORMED);
    }
    /* Every single-bit change of the captured configuration either still
     * probes or is refused; the walk never reads outside the buffer
     * (checked by the bounds of the copy, with total length fixed). */
    for (i = 0; i < cn * 8; i++) {
        long r;
        memcpy(copy, cfg, cn);
        copy[i / 8] ^= (nc_u8)(1 << (i % 8));
        configs[0] = copy; sizes[0] = cn;
        r = NetPackageChip.Probe(dev, dn, configs, sizes, 1, &pr);
        CHECK(r == NC_OK || r == NC_E_UNSUPPORTED || r == NC_E_MALFORMED);
    }
    for (i = 0; i < cn; i++) {
        configs[0] = cfg; sizes[0] = i;
        CHECK(NetPackageChip.Probe(dev, dn, configs, sizes, 1, &pr) != NC_OK);
    }
    CHECK(NetPackageChip.Probe(dev, 17, configs, sizes, 1, &pr) == NC_E_MALFORMED);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: chiptest <source directory>\n"); return 2; }
    StartSequence(0x0000);
    StartSequence(0x0100);
    StartSequence(0xffff);
    StartFailures();
    Links();
    Filters();
    Lifecycle();
    Datapath();
    Probes(argv[1]);
    printf("PASS: %lu AX179F chip-core checks\n", assertions);
    return 0;
}

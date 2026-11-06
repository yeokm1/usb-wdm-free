/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host tests of the SR9700F chip core (srchip.c, ethernet.md section
 * 6.9.2) against a model of the SR9700's register requests and framing,
 * written from the same Linux facts (sr9700.c, sr9700.h at v7.3, facts
 * only). A misreading the core and the model share passes here.
 * Host-test code only.
 */
#include <stdio.h>
#include <string.h>
#include "srchip.h"

static unsigned long assertions, failures;
static void Check(int ok, const char *what, int line)
{
    assertions++;
    if (!ok) { failures++; printf("FAIL line %d: %s\n", line, what); }
}
#define CHECK(x) Check((x) != 0, #x, __LINE__)

typedef struct MODEL {
    nc_u8 Reg[256];
    int Resets, PhyResets, Link;
    unsigned long Requests, RefuseAfter;
    nc_u8 Log[256];
    unsigned LogCount;
} MODEL;

static long Control(void *context, int in, nc_u8 request, nc_u16 value, nc_u16 index,
                    void *data, nc_u16 length, nc_u16 *actual)
{
    MODEL *m = context;
    nc_u8 *b = data;
    nc_u16 i;
    *actual = 0;
    m->Requests++;
    if (m->LogCount < sizeof(m->Log)) m->Log[m->LogCount++] = (nc_u8)(in ? 0x80 | request : request);
    if (m->RefuseAfter && m->Requests > m->RefuseAfter) return NC_E_IO;
    if (index + length > 256) return NC_E_IO;
    if (in) {
        if (request != SR7_REQ_READ || value) return NC_E_IO;
        for (i = 0; i < length; i++) {
            nc_u8 v = m->Reg[index + i];
            if (index + i == SR7_NET_STATUS) v = (nc_u8)(m->Link ? SR7_NET_STATUS_LINK : 0);
            b[i] = v;
        }
    } else if (request == SR7_REQ_WRITE) {
        if (value) return NC_E_IO;
        for (i = 0; i < length; i++) m->Reg[index + i] = b[i];
    } else if (request == SR7_REQ_WRITE1) {
        if (length || value > 0xff) return NC_E_IO;
        if (index == SR7_NET_CTRL && (value & SR7_NET_CTRL_RESET)) m->Resets++;
        if (index == SR7_PHY_RESET && (value & SR7_PHY_RESET_ON)) m->PhyResets++;
        m->Reg[index] = (nc_u8)value;
    } else {
        return NC_E_IO;
    }
    *actual = length;
    return NC_OK;
}

static void Sleep(void *context, nc_u32 ms) { (void)context; (void)ms; }

static union { double Align; nc_u8 Bytes[NET_CHIP_STATE]; } State;

static void TestStartLinkFilter(void)
{
    MODEL m;
    NET_IO io;
    NET_PROBE p;
    NET_PARAMS params;
    NET_LINK link;
    SR7_STATE *s = (SR7_STATE *)State.Bytes;
    static const nc_u8 mac[6] = { 0x00, 0x0e, 0xc6, 0x97, 0x00, 0x01 };
    static const nc_u8 other[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

    memset(&m, 0, sizeof(m));
    memcpy(m.Reg + SR7_ADDRESS, mac, 6);
    memset(&p, 0, sizeof(p));
    p.Vendor = 0x0fe6; p.Product = 0x9700; p.Speed = NC_SPEED_FULL; p.OutMax = 64;
    io.Context = &m; io.Control = Control; io.Sleep = Sleep; io.Status = NC_OK; io.Requests = 0;
    CHECK(Sr7Start(State.Bytes, &io, &p, &params) == NC_OK);
    CHECK(memcmp(params.Mac, mac, 6) == 0);
    CHECK(m.Resets == 1 && m.PhyResets == 1 && m.Reg[SR7_PHY_RESET] == 0);
    CHECK(m.Log[0] == SR7_REQ_WRITE1 && m.Log[1] == (0x80 | SR7_REQ_READ));   /* reset, then the address */
    CHECK(m.Reg[SR7_RX_CTRL] == 0x31 && params.RxConfig == 0x31 && params.RxConfigRead == 0x31);
    CHECK(params.RxSize == 3072 && params.RxCount == 4 && params.TxHeader == 2 && params.TxSize == 2 + 1514 + 1);
    m.Link = 0;
    CHECK(Sr7Link(State.Bytes, &io, &link) == NC_OK && !link.Up);
    CHECK(Sr7Poll(State.Bytes, &io) == 0);
    m.Link = 1;
    CHECK(Sr7Poll(State.Bytes, &io) == 1);
    CHECK(Sr7Link(State.Bytes, &io, &link) == NC_OK && link.Up && link.Mbps == 10 && !link.FullDuplex);
    CHECK(Sr7Poll(State.Bytes, &io) == 0);
    CHECK(Sr7SetFilter(State.Bytes, &io, NC_FILTER_DIRECTED | NC_FILTER_BROADCAST, 0, 0) == NC_OK);
    CHECK(m.Reg[SR7_RX_CTRL] == 0x31);
    CHECK(Sr7SetFilter(State.Bytes, &io, NC_FILTER_MULTICAST, other, 1) == NC_OK);
    CHECK(m.Reg[SR7_RX_CTRL] == (0x31 | SR7_RX_ALL_MULTI));
    CHECK(Sr7SetFilter(State.Bytes, &io, NC_FILTER_MULTICAST, other, 0) == NC_OK && m.Reg[SR7_RX_CTRL] == 0x31);
    CHECK(Sr7SetFilter(State.Bytes, &io, NC_FILTER_PROMISCUOUS, 0, 0) == NC_OK);
    CHECK(m.Reg[SR7_RX_CTRL] == (0x31 | SR7_RX_PROMISC));
    CHECK(Sr7SetMac(State.Bytes, &io, other) == NC_OK && memcmp(m.Reg + SR7_ADDRESS, other, 6) == 0);
    /* An address write that failed: Link writes it again. */
    m.RefuseAfter = m.Requests;
    CHECK(Sr7SetMac(State.Bytes, &io, mac) < 0 && s->MacPending);
    m.RefuseAfter = 0; io.Status = NC_OK;
    CHECK(memcmp(m.Reg + SR7_ADDRESS, other, 6) == 0);
    CHECK(Sr7Poll(State.Bytes, &io) == 1);
    CHECK(Sr7Link(State.Bytes, &io, &link) == NC_OK && link.Up && !s->MacPending);
    CHECK(memcmp(m.Reg + SR7_ADDRESS, mac, 6) == 0 && Sr7Poll(State.Bytes, &io) == 0);
    /* A filter write that failed: Poll asks for Link (and the filter). */
    m.RefuseAfter = m.Requests;
    CHECK(Sr7SetFilter(State.Bytes, &io, NC_FILTER_ALL_MULTICAST, 0, 0) < 0 && s->FilterPending);
    m.RefuseAfter = 0; io.Status = NC_OK;
    CHECK(Sr7Poll(State.Bytes, &io) == 1);
    CHECK(Sr7SetFilter(State.Bytes, &io, NC_FILTER_ALL_MULTICAST, 0, 0) == NC_OK && !s->FilterPending);
    CHECK(Sr7Poll(State.Bytes, &io) == 0);
    CHECK(Sr7Stop(State.Bytes, &io) == NC_OK && m.Reg[SR7_RX_CTRL] == 0 && !s->LinkUp);
    m.RefuseAfter = m.Requests + 1;
    CHECK(Sr7Resume(State.Bytes, &io, &p, &params) == NC_E_IO);   /* three tries, each fails */
    m.RefuseAfter = 0;
    CHECK(Sr7Resume(State.Bytes, &io, &p, &params) == NC_OK && m.Reg[SR7_RX_CTRL] == 0x31);
}

#define MAXF 32
static struct { nc_u32 Count, Length[MAXF]; nc_u8 Data[MAXF][1514]; } Got;
static void Emit(void *context, const nc_u8 *frame, nc_u32 length)
{
    (void)context;
    if (Got.Count < MAXF) { Got.Length[Got.Count] = length; memcpy(Got.Data[Got.Count], frame, length); }
    Got.Count++;
}

static nc_u32 Packet(nc_u8 *b, nc_u32 at, nc_u32 frame, nc_u8 tag)
{
    nc_u32 i, total = frame + 4;
    b[at] = SR7_RX_GOOD; b[at + 1] = (nc_u8)total; b[at + 2] = (nc_u8)(total >> 8);
    for (i = 0; i < frame; i++) b[at + 3 + i] = (nc_u8)(tag + i * 5);
    for (i = 0; i < 4; i++) b[at + 3 + frame + i] = 0xcc;     /* the FCS, not checked here */
    return at + frame + SR7_RX_OVERHEAD;
}

static int Frame(nc_u32 k, nc_u32 frame, nc_u8 tag)
{
    nc_u32 i;
    if (k >= MAXF || Got.Length[k] != frame) return 0;
    for (i = 0; i < frame; i++) if (Got.Data[k][i] != (nc_u8)(tag + i * 5)) return 0;
    return 1;
}

static void TestRx(void)
{
    static nc_u8 b[3072];
    NET_RX_STATS st;
    nc_u32 n;
    memset(&Got, 0, sizeof(Got));
    n = Packet(b, 0, 60, 1);
    n = Packet(b, n, 1514, 2);
    n = Packet(b, n, 61, 3);
    CHECK(Sr7RxParse(State.Bytes, b, n, Emit, 0, &st) == 3 && st.Frames == 3);
    CHECK(Frame(0, 60, 1) && Frame(1, 1514, 2) && Frame(2, 61, 3));
    /* A bad status byte, a length past the end, a frame over 1514: the
     * whole transfer is refused and nothing is delivered. */
    memset(&Got, 0, sizeof(Got));
    n = Packet(b, 0, 60, 1);
    n = Packet(b, n, 70, 2);
    b[67] = 0x41;
    CHECK(Sr7RxParse(State.Bytes, b, n, Emit, 0, &st) == NC_E_MALFORMED && Got.Count == 0);
    n = Packet(b, 0, 60, 1);
    CHECK(Sr7RxParse(State.Bytes, b, n - 1, Emit, 0, &st) == NC_E_MALFORMED && Got.Count == 0);
    n = Packet(b, 0, 1515, 1);
    CHECK(Sr7RxParse(State.Bytes, b, n, Emit, 0, &st) == NC_E_MALFORMED);
    b[0] = SR7_RX_GOOD; b[1] = 3; b[2] = 0;
    CHECK(Sr7RxParse(State.Bytes, b, 10, Emit, 0, &st) == NC_E_MALFORMED);      /* length below the FCS */
    /* A short tail after whole packets: the packets are delivered. */
    n = Packet(b, 0, 100, 4);
    CHECK(Sr7RxParse(State.Bytes, b, n + 5, Emit, 0, &st) == 1 && Frame(0, 100, 4));
    CHECK(((SR7_STATE *)State.Bytes)->Tails == 1);
    /* A runt is counted and dropped; empty transfers are fine. */
    memset(&Got, 0, sizeof(Got));
    n = Packet(b, 0, 10, 5);
    n = Packet(b, n, 60, 6);
    CHECK(Sr7RxParse(State.Bytes, b, n, Emit, 0, &st) == 1 && st.Dropped == 1 && Frame(0, 60, 6));
    CHECK(Sr7RxParse(State.Bytes, b, 0, Emit, 0, &st) == 0);
}

static void TestTx(void)
{
    nc_u8 buffer[2 + 1514 + 1];
    nc_u32 total;
    Sr7TxHeader(State.Bytes, buffer, 60, 64, 1);
    CHECK(buffer[0] == 60 && buffer[1] == 0);
    Sr7TxHeader(State.Bytes, buffer, 62, 64, 1);          /* 2 + 62 = 64: one more */
    CHECK(buffer[0] == 63 && buffer[1] == 0);
    total = 64;
    CHECK(Sr7TxEnd(State.Bytes, buffer, &total, sizeof(buffer), 64) == 0 && total == 65 && buffer[64] == 0);
    total = 2 + 1514;
    CHECK(Sr7TxEnd(State.Bytes, buffer, &total, sizeof(buffer), 64) == 0 && total == 1516);
    Sr7TxHeader(State.Bytes, buffer, 1514, 64, 1);
    CHECK(buffer[0] == 0xea && buffer[1] == 0x05);
}

static void TestStatus(void)
{
    nc_u8 r[8] = { 0x40, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(Sr7Status(State.Bytes, r, 8) == 1);
    r[0] = 0;
    CHECK(Sr7Status(State.Bytes, r, 8) == 0);
    CHECK(Sr7Status(State.Bytes, r, 7) == -1);
}

int main(void)
{
    CHECK(sizeof(SR7_STATE) <= NET_CHIP_STATE);
    TestStartLinkFilter();
    TestRx();
    TestTx();
    TestStatus();
    printf("sr9700f chiptest: %lu checks, %lu failed\n", assertions, failures);
    return failures ? 1 : 0;
}

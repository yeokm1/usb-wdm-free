/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original synthetic host tests, not actual USB Ethernet traffic.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "axframe.h"
static unsigned long checks;
static void Check(int value, unsigned int line, const char *expr)
{ ++checks; if (!value) { printf("FAIL line%u: %s\n", line, expr); exit(1); } }
#define CHECK(x) Check((x), __LINE__, #x)
static unsigned char aggregate[160], changed[160], frame[1600], received[2][1518];
static const unsigned char *input;
static unsigned int inputBytes, calls, lengths[2];
static void W32(unsigned char *p, unsigned long n)
{ unsigned int i; for (i = 0; i < 4; ++i) { p[i] = (unsigned char)n; n >>= 8; } }
static void Receive(void *context, const unsigned char *p, unsigned int bytes, unsigned long status)
{
    CHECK(context == &calls); CHECK(p >= input && p + bytes <= input + inputBytes);
    CHECK(bytes >= 14 && bytes <= 1518 && !(status & 0xa0000000UL));
    if (calls < 2) { lengths[calls] = bytes; memcpy(received[calls], p, bytes); }
    ++calls;
}
static int Parse(const unsigned char *b, unsigned int n, unsigned int limit, AX_FRAME_STATS *s)
{ input = b; inputBytes = n; calls = 0; return AxFrameReceive(b, n, limit, Receive, &calls, s); }
static void Fixture(void)
{
    unsigned int i;
    memset(aggregate, 0xee, sizeof(aggregate));
    for (i = 0; i < 60; ++i) aggregate[2 + i] = (unsigned char)i;
    for (i = 0; i < 64; ++i) aggregate[64 + 2 + i] = (unsigned char)(0x80 + i);
    W32(aggregate + 136, 62UL << 16); W32(aggregate + 140, 0x80000000UL);
    W32(aggregate + 144, 66UL << 16); W32(aggregate + 148, 0x80000000UL);
    W32(aggregate + 156, (136UL << 16) | 4);
}
int main(void)
{
    static const unsigned int packets[] = {8,16,32,64,512,1024};
    SYSTEM_INFO info;
    unsigned char *pages, *end, *out;
    DWORD old;
    AX_FRAME_STATS s;
    unsigned int i, j, k, written;
    GetSystemInfo(&info);
    pages = (unsigned char *)VirtualAlloc(NULL, info.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(pages != NULL); CHECK(VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old));
    end = pages + info.dwPageSize; Fixture();
    CHECK(Parse(aggregate, sizeof(aggregate), 1518, &s));
    CHECK(calls == 2 && s.Headers == 4 && s.Delivered == 2 && s.Dummy == 2 && !s.Dropped);
    CHECK(lengths[0] == 60 && lengths[1] == 64);
    CHECK(!memcmp(received[0], aggregate + 2, 60) && !memcmp(received[1], aggregate + 66, 64));
    memcpy(changed, aggregate, 160); W32(changed + 136, (62UL << 16) | 0x20000000UL);
    CHECK(Parse(changed, 160, 1518, &s) && calls == 1 && s.Dropped == 1);
    CHECK(lengths[0] == 64 && !memcmp(received[0], aggregate + 66, 64));
    memcpy(changed, aggregate, 160); W32(changed + 136, (62UL << 16) | 3UL);
    CHECK(Parse(changed, 160, 1518, &s) && calls == 2 && s.ChecksumErrors == 1);
    CHECK(Parse(aggregate, 160, 60, &s) && calls == 1 && s.Dropped == 1);
    /* Late malformed metadata must not leak the earlier valid frame. */
    memcpy(changed, aggregate, 160); W32(changed + 144, 8191UL << 16);
    CHECK(!Parse(changed, 160, 1518, &s) && !calls && !s.Headers);
    memcpy(changed, aggregate, 160); W32(changed + 156, (135UL << 16) | 4UL);
    CHECK(!Parse(changed, 160, 1518, &s) && !calls);
    memcpy(changed, aggregate, 160); W32(changed + 156, (136UL << 16) | 2049UL);
    CHECK(!Parse(changed, 160, 1518, &s) && !calls);
    /* Old metadata layout without dummy entries or final alignment word. */
    memcpy(changed, aggregate, 136); W32(changed + 136, 62UL << 16);
    W32(changed + 140, 66UL << 16); W32(changed + 144, (136UL << 16) | 2UL);
    CHECK(Parse(changed, 148, 1518, &s) && calls == 2 && !s.Dummy);
    memset(changed, 0, 24); W32(changed + 16, 15UL << 16); W32(changed + 20, (16UL << 16) | 1UL);
    CHECK(Parse(changed, 24, 1518, &s) && !calls && s.Dropped == 1); /* runt */
    for (i = 0; i < 160; ++i) {
        memcpy(end - i, aggregate, i);
        CHECK(!Parse(end - i, i, 1518, &s) && !calls && !s.Delivered);
    }
    for (i = 0; i < 160; ++i) for (j = 1; j <= 128; j <<= 1) {
        memcpy(changed, aggregate, 160); changed[i] ^= (unsigned char)j;
        memcpy(end - 160, changed, 160);
        if (Parse(end - 160, 160, 1518, &s))
            CHECK(calls == s.Delivered && s.Headers == s.Delivered + s.Dropped + s.Dummy);
        else CHECK(!calls && !s.Headers && !s.Delivered);
    }
    CHECK(!Parse(NULL, 160, 1518, &s)); CHECK(!Parse(aggregate, 160, 13, &s));
    CHECK(!Parse(aggregate, 160, 8190, &s)); CHECK(!Parse(end, 65537, 1518, &s));
    for (i = 0; i < sizeof(frame); ++i) frame[i] = (unsigned char)(i ^ 0x5a);
    for (k = 0; k < sizeof(packets) / sizeof(packets[0]); ++k) for (i = 14; i <= 1518; ++i) {
        out = end - i - 8; out[-1] = 0xa7;
        CHECK(AxFrameTransmit(frame, i, 1518, packets[k], out, i + 8, &written));
        CHECK(written == i + 8 && out[-1] == 0xa7 && !memcmp(out + 8, frame, i));
        CHECK(out[0] == (unsigned char)i && out[1] == (unsigned char)(i >> 8) && !out[2] && !out[3]);
        if ((i + 8) % packets[k] == 0)
            CHECK(!out[4] && out[5] == 0x80 && !out[6] && out[7] == 0x80);
        else CHECK(!out[4] && !out[5] && !out[6] && !out[7]);
        memset(out, 0x3c, i + 8);
        CHECK(!AxFrameTransmit(frame, i, 1518, packets[k], out, i + 7, &written) && !written);
        CHECK(out[0] == 0x3c && out[i + 7] == 0x3c);
    }
    CHECK(!AxFrameTransmit(frame, 60, 1518, 0, end - 68, 68, &written));
    CHECK(!AxFrameTransmit(frame, 60, 1518, 63, end - 68, 68, &written));
    CHECK(!AxFrameTransmit(frame, 1519, 1518, 512, end - 1600, 1600, &written));
    CHECK(!AxFrameTransmit(frame, 13, 1518, 512, end - 1600, 1600, &written));
    VirtualFree(pages, 0, MEM_RELEASE);
    printf("PASS: %lu synthetic AX88179 framing/guard/boundary checks\n", checks);
    return 0;
}

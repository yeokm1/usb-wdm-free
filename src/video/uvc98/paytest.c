/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Guarded malformed-header and independent frame-sequence host tests.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "uvcpay.h"
static unsigned int checks, delivered, lastSize, lastStill;
static unsigned char lastFrame[32];
static int Check(int result, const char *expression, unsigned int line)
{
    checks++;
    if (!result) printf("FAIL line %u: %s\n", line, expression);
    return result;
}
#define CHECK(x) if (!Check((x), #x, __LINE__)) return 1
static void Frame(void *context, const unsigned char *p, unsigned int bytes,
    unsigned int still)
{
    unsigned int i;
    (void)context; delivered++; lastSize = bytes; lastStill = still;
    for (i = 0; i < bytes && i < sizeof(lastFrame); i++) lastFrame[i] = p[i];
}
int main(void)
{
    SYSTEM_INFO info;
    DWORD old;
    unsigned char *pages, *p, ring[10];
    unsigned int n, flags, expected, hlen;
    UVC_PAY_HEADER h;
    UVC_PAY s;
    unsigned char initial[] = { 2, 0x82, 99 };
    unsigned char first[] = { 2, 0x81, 1, 2 };
    unsigned char end[] = { 2, 0x83, 3, 4 };
    unsigned char next[] = { 2, 0x80, 5, 6, 7, 8 };
    unsigned char error[] = { 2, 0xc3, 9, 10 };
    unsigned char bad[] = { 8, 0x81, 1 };
    unsigned char overflow[] = { 2, 0x81, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    unsigned char still[] = { 2, 0xa3, 11, 12, 13, 14 };
    GetSystemInfo(&info);
    pages = VirtualAlloc(NULL, info.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(pages != NULL);
    CHECK(VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old));
    for (n = 0; n <= 16; n++) {
        p = pages + info.dwPageSize - n;
        for (flags = 0; flags < 256; flags++) {
            for (hlen = 0; hlen <= 17; hlen++) {
                if (n) memset(p, 0, n);
                if (n) p[0] = (unsigned char)hlen;
                if (n > 1) p[1] = (unsigned char)flags;
                expected = 2 + ((flags & 4) ? 4 : 0) + ((flags & 8) ? 6 : 0);
                CHECK(UvcPayHeader(p, n, &h) ==
                    (n >= 2 && hlen >= expected && hlen <= n));
                if (n >= 2 && hlen >= expected && hlen <= n)
                    CHECK(h.Payload == p + hlen && h.PayloadBytes == n - hlen);
            }
        }
    }
    p = pages + info.dwPageSize - 12;
    for (n = 0; n < 12; n++) p[n] = (unsigned char)n;
    p[0] = 12; p[1] = 0x8c;
    CHECK(UvcPayHeader(p, 12, &h));
    CHECK(h.Pts == 0x05040302UL && h.ScrClock == 0x09080706UL && h.ScrSof == 0x030a);
    CHECK(!UvcPayHeader(NULL, 2, &h));
    CHECK(!UvcPayHeader(p, 12, NULL));
    memset(ring, 0xee, sizeof(ring));
    CHECK(!UvcPayInit(&s, ring + 1, 8, 9, Frame, NULL));
    CHECK(UvcPayInit(&s, ring + 1, 8, 4, Frame, NULL));
    UvcPayFeed(&s, initial, sizeof(initial)); CHECK(!delivered);
    UvcPayFeed(&s, first, sizeof(first)); CHECK(!delivered);
    UvcPayFeed(&s, end, sizeof(end));
    CHECK(delivered == 1 && lastSize == 4 && !lastStill);
    CHECK(!memcmp(lastFrame, "\1\2\3\4", 4));
    UvcPayFeed(&s, end, sizeof(end)); CHECK(delivered == 1);
    UvcPayFeed(&s, next, sizeof(next));
    UvcPayFeed(&s, still, sizeof(still));
    CHECK(delivered == 3 && lastStill && lastFrame[0] == 11);
    UvcPayFeed(&s, next, sizeof(next)); UvcPayLost(&s);
    UvcPayFeed(&s, error, sizeof(error));
    CHECK(s.Dropped == 1 && delivered == 3 && s.ErrorPackets == 1);
    UvcPayFeed(&s, next, sizeof(next));
    UvcPayFeed(&s, bad, sizeof(bad));
    UvcPayFeed(&s, end, sizeof(end));
    CHECK(s.InvalidPackets == 1 && s.Dropped == 2 && delivered == 3);
    UvcPayFeed(&s, next, sizeof(next));
    UvcPayFeed(&s, overflow, sizeof(overflow));
    UvcPayFeed(&s, end, sizeof(end));
    CHECK(s.Overflows == 1 && delivered == 4 && s.Dropped == 3);
    CHECK(ring[0] == 0xee && ring[9] == 0xee);
    UvcPayFeed(&s, NULL, 0); CHECK(s.InvalidPackets == 1);
    CHECK(UvcPayInit(&s, ring + 1, 8, 0, Frame, NULL));
    UvcPayFeed(&s, initial, sizeof(initial));
    UvcPayFeed(&s, first, sizeof(first));
    UvcPayFeed(&s, end, sizeof(end));
    CHECK(delivered == 5 && lastSize == 4);
    /* A lost packet may have carried the next frame's first FID header.
     * Do not deliver that compressed frame just because its EOF survives. */
    CHECK(UvcPayInit(&s, ring + 1, 8, 0, Frame, NULL));
    UvcPayFeed(&s, initial, sizeof(initial));
    UvcPayFeed(&s, first, sizeof(first)); UvcPayLost(&s);
    UvcPayFeed(&s, next, sizeof(next));
    initial[1] = 0x82; UvcPayFeed(&s, initial, sizeof(initial));
    CHECK(delivered == 5 && s.Dropped == 1);
    UvcPayFeed(&s, first, sizeof(first)); UvcPayFeed(&s, end, sizeof(end));
    CHECK(delivered == 6 && lastSize == 4);
    VirtualFree(pages, 0, MEM_RELEASE);
    printf("PASS: %u guarded UVC payload checks and frame-sequence assertions\n", checks);
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host tests of the actual uvciso.c walker over the actual uvcpay.c core.
 * Transfer buffers end at a no-access page so any read past a packet faults.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "uvciso.h"
#include "uvcdesc.h"
static unsigned int checks, failures, frames, lastBytes;
static unsigned char lastFrame[64];
static int Check(int ok, const char *text, unsigned int line)
{
    checks++;
    if (!ok) { failures++; printf("FAIL line %u: %s\n", line, text); }
    return ok;
}
#define CHECK(x) Check((x) != 0, #x, __LINE__)
static void Frame(void *context, const unsigned char *p, unsigned int bytes, unsigned int still)
{
    (void)context; (void)still; frames++; lastBytes = bytes;
    memcpy(lastFrame, p, bytes < sizeof(lastFrame) ? bytes : sizeof(lastFrame));
}
static unsigned char *page;
static unsigned long pageSize;
/* Lay out packets so the transfer's last byte is the last accessible byte. */
static unsigned char *Place(unsigned long bytes)
{
    return page + pageSize - bytes;
}
/* Append one payload (header fid/eof + data) at *at; fill a descriptor. */
static void Put(unsigned char *base, unsigned long *at, UVC_ISO_PACKET *d,
    unsigned int fid, unsigned int eof, const unsigned char *data, unsigned int n)
{
    base[*at] = 2; base[*at + 1] = (unsigned char)(0x80 | fid | (eof ? 2 : 0));
    memcpy(base + *at + 2, data, n);
    d->Offset = *at; d->Length = 2 + n; d->Status = 0; *at += 2 + n;
}
/* Transfer-shape planner: exhaustive small-domain sweep against an
 * independent restatement of the section 5.4 rules, then the physical
 * Brio 100 configuration fixture through the actual descriptor parser. */
static void Plans(void)
{
    UVC_ISO_PLAN p, keep;
    unsigned int speed, type, bytes, tr, svc, urbs, ok, expect;
    char path[MAX_PATH], *slash; FILE *f; unsigned char config[4096];
    size_t n; UVC_DESC *d; unsigned int i, planned = 0, wide = 0;
    memset(&keep, 0x5A, sizeof(keep));
    CHECK(!UvcIsoPlan(UVC_ISO_HIGH_SPEED, 1, 1024, 1, 1, 6, NULL));
    for (speed = 0; speed <= 3; speed++)
    for (type = 0; type <= 3; type++)
    for (bytes = 0; bytes <= 1100; bytes += (bytes < 8 || bytes > 1016) ? 1 : 101)
    for (tr = 0; tr <= 4; tr++)
    for (svc = 0; svc <= 16; svc++)
    for (urbs = 0; urbs <= 8; urbs++) {
        p = keep;
        ok = UvcIsoPlan(speed, type, bytes, tr, svc, urbs, &p);
        expect = type == 1 && urbs && bytes && tr >= 1 && tr <= 3 &&
            ((speed == UVC_ISO_FULL_SPEED && svc == 8 && tr == 1 && bytes <= 1023) ||
             (speed == UVC_ISO_HIGH_SPEED && svc == 1 && bytes <= 1024));
        CHECK(ok == expect);
        if (!ok) { CHECK(!memcmp(&p, &keep, sizeof(p))); continue; }
        if (speed == UVC_ISO_FULL_SPEED) {
            CHECK(p.PacketsPerUrb == 16 && p.Urbs == (urbs < 3 ? urbs : 3) &&
                p.PacketCapacity == bytes && p.PacketStride == bytes &&
                p.UrbMicroseconds == 16000);
        } else {
            CHECK(p.PacketsPerUrb == 8 && p.Urbs == (urbs < 6 ? urbs : 6) &&
                p.PacketCapacity == bytes * tr &&
                p.UrbMicroseconds == 1000);
        }
        /* USBD's implied packet length (the offset distance) is the
         * capacity: never more than the endpoint carries per (micro)frame. */
        CHECK(p.PacketStride == p.PacketCapacity);
        CHECK(p.UrbBytes == (unsigned long)p.PacketsPerUrb * p.PacketStride);
    }
    /* Physical fixture: every Brio 100 isochronous alternate gets a plan. */
    lstrcpyn(path, __FILE__, sizeof(path));
    slash = strrchr(path, '\\');
    if (!Check(slash != NULL, "source path known", __LINE__)) return;
    lstrcpy(slash + 1, "testdata\\brio100\\CONFIG.BIN");
    f = fopen(path, "rb");
    if (!Check(f != NULL, "Brio 100 CONFIG.BIN opens", __LINE__)) return;
    n = fread(config, 1, sizeof(config), f); fclose(f);
    CHECK(n == 2453);
    d = (UVC_DESC *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!Check(d != NULL, "descriptor model allocated", __LINE__)) return;
    CHECK(UvcDescParse(config, (unsigned int)n, 0, UVC_DESC_HIGH_SPEED, d) == UVC_DESC_OK && d->Valid);
    CHECK(d->AltCount == 11);
    for (i = 0; i < d->AltCount; i++) {
        const UVC_DESC_ALT *a = &d->Alt[i];
        ok = UvcIsoPlan(UVC_ISO_HIGH_SPEED, a->Type, a->PacketBytes, a->Transactions,
            a->ServiceMicroframes, 6, &p);
        CHECK(ok);
        if (!ok) continue;
        planned++;
        if (a->Transactions > 1) wide++;
        CHECK(p.PacketCapacity == a->Capacity);
        printf("Brio100 alt %u: %u x %u bytes/uframe -> %u URBs x %u packets, stride %u, %lu bytes per URB\n",
            a->Alternate, a->Transactions, a->PacketBytes, p.Urbs, p.PacketsPerUrb,
            p.PacketStride, p.UrbBytes);
    }
    CHECK(planned == 11 && wide > 0);
    HeapFree(GetProcessHeap(), 0, d);
}

int main(void)
{
    SYSTEM_INFO info; DWORD old;
    unsigned char storage[64], scratch[512], *buf;
    UVC_PAY pay; UVC_ISO_STATS st; UVC_ISO_PACKET d[8];
    const unsigned char a[] = { 1, 2, 3 }, b[] = { 4, 5 }, c[] = { 6 }, e[] = { 7, 8, 9, 10 };
    unsigned long at, total; unsigned int i;
    GetSystemInfo(&info); pageSize = info.dwPageSize;
    page = VirtualAlloc(NULL, pageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!page || !VirtualProtect(page + pageSize, pageSize, PAGE_NOACCESS, &old)) return 2;

    /* Argument validation changes nothing. */
    memset(&st, 0, sizeof(st));
    CHECK(UvcPayInit(&pay, storage, sizeof(storage), 0, Frame, NULL));
    CHECK(!UvcIsoFeed(NULL, &st, scratch, 1, 0, d, 1, 1, 1));
    CHECK(!UvcIsoFeed(&pay, NULL, scratch, 1, 0, d, 1, 1, 1));
    CHECK(!UvcIsoFeed(&pay, &st, NULL, 1, 0, d, 1, 1, 1));
    CHECK(!UvcIsoFeed(&pay, &st, scratch, 1, 0, NULL, 1, 1, 1));
    CHECK(st.Urbs == 0);
    CHECK(UvcIsoFeed(&pay, &st, NULL, 0, 0, NULL, 0, 1, 1) && st.Urbs == 1);

    /* Sync FID 0, then frame FID 1 = a+b (EOF), then FID 0 starts c. */
    at = 0; total = 2 + 2 + 3 + 2 + 2 + 2 + 1;
    buf = scratch; memset(&st, 0, sizeof(st)); frames = 0;
    CHECK(UvcPayInit(&pay, storage, sizeof(storage), 0, Frame, NULL));
    Put(buf, &at, &d[0], 0, 0, NULL, 0);
    Put(buf, &at, &d[1], 1, 0, a, 3);
    Put(buf, &at, &d[2], 1, 1, b, 2);
    Put(buf, &at, &d[3], 0, 0, c, 1);
    CHECK(at == total);
    memcpy(Place(at), buf, at); buf = Place(at);
    CHECK(UvcIsoFeed(&pay, &st, buf, at, 16, d, 4, 1, 1));
    CHECK(frames == 1 && lastBytes == 5 && !memcmp(lastFrame, "\1\2\3\4\5", 5));
    CHECK(st.Packets == 4 && st.Fed == 4 && !st.Failed && !st.Invalid && !st.Empty);

    /* Empty successful packets are ignored; they do not break a frame. */
    d[0].Offset = 0; d[0].Length = 0; d[0].Status = 0;
    at = 0; buf = scratch;
    Put(buf, &at, &d[1], 0, 1, e, 4); /* finishes FID0 frame c+e */
    memcpy(Place(at), buf, at); buf = Place(at);
    CHECK(UvcIsoFeed(&pay, &st, buf, at, 16, d, 2, 1, 1));
    CHECK(frames == 2 && lastBytes == 5 && !memcmp(lastFrame, "\6\7\10\11\12", 5));
    CHECK(st.Empty == 1);

    /* A failed packet inside a frame drops that frame and resynchronizes. */
    at = 0; buf = scratch; frames = 0;
    Put(buf, &at, &d[0], 1, 0, a, 3);
    Put(buf, &at, &d[1], 1, 0, b, 2); d[1].Status = 0xC0000011;
    Put(buf, &at, &d[2], 1, 1, c, 1);
    Put(buf, &at, &d[3], 0, 0, a, 3);
    Put(buf, &at, &d[4], 0, 1, b, 2);
    Put(buf, &at, &d[5], 1, 0, c, 1);
    Put(buf, &at, &d[6], 1, 1, e, 4);
    memcpy(Place(at), buf, at); buf = Place(at);
    CHECK(UvcIsoFeed(&pay, &st, buf, at, 16, d, 7, 1, 1));
    /* Loss marks the FID1 frame bad; the next surviving payload (FID1 c+EOF)
     * is only a sync point; FID0 a+b and FID1 c+e are then delivered. */
    CHECK(st.Failed == 1 && frames == 2 && lastBytes == 5 && !memcmp(lastFrame, "\6\7\10\11\12", 5));

    /* Out-of-bounds and oversized packets are never read; they report loss. */
    at = 0; buf = scratch; frames = 0; memset(&st, 0, sizeof(st));
    Put(buf, &at, &d[0], 0, 0, a, 3);
    memcpy(Place(at), buf, at); buf = Place(at);
    d[1].Offset = at; d[1].Length = 1; d[1].Status = 0;      /* starts at end */
    d[2].Offset = 0; d[2].Length = at + 1; d[2].Status = 0;  /* runs past end */
    d[3].Offset = 0xFFFFFFFFUL; d[3].Length = 2; d[3].Status = 0; /* wrap */
    d[4].Offset = 0; d[4].Length = at; d[4].Status = 0;      /* > maxPacket 4 */
    CHECK(UvcIsoFeed(&pay, &st, buf, at, 4, d, 5, 1, 1));
    CHECK(st.Invalid == 5 && st.Fed == 0 && frames == 0); /* d0 also exceeds maxPacket 4 */

    /* Whole-transfer failure: no packet data is consumed, loss reported. */
    memset(&st, 0, sizeof(st));
    CHECK(UvcPayInit(&pay, storage, sizeof(storage), 0, Frame, NULL));
    at = 0; buf = scratch; frames = 0;
    Put(buf, &at, &d[0], 0, 0, NULL, 0);
    Put(buf, &at, &d[1], 1, 0, a, 3);
    memcpy(Place(at), buf, at); buf = Place(at);
    CHECK(UvcIsoFeed(&pay, &st, buf, at, 16, d, 2, 1, 1));
    CHECK(UvcIsoFeed(&pay, &st, NULL, 0, 16, NULL, 0, 0, 1)); /* failed URB */
    at = 0; buf = scratch;
    Put(buf, &at, &d[0], 1, 1, b, 2); /* would complete a+b without the failure */
    Put(buf, &at, &d[1], 0, 0, c, 1);
    Put(buf, &at, &d[2], 0, 1, e, 4);
    memcpy(Place(at), buf, at); buf = Place(at);
    CHECK(UvcIsoFeed(&pay, &st, buf, at, 16, d, 3, 1, 1));
    CHECK(st.FailedUrbs == 1 && frames == 1 && lastBytes == 5 && !memcmp(lastFrame, "\6\7\10\11\12", 5));

    /* A resubmission gap behaves like a lost packet before the transfer. */
    memset(&st, 0, sizeof(st));
    CHECK(UvcPayInit(&pay, storage, sizeof(storage), 0, Frame, NULL));
    at = 0; buf = scratch; frames = 0;
    Put(buf, &at, &d[0], 0, 0, NULL, 0);
    Put(buf, &at, &d[1], 1, 0, a, 3);
    memcpy(Place(at), buf, at); buf = Place(at);
    CHECK(UvcIsoFeed(&pay, &st, buf, at, 16, d, 2, 1, 1));
    at = 0; buf = scratch;
    Put(buf, &at, &d[0], 1, 1, b, 2);
    memcpy(Place(at), buf, at); buf = Place(at);
    CHECK(UvcIsoFeed(&pay, &st, buf, at, 16, d, 1, 1, 0));
    CHECK(st.Gaps == 1 && frames == 0);

    /* Saturating counters never wrap. */
    memset(&st, 0xFF, sizeof(st));
    CHECK(UvcIsoFeed(&pay, &st, NULL, 0, 0, NULL, 0, 0, 0));
    CHECK(st.Urbs == 0xFFFFFFFFUL && st.FailedUrbs == 0xFFFFFFFFUL && st.Gaps == 0xFFFFFFFFUL);

    /* Exhaustive small sweep: every packet layout of up to 4 payloads with every
     * status/length class against the guard page; must never fault. */
    for (i = 0; i < 4096; i++) {
        unsigned int k, n = 1 + (i & 3);
        memset(&st, 0, sizeof(st)); at = 0; buf = scratch;
        CHECK(UvcPayInit(&pay, storage, sizeof(storage), (i >> 11) & 1 ? 5 : 0, Frame, NULL));
        for (k = 0; k < n; k++) Put(buf, &at, &d[k], (i >> (2 + k)) & 1, (i >> (6 + k)) & 1, a, (i >> 10) & 1 ? 3 : 0);
        memcpy(Place(at), buf, at); buf = Place(at);
        if ((i >> 9) & 1) d[n - 1].Length += 1;          /* overrun guard */
        if ((i >> 8) & 1) d[0].Status = 1;
        CHECK(UvcIsoFeed(&pay, &st, buf, at, 0, d, n, 1, (i >> 7) & 1));
        CHECK(st.Packets == n && st.Fed + st.Failed + st.Invalid + st.Empty == n);
    }
    Plans();
    printf("%s: %u checks, %u failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

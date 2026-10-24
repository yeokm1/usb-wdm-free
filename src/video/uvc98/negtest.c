/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host tests of the actual uvcneg.c sequence over the actual descriptor and
 * control code, using the physical Brio 100 configuration fixture and a
 * scripted fake camera. No USB access.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "uvcneg.h"
static unsigned int checks, failures;
static int Check(int ok, const char *text, unsigned int line)
{
    checks++;
    if (!ok) { failures++; printf("FAIL line %u: %s\n", line, text); }
    return ok;
}
#define CHECK(x) Check((x) != 0, #x, __LINE__)

/* Scripted camera. mode selects one misbehaviour. */
enum { GOOD, SET_FAIL, GET_SHORT, BAD_FORMAT, HUGE_PAYLOAD, BAD_SIZE, COMMIT_FAIL, BAD_INTERVAL };
typedef struct CAM {
    const UVC_DESC *d; unsigned int mode, revision, size, iface;
    unsigned long frameBytes, payload;
    unsigned char probe[48], committed[48];
    unsigned int sets, gets, commits, order[8], count, sawSize;
} CAM;
static int Xfer(void *context, unsigned int request, unsigned int selector,
    unsigned int iface, unsigned char *buffer, unsigned int bytes)
{
    CAM *c = (CAM *)context; UVC_CTRL v;
    if (c->count < 8) c->order[c->count] = request << 8 | selector;
    c->count++;
    if (iface != c->iface || bytes != c->size) c->sawSize = 0;
    if (request == UVC_NEG_SET_CUR && selector == UVC_NEG_PROBE) {
        c->sets++;
        if (c->mode == SET_FAIL) return -1;
        memcpy(c->probe, buffer, bytes); return (int)bytes;
    }
    if (request == UVC_NEG_GET_CUR && selector == UVC_NEG_PROBE) {
        c->gets++;
        if (!UvcCtrlDecode(c->revision, c->probe, bytes, &v)) return -1;
        v.FrameBytes = c->frameBytes; v.PayloadBytes = c->payload;
        if (c->mode == BAD_FORMAT) v.Format = 9;
        if (c->mode == HUGE_PAYLOAD) v.PayloadBytes = 3061;
        if (c->mode == BAD_SIZE) v.FrameBytes = c->frameBytes + 2;
        if (c->mode == BAD_INTERVAL) v.Interval = 1234567;
        if (!UvcCtrlEncode(c->revision, &v, buffer, bytes)) return -1;
        return c->mode == GET_SHORT ? (int)bytes - 1 : (int)bytes;
    }
    if (request == UVC_NEG_SET_CUR && selector == UVC_NEG_COMMIT) {
        c->commits++;
        memcpy(c->committed, buffer, bytes);
        return c->mode == COMMIT_FAIL ? -1 : (int)bytes;
    }
    return -1;
}
static unsigned int MinAlt(const UVC_DESC *d, unsigned int stream, unsigned long payload)
{
    unsigned int i, best = UVC_DESC_MAX_ALTS; unsigned long cap = 0xffffffffUL;
    for (i = 0; i < d->AltCount; i++)
        if (d->Alt[i].Stream == stream && d->Alt[i].Capacity >= payload && d->Alt[i].Capacity < cap) {
            cap = d->Alt[i].Capacity; best = i;
        }
    return best;
}
int main(void)
{
    char path[MAX_PATH], *slash; FILE *f; unsigned char config[4096]; size_t n;
    UVC_DESC *d; UVC_NEG_RESULT r; CAM cam; unsigned int i, mode, ok = 0;
    static const unsigned long payloads[] = { 2, 192, 193, 1024, 2048, 3060 };
    lstrcpyn(path, __FILE__, sizeof(path)); slash = strrchr(path, '\\');
    if (!slash) return 2;
    lstrcpy(slash + 1, "testdata\\brio100\\CONFIG.BIN");
    f = fopen(path, "rb"); if (!f) { printf("FAIL: fixture %s\n", path); return 2; }
    n = fread(config, 1, sizeof(config), f); fclose(f);
    d = (UVC_DESC *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) return 2;
    CHECK(UvcDescParse(config, (unsigned int)n, 0, UVC_DESC_HIGH_SPEED, d) == UVC_DESC_OK);
    CHECK(d->Revision == 0x0100 && UvcCtrlSize(d->Revision) == 26);

    /* Arguments: nothing is sent. */
    memset(&cam, 0, sizeof(cam));
    CHECK(UvcNegotiate(d, 0, 0, Xfer, &cam, NULL) == UVC_NEG_ARG);
    CHECK(UvcNegotiate(NULL, 0, 0, Xfer, &cam, &r) == UVC_NEG_ARG && r.Requests == 0);
    CHECK(UvcNegotiate(d, d->FrameCount, 0, Xfer, &cam, &r) == UVC_NEG_ARG);
    CHECK(UvcNegotiate(d, 0, 0, NULL, &cam, &r) == UVC_NEG_ARG);
    CHECK(UvcNegotiate(d, 0, 1, Xfer, &cam, &r) == UVC_NEG_ARG); /* unlisted interval */
    CHECK(cam.count == 0);

    /* Every frame of the real camera, a range of device payload answers. */
    for (i = 0; i < d->FrameCount; i++) {
        unsigned int p;
        for (p = 0; p < sizeof(payloads) / sizeof(payloads[0]); p++) {
            UVC_CTRL sent, committed;
            memset(&cam, 0, sizeof(cam));
            cam.d = d; cam.mode = GOOD; cam.revision = d->Revision; cam.size = 26;
            cam.iface = d->Stream[d->Frame[i].Stream].Interface; cam.sawSize = 1;
            cam.frameBytes = d->Frame[i].Kind == UVC_DESC_MJPEG ? 777777 : d->Frame[i].FrameBytes;
            cam.payload = payloads[p];
            CHECK(UvcNegotiate(d, i, 0, Xfer, &cam, &r) == UVC_NEG_OK);
            CHECK(cam.count == 3 && r.Requests == 3 && cam.sawSize);
            CHECK(cam.order[0] == (UVC_NEG_SET_CUR << 8 | UVC_NEG_PROBE) &&
                cam.order[1] == (UVC_NEG_GET_CUR << 8 | UVC_NEG_PROBE) &&
                cam.order[2] == (UVC_NEG_SET_CUR << 8 | UVC_NEG_COMMIT));
            CHECK(UvcCtrlDecode(d->Revision, cam.probe, 26, &sent));
            CHECK(sent.Hint == 1 && sent.Format == d->Frame[i].Format &&
                sent.Frame == d->Frame[i].Frame && sent.Interval == d->Frame[i].DefaultInterval &&
                !sent.FrameBytes && !sent.PayloadBytes);
            /* COMMIT carries the device answer, not our request. */
            CHECK(UvcCtrlDecode(d->Revision, cam.committed, 26, &committed));
            CHECK(committed.PayloadBytes == payloads[p] && committed.FrameBytes == cam.frameBytes);
            CHECK(r.FrameIndex == i && r.AltIndex == MinAlt(d, d->Frame[i].Stream, payloads[p]));
            CHECK(r.Control.PayloadBytes == payloads[p]);
            ok++;
        }
    }
    /* Payload limit quirk: the committed bytes and the alternate follow the
     * limit; the device answer is kept; a limit above the answer changes nothing. */
    for (i = 0; i < d->FrameCount; i++) {
        static const unsigned long limits[] = { 192, 640, 944, 3060, 4000 };
        unsigned int l;
        for (l = 0; l < sizeof(limits) / sizeof(limits[0]); l++) {
            UVC_CTRL committed;
            memset(&cam, 0, sizeof(cam));
            cam.d = d; cam.mode = GOOD; cam.revision = d->Revision; cam.size = 26;
            cam.iface = d->Stream[d->Frame[i].Stream].Interface; cam.sawSize = 1;
            cam.frameBytes = d->Frame[i].Kind == UVC_DESC_MJPEG ? 777777 : d->Frame[i].FrameBytes;
            cam.payload = 3060;
            CHECK(UvcNegotiateLimited(d, i, 0, limits[l], Xfer, &cam, &r) == UVC_NEG_OK);
            CHECK(UvcCtrlDecode(d->Revision, cam.committed, 26, &committed));
            CHECK(r.DeviceControl.PayloadBytes == 3060);
            CHECK(committed.PayloadBytes == (limits[l] < 3060 ? limits[l] : 3060));
            CHECK(r.Control.PayloadBytes == committed.PayloadBytes);
            CHECK(r.AltIndex == MinAlt(d, d->Frame[i].Stream, committed.PayloadBytes));
        }
    }
    /* Misbehaving camera: nothing is committed unless every step succeeded. */
    for (mode = SET_FAIL; mode <= BAD_INTERVAL; mode++) {
        int expect = mode == SET_FAIL || mode == GET_SHORT || mode == COMMIT_FAIL ? UVC_NEG_IO : UVC_NEG_REJECTED;
        unsigned int sent = mode == SET_FAIL ? 1 : mode == COMMIT_FAIL ? 3 : 2;
        memset(&cam, 0, sizeof(cam));
        cam.d = d; cam.mode = mode; cam.revision = d->Revision; cam.size = 26;
        cam.iface = d->Stream[d->Frame[0].Stream].Interface;
        cam.frameBytes = d->Frame[0].Kind == UVC_DESC_MJPEG ? 777777 : d->Frame[0].FrameBytes;
        cam.payload = 1024;
        CHECK(UvcNegotiate(d, 0, 0, Xfer, &cam, &r) == expect);
        CHECK(r.Requests == sent && cam.count == sent);
        CHECK(cam.commits == (mode == COMMIT_FAIL ? 1u : 0u));
        CHECK(!r.Control.PayloadBytes && !r.AltIndex && !r.FrameIndex);
    }
    printf("%s: %u checks, %u failures; %u good negotiations over %u Brio 100 frames\n",
        failures ? "FAIL" : "PASS", checks, failures, ok, d->FrameCount);
    HeapFree(GetProcessHeap(), 0, d);
    return failures ? 1 : 0;
}

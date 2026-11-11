/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host executable, built with UAC_V1 as the driver is: the USB Audio 1.0
 * parser (uac1desc.c), its shared model (uac1model.c) and the driver core
 * that runs on it (src/audio/common: uacplan.c, uacvol.c, uacrate.c), on
 * two recorded devices (the Brio 100 microphone, the Logi USB Headset),
 * QEMU 11.1.0's usb-audio layout, an asynchronous headset, a MIDI-only
 * function, a mixed UAC 1.0 / 2.0 configuration and hostile variants;
 * the converted formats, MaxPacketsOnly and the synch value (uacconv.c,
 * docs/design/uac2.md 26.12).
 * usage: uac1test <brio100.bin> <logi0a64.bin> <play2.bin>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uac1desc.h"
#include "uacfmt.h"
#include "uacdesc.h"
#include "uacplan.h"
#include "uacvol.h"
#include "uacrate.h"
#include "uacconv.h"

static unsigned int checks;
static void Check(int ok, const char *expression, unsigned int line)
{
    checks++;
    if (!ok) { printf("FAIL line %u: %s\n", line, expression); exit(1); }
}
#define CHECK(x) Check((x), #x, __LINE__)

static UAC1_DESC d;
static UAC_DESC m;
static unsigned char brio[4096], logi[4096], play2[4096], work[4096];
static unsigned int brioLen, logiLen, play2Len;

static unsigned int Load(const char *path, unsigned char *buffer, unsigned int size)
{
    FILE *f = fopen(path, "rb");
    unsigned int n;
    if (!f) { printf("FAIL cannot open %s\n", path); exit(1); }
    n = (unsigned int)fread(buffer, 1, size, f);
    fclose(f);
    return n;
}

/* QEMU 11.1.0 hw/usb/dev-audio.c, the stereo (non-multi) configuration,
 * transcribed from its descriptor tables; strings as indices 4..10. */
static const unsigned char qemu[] = {
    9, 2, 0x71, 0, 2, 1, 0, 0xc0, 0x32,
    9, 4, 0, 0, 0, 1, 1, 0, 4,
    9, 0x24, 1, 0, 1, 0x2b, 0, 1, 1,
    12, 0x24, 2, 1, 0x01, 0x01, 0, 2, 3, 0, 0, 5,
    13, 0x24, 6, 2, 1, 2, 1, 0, 2, 0, 2, 0, 6,
    9, 0x24, 3, 3, 0x01, 0x03, 0, 2, 7,
    9, 4, 1, 0, 0, 1, 2, 0, 8,
    9, 4, 1, 1, 1, 1, 2, 0, 9,
    7, 0x24, 1, 1, 0, 1, 0,
    11, 0x24, 2, 1, 2, 2, 16, 1, 0x80, 0xbb, 0,
    9, 5, 0x01, 0x0d, 0xc0, 0, 1, 0, 0,
    7, 0x25, 1, 0, 0, 0, 0
};

/* An asynchronous headset: playback 44.1/48 kHz stereo with a 3-byte
 * 10.14 synch endpoint named by bSynchAddress. */
static const unsigned char async1[] = {
    9, 2, 0x70, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 1, 1, 0, 0,
    9, 0x24, 1, 0, 1, 0x1e, 0, 1, 1,
    12, 0x24, 2, 1, 0x01, 0x01, 0, 2, 3, 0, 0, 0,
    9, 0x24, 3, 2, 0x02, 0x03, 0, 1, 0,
    9, 4, 1, 0, 0, 1, 2, 0, 0,
    9, 4, 1, 1, 2, 1, 2, 0, 0,
    7, 0x24, 1, 1, 1, 1, 0,
    14, 0x24, 2, 1, 2, 2, 16, 2, 0x44, 0xac, 0, 0x80, 0xbb, 0,
    9, 5, 0x02, 0x05, 0xc8, 0, 1, 0, 0x82,
    7, 0x25, 1, 1, 0, 0, 0,
    9, 5, 0x82, 0x01, 3, 0, 1, 3, 0
};

/* A MIDI-only function: AudioControl (header only, collecting interface 1)
 * and a MIDIStreaming interface with two bulk endpoints. */
static const unsigned char midi[] = {
    9, 2, 0x42, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 1, 1, 0, 0,
    9, 0x24, 1, 0, 1, 9, 0, 1, 1,
    9, 4, 1, 0, 2, 1, 3, 0, 0,
    7, 0x24, 1, 0, 1, 0x13, 0,
    9, 5, 0x02, 2, 64, 0, 0, 0, 0,
    5, 0x25, 1, 1, 1,
    9, 5, 0x81, 2, 64, 0, 0, 0, 0
};

static void Model(const unsigned char *b, unsigned int n, unsigned int control, int hs)
{
    CHECK(Uac1Parse(b, n, control, hs, &d) == UAC1_OK);
    CHECK(Uac1Model(&d, hs, &m) == UAC1_OK);
    CHECK(m.Bytes == b && m.Control == control && m.Speed == (hs ? 2U : 1U));
}

static void Brio(void)
{
    unsigned int first, i, index;
    UAC_FORMAT f[8];
    CHECK(Uac1View(brio, brioLen, 0) == UAC1_VIEW_NONE);   /* UVC */
    CHECK(Uac1View(brio, brioLen, 2) == UAC1_VIEW_CONTROL);
    CHECK(Uac1View(brio, brioLen, 3) == UAC1_VIEW_STREAMING);
    CHECK(Uac1WholeFirst(brio, brioLen, &first) == UAC1_OK && first == 2);
    CHECK(Uac1Parse(brio, brioLen, 0, 1, &d) == UAC1_NO_AUDIO);
    Model(brio, brioLen, 2, 1);
    CHECK(m.AltCount == 3 && m.Skipped == 0);
    for (i = 0; i < 3; i++) {
        const UAC_ALT *a = &m.Alt[i];
        CHECK(a->Interface == 3 && a->Alternate == i + 1 && (a->DataEndpoint & 0x80));
        CHECK(a->Channels == 1 && a->Subslot == 2 && a->Bits == 16 && a->Terminal == 3);
        CHECK(a->Interval == 4 && a->RateCount == 1 && a->EpControls == UAC_EP_SAMFREQ);
        CHECK(!a->FeedbackEndpoint && a->ChannelConfig == 3); /* IT 1: one channel, L+R named */
    }
    CHECK(m.Alt[0].Rate[0] == 16000 && m.Alt[1].Rate[0] == 32000 && m.Alt[2].Rate[0] == 48000);
    /* One capture format (mono 16-bit) whatever the rate; each rate has
     * its own alternate. 48 kHz at High Speed bInterval 4 (1 ms): 48
     * frames + 1 (asynchronous) = 98 bytes <= 100. */
    CHECK(UacPlanFormats(&m, 1, 1, f, 8) == 1 && f[0].Channels == 1 && f[0].Terminal == 3);
    CHECK(UacPlanFormats(&m, 0, 1, f, 8) == 0);
    CHECK(UacPlanPick(&m, 1, 3, 1, 2, 16, UAC_PLAN_ANY_CONFIG, 48000, 1, &index) == UAC_OK && index == 2);
    CHECK(UacPlanPick(&m, 1, 3, 1, 2, 16, UAC_PLAN_ANY_CONFIG, 16000, 1, &index) == UAC_OK && index == 0);
    CHECK(UacPlanPick(&m, 1, 3, 1, 2, 16, UAC_PLAN_ANY_CONFIG, 44100, 1, &index) == UAC_NO_AUDIO);
    CHECK(UacVolJack(&m, 1, 3) == 0x0201);
    /* At Full Speed bInterval 4 is illegal: every alternate is skipped. */
    CHECK(Uac1Parse(brio, brioLen, 2, 0, &d) == UAC1_NO_AUDIO && d.Skipped == 3);
}

static void Logi(void)
{
    unsigned int index;
    UAC_FORMAT f[8];
    UAC_VOL_MAP v;
    const UAC_ALT *in, *out;
    Model(logi, logiLen, 0, 0);
    CHECK(m.AltCount == 2 && m.Skipped == 0 && m.Count == 3);
    in = &m.Alt[0]; out = &m.Alt[1];
    CHECK(in->Interface == 1 && (in->DataEndpoint & 0x80) && in->Channels == 1 && in->RateCount == 5);
    CHECK(in->Terminal == 3 && in->ChannelConfig == 0 && UacPlanSync(in) == 1);
    CHECK(out->Interface == 2 && out->DataEndpoint == 0x01 && out->Channels == 2 && out->RateCount == 7);
    CHECK(out->Terminal == 4 && out->ChannelConfig == 0 && UacPlanSync(out) == 2 && !out->FeedbackEndpoint);
    CHECK(out->RateMin == 8000 && out->RateMax == 192000 && out->MaxPacket == 768);
    CHECK(UacPlanHasRate(out, 96000) && !UacPlanHasRate(out, 88200) && !UacPlanHasRate(out, 0));
    CHECK(UacPlanHasRate(in, 44100) && !UacPlanHasRate(in, 96000));
    /* Playback 192 kHz stereo needs exactly the 768 bytes offered
     * (adaptive: nominal cadence, no headroom); 11025 is not listed. */
    CHECK(UacPlanPick(&m, 0, 4, 2, 2, 16, 0, 192000, 0, &index) == UAC_OK && index == 1);
    CHECK(UacPlanPick(&m, 0, 4, 2, 2, 16, 0, 44100, 0, &index) == UAC_OK && index == 1);
    CHECK(UacPlanPick(&m, 0, 4, 2, 2, 16, 0, 11025, 0, &index) == UAC_NO_AUDIO);
    CHECK(UacPlanPick(&m, 1, 3, 1, 2, 16, 0, 48000, 0, &index) == UAC_OK && index == 0);
    CHECK(UacPlanPick(&m, 1, 3, 1, 2, 16, 0, 96000, 0, &index) == UAC_NO_AUDIO);
    /* IT 4 names one channel and no positions for a stereo stream. */
    CHECK(UacPlanFormats(&m, 0, 0, f, 8) == 1 && f[0].ChannelConfig == 0);
    CHECK(UacPlanChannelMask(f[0].ChannelConfig, 2) == 0);
    /* Volume: IT 4 -> Mixer 12 -> FU 5 (master mute and volume) -> OT 6
     * (speaker, 0x0301). The stalled Mixer is traversed, never read. */
    CHECK(UacVolFind(&m, 4, 2, &v) == 1 && v.Output == 6);
    CHECK(v.VolumeUnit == 5 && v.VolumeMode == UAC_VOL_MASTER && v.VolumeExtra == 0);
    CHECK(v.MuteUnit == 5 && v.MuteMode == UAC_VOL_MASTER);
    CHECK(UacVolJack(&m, 0, 4) == 0x0301);
    CHECK(UacVolJack(&m, 1, 3) == 0x0201);
    CHECK(UacPlanEndpoints(&m, 0, 0, &index, 1) == 1 && index == 4);
    /* The HID interface and the AS interfaces are not functions. */
    CHECK(Uac1View(logi, logiLen, 3) == UAC1_VIEW_NONE);
    CHECK(Uac1View(logi, logiLen, 1) == UAC1_VIEW_STREAMING);
}

static void Qemu(void)
{
    unsigned int first, index;
    UAC_VOL_MAP v;
    UAC_RATE rate;
    unsigned int i, total = 0;
    CHECK(Uac1WholeFirst(qemu, sizeof(qemu), &first) == UAC1_OK && first == 0);
    Model(qemu, sizeof(qemu), 0, 0);
    CHECK(m.AltCount == 1 && UacPlanSync(&m.Alt[0]) == 3 && m.Alt[0].MaxPacket == 192);
    CHECK(m.Alt[0].ChannelConfig == 3 && m.Alt[0].EpControls == 0);
    CHECK(UacPlanPick(&m, 0, 1, 2, 2, 16, 3, 48000, 0, &index) == UAC_OK);
    CHECK(UacPlanPick(&m, 0, 1, 2, 2, 16, 3, 44100, 0, &index) == UAC_NO_AUDIO);
    /* FU 2: master mute, volume on channels 1 and 2. */
    CHECK(UacVolFind(&m, 1, 2, &v) == 1 && v.Output == 3);
    CHECK(v.MuteUnit == 2 && v.MuteMode == UAC_VOL_MASTER);
    CHECK(v.VolumeUnit == 2 && v.VolumeMode == UAC_VOL_CHANNEL && v.VolumeChannels == 2);
    /* The nominal cadence: 192 bytes every frame. */
    CHECK(UacRateInitialize(&rate, 48000, 0, 1, 4, 192));
    for (i = 0; i < 1000; i++) { unsigned int b = UacRateNextBytes(&rate); CHECK(b == 192); total += b; }
    CHECK(total == 192000);
}

static void Async(void)
{
    unsigned int index, i, sum;
    UAC_FORMAT f[4];
    UAC_RATE rate;
    unsigned char raw[4];
    Model(async1, sizeof(async1), 0, 0);
    CHECK(m.AltCount == 1 && m.Alt[0].DataEndpoint == 0x02 && m.Alt[0].FeedbackEndpoint == 0x82);
    CHECK(m.Alt[0].FeedbackMaxPacket == 3 && m.Alt[0].FeedbackInterval == 1 && UacPlanSync(&m.Alt[0]) == 1);
    CHECK(UacPlanFormats(&m, 0, 0, f, 4) == 1);
    /* 48 kHz asynchronous: 48 + 1 frames of 4 bytes = 196 <= 200. */
    CHECK(UacPlanPick(&m, 0, 1, 2, 2, 16, 3, 48000, 0, &index) == UAC_OK);
    CHECK(UacRateFeedbackStride(0, 3) == 3);
    /* A zero-length feedback packet before any value is no news: the
     * nominal cadence goes on (QEMU's passthrough answers so at first). */
    CHECK(UacRateInitialize(&rate, 44100, 0, 1, 4, 200));
    memset(raw, 0, sizeof(raw));
    CHECK(UacRateFeedbackPacket(&rate, raw, 3, 0, 0, 3) == UAC_FEEDBACK_EMPTY && !rate.FeedbackActive);
    for (i = 0, sum = 0; i < 1000; i++) sum += UacRateNextBytes(&rate) / 4;
    CHECK(sum == 44100);
    /* Then 10.14: 44.1 samples per frame = 0x0B0666. */
    raw[0] = 0x66; raw[1] = 0x06; raw[2] = 0x0b;
    CHECK(UacRateFeedbackPacket(&rate, raw, 3, 0, 3, 3) == UAC_FEEDBACK_VALUE);
    for (i = 0, sum = 0; i < 1000; i++) sum += UacRateNextBytes(&rate) / 4;
    CHECK(sum >= 44099 && sum <= 44101);
    /* Zero-length after a value keeps it; a value outside 12.5% does not. */
    CHECK(UacRateFeedbackPacket(&rate, raw, 3, 0, 0, 3) == UAC_FEEDBACK_KEEPALIVE);
    raw[2] = 0x20;
    CHECK(UacRateFeedbackPacket(&rate, raw, 3, 0, 3, 3) == UAC_FEEDBACK_BAD);
    /* A synch endpoint the data endpoint does not name: the asynchronous
     * sink has no feedback, and is not carried. */
    memcpy(work, async1, sizeof(async1));
    work[sizeof(async1) - 25 + 8] = 0;
    CHECK(Uac1Parse(work, sizeof(async1), 0, 0, &d) == UAC1_NO_AUDIO && d.Skipped == 1);
}

static void Midi(void)
{
    CHECK(Uac1View(midi, sizeof(midi), 0) == UAC1_VIEW_CONTROL);
    CHECK(Uac1View(midi, sizeof(midi), 1) == UAC1_VIEW_MIDI);
    CHECK(Uac1Parse(midi, sizeof(midi), 0, 0, &d) == UAC1_OK && d.MidiCount == 1 && !d.AltCount);
    /* No audio alternate: the driver starts it as a null function. */
    CHECK(Uac1Model(&d, 0, &m) == UAC1_NO_AUDIO && m.AltCount == 0 && m.Control == 0);
}

/* A configuration with a UAC 1.0 function (QEMU's, interfaces 0 and 1) and
 * a UAC 2.0 AudioControl interface 2 after it: each function is its own. */
static void Mixed(void)
{
    static const unsigned char tail[] = { 9, 4, 2, 0, 0, 1, 1, 0x20, 0 };
    unsigned int n = sizeof(qemu), first;
    memcpy(work, qemu, n);
    memcpy(work + n, tail, sizeof(tail));
    n += sizeof(tail);
    work[2] = (unsigned char)n; work[3] = (unsigned char)(n >> 8); work[4] = 3;
    CHECK(Uac1View(work, n, 2) == UAC1_VIEW_NONE);
    CHECK(Uac1WholeFirst(work, n, &first) == UAC1_OK && first == 0);
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_OK && d.AltCount == 1);
    CHECK(Uac1Parse(work, n, 2, 0, &d) == UAC1_NO_AUDIO);
}

/* Layouts uac1 does not run: left out of the model (a null function when
 * nothing is left). */
/* A capture path through a Selector Unit: IT 1 (front pair) and IT 2
 * (another layout, byte 47) into SU 3, then OT 4 (USB streaming). */
static unsigned char selcfg[] = {
    9, 2, 120, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 1, 1, 0, 0,
    9, 0x24, 1, 0, 1, 50, 0, 1, 1,
    12, 0x24, 2, 1, 0x01, 0x02, 0, 2, 0x03, 0, 0, 0,
    12, 0x24, 2, 2, 0x03, 0x06, 0, 2, 0x30, 0, 0, 0,
    8, 0x24, 5, 3, 2, 1, 2, 0,
    9, 0x24, 3, 4, 0x01, 0x01, 0, 3, 0,
    9, 4, 1, 0, 0, 1, 2, 0, 0,
    9, 4, 1, 1, 1, 1, 2, 0, 0,
    7, 0x24, 1, 4, 1, 1, 0,
    11, 0x24, 2, 1, 2, 2, 16, 1, 0x80, 0xbb, 0,
    9, 5, 0x81, 0x05, 0xc8, 0, 1, 0, 0,
    7, 0x25, 1, 0, 0, 0, 0
};

static void Selector(void)
{
    CHECK(sizeof(selcfg) == 120 && selcfg[47] == 0x30);
    Model(selcfg, sizeof(selcfg), 0, 0);
    /* The inputs' layouts differ: none is named. */
    CHECK(m.AltCount == 1 && m.Alt[0].ChannelConfig == 0);
    selcfg[47] = 0x03;
    Model(selcfg, sizeof(selcfg), 0, 0);
    CHECK(m.Alt[0].ChannelConfig == 3);
    selcfg[47] = 0x30;
}

/* QEMU's layout with a 20-rate list, 48 kHz the 20th: every rate the
 * parser keeps reaches the model. */
static void ManyRates(void)
{
    unsigned int i, n = 0, o, k, index;
    for (i = 0; i < sizeof(qemu); i += qemu[i]) {
        if (qemu[i + 1] == 0x24 && qemu[i + 2] == 2 && qemu[i] == 11 && i > 60) {
            memcpy(work + n, qemu + i, 8);
            work[n] = 8 + 3 * 20; work[n + 7] = 20;
            o = n + 8;
            for (k = 0; k < 19; k++) { work[o++] = (unsigned char)(1000 + k); work[o++] = (unsigned char)((1000 + k) >> 8); work[o++] = 0; }
            work[o++] = 0x80; work[o++] = 0xbb; work[o++] = 0;
            n = o;
        } else {
            memcpy(work + n, qemu + i, qemu[i]);
            n += qemu[i];
        }
    }
    work[2] = (unsigned char)n; work[3] = (unsigned char)(n >> 8);
    Model(work, n, 0, 0);
    CHECK(m.Alt[0].RateCount == 20 && m.Alt[0].Rate[19] == 48000 && UacPlanHasRate(&m.Alt[0], 48000));
    CHECK(UacPlanPick(&m, 0, 1, 2, 2, 16, 3, 48000, 0, &index) == UAC_OK);
}

/* An adaptive source: capture on an ADAPTIVE IN endpoint whose
 * bSynchAddress names a synch OUT endpoint (ADC-1 3.7.2.2). */
static const unsigned char adsrc[] = {
    9, 2, 129, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 1, 1, 0, 0,
    9, 0x24, 1, 0, 1, 50, 0, 1, 1,
    12, 0x24, 2, 1, 0x01, 0x02, 0, 1, 0, 0, 0, 0,
    12, 0x24, 2, 2, 0x03, 0x06, 0, 1, 0, 0, 0, 0,
    8, 0x24, 5, 3, 2, 1, 2, 0,
    9, 0x24, 3, 4, 0x01, 0x01, 0, 3, 0,
    9, 4, 1, 0, 0, 1, 2, 0, 0,
    9, 4, 1, 1, 2, 1, 2, 0, 0,
    7, 0x24, 1, 4, 1, 1, 0,
    11, 0x24, 2, 1, 1, 2, 16, 1, 0x80, 0xbb, 0,
    9, 5, 0x81, 0x09, 0x62, 0, 1, 0, 0x02,
    7, 0x25, 1, 0, 0, 0, 0,
    9, 5, 0x02, 0x01, 3, 0, 1, 2, 0
};

static void Adaptive(void)
{
    unsigned int index;
    CHECK(sizeof(adsrc) == 129);
    Model(adsrc, sizeof(adsrc), 0, 0);
    /* The synch OUT endpoint is kept with the alternate; the driver
     * feeds it Ff (docs/design/uac2.md 26.12). */
    CHECK(m.AltCount == 1 && m.Alt[0].DataEndpoint == 0x81 && m.Alt[0].FeedbackEndpoint == 0x02);
    CHECK(UacPlanSync(&m.Alt[0]) == 2);
    CHECK(UacPlanPick(&m, 1, 4, 1, 2, 16, 0, 48000, 0, &index) == UAC_OK);
}

/* selcfg with SU 3 as 32 inputs that all name SU 3 itself: the layout
 * lookup ends promptly (a work budget), with no layout. */
static void SelectorCycle(void)
{
    unsigned int n = 0, i, k;
    for (i = 0; i < sizeof(selcfg); i += selcfg[i]) {
        if (selcfg[i + 1] == 0x24 && selcfg[i + 2] == 5) {
            work[n++] = 38; work[n++] = 0x24; work[n++] = 5; work[n++] = 3; work[n++] = 32;
            for (k = 0; k < 32; k++) work[n++] = 3;
            work[n++] = 0;
        } else {
            memcpy(work + n, selcfg + i, selcfg[i]);
            n += selcfg[i];
        }
    }
    work[2] = (unsigned char)n; work[3] = 0;
    work[18 + 5] = (unsigned char)(work[18 + 5] + 30);   /* AC header wTotalLength */
    CHECK(n == 150);
    Model(work, n, 0, 0);
    CHECK(m.AltCount == 1 && m.Alt[0].ChannelConfig == 0);
}

/* The owner's Sound Blaster Play! 2 (041E:323D, as read by uac1m):
 * asynchronous playback with a 3-byte synch endpoint (bRefresh 5), IT 2 ->
 * Mixer 9 -> FU 5 (master mute, volume per channel) -> OT 4 (speaker);
 * stereo capture IT 1 -> FU 6 -> SU 10 -> OT 3. */
static void Play2(void)
{
    unsigned int index;
    UAC_VOL_MAP v;
    const UAC_ALT *in, *out;
    Model(play2, play2Len, 0, 0);
    CHECK(m.AltCount == 2);
    in = &m.Alt[0]; out = &m.Alt[1];
    CHECK((in->DataEndpoint & 0x80) && in->Channels == 2 && in->Terminal == 3 && in->ChannelConfig == 3);
    CHECK(out->DataEndpoint == 0x01 && out->FeedbackEndpoint == 0x83 && out->FeedbackMaxPacket == 3);
    CHECK(UacPlanSync(out) == 1 && out->Terminal == 2 && out->ChannelConfig == 3 && out->RateCount == 2);
    CHECK(out->EpControls == UAC_EP_SAMFREQ && in->EpControls == UAC_EP_SAMFREQ);
    /* 48 kHz asynchronous: 49 frames of 4 bytes = 196 <= 200. */
    CHECK(UacPlanPick(&m, 0, 2, 2, 2, 16, 3, 48000, 0, &index) == UAC_OK && index == 1);
    CHECK(UacPlanPick(&m, 1, 3, 2, 2, 16, 3, 44100, 0, &index) == UAC_OK && index == 0);
    CHECK(UacPlanPick(&m, 0, 2, 2, 2, 16, 3, 32000, 0, &index) == UAC_NO_AUDIO);
    CHECK(UacVolFind(&m, 2, 2, &v) == 1 && v.Output == 4);
    CHECK(v.MuteUnit == 5 && v.MuteMode == UAC_VOL_MASTER);
    CHECK(v.VolumeUnit == 5 && v.VolumeMode == UAC_VOL_CHANNEL && v.VolumeChannels == 2);
    CHECK(UacVolJack(&m, 0, 2) == 0x0301 && UacVolJack(&m, 1, 3) == 0x0201);
}

/* The format tag of the qemu layout's one alternate, with its format
 * type descriptor's subframe and bits, and its wMaxPacketSize. */
static unsigned int QemuAs(unsigned int tag, unsigned int subframe, unsigned int bits, unsigned int maxPacket)
{
    unsigned int i, n = sizeof(qemu);
    memcpy(work, qemu, n);
    for (i = 9; i < n; i += work[i]) {
        if (work[i + 1] == 0x24 && work[i + 2] == 1 && work[i] == 7) {
            work[i + 5] = (unsigned char)tag; work[i + 6] = (unsigned char)(tag >> 8);
        }
        if (work[i + 1] == 0x24 && work[i + 2] == 2 && work[i] == 11) {
            work[i + 5] = (unsigned char)subframe; work[i + 6] = (unsigned char)bits;
        }
        if (work[i + 1] == 5 && work[i] == 9) {
            work[i + 4] = (unsigned char)maxPacket; work[i + 5] = (unsigned char)(maxPacket >> 8);
        }
    }
    return n;
}

/* Two playback terminals, the first with a float alternate only. */
static void Terminals(void)
{
    UAC_DESC t2;
    memset(&t2, 0, sizeof(t2));
    t2.AltCount = 2; t2.Speed = 1;
    t2.Alt[0].Interface = 1; t2.Alt[0].Alternate = 1; t2.Alt[0].Terminal = 1; t2.Alt[0].Channels = 2;
    t2.Alt[0].Subslot = 4; t2.Alt[0].Bits = 32; t2.Alt[0].FormatTag = UAC_TAG_FLOAT;
    t2.Alt[0].DataEndpoint = 0x01; t2.Alt[0].Attributes = 0x09; t2.Alt[0].Interval = 1; t2.Alt[0].MaxPacket = 392;
    t2.Alt[1] = t2.Alt[0];
    t2.Alt[1].Interface = 2; t2.Alt[1].Terminal = 4; t2.Alt[1].Subslot = 2; t2.Alt[1].Bits = 16;
    t2.Alt[1].FormatTag = UAC_TAG_PCM; t2.Alt[1].DataEndpoint = 0x02; t2.Alt[1].MaxPacket = 196;
    CHECK(UacPlanTerminal(&t2, 0, 0) == 1);
}

/* docs/design/uac2.md 26.12: the four converted formats are carried with
 * their tag; the pin view is 16-bit; UacPlanPick matches the tag. */
static void Formats(void)
{
    static const unsigned int tags[4] = { UAC_TAG_PCM8, UAC_TAG_FLOAT, UAC_TAG_ALAW, UAC_TAG_MULAW };
    unsigned int i, n, index, wire;
    UAC_FORMAT f[4];
    for (i = 0; i < 4; i++) {
        wire = tags[i] == UAC_TAG_FLOAT ? 4 : 1;
        n = QemuAs(tags[i], wire, wire * 8, 48 * 2 * wire);
        Model(work, n, 0, 0);
        CHECK(m.AltCount == 1 && m.Skipped == 0 && m.Alt[0].FormatTag == tags[i]);
        CHECK(m.Alt[0].Subslot == wire && m.Alt[0].Bits == wire * 8);
        CHECK(UacPlanFormats(&m, 0, 0, f, 4) == 1 && f[0].FormatTag == tags[i]);
        CHECK(f[0].Subslot == 2 && f[0].Bits == 16 && f[0].WireSubslot == wire && f[0].WireBits == wire * 8);
        CHECK(f[0].Channels == 2 && f[0].Terminal == 1);
        CHECK(UacPlanPick(&m, 0, 1, 2, wire, UAC_PLAN_BITS(tags[i], wire * 8), UAC_PLAN_ANY_CONFIG,
                          48000, 0, &index) == UAC_OK && index == 0);
        /* As PCM (tag 0 or 1 in the bits): not this alternate. */
        CHECK(UacPlanPick(&m, 0, 1, 2, wire, wire * 8, UAC_PLAN_ANY_CONFIG, 48000, 0, &index) == UAC_NO_AUDIO);
        CHECK(UacPlanPick(&m, 0, 1, 2, 2, UAC_PLAN_BITS(tags[i], 16), UAC_PLAN_ANY_CONFIG, 48000, 0, &index) ==
              UAC_NO_AUDIO);
    }
    /* A-law with 16 bits, a tag beyond MULAW, 8-bit PCM: not run. */
    n = QemuAs(UAC_TAG_ALAW, 2, 16, 192);
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_NO_AUDIO && d.Skipped == 1);
    n = QemuAs(6, 1, 8, 96);
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_NO_AUDIO && d.Skipped == 1);
    n = QemuAs(UAC_TAG_PCM, 1, 8, 96);
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_NO_AUDIO && d.Skipped == 1);
    n = QemuAs(UAC_TAG_FLOAT, 4, 24, 384);
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_NO_AUDIO && d.Skipped == 1);
    /* MaxPacketsOnly OUT: carried now. */
    n = QemuAs(UAC_TAG_PCM, 2, 16, 192);
    work[n - 7 + 3] = UAC_CONV_MPO;
    Model(work, n, 0, 0);
    CHECK(m.AltCount == 1 && m.Alt[0].EpControls == UAC_CONV_MPO && m.Alt[0].FormatTag == UAC_TAG_PCM);
    /* MaxPacketsOnly IN on the adaptive source: carried, its synch
     * endpoint kept. */
    memcpy(work, adsrc, sizeof(adsrc));
    work[sizeof(adsrc) - 9 - 7 + 3] = UAC_CONV_MPO;
    Model(work, sizeof(adsrc), 0, 0);
    CHECK(m.AltCount == 1 && m.Alt[0].EpControls == UAC_CONV_MPO && m.Alt[0].FeedbackEndpoint == 0x02);
    /* ... and on a synchronous source; not on an asynchronous one. */
    memcpy(work, adsrc, sizeof(adsrc));
    work[sizeof(adsrc) - 9 - 7 + 3] = UAC_CONV_MPO;
    work[sizeof(adsrc) - 9 - 7 - 9 + 3] = 0x0d;      /* synchronous */
    work[sizeof(adsrc) - 9 - 7 - 9 + 8] = 0;         /* no bSynchAddress */
    work[9 + 9 + 9 + 12 + 12 + 8 + 9 + 9 + 4] = 1;   /* one endpoint */
    work[2] = (unsigned char)(sizeof(adsrc) - 9);     /* wTotalLength */
    Model(work, sizeof(adsrc) - 9, 0, 0);
    CHECK(m.AltCount == 1 && UacPlanSync(&m.Alt[0]) == 3 && m.Alt[0].EpControls == UAC_CONV_MPO);
    work[sizeof(adsrc) - 9 - 7 - 9 + 3] = 0x05;      /* asynchronous */
    CHECK(Uac1Parse(work, sizeof(adsrc) - 9, 0, 0, &d) == UAC1_NO_AUDIO && d.Skipped == 1);
    /* ... nor in PCM8 (a zero byte is a sample there), synchronous again. */
    work[sizeof(adsrc) - 9 - 7 - 9 + 3] = 0x0d;
    work[86 + 5] = UAC_TAG_PCM8; work[93 + 5] = 1; work[93 + 6] = 8;
    CHECK(Uac1Parse(work, sizeof(adsrc) - 9, 0, 0, &d) == UAC1_NO_AUDIO && d.Skipped == 1);
    work[86 + 5] = UAC_TAG_FLOAT; work[93 + 5] = 4; work[93 + 6] = 32;
    work[sizeof(adsrc) - 9 - 7 - 9 + 4] = 200;        /* wMaxPacketSize for 48 floats */
    Model(work, sizeof(adsrc) - 9, 0, 0);
    CHECK(m.AltCount == 1 && m.Alt[0].FormatTag == UAC_TAG_FLOAT && m.Alt[0].EpControls == UAC_CONV_MPO);
    /* PCM8 and mu-law in 2-byte subframes: carried. */
    n = QemuAs(UAC_TAG_PCM8, 2, 8, 192);
    Model(work, n, 0, 0);
    CHECK(m.AltCount == 1 && m.Alt[0].Subslot == 2 && m.Alt[0].Bits == 8 && m.Alt[0].FormatTag == UAC_TAG_PCM8);
    CHECK(UacPlanFormats(&m, 0, 0, f, 4) == 1 && f[0].Subslot == 2 && f[0].Bits == 16 && f[0].WireSubslot == 2);
    CHECK(UacPlanPick(&m, 0, 1, 2, 2, UAC_PLAN_BITS(UAC_TAG_PCM8, 8), UAC_PLAN_ANY_CONFIG, 48000, 0, &index) == UAC_OK);
    /* The first runnable terminal: a float-only one ahead of PCM. */
    Terminals();
}

static void Conversions(void)
{
    unsigned int a;
    long s;
    unsigned char in[8], out[16];
    /* G.711 reference points. */
    CHECK(UacConvMulawDecode(0xff) == 0 && UacConvMulawDecode(0x80) == 32124 && UacConvMulawDecode(0x00) == -32124);
    CHECK(UacConvMulawDecode(0x7f) == 0 && UacConvMulaw(0) == 0xff);
    CHECK(UacConvAlawDecode(0xd5) == 8 && UacConvAlawDecode(0x55) == -8);
    CHECK(UacConvAlawDecode(0xaa) == 32256 && UacConvAlawDecode(0x2a) == -32256);
    CHECK(UacConvAlaw(0) == 0xd5 && UacConvAlaw(32767) == 0xaa && UacConvAlaw(-32768) == 0x2a);
    CHECK(UacConvMulaw(32767) == 0x80 && UacConvMulaw(-32768) == 0x00);
    for (a = 0; a < 256; a++) {
        CHECK(UacConvAlaw(UacConvAlawDecode(a)) == a);
        CHECK(UacConvMulaw(UacConvMulawDecode(a)) == (a == 0x7f ? 0xffU : a));
    }
    /* Encoders are monotonic in the decoded value. */
    for (s = -32768; s < 32767; s++) {
        CHECK(UacConvAlawDecode(UacConvAlaw((int)s)) <= UacConvAlawDecode(UacConvAlaw((int)s + 1)));
        CHECK(UacConvMulawDecode(UacConvMulaw((int)s)) <= UacConvMulawDecode(UacConvMulaw((int)s + 1)));
    }
    /* Float: exact both ways for every 16-bit value. */
    CHECK(UacConvFloat(0) == 0 && UacConvFloat(-32768) == 0xbf800000UL && UacConvFloat(16384) == 0x3f000000UL);
    CHECK(UacConvFloat(1) == 0x38000000UL && UacConvFloat(-1) == 0xb8000000UL);
    for (s = -32768; s <= 32767; s++) CHECK(UacConvFloatDecode(UacConvFloat((int)s)) == s);
    CHECK(UacConvFloatDecode(0x3f800000UL) == 32767 && UacConvFloatDecode(0xbf800000UL) == -32768);
    CHECK(UacConvFloatDecode(0x7fc00000UL) == 0 && UacConvFloatDecode(0xffc00000UL) == 0);
    CHECK(UacConvFloatDecode(0x7f800000UL) == 32767 && UacConvFloatDecode(0xff800000UL) == -32768);
    CHECK(UacConvFloatDecode(0x40000000UL) == 32767 && UacConvFloatDecode(0x00000001UL) == 0);
    CHECK(UacConvFloatDecode(0x37800000UL) == 1 && UacConvFloatDecode(0x37000000UL) == 0);
    CHECK(UacConvFloatDecode(0x3f000001UL) == 16384 && UacConvFloatDecode(0xb7800000UL) == -1);
    /* PCM8: offset binary, rounded, saturated. */
    in[0] = 0; in[1] = 0; in[2] = 0xff; in[3] = 0x7f; in[4] = 0; in[5] = 0x80; in[6] = 0x80; in[7] = 0;
    UacConvOut(UAC_TAG_PCM8, 1, in, 4, out);
    CHECK(out[0] == 0x80 && out[1] == 0xff && out[2] == 0x00 && out[3] == 0x81);
    UacConvIn(UAC_TAG_PCM8, 1, out, 4, in);
    CHECK(in[0] == 0 && in[1] == 0 && in[2] == 0x00 && in[3] == 0x7f && in[4] == 0 && in[5] == 0x80);
    CHECK(in[6] == 0x00 && in[7] == 0x01);
    /* The array forms, little-endian. */
    in[0] = 0x00; in[1] = 0x40; in[2] = 0x00; in[3] = 0x80;
    UacConvOut(UAC_TAG_FLOAT, 4, in, 2, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0x3f && out[7] == 0xbf && out[6] == 0x80);
    UacConvIn(UAC_TAG_FLOAT, 4, out, 2, in);
    CHECK(in[0] == 0x00 && in[1] == 0x40 && in[2] == 0x00 && in[3] == 0x80);
    in[0] = 0; in[1] = 0; in[2] = 0xff; in[3] = 0x7f;
    UacConvOut(UAC_TAG_ALAW, 1, in, 2, out);
    CHECK(out[0] == 0xd5 && out[1] == 0xaa);
    UacConvOut(UAC_TAG_MULAW, 1, in, 2, out);
    CHECK(out[0] == 0xff && out[1] == 0x80);
    UacConvIn(UAC_TAG_MULAW, 1, out, 2, in);
    CHECK(in[0] == 0 && in[1] == 0 && in[2] == 0x7c && in[3] == 0x7d);
    /* 8 bits left-justified in a wider subframe: the last byte. */
    in[0] = 0x00; in[1] = 0x80; in[2] = 0xff; in[3] = 0x7f;
    UacConvOut(UAC_TAG_PCM8, 2, in, 2, out);
    CHECK(out[0] == 0 && out[1] == 0x00 && out[2] == 0 && out[3] == 0xff);
    out[0] = 0x5a; out[2] = 0xa5;   /* ignored low bytes */
    UacConvIn(UAC_TAG_PCM8, 2, out, 2, in);
    CHECK(in[0] == 0 && in[1] == 0x80 && in[2] == 0 && in[3] == 0x7f);
    UacConvOut(UAC_TAG_MULAW, 3, in, 1, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0x00);
    /* PCM8 out and back: s rounded to a multiple of 256, saturating. */
    for (s = -32768; s <= 32767; s++) {
        long back, want = s >= 32640 ? 32512 : ((s + 128 + 32768) / 256) * 256 - 32768;
        in[0] = (unsigned char)(s & 0xff); in[1] = (unsigned char)((s >> 8) & 0xff);
        UacConvOut(UAC_TAG_PCM8, 1, in, 1, out);
        UacConvIn(UAC_TAG_PCM8, 1, out, 1, in + 2);
        back = (long)(short)(in[2] | (in[3] << 8));
        CHECK(back == want);
    }
    CHECK(UacConvKnown(UAC_TAG_PCM, 3, 24) && !UacConvKnown(UAC_TAG_PCM, 1, 8) && UacConvKnown(UAC_TAG_MULAW, 1, 8));
    CHECK(UacConvKnown(UAC_TAG_PCM8, 2, 8) && UacConvKnown(UAC_TAG_ALAW, 4, 8) && !UacConvKnown(UAC_TAG_PCM8, 2, 16));
    CHECK(!UacConvKnown(UAC_TAG_FLOAT, 2, 16) && !UacConvKnown(UAC_TAG_MULAW, 5, 8) && !UacConvKnown(6, 1, 8));
    CHECK(UacConvRank(UAC_TAG_PCM) < UacConvRank(UAC_TAG_FLOAT) && UacConvRank(UAC_TAG_FLOAT) < UacConvRank(UAC_TAG_ALAW));
    CHECK(UacConvRank(UAC_TAG_ALAW) == UacConvRank(UAC_TAG_MULAW) && UacConvRank(UAC_TAG_MULAW) < UacConvRank(UAC_TAG_PCM8));
    /* Ff (26.12). */
    CHECK(UacConvFf(48000, 0) == 0x0c0000UL && UacConvFf(44100, 0) == 0x0b0666UL && UacConvFf(16000, 0) == 0x040000UL);
    CHECK(UacConvFf(48000, 1) == 0x060000UL && UacConvFf(44100, 1) == 0x058333UL && UacConvFf(7999, 0) == 0);
}

static void Mpo(void)
{
    UAC_CONV_CADENCE c;
    unsigned char p[52 * 4];
    unsigned int i, sum = 0, last, total = 0, owed = 0;
    CHECK(UacConvCadence(&c, 44100, 0, 1, 0) && c.Low == 44 && c.High == 45);
    for (i = 0; i < 10; i++) sum += UacConvNominal(&c);
    CHECK(sum == 441 && c.Owed == 2 * 45 + 2);   /* held to a few packets */
    CHECK(UacConvCadence(&c, 48000, 0, 1, 0) && c.Low == 47 && c.High == 49);   /* 2.2.1, whole nav */
    CHECK(UacConvCadence(&c, 48000, 1, 4, 0) && c.Low == 47 && c.High == 49);
    CHECK(UacConvCadence(&c, 8000, 1, 1, 0) && c.Low == 0 && c.High == 2);
    CHECK(!UacConvCadence(&c, 48000, 0, 2, 0) && !UacConvCadence(&c, 7000, 0, 1, 0));
    /* 44.1 kHz, frames of 4 bytes, packets of 52 frames padded. */
    CHECK(UacConvCadence(&c, 44100, 0, 1, 0));
    memset(p, 0, sizeof(p));
    for (i = 0; i < 45 * 4; i++) p[i] = (unsigned char)(i | 1);
    (void)UacConvNominal(&c);                                      /* owed 44 */
    CHECK(UacConvMpoFrames(&c, p, sizeof(p), 4, &last) == 45 && last == 45 && c.Owed == -1);
    p[44 * 4] = p[44 * 4 + 1] = p[44 * 4 + 2] = p[44 * 4 + 3] = 0;
    (void)UacConvNominal(&c);                                      /* owed 43 */
    CHECK(UacConvMpoFrames(&c, p, sizeof(p), 4, &last) == 44 && last == 44 && c.Owed == -1);
    memset(p, 0, sizeof(p));                                       /* silence */
    (void)UacConvNominal(&c);                                      /* owed 43 */
    CHECK(UacConvMpoFrames(&c, p, sizeof(p), 4, &last) == 44 && last == 0 && c.Owed == -1);
    p[51 * 4] = 9;                                                 /* past High */
    (void)UacConvNominal(&c);
    CHECK(UacConvMpoFrames(&c, p, sizeof(p), 4, &last) == 45 && last == 52);
    p[51 * 4] = 0;
    CHECK(UacConvMpoFrames(&c, p, 40 * 4 + 2, 4, &last) == 40);  /* whole frames received */
    /* Silence over a second: the nominal 44100 frames, no drift. */
    CHECK(UacConvCadence(&c, 44100, 0, 1, 0));
    memset(p, 0, sizeof(p));
    for (i = 0; i < 1000; i++) {
        (void)UacConvNominal(&c);
        total += UacConvMpoFrames(&c, p, sizeof(p), 4, &last);
    }
    CHECK(total == 44100 && c.Owed == 0);
    /* A synchronous 48 kHz source sending 47, 49, 48 (2.2.1), every frame
     * of each nonzero to its end: all taken; the 47 gets one frame of
     * silence (48 were owed), which leaves the stream one frame ahead,
     * within the bounds, not drifting. */
    CHECK(UacConvCadence(&c, 48000, 0, 1, 0));
    for (i = 0; i < 3; i++) {
        unsigned int n = i == 0 ? 47 : i == 1 ? 49 : 48;
        memset(p, 0, sizeof(p));
        memset(p, 0x11, n * 4);
        (void)UacConvNominal(&c);
        owed += UacConvMpoFrames(&c, p, sizeof(p), 4, &last);
        CHECK(last == n);
    }
    CHECK(owed == 145 && c.Owed == -1);
    /* A source one frame late whose packet ends in zeros: the frame owed
     * is counted (silence), and the next packet's frames are not cut. */
    CHECK(UacConvCadence(&c, 48000, 0, 1, 0));
    memset(p, 0, sizeof(p)); memset(p, 0x22, 47 * 4);
    (void)UacConvNominal(&c);
    CHECK(UacConvMpoFrames(&c, p, sizeof(p), 4, &last) == 48 && last == 47 && c.Owed == 0);
    /* A lost packet starts the balance afresh. */
    (void)UacConvNominal(&c);
    UacConvLost(&c);
    CHECK(c.Owed == 0);
    /* 47 and 49 alternating at 48 kHz, every 47 lost: no debt builds up,
     * and silence afterwards is counted at 48 a packet. */
    CHECK(UacConvCadence(&c, 48000, 0, 1, 0));
    for (i = 0; i < 1000; i++) {
        (void)UacConvNominal(&c);
        if (i & 1) {
            memset(p, 0, sizeof(p)); memset(p, 0x33, 49 * 4);
            CHECK(UacConvMpoFrames(&c, p, sizeof(p), 4, &last) == 49);
        } else UacConvLost(&c);
    }
    memset(p, 0, sizeof(p));
    for (i = 0; i < 100; i++) {
        (void)UacConvNominal(&c);
        CHECK(UacConvMpoFrames(&c, p, sizeof(p), 4, &last) == (i ? 48U : 47U));
    }
    /* 44.1 kHz with every fifth transfer of four packets lost: each lost
     * interval still advances the cadence, so its phase stays that of a
     * stream that lost nothing, and silence afterwards counts 44100 a
     * second. */
    {
        UAC_CONV_CADENCE ref;
        memset(&ref, 0, sizeof(ref));
        CHECK(UacConvCadence(&c, 44100, 0, 1, 0));
        CHECK(UacConvCadence(&ref, 44100, 0, 1, 0));
        memset(p, 0, sizeof(p));
        for (i = 0; i < 1000; i++) {
            unsigned int k;
            if (i % 5 == 4) UacConvLostPackets(&c, 4);
            else for (k = 0; k < 4; k++) {
                (void)UacConvNominal(&c);
                (void)UacConvMpoFrames(&c, p, sizeof(p), 4, &last);
            }
            for (k = 0; k < 4; k++) (void)UacConvNominal(&ref);
            CHECK(c.Acc == ref.Acc);
        }
        CHECK(c.Owed == 0);
        total = 0;
        for (i = 0; i < 1000; i++) {
            (void)UacConvNominal(&c);
            total += UacConvMpoFrames(&c, p, sizeof(p), 4, &last);
        }
        CHECK(total == 44100 && c.Owed == 0);
    }
    /* An adaptive source following Ff 0B0666 (44.1 kHz) for five minutes,
     * one packet in seven silent: the frames taken track the frames sent
     * (Ff's rate, not 44100), never more than a frame or two apart. */
    {
        unsigned long acc = 0, sent = 0, taken = 0;
        CHECK(UacConvCadence(&c, 44100, 0, 1, 0x0b0666UL) && c.Low == 44 && c.High == 45);
        for (i = 0; i < 300000; i++) {
            unsigned int n;
            acc += 0x0b0666UL; n = (unsigned int)(acc >> 14); acc &= 0x3fff;
            memset(p, 0, sizeof(p));
            if (i % 7) memset(p, 0x44, n * 4);
            sent += n;
            (void)UacConvNominal(&c);
            taken += UacConvMpoFrames(&c, p, sizeof(p), 4, &last);
            CHECK(taken + 2 >= sent && taken <= sent + 2);
        }
    }
}

static void Offer(void)
{
    UAC_FORMAT f[5];
    unsigned long count[5], rate[5][4];
    unsigned int i;
    memset(f, 0, sizeof(f)); memset(rate, 0, sizeof(rate));
    for (i = 0; i < 5; i++) {
        f[i].Channels = 2; f[i].Subslot = 2; f[i].Bits = 16; f[i].Terminal = 1; f[i].ChannelConfig = 3;
        f[i].WireSubslot = 1; f[i].WireBits = 8;
    }
    f[0].FormatTag = UAC_TAG_PCM8; count[0] = 2; rate[0][0] = 44100; rate[0][1] = 48000;
    f[1].FormatTag = UAC_TAG_PCM; f[1].WireSubslot = 2; f[1].WireBits = 16; count[1] = 1; rate[1][0] = 48000;
    f[2].FormatTag = UAC_TAG_MULAW; count[2] = 3; rate[2][0] = 44100; rate[2][1] = 48000; rate[2][2] = 8000;
    f[3].FormatTag = UAC_TAG_FLOAT; f[3].WireSubslot = 4; f[3].WireBits = 32; count[3] = 1; rate[3][0] = 44100;
    f[4].FormatTag = UAC_TAG_PCM; f[4].Subslot = 3; f[4].Bits = 24; f[4].WireSubslot = 3; f[4].WireBits = 24;
    count[4] = 1; rate[4][0] = 96000;
    CHECK(UacPlanOffer(f, count, &rate[0][0], 4, 5) == 4);
    CHECK(f[0].FormatTag == UAC_TAG_PCM && f[0].Subslot == 2 && count[0] == 1 && rate[0][0] == 48000);
    CHECK(f[1].FormatTag == UAC_TAG_PCM && f[1].Subslot == 3 && count[1] == 1 && rate[1][0] == 96000);
    CHECK(f[2].FormatTag == UAC_TAG_FLOAT && count[2] == 1 && rate[2][0] == 44100);
    CHECK(f[3].FormatTag == UAC_TAG_MULAW && count[3] == 1 && rate[3][0] == 8000);
    /* PCM only: unchanged. */
    f[0].FormatTag = f[1].FormatTag = UAC_TAG_PCM;
    count[0] = 2; rate[0][1] = 44100;
    CHECK(UacPlanOffer(f, count, &rate[0][0], 4, 2) == 2 && count[0] == 2 && rate[0][1] == 44100 &&
          f[1].Subslot == 3);
    /* Another channel count is another pin view. */
    f[0].FormatTag = UAC_TAG_PCM; count[0] = 1; rate[0][0] = 48000;
    f[1] = f[0]; f[1].FormatTag = UAC_TAG_ALAW; f[1].Channels = 1; count[1] = 1; rate[1][0] = 48000;
    CHECK(UacPlanOffer(f, count, &rate[0][0], 4, 2) == 2 && f[1].Channels == 1);
}

static unsigned int Copy(void) { memcpy(work, logi, logiLen); return logiLen; }

static void Hostile(void)
{
    unsigned int n, i;
    n = Copy(); work[9 + 9 + 3] = 0x00; work[9 + 9 + 4] = 0x02;     /* bcdADC 2.0 */
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_BAD_DESCRIPTOR);
    n = Copy(); work[9 + 9 + 5]--;                                 /* wTotalLength */
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_BAD_DESCRIPTOR);
    n = Copy(); work[9 + 9 + 8] = 0;                               /* collects AC */
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_BAD_DESCRIPTOR);
    n = Copy(); work[9 + 9 + 9] = 7;                               /* absent iface */
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_BAD_DESCRIPTOR);
    n = Copy(); work[9 + 9 + 10 + 12 + 5] = 0;                     /* bControlSize 0 */
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_BAD_DESCRIPTOR);
    CHECK(Uac1Parse(logi, logiLen - 1, 0, 0, &d) == UAC1_BAD_DESCRIPTOR);
    /* A sampled mutation smoke test: single-byte changes inside the AC
     * block (16 values each) fail or parse into a model the driver core
     * walks; not a memory-safety proof (the buffer is larger than the
     * configuration). */
    for (i = 18; i < 18 + 103; i++) {
        unsigned int v;
        for (v = 0; v < 256; v += 17) {
            n = Copy(); work[i] = (unsigned char)v;
            if (Uac1Parse(work, n, 0, 0, &d) == UAC1_OK && Uac1Model(&d, 0, &m) == UAC1_OK) {
                UAC_VOL_MAP vm;
                unsigned int k;
                for (k = 0; k < m.AltCount; k++) {
                    (void)UacVolFind(&m, m.Alt[k].Terminal, 2, &vm);
                    (void)UacVolJack(&m, (m.Alt[k].DataEndpoint & 0x80) != 0, m.Alt[k].Terminal);
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) { printf("usage: uac1test <brio100.bin> <logi0a64.bin> <play2.bin>\n"); return 2; }
    brioLen = Load(argv[1], brio, sizeof(brio));
    logiLen = Load(argv[2], logi, sizeof(logi));
    play2Len = Load(argv[3], play2, sizeof(play2));
    Brio(); Logi(); Play2(); Qemu(); Async(); Midi(); Mixed(); Selector(); ManyRates(); Adaptive(); SelectorCycle(); Formats(); Hostile();
    Conversions(); Mpo(); Offer();
    printf("PASS uac1test: %u checks (parser, model, plan, volume walk, feedback, conversions)\n", checks);
    return 0;
}

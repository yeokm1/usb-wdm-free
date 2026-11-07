/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host executable, built with UAC_V1 as the driver is: the USB Audio 1.0
 * parser (uac1desc.c), its shared model (uac1model.c) and the driver core
 * that runs on it (src/audio/common: uacplan.c, uacvol.c, uacrate.c), on
 * two recorded devices (the Brio 100 microphone, the Logi USB Headset),
 * QEMU 11.1.0's usb-audio layout, an asynchronous headset, a MIDI-only
 * function, a mixed UAC 1.0 / 2.0 configuration and hostile variants.
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
    /* Read as a plain IN endpoint; the synch OUT endpoint is kept so the
     * alternate's pipes are all selected, and is never fed. */
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

static void Unsupported(void)
{
    unsigned int i, n;
    /* IEEE float (wFormatTag 3): the parser skips it. */
    memcpy(work, qemu, sizeof(qemu)); n = sizeof(qemu);
    for (i = 9; i < n; i += work[i])
        if (work[i + 1] == 0x24 && work[i + 2] == 1 && work[i] == 7) work[i + 5] = 3;
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_NO_AUDIO && d.Skipped == 1);
    /* PCM8: parsed, not carried by the model. */
    memcpy(work, qemu, sizeof(qemu));
    for (i = 9; i < n; i += work[i]) {
        if (work[i + 1] == 0x24 && work[i + 2] == 1 && work[i] == 7) work[i + 5] = 2;
        if (work[i + 1] == 0x24 && work[i + 2] == 2 && work[i] == 11) { work[i + 5] = 1; work[i + 6] = 8; }
    }
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_OK && d.AltCount == 1);
    CHECK(Uac1Model(&d, 0, &m) == UAC1_NO_AUDIO && m.Skipped == 1);
    /* MaxPacketsOnly on an OUT endpoint: not carried in this version. */
    memcpy(work, qemu, sizeof(qemu));
    work[n - 7 + 3] = 0x80;
    CHECK(Uac1Parse(work, n, 0, 0, &d) == UAC1_OK && d.AltCount == 1);
    CHECK(Uac1Model(&d, 0, &m) == UAC1_NO_AUDIO && m.Skipped == 1);
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
    Brio(); Logi(); Play2(); Qemu(); Async(); Midi(); Mixed(); Selector(); ManyRates(); Adaptive(); SelectorCycle(); Unsupported(); Hostile();
    printf("PASS uac1test: %u checks (parser, model, plan, volume walk, feedback)\n", checks);
    return 0;
}

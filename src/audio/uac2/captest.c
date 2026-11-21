/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host test of uaccap.c (docs/design/uac2.md sections 28.3 to 28.5, 28.12),
 * built twice: by uac2's test.cmd for USB Audio 2.0 layouts and by uac1's
 * with UAC_V1 for USB Audio 1.0 layouts. The recorded X4's capture graph
 * per source (UAC 2.0) or the recorded Play! 2, Logi headset and Brio 100
 * capture graphs (UAC 1.0); built graphs in the build's family (a diamond,
 * a loop on one route, 64 and 65 routes, depth 32 and 33, a malformed and
 * an unlisted entity, a clock selector never a source, a unit on one mixer
 * input, a unit after the mixer, a multi-input Processing Unit, a one-input
 * Extension Unit, a channel-changing mixer, a common unit after a chooser,
 * selectors in series with a shared inner selector, a selector behind a
 * join, 8 and 9 sources, a "Stereo Mix" branch, truncated layouts), the
 * claims table for two playback and two capture endpoints, and the route
 * diagnostics.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uaccap.h"

static unsigned int checks;
static void Check(int ok, const char *expression, unsigned int line)
{
    checks++;
    if (!ok) { printf("FAIL line %u: %s\n", line, expression); exit(1); }
}
#define CHECK(x) Check((x), #x, __LINE__)

/* The family's subtypes (ADC-2 Table A-9, ADC-1 Table A-5). */
#ifdef UAC_V1
#define PU 0x07
#define XU 0x08
#else
#define PU 0x08
#define XU 0x09
#endif

static UAC_DESC desc;
static UAC_BYTE file[65536], bytes[8192];
static unsigned int used;
static UAC_CAP_GRAPH g, g2;

static void Reset(void)
{
    memset(&desc, 0, sizeof(desc));
    memset(bytes, 0, sizeof(bytes));
    used = 9;
    desc.Bytes = bytes;
}
static void Add(const UAC_BYTE *d)
{
    desc.Entity[d[3]] = (UAC_WORD)used;
    memcpy(bytes + used, d, d[0]);
    used += d[0];
    desc.Length = used;
}
/* Input Terminal: ADC-2 17 bytes (bNrChannels at 8), ADC-1 12 (at 7). */
static void It(UAC_BYTE id, unsigned int type, UAC_BYTE channels)
{
    UAC_BYTE d[17];
    memset(d, 0, sizeof(d));
    d[1] = 0x24; d[2] = 0x02; d[3] = id; d[4] = (UAC_BYTE)type; d[5] = (UAC_BYTE)(type >> 8);
#ifdef UAC_V1
    d[0] = 12; d[7] = channels;
#else
    d[0] = 17; d[7] = 40; d[8] = channels;
#endif
    Add(d);
}
#ifdef UAC_V1
#define IT_LENGTH 12
#else
#define IT_LENGTH 17
#endif
/* Output Terminal: ADC-2 12 bytes, ADC-1 9. */
static void Ot(UAC_BYTE id, unsigned int type, UAC_BYTE source)
{
    UAC_BYTE d[12];
    memset(d, 0, sizeof(d));
    d[1] = 0x24; d[2] = 0x03; d[3] = id; d[4] = (UAC_BYTE)type; d[5] = (UAC_BYTE)(type >> 8); d[7] = source;
#ifdef UAC_V1
    d[0] = 9;
#else
    d[0] = 12; d[8] = 40;
#endif
    Add(d);
}
/* Feature Unit with `channels` logical channels: ADC-2 6 + (ch + 1) * 4,
 * the bit pairs as given; ADC-1 7 + (ch + 1) * 1 (bControlSize 1), D0 mute
 * and D1 volume set where the given pair is read/write. */
#ifdef UAC_V1
#define FU_LENGTH(ch) (7 + ((ch) + 1))
#else
#define FU_LENGTH(ch) (6 + 4 * ((ch) + 1))
#endif
static void Fu(UAC_BYTE id, UAC_BYTE source, unsigned int channels, const unsigned long *controls)
{
    UAC_BYTE d[64];
    unsigned int i;
    unsigned long v;
    memset(d, 0, sizeof(d));
    d[0] = (UAC_BYTE)FU_LENGTH(channels); d[1] = 0x24; d[2] = 0x06; d[3] = id; d[4] = source;
    for (i = 0; i <= channels; i++) {
        v = controls[i < 3 ? i : 2];        /* channels past 2 as channel 2 */
#ifdef UAC_V1
        d[5] = 1;
        d[6 + i] = (UAC_BYTE)(((v & 3) == 3 ? 1 : 0) | (((v >> 2) & 3) == 3 ? 2 : 0));
#else
        d[5 + 4 * i] = (UAC_BYTE)v; d[6 + 4 * i] = (UAC_BYTE)(v >> 8);
#endif
    }
    Add(d);
}
/* Selector Unit with n inputs: ADC-2 7 + p (bmControls read/write,
 * iSelector), ADC-1 6 + p (iSelector). */
#ifdef UAC_V1
#define SEL_LENGTH(p) (6 + (p))
#else
#define SEL_LENGTH(p) (7 + (p))
#endif
static void Sel(UAC_BYTE id, const UAC_BYTE *in, unsigned int n)
{
    UAC_BYTE d[255];
    memset(d, 0, sizeof(d));
    d[0] = (UAC_BYTE)SEL_LENGTH(n); d[1] = 0x24; d[2] = 0x05; d[3] = id; d[4] = (UAC_BYTE)n;
    memcpy(d + 5, in, n);
#ifndef UAC_V1
    d[5 + n] = 0x03;
#endif
    Add(d);
}
/* Mixer Unit with n inputs carrying `total` logical channels together and
 * `channels` output channels: bmMixerControls N = total * channels bits
 * in whole bytes (ADC-2 4.7.2.6, ADC-1 4.3.2.3), every mix control
 * programmable; ADC-2 13 + p + N, ADC-1 10 + p + N. */
#define MIX_BYTES(total, m) (((total) * (m) + 7) / 8)
#ifdef UAC_V1
#define MIX_LENGTH(p, N) (10 + (p) + (N))
#define MIX_AFTER 1         /* iMixer after the bitmap */
#else
#define MIX_LENGTH(p, N) (13 + (p) + (N))
#define MIX_AFTER 2         /* bmControls and iMixer after the bitmap */
#endif
static void Mix(UAC_BYTE id, const UAC_BYTE *in, unsigned int n, unsigned int total, UAC_BYTE channels)
{
    UAC_BYTE d[255];
    unsigned int N = MIX_BYTES(total, channels);
    memset(d, 0, sizeof(d));
    d[0] = (UAC_BYTE)MIX_LENGTH(n, N); d[1] = 0x24; d[2] = 0x04; d[3] = id; d[4] = (UAC_BYTE)n;
    memcpy(d + 5, in, n);
    d[5 + n] = channels;
    memset(d + d[0] - MIX_AFTER - N, 0xFF, N);
    Add(d);
}
/* Extension Unit with n inputs: ADC-2 15 + p; ADC-1 13 + p + n
 * (bControlSize n = 1). */
static void Xu(UAC_BYTE id, const UAC_BYTE *in, unsigned int n, UAC_BYTE channels)
{
    UAC_BYTE d[64];
    memset(d, 0, sizeof(d));
#ifdef UAC_V1
    d[0] = (UAC_BYTE)(14 + n); d[11 + n] = 1;
#else
    d[0] = (UAC_BYTE)(15 + n);
#endif
    d[1] = 0x24; d[2] = XU; d[3] = id;
    d[6] = (UAC_BYTE)n;
    memcpy(d + 7, in, n);
    d[7 + n] = channels;
    Add(d);
}
/* Processing Unit of process type `type` with n inputs and, for
 * Up/Down-mix (1) and Dolby Prologic (2), bNrModes `modes` and that many
 * modes (Left and Right): ADC-2 16 + p, 4 bytes a mode (18 + 4 * m for one
 * input); ADC-1 13 + p + 1 (bControlSize 1), 2 bytes a mode. */
#ifdef UAC_V1
#define MODE 2
#else
#define MODE 4
#endif
static void Proc(UAC_BYTE id, const UAC_BYTE *in, unsigned int n, unsigned int type, unsigned int modes,
                 UAC_BYTE channels)
{
    UAC_BYTE d[255];
    unsigned int tail, i;
    memset(d, 0, sizeof(d));
    d[1] = 0x24; d[2] = PU; d[3] = id; d[4] = (UAC_BYTE)type; d[5] = (UAC_BYTE)(type >> 8);
    d[6] = (UAC_BYTE)n;
    memcpy(d + 7, in, n);
    d[7 + n] = channels;
#ifdef UAC_V1
    d[11 + n] = 1; d[12 + n] = 0x03;    /* Enable Processing, Mode Select */
    tail = 14 + n;
#else
    d[13 + n] = 0x0f;                   /* Enable, Mode Select: read/write */
    tail = 16 + n;
#endif
    if (type == 1 || type == 2) {
        d[tail] = (UAC_BYTE)modes;
        for (i = 0; i < modes; i++) d[tail + 1 + MODE * i] = 0x03;
        tail += 1 + MODE * modes;
    }
    d[0] = (UAC_BYTE)tail;
    Add(d);
}
/* An entity's bLength shortened in place: its tail cut off. */
static void Cut(UAC_BYTE id, UAC_BYTE length)
{
    bytes[desc.Entity[id]] = length;
}
static UAC_BYTE Length(UAC_BYTE id)
{
    return bytes[desc.Entity[id]];
}
/* A Clock Selector (ADC-2 0x0B); in ADC-1 an undefined subtype. */
static void ClockSel(UAC_BYTE id)
{
    UAC_BYTE d[8] = { 8, 0x24, 0x0b, 0, 1, 41, 3, 0 };
    d[3] = id;
    Add(d);
}

static const unsigned long both[3] = { 0x0f, 0x0f, 0x0f };     /* mute+volume rw, master and 2 channels */
static const unsigned long chans[3] = { 0, 0x0f, 0x0f };
static const unsigned long master[3] = { 0x0f, 0, 0 };

/* Stage 1 (design 28.18): UacCapStart's whole Uac2VolMapC record against
 * one built field by field here: Channels, Output, Gain[8] at unity, the
 * route fields given, Selector[i].Id from `ids` with no position or status
 * read, every other byte 0, whatever the record held before. */
static unsigned long Get(const unsigned char *r, unsigned int o)
{
    return (unsigned long)r[o] | ((unsigned long)r[o + 1] << 8) | ((unsigned long)r[o + 2] << 16) |
           ((unsigned long)r[o + 3] << 24);
}
static void Record(const UAC_CAP_GRAPH *graph, unsigned int output, unsigned int channels, int want,
                   unsigned long node, unsigned long sources, unsigned long state, unsigned long flags,
                   const UAC_BYTE *ids, unsigned int nids, unsigned int line)
{
    unsigned char rec[UAC_CAP_DIAG_SIZE + 4], ref[UAC_CAP_DIAG_SIZE];
    unsigned int i, o;
    int got;
    memset(rec, 0xEE, sizeof(rec));
    memset(ref, 0, sizeof(ref));
    got = UacCapStart(&desc, output, channels, (UAC_CAP_GRAPH *)graph, rec);
    ref[0] = (unsigned char)node; ref[4] = (unsigned char)output;
    for (o = 116; o <= 144; o += 4) ref[o + 2] = 1;             /* 65536, unity */
    ref[232] = (unsigned char)sources; ref[244] = (unsigned char)state;
    ref[252] = (unsigned char)flags;
    for (i = 0; i < nids; i++) ref[256 + 12 * i] = ids[i];
    Check(got == want, "UacCapStart's answer", line);
    Check(!memcmp(rec, ref, UAC_CAP_DIAG_SIZE), "the whole record", line);
    for (i = UAC_CAP_DIAG_SIZE; i < sizeof(rec); i++) Check(rec[i] == 0xEE, "nothing past the record", line);
    Check(Get(rec, 116) == UAC_VOL_UNITY && Get(rec, 244) == state && Get(rec, 252) == flags, "fields", line);
}

static void Level(unsigned int s, UAC_BYTE vu, unsigned int vm, UAC_BYTE mu, unsigned int mm,
                  const UAC_CLAIMS *t, unsigned int holder, unsigned int line)
{
    UAC_CAP_LEVEL lv;
    UacCapPick(&desc, &g, s, 2, t, holder, &lv);
    Check(lv.VolumeUnit == vu && lv.VolumeMode == vm && lv.MuteUnit == mu && lv.MuteMode == mm, "level pick", line);
}

/* The graph of capture Output Terminal 6 as built: complete, with `unit`
 * carrying the volume and its volume claimed by the endpoint; or
 * incomplete (a malformed entity), with nothing eligible and nothing
 * claimed. */
static void Expect(int complete, UAC_BYTE unit, unsigned int line)
{
    static UAC_CLAIMS t;
    UAC_CAP_ENDPOINT ep;
    UAC_CAP_LEVEL lv;
    UAC_CAP_CONTROL c;
    int built = UacCapBuild(&desc, 6, &g);
    memset(&ep, 0, sizeof(ep));
    ep.Capture = 1; ep.Graph = &g; ep.Channels = 2;
    UacCapClaims(&desc, &ep, 1, &t);
    UacCapPick(&desc, &g, 1, 2, NULL, 0, &lv);
    c.Entity = unit; c.Selector = 2; c.Channel = (UAC_BYTE)(lv.VolumeMode == UAC_VOL_CHANNEL ? 1 : 0);
    if (complete)
        Check(built == 1 && !g.Flags && lv.VolumeUnit == unit && lv.VolumeMode != UAC_VOL_NONE &&
              UacClaimHolder(&t, &c, NULL) == 0, "complete, the unit eligible and claimed", line);
    else
        Check(built == 0 && (g.Flags & UAC_CAP_MALFORMED) && !lv.VolumeUnit && !lv.MuteUnit &&
              lv.VolumeMode == UAC_VOL_NONE && lv.MuteMode == UAC_VOL_NONE && t.Count == 0,
              "incomplete, nothing eligible or claimed", line);
}

/* Processing Units: each defined process type's whole layout, the
 * process-specific part included (ADC-2 4.7.2.11, ADC-1 4.3.2.6). The
 * graph: Output Terminal 6 <- Processing Unit 7 <- Feature Unit 3 (master
 * read/write) <- Input Terminal 1. */
static void Processing(void)
{
    static const unsigned int defined[] = {
#ifdef UAC_V1
        3, 4, 5, 6      /* 3D-Stereo Extender, Reverberation, Chorus, Dynamic Range Compressor: 14 + n */
#else
        3               /* Stereo Extender: 17 */
#endif
    };
    UAC_BYTE in[2];
    unsigned int k, type, m;
    in[0] = 3; in[1] = 3;
    /* Up/Down-mix and Dolby Prologic with 1 to 3 modes: valid, the master
     * upstream eligible across the one-input opaque unit. Every shorter
     * length is malformed: a mode one byte short (tail truncation), the
     * whole mode array gone with bNrModes kept, bNrModes gone (ADC-2: 17
     * bytes), iProcessing gone. */
    for (type = 1; type <= 2; type++)
        for (m = 1; m <= 3; m++) {
            unsigned int full;
            Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Proc(7, in, 1, type, m, 2); Ot(6, 0x0101, 7);
            full = Length(7);
            CHECK(full == (MODE == 4 ? 18 + 4 * m : 15 + 1 + 2 * m));
            Expect(1, 3, __LINE__);
            Level(1, 3, UAC_VOL_MASTER, 3, UAC_VOL_MASTER, NULL, 0, __LINE__);
            for (k = full - 1; k >= full - MODE * m - 2; k--) {
                Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Proc(7, in, 1, type, m, 2); Ot(6, 0x0101, 7);
                Cut(7, (UAC_BYTE)k);
                Expect(0, 3, __LINE__);
            }
        }
    /* Zero modes: bNrModes alone. */
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Proc(7, in, 1, 1, 0, 2); Ot(6, 0x0101, 7);
    Expect(1, 3, __LINE__);
    /* bNrModes claiming more modes than bLength holds. */
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Proc(7, in, 1, 1, 2, 2); Ot(6, 0x0101, 7);
    bytes[desc.Entity[7] + Length(7) - 1 - 2 * MODE] = 3;
    Expect(0, 3, __LINE__);
    /* The types with no process-specific part: valid at their length, not
     * one byte shorter. */
    for (k = 0; k < sizeof(defined) / sizeof(defined[0]); k++) {
        Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Proc(7, in, 1, defined[k], 0, 2); Ot(6, 0x0101, 7);
        CHECK(Length(7) == (MODE == 4 ? 17 : 15));
        Expect(1, 3, __LINE__);
        Cut(7, (UAC_BYTE)(Length(7) - 1));
        Expect(0, 3, __LINE__);
    }
    /* A process type no specification table defines (undefined 0, the
     * first value after the defined ones, a vendor-looking 0x8001), with
     * no tail or with a tail shaped like one mode: its tail cannot be
     * checked, so the graph is incomplete. */
    for (k = 0; k < 3; k++)
        for (m = 0; m <= 1; m++) {
#ifdef UAC_V1
            static const unsigned int undefined[3] = { 0, 7, 0x8001 };
#else
            static const unsigned int undefined[3] = { 0, 4, 0x8001 };
#endif
            Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Proc(7, in, 1, m ? 1 : 3, 1, 2); Ot(6, 0x0101, 7);
            bytes[desc.Entity[7] + 4] = (UAC_BYTE)undefined[k];
            bytes[desc.Entity[7] + 5] = (UAC_BYTE)(undefined[k] >> 8);
            Expect(0, 3, __LINE__);
        }
    /* A defined type with two input pins (each defined type has one). */
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Proc(7, in, 2, 1, 1, 2); Ot(6, 0x0101, 7);
    Expect(0, 3, __LINE__);
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Proc(7, in, 2, 3, 0, 2); Ot(6, 0x0101, 7);
    Expect(0, 3, __LINE__);
}

static unsigned int Load(const char *path)
{
    unsigned int n;
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    n = (unsigned int)fread(file, 1, sizeof(file), f);
    fclose(f);
    return n;
}

#ifndef UAC_V1
static void X4(const char *path)
{
    static const unsigned int types[4] = { 0x0201, 0x0602, 0x0603, 0x0605 };
    static const UAC_BYTE ends[4] = { 4, 18, 24, 19 }, units[4] = { 2, 23, 25, 8 };
    UAC_BYTE positions[256];
    unsigned char rec[UAC_CAP_DIAG_SIZE];
    unsigned int s, n = Load(path);
    CHECK(UacParse(file, n, 3, 3, 4, &desc) == UAC_OK);
    /* Capture Output Terminal 6 behind Selector Unit 26's four inputs. */
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && !g.Flags);
    CHECK(g.Chooser == 26 && g.SourceCount == 4 && g.SelectorCount == 1 && g.Selector[0] == 26);
    for (s = 0; s < 4; s++) {
        CHECK(g.Source[s].Count == 1 && g.Source[s].Selector[0] == 26 && g.Source[s].Input[0] == s + 1);
        CHECK(g.Source[s].End == ends[s] && g.Source[s].Type == types[s]);
        Level(s + 1, units[s], UAC_VOL_MASTER, units[s], UAC_VOL_MASTER, NULL, 0, __LINE__);
    }
    /* Never the monitoring path, never a playback unit. */
    for (n = 0; n < g.RouteCount; n++) {
        unsigned int i;
        for (i = 0; i < g.Route[n].Length; i++)
            CHECK(g.Route[n].Id[i] != 13 && g.Route[n].Id[i] != 7 && g.Route[n].Id[i] != 1);
    }
    /* Playback's terminal is no capture endpoint. */
    CHECK(UacCapBuild(&desc, 5, &g2) == 0 && g2.Flags == UAC_CAP_MALFORMED);
    /* Diagnostics: the position read, unknown, a stalled selector. */
    memset(positions, 0, sizeof(positions));
    memset(rec, 0xEE, sizeof(rec));
    positions[26] = 3;
    UacCapDiag(&g, positions, NULL, rec);
    CHECK(rec[232] == 4 && rec[236] == 3 && rec[244] == 0 && rec[252] == 0);
    CHECK(rec[256] == 26 && rec[260] == 3 && rec[264] == 0 && rec[268] == 0 && rec[240] == 0xEE);
    positions[26] = 0;
    UacCapDiag(&g, positions, NULL, rec);
    CHECK(rec[236] == 0 && rec[244] == 1);
    CHECK(UacCapConfirmed(&g, positions) == 0);
    positions[26] = 9;
    CHECK(UacCapConfirmed(&g, positions) == 0);
    /* Stage 1: the record at start, no position read: the route unknown. */
    {
        static const UAC_BYTE ids[1] = { 26 };
        Record(&g, 6, 2, 1, 2, 4, 1, 0, ids, 1, __LINE__);
        CHECK(g.Chooser == 26 && g.SourceCount == 4);       /* the graph as UacCapBuild makes it */
        Record(&g, 5, 2, 0, 2, 0, 3, UAC_CAP_MALFORMED, NULL, 0, __LINE__);
    }
}
#else
/* A recorded UAC 1.0 configuration: each entity of its AudioControl
 * interface (class 1, subclass 1) at its offset. */
static void Recorded(const char *path)
{
    unsigned int n = Load(path), i, ac = 0;
    memset(&desc, 0, sizeof(desc));
    desc.Bytes = file; desc.Length = n;
    for (i = 0; i + 2 <= n && file[i] >= 2; i += file[i]) {
        if (file[i + 1] == 0x04 && i + 7 <= n) ac = file[i + 5] == 1 && file[i + 6] == 1;
        else if (ac && file[i + 1] == 0x24 && file[i] >= 4 && i + file[i] <= n && file[i + 2] >= 0x02)
            desc.Entity[file[i + 3]] = (UAC_WORD)i;
    }
}

static void Real(const char *play2, const char *logi, const char *brio)
{
    /* Play! 2: capture Output Terminal 3 <- one-input Selector Unit 10 <-
     * Feature Unit 6 <- microphone 1; no chooser. The speaker's Output
     * Terminal 4 (behind Mixer Unit 9 and the sidetone's Selector Unit 12)
     * is no capture endpoint. */
    Recorded(play2);
    CHECK(UacCapBuild(&desc, 3, &g) == 1 && !g.Flags && g.RouteCount == 1 && !g.Chooser && !g.SourceCount);
    CHECK(g.Route[0].Length == 4 && g.Route[0].Id[1] == 10 && g.Route[0].Id[2] == 6 && g.Route[0].Id[3] == 1);
    Level(1, 6, UAC_VOL_MASTER, 6, UAC_VOL_MASTER, NULL, 0, __LINE__);
    CHECK(UacCapBuild(&desc, 4, &g2) == 0 && g2.Flags == UAC_CAP_MALFORMED);
    /* Logi headset: capture Output Terminal 3 <- Feature Unit 2 (master
     * only) <- microphone 1. */
    Recorded(logi);
    CHECK(UacCapBuild(&desc, 3, &g) == 1 && !g.Flags && g.RouteCount == 1);
    Level(1, 2, UAC_VOL_MASTER, 2, UAC_VOL_MASTER, NULL, 0, __LINE__);
    /* Brio 100: capture Output Terminal 3 <- Feature Unit 5 (master only)
     * <- microphone 1; the video function's entities are not read. */
    Recorded(brio);
    CHECK(UacCapBuild(&desc, 3, &g) == 1 && !g.Flags && g.RouteCount == 1);
    Level(1, 5, UAC_VOL_MASTER, 5, UAC_VOL_MASTER, NULL, 0, __LINE__);
    /* Stage 1: the records at start; no chooser, so the route is known. */
    Record(&g, 3, 1, 1, 1, 0, 0, 0, NULL, 0, __LINE__);
    Recorded(logi);
    Record(&g, 3, 1, 1, 1, 0, 0, 0, NULL, 0, __LINE__);
    Recorded(play2);
    Record(&g, 3, 2, 1, 2, 0, 0, 0, NULL, 0, __LINE__);
    Record(&g, 4, 2, 0, 2, 0, 3, UAC_CAP_MALFORMED, NULL, 0, __LINE__);
}
#endif

static void Built(void)
{
    UAC_BYTE in[80];
    unsigned int i, s;

    /* A diamond: both routes kept, unit 2 once in the selectors' sense but
     * on both routes; it covers the whole signal. */
    Reset(); It(1, 0x0201, 2); Fu(2, 1, 2, both); Fu(3, 2, 2, master); Fu(4, 2, 2, master);
    in[0] = 3; in[1] = 4; Mix(5, in, 2, 4, 2); Ot(6, 0x0101, 5);
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.RouteCount == 2 && !g.Chooser && !g.SourceCount);
    CHECK(g.Route[0].Length == 5 && g.Route[0].Id[2] == 3 && g.Route[1].Id[2] == 4 &&
          g.Route[0].Id[3] == 2 && g.Route[1].Id[3] == 2 && g.Route[0].Pin[1] == 1 && g.Route[1].Pin[1] == 2);
    Level(1, 2, UAC_VOL_MASTER, 2, UAC_VOL_MASTER, NULL, 0, __LINE__);  /* 3 and 4: a part only */

    /* A loop on one route beside a valid one: listed, incomplete, nothing picked. */
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, both); Fu(4, 7, 2, both); Fu(7, 4, 2, both);
    in[0] = 3; in[1] = 4; Mix(5, in, 2, 4, 2); Ot(6, 0x0101, 5);
    CHECK(UacCapBuild(&desc, 6, &g) == 0 && (g.Flags & UAC_CAP_LOOP) && g.RouteCount == 2);
    CHECK(!g.Route[0].Cut && g.Route[1].Cut == UAC_CAP_LOOP && g.Route[1].Id[g.Route[1].Length - 1] == 4);
    Level(1, 0, UAC_VOL_NONE, 0, UAC_VOL_NONE, NULL, 0, __LINE__);
    {
        unsigned char rec[UAC_CAP_DIAG_SIZE];
        memset(rec, 0, sizeof(rec));
        UacCapDiag(&g, NULL, NULL, rec);
        CHECK(rec[244] == 3 && rec[252] == 0x40);
    }

    /* 64 routes are complete; 65 are not. */
    for (s = 64; s <= 65; s++) {
        Reset();
        for (i = 0; i < s; i++) { It((UAC_BYTE)(100 + i), 0x0201, 2); in[i] = (UAC_BYTE)(100 + i); }
        Mix(5, in, s, 2 * s, 2); Ot(6, 0x0101, 5);
        CHECK(UacCapBuild(&desc, 6, &g) == (s == 64) && g.RouteCount == 64);
        CHECK(((g.Flags & UAC_CAP_MANY_ROUTES) != 0) == (s == 65));
    }
    /* Depth: Output Terminal, k Feature Units, Input Terminal. 30 is 32
     * entities; 31 is 33. */
    for (s = 30; s <= 31; s++) {
        Reset(); It(1, 0x0201, 2);
        for (i = 0; i < s; i++) Fu((UAC_BYTE)(10 + i), (UAC_BYTE)(i ? 9 + i : 1), 2, both);
        Ot(6, 0x0101, (UAC_BYTE)(9 + s));
        CHECK(UacCapBuild(&desc, 6, &g) == (s == 30));
        CHECK(((g.Flags & UAC_CAP_TOO_DEEP) != 0) == (s == 31) && g.Route[0].Length == 32);
    }
    /* A source id naming no entity; a unit cut short. */
    Reset(); It(1, 0x0201, 2); Fu(3, 99, 2, both); Ot(6, 0x0101, 3);
    CHECK(UacCapBuild(&desc, 6, &g) == 0 && g.Flags == UAC_CAP_MALFORMED);
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, both); Ot(6, 0x0101, 3);
    desc.Length = desc.Entity[3] + 5;
    CHECK(UacCapBuild(&desc, 6, &g) == 0 && (g.Flags & UAC_CAP_MALFORMED));
    /* An unlisted entity on the audio path; a clock selector is never
     * reached (bCSourceID is not followed). */
    Reset(); It(1, 0x0201, 2); ClockSel(40); Fu(3, 40, 2, both); Ot(6, 0x0101, 3);
    CHECK(UacCapBuild(&desc, 6, &g) == 0 && g.Flags == UAC_CAP_UNLISTED);
    Reset(); It(1, 0x0201, 2); ClockSel(40); Fu(3, 1, 2, both); Ot(6, 0x0101, 3);
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && !g.SelectorCount && g.RouteCount == 1);

    /* A unit on one mixer input is not the level; a unit after the mixer is. */
    Reset(); It(1, 0x0201, 2); It(2, 0x0603, 2); Fu(3, 1, 2, both);
    in[0] = 3; in[1] = 2; Mix(5, in, 2, 4, 2); Ot(6, 0x0101, 5);
    CHECK(UacCapBuild(&desc, 6, &g) == 1);
    Level(1, 0, UAC_VOL_NONE, 0, UAC_VOL_NONE, NULL, 0, __LINE__);
    Reset(); It(1, 0x0201, 2); It(2, 0x0603, 2); Fu(3, 1, 2, both);
    in[0] = 3; in[1] = 2; Mix(5, in, 2, 4, 2); Fu(9, 5, 2, chans); Ot(6, 0x0101, 9);
    CHECK(UacCapBuild(&desc, 6, &g) == 1);
    Level(1, 9, UAC_VOL_CHANNEL, 9, UAC_VOL_CHANNEL, NULL, 0, __LINE__);
    /* A channel-changing mixer: per-channel controls upstream are not
     * eligible, a master is. */
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, chans); in[0] = 3; Mix(5, in, 1, 2, 1); Ot(6, 0x0101, 5);
    CHECK(UacCapBuild(&desc, 6, &g) == 1);
    Level(1, 0, UAC_VOL_NONE, 0, UAC_VOL_NONE, NULL, 0, __LINE__);
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); in[0] = 3; Mix(5, in, 1, 2, 1); Ot(6, 0x0101, 5);
    CHECK(UacCapBuild(&desc, 6, &g) == 1);
    Level(1, 3, UAC_VOL_MASTER, 3, UAC_VOL_MASTER, NULL, 0, __LINE__);
    /* A multi-input Extension Unit: nothing upstream eligible. (Every
     * process type a Processing Unit can have takes one input, so a
     * multi-input Processing Unit is malformed: Processing below.) */
    Reset(); It(1, 0x0201, 2); Fu(2, 1, 2, both); Fu(3, 2, 2, both); Fu(4, 2, 2, both);
    in[0] = 3; in[1] = 4; Xu(7, in, 2, 2); Ot(6, 0x0101, 7);
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.RouteCount == 2);
    Level(1, 0, UAC_VOL_NONE, 0, UAC_VOL_NONE, NULL, 0, __LINE__);
    /* A one-input Extension Unit: a master upstream stays eligible, a
     * per-channel control does not. */
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, both); in[0] = 3; Xu(7, in, 1, 2); Ot(6, 0x0101, 7);
    CHECK(UacCapBuild(&desc, 6, &g) == 1);
    Level(1, 3, UAC_VOL_MASTER, 3, UAC_VOL_MASTER, NULL, 0, __LINE__);
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, chans); in[0] = 3; Xu(7, in, 1, 2); Ot(6, 0x0101, 7);
    CHECK(UacCapBuild(&desc, 6, &g) == 1);
    Level(1, 0, UAC_VOL_NONE, 0, UAC_VOL_NONE, NULL, 0, __LINE__);

    /* A common unit after a chooser: each source's level is that unit. */
    Reset(); It(4, 0x0201, 2); It(5, 0x0603, 2); Fu(2, 4, 2, both); Fu(3, 5, 2, both);
    in[0] = 2; in[1] = 3; Sel(26, in, 2); Fu(9, 26, 2, both); Ot(6, 0x0101, 9);
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.Chooser == 26 && g.SourceCount == 2);
    Level(1, 9, UAC_VOL_MASTER, 9, UAC_VOL_MASTER, NULL, 0, __LINE__);
    Level(2, 9, UAC_VOL_MASTER, 9, UAC_VOL_MASTER, NULL, 0, __LINE__);
    /* Per channel across a chooser: refused even where its inputs' channel
     * counts agree (equal counts do not prove the same channels in the
     * same order); a per-channel unit after the chooser, or behind
     * one-input Pass entities only (a one-input Selector, an SRC, an
     * Effect Unit), is eligible. */
    Reset(); It(4, 0x0201, 1); It(5, 0x0603, 2); Fu(2, 4, 1, chans); Fu(3, 5, 2, chans);
    in[0] = 2; in[1] = 3; Sel(26, in, 2); Ot(6, 0x0101, 26);
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.SourceCount == 2);
    Level(2, 0, UAC_VOL_NONE, 0, UAC_VOL_NONE, NULL, 0, __LINE__);
    Reset(); It(4, 0x0201, 2); It(5, 0x0603, 2); Fu(2, 4, 2, chans); Fu(3, 5, 2, chans);
    in[0] = 2; in[1] = 3; Sel(26, in, 2); Ot(6, 0x0101, 26);
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.SourceCount == 2);
    Level(1, 0, UAC_VOL_NONE, 0, UAC_VOL_NONE, NULL, 0, __LINE__);
    Level(2, 0, UAC_VOL_NONE, 0, UAC_VOL_NONE, NULL, 0, __LINE__);
    Reset(); It(4, 0x0201, 2); It(5, 0x0603, 2); Fu(2, 4, 2, both); Fu(3, 5, 2, both);
    in[0] = 2; in[1] = 3; Sel(26, in, 2); Ot(6, 0x0101, 26);
    CHECK(UacCapBuild(&desc, 6, &g) == 1);
    Level(2, 3, UAC_VOL_MASTER, 3, UAC_VOL_MASTER, NULL, 0, __LINE__);   /* a master needs no correspondence */
    Reset(); It(4, 0x0201, 2); It(5, 0x0603, 2); Fu(2, 4, 2, chans); Fu(3, 5, 2, chans);
    in[0] = 2; in[1] = 3; Sel(26, in, 2); Fu(9, 26, 2, chans); Ot(6, 0x0101, 9);
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.SourceCount == 2);
    Level(1, 9, UAC_VOL_CHANNEL, 9, UAC_VOL_CHANNEL, NULL, 0, __LINE__);
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, chans); in[0] = 3; Sel(11, in, 1); Ot(6, 0x0101, 11);
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.Route[0].Length == 4 && !g.Chooser);
    Level(1, 3, UAC_VOL_CHANNEL, 3, UAC_VOL_CHANNEL, NULL, 0, __LINE__);
#ifndef UAC_V1
    {
        UAC_BYTE e[24] = { 24, 0x24, 0x07, 8, 1, 0, 11 };   /* stereo Effect Unit 8 after selector 11: 16 + 4 * 2 */
        UAC_BYTE r[8] = { 8, 0x24, 0x0d, 10, 8, 40, 40, 0 }; /* SRC 10 after it */
        Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, chans); in[0] = 3; Sel(11, in, 1);
        Add(e); Add(r); Ot(6, 0x0101, 10);
        CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.Route[0].Length == 6 && !g.Chooser);
        Level(1, 3, UAC_VOL_CHANNEL, 3, UAC_VOL_CHANNEL, NULL, 0, __LINE__);
    }
#endif

    /* Selectors in series, inner selector 31 shared by chooser inputs 1
     * and 2 (through unit 7): five sources, in route order. */
    Reset(); It(1, 0x0201, 2); It(2, 0x0603, 2); It(5, 0x0605, 2);
    in[0] = 1; in[1] = 2; Sel(31, in, 2); Fu(7, 31, 2, both);
    in[0] = 31; in[1] = 7; in[2] = 5; Sel(30, in, 3); Ot(6, 0x0101, 30);
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.Chooser == 30 && g.SourceCount == 5);
    CHECK(g.SelectorCount == 2 && g.Selector[0] == 30 && g.Selector[1] == 31);
    CHECK(g.Source[0].Count == 2 && g.Source[0].Input[0] == 1 && g.Source[0].Selector[1] == 31 &&
          g.Source[0].Input[1] == 1 && g.Source[0].End == 1);
    CHECK(g.Source[1].Input[0] == 1 && g.Source[1].Input[1] == 2 && g.Source[1].End == 2);
    CHECK(g.Source[2].Input[0] == 2 && g.Source[2].Input[1] == 1 && g.Source[3].Input[1] == 2);
    CHECK(g.Source[4].Count == 1 && g.Source[4].Input[0] == 3 && g.Source[4].Type == 0x0605);
    {
        UAC_BYTE positions[256];
        memset(positions, 0, sizeof(positions));
        positions[30] = 2; positions[31] = 2;
        CHECK(UacCapConfirmed(&g, positions) == 4);
        positions[31] = 0;
        CHECK(UacCapConfirmed(&g, positions) == 0);
        positions[30] = 3;
        CHECK(UacCapConfirmed(&g, positions) == 5);
    }
    /* Truncated tails: each supported entity one byte short of its last
     * field is malformed, the graph incomplete, nothing eligible. */
    {
        static const struct { UAC_BYTE id, length; } cuts[] = {
            { 26, SEL_LENGTH(2) - 1 },  /* two-input Selector */
            { 26, SEL_LENGTH(2) - 2 },  /* two bytes short */
#ifdef UAC_V1
            { 2, 7 },                   /* Feature Unit: its master bitmap gone */
            { 2, 9 },                   /* a whole channel entry short: one entry for a stereo cluster */
#else
            { 2, 13 },                  /* Feature Unit, two channels: 18; not 6 + 4k */
            { 2, 14 },                  /* a whole channel entry short: one entry for a stereo cluster */
#endif
            { 4, IT_LENGTH - 1 },       /* Input Terminal */
            { 9, MIX_LENGTH(1, 1) - 1 },            /* a mixer after the chooser, its iMixer gone */
            { 9, MIX_LENGTH(1, 1) - MIX_AFTER - 1 } /* its bmMixerControls gone too */
        };
        unsigned int k;
        for (k = 0; k < sizeof(cuts) / sizeof(cuts[0]); k++) {
            Reset(); It(4, 0x0201, 2); It(5, 0x0603, 2); Fu(2, 4, 2, both); Fu(3, 5, 2, both);
            in[0] = 2; in[1] = 3; Sel(26, in, 2); in[0] = 26; Mix(9, in, 1, 2, 2);
            Ot(6, 0x0101, 9);
            CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.SourceCount == 0);   /* a join first: no chooser */
            Cut(cuts[k].id, cuts[k].length);
            CHECK(UacCapBuild(&desc, 6, &g) == 0 && (g.Flags & UAC_CAP_MALFORMED));
            Level(1, 0, UAC_VOL_NONE, 0, UAC_VOL_NONE, NULL, 0, __LINE__);
        }
        /* Extension (and Effect and SRC) one byte short; Processing:
         * Processing below. */
        Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); in[0] = 3; Xu(7, in, 1, 2); Ot(6, 0x0101, 7);
        CHECK(UacCapBuild(&desc, 6, &g) == 1);
        Cut(7, (UAC_BYTE)(Length(7) - 1));
        CHECK(UacCapBuild(&desc, 6, &g) == 0 && g.Flags == UAC_CAP_MALFORMED);
#ifndef UAC_V1
        {
            UAC_BYTE e[24] = { 24, 0x24, 0x07, 8, 1, 0, 3, 0x0f };   /* stereo Effect Unit 8 after unit 3 */
            UAC_BYTE r[8] = { 8, 0x24, 0x0d, 10, 8, 40, 40, 0 };      /* SRC 10 after it */
            Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Add(e); Add(r); Ot(6, 0x0101, 10);
            CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.Route[0].Length == 5);
            Cut(8, 23);
            CHECK(UacCapBuild(&desc, 6, &g) == 0 && g.Flags == UAC_CAP_MALFORMED);
            Cut(8, 24); Cut(10, 7);
            CHECK(UacCapBuild(&desc, 6, &g) == 0 && g.Flags == UAC_CAP_MALFORMED);
        }
#endif
        /* Output Terminal one byte short: no graph. */
        Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Ot(6, 0x0101, 3);
        CHECK(UacCapBuild(&desc, 6, &g) == 1);
        Cut(6, (UAC_BYTE)(Length(6) - 1));
        CHECK(UacCapBuild(&desc, 6, &g) == 0 && g.Flags == UAC_CAP_MALFORMED && !g.RouteCount);
    }
    /* A selector behind a join: no chooser; listed for the diagnostics. */
    Reset(); It(1, 0x0201, 2); It(2, 0x0603, 2); It(3, 0x0605, 2);
    in[0] = 1; in[1] = 2; Sel(30, in, 2); in[0] = 30; in[1] = 3; Mix(5, in, 2, 4, 2);
    Ot(6, 0x0101, 5);
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && !g.Chooser && !g.SourceCount && g.SelectorCount == 1);
    /* 8 sources; 9: no chooser. A "Stereo Mix" branch is a source. */
    for (s = 8; s <= 9; s++) {
        Reset();
        for (i = 0; i < s; i++) { It((UAC_BYTE)(100 + i), i ? 0x0603 : 0x0101, 2); in[i] = (UAC_BYTE)(100 + i); }
        Sel(30, in, s); Ot(6, 0x0101, 30);
        CHECK(UacCapBuild(&desc, 6, &g) == 1);
        CHECK(s == 8 ? g.SourceCount == 8 && g.Chooser == 30 && g.Source[0].Type == 0x0101
                     : !g.SourceCount && !g.Chooser && g.Flags == UAC_CAP_MANY_SOURCES);
    }
}

/* Mixer Units: bmMixerControls is N bytes, N established from every input
 * pin's channels and bNrChannels (ADC-2 Table 4-11, ADC-1 4.3.2.3). The
 * graph: Output Terminal 6 <- Feature Unit 9 (read/write master) <- Mixer
 * Unit 5 <- Input Terminals 100.. */
static void Mixer(void)
{
    static const struct { unsigned int n, ch[4], m; } shapes[] = {
        { 1, { 2, 0, 0, 0 }, 2 },   /* one stereo input to stereo: 4 bits, N 1 */
        { 2, { 2, 2, 0, 0 }, 2 },   /* 8 bits, N 1 */
        { 2, { 1, 2, 0, 0 }, 2 },   /* mono and stereo: 6 bits, N 1 */
        { 2, { 2, 2, 0, 0 }, 3 },   /* 12 bits, N 2 */
        { 4, { 8, 2, 2, 2 }, 8 }    /* the X4's Mixer Unit 7: 112 bits, N 14 */
    };
    UAC_BYTE in[4];
    unsigned int k, i, total, N, full, cut;
    for (k = 0; k < sizeof(shapes) / sizeof(shapes[0]); k++) {
        for (total = 0, i = 0; i < shapes[k].n; i++) total += shapes[k].ch[i];
        N = MIX_BYTES(total, shapes[k].m);
        /* Valid; then every shorter length, from iMixer gone through the
         * whole bitmap gone (the length a mixer with no bitmap would
         * have, N bytes short, included): incomplete, the unit after the
         * mixer neither eligible nor claimed. */
        for (cut = 0; cut <= N + MIX_AFTER; cut++) {
            Reset();
            for (i = 0; i < shapes[k].n; i++) { It((UAC_BYTE)(100 + i), 0x0201, (UAC_BYTE)shapes[k].ch[i]); in[i] = (UAC_BYTE)(100 + i); }
            Mix(5, in, shapes[k].n, total, (UAC_BYTE)shapes[k].m); Fu(9, 5, shapes[k].m, both); Ot(6, 0x0101, 9);
            full = Length(5);
            CHECK(full == MIX_LENGTH(shapes[k].n, N));
            Cut(5, (UAC_BYTE)(full - cut));
            Expect(!cut, 9, __LINE__);
        }
    }
    /* The bitmap sized for fewer output channels than bNrChannels says
     * (stereo in, 5 out: 10 bits, N 2, one byte given). Feature Unit 9 is
     * master only here, so the mixer alone decides. */
    Reset(); It(100, 0x0201, 2); in[0] = 100; Mix(5, in, 1, 2, 2); Fu(9, 5, 0, both); Ot(6, 0x0101, 9);
    Expect(1, 9, __LINE__);
    bytes[desc.Entity[5] + 6] = 5;
    Expect(0, 9, __LINE__);
    /* An input whose channel count is not defined: a Selector whose inputs
     * differ (mono and stereo), an Input Terminal of no channels. */
    Reset(); It(100, 0x0201, 1); It(101, 0x0603, 2); in[0] = 100; in[1] = 101; Sel(30, in, 2);
    in[0] = 30; Mix(5, in, 1, 2, 2); Fu(9, 5, 0, both); Ot(6, 0x0101, 9);
    Expect(0, 9, __LINE__);
    Reset(); It(100, 0x0201, 2); It(101, 0x0603, 2); in[0] = 100; in[1] = 101; Sel(30, in, 2);
    in[0] = 30; Mix(5, in, 1, 2, 2); Fu(9, 5, 0, both); Ot(6, 0x0101, 9);
    Expect(1, 9, __LINE__);                     /* inputs that agree: defined */
    Reset(); It(100, 0x0201, 0); in[0] = 100; Mix(5, in, 1, 0, 2); Fu(9, 5, 0, both); Ot(6, 0x0101, 9);
    Expect(0, 9, __LINE__);
}

/* Feature and Effect Units: the per-channel array has one entry per
 * logical channel of the cluster actually entering the unit, not merely a
 * length the entry size divides (ADC-2 Tables 4-13 and 4-15 to 4-19,
 * ADC-1 Table 4-7). A Feature Unit with the master's entry alone is
 * accepted on any cluster (the recorded Logi headset and Brio 100 have
 * such units): no channel's controls are read from it. */
static void Feature(void)
{
    static const unsigned int widths[3] = { 1, 2, 8 };
    static UAC_CLAIMS t;
    UAC_CAP_ENDPOINT ep;
    UAC_BYTE in[2], d[64];
    unsigned int k, c, cut, full;
    /* Output Terminal 6 <- Feature Unit 3 (mute and volume read/write on
     * master and channels) <- Input Terminal 1 of c channels: valid at
     * its length; one entry too many, and every shorter length (iFeature
     * gone, part of an entry, whole channel entries, the master's entry)
     * malformed, except the master's entry alone. */
    for (k = 0; k < 3; k++) {
        c = widths[k];
        full = FU_LENGTH(c);
        for (cut = 0; full - cut >= 5; cut++) {
            Reset(); It(1, 0x0201, (UAC_BYTE)c); Fu(3, 1, c, both); Ot(6, 0x0101, 3);
            Cut(3, (UAC_BYTE)(full - cut));
            Expect(!cut || full - cut == FU_LENGTH(0), 3, __LINE__);
        }
        Reset(); It(1, 0x0201, (UAC_BYTE)c); Fu(3, 1, c + 1, both); Ot(6, 0x0101, 3);
        Expect(0, 3, __LINE__);
    }
    /* A stereo unit with channel controls only, cut to the master's entry:
     * accepted, but none of its channel controls is read, eligible or
     * claimed. */
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, chans); Ot(6, 0x0101, 3);
    Cut(3, FU_LENGTH(0));
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && UacCapUnitChannels(&desc, 3) == 0);
    Level(1, 0, UAC_VOL_NONE, 0, UAC_VOL_NONE, NULL, 0, __LINE__);
    memset(&ep, 0, sizeof(ep));
    ep.Capture = 1; ep.Graph = &g; ep.Channels = 2;
    UacCapClaims(&desc, &ep, 1, &t);
    CHECK(t.Count == 0);
#ifdef UAC_V1
    /* bControlSize 2, stereo: 7 + 3 * 2 = 13; 12 and 10 part of an entry,
     * 11 a channel entry short, 9 the master's alone. */
    for (cut = 0; cut <= 4; cut++) {
        memset(d, 0, sizeof(d));
        d[0] = 13; d[1] = 0x24; d[2] = 0x06; d[3] = 3; d[4] = 1; d[5] = 2; d[6] = 0x03; d[8] = 0x03; d[10] = 0x03;
        Reset(); It(1, 0x0201, 2); Add(d); Ot(6, 0x0101, 3);
        Cut(3, (UAC_BYTE)(13 - cut));
        Expect(cut == 0 || cut == 4, 3, __LINE__);
    }
#endif
    /* The entering cluster as established upstream: after a Processing
     * Unit (its bNrChannels, 6), after an Extension Unit (1); after a
     * Selector whose inputs differ there is none, so only a master-only
     * unit (the tolerated nonconforming layout) is accepted. */
    in[0] = 3;
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Proc(7, in, 1, 1, 1, 6); Fu(9, 7, 6, both); Ot(6, 0x0101, 9);
    Expect(1, 9, __LINE__);
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Proc(7, in, 1, 1, 1, 6); Fu(9, 7, 2, both); Ot(6, 0x0101, 9);
    Expect(0, 9, __LINE__);
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Xu(7, in, 1, 1); Fu(9, 7, 1, both); Ot(6, 0x0101, 9);
    Expect(1, 9, __LINE__);
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, master); Xu(7, in, 1, 1); Fu(9, 7, 2, both); Ot(6, 0x0101, 9);
    Expect(0, 9, __LINE__);
    in[0] = 1; in[1] = 2;
    Reset(); It(1, 0x0201, 1); It(2, 0x0603, 2); Sel(30, in, 2); Fu(9, 30, 2, both); Ot(6, 0x0101, 9);
    Expect(0, 9, __LINE__);
    Reset(); It(1, 0x0201, 1); It(2, 0x0603, 2); Sel(30, in, 2); Fu(9, 30, 0, both); Ot(6, 0x0101, 9);
    CHECK(UacCapBuild(&desc, 6, &g) == 1);
    Level(1, 9, UAC_VOL_MASTER, 9, UAC_VOL_MASTER, NULL, 0, __LINE__);
    Level(2, 9, UAC_VOL_MASTER, 9, UAC_VOL_MASTER, NULL, 0, __LINE__);
#ifndef UAC_V1
    /* Effect Units, 16 + 4 * ch: Output Terminal 6 <- Effect Unit 8 <-
     * Feature Unit 3 (master) <- Input Terminal 1 of c channels. Valid at
     * that length; one entry too many and every shorter length malformed
     * (iEffects gone, part of an entry, whole channel entries, 12 + 4 * ch
     * and the 16 of no channel entry among them). */
    for (c = 1; c <= 2; c++) {
        full = 16 + 4 * c;
        for (cut = 0; cut <= full - 5 + 4; cut++) {
            memset(d, 0, sizeof(d));
            d[0] = (UAC_BYTE)(full + 4 - cut); d[1] = 0x24; d[2] = 0x07; d[3] = 8; d[4] = 1; d[6] = 3; d[7] = 0x0f;
            Reset(); It(1, 0x0201, (UAC_BYTE)c); Fu(3, 1, c, master); Add(d); Ot(6, 0x0101, 8);
            Expect(cut == 4, 3, __LINE__);
        }
    }
#endif
}

/* Two playback and two capture endpoints: the order of 28.5. */
static void Claims(void)
{
    static UAC_CLAIMS t;
    UAC_CAP_ENDPOINT eps[4];
    UAC_CAP_CONTROL p0[2], p1[2], n0[2], c;
    UAC_BYTE in[4];
    unsigned int kind;

    /* Capture A (OT 6): chooser 26 over units 2 and 3; capture B (OT 16):
     * through the same chooser after unit 9. Playback: units 11 and 12. */
    Reset(); It(4, 0x0201, 2); It(5, 0x0603, 2); Fu(2, 4, 2, both); Fu(3, 5, 2, both);
    in[0] = 2; in[1] = 3; Sel(26, in, 2); Ot(6, 0x0101, 26);
    Fu(9, 26, 2, both); Ot(16, 0x0101, 9);
    It(20, 0x0101, 2); Fu(11, 20, 2, both); Ot(21, 0x0301, 11);
    It(22, 0x0101, 2); Fu(12, 22, 2, both); Ot(23, 0x0302, 12);
    CHECK(UacCapBuild(&desc, 6, &g) == 1 && g.SourceCount == 2);
    CHECK(UacCapBuild(&desc, 16, &g2) == 1 && g2.SourceCount == 2 && g2.Chooser == 26);
    p0[0].Entity = 11; p0[0].Selector = 2; p0[0].Channel = 0; p0[1] = p0[0]; p0[1].Selector = 1;
    p1[0].Entity = 12; p1[0].Selector = 2; p1[0].Channel = 0; p1[1] = p1[0]; p1[1].Selector = 1;
    /* Playback 0 normalises unit 2's channel volume 1 (a unit whose
     * controls go to different holders) and unit 11's master mute, which
     * is its own level already. */
    n0[0].Entity = 2; n0[0].Selector = 2; n0[0].Channel = 1; n0[1] = p0[1];
    memset(eps, 0, sizeof(eps));
    eps[0].Capture = 0; eps[0].Level = p0; eps[0].LevelCount = 2; eps[0].Normal = n0; eps[0].NormalCount = 2;
    eps[1].Capture = 1; eps[1].Graph = &g; eps[1].Channels = 2;
    eps[2].Capture = 0; eps[2].Level = p1; eps[2].LevelCount = 2;
    eps[3].Capture = 1; eps[3].Graph = &g2; eps[3].Channels = 2;
    UacCapClaims(&desc, eps, 4, &t);
    CHECK(t.Sealed && !t.Refused);
    CHECK(UacClaimHolder(&t, &p0[0], &kind) == 0 && kind == UAC_CLAIM_LEVEL);
    CHECK(UacClaimHolder(&t, &p1[1], &kind) == 2 && kind == UAC_CLAIM_LEVEL);
    c.Entity = 26; c.Selector = 1; c.Channel = 0;
    CHECK(UacClaimHolder(&t, &c, &kind) == 1 && kind == UAC_CLAIM_SELECTOR);   /* first wins */
    CHECK(!UacClaimMay(&t, &c, 3) && UacClaimMay(&t, &c, 1));
    c.Entity = 2; c.Selector = 2; c.Channel = 0;
    CHECK(UacClaimHolder(&t, &c, &kind) == 1 && kind == UAC_CLAIM_LEVEL);
    c.Entity = 3; c.Selector = 1;
    CHECK(UacClaimHolder(&t, &c, &kind) == 1 && kind == UAC_CLAIM_LEVEL);
    /* B's sources: units 2 and 3 are A's, so its level is unit 9 for both. */
    c.Entity = 9; c.Selector = 2; c.Channel = 0;
    CHECK(UacClaimHolder(&t, &c, &kind) == 3 && kind == UAC_CLAIM_LEVEL);
    /* Unit 2's channel 1 volume: A's pass reservation came after playback
     * 0's normalisation, so it is playback 0's; channel 2's is A's. */
    c.Entity = 2; c.Selector = 2; c.Channel = 1;
    CHECK(UacClaimHolder(&t, &c, &kind) == 0 && kind == UAC_CLAIM_NORMAL);
    c.Channel = 2;
    CHECK(UacClaimHolder(&t, &c, &kind) == 1 && kind == UAC_CLAIM_RESERVED);
    /* Unit 9's channel controls: B's reservations. */
    c.Entity = 9; c.Selector = 1; c.Channel = 2;
    CHECK(UacClaimHolder(&t, &c, &kind) == 3 && kind == UAC_CLAIM_RESERVED);
    /* Sealed: no later claim, even on an unheld control. */
    c.Entity = 77; c.Selector = 2; c.Channel = 0;
    CHECK(!UacClaimTake(&t, &c, 0, UAC_CLAIM_LEVEL) && t.Refused == 1 && UacClaimHolder(&t, &c, NULL) < 0);
    /* B's own pick against the sealed table never lands on A's units. */
    {
        UAC_CAP_LEVEL lv;
        UacCapPick(&desc, &g2, 1, 2, &t, 3, &lv);
        CHECK(lv.VolumeUnit == 9 && lv.MuteUnit == 9);
        UacCapPick(&desc, &g2, 1, 2, NULL, 3, &lv);
        CHECK(lv.VolumeUnit == 9);  /* nearest the Output Terminal anyway */
    }
    /* An incomplete capture graph claims nothing. */
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, both); Fu(4, 7, 2, both); Fu(7, 4, 2, both);
    in[0] = 3; in[1] = 4; Mix(5, in, 2, 4, 2); Ot(6, 0x0101, 5);
    CHECK(UacCapBuild(&desc, 6, &g) == 0);
    memset(eps, 0, sizeof(eps));
    eps[0].Capture = 1; eps[0].Graph = &g; eps[0].Channels = 2;
    UacCapClaims(&desc, eps, 1, &t);
    CHECK(t.Count == 0 && t.Sealed);
}

/* Stage 1 on built graphs, in the build's family: a chooser, none, a
 * selector behind a join, an incomplete graph (a loop beside a valid
 * route; a truncated unit), no storage, the node's channel count, and the
 * value names. */
static void Start(void)
{
    UAC_BYTE in[4];
    unsigned short name[UAC_CAP_NAME_UNITS + 2];
    static const UAC_BYTE ids2[2] = { 30, 31 }, ids1[1] = { 31 };
    static const struct { unsigned int pair; const char *text; } names[] = {
        { 0, "Uac2VolMapC" }, { 1, "Uac2VolMapC01" }, { 3, "Uac2VolMapC03" }, { 0x1F, "Uac2VolMapC1F" },
        { 0xAB, "Uac2VolMapCAB" }
    };
    unsigned int i, k;
    /* A chooser of two sources: route unknown until read. */
    in[0] = 1; in[1] = 2;
    Reset(); It(1, 0x0201, 2); It(2, 0x0603, 2); Sel(30, in, 2); Fu(9, 30, 2, both); Ot(6, 0x0101, 9);
    Record(&g, 6, 2, 1, 2, 2, 1, 0, ids2, 1, __LINE__);
    CHECK(g.Chooser == 30 && g.SourceCount == 2 && g.Source[1].Type == 0x0603);
    /* No chooser (one input): the route known; a mono node. */
    Reset(); It(1, 0x0201, 1); Fu(3, 1, 1, both); Ot(6, 0x0101, 3);
    Record(&g, 6, 1, 1, 1, 0, 0, 0, NULL, 0, __LINE__);
    Record(&g, 6, 0, 1, 1, 0, 0, 0, NULL, 0, __LINE__);          /* no formats' count: 1 */
    Record(&g, 6, 6, 1, 2, 0, 0, 0, NULL, 0, __LINE__);          /* six channels: a pair */
    /* A selector behind a join: listed, no chooser, no source count. */
    in[0] = 1; in[1] = 2;
    Reset(); It(1, 0x0201, 2); It(2, 0x0603, 2); Sel(31, in, 2); Fu(3, 31, 2, both); Fu(4, 1, 2, both);
    in[0] = 3; in[1] = 4; Mix(5, in, 2, 4, 2); Ot(6, 0x0101, 5);
    Record(&g, 6, 2, 1, 2, 0, 0, 0, ids1, 1, __LINE__);
    CHECK(!g.Chooser && g.SelectorCount == 1);
    /* A loop beside a valid route: incomplete, RouteState 3. */
    Reset(); It(1, 0x0201, 2); Fu(3, 1, 2, both); Fu(4, 7, 2, both); Fu(7, 4, 2, both);
    in[0] = 3; in[1] = 4; Mix(5, in, 2, 4, 2); Ot(6, 0x0101, 5);
    Record(&g, 6, 2, 0, 2, 0, 3, UAC_CAP_LOOP, NULL, 0, __LINE__);
    /* A chooser whose unit after it is truncated: incomplete, and the
     * chooser not offered (Sources 0); no selector is reached. */
    in[0] = 1; in[1] = 2;
    Reset(); It(1, 0x0201, 2); It(2, 0x0603, 2); Sel(30, in, 2); Fu(9, 30, 2, both); Ot(6, 0x0101, 9);
    Cut(9, (UAC_BYTE)(Length(9) - 1));
    Record(&g, 6, 2, 0, 2, 0, 3, UAC_CAP_MALFORMED, NULL, 0, __LINE__);
    /* No storage: RouteState 3, GraphFlags "not built", nothing else read. */
    Record(NULL, 6, 2, 0, 2, 0, 3, UAC_CAP_NOT_BUILT, NULL, 0, __LINE__);
    CHECK((UAC_CAP_INCOMPLETE & UAC_CAP_NOT_BUILT) && !(UAC_CAP_INCOMPLETE & (UAC_CAP_MANY_SOURCES | UAC_CAP_CHOOSER_HELD)));
    CHECK(UacCapStart(&desc, 6, 2, &g, NULL) == 0);
    /* Names (28.12). */
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        for (k = 0; k < UAC_CAP_NAME_UNITS + 2; k++) name[k] = 0xEEEE;
        UacCapDiagName(names[i].pair, name);
        for (k = 0; names[i].text[k]; k++) CHECK(name[k] == (unsigned short)names[i].text[k]);
        CHECK(name[k] == 0 && k + 1 <= UAC_CAP_NAME_UNITS && name[UAC_CAP_NAME_UNITS] == 0xEEEE);
    }
}

int main(int argc, char **argv)
{
#ifdef UAC_V1
    if (argc != 4) { printf("usage: captest play2.bin logi0a64.bin brio100.bin\n"); return 2; }
    Real(argv[1], argv[2], argv[3]);
#else
    if (argc != 2) { printf("usage: captest x4-config0.bin\n"); return 2; }
    X4(argv[1]);
#endif
    Built();
    Processing();
    Mixer();
    Feature();
    Claims();
    Start();
#ifdef UAC_V1
    printf("captest (UAC 1.0): %u checks passed\n", checks);
#else
    printf("captest: %u checks passed\n", checks);
#endif
    return 0;
}

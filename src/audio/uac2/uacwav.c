/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original WinMM test for uac2 beyond one format (test only):
 *   UACWAV list                          wave devices and their caps
 *   UACWAV play <ch> <bits> <rate> <s>   a quiet 440 Hz sine on the first
 *                                        "UAC2" output, queued buffers
 *   UACWAV rec <ch> <bits> <rate> <s>    record from the first "UAC2"
 *                                        input; non-silent frames and the
 *                                        tone's frequency from zero crossings
 *   UACWAV mix                           the mixer of the first "UAC2"
 *                                        output: lines, controls, values
 *   UACWAV vol <v> [right]               its speaker line's volume, 0-65535
 *   UACWAV mute <0|1>                    its speaker line's mute
 *   UACWAV ramp <s>                      play 2ch 16-bit 48 kHz while the
 *                                        speaker volume steps 65535, 32768,
 *                                        16384, 4096, then mute on and off,
 *                                        <s> seconds a step; then restores
 *                                        the volume and mute it found
 * The mixer commands go through the mixer API, the path the Volume
 * Control (sndvol32) takes.
 * Bits 16 is WAVE_FORMAT_PCM for 1-2 channels; 24/32 bits or more than two
 * channels is WAVE_FORMAT_EXTENSIBLE (container = bits). Output on stdout;
 * the last line is PASS: or FAIL:. Never claims audibility.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* WAVEFORMATEXTENSIBLE without the SDK's ksmedia.h (VC6). */
typedef struct {
    WAVEFORMATEX Format;
    WORD ValidBits;
    DWORD ChannelMask;
    GUID SubFormat;
} WFX;
static const GUID PcmSub = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
#define BLOCKS 8

static void Format(WFX *w, unsigned ch, unsigned bits, unsigned rate)
{
    ZeroMemory(w, sizeof(*w));
    w->Format.nChannels = (WORD)ch; w->Format.nSamplesPerSec = rate;
    w->Format.wBitsPerSample = (WORD)bits;
    w->Format.nBlockAlign = (WORD)(ch * bits / 8);
    w->Format.nAvgBytesPerSec = rate * w->Format.nBlockAlign;
    if (bits == 16 && ch <= 2) { w->Format.wFormatTag = WAVE_FORMAT_PCM; return; }
    w->Format.wFormatTag = 0xFFFE; /* WAVE_FORMAT_EXTENSIBLE */
    w->Format.cbSize = 22;
    w->ValidBits = (WORD)bits;
    w->ChannelMask = ch >= 32 ? 0 : (1UL << ch) - 1;
    w->SubFormat = PcmSub;
}

static int Uac2(const char *name)
{
    char upper[MAXPNAMELEN];
    lstrcpyn(upper, name, sizeof(upper)); CharUpperBuff(upper, lstrlen(upper));
    return strstr(upper, "UAC2") != NULL;
}

static UINT List(int print, int input)
{
    UINT n, i, chosen = (UINT)-1;
    if (input || print) {
        WAVEINCAPS c;
        n = waveInGetNumDevs();
        if (print) printf("waveIn devices=%u\n", n);
        for (i = 0; i < n; i++) if (!waveInGetDevCaps(i, &c, sizeof(c))) {
            if (print) printf("  in %u: %s channels=%u formats=%08lX\n", i, c.szPname, c.wChannels, c.dwFormats);
            if (input && Uac2(c.szPname) && chosen == (UINT)-1) chosen = i;
        }
    }
    if (!input || print) {
        WAVEOUTCAPS c;
        n = waveOutGetNumDevs();
        if (print) printf("waveOut devices=%u\n", n);
        for (i = 0; i < n; i++) if (!waveOutGetDevCaps(i, &c, sizeof(c))) {
            if (print) printf("  out %u: %s channels=%u formats=%08lX\n", i, c.szPname, c.wChannels, c.dwFormats);
            if (!input && Uac2(c.szPname) && chosen == (UINT)-1) chosen = i;
        }
    }
    return chosen;
}

/* One sample of a quiet sine, written little-endian in a bits/8 slot. */
static void Put(unsigned char *p, unsigned bits, double v)
{
    long s = (long)(v * 0.1 * (bits == 16 ? 32767.0 : 8388607.0));
    if (bits == 32) s <<= 8;
    p[0] = (unsigned char)s; p[1] = (unsigned char)(s >> 8);
    if (bits >= 24) p[2] = (unsigned char)(s >> 16);
    if (bits == 32) p[3] = (unsigned char)(s >> 24);
}

static long Get(const unsigned char *p, unsigned bits)
{
    long s;
    if (bits == 16) return (short)(p[0] | (p[1] << 8));
    s = p[0] | (p[1] << 8) | ((long)p[2] << 16);
    if (bits == 24) { if (s & 0x800000) s -= 0x1000000; return s; }
    s = (long)(p[0] | (p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24));
    return s >> 8;
}

static int Play(unsigned ch, unsigned bits, unsigned rate, unsigned secs)
{
    WFX w; HWAVEOUT h; WAVEHDR hdr[BLOCKS]; HANDLE ev; MMRESULT r;
    UINT dev = List(0, 0), i, done = 0, frames, f, c;
    unsigned char *buf[BLOCKS];
    DWORD start, elapsed;
    double phase = 0, step = 2 * 3.14159265358979 * 440.0 / rate;
    if (dev == (UINT)-1) { printf("FAIL: no UAC2 output\n"); return 1; }
    Format(&w, ch, bits, rate);
    frames = rate * secs / BLOCKS;
    ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    r = waveOutOpen(&h, dev, (WAVEFORMATEX *)&w, (DWORD)ev, 0, CALLBACK_EVENT);
    printf("play dev=%u %uch %ubit %uHz %us: open=%u\n", dev, ch, bits, rate, secs, r);
    if (r) { printf("FAIL: open\n"); return 1; }
    for (i = 0; i < BLOCKS; i++) {
        buf[i] = (unsigned char *)malloc(frames * w.Format.nBlockAlign);
        for (f = 0; f < frames; f++, phase += step)
            for (c = 0; c < ch; c++) Put(buf[i] + f * w.Format.nBlockAlign + c * (bits / 8), bits, sin(phase));
        ZeroMemory(&hdr[i], sizeof(hdr[i]));
        hdr[i].lpData = (LPSTR)buf[i]; hdr[i].dwBufferLength = frames * w.Format.nBlockAlign;
        waveOutPrepareHeader(h, &hdr[i], sizeof(hdr[i]));
    }
    start = GetTickCount();
    for (i = 0; i < BLOCKS; i++) waveOutWrite(h, &hdr[i], sizeof(hdr[i]));
    while (done < BLOCKS && GetTickCount() - start < secs * 1000 + 10000) {
        WaitForSingleObject(ev, 200);
        for (done = 0, i = 0; i < BLOCKS; i++) if (hdr[i].dwFlags & WHDR_DONE) done++;
    }
    elapsed = GetTickCount() - start;
    printf("completed=%u/%u elapsed_ms=%lu\n", done, BLOCKS, elapsed);
    printf("reset=%u\n", waveOutReset(h));
    for (i = 0; i < BLOCKS; i++) { waveOutUnprepareHeader(h, &hdr[i], sizeof(hdr[i])); free(buf[i]); }
    printf("close=%u\n", waveOutClose(h));
    printf(done == BLOCKS ? "PASS: all buffers played\n" : "FAIL: buffers not completed\n");
    return done == BLOCKS ? 0 : 1;
}

/* The mixer of the first UAC2 output and its speaker destination line. */
static int Mixer(HMIXER *mx, MIXERLINE *line)
{
    UINT dev = List(0, 0), id;
    MMRESULT r;
    if (dev == (UINT)-1) { printf("FAIL: no UAC2 output\n"); return 0; }
    r = mixerGetID((HMIXEROBJ)(UINT)dev, &id, MIXER_OBJECTF_WAVEOUT);
    if (r) { printf("FAIL: no mixer for waveOut %u (%u)\n", dev, r); return 0; }
    r = mixerOpen(mx, id, 0, 0, MIXER_OBJECTF_MIXER);
    if (r) { printf("FAIL: mixerOpen %u (%u)\n", id, r); return 0; }
    ZeroMemory(line, sizeof(*line));
    line->cbStruct = sizeof(*line);
    line->dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
    r = mixerGetLineInfo((HMIXEROBJ)*mx, line, MIXER_GETLINEINFOF_COMPONENTTYPE);
    if (r) { printf("FAIL: no speaker line (%u)\n", r); mixerClose(*mx); return 0; }
    printf("mixer %u for waveOut %u: line \"%s\" channels=%lu controls=%lu\n", id, dev,
           line->szName, line->cChannels, line->cControls);
    return 1;
}

static int Control(HMIXER mx, const MIXERLINE *line, DWORD type, MIXERCONTROL *c)
{
    MIXERLINECONTROLS lc;
    ZeroMemory(&lc, sizeof(lc)); ZeroMemory(c, sizeof(*c));
    lc.cbStruct = sizeof(lc); lc.dwLineID = line->dwLineID; lc.dwControlType = type;
    lc.cControls = 1; lc.cbmxctrl = sizeof(*c); lc.pamxctrl = c;
    return !mixerGetLineControls((HMIXEROBJ)mx, &lc, MIXER_GETLINECONTROLSF_ONEBYTYPE);
}

/* Read (set=0) or write a control's values, one per channel (uniform: one). */
static MMRESULT Values(HMIXER mx, const MIXERLINE *line, const MIXERCONTROL *c, DWORD *v, int set)
{
    MIXERCONTROLDETAILS d;
    DWORD n = (c->fdwControl & MIXERCONTROL_CONTROLF_UNIFORM) ? 1 : line->cChannels;
    ZeroMemory(&d, sizeof(d));
    d.cbStruct = sizeof(d); d.dwControlID = c->dwControlID; d.cChannels = n ? n : 1;
    d.cbDetails = sizeof(DWORD); d.paDetails = v;
    return set ? mixerSetControlDetails((HMIXEROBJ)mx, &d, MIXER_SETCONTROLDETAILSF_VALUE)
               : mixerGetControlDetails((HMIXEROBJ)mx, &d, MIXER_GETCONTROLDETAILSF_VALUE);
}

static int Mix(void)
{
    HMIXER mx; MIXERLINE line; MIXERCAPS caps; UINT d;
    if (!Mixer(&mx, &line)) return 1;
    mixerGetDevCaps((UINT)mx, &caps, sizeof(caps));
    printf("mixer \"%s\" destinations=%lu\n", caps.szPname, caps.cDestinations);
    for (d = 0; d < caps.cDestinations; d++) {
        MIXERLINE l; MIXERLINECONTROLS lc; MIXERCONTROL c[16]; DWORD k;
        ZeroMemory(&l, sizeof(l)); l.cbStruct = sizeof(l); l.dwDestination = d;
        if (mixerGetLineInfo((HMIXEROBJ)mx, &l, MIXER_GETLINEINFOF_DESTINATION)) continue;
        printf(" destination %u \"%s\" type=%08lX channels=%lu sources=%lu controls=%lu\n", d,
               l.szName, l.dwComponentType, l.cChannels, l.cConnections, l.cControls);
        ZeroMemory(&lc, sizeof(lc)); lc.cbStruct = sizeof(lc); lc.dwLineID = l.dwLineID;
        lc.cControls = l.cControls > 16 ? 16 : l.cControls; lc.cbmxctrl = sizeof(c[0]); lc.pamxctrl = c;
        if (lc.cControls && !mixerGetLineControls((HMIXEROBJ)mx, &lc, MIXER_GETLINECONTROLSF_ALL))
            for (k = 0; k < lc.cControls; k++) {
                DWORD v[8] = { 0 };
                MMRESULT r = Values(mx, &l, &c[k], v, 0);
                printf("  control \"%s\" type=%08lX flags=%08lX min=%ld max=%ld steps=%lu value=%lu,%lu (%u)\n",
                       c[k].szName, c[k].dwControlType, c[k].fdwControl, c[k].Bounds.lMinimum,
                       c[k].Bounds.lMaximum, c[k].Metrics.cSteps, v[0], v[1], r);
            }
    }
    mixerClose(mx);
    printf("PASS: mixer listed\n");
    return 0;
}

static int SetLevel(int mute, DWORD left, DWORD right)
{
    HMIXER mx; MIXERLINE line; MIXERCONTROL c; DWORD v[8] = { 0 }, now[8] = { 0 };
    MMRESULT r;
    if (!Mixer(&mx, &line)) return 1;
    if (!Control(mx, &line, mute ? MIXERCONTROL_CONTROLTYPE_MUTE : MIXERCONTROL_CONTROLTYPE_VOLUME, &c)) {
        printf("FAIL: no %s control on the speaker line\n", mute ? "mute" : "volume");
        mixerClose(mx); return 1;
    }
    Values(mx, &line, &c, now, 0);
    v[0] = left; v[1] = right;
    r = Values(mx, &line, &c, v, 1);
    ZeroMemory(v, sizeof(v));
    Values(mx, &line, &c, v, 0);
    printf("%s \"%s\": was %lu,%lu set %lu,%lu (%u) now %lu,%lu\n", mute ? "mute" : "volume", c.szName,
           now[0], now[1], left, right, r, v[0], v[1]);
    mixerClose(mx);
    printf(r ? "FAIL: set\n" : "PASS: set\n");
    return r ? 1 : 0;
}

/* Play while the speaker line's volume and mute step (see the header). */
static int Ramp(unsigned secs)
{
    static const DWORD level[6] = { 65535, 32768, 16384, 4096, 65535, 65535 };
    static const DWORD muted[6] = { 0, 0, 0, 0, 1, 0 };
    HMIXER mx; MIXERLINE line; MIXERCONTROL vol, mute;
    WFX w; HWAVEOUT h; WAVEHDR hdr[BLOCKS]; HANDLE ev; MMRESULT r;
    DWORD oldVol[8] = { 0 }, oldMute[8] = { 0 }, v[8], start, total = 6 * secs;
    UINT dev = List(0, 0), i, done = 0, frames, f, step = 0;
    unsigned char *buf[BLOCKS];
    double phase = 0, rad = 2 * 3.14159265358979 * 440.0 / 48000;
    int haveVol, haveMute;
    if (dev == (UINT)-1 || !Mixer(&mx, &line)) { printf("FAIL: no UAC2 output or mixer\n"); return 1; }
    haveVol = Control(mx, &line, MIXERCONTROL_CONTROLTYPE_VOLUME, &vol);
    haveMute = Control(mx, &line, MIXERCONTROL_CONTROLTYPE_MUTE, &mute);
    if (haveVol) Values(mx, &line, &vol, oldVol, 0);
    if (haveMute) Values(mx, &line, &mute, oldMute, 0);
    printf("volume control=%d (was %lu,%lu) mute control=%d (was %lu)\n", haveVol, oldVol[0], oldVol[1],
           haveMute, oldMute[0]);
    Format(&w, 2, 16, 48000);
    frames = 48000 * total / BLOCKS;
    ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    r = waveOutOpen(&h, dev, (WAVEFORMATEX *)&w, (DWORD)ev, 0, CALLBACK_EVENT);
    if (r) { printf("FAIL: open %u\n", r); mixerClose(mx); return 1; }
    for (i = 0; i < BLOCKS; i++) {
        buf[i] = (unsigned char *)malloc(frames * w.Format.nBlockAlign);
        /* Half scale: the simulator reads -6 dBFS at full volume. */
        for (f = 0; f < frames; f++, phase += rad) {
            short x = (short)(sin(phase) * 16384.0);
            buf[i][4 * f] = buf[i][4 * f + 2] = (unsigned char)x;
            buf[i][4 * f + 1] = buf[i][4 * f + 3] = (unsigned char)(x >> 8);
        }
        ZeroMemory(&hdr[i], sizeof(hdr[i]));
        hdr[i].lpData = (LPSTR)buf[i]; hdr[i].dwBufferLength = frames * w.Format.nBlockAlign;
        waveOutPrepareHeader(h, &hdr[i], sizeof(hdr[i]));
    }
    start = GetTickCount();
    for (i = 0; i < BLOCKS; i++) waveOutWrite(h, &hdr[i], sizeof(hdr[i]));
    while (done < BLOCKS && GetTickCount() - start < total * 1000 + 10000) {
        DWORD now = GetTickCount() - start;
        while (step < 6 && now >= step * secs * 1000) {
            MMRESULT a = 0, b = 0;
            v[0] = v[1] = level[step];
            if (haveVol) a = Values(mx, &line, &vol, v, 1);
            v[0] = v[1] = muted[step];
            if (haveMute) b = Values(mx, &line, &mute, v, 1);
            printf("step %u at %lu ms: volume %lu (%u) mute %lu (%u)\n", step, now, level[step], a,
                   muted[step], b);
            step++;
        }
        WaitForSingleObject(ev, 50);
        for (done = 0, i = 0; i < BLOCKS; i++) if (hdr[i].dwFlags & WHDR_DONE) done++;
    }
    printf("completed=%u/%u elapsed_ms=%lu\n", done, BLOCKS, GetTickCount() - start);
    waveOutReset(h);
    for (i = 0; i < BLOCKS; i++) { waveOutUnprepareHeader(h, &hdr[i], sizeof(hdr[i])); free(buf[i]); }
    waveOutClose(h);
    if (haveVol) Values(mx, &line, &vol, oldVol, 1);
    if (haveMute) Values(mx, &line, &mute, oldMute, 1);
    mixerClose(mx);
    printf(done == BLOCKS && haveVol && haveMute ? "PASS: ramp played with volume and mute\n"
                                                 : "FAIL: buffers or controls missing\n");
    return done == BLOCKS && haveVol && haveMute ? 0 : 1;
}

static int Rec(unsigned ch, unsigned bits, unsigned rate, unsigned secs)
{
    WFX w; HWAVEIN h; WAVEHDR hdr[BLOCKS]; HANDLE ev; MMRESULT r;
    UINT dev = List(0, 1), i, done = 0, frames, f;
    unsigned char *buf[BLOCKS];
    DWORD start, bytes = 0, nonzero = 0, crossings = 0;
    long last = 0;
    if (dev == (UINT)-1) { printf("FAIL: no UAC2 input\n"); return 1; }
    Format(&w, ch, bits, rate);
    frames = rate * secs / BLOCKS;
    ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    r = waveInOpen(&h, dev, (WAVEFORMATEX *)&w, (DWORD)ev, 0, CALLBACK_EVENT);
    printf("rec dev=%u %uch %ubit %uHz %us: open=%u\n", dev, ch, bits, rate, secs, r);
    if (r) { printf("FAIL: open\n"); return 1; }
    for (i = 0; i < BLOCKS; i++) {
        buf[i] = (unsigned char *)calloc(frames, w.Format.nBlockAlign);
        ZeroMemory(&hdr[i], sizeof(hdr[i]));
        hdr[i].lpData = (LPSTR)buf[i]; hdr[i].dwBufferLength = frames * w.Format.nBlockAlign;
        waveInPrepareHeader(h, &hdr[i], sizeof(hdr[i]));
        waveInAddBuffer(h, &hdr[i], sizeof(hdr[i]));
    }
    start = GetTickCount();
    printf("start=%u\n", waveInStart(h));
    while (done < BLOCKS && GetTickCount() - start < secs * 1000 + 10000) {
        WaitForSingleObject(ev, 200);
        for (done = 0, i = 0; i < BLOCKS; i++) if (hdr[i].dwFlags & WHDR_DONE) done++;
    }
    printf("completed=%u/%u elapsed_ms=%lu\n", done, BLOCKS, GetTickCount() - start);
    printf("reset=%u\n", waveInReset(h));
    for (i = 0; i < BLOCKS; i++) {
        bytes += hdr[i].dwBytesRecorded;
        for (f = 0; f + w.Format.nBlockAlign <= hdr[i].dwBytesRecorded; f += w.Format.nBlockAlign) {
            long s = Get(buf[i] + f, bits);
            if (s) nonzero++;
            if (last < 0 && s >= 0) crossings++;
            last = s;
        }
        waveInUnprepareHeader(h, &hdr[i], sizeof(hdr[i])); free(buf[i]);
    }
    printf("close=%u\n", waveInClose(h));
    printf("bytes=%lu frames=%lu nonzero=%lu crossings=%lu tone_hz=%lu\n", bytes,
           bytes / w.Format.nBlockAlign, nonzero, crossings,
           bytes ? (DWORD)((double)crossings * rate / (bytes / w.Format.nBlockAlign)) : 0);
    printf(done == BLOCKS && nonzero ? "PASS: all buffers recorded, signal present\n" :
                                       "FAIL: buffers not completed or silent\n");
    return done == BLOCKS && nonzero ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !lstrcmpi(argv[1], "list")) { List(1, 0); return 0; }
    if (argc == 6 && (!lstrcmpi(argv[1], "play") || !lstrcmpi(argv[1], "rec"))) {
        unsigned ch = atoi(argv[2]), bits = atoi(argv[3]), rate = atoi(argv[4]), secs = atoi(argv[5]);
        if (!ch || ch > 32 || (bits != 16 && bits != 24 && bits != 32) || rate < 8000 || !secs || secs > 60) {
            printf("FAIL: arguments\n"); return 2;
        }
        return !lstrcmpi(argv[1], "play") ? Play(ch, bits, rate, secs) : Rec(ch, bits, rate, secs);
    }
    if (argc == 2 && !lstrcmpi(argv[1], "mix")) return Mix();
    if ((argc == 3 || argc == 4) && !lstrcmpi(argv[1], "vol")) {
        DWORD left = (DWORD)atol(argv[2]), right = argc == 4 ? (DWORD)atol(argv[3]) : left;
        return SetLevel(0, left > 65535 ? 65535 : left, right > 65535 ? 65535 : right);
    }
    if (argc == 3 && !lstrcmpi(argv[1], "mute")) return SetLevel(1, atoi(argv[2]) ? 1 : 0, atoi(argv[2]) ? 1 : 0);
    if (argc == 3 && !lstrcmpi(argv[1], "ramp")) {
        unsigned secs = atoi(argv[2]);
        if (!secs || secs > 10) { printf("FAIL: arguments\n"); return 2; }
        return Ramp(secs);
    }
    printf("usage: UACWAV list | play|rec <ch> <bits> <rate> <seconds> | mix | vol <v> [right] |\n"
           "       mute <0|1> | ramp <seconds>\n");
    return 2;
}

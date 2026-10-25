/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original WinMM test for uac2 beyond one format (test only):
 *   UACWAV list                          wave devices and their caps
 *   UACWAV play <ch> <bits> <rate> <s>   a quiet 440 Hz sine on the first
 *                                        "UAC2" output, queued buffers
 *   UACWAV rec <ch> <bits> <rate> <s>    record from the first "UAC2"
 *                                        input; non-silent frames and the
 *                                        tone's frequency from zero crossings
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
    printf("usage: UACWAV list | play|rec <ch> <bits> <rate> <seconds>\n");
    return 2;
}

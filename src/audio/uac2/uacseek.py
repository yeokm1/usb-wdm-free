#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Reads the PCM uacemu.py received (--pcm) while UACSEEK.EXE played (test only).

    uacseek.py <capture.pcm> <subslot 2|3|4> [--rate 44100] [--runs] [--max N] [--coarse]

UACSEEK's signal counts source frames: the left channel's high byte is
the frame number's low byte, the right channel's the next byte, every
sample's low bits 0x40. This reads every received stereo frame back to
that 16-bit count (from each sample's top 16 bits, within +-127 of the
code) and splits the stream into runs of consecutive counts. A clean
playback with S seeks is S + 1 runs, with silence (all-zero or near-zero
frames) between them at most. Anything else is listed: a short run, a
frame that is no code at all (bytes out of frame), and the jumps between
runs (a seek's jump can read as backward: the count is 16 bits). A stale
block is a run whose neighbours join up as if it were not there (bytes
from elsewhere in the ring, section 17 of docs/design/uac2.md); a leg's
pass is none.
"""
import argparse
import sys


def frames(data, subslot):
    step = 2 * subslot
    for i in range(0, len(data) - step + 1, step):
        yield data[i:i + step]


def top16(b, subslot):
    v = int.from_bytes(b[subslot - 2:subslot], 'little', signed=True)
    return v


def decode(frame, subslot):
    if not any(frame):
        return 'silence'
    out, quiet = [], True
    for c in range(2):
        if abs(top16(frame[c * subslot:(c + 1) * subslot], subslot)) >= 0x100:
            quiet = False
    for c in range(2):
        v = top16(frame[c * subslot:(c + 1) * subslot], subslot) - 0x40
        byte = (v + 128) >> 8
        if abs(v - byte * 256) > 127 or not -128 <= byte <= 127:
            return None
        out.append(byte + 128)
    code = out[0] | out[1] << 8
    return ('quiet', code) if quiet else code


def coarse(data, subslot, rate):
    """A resampled signal (KMixer converting the client's rate) keeps only
    the right channel's staircase: one step of the count's high byte every
    256 source frames. From the first 100 ms of a clean staircase on, a
    jump of three steps or more either way is listed; a backward jump that
    a forward one undoes 0.5 to 100 ms later is a stale block (old ring
    data); a resampler's ringing at a cut flips sample by sample instead."""
    steps = []
    for f in frames(data, subslot):
        r = top16(f[subslot:2 * subslot], subslot)
        if abs(r) < 0x100 and abs(top16(f[:subslot], subslot)) < 0x100:
            steps.append(None)
        else:
            steps.append((int(round((r - 0x40) / 256.0)) + 128) & 0xff)
    jumps, prev = [], None
    for i, step in enumerate(steps):
        if step is None:
            prev = None
            continue
        if prev is not None:
            d = (step - prev) & 0xff
            if d > 0x80:
                d -= 0x100
            if abs(d) >= 3:
                jumps.append((i, d))
        prev = step
    # UACSEEK's source starts at frame 1000, step 3: the signal starts at
    # the first 1 ms of a clean staircase within half a second of that (an
    # earlier sound is Windows' own); failing that, at the first clean
    # 100 ms, and the leg's start is not covered.
    jump_at = set(i for i, _ in jumps)
    top = 3 + rate // 2 // 256
    start = covered = None
    clean = 0
    long_start = None
    for i, step in enumerate(steps):
        clean = 0 if step is None or i in jump_at else clean + 1
        if clean >= rate // 10:
            long_start = i - clean + 1
            break
    if long_start is None:
        print('no clean staircase: not a UACSEEK signal')
        return 1
    clean = 0
    for i, step in enumerate(steps[:long_start + rate // 10]):
        clean = 0 if step is None or i in jump_at else clean + 1
        if clean >= rate // 1000 and 3 <= steps[i - clean + 1] <= top:
            start, covered = i - clean + 1, True
            break
    if start is None:
        start = long_start
        print('START NOT FOUND: the source start (step 3) is not in the capture; coverage incomplete')
    elif steps[start] > 4:
        print('START LATE: the capture begins at step %d; about %.0f ms are not covered' %
              (steps[start], (steps[start] - 3) * 256 * 1000.0 / rate))
    end = max(i for i, step in enumerate(steps) if step is not None) + 1
    jumps = [(i, d) for i, d in jumps if start <= i < end]
    stale = 0
    for k, (i, d) in enumerate(jumps):
        if d < 0 and any(i + 24 <= j <= i + rate // 10 and e > 0 and abs(e + d) <= 2 for j, e in jumps[k + 1:k + 8]):
            stale += 1
    silent = sum(1 for step in steps[start:end] if step is None)
    print('skipped %d frames before the signal' % start)
    print('frames=%d (%.3f s) silent=%d step_jumps=%d backward=%d stale_blocks=%d' %
          (end - start, (end - start) / float(rate), silent, len(jumps),
           sum(1 for j in jumps if j[1] < 0), stale))
    for i, d in jumps[:200]:
        print('  at %9d (%8.3f s) step %+d' % (i - start, (i - start) / float(rate), d))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pcm')
    ap.add_argument('subslot', type=int, choices=(2, 3, 4))
    ap.add_argument('--rate', type=int, default=44100)
    ap.add_argument('--runs', action='store_true', help='list every run')
    ap.add_argument('--max', type=int, default=200, help='lines of runs listed at most')
    ap.add_argument('--coarse', action='store_true', help='a resampled stream: the right channel steps only')
    a = ap.parse_args()
    data = open(a.pcm, 'rb').read()
    if a.coarse:
        return coarse(data, a.subslot, a.rate)
    runs = []          # [kind, first frame index, length, first code, last code]
    garbage = silence = 0
    prev = None
    for i, f in enumerate(frames(data, a.subslot)):
        d = decode(f, a.subslot)
        if isinstance(d, tuple):
            # a near-zero frame is code 0x8080 only where the run expects it
            expected = prev is not None and runs and runs[-1][0] == 'code' and d[1] == (prev + 1) & 0xffff
            d = d[1] if expected else 'silence'
        if d is None:
            garbage += 1
            kind = 'garbage'
        elif d == 'silence':
            silence += 1
            kind = 'silence'
        else:
            kind = 'code'
        if runs and runs[-1][0] == kind and (kind != 'code' or d == (prev + 1) & 0xffff):
            runs[-1][2] += 1
            if kind == 'code':
                runs[-1][4] = d
        else:
            runs.append([kind, i, 1, d if kind == 'code' else None, d if kind == 'code' else None])
        prev = d if kind == 'code' else prev
    # Other sounds (Windows' own) come before the test. UACSEEK's source
    # starts at frame 1000: the signal starts at the first code run of 1 ms
    # beginning within half a second of it; failing that, at the first run
    # of 100 ms, and the leg's start is not covered. It ends at the last
    # code frame.
    long_run = next((k for k, r in enumerate(runs) if r[0] == 'code' and r[2] >= a.rate // 10), None)
    if long_run is None:
        print('no code run of 100 ms: not a UACSEEK signal')
        return 1
    first = next((k for k, r in enumerate(runs[:long_run + 1]) if r[0] == 'code' and
                  r[2] >= a.rate // 1000 and 1000 <= r[3] <= 1000 + a.rate // 2), None)
    if first is None:
        first = long_run
        print('START NOT FOUND: source frame 1000 is not in the capture; coverage incomplete')
    elif runs[first][3] - 1000 > a.rate // 100:
        print('START LATE: the capture begins at source frame %d; the first %.1f ms are not covered' %
              (runs[first][3], (runs[first][3] - 1000) * 1000.0 / a.rate))
    last = max(k for k, r in enumerate(runs) if r[0] == 'code')
    skipped = runs[first][1]
    runs = runs[first:last + 1]
    silence = sum(r[2] for r in runs if r[0] == 'silence')
    garbage = sum(r[2] for r in runs if r[0] == 'garbage')
    total = runs[-1][1] + runs[-1][2] - skipped
    print('skipped %d frames before the signal' % skipped)
    codes = [r for r in runs if r[0] == 'code']
    print('frames=%d (%.3f s) code_runs=%d silence_frames=%d garbage_frames=%d' %
          (total, total / a.rate, len(codes), silence, garbage))
    last_end = None
    short = back = 0
    for r in runs:
        kind, at, n, first, last = r
        note = ''
        if kind == 'code':
            if last_end is not None:
                jump = (first - last_end - 1) & 0xffff
                if jump > 0x8000:
                    jump -= 0x10000
                note = 'jump %+d' % jump
                if jump < 0:
                    back += 1
            if n < a.rate // 10:
                short += 1
            last_end = last
        if (a.runs or kind == 'garbage' or (kind == 'code' and n < a.rate // 10)) and a.max > 0:
            a.max -= 1
            at -= skipped
            print('  %-7s at %9d (%8.3f s) len %7d (%7.1f ms) %s%s' %
                  (kind, at, at / a.rate, n, n * 1000.0 / a.rate,
                   ('codes %5d..%5d ' % (first, last)) if kind == 'code' else '', note))
    # A stale block: a code run between two code runs (no silence between)
    # whose neighbours join up as if it were not there: the run after it
    # starts where the run before it ended, or up to the block's length
    # later (within 1 ms), while the block itself does not continue the run
    # before. A seek is one jump that the following run continues.
    stale = 0
    for k in range(1, len(runs) - 1):
        p, b, n = runs[k - 1], runs[k], runs[k + 1]
        if p[0] != 'code' or b[0] != 'code' or n[0] != 'code':
            continue
        gap = (n[3] - p[4] - 1) & 0xffff
        if gap > 0x8000:
            gap -= 0x10000
        own = (b[3] - p[4] - 1) & 0xffff
        tol = a.rate // 1000
        if -tol <= gap <= b[2] + tol and own >= 0x8000:
            stale += 1
    print('short_runs(<100ms)=%d backward_jumps=%d stale_blocks=%d' % (short, back, stale))
    return 0


if __name__ == '__main__':
    sys.exit(main())

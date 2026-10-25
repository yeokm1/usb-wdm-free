#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""External clock reference for the guest's CLKSER.EXE (COM1 lines).

  python -B clkser.py recv PORT OUT.jsonl [SECONDS]
      Connect to QEMU's `-serial tcp:127.0.0.1:PORT,server=on,wait=off`
      socket, stamp every complete line on arrival with the host
      QueryPerformanceCounter (same ns formula as QEMU's Windows get_clock),
      write one JSON record per line, stop after END or SECONDS.
      Reads a socket and writes a file only; touches no device.
  python -B clkser.py fit OUT.jsonl RESULT.json
      Least-squares slopes of guest QPC and guest GetTickCount against host
      arrival time, plus per-window tick rates to locate where tick time is
      lost. Arrival stamps include serial/emulation latency; the fit uses
      many samples and reports residuals so that latency jitter is visible.
"""
import ctypes, json, socket, sys, time


def host_ns():
    k = ctypes.WinDLL('kernel32')
    q, f = ctypes.c_longlong(), ctypes.c_longlong()
    k.QueryPerformanceFrequency(ctypes.byref(f))
    k.QueryPerformanceCounter(ctypes.byref(q))
    return q.value * 1000000000 // f.value


def recv(port, out, seconds):
    deadline = time.monotonic() + seconds
    while True:
        try:
            s = socket.create_connection(('127.0.0.1', port), 2)
            break
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.2)
    s.settimeout(0.5)
    buf, done = b'', False
    with open(out, 'w') as f:
        while not done and time.monotonic() < deadline:
            try:
                data = s.recv(4096)
            except socket.timeout:
                continue
            if not data:
                break
            stamp = host_ns()
            buf += data
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                text = line.decode('ascii', 'replace').strip()
                f.write(json.dumps({'host_ns': stamp, 'line': text}) + '\n')
                f.flush()
                if text.startswith('END'):
                    done = True
    s.close()
    return 0 if done else 1


def slope(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    a = my - b * mx
    res = [y - (a + b * x) for x, y in zip(xs, ys)]
    return b, max(abs(r) for r in res)


def fit(path, result):
    recs = [json.loads(l) for l in open(path)]
    begin = next(r for r in recs if r['line'].startswith('BEGIN'))
    freq = float(begin['line'].split()[2])
    samples = []
    for r in recs:
        p = r['line'].split()
        if len(p) == 6 and p[0] == 'S' and p[2] == 'qpc' and p[4] == 'tick':
            samples.append((r['host_ns'] / 1e6, float(p[3]) * 1000 / freq, int(p[5])))
    if len(samples) < 10:
        raise ValueError('too few samples')
    h0, q0, t0 = samples[0]
    hs = [h - h0 for h, _, _ in samples]
    qs = [q - q0 for _, q, _ in samples]
    ts = [t - t0 for _, _, t in samples]
    qslope, qres = slope(hs, qs)
    tslope, tres = slope(hs, ts)
    windows = []
    step = 1000.0
    i = 0
    while i < len(samples):
        j = i
        while j + 1 < len(samples) and hs[j + 1] - hs[i] < step:
            j += 1
        if j > i:
            dh = hs[j] - hs[i]
            windows.append({'host_start_ms': round(hs[i], 1), 'host_ms': round(dh, 1),
                            'qpc_rate': round((qs[j] - qs[i]) / dh, 4),
                            'tick_rate': round((ts[j] - ts[i]) / dh, 4)})
        i = j + 1
    out = {'samples': len(samples), 'host_span_ms': round(hs[-1], 1),
           'guest_qpc_per_host': round(qslope, 6), 'guest_qpc_max_residual_ms': round(qres, 2),
           'guest_tick_per_host': round(tslope, 6), 'guest_tick_max_residual_ms': round(tres, 2),
           'windows': windows,
           'limits': ['Host stamps are arrival times after serial emulation and socket delivery.',
                      'A slope is a ratio over this run only; it is not a device or driver verdict.']}
    open(result, 'w').write(json.dumps(out, indent=2) + '\n')
    print(json.dumps({k: v for k, v in out.items() if k != 'windows'}, indent=2))
    return 0


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'recv':
        sys.exit(recv(int(sys.argv[2]), sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else 300))
    if len(sys.argv) == 4 and sys.argv[1] == 'fit':
        sys.exit(fit(sys.argv[2], sys.argv[3]))
    print(__doc__)
    sys.exit(2)

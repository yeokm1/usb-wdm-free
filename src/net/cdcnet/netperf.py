#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host side of NETPERF.EXE: connects out to the guest (so an inbound
firewall on the host does not matter), runs SEND, RECV or both at once,
checks the pattern stream and prints the rate per interval and in total.

  netperf.py <guest> send|recv|both|cpu|last [--seconds N] [--port P]
             [--bind ADDR] [--pid QEMU_PID] [--interval S] [--log FILE]

--pid samples a process's CPU time (kernel + user, all threads) over the
run, as a share of one logical processor. No third-party module needed.
"""
import argparse
import ctypes
import socket
import sys
import threading
import time

PERIOD = 65521
BLOCK = 65536
TABLE = bytes(((k * 37 + 11 + (k >> 9)) & 255) for k in range(PERIOD))
TABLE2 = TABLE * ((BLOCK * 2) // PERIOD + 2)


def process_seconds(pid):
    """Kernel + user CPU seconds of pid (Windows), or None."""
    if not pid or sys.platform != 'win32':
        return None
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    try:
        t = [ctypes.c_ulonglong() for _ in range(4)]
        if not k32.GetProcessTimes(h, *[ctypes.byref(x) for x in t]):
            return None
        return (t[2].value + t[3].value) / 1e7
    finally:
        k32.CloseHandle(h)


class Meter:
    def __init__(self, name, interval, out):
        self.name, self.interval, self.out = name, interval, out
        self.bytes = 0
        self.mismatch = 0
        self.first_bad = None
        self.start = self.mark_t = time.monotonic()
        self.mark_b = 0
        self.samples = []

    def add(self, n):
        self.bytes += n
        now = time.monotonic()
        if now - self.mark_t >= self.interval:
            rate = (self.bytes - self.mark_b) * 8 / (now - self.mark_t) / 1e6
            self.samples.append(rate)
            self.out('%s %7.1f s %8.2f Mb/s' % (self.name, now - self.start, rate))
            self.mark_t, self.mark_b = now, self.bytes

    def total(self):
        el = time.monotonic() - self.start
        return el, (self.bytes * 8 / el / 1e6) if el > 0 else 0.0


def connect(a, line):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    if a.bind:
        s.bind((a.bind, 0))
    s.settimeout(30)
    s.connect((a.guest, a.port))
    s.sendall((line + '\n').encode())
    return s


def do_send(a, out, res):
    """Guest sends; the host receives and checks."""
    m = Meter('rx', a.interval, out)
    off = 0
    buf = bytearray(BLOCK)
    view = memoryview(buf)
    err = None
    s = None
    try:
        s = connect(a, 'SEND %d %d' % (a.seconds, a.block))
        while True:
            n = s.recv_into(view)
            if not n:
                break
            if view[:n] != TABLE2[off:off + n]:
                for i in range(n):
                    if buf[i] != TABLE[(off + i) % PERIOD]:
                        m.mismatch += 1
                        if m.first_bad is None:
                            m.first_bad = m.bytes + i
            off = (off + n) % PERIOD
            m.add(n)
    except OSError as e:
        err = repr(e)
    if s:
        s.close()
    el, rate = m.total()
    res['failed'] = res.get('failed', False) or err is not None
    res['send'] = 'guest->host bytes=%d s=%.1f Mb/s=%.2f mismatch=%d firstbad=%s err=%s' % (
        m.bytes, el, rate, m.mismatch, m.first_bad, err)


def do_recv(a, out, res):
    """The host sends; the guest checks and answers."""
    m = Meter('tx', a.interval, out)
    off = 0
    err = None
    s = None
    end = time.monotonic() + a.seconds
    try:
        s = connect(a, 'RECV %d' % a.seconds)
        while time.monotonic() < end:
            n = s.send(TABLE2[off:off + a.block])
            off = (off + n) % PERIOD
            m.add(n)
        s.shutdown(socket.SHUT_WR)
        s.settimeout(60)
        reply = b''
        while not reply.endswith(b'\n'):
            part = s.recv(512)
            if not part:
                break
            reply += part
    except OSError as e:
        err = repr(e)
        reply = b''
    if s:
        s.close()
    el, rate = m.total()
    res['failed'] = res.get('failed', False) or err is not None or b'mismatch=0 ' not in reply
    res['recv'] = 'host->guest bytes=%d s=%.1f Mb/s=%.2f err=%s guest: %s' % (
        m.bytes, el, rate, err, reply.decode(errors='replace').strip())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('guest')
    ap.add_argument('mode', choices=['send', 'recv', 'both', 'cpu', 'last'])
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--port', type=int, default=51130)
    ap.add_argument('--block', type=int, default=32768)
    ap.add_argument('--bind', default='')
    ap.add_argument('--pid', type=int, default=0)
    ap.add_argument('--interval', type=float, default=5.0)
    ap.add_argument('--log', default='')
    a = ap.parse_args()
    lock = threading.Lock()
    logf = open(a.log, 'a') if a.log else None

    def out(text):
        line = time.strftime('%H:%M:%S ') + text
        with lock:
            print(line, flush=True)
            if logf:
                logf.write(line + '\n')
                logf.flush()

    out('netperf %s %s %ds port %d block %d' % (a.guest, a.mode, a.seconds, a.port, a.block))
    if a.mode in ('cpu', 'last'):
        s = connect(a, 'CPU %d' % a.seconds if a.mode == 'cpu' else 'LAST')
        s.settimeout(a.seconds + 30)
        data = b''
        while True:
            part = s.recv(1024)
            if not part:
                break
            data += part
        out(data.decode(errors='replace').strip())
        return 0
    res = {}
    c0, t0 = process_seconds(a.pid), time.monotonic()
    jobs = []
    if a.mode in ('send', 'both'):
        jobs.append(threading.Thread(target=do_send, args=(a, out, res)))
    if a.mode in ('recv', 'both'):
        jobs.append(threading.Thread(target=do_recv, args=(a, out, res)))
    for j in jobs:
        j.start()
    for j in jobs:
        j.join()
    c1, t1 = process_seconds(a.pid), time.monotonic()
    for k in ('send', 'recv'):
        if k in res:
            out('RESULT ' + res[k])
    if c0 is not None and c1 is not None:
        out('QEMU pid %d cpu %.1f%% of one logical processor over %.1f s' % (
            a.pid, (c1 - c0) * 100 / (t1 - t0), t1 - t0))
    return 1 if res.get('failed') else 0


if __name__ == '__main__':
    sys.exit(main())

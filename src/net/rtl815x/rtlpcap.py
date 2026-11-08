#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Reads a usbmon capture (pcap, link type 220 or 189, as QEMU's usb-host
pcap= option writes) of an RTL815x chip and prints what a driver did:

  rtlpcap.py <file.pcap> summary            transfers and bytes per endpoint
  rtlpcap.py <file.pcap> rate [--from S] [--to S] [--step S]
                                            transfers per second per endpoint
  rtlpcap.py <file.pcap> regs [--from S] [--to S]
                                            every register access in order:
                                            MAC/USB space, PHY (through the
                                            0xe86c page and 0xb000 window),
                                            PHY SRAM (0xa436/0xa438 and
                                            0xb87c/0xb87e) and the indirect
                                            window (0xd5d6/0xd5d8/0xd5dc)
  rtlpcap.py <file.pcap> final [--to S]     the last value written to each
                                            register, and the last read
  rtlpcap.py <file.pcap> bulk [--from S] [--to S]
                                            bulk transfer sizes (histogram)

Register access is vendor request 0x05: wValue the dword address, wIndex
the space (0x0100 MAC, 0x0000 USB) with the byte-lane bits, the data a
dword (or more) little-endian. Times are seconds from the first record.
"""
import argparse
import collections
import struct
import sys

SPACE = {0x0100: 'MAC', 0x0000: 'USB'}
KIND = {0: 'iso', 1: 'int', 2: 'ctl', 3: 'bulk'}


def records(path):
    with open(path, 'rb') as f:
        head = f.read(24)
        if len(head) < 24:
            return
        magic = struct.unpack('<I', head[:4])[0]
        if magic == 0xa1b2c3d4:
            e = '<'
        elif magic == 0xd4c3b2a1:
            e = '>'
        else:
            raise SystemExit('not a pcap file')
        link = struct.unpack(e + 'I', head[20:24])[0]
        hl = 64 if link == 220 else 48
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            sec, usec, incl, orig = struct.unpack(e + 'IIII', rh)
            data = f.read(incl)
            if len(data) < incl:
                return
            if len(data) < hl:
                continue
            (rid, typ, xt, ep, dev, bus, fsetup, fdata, tsec, tusec, status, length,
             lencap) = struct.unpack('<QBBBBHbbqiiII', data[:40])
            setup = data[40:48]
            payload = data[hl:]
            yield {'t': sec + usec / 1e6, 'id': rid, 'type': chr(typ), 'xt': xt, 'ep': ep, 'dev': dev,
                   'setup': setup if fsetup == 0 else None, 'status': status, 'length': length,
                   'data': payload}


def load(path):
    recs = list(records(path))
    if not recs:
        raise SystemExit('no records')
    t0 = recs[0]['t']
    for r in recs:
        r['t'] -= t0
    return recs


def in_range(r, a):
    return (a.frm is None or r['t'] >= a.frm) and (a.to is None or r['t'] <= a.to)


def epname(r):
    return '%s ep%d %s' % (KIND.get(r['xt'], '?'), r['ep'] & 0x7f, 'in' if r['ep'] & 0x80 else 'out')


def summary(recs, a):
    sub = collections.Counter()
    done = collections.Counter()
    by = collections.Counter()
    errs = collections.Counter()
    for r in recs:
        if not in_range(r, a):
            continue
        k = epname(r)
        if r['type'] == 'S':
            sub[k] += 1
        else:
            done[k] += 1
            if r['status'] not in (0,):
                errs[(k, r['status'])] += 1
            if r['ep'] & 0x80:
                by[k] += r['length']
        if r['type'] == 'S' and not r['ep'] & 0x80:
            by[k] += r['length']
    span = recs[-1]['t'] - recs[0]['t']
    print('span %.3f s, %d records' % (span, len(recs)))
    for k in sorted(set(sub) | set(done)):
        print('%-16s submitted %8d completed %8d bytes %12d' % (k, sub[k], done[k], by[k]))
    for (k, st), n in sorted(errs.items()):
        print('%-16s status %d: %d' % (k, st, n))


def rate(recs, a):
    step = a.step
    buckets = collections.defaultdict(collections.Counter)
    for r in recs:
        if r['type'] != 'C' or not in_range(r, a):
            continue
        buckets[int(r['t'] // step)][epname(r)] += 1
    keys = sorted({k for b in buckets.values() for k in b})
    print('%8s ' % 't' + ' '.join('%16s' % k for k in keys))
    for i in sorted(buckets):
        print('%8.1f ' % (i * step) + ' '.join('%16.1f' % (buckets[i][k] / step) for k in keys))


class Regs:
    """Follows the chip's register accesses, PHY page and windows."""

    def __init__(self, out):
        self.out = out
        self.page = None
        self.sram = {}
        self.sram_addr = {0xa436: None, 0xb87c: None}
        self.adv = [0, 0]
        self.written = {}
        self.read = {}
        self.phyw = {}
        self.phyr = {}

    def access(self, t, write, value, index, data, status):
        space = SPACE.get(index & 0xff00, 'S%x' % (index & 0xff00))
        lanes = index & 0xff
        base = value
        for off in range(0, len(data), 4):
            word = data[off:off + 4].ljust(4, b'\0')
            # the first dword's lanes are wIndex's; the rest of a longer write are whole dwords
            ln = lanes if off == 0 else 0xff
            self.dword(t, write, space, base + off, word, ln, status)

    def dword(self, t, write, space, addr, b, lanes, status):
        if status:
            self.out('%9.4f %s %s %04x failed, status=%d (no state change)' % (
                t, 'W' if write else 'R', space, addr, status))
            return
        enabled = [(lanes >> i) & 1 for i in range(4)] if write else [1, 1, 1, 1]
        if write and lanes in (0x11, 0x22, 0x44, 0x88):
            enabled = [1 if lanes == (0x11 << i) else 0 for i in range(4)]
        elif write and lanes in (0x33, 0xcc):
            enabled = [1, 1, 0, 0] if lanes == 0x33 else [0, 0, 1, 1]
        elif write and lanes == 0xff:
            enabled = [1, 1, 1, 1]
        key = (space, addr)
        store = self.written if write else self.read
        cur = list(store.get(key, [None] * 4))
        for i in range(4):
            if enabled[i]:
                cur[i] = b[i]
        store[key] = cur
        v = struct.unpack('<I', b)[0]
        tag = 'W' if write else 'R'
        what = ''
        # 16-bit views for the PHY window and the page register
        words = []
        if enabled[0] or enabled[1]:
            words.append((addr, v & 0xffff))
        if enabled[2] or enabled[3]:
            words.append((addr + 2, v >> 16))
        if space == 'MAC':
            for wa, wv in words:
                if wa == 0xe86c and write:
                    self.page = wv
                    what += ' page=%04x' % wv
                elif 0xb000 <= wa < 0xc000:
                    reg = ((self.page or 0) & 0xf000) | (wa & 0x0fff)
                    what += ' PHY[%04x]%s%04x' % (reg, '=' if write else '->', wv)
                    self.phy(reg, wv, write, t)
        if space == 'USB' and addr in (0xd5d4, 0xd5d8, 0xd5dc):
            if addr == 0xd5d4 and (enabled[2] or enabled[3]):
                self.adv[0] = v >> 16
            if addr == 0xd5d8:
                self.adv[1] = v
            if addr == 0xd5dc and write and (enabled[0] or enabled[1]) and (v & 1):
                what += ' ADV %s %s[%04x]%s' % ('IP' if v & 4 else 'BMU', 'W' if v & 2 else 'R',
                                               self.adv[0], ('=%08x' % self.adv[1]) if v & 2 else '')
        lane_txt = ''.join('x' if e else '.' for e in enabled)
        if status:
            what += ' status=%d' % status
        self.out('%9.4f %s %s %04x %s %08x%s' % (t, tag, space, addr, lane_txt, v, what))

    def phy(self, reg, value, write, t):
        (self.phyw if write else self.phyr)[reg] = value
        if reg in (0xa436, 0xb87c) and write:
            self.sram_addr[reg] = value
        elif reg in (0xa438, 0xb87e):
            win = 0xa436 if reg == 0xa438 else 0xb87c
            at = self.sram_addr.get(win)
            if at is not None:
                if write:
                    self.sram[(win, at)] = value
                self.out('%9.4f   SRAM%s[%04x]%s%04x' % (t, '' if win == 0xa436 else '2', at,
                                                       '=' if write else '->', value))


def regs(recs, a, final=False):
    pending = {}
    lines = []
    quiet = [False]

    def emit(text):
        if final or quiet[0]:
            lines.append(text)
        else:
            print(text)
    st = Regs(emit)
    for r in recs:
        if r['xt'] != 2:
            continue
        if r['type'] == 'S' and r['setup']:
            bm, req, val, idx, ln = struct.unpack('<BBHHH', r['setup'])
            pending[r['id']] = (bm, req, val, idx, ln, r['data'], r['t'])
            continue
        if r['type'] != 'C' or r['id'] not in pending:
            continue
        bm, req, val, idx, ln, wdata, ts = pending.pop(r['id'])
        if a.to is not None and ts > a.to:
            break
        # accesses before --from still move the PHY page and windows
        quiet[0] = a.frm is not None and ts < a.frm
        if bm & 0x60 != 0x40 or req != 0x05:
            if not final and not quiet[0]:
                print('%9.4f ctl bm=%02x req=%02x val=%04x idx=%04x len=%d status=%d' % (
                    ts, bm, req, val, idx, ln, r['status']))
            continue
        if bm & 0x80:
            st.access(ts, False, val, idx, r['data'][:ln], r['status'])
        else:
            st.access(ts, True, val, idx, wdata[:ln], r['status'])
    if final:
        print('# last written (bytes not written shown as --)')
        for (space, addr) in sorted(st.written):
            b = st.written[(space, addr)]
            print('W %s %04x %s' % (space, addr, ' '.join('--' if x is None else '%02x' % x for x in b)))
        print('# last read')
        for (space, addr) in sorted(st.read):
            b = st.read[(space, addr)]
            print('R %s %04x %s' % (space, addr, ' '.join('%02x' % x for x in b)))
        print('# PHY registers, last written / last read')
        for reg in sorted(set(st.phyw) | set(st.phyr)):
            w = st.phyw.get(reg); r = st.phyr.get(reg)
            print('PHY %04x W %s R %s' % (reg, '----' if w is None else '%04x' % w, '----' if r is None else '%04x' % r))
        print('# PHY SRAM last written')
        for (win, at) in sorted(st.sram):
            print('SRAM%s %04x %04x' % ('' if win == 0xa436 else '2', at, st.sram[(win, at)]))


def bulk(recs, a):
    h = collections.defaultdict(collections.Counter)
    for r in recs:
        if r['xt'] != 3 or not in_range(r, a):
            continue
        if r['type'] == 'C' and r['ep'] & 0x80:
            h['in done'][r['length']] += 1
        if r['type'] == 'S' and r['ep'] & 0x80:
            h['in asked'][r['length']] += 1
        if r['type'] == 'S' and not r['ep'] & 0x80:
            h['out'][r['length']] += 1
    for k in h:
        tot = sum(h[k].values())
        byt = sum(n * c for n, c in h[k].items())
        print('%s: %d transfers, %d bytes, mean %.0f' % (k, tot, byt, byt / tot if tot else 0))
        for n, c in sorted(h[k].items(), key=lambda x: -x[1])[:12]:
            print('   %6d bytes x %d' % (n, c))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pcap')
    ap.add_argument('what', choices=['summary', 'rate', 'regs', 'final', 'bulk'])
    ap.add_argument('--from', dest='frm', type=float)
    ap.add_argument('--to', type=float)
    ap.add_argument('--step', type=float, default=1.0)
    a = ap.parse_args()
    recs = load(a.pcap)
    if a.what == 'summary':
        summary(recs, a)
    elif a.what == 'rate':
        rate(recs, a)
    elif a.what in ('regs', 'final'):
        regs(recs, a, a.what == 'final')
    else:
        bulk(recs, a)


if __name__ == '__main__':
    sys.exit(main())

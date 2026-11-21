#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""levbase.py <uacemu log.jsonl> <profile> [<harness log>] - the Feature Unit requests of a
uacemu log (test aid: docs/design/uac2.md 28.15's playback level baselines).

Prints one line per class request to a Feature Unit of the AudioControl
interface, in order, in the form testdata/levels/*.lvl and leveltest read:
  bmRequestType bRequest wValue wIndex wLength status actual data
(hex, hex, hex, hex, decimal, decimal, decimal, the data stage as hex or -).
The two reads uacusb.c's ReadFeature makes at a stream's start (GET CUR of
the first Feature Unit's master mute and volume, after the clock and
before SET_INTERFACE) are not the level engine's and are left out, as a
comment. With a harness log (lines 'HH:MM:SS.mmm text'), each harness line
is put in as a comment where it falls in time, so the events that caused
the requests can be written in by hand ('!' lines, see leveltest.c).
"""
import datetime
import json
import sys

import uacemu


def main():
    trace = [json.loads(line) for line in open(sys.argv[1], encoding='utf-8')]
    marks = []
    if len(sys.argv) > 3:
        day = datetime.datetime.fromtimestamp(trace[0]['time']).date() if trace else datetime.date.today()
        for line in open(sys.argv[3], encoding='utf-8'):
            line = line.rstrip('\n')
            try:
                t = datetime.datetime.combine(day, datetime.datetime.strptime(line[:12], '%H:%M:%S.%f').time())
            except ValueError:
                continue
            if trace and t.timestamp() >= trace[0]['time']:
                marks.append((t.timestamp(), line[13:]))
        marks.sort(key=lambda m: m[0])
    profile = uacemu.Profile(sys.argv[2], None, None)
    fus = set(i for i, e in profile.entities.items() if e['kind'] == 'fu')
    ac = profile.ac
    out = []
    for e in trace:
        while marks and marks[0][0] <= e['time']:
            out.append(('mark', '# %s' % marks.pop(0)[1]))
        if e['event'] == 'alternate' and not e.get('alternate') and e.get('interface') != profile.ac:
            out.append(('mark', '# SET_INTERFACE %d alt 0' % e['interface']))
            continue
        if e['event'] == 'alternate' and e.get('alternate'):
            # ReadFeature: FU GET CURs on channel 0 just before SET_INTERFACE.
            k = len(out)
            while k and out[k - 1][0] == 'fu' and out[k - 1][2]['request_type'] == 0xA1 and \
                    out[k - 1][2]['request'] == 1 and out[k - 1][2]['value'] & 255 == 0:
                k -= 1
            for j in range(k, len(out)):
                out[j] = ('mark', '# (stream start, uacusb ReadFeature) ' + out[j][1])
            out.append(('mark', '# SET_INTERFACE %d alt %d' % (e['interface'], e['alternate'])))
            continue
        if e['event'] != 'control' or e['request_type'] not in (0x21, 0xA1):
            continue
        entity, selector = e['index'] >> 8, e['value'] >> 8
        if entity not in fus or selector not in (1, 2) or e['index'] & 255 != ac:
            continue
        line = '%02X %02X %04X %04X %d %d %d %s' % (e['request_type'], e['request'], e['value'], e['index'],
                                                    e['requested'], e['status'], e['actual'],
                                                    e.get('data') or '-')
        out.append(('fu', line, e))
    for m in marks:
        if trace and m[0] <= trace[-1]['time'] + 30:
            out.append(('mark', '# %s' % m[1]))
    for item in out:
        print(item[1])


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host tests of absemu.py's report shapes (no guest, no USB device).
Run: python -B abssimtest.py"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import absemu as a  # noqa: E402


def pen(**kw):
    s = dict(x=0.5, y=0.5, tip=False, buttons=0, range=True, pressure=0.6, barrel=False, eraser=False)
    s.update(kw)
    return s


class Tests(unittest.TestCase):
    def test_penpartner_mouse_mode_matches_the_descriptor(self):
        """Before a mode switch: report 1, eight bytes with its ID, relative
        motion from the pen in steps of at most 127."""
        state = dict(mode=1)
        first = a.reports('penpartner', pen(), state)
        self.assertEqual(first, [b'\x01' + bytes(7)])
        moved = a.reports('penpartner', pen(x=0.9, tip=True), state)
        steps = [struct.unpack('<BBbbb3x', r) for r in moved]
        self.assertTrue(all(len(r) == 8 and r[0] == 1 for r in moved))
        self.assertEqual(sum(s[2] for s in steps), round(0.4 * 5040) // a.PP_MOUSE_DIV)
        self.assertTrue(all(s[1] == 1 for s in steps))                  # the tip is the left button
        self.assertEqual(a.reports('penpartner', pen(x=0.9, tip=True), state), [b'\x01\x01' + bytes(6)])
        peek = a.reports('penpartner', pen(x=0.1), dict(state), advance=False)
        self.assertEqual(len(peek), 1)                                  # GET_REPORT: no motion used up

    def test_penpartner_tablet_mode(self):
        r = a.reports('penpartner', pen(x=0.25, y=0.75, tip=True, pressure=0.5), dict(mode=2))[0]
        self.assertEqual(len(r), 8)                                     # ID and seven data bytes
        rid, x, y, status, press = struct.unpack('<BHHBb', r[:7])
        self.assertEqual((rid, x, y, status, press), (2, 1260, 2835, 0, 0))
        r = a.reports('penpartner', pen(eraser=True, barrel=True), dict(mode=2))[0]
        self.assertEqual((r[5], struct.unpack('<b', r[6:7])[0]), (0x60, -127))


if __name__ == '__main__':
    unittest.main()

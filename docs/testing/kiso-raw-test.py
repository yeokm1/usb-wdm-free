#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Host-only diagnostic evidence regression, no USB or Windows guest."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "kiso_raw", Path(__file__).with_name("kiso-raw.py"))
kiso = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kiso)
CLEAN = (b"KISO_RAW ep=01 packets=32 raw_count=32 raw_errors=0 "
         b"raw_urb=00000000 failed=0 first=0000 "
         b"first_index=0 last_index=0 mask32=00000000\n")
FAILED = (CLEAN.replace(b"raw_errors=0", b"raw_errors=24")
          .replace(b"failed=0", b"failed=24").replace(b"first=0000", b"first=c005")
          .replace(b"last_index=0", b"last_index=23")
          .replace(b"mask32=00000000", b"mask32=00ffffff"))


class Evidence(unittest.TestCase):
    def test_clean_record_is_only_parsed_evidence(self):
        r = kiso.summarize(CLEAN)
        self.assertFalse(r["invalid_records"])
        self.assertFalse(r["failures"])
        self.assertEqual(r["endpoints"]["01"]["packets"], 32)
        self.assertIn("not a playback PASS", r["limits"][0])

    def test_failed_callback_preserves_ordinal_and_folded_value(self):
        r = kiso.summarize(CLEAN + FAILED)
        self.assertFalse(r["invalid_records"])
        self.assertEqual(r["endpoints"]["01"]["raw_errors"], 24)
        self.assertEqual(r["failures"][0]["endpoint_callback_ordinal"], 2)
        self.assertEqual(r["failures"][0]["first_folded_status"], "c005")

    def test_missing_or_damaged_records_cannot_be_clean(self):
        cases = [b"", b"KISO_RAW ep=01\n",
                 CLEAN.replace(b"raw_count=32", b"raw_count=31"),
                 CLEAN.replace(b"raw_errors=0", b"raw_errors=1"),
                 CLEAN.replace(b"mask32=00000000", b"mask32=00000001"),
                 CLEAN.replace(b"first_index=0 ", b""),
                 FAILED.replace(b"last_index=23", b"last_index=24"),
                 FAILED.replace(b"00ffffff", b"80ffffff"),
                 FAILED.replace(b"first=c005", b"first=0000"),
                 CLEAN.replace(b"packets=32", b"packets=0"),
                 CLEAN.replace(b"ep=01", b"ep=100")]
        cases += [CLEAN.replace(b"raw_errors=0", b"raw_errors=0garbage"),
                  CLEAN.replace(b"raw_urb=00000000",
                                b"raw_urb=c0000001 raw_urb=00000000")]
        for data in cases:
            with self.subTest(data=data):
                self.assertTrue(kiso.summarize(data)["invalid_records"])

    def test_old_format_keeps_errors_without_invented_locations(self):
        r = kiso.summarize(FAILED.split(b" first_index=")[0])
        self.assertFalse(r["invalid_records"])
        self.assertEqual(r["failures"][0]["failed"], 24)
        self.assertNotIn("first_index", r["failures"][0])

    def test_multiple_endpoints_and_urb_failure_remain_separate(self):
        feedback = CLEAN.replace(b"ep=01", b"ep=83")
        urb = CLEAN.replace(b"raw_urb=00000000", b"raw_urb=c0000001")
        r = kiso.summarize(FAILED + feedback + urb)
        self.assertFalse(r["invalid_records"])
        self.assertEqual(r["endpoints"]["01"]["nonzero_urb_callbacks"], 1)
        self.assertEqual(r["endpoints"]["83"]["raw_errors"], 0)
        self.assertEqual(len(r["failures"]), 2)


if __name__ == "__main__":
    unittest.main()

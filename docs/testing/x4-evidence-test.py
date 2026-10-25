#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Test false-positive prevention in the bounded evidence gate; no USB."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "x4_evidence", Path(__file__).with_name("x4-evidence.py"))
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)


def fixture():
    raw = {"invalid_records": [], "failures": [], "endpoints": {
        "01": {"callbacks": 500, "packets": 16000},
        "83": {"callbacks": 65, "packets": 2080}}}
    host = {"host_endpoint_counters": {}, "ring_events": {},
            "host_first_submit_to_final_callback_ms": {"1": 2000, "3": 2080},
            "host_submissions_still_pending": 0, "unpaired_host_callbacks": 0}
    for ep, count, packets in (("1", 500, 16000), ("3", 65, 2080)):
        host["host_endpoint_counters"][ep] = {
            "callbacks": count, "submissions": count, "submitted_packets": packets,
            "submit_result:0": count, "callback_status:0": count,
            "failed_packets": 0, "submitted_requested_bytes": packets * (24 if ep == "1" else 4)}
    player = ("Submitted=8 completed=8 samples=96000 elapsed_ms=1500\n"
              "Performance elapsed_ms=2000 (required wall-clock check)\n"
              "Close=0\nPASS: WinMM real-buffer completion/position/close.\n")
    return raw, host, player


class Gate(unittest.TestCase):
    def test_matching_clean_fixture(self):
        self.assertEqual(evidence.check(*fixture()), [])

    def test_player_pass_does_not_hide_raw_failure(self):
        raw, host, player = fixture()
        raw["failures"] = [{"failed": 24}]
        self.assertTrue(evidence.check(raw, host, player))

    def test_incomplete_counts_cannot_pass(self):
        raw, host, player = fixture()
        for field in ("callbacks", "submissions", "submitted_packets",
                      "submit_result:0", "callback_status:0"):
            h = copy.deepcopy(host)
            h["host_endpoint_counters"]["1"][field] -= 1
            with self.subTest(field=field):
                self.assertTrue(evidence.check(raw, h, player))
        for field in ("host_submissions_still_pending", "unpaired_host_callbacks"):
            h = copy.deepcopy(host)
            h.pop(field)
            self.assertTrue(evidence.check(raw, h, player))

    def test_loss_or_extra_endpoints_cannot_pass(self):
        raw, host, player = fixture()
        host["ring_events"]["out_of_bufs:1"] = 1
        self.assertTrue(evidence.check(raw, host, player))
        host["ring_events"].clear()
        raw["endpoints"]["86"] = {"callbacks": 1, "packets": 32}
        self.assertTrue(evidence.check(raw, host, player))

    def test_strict_player_fail_or_missing_close(self):
        raw, host, player = fixture()
        for p in (player.replace("1500", "1499"), player.replace("Close=0", "Close=1"),
                  player.replace("samples=96000", "samples=95999"),
                  player + "Close=1\n",
                  player + "Submitted=0 completed=0 samples=0 elapsed_ms=0\n",
                  player + "FAIL: retained buffers\n", ""):
            with self.subTest(player=p):
                self.assertTrue(evidence.check(raw, host, p))

    def test_tiny_or_stretched_transport_cannot_pass(self):
        raw, host, player = fixture()
        for field, value in (("submitted_packets", 32), ("submitted_requested_bytes", 768)):
            h = copy.deepcopy(host)
            h["host_endpoint_counters"]["1"][field] = value
            self.assertTrue(evidence.check(raw, h, player))
        for value in (1499, 2501, float("nan"), None):
            h = copy.deepcopy(host)
            h["host_first_submit_to_final_callback_ms"]["1"] = value
            self.assertTrue(evidence.check(raw, h, player))
        host["host_first_submit_to_final_callback_ms"]["3"] = 20
        self.assertTrue(evidence.check(raw, host, player))

    def test_mandatory_wall_clock_record(self):
        raw, host, player = fixture()
        line = "Performance elapsed_ms=2000 (required wall-clock check)\n"
        for replacement in ("", line + line,
                            "Performance elapsed_ms=2000 (diagnostic only)\n",
                            line.replace("2000", "1499"),
                            line.replace("2000", "2501"),
                            line.replace("2000", "NaN")):
            with self.subTest(replacement=replacement):
                self.assertTrue(evidence.check(raw, host, player.replace(line, replacement)))
        for value in (1500, 2500):
            self.assertEqual(evidence.check(raw, host, player.replace(line,
                             line.replace("2000", str(value)))), [])

    def test_soak_requires_separate_full_coverage(self):
        raw, host, player = fixture()
        self.assertTrue(evidence.check(raw, host, player, soak=True))
        player = ("Submitted=120 completed=120 samples=1440000 elapsed_ms=30000\n"
                  "Performance elapsed_ms=30000 (required wall-clock check)\n"
                  "Close=0\nPASS: WinMM real-buffer completion/position/close.\n")
        for raw_ep, ep, callbacks, packets, size in (
                ("01", "1", 7500, 240000, 5760000),
                ("83", "3", 938, 30016, 120064)):
            raw["endpoints"][raw_ep] = {"callbacks": callbacks, "packets": packets}
            h = host["host_endpoint_counters"][ep]
            h.update(callbacks=callbacks, submissions=callbacks,
                     submitted_packets=packets, submitted_requested_bytes=size)
            h["submit_result:0"] = h["callback_status:0"] = callbacks
            host["host_first_submit_to_final_callback_ms"][ep] = 30000
        self.assertEqual(evidence.check(raw, host, player, soak=True), [])
        self.assertTrue(evidence.check(raw, host, player))
        for value in (29500, 30500):
            self.assertEqual(evidence.check(raw, host, player.replace("30000", str(value)),
                                            soak=True), [])
        for value in (29499, 30501):
            self.assertTrue(evidence.check(raw, host, player.replace("30000", str(value)),
                                           soak=True))
        for ep, field, value in (("1", "submitted_requested_bytes", 5759999),
                                 ("3", "submitted_packets", 29499)):
            h = copy.deepcopy(host)
            h["host_endpoint_counters"][ep][field] = value
            self.assertTrue(evidence.check(raw, h, player, soak=True))


VM_FAIL = ("Submitted=8 completed=8 samples=96000 elapsed_ms=1359\n"
           "Performance elapsed_ms=2019 (required wall-clock check)\n"
           "FAIL unless all8buffers complete near96000samples after real duration.\n"
           "Reset=0\nClose=0\nFAIL: WinMM queued playback test.\n")


class VmRule(unittest.TestCase):
    """Owner 2026-10-22: in a VM, GetTick is diagnostic; QPC and host window decide."""
    def test_gettick_only_failure_passes_in_vm_not_physical(self):
        raw, host, _ = fixture()
        host["host_first_submit_to_final_callback_ms"]["1"] = 2010
        self.assertEqual(evidence.check(raw, host, VM_FAIL, vm=True), [])
        physical = evidence.check(raw, host, VM_FAIL)
        self.assertIn("original minimum guest elapsed-time criterion fails", physical)
        self.assertIn("one unambiguous strict WinMM PASS required", physical)

    def test_host_window_must_agree_with_qpc(self):
        raw, host, _ = fixture()
        host["host_first_submit_to_final_callback_ms"]["1"] = 2120  # 101 ms off QPC 2019
        self.assertTrue(any("agree" in r for r in evidence.check(raw, host, VM_FAIL, vm=True)))
        host["host_first_submit_to_final_callback_ms"]["1"] = 1919
        self.assertEqual(evidence.check(raw, host, VM_FAIL, vm=True), [])

    def test_other_player_failures_still_fail(self):
        raw, host, _ = fixture()
        host["host_first_submit_to_final_callback_ms"]["1"] = 2010
        for extra in ("FAIL: completion deadline.\n", "FAIL: playback position moved backward.\n",
                      "FAIL: independent wall-clock completion deadline.\n"):
            self.assertIn("player failure other than the VM GetTick diagnostic",
                          evidence.check(raw, host, VM_FAIL + extra, vm=True))
        # Verdict lines without a GetTick shortfall are not excused.
        ok_tick = VM_FAIL.replace("elapsed_ms=1359", "elapsed_ms=1900")
        self.assertIn("player failure other than the VM GetTick diagnostic",
                      evidence.check(raw, host, ok_tick, vm=True))
        # QPC outside +/-500 ms still fails.
        bad_qpc = VM_FAIL.replace("Performance elapsed_ms=2019", "Performance elapsed_ms=2600")
        self.assertTrue(any("wall-clock" in r for r in evidence.check(raw, host, bad_qpc, vm=True)))
        # Incomplete samples still fail.
        short = VM_FAIL.replace("samples=96000", "samples=95000")
        self.assertIn("complete configured client sample/buffer record missing",
                      evidence.check(raw, host, short, vm=True))

    def test_transport_rules_unchanged_in_vm(self):
        raw, host, _ = fixture()
        host["host_first_submit_to_final_callback_ms"]["1"] = 2010
        raw["failures"] = [{"ep": "01"}]
        self.assertIn("nonzero raw packet or URB errors", evidence.check(raw, host, VM_FAIL, vm=True))


if __name__ == "__main__":
    unittest.main()

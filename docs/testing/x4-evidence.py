#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""Check a bounded X4 playback run against independent host transport logs.

Usage: python -B x4-evidence.py [--soak] RAW_LOG HOST_TRACE PLAY_REPORT OUTPUT_JSON
Exit0: strict player PASS plus matched, zero-error bounded transport evidence.
Exit1: evidence does not meet that gate. Exit2: input/parser failure.
This does not establish audible output, sample fidelity or long-run reliability.
Designed only for X4 OUT01/feedback83 with the diagnostic QEMU/libusb build.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("kiso_raw", HERE / "kiso-raw.py")
kiso = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kiso)


# VM rule (Owner, 2026-10-22): inside a VM the guest's GetTickCount is a
# recorded diagnostic, not a duration verdict, because CLKSER showed it
# running at about 0.70x only during ISO playback while guest QPC tracked an
# independent host clock exactly (uac2-vm.md, "Quiet slot #3"). Duration is
# judged by guest QPC (+/-500 ms, unchanged) AND agreement of the independent
# host OUT window with QPC within 100 ms. On physical systems (no --vm) the
# original GetTick criterion stays a hard check.
VM_AGREEMENT_MS = 100
PLAYER_VERDICT_LINES = (r"FAIL unless all\d+buffers complete near\d+samples after real duration\.",
                        r"FAIL: WinMM queued playback test\.")
vm_diagnostics = {}


def check(raw, host, player, soak=False, vm=False):
    reasons = []
    buffers, frames = (120, 1440000) if soak else (8, 96000)
    minimum, maximum = (29500, 30500) if soak else (1500, 2500)
    out_packets, feedback_packets = (240000, 29500) if soak else (16000, 1600)
    if raw["invalid_records"]:
        reasons.append("missing or malformed raw packet records")
    if raw["failures"]:
        reasons.append("nonzero raw packet or URB errors")
    lines = player.splitlines()
    summaries = [line for line in lines if line.startswith("Submitted=")]
    match = re.fullmatch(r"Submitted=(\d+) completed=(\d+) samples=(\d+) elapsed_ms=(\d+)",
                         summaries[0]) if len(summaries) == 1 else None
    tick_short = bool(match) and int(match[4]) < minimum
    passed = lines.count("PASS: WinMM real-buffer completion/position/close.") == 1
    failures = [line for line in lines if re.match(r"(?:FAIL|NOTEST)\b", line)]
    if vm:
        # The player's own verdict folds in GetTick. Accept its FAIL only when
        # the verdict lines are its sole failures and GetTick is the only
        # internal criterion that missed; every other check below still runs.
        only_verdict = failures and all(
            any(re.fullmatch(p, line) for p in PLAYER_VERDICT_LINES) for line in failures)
        if not passed and not (only_verdict and tick_short):
            reasons.append("player failure other than the VM GetTick diagnostic")
        if passed and failures:
            reasons.append("player reports failure or no test")
    else:
        if not passed:
            reasons.append("one unambiguous strict WinMM PASS required")
        if failures:
            reasons.append("player reports failure or no test")
    if not match or tuple(map(int, match.groups()[:3])) != (buffers, buffers, frames):
        reasons.append("complete configured client sample/buffer record missing")
    elif tick_short and not vm:
        reasons.append("original minimum guest elapsed-time criterion fails")
    wall_lines = [line for line in lines if line.startswith("Performance elapsed_ms=")]
    wall = re.fullmatch(r"Performance elapsed_ms=(\d+) \(required wall-clock check\)",
                        wall_lines[0]) if len(wall_lines) == 1 else None
    if not wall or not minimum <= int(wall[1]) <= maximum:
        reasons.append(f"one mandatory player wall-clock check within {minimum}..{maximum}ms required")
    if vm:
        window = host.get("host_first_submit_to_final_callback_ms", {}).get("1")
        agree = (wall is not None and isinstance(window, (int, float)) and
                 abs(window - int(wall[1])) <= VM_AGREEMENT_MS)
        if not agree:
            reasons.append(f"VM rule: host OUT window must agree with guest QPC within {VM_AGREEMENT_MS}ms")
        vm_diagnostics.update({
            "rule": "VM: duration by guest QPC +/-500ms and host OUT window within 100ms of QPC; GetTick diagnostic only",
            "guest_gettick_ms": int(match[4]) if match else None,
            "guest_qpc_ms": int(wall[1]) if wall else None,
            "host_out_window_ms": window,
            "gettick_below_original_minimum": tick_short})
    if [line for line in lines if line.startswith("Close=")] != ["Close=0"]:
        reasons.append("one unambiguous successful player close required")
    counters = host.get("host_endpoint_counters", {})
    for raw_ep, ep in (("01", "1"), ("83", "3")):
        r = raw["endpoints"].get(raw_ep, {})
        h = counters.get(ep, {})
        callbacks = r.get("callbacks", 0)
        if not callbacks or callbacks != h.get("callbacks"):
            reasons.append(f"raw/trace callback coverage mismatch endpoint {raw_ep}")
        if not callbacks or h.get("submissions") != callbacks:
            reasons.append(f"submission/callback mismatch endpoint {raw_ep}")
        if r.get("packets", 0) != h.get("submitted_packets"):
            reasons.append(f"raw/trace packet coverage mismatch endpoint {raw_ep}")
        if h.get("submit_result:0", 0) != callbacks:
            reasons.append(f"non-success or missing submissions endpoint {raw_ep}")
        if h.get("callback_status:0", 0) != callbacks:
            reasons.append(f"non-success or missing callbacks endpoint {raw_ep}")
        if h.get("failed_packets", 0):
            reasons.append(f"translated host packet errors endpoint {raw_ep}")
    out = counters.get("1", {})
    if out.get("submitted_packets", 0) < out_packets or out.get("submitted_requested_bytes", 0) < frames * 4:
        reasons.append(f"insufficient OUT coverage for {frames} stereo16 samples")
    if counters.get("3", {}).get("submitted_packets", 0) < feedback_packets:
        reasons.append("insufficient X4 feedback interval coverage")
    window = host.get("host_first_submit_to_final_callback_ms", {}).get("1")
    # Additional bounded-test policy, not a USB/UAC requirement or replacement
    # for the unchanged player criterion. Reject badly stretched/short logs.
    if not isinstance(window, (int, float)) or not minimum <= window <= maximum:
        reasons.append(f"host OUT window outside bounded test policy ({minimum}..{maximum}ms)")
    feedback_window = host.get("host_first_submit_to_final_callback_ms", {}).get("3")
    if not isinstance(feedback_window, (int, float)) or not feedback_window >= minimum:
        reasons.append(f"insufficient feedback host-window coverage ({minimum}ms minimum)")
    for event, count in host.get("ring_events", {}).items():
        if event.startswith("out_of_bufs:") and count:
            reasons.append(f"host empty-ring losses: {count} ({event})")
    for field in ("host_submissions_still_pending", "unpaired_host_callbacks"):
        if host.get(field) != 0:
            reasons.append(f"incomplete host accounting: {field}")
    # Unknown endpoint records cannot substitute for the two expected endpoints.
    if set(raw["endpoints"]) != {"01", "83"} or set(counters) != {"1", "3"}:
        reasons.append("unexpected or missing ISO endpoints")
    return reasons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--soak", action="store_true", help="require the fixed 30-second UACSOAK experiment")
    parser.add_argument("--vm", action="store_true", help="apply the owner's VM rule (GetTick diagnostic; QPC and host window decide duration)")
    for name in ("raw", "trace", "player", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    host_output = args.output.with_suffix(".host.json")
    paths = [p.resolve() for p in
             (args.raw, args.trace, args.player, args.output, host_output)]
    aliases = len(set(paths)) != len(paths)
    for i, path in enumerate(paths):
        for other in paths[i+1:]:
            if path.exists() and other.exists() and path.samefile(other):
                aliases = True
    if aliases:
        print("Inputs, output and generated host summary must be distinct paths",
              file=sys.stderr)
        return 2
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Invalidate an earlier PASS before any subprocess/input parsing can fail.
    args.output.write_text(json.dumps({"bounded_transport_gate": "INCOMPLETE",
                                      "reasons": ["analysis did not finish"]}) + "\n",
                           encoding="utf-8")
    command = [sys.executable, "-B", str(HERE / "qemu-iso-trace.py"),
               str(args.trace), str(host_output)]
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode:
        print(completed.stderr, file=sys.stderr)
        return 2
    host = json.loads(host_output.read_text())
    # Reject trace mutation between independent analyzer and this consumer.
    trace_hash = hashlib.sha256(args.trace.read_bytes()).hexdigest()
    if trace_hash != host.get("sha256"):
        print("Trace changed during analysis", file=sys.stderr)
        return 2
    raw = kiso.summarize(args.raw.read_bytes())
    player_bytes = args.player.read_bytes()
    reasons = check(raw, host, player_bytes.decode("utf-8", "replace"), args.soak, args.vm)
    result = {
        "bounded_transport_gate": "FAIL" if reasons else "PASS",
        "test_duration_seconds": 30 if args.soak else 2,
        "duration_rule": "VM (Owner 2026-10-22)" if args.vm else "physical (GetTick hard)",
        "vm_diagnostics": vm_diagnostics if args.vm else None,
        "reasons": reasons, "raw": raw,
        "host_summary": str(host_output), "trace_sha256": trace_hash,
        "player_sha256": hashlib.sha256(player_bytes).hexdigest(),
        "limits": [
            "A single bounded pass does not establish repeatability or long-run reliability.",
            "No audible output or sample fidelity claim.",
            "Applies only to the exact instrumented X4 test configuration.",
            "Mandatory player QPC and independent host windows are both required.",
            "Host coverage/window thresholds are fixed X4 test policies, not class limits.",
            "Matching counts cannot by themselves establish same-run client provenance."
        ]
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"bounded_transport_gate": result["bounded_transport_gate"],
                      "reasons": reasons}, indent=2))
    return int(bool(reasons))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(2)

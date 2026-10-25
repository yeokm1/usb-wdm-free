#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Summarize bounded original/patched QEMU USB traces, without reading PCM.

Usage: python -B qemu-iso-trace.py INPUT_LOG OUTPUT_JSON
Guest completion and host callback measurements are kept separate.
"""
import collections
import hashlib
import json
from pathlib import Path
import re
import statistics
import sys

path, output = map(Path, sys.argv[1:3])
counts = collections.Counter()
sizes = collections.defaultdict(collections.Counter)
pending_guest = {}
guest_results = collections.defaultdict(collections.Counter)
pending_host = {}
host = collections.defaultdict(collections.Counter)
latency = collections.defaultdict(list)
callback_gaps = collections.defaultdict(list)
last_callback = {}
first_submit = {}
unpaired = 0
ring_free = []
ehci_out = {"last_summary": None, "fail_status_counts": {}}
ehci_skips = []
last_ns = None

for line in path.read_text().splitlines():
    m = re.search(r"usb_host_req_data .*packet (\w+), in (\d+), ep (\d+), size (\d+)", line)
    if m:
        pointer, direction, ep, size = m.groups()
        key = direction + ":" + ep
        sizes[key][int(size)] += 1
        pending_guest[pointer] = key
    m = re.search(r"usb_host_req_complete .*packet (\w+), status (-?\d+), length (\d+)", line)
    if m:
        pointer, status, length = m.groups()
        key = pending_guest.pop(pointer, "control-or-unpaired")
        guest_results[key][status + ":" + length] += 1
    m = re.search(r"usb_host_iso_(start|stop|out_of_bufs) .*ep (\d+)", line)
    if m:
        counts[m[1] + ":" + m[2]] += 1
    # qemu-acct.patch: guest packets a ring accepted/refused, and those still
    # waiting in its copy queue (discarded) when the ring was released.
    m = re.search(r"usb_host_iso_ring_free .*ep (\d+), accepted (\d+), accepted_bytes (\d+), refused (\d+), pending (\d+)", line)
    if m:
        ring_free.append({"ep": m[1], "accepted_packets": int(m[2]), "accepted_bytes": int(m[3]),
                          "refused_packets": int(m[4]), "pending_discarded_packets": int(m[5])})
    # qemu-ehci-acct.patch: OUT iTD transactions EHCI executed, by result.
    m = re.search(r"usb_ehci_iso_out_summary ok (\d+) ok_bytes (\d+) fail (\d+) fail_bytes (\d+)", line)
    if m:
        ehci_out["last_summary"] = {"ok": int(m[1]), "ok_bytes": int(m[2]),
                                    "fail": int(m[3]), "fail_bytes": int(m[4])}
    m = re.search(r"usb_ehci_iso_out_fail .*status (-?\d+) len (\d+)", line)
    if m:
        ehci_out["fail_status_counts"][m[1]] = ehci_out["fail_status_counts"].get(m[1], 0) + 1
    # Upstream EHCI: microframes skipped (never executed) when far behind.
    m = re.search(r"usb_ehci_skipped_uframes skipped (\d+) uframes", line)
    if m:
        ehci_skips.append({"uframes": int(m[1]), "after_last_ns": last_ns})
    m = re.search(r" ns (\d+)", line)
    if m:
        last_ns = int(m[1])
    m = re.search(r"usb_host_iso_submit .*ep (\d+), transfer (\w+), ns (\d+), packets (\d+), length (\d+), result (-?\d+)", line)
    if m:
        ep, pointer, ns, packets, length, result = m.groups()
        h = host[ep]
        h["submissions"] += 1
        h["submit_result:" + result] += 1
        h["submitted_requested_bytes"] += int(length)
        h["submitted_packets"] += int(packets)
        if result == "0":
            first_submit.setdefault(ep, int(ns))
            assert pointer not in pending_host, "overlapping host transfer reuse"
            pending_host[pointer] = ep, int(ns)
    m = re.search(r"usb_host_iso_callback .*ep (\d+), transfer (\w+), ns (\d+), status (-?\d+), packets (\d+), failed (\d+), first (-?\d+), actual (\d+)", line)
    orphan = None
    if not m:
        # Completion of a transfer whose ring was released while inflight
        # (qemu-orphan.patch). The trace has no endpoint: attribute it only
        # through its own outstanding submission; otherwise it is unpaired.
        orphan = re.search(r"usb_host_iso_orphan_callback .*transfer (\w+), ns (\d+), status (-?\d+), packets (\d+), failed (\d+), first (-?\d+), actual (\d+)", line)
        if orphan:
            start = pending_host.get(orphan[1])
            if start is None:
                unpaired += 1
                counts["orphan_unpaired"] += 1
                continue
            m = (start[0],) + orphan.groups()
    if m:
        ep, pointer, ns, status, packets, failed, first, actual = m if orphan else m.groups()
        ns = int(ns)
        h = host[ep]
        h["callbacks"] += 1
        if orphan:
            h["orphan_callbacks"] += 1
        h["callback_status:" + status] += 1
        h["reported_actual_bytes"] += int(actual)
        h["failed_packets"] += int(failed)
        if status == "0":
            h["failed_packets_in_successful_callbacks"] += int(failed)
        if int(failed):
            h["first_packet_error_status:" + first] += 1
        start = pending_host.pop(pointer, None)
        if start is None:
            unpaired += 1
        else:
            assert start[0] == ep and ns >= start[1]
            latency[ep].append((ns - start[1]) / 1e6)
        if ep in last_callback:
            assert ns >= last_callback[ep]
            callback_gaps[ep].append((ns - last_callback[ep]) / 1e6)
        last_callback[ep] = ns


def summary(values):
    if not values:
        return None
    ordered = sorted(values)
    return {"count": len(values), "min_ms": min(values),
            "median_ms": statistics.median(values),
            "p95_ms": ordered[int((len(ordered)-1)*.95)], "max_ms": max(values)}


result = {
    "trace": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    "ring_events": dict(counts),
    "guest_request_packet_sizes_direction_endpoint": {k: dict(v) for k, v in sizes.items()},
    "guest_completion_status_length": {k: dict(v) for k, v in guest_results.items()},
    "guest_requests_still_pending": dict(collections.Counter(pending_guest.values())),
    "host_endpoint_counters": {k: dict(v) for k, v in host.items()},
    "host_submit_to_callback": {k: summary(v) for k, v in latency.items()},
    "host_callback_gaps": {k: summary(v) for k, v in callback_gaps.items()},
    "host_first_submit_to_final_callback_ms": {
        ep: (last_callback[ep]-ns)/1e6 for ep, ns in first_submit.items()
        if ep in last_callback},
    "host_submissions_still_pending": len(pending_host), "unpaired_host_callbacks": unpaired,
    "ring_free_accounting": ring_free,
    "ehci_iso_out": ehci_out,
    "ehci_skipped_uframes": {"events": len(ehci_skips), "total": sum(s["uframes"] for s in ehci_skips),
                             "list": ehci_skips[:50]},
    "limits": ["Guest request completion is not a host acknowledgement.",
               "libusbK OUT actual lengths are copied requested lengths; inspect packet statuses separately.",
               "Translated libusbK packet success may mask folded raw errors; require raw KISO diagnostics.",
               "Trace counts and statuses do not certify audible output or PCM fidelity.",
               "Original traces lack host submit/callback records and timestamps."]
}
output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))

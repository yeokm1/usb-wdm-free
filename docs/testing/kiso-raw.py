#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Summarize original diagnostic KISO_RAW records without decoding USB status.

Usage: python -B kiso-raw.py INPUT_LOG OUTPUT_JSON
Exit 0 means records were parsed, NOT clean playback. Invalid/missing records
return 2. Match callback counts against the independent QEMU trace before
using zero errors as transport evidence; these logs do not certify sound.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re


def summarize(data):
    totals = collections.defaultdict(collections.Counter)
    failures, invalid, policies = [], [], []
    required = {"ep", "packets", "raw_count", "raw_errors", "raw_urb",
                "failed", "first"}
    for line_number, line in enumerate(data.decode("utf-8", "replace").splitlines(), 1):
        if "KISO_POLICY " in line or "KISO_POLICY_LATENCY " in line:
            policies.append(line)
        if "KISO_RAW" not in line:
            continue
        try:
            fields = {}
            for token in line.split("KISO_RAW", 1)[1].strip().split():
                match = re.fullmatch(r"([A-Za-z_]\w*)=([0-9A-Fa-f]+)", token)
                if not match or match[1] in fields:
                    raise ValueError("malformed or duplicate raw field")
                fields[match[1]] = match[2]
            if not required.issubset(fields):
                raise ValueError("missing required fields")
            ep = int(fields["ep"], 16)
            if not 0 <= ep <= 255:
                raise ValueError("endpoint out of range")
            packets, count, errors, failed = (
                int(fields[k], 10) for k in
                ("packets", "raw_count", "raw_errors", "failed"))
            urb, first = int(fields["raw_urb"], 16), int(fields["first"], 16)
            if not 0 < packets <= 65536 or count != packets:
                raise ValueError("packet count mismatch or out of range")
            if not 0 <= failed <= packets or not 0 <= errors <= packets:
                raise ValueError("error count out of range")
            if errors != failed or bool(failed) != bool(first):
                raise ValueError("raw and scanned packet errors disagree")
            if not 0 <= urb <= 0xffffffff or not 0 <= first <= 0xffff:
                raise ValueError("status outside recorded width")
            location_fields = {"first_index", "last_index", "mask32"}
            present = location_fields.intersection(fields)
            location = {}
            if present:
                if present != location_fields:
                    raise ValueError("incomplete packet locations")
                lo, hi = int(fields["first_index"]), int(fields["last_index"])
                mask = int(fields["mask32"], 16)
                if mask > 0xffffffff:
                    raise ValueError("mask exceeds 32 bits")
                if not failed:
                    if lo or hi or mask:
                        raise ValueError("locations without failures")
                else:
                    if not 0 <= lo <= hi < packets or failed > hi - lo + 1:
                        raise ValueError("invalid failed packet span")
                    if mask.bit_count() > failed:
                        raise ValueError("mask has too many failed packets")
                    if packets <= 32:
                        if mask >> packets or mask.bit_count() != failed:
                            raise ValueError("mask and failed count disagree")
                        indices = [i for i in range(packets) if mask & (1 << i)]
                        if not indices or (indices[0], indices[-1]) != (lo, hi):
                            raise ValueError("mask and failed span disagree")
                location = {"first_index": lo, "last_index": hi,
                            "mask32": f"{mask:08x}"}
            key = f"{ep:02x}"
            t = totals[key]
            t.update(callbacks=1, packets=packets, raw_errors=errors,
                     failed_packets=failed, nonzero_urb_callbacks=int(bool(urb)))
            if failed or urb:
                failures.append({"line": line_number, "ep": key,
                                 "endpoint_callback_ordinal": t["callbacks"],
                                 "failed": failed, "first_folded_status": f"{first:04x}",
                                 "urb_status": f"{urb:08x}", **location})
        except ValueError as exc:
            invalid.append({"line": line_number, "reason": str(exc)})
    if not totals:
        invalid.append({"line": 0, "reason": "no valid KISO_RAW records"})
    return {
        "raw_sha256": hashlib.sha256(data).hexdigest(),
        "endpoints": {ep: dict(t) for ep, t in sorted(totals.items())},
        "failures": failures, "invalid_records": invalid,
        "policy_messages": policies,
        "limits": [
            "Exit 0 means parsed evidence, not a playback PASS.",
            "Folded 16-bit status cannot uniquely reconstruct a native USB status.",
            "Match callbacks against independently hashed full host traces.",
            "No audible output, PCM fidelity or complete log coverage is established."
        ]
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = summarize(args.input.read_bytes())
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"endpoints": result["endpoints"],
                      "invalid_records": result["invalid_records"]}, indent=2))
    return 2 if result["invalid_records"] else 0


if __name__ == "__main__":
    raise SystemExit(main())

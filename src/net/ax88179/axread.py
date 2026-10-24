#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""Read-only host AX88179 observations using the actual C request planner.

No configuration, interface claim, reset, driver change or vendor OUT.
Does not establish guest-driver support or a usable Ethernet link.
"""
import argparse
import ctypes as c
import datetime
import json
from pathlib import Path


class Device(c.Structure):
    _fields_ = [(n, c.c_uint) for n in (
        'Valid Speed UsbRevision Configuration Interface BulkIn BulkOut StatusIn '
        'InPacket OutPacket StatusPacket InBurst OutBurst StatusBurst StatusInterval '
        'StatusBytes ConfigBytes Warnings').split()]


class Step(c.Structure):
    _fields_ = [(n, c.c_uint) for n in
                'Kind RequestType Request Value Index Length ResultTag DelayMs'.split()] + [('Data', c.c_ubyte * 8)]


class Plan(c.Structure):
    _fields_ = [(n, c.c_uint) for n in 'Valid Count HasWrites NextGate'.split()] + [('Step', Step * 8)]


class Link(c.Structure):
    _fields_ = [(n, c.c_uint) for n in 'Up Megabits FullDuplex'.split()]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--libusb', required=True)
    ap.add_argument('--output', type=Path, required=True, help='new JSON file; includes raw MAC')
    args = ap.parse_args()
    core = c.CDLL(str(Path(__file__).resolve().parents[3] / 'out/net/ax88179/hostabi/axcore.dll'))
    core.AxCtrlDescribe.argtypes = [c.c_void_p, c.c_uint, c.c_void_p, c.c_uint, c.c_uint, c.POINTER(Device)]
    core.AxCtrlReadPlan.argtypes = [c.POINTER(Device), c.POINTER(Plan)]
    core.AxCtrlResponse.argtypes = [c.POINTER(Step), c.c_uint]
    core.AxCtrlMac.argtypes = [c.c_void_p, c.c_uint]
    core.AxCtrlPhy.argtypes = [c.c_void_p, c.c_uint, c.POINTER(Link)]
    lib, ptr = c.CDLL(str(Path(args.libusb).resolve())), c.c_void_p
    signatures = {
        'init': ([c.POINTER(ptr)], c.c_int), 'exit': ([ptr], None),
        'get_device_list': ([ptr, c.POINTER(c.POINTER(ptr))], c.c_ssize_t),
        'free_device_list': ([c.POINTER(ptr), c.c_int], None),
        'get_device_descriptor': ([ptr, ptr], c.c_int),
        'get_device_speed': ([ptr], c.c_int),
        'open': ([ptr, c.POINTER(ptr)], c.c_int), 'close': ([ptr], None),
        'control_transfer': ([ptr, c.c_uint8, c.c_uint8, c.c_uint16, c.c_uint16,
                              ptr, c.c_uint16, c.c_uint], c.c_int)}
    for name, (params, result) in signatures.items():
        fn = getattr(lib, 'libusb_' + name)
        fn.argtypes, fn.restype = params, result
    ctx, devices, handle = ptr(), c.POINTER(ptr)(), ptr()
    if lib.libusb_init(c.byref(ctx)):
        raise RuntimeError('libusb_init failed')
    try:
        count = lib.libusb_get_device_list(ctx, c.byref(devices))
        if count < 0:
            raise RuntimeError(f'get_device_list: {count}')
        matches = []
        for i in range(count):
            desc = (c.c_ubyte * 18)()
            if not lib.libusb_get_device_descriptor(devices[i], desc) and bytes(desc[8:12]) == b'\x95\x0b\x90\x17':
                matches.append((devices[i], bytes(desc)))
        if len(matches) != 1:
            raise RuntimeError(f'exactly one 0b95:1790 required, found {len(matches)}')
        device, descriptor = matches[0]
        speed = lib.libusb_get_device_speed(device)
        rc = lib.libusb_open(device, c.byref(handle))
        if rc:
            raise RuntimeError(f'open: {rc}')
        buf = (c.c_ubyte * 65535)()
        rc = lib.libusb_control_transfer(handle, 0x80, 6, 0x200, 0, buf, 9, 3000)
        if rc != 9:
            raise RuntimeError(f'configuration header: {rc}')
        total = int.from_bytes(bytes(buf[2:4]), 'little')
        if total < 9:
            raise RuntimeError('invalid configuration length')
        rc = lib.libusb_control_transfer(handle, 0x80, 6, 0x200, 0, buf, total, 3000)
        if rc != total:
            raise RuntimeError(f'configuration body: {rc}/{total}')
        profile, plan = Device(), Plan()
        rc = core.AxCtrlDescribe(descriptor, len(descriptor), buf, total, {2: 1, 3: 2, 4: 3}.get(speed, 0), c.byref(profile))
        if rc or core.AxCtrlReadPlan(c.byref(profile), c.byref(plan)):
            raise RuntimeError(f'descriptor/planner rejected: {rc}')
        if not plan.Valid or plan.HasWrites or not 0 < plan.Count <= 8:
            raise RuntimeError('unsafe read plan')
        report = dict(time_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      libusb_speed=speed, warnings=profile.Warnings, reads=[])
        # Check the entire plan before any vendor request. No OUT execution path.
        for step in plan.Step[:plan.Count]:
            if step.Kind != 1 or step.RequestType != 0xc0 or not 0 < step.Length <= 8:
                raise RuntimeError('non-read or oversized step')
        for step in plan.Step[:plan.Count]:
            data = (c.c_ubyte * 8)()
            rc = lib.libusb_control_transfer(handle, step.RequestType, step.Request,
                step.Value, step.Index, data, step.Length, 3000)
            entry = dict(tag=step.ResultTag, request=step.Request, value=step.Value,
                         index=step.Index, expected=step.Length, actual=rc)
            if rc >= 0:
                entry['hex'] = bytes(data[:rc]).hex()
            entry['valid_length'] = rc >= 0 and bool(core.AxCtrlResponse(c.byref(step), rc))
            if entry['valid_length'] and step.ResultTag == 2:
                entry['valid_unicast_mac'] = bool(core.AxCtrlMac(data, rc))
            if entry['valid_length'] and step.ResultTag == 3:
                link = Link()
                entry['valid_phy_status'] = bool(core.AxCtrlPhy(data, rc, c.byref(link)))
                entry['link'] = dict(up=link.Up, megabits=link.Megabits, full_duplex=link.FullDuplex)
            report['reads'].append(entry)
        with args.output.open('x', encoding='utf-8') as output:
            json.dump(report, output, indent=2)
        print(f'AX88179 speed={speed}; {sum(r["valid_length"] for r in report["reads"])}/{plan.Count} exact-length reads; {args.output}')
        return 0 if all(r['valid_length'] for r in report['reads']) else 1
    finally:
        if handle:
            lib.libusb_close(handle)
        if devices:
            lib.libusb_free_device_list(devices, 1)
        lib.libusb_exit(ctx)


if __name__ == '__main__':
    raise SystemExit(main())

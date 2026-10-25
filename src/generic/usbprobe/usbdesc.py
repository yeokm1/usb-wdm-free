#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Read-only USB descriptor capture through an installed libusb-1.0.

No configuration, interface claim, reset, string or class requests. Serial
numbers are deliberately not read. --output must name a new directory.
Requires Python 3 and QEMU's libusb-1.0.dll on this Windows test host.
"""
import argparse
import ctypes as c
import hashlib
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dll', required=True)
    ap.add_argument('--id', help='VID:PID, hex; omit to list devices')
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    if args.output and not args.id:
        ap.error('--output requires --id')
    lib = c.CDLL(str(Path(args.dll).resolve()))
    ptr = c.c_void_p
    signatures = {
        'init': ([c.POINTER(ptr)], c.c_int),
        'exit': ([ptr], None),
        'get_device_list': ([ptr, c.POINTER(c.POINTER(ptr))], c.c_ssize_t),
        'free_device_list': ([c.POINTER(ptr), c.c_int], None),
        'get_device_descriptor': ([ptr, ptr], c.c_int),
        'get_bus_number': ([ptr], c.c_uint8),
        'get_device_address': ([ptr], c.c_uint8),
        'get_device_speed': ([ptr], c.c_int),
        'open': ([ptr, c.POINTER(ptr)], c.c_int),
        'close': ([ptr], None),
        'control_transfer': ([ptr, c.c_uint8, c.c_uint8, c.c_uint16,
                              c.c_uint16, ptr, c.c_uint16, c.c_uint], c.c_int),
    }
    for name, (parameters, result) in signatures.items():
        fn = getattr(lib, 'libusb_' + name)
        fn.argtypes, fn.restype = parameters, result
    ctx, devices = ptr(), c.POINTER(ptr)()
    if lib.libusb_init(c.byref(ctx)):
        raise RuntimeError('libusb_init failed')
    try:
        count = lib.libusb_get_device_list(ctx, c.byref(devices))
        if count < 0:
            raise RuntimeError(f'get_device_list: {count}')
        matches = []
        for i in range(count):
            desc = (c.c_ubyte * 18)()
            if lib.libusb_get_device_descriptor(devices[i], desc):
                continue
            raw = bytes(desc)
            ident = f'{int.from_bytes(raw[8:10], "little"):04x}:' \
                    f'{int.from_bytes(raw[10:12], "little"):04x}'
            if args.id and ident != args.id.lower():
                continue
            dev = devices[i]
            print(f'{ident} bus={lib.libusb_get_bus_number(dev)} '
                  f'addr={lib.libusb_get_device_address(dev)} '
                  f'libusb_speed={lib.libusb_get_device_speed(dev)}')
            matches.append((dev, raw))
        if args.id:
            if len(matches) != 1:
                raise RuntimeError(f'need exactly one matching device, got {len(matches)}')
            dev, raw = matches[0]
            handle = ptr()
            rc = lib.libusb_open(dev, c.byref(handle))
            if rc:
                raise RuntimeError(f'open: {rc}; check WinUSB binding')
            files = {'device.bin': raw}
            try:
                for index in range(raw[17]):
                    buf = (c.c_ubyte * 65535)()
                    rc = lib.libusb_control_transfer(handle, 0x80, 6,
                                                    0x0200 | index, 0, buf, 9, 3000)
                    if rc != 9:
                        raise RuntimeError(f'configuration header: {rc}')
                    total = int.from_bytes(bytes(buf[2:4]), 'little')
                    if total < 9:
                        raise RuntimeError('invalid total length')
                    rc = lib.libusb_control_transfer(handle, 0x80, 6,
                                                    0x0200 | index, 0, buf, total, 3000)
                    if rc != total:
                        raise RuntimeError(f'configuration body: {rc}/{total}')
                    data = bytes(buf[:total])
                    files[f'config{index}.bin'] = data
                    off = 0
                    while off < total:
                        n = data[off]
                        if n < 2 or off + n > total:
                            raise RuntimeError(f'malformed descriptor at {off}')
                        d = data[off:off + n]
                        if d[1] == 4 and n >= 9:
                            print(f'  interface={d[2]} alt={d[3]} '
                                  f'class={d[5]:02x}/{d[6]:02x}/{d[7]:02x}')
                        if d[1] == 5 and n >= 7:
                            print(f'    endpoint={d[2]:02x} attributes={d[3]:02x} '
                                  f'maxpacket={int.from_bytes(d[4:6], "little")}')
                        off += n
            finally:
                lib.libusb_close(handle)
            if args.output:
                args.output.mkdir(parents=True, exist_ok=False)
                for name, data in files.items():
                    (args.output / name).write_bytes(data)
                    print(f'{hashlib.sha256(data).hexdigest()}  {name}')
    finally:
        if devices:
            lib.libusb_free_device_list(devices, 1)
        lib.libusb_exit(ctx)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""SIMULATED MTP/PTP device (phone or camera) on localhost usbredir (test
aid only; docs/design/mtp.md, sections 11.2 and 12, M0).

A still image class responder written for this repository from the USB
Still Image Capture Device Definition 1.0 (with its errata of 2007-03-16),
MTP 1.1 and USB 2.0 chapter 9, over SPICE's usb-redirection-protocol v0.7.
The usbredir framing follows this repository's ufiemu.py. Nothing of a real
phone's firmware is used; every string says SIMULATED. No physical USB
device is accessed.

Profiles (VID FFFF; High Speed unless --speed full or --speed super; at
SuperSpeed the device descriptor says bcdUSB 3.0 with bMaxPacketSize0 09h,
bulk is 1,024 bytes, every endpoint has its SuperSpeed companion, and a
BOS descriptor answers):

  android    composite: interface 0 the MTP function (06/01/01, interface
             string "MTP"), interface 1 an ADB-shaped function (FF/42/01);
             a Microsoft OS string and Extended Compat ID naming MTP; two
             storages; GetObjectPropList, SetObjectPropValue (rename),
             GetObjectPropDesc; Android's extension string; events; a
             zero-length packet after a phase that fills its last packet
             (FFFF:0060)
  androidff  as android, the MTP interface vendor class FF/FF/00 (FFFF:0061)
  camera     one interface 06/01/01, PTP only: no property operations, a
             DCIM tree of associations, GetObjectHandles refuses a parent
             filter, no zero-length packets, a read-only storage (FFFF:0062)
  split      as android, every data phase in MTP 1.1 Appendix H's split
             form (the 12-byte header in a packet of its own), both
             directions (FFFF:0063)
  locked     as android with no storage until {"unlock": 1} (FFFF:0064)
  single     as android without the ADB interface (FFFF:0065)

Faults and events are driven over --ctl, one JSON object a line, each
applying once (to the next matching moment) unless it says otherwise:

  {"fault": "stall_data"}       device-initiated cancel in the next data
                                phase from the device: bulk endpoints
                                halted, Get Device Status 201Fh with them
  {"fault": "short_data"}       the next data phase from the device stops
                                early with a short packet (a truncation)
  {"fault": "no_response"}      the next response is never sent
  {"fault": "stale_response"}   a response with the previous transaction id
                                goes before the next real one
  {"fault": "response_for_zlp"} the next zero-length packet is not sent
  {"fault": "busy", "n": 3}     Get Device Status says 2019h n times
  {"fault": "bad_status", "kind": "length"|"code"|"endpoint"|"repeat"}
                                the next Get Device Status is malformed
  {"fault": "ignore_cancel"}    Cancel Request is accepted but the device
                                stays busy (so the host's reset path runs)
  {"fault": "ep0_hang"}         the next class request is never answered
  {"fault": "out_hang"}         the next bulk OUT transfer is never answered
                                (until the host cancels it or resets)
  {"fault": "reset_event"}      queue a DeviceReset event (400Bh) and close
                                the session
  {"fault": "cancel_event"}     queue a CancelTransaction event for the
                                transaction in progress
  {"flood": 100}                queue that many ObjectAdded events
  {"unlock": 1}                 locked profile: add the storage, StoreAdded
  {"delay_ms": 5}               delay each bulk IN answer (races)
  {"quit": 1}                   end the simulation (the test host's unplug)
The answer is a JSON line: {"ok": true, ...}.

--selftest runs the model against a scripted host in this process.
"""
import argparse
import collections
import json
import select
import socket
import struct
import sys
import time
import zlib
from pathlib import Path

CAP_VERSION, CAP_EP_MAX, CAP_ID64, CAP_BULK32 = 1, 4, 5, 6
CAPS = (1 << CAP_VERSION) | (1 << CAP_EP_MAX) | (1 << CAP_ID64) | (1 << CAP_BULK32)

# usbredir message types (usb-redirection-protocol.txt)
HELLO, CONNECT, RESET, IFACE_INFO, EP_INFO = 0, 1, 3, 4, 5
SET_CONFIG, GET_CONFIG, CONFIG_STATUS = 6, 7, 8
SET_ALT, GET_ALT, ALT_STATUS = 9, 10, 11
START_INT, STOP_INT, INT_STATUS = 15, 16, 17
CANCEL, CONTROL, BULK, INTERRUPT = 21, 100, 101, 103
ST_OK, ST_CANCELLED, ST_STALL = 0, 1, 4

# Endpoints: MTP function on interface 0, ADB-shaped on interface 1.
EP_IN, EP_OUT, EP_INT = 0x81, 0x01, 0x82
ADB_IN, ADB_OUT = 0x83, 0x02

# Container types and codes (MTP 1.1 tables; still image 5.2 for class requests)
T_CMD, T_DATA, T_RESP, T_EVENT = 1, 2, 3, 4
OK, GENERAL, SESSION_NOT_OPEN, OP_NOT_SUPPORTED, PARAM_NOT_SUPPORTED = 0x2001, 0x2002, 0x2003, 0x2005, 0x2006
INVALID_STORAGE, INVALID_HANDLE, STORE_FULL, STORE_RO = 0x2008, 0x2009, 0x200C, 0x200E
DEVICE_BUSY, INVALID_PARENT, INVALID_PARAMETER, SESSION_OPEN, TRANSACTION_CANCELLED = \
    0x2019, 0x201A, 0x201D, 0x201E, 0x201F
INVALID_PROP_CODE, NO_VALID_OBJECTINFO = 0xA801, 0x2015
E_CANCEL, E_ADDED, E_REMOVED, E_STORE_ADDED, E_STORE_REMOVED = 0x4001, 0x4002, 0x4003, 0x4004, 0x4005
E_INFO_CHANGED, E_DEVICE_RESET = 0x4007, 0x400B
F_UNDEFINED, F_ASSOCIATION, F_JPEG = 0x3000, 0x3001, 0x3801
P_STORAGE, P_FORMAT, P_SIZE, P_NAME, P_MODIFIED, P_PARENT = 0xDC01, 0xDC02, 0xDC04, 0xDC07, 0xDC09, 0xDC0B
DT_UINT16, DT_UINT32, DT_UINT64, DT_STR = 0x0004, 0x0006, 0x0008, 0xFFFF
PROP_TYPES = {P_STORAGE: DT_UINT32, P_FORMAT: DT_UINT16, P_SIZE: DT_UINT64, P_NAME: DT_STR,
              P_MODIFIED: DT_STR, P_PARENT: DT_UINT32}

OPS = {
    0x1001: 'GetDeviceInfo', 0x1002: 'OpenSession', 0x1003: 'CloseSession', 0x1004: 'GetStorageIDs',
    0x1005: 'GetStorageInfo', 0x1006: 'GetNumObjects', 0x1007: 'GetObjectHandles', 0x1008: 'GetObjectInfo',
    0x1009: 'GetObject', 0x100B: 'DeleteObject', 0x100C: 'SendObjectInfo', 0x100D: 'SendObject',
    0x101B: 'GetPartialObject', 0x9801: 'GetObjectPropsSupported', 0x9802: 'GetObjectPropDesc',
    0x9803: 'GetObjectPropValue', 0x9804: 'SetObjectPropValue', 0x9805: 'GetObjectPropList',
}
PTP_OPS = [0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008, 0x1009, 0x101B]
MTP_OPS = PTP_OPS + [0x100B, 0x100C, 0x100D, 0x9801, 0x9802, 0x9803, 0x9804, 0x9805]
# A data phase to the device belongs to these operations.
OUT_DATA_OPS = (0x100C, 0x100D, 0x9804)

PROFILES = {
    'android': dict(pid=0x0060, cls=(6, 1, 1), adb=True, mtp=True, zlp=True, split=False, locked=False),
    'androidff': dict(pid=0x0061, cls=(0xFF, 0xFF, 0), adb=True, mtp=True, zlp=True, split=False, locked=False),
    'camera': dict(pid=0x0062, cls=(6, 1, 1), adb=False, mtp=False, zlp=False, split=False, locked=False),
    'split': dict(pid=0x0063, cls=(6, 1, 1), adb=True, mtp=True, zlp=True, split=True, locked=False),
    'locked': dict(pid=0x0064, cls=(6, 1, 1), adb=True, mtp=True, zlp=True, split=False, locked=True),
    'single': dict(pid=0x0065, cls=(6, 1, 1), adb=False, mtp=True, zlp=True, split=False, locked=False),
}


def usb_string(text):
    raw = text.encode('utf-16-le')
    return bytes((2 + len(raw), 3)) + raw


# ---------------------------------------------------------------- datasets

def u8(v): return struct.pack('<B', v)
def u16(v): return struct.pack('<H', v)
def u32(v): return struct.pack('<I', v & 0xffffffff)
def u64(v): return struct.pack('<Q', v)


def pstr(text):
    """PTP string: UINT8 count of UTF-16 units including the NUL (0 = empty)."""
    if not text:
        return b'\0'
    raw = text.encode('utf-16-le') + b'\0\0'
    return u8(len(raw) // 2) + raw


def arr16(values): return u32(len(values)) + b''.join(u16(v) for v in values)
def arr32(values): return u32(len(values)) + b''.join(u32(v) for v in values)


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise ValueError('dataset too short')
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def u16(self): return struct.unpack('<H', self.take(2))[0]
    def u32(self): return struct.unpack('<I', self.take(4))[0]

    def str(self):
        n = self.take(1)[0]
        if not n:
            return ''
        raw = self.take(2 * n)
        if raw[-2:] != b'\0\0':
            raise ValueError('string without NUL')
        return raw[:-2].decode('utf-16-le', 'replace')


def mtp_date(t):
    return time.strftime('%Y%m%dT%H%M%S', time.localtime(t))


# ---------------------------------------------------------------- the store

class Store:
    """Storages and objects of the simulated device, in memory."""

    def __init__(self, profile):
        self.next_handle = 1
        self.storages = collections.OrderedDict()
        self.objects = collections.OrderedDict()
        now = time.time()
        if profile == 'camera':
            self.add_storage(0x00010001, 'SD card - SIMULATED', 'SIMCAM', access=1, fs=3, kind=4)
            dcim = self.add(0x00010001, 0, 'DCIM', None, now)
            sub = self.add(0x00010001, dcim, '100SIMUL', None, now)
            for i in range(1, 6):
                self.add(0x00010001, sub, 'IMG_%04d.JPG' % i, self.pattern(40000 + 977 * i, i), now, F_JPEG)
        else:
            self.add_storage(0x00010001, 'Internal shared storage - SIMULATED', 'SIMPHONE')
            if profile != 'locked':
                self.populate(0x00010001, now)
            self.add_storage(0x00020001, 'SD card - SIMULATED', 'SIMSD', kind=4)
            self.add(0x00020001, 0, 'Music', None, now)
            if profile == 'locked':
                self.storages[0x00010001]['hidden'] = True

    @staticmethod
    def pattern(n, seed):
        block = bytes(((i * 7 + seed) ^ (i >> 8)) & 255 for i in range(4096))
        return (block * (n // 4096 + 1))[:n]

    def add_storage(self, sid, desc, label, access=0, fs=2, kind=3):
        self.storages[sid] = dict(desc=desc, label=label, access=access, fs=fs, kind=kind,
                                  capacity=8 << 30, hidden=False)

    def populate(self, sid, now):
        dcim = self.add(sid, 0, 'DCIM', None, now)
        cam = self.add(sid, dcim, 'Camera', None, now)
        for i in range(1, 4):
            self.add(sid, cam, 'IMG_2026111%d_SIM.jpg' % i, self.pattern(65536 * i + 12, i), now, F_JPEG)
        dl = self.add(sid, 0, 'Download', None, now)
        self.add(sid, dl, 'readme-SIMULATED.txt', b'SIMULATED MTP device of usb-wdm-free\r\n', now)
        # Boundary sizes of the framing rules (docs/design/mtp.md 7.2): a
        # payload of exactly one and two High Speed packets, and one less
        # than a Full Speed packet's multiple.
        self.add(sid, dl, 'exact512.bin', self.pattern(512 - 12, 3), now)
        self.add(sid, dl, 'exact1024.bin', self.pattern(1024 - 12, 4), now)
        self.add(sid, dl, 'odd.bin', self.pattern(1000, 5), now)
        self.add(sid, 0, 'Empty folder', None, now)

    def add(self, sid, parent, name, data, mtime, fmt=None):
        handle = self.next_handle
        self.next_handle += 1
        folder = data is None
        self.objects[handle] = dict(storage=sid, parent=parent, name=name,
                                    format=F_ASSOCIATION if folder else (fmt or F_UNDEFINED),
                                    data=b'' if folder else bytes(data), mtime=mtime)
        return handle

    def visible(self, sid):
        s = self.storages.get(sid)
        return s is not None and not s['hidden']

    def children(self, sid, parent):
        return [h for h, o in self.objects.items()
                if (sid == 0xFFFFFFFF or o['storage'] == sid) and self.visible(o['storage'])
                and (parent == 0 or o['parent'] == (0 if parent == 0xFFFFFFFF else parent))]

    def delete(self, handle):
        for child in [h for h, o in self.objects.items() if o['parent'] == handle]:
            self.delete(child)
        self.objects.pop(handle, None)


# ---------------------------------------------------------------- the model

class Model:
    def __init__(self, send, trace, profile='android', speed='high', now=time.monotonic):
        self.sendraw, self.trace, self.now = send, trace, now
        self.profile_name = profile
        self.p = PROFILES[profile]
        self.speed = speed
        self.mps = {'super': 1024, 'high': 512}.get(speed, 64)
        self.store = Store(profile)
        self.pid = self.p['pid']
        self.device = struct.pack('<BBHBBBBHHHBBBB', 18, 1,
                                  {'super': 0x300, 'high': 0x200}.get(speed, 0x110), 0, 0, 0,
                                  9 if speed == 'super' else 64, 0xFFFF, self.pid, 0x0100, 1, 2, 3, 1)
        # BOS (USB 3.2 9.6.2): a USB 2.0 extension and a SuperSpeed device capability.
        self.bos = (struct.pack('<BBHB', 5, 15, 22, 2) + struct.pack('<BBBI', 7, 16, 2, 0x2) +
                    struct.pack('<BBBBHBBH', 10, 16, 3, 0, 0x000E, 2, 10, 32)) if speed == 'super' else None
        self.config_desc = self.configuration()
        self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('usb-wdm-free SIMULATED'),
                        2: usb_string('MTP %s - SIMULATED' % profile), 3: usb_string('SIM%04X' % self.pid),
                        4: usb_string('MTP'), 5: usb_string('ADB Interface')}
        if self.p['mtp']:
            # Microsoft OS string descriptor: "MSFT100", vendor code 0x01.
            raw = 'MSFT100'.encode('utf-16-le')
            self.strings[0xEE] = bytes((18, 3)) + raw + bytes((0x01, 0))
        self.peer_caps = 0
        self.verbose = False
        self.config = 0
        self.interrupt = False
        self.faults = {}
        self.delay = 0.0
        self.totals = collections.Counter()
        self.held_controls = []
        self.held_outs = []
        self.reset_protocol()

    # -- descriptors ----------------------------------------------------------
    def configuration(self):
        bulk = self.mps
        interval = 4 if self.speed == 'full' else 6
        c, s, pr = self.p['cls']

        def comp(per_interval):
            # SuperSpeed endpoint companion (USB 3.2 9.6.7): no burst, no streams.
            return struct.pack('<BBBBH', 6, 0x30, 0, 0, per_interval) if self.speed == 'super' else b''
        body = bytes((9, 4, 0, 0, 3, c, s, pr, 4))
        body += bytes((7, 5, EP_IN, 2)) + struct.pack('<HB', bulk, 0) + comp(0)
        body += bytes((7, 5, EP_OUT, 2)) + struct.pack('<HB', bulk, 0) + comp(0)
        body += bytes((7, 5, EP_INT, 3)) + struct.pack('<HB', 28, interval) + comp(28)
        count = 1
        if self.p['adb']:
            body += bytes((9, 4, 1, 0, 2, 0xFF, 0x42, 1, 5))
            body += bytes((7, 5, ADB_IN, 2)) + struct.pack('<HB', bulk, 0) + comp(0)
            body += bytes((7, 5, ADB_OUT, 2)) + struct.pack('<HB', bulk, 0) + comp(0)
            count = 2
        return bytes((9, 2)) + struct.pack('<HBBBBB', 9 + len(body), count, 1, 0, 0x80, 250) + body

    def ext_compat_id(self):
        # Header: dwLength, bcdVersion 0x0100, wIndex 4, bCount, 7 reserved;
        # one function: bFirstInterfaceNumber 0, reserved, "MTP", zeros.
        func = bytes((0, 1)) + b'MTP\0\0\0\0\0' + bytes(8) + bytes(6)
        return struct.pack('<IHHB', 16 + len(func), 0x0100, 4, 1) + bytes(7) + func

    # -- plumbing ---------------------------------------------------------------
    def log(self, event, **fields):
        if self.trace:
            self.trace.write(json.dumps(dict(time=time.time(), event=event, simulated=True, **fields)) + '\n')
            self.trace.flush()

    @property
    def id64(self):
        return bool(self.peer_caps & CAPS & (1 << CAP_ID64))

    @property
    def len32(self):
        return bool(self.peer_caps & CAPS & (1 << CAP_BULK32))

    def send(self, kind, ident=0, data=b''):
        self.sendraw(struct.pack('<IIQ' if self.id64 else '<III', kind, len(data), ident) + data)

    def bulk_header(self, endpoint, status, length):
        header = struct.pack('<BBHI', endpoint, status, length & 0xffff, 0)
        return header + struct.pack('<H', length >> 16) if self.len32 else header

    def info(self):
        types, intervals, interfaces = bytearray([255] * 32), bytearray(32), bytearray(32)
        packets = [0] * 32
        types[0] = types[16] = 0
        packets[0] = packets[16] = 512 if self.speed == 'super' else 64
        if self.config:
            eps = [(16 + (EP_IN & 15), 2, 0, self.mps, 0), (EP_OUT, 2, 0, self.mps, 0),
                   (16 + (EP_INT & 15), 3, 4 if self.speed == 'full' else 6, 28, 0)]
            if self.p['adb']:
                eps += [(16 + (ADB_IN & 15), 2, 0, self.mps, 1), (ADB_OUT, 2, 0, self.mps, 1)]
            for slot, kind, interval, size, iface in eps:
                types[slot], intervals[slot], packets[slot], interfaces[slot] = kind, interval, size, iface
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << CAP_EP_MAX):
            data += struct.pack('<32H', *packets)
        self.send(EP_INFO, data=data)
        numbers, classes, subclasses, protocols = (bytearray(32) for _ in range(4))
        count = 0
        if self.config:
            c, s, pr = self.p['cls']
            numbers[0], classes[0], subclasses[0], protocols[0] = 0, c, s, pr
            count = 1
            if self.p['adb']:
                numbers[1], classes[1], subclasses[1], protocols[1] = 1, 0xFF, 0x42, 1
                count = 2
        self.send(IFACE_INFO, data=struct.pack('<I', count) + numbers + classes + subclasses + protocols)

    # -- packets of the MTP function -------------------------------------------------
    def reset_protocol(self):
        self.session = 0
        self.inq = collections.deque()          # bulk IN packets queued (b'' = zero-length packet)
        self.pending_in = collections.OrderedDict()   # ident -> [length, bytearray]
        self.out_buf = bytearray()
        self.out_container = None               # header of the data container being received
        self.out_split = False                  # split form seen on the current OUT data phase
        self.cmd = None                         # the command awaiting its data phase
        self.halted = set()
        self.status_code = OK
        self.status_eps = []
        self.busy_left = 0
        self.last_tid = 0
        self.sent_object = None                 # handle created by SendObjectInfo
        self.stall_after = False                # device cancel once the queued packets went
        self.events = collections.deque()

    def packets(self, container, data_phase):
        """Split a container into USB packets per the profile's form."""
        out = []
        if data_phase and self.p['split'] and len(container) > 12:
            out.append(container[:12])
            body = container[12:]
        else:
            body = container
        for i in range(0, len(body), self.mps):
            out.append(body[i:i + self.mps])
        if body and len(body) % self.mps == 0:
            if self.p['zlp'] and not self.faults.pop('response_for_zlp', False):
                out.append(b'')
        if not body:
            out.append(b'')
        return out

    def queue_in(self, container, data_phase=False):
        self.inq.extend(self.packets(container, data_phase))
        self.drain()

    def container(self, ctype, code, tid, payload=b''):
        return struct.pack('<IHHI', 12 + len(payload), ctype, code, tid) + payload

    def respond(self, code, tid, *params):
        if self.faults.pop('no_response', False):
            self.log('fault_no_response', code='%04x' % code, tid=tid)
            return
        if self.faults.pop('stale_response', False):
            self.queue_in(self.container(T_RESP, OK, max(0, tid - 1)))
        self.queue_in(self.container(T_RESP, code, tid, b''.join(u32(p) for p in params)))
        self.log('response', code='%04x' % code, tid=tid, params=list(params))

    def data_in(self, code, tid, payload):
        if self.faults.pop('short_data', False) and len(payload) > self.mps * 2:
            full = self.container(T_DATA, code, tid, payload)
            cut = self.mps + 7
            self.inq.extend([full[:self.mps], full[self.mps:cut]])
            self.log('fault_short_data', tid=tid, sent=cut, advertised=len(full))
            self.drain()
            return False
        if self.faults.pop('stall_data', False):
            # The first packet goes out, then the device halts both bulk
            # endpoints (still image 7.2.1.1, device-initiated cancel).
            full = self.container(T_DATA, code, tid, payload)
            self.inq.append(full[:self.mps])
            self.stall_after = True
            self.drain()
            return False
        self.queue_in(self.container(T_DATA, code, tid, payload), data_phase=True)
        return True

    def device_cancel(self):
        self.halted = {EP_IN, EP_OUT}
        self.status_code, self.status_eps = TRANSACTION_CANCELLED, [EP_IN, EP_OUT]
        self.inq.clear()
        for ident, (length, acc) in list(self.pending_in.items()):
            # A transfer that had taken packets ends with them and the STALL.
            self.send(BULK, ident, self.bulk_header(EP_IN, ST_STALL, len(acc)) + bytes(acc))
        self.pending_in.clear()
        self.cmd = None
        self.log('device_cancel')

    def drain(self):
        """Answer pending bulk IN transfers from the packet queue: a transfer
        ends on a short or zero-length packet or when it is full; packets
        that do not end it stay with the transfer until more arrive."""
        while self.pending_in and self.inq:
            ident, (length, acc) = next(iter(self.pending_in.items()))
            done = False
            while self.inq and not done:
                pkt = self.inq[0]
                if len(acc) + len(pkt) > length:
                    break
                self.inq.popleft()
                acc += pkt
                done = len(pkt) < self.mps or len(acc) == length
            if not done and len(acc) < length and self.inq:
                done = True          # the next packet would overflow: end here
            if not done:
                break
            del self.pending_in[ident]
            if self.delay:
                time.sleep(self.delay)
            self.send(BULK, ident, self.bulk_header(EP_IN, ST_OK, len(acc)) + bytes(acc))
            self.totals['bytes_in'] += len(acc)
        if self.stall_after and not self.inq:
            self.stall_after = False
            self.device_cancel()

    # -- the protocol ------------------------------------------------------------
    def ops(self):
        return MTP_OPS if self.p['mtp'] else PTP_OPS

    def device_info(self):
        mtp = self.p['mtp']
        ext = 'microsoft.com: 1.0; android.com: 1.0;' if mtp else ''
        events = [E_CANCEL, E_ADDED, E_REMOVED, E_STORE_ADDED, E_STORE_REMOVED, E_INFO_CHANGED, E_DEVICE_RESET]
        return (u16(100) + u32(6 if mtp else 0) + u16(100 if mtp else 0) + pstr(ext) + u16(0)
                + arr16(self.ops()) + arr16(events) + arr16([]) + arr16([])
                + arr16([F_UNDEFINED, F_ASSOCIATION, F_JPEG])
                + pstr('usb-wdm-free SIMULATED') + pstr('MTP %s SIMULATED' % self.profile_name)
                + pstr('0.1') + pstr('SIM%04X' % self.pid))

    def storage_info(self, sid):
        s = self.store.storages[sid]
        used = sum(len(o['data']) for o in self.store.objects.values() if o['storage'] == sid)
        return (u16(s['kind']) + u16(s['fs']) + u16(s['access']) + u64(s['capacity'])
                + u64(s['capacity'] - used) + u32(0xFFFFFFFF) + pstr(s['desc']) + pstr(s['label']))

    def object_info(self, h):
        o = self.store.objects[h]
        size = len(o['data'])
        folder = o['format'] == F_ASSOCIATION
        return (u32(o['storage']) + u16(o['format']) + u16(0) + u32(min(size, 0xFFFFFFFF))
                + u16(0) + u32(0) * 6 + u32(o['parent']) + u16(1 if folder else 0) + u32(0) + u32(0)
                + pstr(o['name']) + pstr(mtp_date(o['mtime'])) + pstr(mtp_date(o['mtime'])) + pstr(''))

    def prop_value(self, h, prop):
        o = self.store.objects[h]
        return {P_STORAGE: u32(o['storage']), P_FORMAT: u16(o['format']), P_SIZE: u64(len(o['data'])),
                P_NAME: pstr(o['name']), P_MODIFIED: pstr(mtp_date(o['mtime'])),
                P_PARENT: u32(o['parent'])}[prop]

    def command(self, code, tid, params):
        name = OPS.get(code, '%04x' % code)
        self.log('command', op=name, tid=tid, params=list(params))
        self.totals['commands'] += 1
        self.last_tid = tid
        p = list(params) + [0] * 5
        if code not in self.ops():
            return self.respond(OP_NOT_SUPPORTED, tid)
        if code == 0x1001:
            if self.data_in(code, tid, self.device_info()):
                self.respond(OK, tid)
            return
        if code == 0x1002:
            if p[0] == 0:
                return self.respond(INVALID_PARAMETER, tid)
            if self.session:
                return self.respond(SESSION_OPEN, tid, self.session)
            self.session = p[0]
            return self.respond(OK, tid)
        if not self.session:
            return self.respond(SESSION_NOT_OPEN, tid)
        st = self.store
        if code == 0x1003:
            self.session = 0
            return self.respond(OK, tid)
        if code == 0x1004:
            ids = [sid for sid in st.storages if st.visible(sid)]
            if self.data_in(code, tid, arr32(ids)):
                self.respond(OK, tid)
            return
        if code == 0x1005:
            if not st.visible(p[0]):
                return self.respond(INVALID_STORAGE, tid)
            if self.data_in(code, tid, self.storage_info(p[0])):
                self.respond(OK, tid)
            return
        if code in (0x1006, 0x1007):
            sid, parent = p[0], p[2]
            if sid != 0xFFFFFFFF and not st.visible(sid):
                return self.respond(INVALID_STORAGE, tid)
            if not self.p['mtp'] and parent not in (0,):
                return self.respond(PARAM_NOT_SUPPORTED, tid)
            if parent not in (0, 0xFFFFFFFF) and parent not in st.objects:
                return self.respond(INVALID_PARENT, tid)
            handles = st.children(sid, parent)
            if code == 0x1006:
                return self.respond(OK, tid, len(handles))
            if self.data_in(code, tid, arr32(handles)):
                self.respond(OK, tid)
            return
        if code in (0x1008, 0x1009, 0x101B, 0x100B, 0x9803):
            if p[0] not in st.objects or not st.visible(st.objects[p[0]]['storage']):
                return self.respond(INVALID_HANDLE, tid)
        if code == 0x1008:
            if self.data_in(code, tid, self.object_info(p[0])):
                self.respond(OK, tid)
            return
        if code == 0x1009:
            if self.data_in(code, tid, st.objects[p[0]]['data']):
                self.respond(OK, tid)
            return
        if code == 0x101B:
            data = st.objects[p[0]]['data'][p[1]:p[1] + p[2]]
            if self.data_in(code, tid, data):
                self.respond(OK, tid, len(data))
            return
        if code == 0x100B:
            if st.storages[st.objects[p[0]]['storage']]['access'] == 1:
                return self.respond(STORE_RO, tid)
            st.delete(p[0])
            return self.respond(OK, tid)
        if code == 0x9801:
            if self.data_in(code, tid, arr16(sorted(PROP_TYPES))):
                self.respond(OK, tid)
            return
        if code == 0x9802:
            prop = p[0]
            if prop not in PROP_TYPES:
                return self.respond(INVALID_PROP_CODE, tid)
            dt = PROP_TYPES[prop]
            default = {DT_UINT16: u16(0), DT_UINT32: u32(0), DT_UINT64: u64(0), DT_STR: pstr('')}[dt]
            desc = u16(prop) + u16(dt) + u8(1 if prop == P_NAME else 0) + default + u32(0) + u8(0)
            if self.data_in(code, tid, desc):
                self.respond(OK, tid)
            return
        if code == 0x9803:
            if p[1] not in PROP_TYPES:
                return self.respond(INVALID_PROP_CODE, tid)
            if self.data_in(code, tid, self.prop_value(p[0], p[1])):
                self.respond(OK, tid)
            return
        if code == 0x9805:
            handle, prop, depth = p[0], p[2], p[4]
            if prop not in PROP_TYPES and prop != 0xFFFFFFFF:
                return self.respond(INVALID_PROP_CODE, tid)
            if handle == 0:
                objs = [] if depth == 0 else st.children(0xFFFFFFFF, 0xFFFFFFFF)
            elif handle == 0xFFFFFFFF:
                objs = st.children(0xFFFFFFFF, 0)
            elif handle in st.objects:
                objs = [handle] + (st.children(0xFFFFFFFF, handle) if depth >= 1 else [])
            else:
                return self.respond(INVALID_HANDLE, tid)
            props = sorted(PROP_TYPES) if prop == 0xFFFFFFFF else [prop]
            elems = [u32(h) + u16(pc) + u16(PROP_TYPES[pc]) + self.prop_value(h, pc) for h in objs for pc in props]
            if self.data_in(code, tid, u32(len(elems)) + b''.join(elems)):
                self.respond(OK, tid)
            return
        if code in OUT_DATA_OPS:
            self.cmd = (code, tid, p)          # the data phase from the host follows
            return
        return self.respond(OP_NOT_SUPPORTED, tid)

    def command_data(self, code, tid, p, payload):
        st = self.store
        if code == 0x100C:
            sid, parent = p[0] or 0x00010001, p[1]
            if not st.visible(sid):
                return self.respond(INVALID_STORAGE, tid)
            if st.storages[sid]['access']:
                return self.respond(STORE_RO, tid)
            if parent == 0xFFFFFFFF:
                parent = 0
            if parent and parent not in st.objects:
                return self.respond(INVALID_PARENT, tid)
            try:
                r = Reader(payload)
                r.u32()
                fmt = r.u16()
                r.u16()
                size = r.u32()
                r.take(2 + 4 * 6)
                r.u32()
                r.u16()
                r.u32()
                r.u32()
                name = r.str()
            except ValueError as e:
                self.log('bad_object_info', error=str(e))
                return self.respond(GENERAL, tid)
            h = st.add(sid, parent, name, None if fmt == F_ASSOCIATION else b'', time.time(), fmt)
            self.sent_object = None if fmt == F_ASSOCIATION else (h, size)
            self.queue_event(E_ADDED, h)
            return self.respond(OK, tid, sid, parent or 0xFFFFFFFF, h)
        if code == 0x100D:
            if not self.sent_object:
                return self.respond(NO_VALID_OBJECTINFO, tid)
            h, size = self.sent_object
            st.objects[h]['data'] = bytes(payload)
            self.sent_object = None
            self.log('object_received', handle=h, length=len(payload), crc32='%08x' % zlib.crc32(payload))
            return self.respond(OK, tid)
        if code == 0x9804:
            if p[0] not in st.objects:
                return self.respond(INVALID_HANDLE, tid)
            if p[1] != P_NAME:
                return self.respond(INVALID_PROP_CODE, tid)
            try:
                st.objects[p[0]]['name'] = Reader(payload).str()
            except ValueError:
                return self.respond(INVALID_PARAMETER, tid)
            self.queue_event(E_INFO_CHANGED, p[0])
            return self.respond(OK, tid)

    def bulk_out(self, data):
        """Bytes of one bulk OUT transfer: a command, or (part of) a data phase."""
        self.out_buf += data
        short = len(data) % self.mps != 0 or not data
        while True:
            if self.out_container is None:
                if len(self.out_buf) < 12:
                    return
                length, ctype, code, tid = struct.unpack('<IHHI', self.out_buf[:12])
                if ctype == T_CMD:
                    if len(self.out_buf) < length:
                        return
                    params = struct.unpack('<%dI' % ((length - 12) // 4), self.out_buf[12:length])
                    del self.out_buf[:length]
                    self.command(code, tid, params)
                    continue
                if ctype == T_DATA:
                    self.out_container = (length, code, tid)
                    self.out_split = len(data) == 12 and length > 12 and len(self.out_buf) == 12
                    continue
                self.log('bulk_out_unexpected', type=ctype, length=len(self.out_buf))
                self.out_buf.clear()
                return
            length, code, tid = self.out_container
            if length != 0xFFFFFFFF:
                if len(self.out_buf) < length:
                    return
                payload = bytes(self.out_buf[12:length])
                del self.out_buf[:length]
            else:
                if not short:
                    return
                payload = bytes(self.out_buf[12:])
                self.out_buf.clear()
            self.out_container = None
            self.totals['bytes_out'] += len(payload)
            if self.cmd and self.cmd[0] == code and self.cmd[1] == tid:
                c, t, p = self.cmd
                self.cmd = None
                self.log('data_out', op=OPS.get(c), tid=t, length=len(payload), split=self.out_split)
                self.command_data(c, t, p, payload)
            else:
                self.log('data_out_unexpected', code='%04x' % code, tid=tid)
            if not self.out_buf:
                return

    # -- events ------------------------------------------------------------------
    def queue_event(self, code, *params, tid=0xFFFFFFFF):
        self.events.append(self.container(T_EVENT, code, tid, b''.join(u32(x) for x in params)))
        self.flush_events()

    def flush_events(self):
        while self.interrupt and self.events:
            ev = self.events.popleft()
            self.send(INTERRUPT, data=struct.pack('<BBH', EP_INT, 0, len(ev)) + ev)
            self.totals['events'] += 1

    # -- class requests and control ---------------------------------------------------
    def device_status(self):
        kind = self.faults.pop('bad_status', None)
        if self.busy_left:
            self.busy_left -= 1
            code, eps = DEVICE_BUSY, []
        else:
            code, eps = self.status_code, [ep for ep in self.status_eps if ep in self.halted]
            if code == TRANSACTION_CANCELLED and not eps:
                code = OK
                self.status_code = OK
        body = u16(code) + b''.join(u32(ep) for ep in eps)
        data = u16(2 + len(body)) + body
        if kind == 'length':
            data = u16(len(data) + 4) + data[2:]
        elif kind == 'code':
            data = u16(4) + u16(0x2002)
        elif kind == 'endpoint':
            data = u16(8) + u16(TRANSACTION_CANCELLED) + u32(0x85)
        elif kind == 'repeat':
            data = u16(12) + u16(TRANSACTION_CANCELLED) + u32(EP_IN) + u32(EP_IN)
        self.log('device_status', status=data.hex(), fault=kind)
        return data

    def class_request(self, rt, request, value, index, length, payload):
        """Still image class requests (5.2, wIndex the interface: errata 2007)."""
        if index & 0xff != 0:
            return ST_STALL, b''
        if rt == 0x21 and request == 0x64 and len(payload) == 6:
            code, tid = struct.unpack('<HI', payload)
            self.log('cancel_request', code='%04x' % code, tid=tid)
            if code != E_CANCEL:
                return ST_STALL, b''
            self.inq.clear()
            for ident in list(self.pending_in):
                self.send(BULK, ident, self.bulk_header(EP_IN, ST_CANCELLED, 0))
            self.pending_in.clear()
            self.out_buf.clear()
            self.out_container = None
            self.cmd = None
            if self.faults.pop('ignore_cancel', False):
                self.busy_left = 1 << 30
            else:
                self.busy_left = max(self.busy_left, 1)
            return ST_OK, b''
        if rt == 0xA1 and request == 0x67:
            return ST_OK, self.device_status()[:length]
        if rt == 0x21 and request == 0x66:
            self.log('device_reset_request')
            self.reset_protocol()
            return ST_OK, b''
        return ST_STALL, b''           # 0x65 Get Extended Event Data: not supported

    def control(self, ident, packet):
        if len(packet) < 10:
            raise ValueError('short control header')
        ep, request, rt, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload = packet[10:]
        if len(payload) != (0 if rt & 128 else length):
            raise ValueError('control length mismatch')
        status, data = ST_STALL, b''
        if rt & 0x60 == 0x20 and rt & 0x1f == 1:
            if self.faults.pop('ep0_hang', False):
                self.log('fault_ep0_hang', request='%02x' % request)
                self.held_controls.append(ident)
                return
            status, data = self.class_request(rt, request, value, index, length, payload)
        elif rt == 0x80 and request == 6:
            kind, number = value >> 8, value & 255
            data = self.device if kind == 1 and not number else \
                self.config_desc if kind == 2 and not number else \
                self.strings.get(number) if kind == 3 else \
                self.bos if kind == 15 and not number else None
            status, data = (ST_OK, data) if data is not None else (ST_STALL, b'')
            if kind == 3 and number == 0xEE:
                self.log('os_string_read', present=data is not None)
        elif rt == 0xC0 and request == 0x01 and index == 4 and self.p['mtp']:
            status, data = ST_OK, self.ext_compat_id()
            self.log('ext_compat_id_read')
        elif request == 0 and rt in (0x80, 0x81, 0x82):
            status = ST_OK
            data = b'\1\0' if rt == 0x82 and (index & 0xff) in self.halted else b'\0\0'
        elif request == 8 and rt == 0x80:
            status, data = ST_OK, bytes((self.config,))
        elif request == 10 and rt == 0x81:
            status, data = ST_OK, b'\0'
        elif request == 1 and rt == 2 and value == 0:          # CLEAR_FEATURE(ENDPOINT_HALT)
            status = ST_OK
            self.halted.discard(index & 0xff)
            self.log('clear_halt', endpoint='%02x' % (index & 0xff))
        elif request == 3 and rt == 2 and value == 0:          # SET_FEATURE(ENDPOINT_HALT)
            status = ST_OK
            self.halted.add(index & 0xff)
        data = data[:length]
        actual = len(data) if rt & 128 else (0 if status else length)
        self.send(CONTROL, ident, struct.pack('<BBBBHHH', ep, request, rt, status, value, index, actual) + data)

    def handle(self, kind, ident, packet):
        if self.verbose and kind != CONTROL:
            self.log('usbredir', kind=kind, id=ident, head=packet[:12].hex())
        if kind == RESET:
            for i in list(self.pending_in):
                self.send(BULK, i, self.bulk_header(EP_IN, ST_CANCELLED, 0))
            for i in self.held_outs:
                self.send(BULK, i, self.bulk_header(EP_OUT, ST_CANCELLED, 0))
            self.held_outs = []
            self.reset_protocol()
            self.config, self.interrupt = 0, False
            self.log('reset')
        elif kind in (SET_CONFIG, GET_CONFIG):
            status = 0
            if kind == SET_CONFIG:
                status = 0 if packet[0] in (0, 1) else 4
                if not status:
                    self.config = packet[0]
                    self.info()
                    self.log('configuration', value=self.config)
            self.send(CONFIG_STATUS, ident, bytes((status, self.config)))
        elif kind in (SET_ALT, GET_ALT):
            ok = self.config and packet[0] in ((0, 1) if self.p['adb'] else (0,)) and (kind == GET_ALT or packet[1] == 0)
            self.send(ALT_STATUS, ident, bytes((0 if ok else 4, packet[0], 0)))
        elif kind in (START_INT, STOP_INT):
            ok = packet[0] == EP_INT and self.config
            self.interrupt = kind == START_INT and bool(ok)
            self.send(INT_STATUS, ident, bytes((0 if ok else 4, packet[0])))
            self.flush_events()
        elif kind == CANCEL:
            self.log('usbredir_cancel', id=ident, pending=ident in self.pending_in)
            if ident in self.pending_in:
                length, acc = self.pending_in.pop(ident)
                self.send(BULK, ident, self.bulk_header(EP_IN, ST_CANCELLED, len(acc)) + bytes(acc))
            if ident in self.held_controls:
                self.held_controls.remove(ident)
            if ident in self.held_outs:
                self.held_outs.remove(ident)
                self.send(BULK, ident, self.bulk_header(EP_OUT, ST_CANCELLED, 0))
        elif kind == 24:
            pass
        elif kind == CONTROL:
            self.control(ident, packet)
        elif kind == BULK:
            size = 10 if self.len32 else 8
            endpoint, _, length, _stream = struct.unpack('<BBHI', packet[:8])
            if self.len32:
                length |= struct.unpack('<H', packet[8:10])[0] << 16
            if not self.config or endpoint not in (EP_IN, EP_OUT, ADB_IN, ADB_OUT):
                self.send(BULK, ident, self.bulk_header(endpoint, ST_STALL, 0))
            elif endpoint in (ADB_IN, ADB_OUT):
                if endpoint == ADB_OUT:          # ADB-shaped function: accept and ignore
                    self.send(BULK, ident, self.bulk_header(endpoint, ST_OK, length))
                # ADB IN requests are left pending, as an idle function's
            elif endpoint in self.halted:
                self.send(BULK, ident, self.bulk_header(endpoint, ST_STALL, 0))
            elif endpoint == EP_OUT and self.faults.pop('out_hang', False):
                self.log('fault_out_hang', length=length)
                self.held_outs.append(ident)
            elif endpoint == EP_OUT:
                data = packet[size:]
                self.send(BULK, ident, self.bulk_header(EP_OUT, ST_OK, len(data)))
                self.bulk_out(data)
            else:
                self.pending_in[ident] = [length, bytearray()]
                self.drain()
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    def connect(self):
        self.info()
        # usbredir speeds: 1 full, 2 high, 3 super
        data = struct.pack('<BBBBHH', {'super': 3, 'high': 2}.get(self.speed, 1), 0, 0, 0, 0xFFFF, self.pid)
        if self.peer_caps & (1 << CAP_VERSION):
            data += struct.pack('<H', 0x0100)
        self.send(CONNECT, data=data)
        self.log('connected', model='SIMULATED MTP device', profile=self.profile_name, speed=self.speed,
                 pid='%04x' % self.pid)

    # -- the control socket ------------------------------------------------------------
    def ctl(self, msg):
        if 'fault' in msg:
            f = msg['fault']
            if f == 'busy':
                self.busy_left = int(msg.get('n', 1))
            elif f == 'bad_status':
                self.faults['bad_status'] = msg.get('kind', 'length')
            elif f == 'reset_event':
                self.queue_event(E_DEVICE_RESET)
                self.session = 0
            elif f == 'cancel_event':
                self.queue_event(E_CANCEL, tid=self.last_tid)
            else:
                self.faults[f] = True
            self.log('fault_armed', fault=f)
            return dict(ok=True, fault=f)
        if 'flood' in msg:
            for i in range(int(msg['flood'])):
                self.queue_event(E_ADDED, 0x7000 + i)
            return dict(ok=True, events=int(msg['flood']))
        if 'unlock' in msg:
            for sid, s in self.store.storages.items():
                if s['hidden']:
                    s['hidden'] = False
                    self.store.populate(sid, time.time())
                    self.queue_event(E_STORE_ADDED, sid)
            return dict(ok=True)
        if 'delay_ms' in msg:
            self.delay = float(msg['delay_ms']) / 1000.0
            return dict(ok=True)
        return dict(ok=False, error='unknown command')


def parse_frames(model, buffer, hello):
    """Take whole usbredir messages from buffer; returns hello state."""
    while True:
        size = 16 if model.id64 else 12
        if len(buffer) < size:
            return hello
        kind, length, ident = struct.unpack('<IIQ' if model.id64 else '<III', buffer[:size])
        if len(buffer) < size + length:
            return hello
        packet = bytes(buffer[size:size + length])
        del buffer[:size + length]
        if not hello:
            model.peer_caps = int.from_bytes(packet[64:68], 'little') if length >= 68 else 0
            model.connect()
            hello = True
        else:
            model.handle(kind, ident, packet)


def serve(args):
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        ctl_server = None
        if args.ctl:
            ctl_server = socket.socket()
            ctl_server.bind(('127.0.0.1', args.ctl))
            ctl_server.listen(4)
        print(f'SIMULATED MTP device ({args.profile}) listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        model = Model(connection.sendall, trace, args.profile, args.speed)
        model.verbose = args.verbose
        model.sendraw(struct.pack('<III', HELLO, 68, 0) +
                      b'usb-wdm-free MTP SIMULATED'.ljust(64, b'\0') + struct.pack('<I', CAPS))
        buffer, hello, clients = bytearray(), False, {}
        try:
            while True:
                socks = [connection] + ([ctl_server] if ctl_server else []) + list(clients)
                ready, _, _ = select.select(socks, [], [], 1.0)
                for s in ready:
                    if s is connection:
                        data = connection.recv(1 << 20)
                        if not data:
                            raise EOFError
                        buffer.extend(data)
                        hello = parse_frames(model, buffer, hello)
                    elif s is ctl_server:
                        c, _ = ctl_server.accept()
                        clients[c] = bytearray()
                    else:
                        data = s.recv(4096)
                        if not data:
                            s.close()
                            del clients[s]
                            continue
                        clients[s].extend(data)
                        while b'\n' in clients[s]:
                            line, _, rest = bytes(clients[s]).partition(b'\n')
                            clients[s] = bytearray(rest)
                            msg = json.loads(line)
                            if msg.get('quit'):
                                s.sendall(b'{"ok": true}\n')
                                raise EOFError
                            s.sendall((json.dumps(model.ctl(msg)) + '\n').encode())
        except (EOFError, ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
            model.log('disconnected', **model.totals)
        finally:
            connection.close()
            if ctl_server:
                ctl_server.close()


# ---------------------------------------------------------------- self-test

class FakeHost:
    """A scripted usbredir host for --selftest: sends messages to the model
    and collects its answers by id."""

    def __init__(self, profile, speed='high'):
        self.out = []
        self.model = Model(self.sink, None, profile, speed)
        self.model.peer_caps = CAPS
        self.next_id = 1
        self.answers = {}

    def sink(self, raw):
        kind, length, ident = struct.unpack('<IIQ', raw[:16])
        self.out.append((kind, ident, raw[16:16 + length]))
        self.answers[ident] = (kind, raw[16:16 + length])

    def msg(self, kind, packet):
        ident = self.next_id
        self.next_id += 1
        self.model.handle(kind, ident, packet)
        return ident

    def control(self, rt, request, value, index, length, data=b''):
        i = self.msg(CONTROL, struct.pack('<BBBBHHH', 0, request, rt, 0, value, index, length) + data)
        kind, pkt = self.answers[i]
        return pkt[3], pkt[10:]

    def bulk_out(self, data):
        self.msg(BULK, struct.pack('<BBHIH', EP_OUT, 0, len(data) & 0xffff, 0, len(data) >> 16) + data)

    def bulk_in(self, length):
        i = self.msg(BULK, struct.pack('<BBHIH', EP_IN, 0, length & 0xffff, 0, length >> 16))
        if i not in self.answers:
            return None
        kind, pkt = self.answers.pop(i)
        return pkt[0:2], pkt[10:]

    def read_container(self, length=None):
        """One read as the library makes it: the first read of a phase is one
        packet, later ones what is left, rounded up to whole packets."""
        status, data = self.bulk_in(length or self.model.mps)
        return data

    def transaction(self, code, tid, *params, out=None):
        self.bulk_out(struct.pack('<IHHI', 12 + 4 * len(params), T_CMD, code, tid)
                      + b''.join(u32(p) for p in params))
        if out is not None:
            self.bulk_out(struct.pack('<IHHI', 12 + len(out), T_DATA, code, tid) + out)
        first = self.read_container()
        length, ctype = struct.unpack('<IH', first[:6])
        data = None
        if ctype == T_DATA:
            mps = self.model.mps
            data = bytearray(first)
            split = len(first) == 12 and length > 12
            while len(data) < length:
                left = length - len(data)
                data += self.read_container((left + mps - 1) // mps * mps)
            run = length - 12 if split else length
            resp = self.read_container()
            if run % mps == 0 and run and self.model.p['zlp']:
                assert resp == b'', 'zlp expected'
                resp = self.read_container()
            data = bytes(data[12:])
        else:
            resp = first
        rl, rt, rc, rtid = struct.unpack('<IHHI', resp[:12])
        assert rt == T_RESP and rtid == tid, (rt, rtid, tid)
        return rc, data, struct.unpack('<%dI' % ((rl - 12) // 4), resp[12:rl])


def selftest():
    host = FakeHost('android')
    m = host.model
    st, dev = host.control(0x80, 6, 0x0100, 0, 18)
    assert st == 0 and dev[:2] == b'\x12\x01'
    st, cfg = host.control(0x80, 6, 0x0200, 0, 255)
    assert st == 0 and cfg[4] == 2, 'composite: two interfaces'
    st, os_str = host.control(0x80, 6, 0x03EE, 0, 255)
    assert st == 0 and os_str[2:16].decode('utf-16-le') == 'MSFT100'
    host.msg(SET_CONFIG, b'\x01')
    rc, info, _ = host.transaction(0x1001, 0)
    assert rc == OK and b'm\0i\0c\0r\0o\0s\0o\0f\0t' in info
    rc, _, _ = host.transaction(0x1002, 0, 1)
    assert rc == OK
    rc, _, prm = host.transaction(0x1002, 1, 1)
    assert rc == SESSION_OPEN and prm == (1,)
    rc, ids, _ = host.transaction(0x1004, 2)
    assert rc == OK and struct.unpack('<I', ids[:4])[0] == 2
    rc, hs, _ = host.transaction(0x1007, 3, 0x00010001, 0, 0xFFFFFFFF)
    roots = struct.unpack('<%dI' % struct.unpack('<I', hs[:4])[0], hs[4:])
    assert rc == OK and len(roots) == 3
    # GetObjectPropList on a folder includes the folder itself (depth counts it).
    rc, pl, _ = host.transaction(0x9805, 4, roots[0], 0, 0xFFFFFFFF, 0, 1)
    assert rc == OK and struct.unpack('<I', pl[:4])[0] == 6 * 2
    # The boundary object: payload 500, container 512 = one High Speed packet, then a ZLP.
    exact = [h for h, o in m.store.objects.items() if o['name'] == 'exact512.bin'][0]
    rc, data, _ = host.transaction(0x1009, 5, exact)
    assert rc == OK and data == m.store.objects[exact]['data']
    # SendObjectInfo + SendObject.
    oi = (u32(0x00010001) + u16(F_UNDEFINED) + u16(0) + u32(5) + u16(0) + u32(0) * 6 + u32(0)
          + u16(0) + u32(0) + u32(0) + pstr('new.txt') + pstr('') + pstr('') + pstr(''))
    rc, _, prm = host.transaction(0x100C, 6, 0x00010001, 0xFFFFFFFF, out=oi)
    assert rc == OK and len(prm) == 3
    rc, _, _ = host.transaction(0x100D, 7, out=b'hello')
    assert rc == OK and m.store.objects[prm[2]]['data'] == b'hello'
    # Rename.
    rc, _, _ = host.transaction(0x9804, 8, prm[2], P_NAME, out=pstr('renamed.txt'))
    assert rc == OK and m.store.objects[prm[2]]['name'] == 'renamed.txt'
    # Host cancel: Cancel Request, then status busy once, then OK.
    st, _ = host.control(0x21, 0x64, 0, 0, 6, struct.pack('<HI', E_CANCEL, 9))
    assert st == 0
    st, s1 = host.control(0xA1, 0x67, 0, 0, 64)
    st, s2 = host.control(0xA1, 0x67, 0, 0, 64)
    assert s1 == u16(4) + u16(DEVICE_BUSY) and s2 == u16(4) + u16(OK), (s1, s2)
    # Device-initiated cancel in a data phase.
    m.ctl(dict(fault='stall_data'))
    big = [h for h, o in m.store.objects.items() if o['name'].startswith('IMG_')][2]
    host.bulk_out(struct.pack('<IHHII', 16, T_CMD, 0x1009, 10, big))
    status, first = host.bulk_in(4096)
    assert len(first) == m.mps
    st, s = host.control(0xA1, 0x67, 0, 0, 64)
    assert s == u16(12) + u16(TRANSACTION_CANCELLED) + u32(EP_IN) + u32(EP_OUT), s.hex()
    host.control(0x02, 1, 0, EP_IN, 0)
    host.control(0x02, 1, 0, EP_OUT, 0)
    st, s = host.control(0xA1, 0x67, 0, 0, 64)
    assert s == u16(4) + u16(OK), s.hex()
    rc, _, _ = host.transaction(0x1004, 11)
    assert rc == OK
    # Malformed status fault.
    m.ctl(dict(fault='bad_status', kind='endpoint'))
    st, s = host.control(0xA1, 0x67, 0, 0, 64)
    assert s == u16(8) + u16(TRANSACTION_CANCELLED) + u32(0x85)
    # Split form: the header arrives as its own 12-byte packet.
    hs2 = FakeHost('split')
    hs2.msg(SET_CONFIG, b'\x01')
    hs2.bulk_out(struct.pack('<IHHI', 12, T_CMD, 0x1001, 0))
    status, head = hs2.bulk_in(4096)
    assert len(head) == 12 and struct.unpack('<I', head[:4])[0] > 12
    # Camera: no MTP operations, parent filter refused.
    cam = FakeHost('camera')
    cam.msg(SET_CONFIG, b'\x01')
    rc, _, _ = cam.transaction(0x1002, 0, 1)
    rc, _, _ = cam.transaction(0x1007, 1, 0x00010001, 0, 0xFFFFFFFF)
    assert rc == PARAM_NOT_SUPPORTED
    rc, _, _ = cam.transaction(0x9805, 2, 0, 0, 0xFFFFFFFF, 0, 1)
    assert rc == OP_NOT_SUPPORTED
    st, _ = cam.control(0x80, 6, 0x03EE, 0, 255)
    assert st == ST_STALL
    print('mtpemu selftest: PASS')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int)
    ap.add_argument('--ctl', type=int, help='control port for faults and events (JSON lines)')
    ap.add_argument('--log', type=Path)
    ap.add_argument('--profile', choices=sorted(PROFILES), default='android')
    ap.add_argument('--speed', choices=('full', 'high', 'super'), default='high')
    ap.add_argument('--verbose', action='store_true', help='log every usbredir message but control')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    if not args.port or not args.log:
        ap.error('--port and --log are required')
    serve(args)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""Original native-host exact-X4 asynchronous ISO diagnostic, not a driver.

Public libusb1.0.30 ABI only. Preparation must pass nattest.py before use.
No device runs on import. A stuck cancellation retains buffers and pumps
events indefinitely rather than releasing lower-owned memory.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
import struct
import time
from pathlib import Path

DLL_SHA = '622FC378B496A5F348F06DF94DB07E9BA8CE21C63E8C303CC49A9B16EBD1F29B'
RATE_DLL_SHA = '59ED120AEDB5BAF732C67D405D5EA9415D0D3A37548B833651CC30EA5EFFE21B'
LIBUSBK_SHA = '0690CD87D0069EDAC3A867E0B51F379AFA77159EDA3B45CD02740FC6AFB546D8'
CALL = getattr(c, 'WINFUNCTYPE', c.CFUNCTYPE)
RETAINED_CONTEXTS = []  # emergency native owner retention after teardown exception


class Packet(c.Structure):
    _fields_ = [('length', c.c_uint), ('actual_length', c.c_uint), ('status', c.c_int)]


class Transfer(c.Structure):
    pass


TP = c.POINTER(Transfer)
CALLBACK = CALL(None, TP)
Transfer._fields_ = [('dev_handle', c.c_void_p), ('flags', c.c_uint8),
                    ('endpoint', c.c_uint8), ('type', c.c_uint8), ('timeout', c.c_uint),
                    ('status', c.c_int), ('length', c.c_int), ('actual_length', c.c_int),
                    ('callback', CALLBACK), ('user_data', c.c_void_p),
                    ('buffer', c.POINTER(c.c_ubyte)), ('num_iso_packets', c.c_int),
                    ('iso', Packet * 0)]


def abi_check():
    expected = (64, 32, 48, 60) if c.sizeof(c.c_void_p) == 8 else (40, 24, 32, 40)
    actual = (c.sizeof(Transfer), Transfer.callback.offset, Transfer.buffer.offset, Transfer.iso.offset)
    if actual != expected or c.sizeof(Packet) != 12:
        raise RuntimeError(f'libusb public ABI mismatch {actual}/{expected}')


def packets(pointer):
    n = pointer.contents.num_iso_packets
    if not 0 <= n <= 32:
        raise RuntimeError(f'invalid completion packet count {n}')
    return (Packet * n).from_address(c.addressof(pointer.contents) + Transfer.iso.offset)


class RateState(c.Structure):
    _fields_ = [(n, c.c_uint) for n in ('Base Period FrameBytes MaxFrames Numerator Remainder '
                  'Nominal FeedbackStep Phase GoodFeedback BadFeedback').split()] + [
                  (n, c.c_int) for n in ('FeedbackActive Ready HighSpeed').split()]


class Rate:
    """Calls this repository's actual original uacrate.c, not a second planner."""
    def __init__(self, path):
        self.dll = c.CDLL(str(Path(path).resolve()))
        self.dll.UacRateInitialize.argtypes = [c.POINTER(RateState), c.c_uint, c.c_int,
                                             c.c_uint, c.c_uint, c.c_uint]
        self.dll.UacRateInitialize.restype = c.c_int
        self.dll.UacRateFeedback.argtypes = [c.POINTER(RateState), c.c_void_p, c.c_uint]
        self.dll.UacRateFeedback.restype = c.c_int
        self.dll.UacRateNextBytes.argtypes = [c.POINTER(RateState)]
        self.dll.UacRateNextBytes.restype = c.c_uint
        self.state = RateState()
        if c.sizeof(self.state) != 56 or not self.dll.UacRateInitialize(c.byref(self.state),48000,1,1,4,112):
            raise RuntimeError('actual rate core initialization failed')

    def next_bytes(self):
        return self.dll.UacRateNextBytes(c.byref(self.state))

    def feedback(self, raw):
        b = c.create_string_buffer(raw)
        return bool(self.dll.UacRateFeedback(c.byref(self.state), b, len(raw)))


class Engine:
    """One event-pump thread owns planner, callbacks and all submissions."""
    def __init__(self, backend, rate, target_frames=96000, clock=time.perf_counter_ns):
        self.backend, self.rate, self.clock = backend, rate, clock
        self.remaining = target_frames * 4
        self.rows, self.slots = [], []
        self.stopping, self.failure = False, None
        self.good_fb = self.bad_fb = self.keepalives = self.nonzero = 0
        self.queued_bytes = self.completed_requested = self.actual_out = self.packet_errors = 0
        self.started = self.last_out = None
        self.callback = CALLBACK(self._callback)  # retained until ALL callbacks drained
        try:
            for endpoint, count in ((1,4),(0x83,2)):
                for _ in range(count):
                    pointer, buffer = backend.allocate(32, endpoint)
                    self.slots.append(dict(pointer=pointer, buffer=buffer, endpoint=endpoint,
                                           pending=False, lengths=[]))
        except BaseException:
            self.free()
            raise

    @property
    def pending(self):
        return sum(s['pending'] for s in self.slots)

    @property
    def out_pending(self):
        return any(s['pending'] and s['endpoint']==1 for s in self.slots)

    def submit(self, slot):
        if self.stopping:
            return False
        lengths = []
        if slot['endpoint'] == 1:
            for _ in range(32):
                if not self.remaining:
                    break
                count = min(self.rate.next_bytes(), self.remaining)
                if count % 4 or not 0 < count <= 112:
                    raise RuntimeError('planner produced invalid packet length')
                lengths.append(count)
                self.remaining -= count
            if not lengths:
                return False
            # Original quiet triangle. Continuous phase follows queued PCM frames.
            raw = bytearray()
            frame = self.queued_bytes // 4
            for _ in range(sum(lengths)//4):
                phase = (frame * 440) % 48000
                value = (min(phase,48000-phase)*2048//24000)-1024
                raw += struct.pack('<hh',value,value)
                frame += 1
            self.nonzero += sum(b != 0 for b in raw)
            self.queued_bytes += len(raw)
            c.memmove(slot['buffer'], bytes(raw), len(raw))
        else:
            lengths = [4]*32  # explicit native queue policy, unlike guest's 2x8 FB
        pointer = slot['pointer']
        t = pointer.contents
        t.dev_handle = self.backend.handle
        t.flags, t.endpoint, t.type, t.timeout = 0, slot['endpoint'], 1, 0
        t.callback, t.user_data = self.callback, None
        t.buffer, t.length, t.num_iso_packets = slot['buffer'], sum(lengths), len(lengths)
        for packet, count in zip(packets(pointer), lengths):
            packet.length, packet.actual_length, packet.status = count, 0, 0
        slot['lengths'] = lengths
        if self.started is None and slot['endpoint'] == 1:
            self.started = self.clock()
        rc = self.backend.submit(pointer)
        if rc:
            self.failure, self.stopping = f'submit endpoint{slot["endpoint"]:02x}: {rc}', True
            return False
        slot['pending'] = True  # libusb does not synchronously invoke callback on submit
        return True

    def start(self):
        for endpoint in (0x83,1):
            for slot in self.slots:
                if slot['endpoint'] == endpoint:
                    self.submit(slot)

    def _callback(self, pointer):
        try:
            address = c.addressof(pointer.contents)
            slot = next(s for s in self.slots if c.addressof(s['pointer'].contents)==address)
            if not slot['pending']:
                raise RuntimeError('duplicate/unowned completion')
            slot['pending'] = False
            t, now = pointer.contents, self.clock()
            if t.num_iso_packets != len(slot['lengths']):
                raise RuntimeError('packet-count changed on completion')
            row = dict(qpc_ns=now, endpoint=slot['endpoint'], status=t.status, packets=[])
            offset = 0
            for packet, requested in zip(packets(pointer),slot['lengths']):
                if packet.length != requested or packet.actual_length > requested:
                    raise RuntimeError('completion actual/requested bounds invalid')
                entry = dict(requested=requested, actual=packet.actual_length, status=packet.status)
                if slot['endpoint'] == 0x83:
                    raw = bytes(slot['buffer'][offset:offset+packet.actual_length])
                    entry['feedback_hex'] = raw.hex()
                    if t.status == 0 and packet.status == 0:
                        if not raw and self.good_fb:
                            self.keepalives += 1
                        elif self.rate.feedback(raw):
                            self.good_fb += 1
                        else:
                            self.bad_fb += 1
                elif t.status == 0 and packet.status == 0:
                    self.completed_requested += requested
                    self.actual_out += packet.actual_length
                if packet.status and t.status != 3:
                    self.packet_errors += 1
                row['packets'].append(entry)
                offset += requested  # public libusb cumulative requested offsets
            self.rows.append(row)
            if slot['endpoint'] == 1 and t.status == 0:
                self.last_out = now
            if t.status not in (0,3):
                self.failure, self.stopping = f'transfer status {t.status}', True
            if not self.stopping:
                self.submit(slot)
        except BaseException as exc:
            # Never let an exception escape the native callback trampoline.
            self.failure, self.stopping = repr(exc), True

    def cancel(self):
        self.stopping = True
        for slot in self.slots:
            if slot['pending']:
                rc = self.backend.cancel(slot['pointer'])
                if rc not in (0,-5):  # NOT_FOUND still needs completion retirement
                    self.failure = f'cancel failed {rc}'

    def free(self):
        if self.pending:
            raise RuntimeError('REFUSED: lower-owned transfers cannot be freed')
        for slot in self.slots:
            self.backend.free(slot['pointer'])
        self.slots.clear()


class Libusb:
    def __init__(self, dll):
        self.dll = c.WinDLL(str(dll))
        self.ctx, self.handle = c.c_void_p(), c.c_void_p()
        ptr = c.c_void_p
        signatures = {
            'init':([c.POINTER(ptr)],c.c_int),'exit':([ptr],None),
            'get_device_list':([ptr,c.POINTER(c.POINTER(ptr))],c.c_ssize_t),
            'free_device_list':([c.POINTER(ptr),c.c_int],None),
            'get_device_descriptor':([ptr,ptr],c.c_int),'get_device_speed':([ptr],c.c_int),
            'open':([ptr,c.POINTER(ptr)],c.c_int),'close':([ptr],None),
            'claim_interface':([ptr,c.c_int],c.c_int),'release_interface':([ptr,c.c_int],c.c_int),
            'set_interface_alt_setting':([ptr,c.c_int,c.c_int],c.c_int),
            'control_transfer':([ptr,c.c_uint8,c.c_uint8,c.c_uint16,c.c_uint16,ptr,c.c_uint16,c.c_uint],c.c_int),
            'alloc_transfer':([c.c_int],TP),'free_transfer':([TP],None),
            'submit_transfer':([TP],c.c_int),'cancel_transfer':([TP],c.c_int),
            'handle_events_timeout_completed':([ptr,ptr,c.POINTER(c.c_int)],c.c_int)}
        for name,(args,result) in signatures.items():
            fn=getattr(self.dll,'libusb_'+name);fn.argtypes,fn.restype=args,result
            setattr(self,name,fn)
        initialized=False
        try:
            self.checked(self.init(c.byref(self.ctx)),'init');initialized=True
            self._open_exact()
        except BaseException:
            if self.handle:
                try:self.close(self.handle);self.handle=c.c_void_p()
                except BaseException:
                    RETAINED_CONTEXTS.append(self)
                    raise RuntimeError('constructor close failed; open handle/context retained')
            if initialized:self.exit(self.ctx);self.ctx=c.c_void_p()
            raise

    def _open_exact(self):
        ptr=c.c_void_p
        devices=c.POINTER(ptr)()
        count=self.get_device_list(self.ctx,c.byref(devices))
        try:
            self.checked(count,'device list')
            matches=[]
            for i in range(count):
                desc=(c.c_ubyte*18)()
                if self.get_device_descriptor(devices[i],desc)==0 and bytes(desc[8:12])==bytes.fromhex('1e047832'):
                    matches.append(devices[i])
            if len(matches)!=1:
                raise RuntimeError(f'exactly one041e:3278 required: {len(matches)}')
            if self.get_device_speed(matches[0])!=3:
                raise RuntimeError('exact baseline requires actual high speed')
            rc=self.open(matches[0],c.byref(self.handle))
            if rc<0:
                self.handle=c.c_void_p()  # failure does not transfer handle ownership
                self.checked(rc,'open')
        finally:
            if devices:self.free_device_list(devices,1)

    @staticmethod
    def checked(rc,what):
        if rc<0:
            raise RuntimeError(f'{what}: {rc}')
        return rc

    def control(self,typ,request,value,index,data):
        buf=(c.c_ubyte*len(data)).from_buffer_copy(data)
        rc=self.control_transfer(self.handle,typ,request,value,index,buf,len(data),1000)
        if rc!=len(data):
            raise RuntimeError(f'control {typ:02x}/{request:02x}/{value:04x}/{index:04x}: {rc}/{len(data)}')
        return bytes(buf)

    def allocate(self,n,endpoint):
        pointer=self.alloc_transfer(n)
        if not pointer:
            raise MemoryError('libusb_alloc_transfer')
        try:buffer=(c.c_ubyte*(32*(112 if endpoint==1 else 4)))()
        except BaseException:self.free_transfer(pointer);raise
        return pointer,c.cast(buffer,c.POINTER(c.c_ubyte))

    def submit(self,p): return self.submit_transfer(p)
    def cancel(self,p): return self.cancel_transfer(p)
    def free(self,p): self.free_transfer(p)
    def events(self):
        # Windows public timeval uses two C longs (32-bit even in x64 LLP64).
        tv=(c.c_long*2)(0,1000)
        return self.handle_events_timeout_completed(self.ctx,tv,None)


def verify_configuration(raw):
    """Exact experimental X4 profile gate, not a generic UAC2 parser."""
    if len(raw)<9 or raw[0]!=9 or raw[1]!=2 or int.from_bytes(raw[2:4],'little')!=len(raw):
        raise ValueError('invalid configuration envelope')
    interface=None
    found=set()
    pos=0
    while pos<len(raw):
        if pos+2>len(raw) or raw[pos]<2 or pos+raw[pos]>len(raw):
            raise ValueError('truncated descriptor')
        d=raw[pos:pos+raw[pos]];pos+=len(d)
        if d[1]==4:
            if len(d)!=9: raise ValueError('interface length')
            interface=(d[2],d[3])
            if interface==(3,0) and d[5:8]==bytes([1,1,0x20]):found.add('control')
            if interface==(4,1) and d[5:8]==bytes([1,2,0x20]):found.add('stream')
        elif d[1]==0x24 and interface==(3,0) and len(d)==8 and d[2:4]==bytes([10,33]):
            if d[5]&15==7:found.add('clock')
        elif d[1]==0x24 and interface==(3,0) and len(d)==8 and d[2:6]==bytes([11,34,1,33]):
            found.add('selector')
        elif d[1]==0x24 and interface==(4,1) and len(d)==16 and d[2:4]==bytes([1,3]):
            if d[5]==1 and d[6:10]==bytes([1,0,0,0]) and d[10]==2:found.add('stereo')
        elif d[1]==0x24 and interface==(4,1) and len(d)==6 and d[2:6]==bytes([2,1,2,16]):
            found.add('pcm16')
        elif d[1]==5 and interface==(4,1) and len(d)>=7:
            if d[2]==1 and d[3]&0x3f==5 and d[4:7]==bytes([112,0,1]):found.add('out')
            if d[2]==0x83 and d[3]&0x3f==0x11 and d[4:7]==bytes([4,0,4]):found.add('feedback')
    if found!={'control','stream','clock','selector','stereo','pcm16','out','feedback'}:
        raise ValueError(f'exact X4 profile not established: {sorted(found)}')


def range_supports(raw,rate):
    if len(raw)<2:
        raise ValueError('short RANGE')
    count=int.from_bytes(raw[:2],'little')
    if not 1<=count<=32 or len(raw)!=2+12*count:
        raise ValueError('RANGE count/length')
    previous=-1;supported=False
    for off in range(2,len(raw),12):
        lo,hi,step=struct.unpack_from('<III',raw,off)
        if not lo or lo>hi or lo<=previous or (step!=0 if lo==hi else step==0):
            raise ValueError('RANGE ordering/resolution')
        previous=hi
        supported |= lo<=rate<=hi and (lo==hi or (rate-lo)%step==0)
    return supported


class DeviceState:
    def __init__(self,backend,report):
        self.b,self.report=backend,report
        self.claimed=[];self.old_alt=self.old_rate=None
        self.clock_maybe_changed=False;self.alt_maybe_changed=False
        self.state_restored=False

    def ctrl(self,typ,request,value,index,raw):
        answer=self.b.control(typ,request,value,index,raw)
        self.report.setdefault('controls',[]).append(dict(type=typ,request=request,value=value,index=index,
                                                        out=raw.hex() if not typ&0x80 else None,result=answer.hex()))
        return answer

    def setup(self):
        header=self.ctrl(0x80,6,0x200,0,bytes(9))
        total=int.from_bytes(header[2:4],'little')
        if not 9<=total<=65535:raise ValueError('configuration total')
        raw=self.ctrl(0x80,6,0x200,0,bytes(total))
        verify_configuration(raw)
        if self.ctrl(0x80,8,0,0,bytes(1))!=raw[5:6]:
            raise RuntimeError('active configuration differs; no SET_CONFIGURATION permitted')
        for interface in (3,4):
            self.b.checked(self.b.claim_interface(self.b.handle,interface),f'claim{interface}')
            self.claimed.append(interface)
        self.old_alt=self.ctrl(0x81,10,0,4,bytes(1))[0]
        if self.old_alt!=0:
            raise RuntimeError('refuse to replace already active playback alt')
        self.old_rate=int.from_bytes(self.ctrl(0xa1,1,0x100,0x2103,bytes(4)),'little')
        count=int.from_bytes(self.ctrl(0xa1,2,0x100,0x2103,bytes(2)),'little')
        if not 1<=count<=32:raise ValueError('clock subrange count')
        ranges=self.ctrl(0xa1,2,0x100,0x2103,bytes(2+12*count))
        if not range_supports(ranges,48000):raise ValueError('48k unsupported')
        if not self.old_rate or not range_supports(ranges,self.old_rate):
            raise ValueError('original clock not a restorable supported frequency')
        # Clock Selector34 currently routes pin1 -> Clock Source33. No selector SET.
        if self.ctrl(0xa1,1,0x100,0x2203,bytes(1))!=b'\1':
            raise RuntimeError('clock selector34 is not pin1')
        if self.old_rate!=48000:
            self.clock_maybe_changed=True
            self.ctrl(0x21,1,0x100,0x2103,struct.pack('<I',48000))
        if self.ctrl(0xa1,1,0x100,0x2103,bytes(4))!=struct.pack('<I',48000):
            raise RuntimeError('clock48k readback mismatch')
        if self.ctrl(0xa1,1,0x200,0x2103,bytes(1))!=b'\1':
            raise RuntimeError('clock validity not1')
        self.alt_maybe_changed=True
        self.b.checked(self.b.set_interface_alt_setting(self.b.handle,4,1),'alt1')
        if self.ctrl(0x81,10,0,4,bytes(1))!=b'\1':raise RuntimeError('alt1 readback')

    def restore(self):
        errors=[]
        def attempt(name,fn):
            try:fn()
            except BaseException as exc:errors.append(f'{name}: {exc!r}')
        if not self.state_restored and self.alt_maybe_changed:
            attempt('alt0',lambda:self.b.checked(self.b.set_interface_alt_setting(self.b.handle,4,0),'alt0'))
        if not self.state_restored and self.clock_maybe_changed:
            attempt('restore clock',lambda:self.ctrl(0x21,1,0x100,0x2103,struct.pack('<I',self.old_rate)))
            def clock_readback():
                if self.ctrl(0xa1,1,0x100,0x2103,bytes(4))!=struct.pack('<I',self.old_rate):
                    raise RuntimeError('clock restore readback mismatch')
            attempt('clock readback',clock_readback)
        if not self.state_restored and self.alt_maybe_changed:
            def alt_readback():
                if self.ctrl(0x81,10,0,4,bytes(1))!=bytes([self.old_alt]):
                    raise RuntimeError('alt restore readback mismatch')
            attempt('alt readback',alt_readback)
        if not errors:self.state_restored=True
        # Keep every needed claim while any state restoration remains unknown.
        # Once state is confirmed, retries release only remaining claims and
        # never issue class controls through an already released interface.
        if self.state_restored:
            for interface in list(reversed(self.claimed)):
                def release(i=interface):
                    self.b.checked(self.b.release_interface(self.b.handle,i),f'release{i}')
                    self.claimed.remove(i)  # only confirmed release discharges ownership
                attempt(f'release{interface}',release)
        self.report.setdefault('device_restore_attempts',[]).append(errors)
        self.report.setdefault('device_restore_errors',[]).extend(errors)
        self.report['device_claims_unreleased']=list(self.claimed)
        return not errors


class HostTiming:
    """Own-process API-only timer/policy snapshot and checked restoration."""
    class Power(c.Structure):
        _fields_=[('Version',c.c_uint32),('ControlMask',c.c_uint32),('StateMask',c.c_uint32)]
    class Caps(c.Structure):
        _fields_=[('minimum',c.c_uint),('maximum',c.c_uint)]

    def __init__(self,report):
        self.report=report;self.active=False;self.restore_pending=False
        self.k=c.WinDLL('kernel32',use_last_error=True);self.mm=c.WinDLL('winmm')
        for name in ('GetProcessInformation','SetProcessInformation'):
            fn=getattr(self.k,name);fn.argtypes=[c.c_void_p,c.c_int,c.c_void_p,c.c_uint];fn.restype=c.c_int
        self.k.GetCurrentProcess.argtypes=[];self.k.GetCurrentProcess.restype=c.c_void_p
        self.process=self.k.GetCurrentProcess()
        self.mm.timeGetDevCaps.argtypes=[c.POINTER(self.Caps),c.c_uint];self.mm.timeGetDevCaps.restype=c.c_uint
        for name in ('timeBeginPeriod','timeEndPeriod'):
            fn=getattr(self.mm,name);fn.argtypes=[c.c_uint];fn.restype=c.c_uint

    def query(self):
        state=self.Power(1,0,0)
        if not self.k.GetProcessInformation(self.process,4,c.byref(state),12):
            raise OSError(c.get_last_error(),'GetProcessInformation')
        if state.Version!=1:raise RuntimeError('power ABI version')
        return state

    def change(self,state):
        if not self.k.SetProcessInformation(self.process,4,c.byref(state),12):
            raise OSError(c.get_last_error(),'SetProcessInformation')
        actual=self.query()
        if bytes(actual)!=bytes(state):raise RuntimeError('power state readback mismatch')

    def start(self):
        self.original=self.query()
        self.report['power_snapshot']=bytes(self.original).hex()
        desired=self.Power(1,self.original.ControlMask|4,self.original.StateMask&~4)
        self.restore_pending=True
        self.change(desired)
        self.report['power_honor']=bytes(desired).hex()
        caps=self.Caps();rc=self.mm.timeGetDevCaps(c.byref(caps),c.sizeof(caps))
        if rc or not 0<caps.minimum<=caps.maximum:raise RuntimeError(f'timeGetDevCaps {rc}')
        self.period=caps.minimum
        rc=self.mm.timeBeginPeriod(self.period)
        self.report['timeBeginPeriod']=[self.period,rc]
        if rc:raise RuntimeError(f'timeBeginPeriod {rc}')
        self.active=True

    def restore(self):
        errors=[]
        if self.active:
            try:
                rc=self.mm.timeEndPeriod(self.period)
                self.report.setdefault('timeEndPeriod_attempts',[]).append([self.period,rc])
                if rc:errors.append(f'timeEndPeriod {rc}')
                else:self.active=False
            except BaseException as exc:errors.append(f'timeEndPeriod exception {exc!r}')
        if self.restore_pending:
            try:self.change(self.original);self.restore_pending=False
            except BaseException as exc:errors.append(repr(exc))
        self.report.setdefault('timing_restore_attempts',[]).append(errors)
        self.report.setdefault('timing_restore_errors',[]).extend(errors)
        self.report['timing_unconfirmed']=self.active or self.restore_pending
        return not errors


class NativeDependency:
    """Hash + public loader metadata only; never reads DLL code/disassembly."""
    def __init__(self,path,expected_hash,report):
        self.path=path.resolve()
        digest=hashlib.sha256(self.path.read_bytes()).hexdigest().upper()
        if digest!=expected_hash.upper():raise ValueError('libusbK dependency hash mismatch')
        self.digest=digest;self.report=report
        self.module=c.WinDLL(str(self.path))  # keep exact dependency resident for entire run
        self.kernel=c.WinDLL('kernel32',use_last_error=True)
        self.kernel.GetModuleHandleW.argtypes=[c.c_wchar_p];self.kernel.GetModuleHandleW.restype=c.c_void_p
        self.kernel.GetModuleFileNameW.argtypes=[c.c_void_p,c.c_wchar_p,c.c_uint];self.kernel.GetModuleFileNameW.restype=c.c_uint
        self.verify()

    def verify(self):
        module=self.kernel.GetModuleHandleW('libusbK.dll')
        name=c.create_unicode_buffer(32768)
        size=self.kernel.GetModuleFileNameW(module,name,len(name)) if module else 0
        if not size or size>=len(name):raise OSError(c.get_last_error(),'libusbK module path unavailable')
        actual=Path(name.value).resolve()
        if not actual.samefile(self.path):raise RuntimeError(f'wrong loaded libusbK dependency: {actual}')
        digest=hashlib.sha256(actual.read_bytes()).hexdigest().upper()
        if digest!=self.digest:raise RuntimeError('loaded libusbK file identity changed')
        self.report['libusbk_loaded']=dict(path=str(actual),sha256=digest)


def drain(engine, report, on_stall=lambda:None):
    """No timeout ever authorizes freeing lower-owned transfers.

    After five seconds record a negative and retain/pump indefinitely. The
    caller/operator must not infer successful cleanup from a hung process.
    """
    try:engine.cancel()
    except BaseException as exc:
        engine.stopping=True
        report['cancel_exception']=repr(exc)  # still pump/retain every lower-owned slot
    first=engine.clock();announced=False
    while engine.pending:
        try:
            rc=engine.backend.events()
            if rc not in (0,-10):
                report['drain_event_error_count']=report.get('drain_event_error_count',0)+1
                if len(report.setdefault('drain_event_errors',[]))<16:report['drain_event_errors'].append(rc)
                time.sleep(0.001)
            if engine.clock()-first>5_000_000_000 and not announced:
                report['drain_stalled_retaining']=True
                announced=True
                on_stall()
            if announced:time.sleep(0.001)
        except KeyboardInterrupt:
            report['drain_interrupt_ignored_lower_owned']=True
        except BaseException as exc:
            report['drain_exception_count']=report.get('drain_exception_count',0)+1
            if len(report.setdefault('drain_exceptions',[]))<16:report['drain_exceptions'].append(repr(exc))
            time.sleep(0.001)  # retain all owners; no exception permits teardown
    report['all_transfers_retired']=True


def run_engine(engine,report,on_stall=lambda:None):
    engine.start()
    try:
        while engine.out_pending and not engine.failure:
            if engine.clock()-engine.started>10_000_000_000:
                engine.failure='native host10s completion deadline';break
            rc=engine.backend.events()
            if rc not in (0,-10):
                engine.failure=f'events {rc}';break
    except BaseException as exc:
        engine.failure=f'event loop exception {exc!r}'
    finally:
        drain(engine,report,on_stall)
    report.update(transfers=engine.rows,failure=engine.failure,queued_bytes=engine.queued_bytes,
                  completed_requested=engine.completed_requested,actual_out_bytes=engine.actual_out,
                  nonzero_submitted_bytes=engine.nonzero,packet_errors=engine.packet_errors,
                  good_feedback=engine.good_fb,bad_feedback=engine.bad_fb,feedback_keepalives=engine.keepalives,
                  first_out_submit_qpc_ns=engine.started,last_out_callback_qpc_ns=engine.last_out,
                  native_out_window_ms=((engine.last_out-engine.started)/1e6
                                        if engine.last_out is not None and engine.started is not None else None))


def write_report(path,report):
    # All output is outside measurement, except a negative stuck-drain report.
    with path.open('w',encoding='utf-8') as stream:
        json.dump(report,stream,indent=2)
        stream.flush();os.fsync(stream.fileno())


def cleanup_resources(engine,device,backend,timing,report,on_stall=lambda:None):
    """Only async drain may block cleanup; other independent steps all run."""
    if engine and engine.pending:
        drain(engine,report,on_stall)  # never returns with lower-owned buffers
    def attempt(name,fn):
        try:return True,fn()
        except BaseException as exc:
            report.setdefault('cleanup_exceptions',[]).append(dict(step=name,error=repr(exc)))
            return False,None
    if device:
        ok,result=attempt('device_restore',device.restore)
        if not ok or not result:attempt('device_restore_retry',device.restore)
    if engine:attempt('transfer_free',engine.free)
    if backend:
        def close_handle():
            if backend.handle:
                backend.close(backend.handle);backend.handle=c.c_void_p()
                report['device_handle_closed']=True
        attempt('device_close',close_handle)
        if not backend.handle:
            attempt('libusb_exit',lambda:backend.exit(backend.ctx))
        else:
            report['libusb_context_retained_open_handle']=True
            if backend not in RETAINED_CONTEXTS:RETAINED_CONTEXTS.append(backend)
    if timing:
        ok,result=attempt('host_timing_restore',timing.restore)
        if not ok or not result:attempt('host_timing_restore_retry',timing.restore)


def execute(args):
    abi_check()
    if c.sizeof(c.c_void_p)!=8:raise RuntimeError('reviewed native baseline is x64 only')
    dll=args.libusb.resolve();output=args.output.resolve()
    if output.drive.upper()!='E:':raise ValueError('report must resolve to E:')
    if output.exists():raise ValueError('report must be a new file')
    actual_hash=hashlib.sha256(dll.read_bytes()).hexdigest().upper()
    if actual_hash!=DLL_SHA:raise ValueError('exact622FC libusb DLL required')
    rate_hash=hashlib.sha256(args.rate_core.read_bytes()).hexdigest().upper()
    if rate_hash!=RATE_DLL_SHA:raise ValueError('exact reviewed x64 UACRATE.DLL required')
    report=dict(state='STARTING',libusb=str(dll),libusb_sha256=actual_hash,
                rate_core_sha256=rate_hash,
                rate_source_sha256=hashlib.sha256(Path(__file__).with_name('uacrate.c').read_bytes()).hexdigest(),
                probe_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                out_actual_length_caveat='Windows libusbK synthesizes requested OUT lengths; these are API accounting, not measured wire bytes',
                initial_feedback_policy='ZLP before first valid feedback counted bad; raw bytes/lengths preserved for investigation',
                queue_policy='native4x32OUT/2x32FB; guest has2x8FB; distinct scheduling baseline',
                started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                pcm='original quiet triangle440Hz stereo16LE/48000;96000frames; no PCM data logged',
                abi=dict(pointer_bytes=c.sizeof(c.c_void_p),transfer_bytes=c.sizeof(Transfer),
                         iso_offset=Transfer.iso.offset,packet_bytes=c.sizeof(Packet)))
    write_report(output,report)  # prove output persistable before touching device/policy
    timing=backend=device=engine=None
    old_debug=os.environ.get('LIBUSB_DEBUG')
    os.environ['LIBUSB_DEBUG']='2'  # instrumented DLL raw KISO records go to captured stderr
    try:
        rate=Rate(args.rate_core)
        dependency=NativeDependency(args.libusbk,LIBUSBK_SHA,report)
        timing=HostTiming(report);timing.start()
        backend=Libusb(dll)
        dependency.verify()
        device=DeviceState(backend,report);device.setup()
        engine=Engine(backend,rate)
        def stalled():
            report['state']='DRAIN_STALLED_RETAINED'
            write_report(output,report)
            print('NEGATIVE: cancellation stalled; retaining/pumping lower-owned transfers; do not assume cleanup',flush=True)
        run_engine(engine,report,stalled)
        report['state']='USB_MEASUREMENT_COMPLETE'
        # Explicit diagnostic criteria, no WinMM position/fidelity inference.
        report['transport_criteria_met']=bool(not engine.failure and engine.completed_requested==384000 and
                engine.packet_errors==0 and engine.good_fb>=1600 and engine.bad_fb==0 and
                report['native_out_window_ms'] is not None and 1500<=report['native_out_window_ms']<=2500)
    except BaseException as exc:
        report['state']='ERROR';report['exception']=repr(exc)
    finally:
        # If asynchronous ownership exists, it MUST retire before any state
        # restore, interface release, transfer free, DLL/context close or exit.
        cleanup_resources(engine,device,backend,timing,report,lambda:write_report(output,report))
        if old_debug is None:os.environ.pop('LIBUSB_DEBUG',None)
        else:os.environ['LIBUSB_DEBUG']=old_debug
        try:write_report(output,report)
        except BaseException as exc:report.setdefault('cleanup_exceptions',[]).append(dict(step='report_write',error=repr(exc)))
    print(json.dumps({k:v for k,v in report.items() if k not in ('transfers','controls')},indent=2),flush=True)
    return 0 if (report.get('transport_criteria_met') and not report.get('device_restore_errors') and
                 not report.get('timing_unconfirmed') and not report.get('timing_restore_errors') and
                 not report.get('drain_stalled_retaining') and not report.get('cleanup_exceptions')) else 1


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--libusb',type=Path,required=True)
    ap.add_argument('--rate-core',type=Path,required=True)
    ap.add_argument('--libusbk',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--run-physical',action='store_true')
    args=ap.parse_args()
    if not args.run_physical:
        ap.error('physical run requires explicit --run-physical after review/quiet coordination')
    return execute(args)


if __name__=='__main__':
    raise SystemExit(main())

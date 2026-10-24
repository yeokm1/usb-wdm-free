#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""Actual native probe Engine, mocked libusb; never loads libusb/opens USB."""
import ctypes as c
import os
import struct
import unittest
import tempfile
import hashlib
import sys
from unittest.mock import patch
from types import SimpleNamespace
from pathlib import Path
import natprobe as n


class Mock:
    handle=123
    def __init__(self):
        self.buffers={};self.queued=[];self.next_due={1:0,0x83:0};self.now=0
        self.freed=[];self.fail_submit=0;self.submit_count=0;self.inject=None

    def allocate(self,count,endpoint):
        raw=c.create_string_buffer(n.Transfer.iso.offset+count*c.sizeof(n.Packet))
        pointer=c.cast(raw,n.TP)
        pcm=(c.c_ubyte*(32*112))()
        self.buffers[c.addressof(pointer.contents)]=(raw,pcm)
        return pointer,c.cast(pcm,c.POINTER(c.c_ubyte))

    def submit(self,pointer):
        self.submit_count+=1
        if self.submit_count==self.fail_submit:return -1
        ep=pointer.contents.endpoint
        self.next_due[ep]=max(self.next_due[ep],self.now)+pointer.contents.num_iso_packets*(125000 if ep==1 else 1000000)
        self.queued.append((self.next_due[ep],pointer,False))
        return 0

    def cancel(self,pointer):
        addr=c.addressof(pointer.contents)
        for i,(due,p,cancelled) in enumerate(self.queued):
            if c.addressof(p.contents)==addr:
                self.queued[i]=(self.now,p,True);return 0
        return -5

    def free(self,pointer):
        addr=c.addressof(pointer.contents)
        if any(c.addressof(p.contents)==addr for _,p,_ in self.queued):raise AssertionError('free lower-owned')
        self.freed.append(addr)

    def events(self):
        if not self.queued:return 0
        self.queued.sort(key=lambda item:item[0])
        due,p,cancelled=self.queued.pop(0);self.now=max(self.now,due)
        t=p.contents;t.status=3 if cancelled else 0
        off=0
        for packet in n.packets(p):
            packet.status=3 if cancelled else 0
            packet.actual_length=0 if cancelled else packet.length
            if t.endpoint==0x83 and not cancelled:
                c.memmove(c.addressof(t.buffer.contents)+off,b'\0\0\6\0',4)
            off+=packet.length
        if self.inject:self.inject(p)
        t.callback(p)
        return 0


class Tests(unittest.TestCase):
    def rate(self):return n.Rate(os.environ['UAC_NATIVE_RATE'])
    def engine(self,frames=96000):
        backend=Mock()
        engine=n.Engine(backend,self.rate(),frames,lambda:backend.now)
        return backend,engine
    def drain(self,b,e):
        e.cancel()
        while e.pending:b.events()
        e.free()

    def test_abi(self):n.abi_check()
    def test_two_second_async(self):
        b,e=self.engine();e.start()
        while e.out_pending and not e.failure:b.events()
        self.assertEqual(e.remaining,0);self.assertEqual(e.queued_bytes,384000)
        self.assertEqual(e.completed_requested,384000);self.assertEqual(e.actual_out,384000)
        self.assertEqual(e.last_out-e.started,2000000000)
        self.assertGreater(e.nonzero,300000);self.assertGreater(e.good_fb,1600)
        self.assertFalse(e.failure);self.assertEqual(e.packet_errors,0)
        self.drain(b,e);self.assertEqual(len(b.freed),6)
    def test_partial_tail(self):
        b,e=self.engine(101);e.start()
        while e.out_pending:b.events()
        self.assertEqual(e.queued_bytes,404);self.assertEqual(e.completed_requested,404)
        self.assertEqual(sum(row['endpoint']==1 for row in e.rows),1)
        self.drain(b,e)
    def test_lower_ownership(self):
        b,e=self.engine();e.start();e.cancel()
        with self.assertRaisesRegex(RuntimeError,'lower-owned'):e.free()
        self.assertEqual(len(b.freed),0)
        while e.pending:b.events()
        e.free();self.assertEqual(len(b.freed),6)
    def test_submit_failure_and_drain(self):
        b,e=self.engine();b.fail_submit=3;e.start()
        self.assertTrue(e.failure);self.assertEqual(e.pending,2)
        self.drain(b,e);self.assertEqual(len(b.freed),6)
    def test_bad_callback_bounds_captured(self):
        b,e=self.engine();e.start()
        def corrupt(p):n.packets(p)[0].actual_length=n.packets(p)[0].length+1
        b.inject=corrupt;b.events();b.inject=None
        self.assertIn('bounds',e.failure);self.assertTrue(e.stopping)
        self.drain(b,e)
    def test_aggregate_success_does_not_hide_packet_error(self):
        b,e=self.engine();e.start()
        b.inject=lambda p:setattr(n.packets(p)[0],'status',1)
        b.events();b.inject=None
        self.assertEqual(e.packet_errors,1)
        self.drain(b,e)
    def test_feedback_zlp_and_invalid_preserve_rate(self):
        b,e=self.engine();e.start()
        while not e.good_fb:b.events()
        def zlp(p):
            if p.contents.endpoint==0x83:
                for packet in n.packets(p):packet.actual_length=0
        b.inject=zlp
        while not e.keepalives:b.events()
        b.inject=None
        self.assertGreater(e.keepalives,0);self.assertEqual(e.bad_fb,0)
        original_step=e.rate.state.FeedbackStep
        def invalid(p):
            if p.contents.endpoint==0x83:
                for packet in n.packets(p):packet.actual_length=3
        b.inject=invalid
        while not e.bad_fb:b.events()
        b.inject=None
        self.assertEqual(e.rate.state.FeedbackStep,original_step)
        self.drain(b,e)
    def test_range(self):
        singleton=struct.pack('<HIII',1,48000,48000,0)
        self.assertTrue(n.range_supports(singleton,48000))
        self.assertFalse(n.range_supports(singleton,44100))
        for bad in (b'',singleton[:-1],struct.pack('<HIII',1,48000,48000,1),
                    struct.pack('<HIII',1,44100,96000,0),
                    struct.pack('<HIIIIII',2,48000,48000,0,48000,96000,100)):
            with self.assertRaises(ValueError):n.range_supports(bad,48000)

    def test_real_descriptor_and_truncations(self):
        raw=(Path(__file__).parent/'testdata/sbx4/config0.bin').read_bytes()
        n.verify_configuration(raw)
        for cut in range(len(raw)):
            with self.assertRaises(ValueError):n.verify_configuration(raw[:cut])

    def test_run_orchestrator_drains_before_free(self):
        b,e=self.engine();report={};n.run_engine(e,report)
        self.assertTrue(report['all_transfers_retired']);self.assertEqual(e.pending,0)
        self.assertEqual(report['native_out_window_ms'],2000)
        e.free();self.assertEqual(len(b.freed),6)

    def test_stalled_drain_retains_until_actual_callbacks(self):
        b,e=self.engine();e.start();original_events=b.events;first=[True];observed=[]
        def stall_once():
            if first[0]:first[0]=False;b.now+=6_000_000_000;return 0
            return original_events()
        b.events=stall_once;report={}
        n.drain(e,report,lambda:observed.append((e.pending,len(b.freed))))
        self.assertEqual(observed,[(6,0)]);self.assertTrue(report['drain_stalled_retaining'])
        self.assertEqual(e.pending,0);e.free()


class DeviceMock:
    handle=123
    checked=staticmethod(n.Libusb.checked)
    def __init__(self):
        self.raw=(Path(__file__).parent/'testdata/sbx4/config0.bin').read_bytes()
        self.clock,self.alt=44100,0;self.claims=[]
        self.release_failure=False;self.fail_write=False;self.fail_alt=False
        self.restore_alt_failure=False
    def claim_interface(self,h,i):self.claims.append(i);return 0
    def release_interface(self,h,i):
        if self.release_failure:return -1
        self.claims.remove(i);return 0
    def set_interface_alt_setting(self,h,i,alt):
        if i not in self.claims:raise AssertionError('unclaimed alt change')
        self.alt=alt
        if self.fail_alt and alt==1:self.fail_alt=False;return -1
        if self.restore_alt_failure and alt==0:return -1
        return 0
    def control(self,typ,req,value,index,raw):
        if typ&0x7f==0x21 and 3 not in self.claims:raise AssertionError('unclaimed class control')
        if (typ,req)==(0x81,10) and 4 not in self.claims:raise AssertionError('unclaimed interface read')
        if (typ,req)==(0x80,6):return self.raw[:len(raw)]
        if (typ,req)==(0x80,8):return b'\1'
        if (typ,req)==(0x81,10):return bytes([self.alt])
        if index==0x2203:return b'\1'
        if index==0x2103:
            if req==2:
                ranges=struct.pack('<HIIIIII',2,44100,44100,0,48000,48000,0)
                return ranges[:len(raw)]
            if value==0x200:return b'\1'
            if typ==0xa1:return struct.pack('<I',self.clock)
            if typ==0x21:
                self.clock=int.from_bytes(raw,'little')
                if self.fail_write:self.fail_write=False;raise RuntimeError('injected write after mutation')
                return raw
        raise AssertionError((typ,req,value,index))


class DeviceTests(unittest.TestCase):
    def test_setup_restore(self):
        b=DeviceMock();report={};state=n.DeviceState(b,report);state.setup()
        self.assertEqual((b.clock,b.alt),(48000,1));self.assertTrue(state.restore())
        self.assertEqual((b.clock,b.alt,b.claims),(44100,0,[]))
    def test_partial_control_and_alt_failure_restore(self):
        for attr in ('fail_write','fail_alt'):
            b=DeviceMock();setattr(b,attr,True);report={};state=n.DeviceState(b,report)
            with self.assertRaises(RuntimeError):state.setup()
            self.assertTrue(state.restore());self.assertEqual((b.clock,b.alt,b.claims),(44100,0,[]))
    def test_failed_release_retains_claim_and_error_history(self):
        b=DeviceMock();report={};state=n.DeviceState(b,report);state.setup();b.release_failure=True
        self.assertFalse(state.restore());self.assertEqual(state.claimed,[3,4])
        b.release_failure=False;self.assertTrue(state.restore());self.assertEqual(state.claimed,[])
        self.assertTrue(report['device_restore_errors']);self.assertEqual(len(report['device_restore_attempts']),2)
    def test_failed_state_restore_keeps_all_claims_until_retry(self):
        b=DeviceMock();report={};state=n.DeviceState(b,report);state.setup();b.restore_alt_failure=True
        self.assertFalse(state.restore());self.assertEqual(state.claimed,[3,4]);self.assertFalse(state.state_restored)
        b.restore_alt_failure=False;self.assertTrue(state.restore());self.assertEqual(state.claimed,[])


class PowerMock:
    def __init__(self):self.state=(1,1,1);self.gets=self.sets=0;self.fail_get=set();self.fail_set=set();self.mismatch=False
    def GetProcessInformation(self,h,klass,p,size):
        self.gets+=1
        if self.gets in self.fail_get:return 0
        p._obj.Version,p._obj.ControlMask,p._obj.StateMask=self.state
        if self.mismatch and self.gets==2:p._obj.StateMask^=4
        return 1
    def SetProcessInformation(self,h,klass,p,size):
        self.sets+=1
        if self.sets in self.fail_set:return 0
        if p._obj.ControlMask&~4 != self.state[1]&~4:raise AssertionError('unrelated control change')
        if p._obj.StateMask&~4 != self.state[2]&~4:raise AssertionError('unrelated state change')
        self.state=(p._obj.Version,p._obj.ControlMask,p._obj.StateMask);return 1


class TimerMock:
    def __init__(self):self.begins=[];self.ends=[];self.fail_begin=False;self.fail_end=False
    def timeGetDevCaps(self,p,size):p._obj.minimum,p._obj.maximum=1,1000;return 0
    def timeBeginPeriod(self,period):self.begins.append(period);return 1 if self.fail_begin else 0
    def timeEndPeriod(self,period):
        self.ends.append(period)
        if self.fail_end:self.fail_end=False;return 1
        return 0


class PolicyTests(unittest.TestCase):
    def guard(self):
        g=n.HostTiming.__new__(n.HostTiming);g.report={};g.active=g.restore_pending=False
        g.k=PowerMock();g.mm=TimerMock();g.process=1;return g
    def test_matched_success(self):
        g=self.guard();g.start();self.assertEqual(g.k.state,(1,5,1));self.assertTrue(g.restore())
        self.assertEqual(g.k.state,(1,1,1));self.assertEqual(g.mm.begins,g.mm.ends)
    def test_begin_failure_no_bogus_end(self):
        g=self.guard();g.mm.fail_begin=True
        with self.assertRaises(RuntimeError):g.start()
        self.assertTrue(g.restore());self.assertEqual(g.mm.ends,[]);self.assertEqual(g.k.state,(1,1,1))
    def test_apply_query_set_or_readback_failure_restores(self):
        for kind in ('get','set','mismatch'):
            g=self.guard()
            if kind=='get':g.k.fail_get={2}
            if kind=='set':g.k.fail_set={1}
            if kind=='mismatch':g.k.mismatch=True
            with self.assertRaises((OSError,RuntimeError)):g.start()
            self.assertTrue(g.restore());self.assertEqual(g.k.state,(1,1,1))
    def test_restore_failures_keep_history(self):
        g=self.guard();g.start();g.mm.fail_end=True;g.k.fail_set={2}
        self.assertFalse(g.restore());self.assertTrue(g.report['timing_unconfirmed'])
        self.assertTrue(g.restore());self.assertFalse(g.report['timing_unconfirmed'])
        self.assertEqual(g.mm.ends,[1,1]);self.assertEqual(len(g.report['timing_restore_errors']),2)


class LibraryMock:
    def __init__(self,failure):
        self.failure=failure;self.calls=[];self.functions={}
        self.devices=(c.c_void_p*2)(101,None)
    def __getattr__(self,name):
        if name in self.functions:return self.functions[name]
        def function(*args):
            self.calls.append(name)
            if name=='libusb_init':args[0]._obj.value=55;return -1 if self.failure=='init' else 0
            if name=='libusb_get_device_list':
                c.cast(args[1],c.POINTER(c.POINTER(c.c_void_p)))[0]=c.cast(self.devices,c.POINTER(c.c_void_p))
                return -1 if self.failure=='list' else (0 if self.failure=='missing' else 1)
            if name=='libusb_get_device_descriptor':
                raw=bytearray(18);raw[8:12]=bytes.fromhex('1e047832');args[1][:]=raw;return 0
            if name=='libusb_get_device_speed':return 2 if self.failure=='speed' else 3
            if name=='libusb_open':args[1]._obj.value=0xbad;return -1 if self.failure=='open' else 0
            if self.failure=='cleanup_close' and name in ('libusb_free_device_list','libusb_close'):
                raise RuntimeError('injected '+name)
            return 0
        self.functions[name]=function;return function


class ConstructorTests(unittest.TestCase):
    def test_failed_constructor_cleans_context_without_bogus_close(self):
        for failure in ('init','list','missing','speed','open'):
            fake=LibraryMock(failure)
            with patch.object(n.c,'WinDLL',return_value=fake,create=True):
                with self.assertRaises(RuntimeError):n.Libusb(Path('not-opened.dll'))
            self.assertEqual(fake.calls.count('libusb_exit'),0 if failure=='init' else 1)
            self.assertNotIn('libusb_close',fake.calls)
            if failure!='init':self.assertEqual(fake.calls.count('libusb_free_device_list'),1)
    def test_constructor_close_exception_retains_context(self):
        fake=LibraryMock('cleanup_close')
        with patch.object(n.c,'WinDLL',return_value=fake,create=True):
            with self.assertRaisesRegex(RuntimeError,'context retained'):n.Libusb(Path('not-opened.dll'))
        self.assertNotIn('libusb_exit',fake.calls)
        self.assertIs(n.RETAINED_CONTEXTS[-1].dll,fake)


class CleanupTests(unittest.TestCase):
    def test_independent_cleanup_steps_even_if_one_raises(self):
        for failed in ('device','free','close','exit','timing'):
            calls=[]
            def operation(name):
                def fn(*args):
                    calls.append(name)
                    if name==failed:raise RuntimeError('injected '+name)
                    return True
                return fn
            engine=SimpleNamespace(pending=0,free=operation('free'))
            device=SimpleNamespace(restore=operation('device'))
            backend=SimpleNamespace(handle=123,ctx=1,close=operation('close'),exit=operation('exit'))
            timing=SimpleNamespace(restore=operation('timing'))
            report={};n.cleanup_resources(engine,device,backend,timing,report)
            for required in ('device','free','close','timing'):self.assertIn(required,calls)
            if failed=='close':
                self.assertNotIn('exit',calls);self.assertTrue(report['libusb_context_retained_open_handle'])
                self.assertIn(backend,n.RETAINED_CONTEXTS)
            else:self.assertIn('exit',calls)
            self.assertTrue(report['cleanup_exceptions'])
            if failed=='timing':self.assertEqual(calls.count('timing'),2)

    def test_no_cleanup_before_async_retirement(self):
        b=Mock();e=n.Engine(b,n.Rate(os.environ['UAC_NATIVE_RATE']),96000,lambda:b.now);e.start()
        called=[]
        def device_restore():
            self.assertEqual(e.pending,0);called.append('restored');return True
        n.cleanup_resources(e,SimpleNamespace(restore=device_restore),None,None,{})
        self.assertEqual(called,['restored']);self.assertEqual(len(b.freed),6)


class DependencyTests(unittest.TestCase):
    def test_loaded_path_and_hash_checked_without_loading_binary(self):
        scratch=Path(os.environ['UAC_NATIVE_RATE']).parent
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            root=Path(directory);expected=root/'expected.dll';wrong=root/'wrong.dll'
            expected.write_bytes(b'mock, never executable');wrong.write_bytes(expected.read_bytes())
            digest=hashlib.sha256(expected.read_bytes()).hexdigest()
            for loaded in (expected,wrong):
                kernel=SimpleNamespace()
                kernel.GetModuleHandleW=lambda name:42
                def filename(module,buf,count):buf.value=str(loaded);return len(buf.value)
                kernel.GetModuleFileNameW=filename
                report={}
                with patch.object(n.c,'WinDLL',side_effect=[object(),kernel],create=True):
                    if loaded==wrong:
                        with self.assertRaisesRegex(RuntimeError,'wrong loaded'):n.NativeDependency(expected,digest,report)
                    else:
                        dep=n.NativeDependency(expected,digest,report)
                        self.assertEqual(report['libusbk_loaded']['sha256'],digest.upper())
                        expected.write_bytes(b'changed')
                        with self.assertRaisesRegex(RuntimeError,'identity changed'):dep.verify()
                        expected.write_bytes(b'mock, never executable')


class CliTests(unittest.TestCase):
    def test_failure_status_propagates_without_device_access(self):
        argv=['natprobe.py','--libusb','never.dll','--rate-core','never.dll',
              '--libusbk','never.dll','--output','never.json','--run-physical']
        with patch.object(sys,'argv',argv),patch.object(n,'execute',return_value=1) as execute:
            self.assertEqual(n.main(),1);execute.assert_called_once()


if __name__=='__main__':unittest.main()

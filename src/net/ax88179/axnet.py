#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# ASIX AX88179/178A USB 3.0/2.0 to Gigabit Ethernet Devices
# Copyright (C) 2011-2013 ASIX
# Copyright (C) 2026 Yeo Kheng Meng
# Volatile register/init semantics adapted from Linux v6.12
# ax88179_178a.c, adc218676eef25575469234709c2d87185ca223a.
# Original bounded host executor and Ethernet/DHCP/ARP/ICMP test.
"""Exact AX88179 host test: volatile init, DHCP, router ARP/one ping.

Requires an explicitly authorized live network test. Never rebinds a driver,
writes EEPROM/eFuse or node MAC, or configures the host OS network stack.
Logs all control and generated/received packet bytes to a new JSONL file.
"""
import argparse
import ctypes as c
import datetime
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import struct
import time
from axread import Device, Plan, Link


class LinkPlan(c.Structure):
    _fields_ = [(n, c.c_uint) for n in 'Valid Medium ReceiveBufferBytes'.split()] + [
        ('Queue', c.c_ubyte * 5), ('Link', Link)]


class FrameStats(c.Structure):
    _fields_ = [(n, c.c_uint) for n in 'Headers Delivered Dropped Dummy ChecksumErrors'.split()]


RECEIVE = c.CFUNCTYPE(None, c.c_void_p, c.c_void_p, c.c_uint, c.c_ulong)


def checksum(data):
    if len(data) & 1:
        data += b'\0'
    value = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while value >> 16:
        value = (value & 65535) + (value >> 16)
    return (~value) & 65535


def ipv4(src, dst, protocol, payload):
    header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), 0x5542,
                         0x4000, 64, protocol, 0, src, dst)
    return header[:10] + struct.pack('!H', checksum(header)) + header[12:] + payload


def ip_payload(frame, protocol):
    if len(frame) < 34 or frame[12:14] != b'\x08\0':
        return None
    packet = frame[14:]
    size = (packet[0] & 15) * 4
    total = int.from_bytes(packet[2:4], 'big')
    if packet[0] >> 4 != 4 or size < 20 or total < size or total > len(packet):
        return None
    if packet[9] != protocol or int.from_bytes(packet[6:8], 'big') & 0x3fff or checksum(packet[:size]):
        return None
    return packet[12:16], packet[16:20], packet[size:total]


def dhcp_packet(mac, xid, kind, offered=None, server=None, client=b'\0' * 4):
    bootp = struct.pack('!BBBBIHH4s4s4s4s16s64s128s', 1, 1, 6, 0, xid, 0,
                        0x8000 if kind != 7 else 0, client, b'\0'*4, b'\0'*4,
                        b'\0'*4, mac + b'\0'*10, b'\0'*64, b'\0'*128)
    options = b'\x63\x82\x53\x63\x35\x01' + bytes([kind]) + b'\x3d\x07\x01' + mac
    if offered:
        options += b'\x32\x04' + offered
    if server:
        options += b'\x36\x04' + server
    options += b'\x37\x04\x01\x03\x33\x36\xff'
    body = (bootp + options).ljust(300, b'\0')
    return struct.pack('!HHHH', 68, 67, 8 + len(body), 0) + body


def parse_dhcp(frame, mac, xid):
    found = ip_payload(frame, 17)
    if not found:
        return None
    src, dst, udp = found
    if len(udp) < 248 or udp[:4] != b'\x00\x43\x00\x44':
        return None
    length = int.from_bytes(udp[4:6], 'big')
    if length < 248 or length > len(udp):
        return None
    if udp[6:8] != b'\0\0' and checksum(src + dst + bytes([0,17]) + udp[4:6] + udp[:length]):
        return None
    data = udp[8:length]
    if data[:3] != b'\x02\x01\x06' or data[4:8] != struct.pack('!I', xid) or data[28:34] != mac:
        return None
    if data[236:240] != b'\x63\x82\x53\x63':
        return None
    options, offset = {}, 240
    while offset < len(data):
        key = data[offset]; offset += 1
        if key == 255:
            break
        if key == 0:
            continue
        if offset == len(data) or offset + 1 + data[offset] > len(data):
            return None
        size = data[offset]; offset += 1
        if key in options:
            return None
        options[key] = data[offset:offset+size]; offset += size
    if len(options.get(53, b'')) != 1:
        return None
    return dict(kind=options[53][0], address=data[16:20], options=options, sender=frame[6:12])


class Ax:
    # Only bounded volatile MAC/PHY writes. No node-ID/EEPROM/eFuse path.
    MAC_WRITES = {0x0b:2, 0x22:2, 0x26:2, 0x2e:5, 0x33:1, 0x34:1, 0x35:1, 0x54:1, 0x55:1}

    def __init__(self, dll, trace):
        self.trace, self.started = trace, time.monotonic()
        self.lib, ptr = c.CDLL(str(Path(dll).resolve())), c.c_void_p
        functions = {
            'init':([c.POINTER(ptr)],c.c_int), 'exit':([ptr],None),
            'get_device_list':([ptr,c.POINTER(c.POINTER(ptr))],c.c_ssize_t),
            'free_device_list':([c.POINTER(ptr),c.c_int],None),
            'get_device_descriptor':([ptr,ptr],c.c_int), 'get_device_speed':([ptr],c.c_int),
            'open':([ptr,c.POINTER(ptr)],c.c_int), 'close':([ptr],None),
            'claim_interface':([ptr,c.c_int],c.c_int), 'release_interface':([ptr,c.c_int],c.c_int),
            'get_configuration':([ptr,c.POINTER(c.c_int)],c.c_int),
            'control_transfer':([ptr,c.c_uint8,c.c_uint8,c.c_uint16,c.c_uint16,ptr,c.c_uint16,c.c_uint],c.c_int),
            'bulk_transfer':([ptr,c.c_ubyte,ptr,c.c_int,c.POINTER(c.c_int),c.c_uint],c.c_int)}
        for name,(params,result) in functions.items():
            fn=getattr(self.lib,'libusb_'+name); fn.argtypes,fn.restype=params,result
        self.core=c.CDLL(str(Path(__file__).resolve().parents[3]/'out/net/ax88179/hostabi/axcore.dll'))
        self.core.AxCtrlDescribe.argtypes=[ptr,c.c_uint,ptr,c.c_uint,c.c_uint,c.POINTER(Device)]
        self.core.AxCtrlPowerPrefix.argtypes=[c.POINTER(Device),c.POINTER(Plan)]
        self.core.AxCtrlMac.argtypes=[ptr,c.c_uint]
        self.core.AxCtrlLinkPlan.argtypes=[c.POINTER(Device),ptr,ptr,c.c_uint,ptr,c.c_uint,c.POINTER(LinkPlan)]
        self.core.AxFrameTransmit.argtypes=[ptr,c.c_uint,c.c_uint,c.c_uint,ptr,c.c_uint,c.POINTER(c.c_uint)]
        self.core.AxFrameReceive.argtypes=[ptr,c.c_uint,c.c_uint,RECEIVE,ptr,c.POINTER(FrameStats)]
        self.core.AxFrameReceiveFcs.argtypes=self.core.AxFrameReceive.argtypes
        self.ctx,self.devices,self.handle=ptr(),c.POINTER(ptr)(),ptr()
        self.claimed=False; self.snapshot={}; self.mac=None

    def log(self,event,**fields):
        fields.update(event=event,seconds=round(time.monotonic()-self.started,6))
        self.trace.write(json.dumps(fields)+'\n'); self.trace.flush()

    def control(self,request,value,index,length,data=None):
        if data is not None:
            if (request==1 and (self.MAC_WRITES.get(value)!=length or index!=length)) or (request==2 and (value!=3 or index!=0 or length!=2)) or request not in (1,2):
                raise RuntimeError('write outside volatile allowlist')
            if len(data)!=length:
                raise RuntimeError('write length')
        buf=(c.c_ubyte*length)()
        if data is not None:
            buf[:]=data
        request_type=0xc0 if data is None else 0x40
        actual=self.lib.libusb_control_transfer(self.handle,request_type,request,value,index,buf,length,2000)
        raw=bytes(buf)
        self.log('control',request_type=request_type,request=request,value=value,index=index,
                 expected=length,actual=actual,hex=raw.hex() if actual>=0 else None)
        if actual!=length:
            raise RuntimeError(f'control {request:02x}/{value:04x}: {actual}/{length}')
        return raw

    def read(self,reg,size): return self.control(1,reg,size,size)
    def write(self,reg,data): return self.control(1,reg,len(data),len(data),data)
    def phy(self,reg): return self.control(2,3,reg,2)

    def open(self):
        if self.lib.libusb_init(c.byref(self.ctx)):
            raise RuntimeError('libusb init')
        count=self.lib.libusb_get_device_list(self.ctx,c.byref(self.devices))
        if count<0: raise RuntimeError(f'device list {count}')
        found=[]
        for i in range(count):
            descriptor=(c.c_ubyte*18)()
            if not self.lib.libusb_get_device_descriptor(self.devices[i],descriptor) and bytes(descriptor[8:12])==b'\x95\x0b\x90\x17':
                found.append((self.devices[i],bytes(descriptor)))
        if len(found)!=1: raise RuntimeError(f'expected one exact AX88179, found{len(found)}')
        device,descriptor=found[0]; speed=self.lib.libusb_get_device_speed(device)
        rc=self.lib.libusb_open(device,c.byref(self.handle))
        if rc: raise RuntimeError(f'open {rc}')
        # Standard descriptor reads use the same checked length policy, no configuration selection.
        buf=(c.c_ubyte*65535)()
        n=self.lib.libusb_control_transfer(self.handle,0x80,6,0x200,0,buf,9,2000)
        if n!=9: raise RuntimeError(f'config header {n}')
        total=int.from_bytes(bytes(buf[2:4]),'little')
        if total<9: raise RuntimeError('config size')
        n=self.lib.libusb_control_transfer(self.handle,0x80,6,0x200,0,buf,total,2000)
        self.profile=Device()
        if n!=total or self.core.AxCtrlDescribe(descriptor,18,buf,total,{2:1,3:2,4:3}.get(speed,0),c.byref(self.profile)):
            raise RuntimeError('descriptor mismatch')
        config=c.c_int()
        if self.lib.libusb_get_configuration(self.handle,c.byref(config)) or config.value!=self.profile.Configuration:
            raise RuntimeError('configuration not already active')
        rc=self.lib.libusb_claim_interface(self.handle,0)
        if rc: raise RuntimeError(f'claim0 {rc}')
        self.claimed=True; self.mac=self.read(0x10,6)
        if not self.core.AxCtrlMac(self.mac,6): raise RuntimeError('invalid MAC')
        self.log('opened',speed=speed,mac=self.mac.hex(),warnings=self.profile.Warnings,
                 core_sha256=hashlib.sha256(Path(self.core._name).read_bytes()).hexdigest(),
                 libusb_sha256=hashlib.sha256(Path(self.lib._name).read_bytes()).hexdigest(),
                 receive_fcs_mode='verified4byteEthernetCRC32',
                 utc=datetime.datetime.now(datetime.timezone.utc).isoformat())

    def initialize(self):
        for reg,size in self.MAC_WRITES.items(): self.snapshot[reg]=self.read(reg,size)
        plan=Plan()
        if self.core.AxCtrlPowerPrefix(c.byref(self.profile),c.byref(plan)) or not plan.Valid or plan.NextGate!=2:
            raise RuntimeError('power plan')
        eeprom=None
        for step in plan.Step[:plan.Count]:
            if step.Kind==3: time.sleep(step.DelayMs/1000); continue
            raw=self.control(step.Request,step.Value,step.Index,step.Length,
                             bytes(step.Data[:step.Length]) if step.Kind==2 else None)
            if step.ResultTag==4: eeprom=int.from_bytes(raw,'little')
        if eeprom is None: raise RuntimeError('auto detach gate not observed')
        if eeprom!=65535 and eeprom&0x100:
            self.write(0x33,bytes([self.read(0x33,1)[0]|8]))
            self.write(0x26,(int.from_bytes(self.read(0x26,2),'little')|0x1000).to_bytes(2,'little'))
        if self.read(0x10,6)!=self.mac: raise RuntimeError('MAC changed after PHY reset')
        self.write(0x54,b'\x34'); self.write(0x55,b'\x52')
        self.write(0x34,b'\0'); self.write(0x35,b'\0') # software checksums, no offload
        self.rxctl=struct.pack('<H',0x3a8) # CRC drop, alignment prefix, physical+broadcast, start
        self.write(0x0b,self.rxctl)
        bmcr=int.from_bytes(self.phy(0),'little')
        self.control(2,3,0,2,((bmcr|0x1200)&~0xc00).to_bytes(2,'little'))
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            phy=self.phy(0x11); usb=self.read(2,1); link=LinkPlan()
            if self.core.AxCtrlLinkPlan(c.byref(self.profile),self.mac,usb,1,phy,2,c.byref(link)):
                raise RuntimeError('link observation rejected')
            if link.Link.Up: break
            time.sleep(.2)
        else: raise RuntimeError('no PHY link in10seconds')
        # Linux link_reset checks the TX FIFO before applying negotiated medium.
        deadline=time.monotonic()+.1
        while True:
            self.write(0x0b,b'\0\0'); self.write(0x0b,self.rxctl)
            fifo=int.from_bytes(self.control(0x81,0x8c,0,4),'little')
            if not fifo&0x40000000: break
            if time.monotonic()>deadline: raise RuntimeError('TX FIFO not ready')
        self.write(0x2e,bytes(link.Queue)); self.write(0x22,struct.pack('<H',link.Medium))
        self.log('link',megabits=link.Link.Megabits,full_duplex=link.Link.FullDuplex,
                 medium=link.Medium,queue=bytes(link.Queue).hex(),rx_bytes=link.ReceiveBufferBytes)

    def send(self,frame,label):
        frame=frame.ljust(60,b'\0'); out=(c.c_ubyte*2048)(); used=c.c_uint()
        if not self.core.AxFrameTransmit(frame,len(frame),1518,self.profile.OutPacket,out,len(out),c.byref(used)):
            raise RuntimeError('C TX framing rejected')
        actual=c.c_int(); rc=self.lib.libusb_bulk_transfer(self.handle,self.profile.BulkOut,out,used.value,c.byref(actual),1000)
        self.log('tx',label=label,usb_hex=bytes(out[:used.value]).hex(),expected=used.value,actual=actual.value,status=rc)
        if rc or actual.value!=used.value: raise RuntimeError('bulk OUT failed/short')

    def receive(self,predicate,seconds):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            buf=(c.c_ubyte*65536)(); actual=c.c_int()
            rc=self.lib.libusb_bulk_transfer(self.handle,self.profile.BulkIn,buf,len(buf),c.byref(actual),250)
            if rc not in (0,-7): raise RuntimeError(f'bulk IN {rc}')
            if not actual.value: continue
            frames=[]
            @RECEIVE
            def collect(_,data,size,status): frames.append(c.string_at(data,size))
            stats=FrameStats()
            valid=self.core.AxFrameReceiveFcs(buf,actual.value,1514,collect,None,c.byref(stats))
            self.log('rx',status=rc,actual=actual.value,usb_hex=bytes(buf[:actual.value]).hex(),
                     valid=bool(valid),delivered=stats.Delivered,dropped=stats.Dropped,checksum_errors=stats.ChecksumErrors)
            if not valid: raise RuntimeError('C RX aggregate rejected')
            for frame in frames:
                result=predicate(frame)
                if result is not None: return result
        raise RuntimeError('receive deadline expired')

    def cleanup(self):
        clean=True
        if self.handle and self.snapshot:
            try:
                self.write(0x0b,b'\0\0')
                for reg in (0x22,0x2e,0x34,0x35,0x54,0x55,0x33,0x26,0x0b):
                    if reg in self.snapshot: self.write(reg,self.snapshot[reg])
                if self.read(0x10,6)!=self.mac: raise RuntimeError('MAC changed')
                self.log('restored',mac_preserved=True)
            except Exception as exc:
                self.log('restore_error',error=str(exc)); clean=False
        if self.claimed:
            status=self.lib.libusb_release_interface(self.handle,0)
            self.log('release',status=status); clean=clean and status==0
        if self.handle: self.lib.libusb_close(self.handle)
        if self.devices: self.lib.libusb_free_device_list(self.devices,1)
        if self.ctx: self.lib.libusb_exit(self.ctx)
        return clean


def network_test(ax):
    mac=ax.mac; xid=int.from_bytes(os.urandom(4),'big'); broadcast=b'\xff'*6
    def ethernet(dst,kind,data): return dst+mac+struct.pack('!H',kind)+data
    def dhcp_receive(frame,kind):
        reply=parse_dhcp(frame,mac,xid)
        return reply if reply and reply['kind']==kind else None
    ax.send(ethernet(broadcast,0x800,ipv4(b'\0'*4,b'\xff'*4,17,dhcp_packet(mac,xid,1))),'DHCP discover')
    offer=ax.receive(lambda f:dhcp_receive(f,2),5)
    server=offer['options'].get(54,b''); address=offer['address']; routers=offer['options'].get(3,b'')
    if len(server)!=4 or len(routers)<4 or len(routers)%4 or not any(address): raise RuntimeError('offer lacks server/router/address')
    router=routers[:4]
    ax.log('offer',address=str(ipaddress.IPv4Address(address)),router=str(ipaddress.IPv4Address(router)),server=str(ipaddress.IPv4Address(server)))
    ax.send(ethernet(broadcast,0x800,ipv4(b'\0'*4,b'\xff'*4,17,dhcp_packet(mac,xid,3,address,server))),'DHCP request')
    ack=ax.receive(lambda f:dhcp_receive(f,5),5)
    if ack['address']!=address or ack['options'].get(54)!=server: raise RuntimeError('ACK identity mismatch')
    ax.log('dhcp_ack',address=str(ipaddress.IPv4Address(address)))
    try:
        arp=struct.pack('!HHBBH',1,0x800,6,4,1)+mac+address+b'\0'*6+router
        ax.send(ethernet(broadcast,0x806,arp),'router ARP')
        def arp_reply(frame):
            if len(frame)<42 or frame[12:14]!=b'\x08\x06': return None
            body=frame[14:42]
            if body[:8]!=struct.pack('!HHBBH',1,0x800,6,4,2) or body[14:18]!=router or body[18:24]!=mac or body[24:28]!=address: return None
            return body[8:14]
        router_mac=ax.receive(arp_reply,3)
        if not ax.core.AxCtrlMac(router_mac,6): raise RuntimeError('invalid router MAC')
        nonce=b'usb-wdm-free'+os.urandom(8)
        icmp=struct.pack('!BBHHH',8,0,0,0x5542,1)+nonce
        icmp=icmp[:2]+struct.pack('!H',checksum(icmp))+icmp[4:]
        ax.send(ethernet(router_mac,0x800,ipv4(address,router,1,icmp)),'router ICMP echo')
        def echo_reply(frame):
            found=ip_payload(frame,1)
            if not found: return None
            src,dst,data=found
            if src==router and dst==address and len(data)==8+len(nonce) and data[:2]==b'\0\0' and data[4:8]==struct.pack('!HH',0x5542,1) and data[8:]==nonce and not checksum(data): return True
            return None
        ax.receive(echo_reply,3)
        ax.log('PASS',dhcp=True,router_arp=True,router_echo=True)
    finally:
        # Return only the acquired lease; no network settings were installed in Windows.
        ax.send(ethernet(ack['sender'],0x800,ipv4(address,server,17,dhcp_packet(mac,xid,7,server=server,client=address))),'DHCP release')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--libusb',required=True); ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args(); args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x',encoding='utf8') as trace:
        ax=Ax(args.libusb,trace); result=1
        try:
            ax.open(); ax.initialize(); network_test(ax); result=0
        except Exception as exc:
            ax.log('FAIL',error=str(exc)); print(str(exc))
        finally:
            if not ax.cleanup(): result=1
            ax.log('final',success=result==0)
    print('AX88179 host network '+('PASS' if not result else 'FAIL')+'; '+str(args.output))
    return result


if __name__=='__main__': raise SystemExit(main())

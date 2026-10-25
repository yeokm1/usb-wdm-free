# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Original host packet checks; never opens USB or sends a network packet."""
import struct
import unittest
import ctypes as c
import zlib
from pathlib import Path
import axnet


class Packets(unittest.TestCase):
    def setUp(self):
        self.mac=bytes.fromhex('020102030405'); self.xid=0x13572468
        self.server=bytes([192,168,17,1]); self.client=bytes([192,168,17,33])

    def offer(self,options=None):
        body=bytearray(240); body[:4]=bytes([2,1,6,0]); body[4:8]=struct.pack('!I',self.xid)
        body[16:20]=self.client; body[28:34]=self.mac; body[236:240]=bytes.fromhex('63825363')
        if options is None:
            options=bytes([53,1,2,54,4])+self.server+bytes([3,4])+self.server+b'\xff'
        body+=options
        udp=struct.pack('!HHHH',67,68,len(body)+8,0)+body
        return self.mac+bytes.fromhex('0209080706050800')+axnet.ipv4(self.server,b'\xff'*4,17,udp)

    def test_request_fields_and_checksum(self):
        udp=axnet.dhcp_packet(self.mac,self.xid,1)
        self.assertEqual(struct.unpack('!HHHH',udp[:8]),(68,67,len(udp),0))
        self.assertEqual(udp[8:12],bytes([1,1,6,0]))
        self.assertEqual(udp[12:16],struct.pack('!I',self.xid))
        self.assertEqual(udp[36:42],self.mac)
        self.assertIn(bytes([53,1,1]),udp[248:])
        ip=axnet.ipv4(b'\0'*4,b'\xff'*4,17,udp)
        self.assertEqual(axnet.checksum(ip[:20]),0)
        self.assertEqual(int.from_bytes(ip[2:4],'big'),len(ip))

    def test_offer_identity(self):
        frame=self.offer(); result=axnet.parse_dhcp(frame,self.mac,self.xid)
        self.assertEqual(result['kind'],2); self.assertEqual(result['address'],self.client)
        self.assertEqual(result['options'][54],self.server)
        self.assertIsNone(axnet.parse_dhcp(frame,self.mac,self.xid+1))
        self.assertIsNone(axnet.parse_dhcp(frame,b'\0'*6,self.xid))

    def test_truncation_and_bad_options(self):
        frame=self.offer()
        for length in range(len(frame)):
            self.assertIsNone(axnet.parse_dhcp(frame[:length],self.mac,self.xid))
        for options in [bytes([53,2,2]),bytes([53]),bytes([53,1,2,53,1,5,255])]:
            self.assertIsNone(axnet.parse_dhcp(self.offer(options),self.mac,self.xid))

    def test_ip_integrity(self):
        frame=bytearray(self.offer()); frame[24]^=1
        self.assertIsNone(axnet.parse_dhcp(bytes(frame),self.mac,self.xid))
        frame=self.offer(); packet=bytearray(frame[14:]);packet[6:8]=b'\x20\0'
        packet[10:12]=b'\0\0';packet[10:12]=struct.pack('!H',axnet.checksum(packet[:20]))
        self.assertIsNone(axnet.parse_dhcp(frame[:14]+packet,self.mac,self.xid))

    def test_persistent_writes_refused_before_io(self):
        device=axnet.Ax.__new__(axnet.Ax)
        for request,value,index,size in [(4,0x43,1,2),(5,0,0,2),(1,0x10,6,6),(2,3,1,2)]:
            with self.assertRaisesRegex(RuntimeError,'allowlist'):
                device.control(request,value,index,size,b'\0'*size)

    def test_actual_c_fcs_mode(self):
        core=c.CDLL(str(Path(__file__).resolve().parents[3]/'out/net/ax88179/hostabi/axcore.dll'))
        core.AxFrameReceiveFcs.argtypes=[c.c_void_p,c.c_uint,c.c_uint,axnet.RECEIVE,c.c_void_p,c.POINTER(axnet.FrameStats)]
        frame=(self.mac+bytes.fromhex('0209080706050800')+bytes(range(46)))
        fcs=zlib.crc32(frame).to_bytes(4,'little'); raw=b'\xee\xee'+frame+fcs
        padded=raw+b'\0'*((-len(raw))%8)
        aggregate=padded+struct.pack('<II',len(raw)<<16,1|(len(padded)<<16))
        for corrupt in range(-1,len(frame+fcs)*8):
            data=bytearray(aggregate)
            if corrupt>=0: data[2+corrupt//8]^=1<<(corrupt%8)
            received=[]
            @axnet.RECEIVE
            def collect(_,ptr,size,status): received.append(c.string_at(ptr,size))
            stats=axnet.FrameStats();buf=c.create_string_buffer(bytes(data))
            self.assertEqual(core.AxFrameReceiveFcs(buf,len(data),1514,collect,None,c.byref(stats)),1)
            self.assertEqual(received,[frame] if corrupt<0 else [])
            self.assertEqual(stats.Dropped,0 if corrupt<0 else 1)


if __name__=='__main__': unittest.main()

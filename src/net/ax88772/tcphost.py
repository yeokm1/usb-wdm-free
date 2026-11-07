#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""TCP for the SIMULATED host of axemu.py's Network (192.168.77.1), so that
a guest's TCP traffic through a simulated adapter can be checked end to end
(test aid only; no physical network). RFC 9293's segment handling, kept to
what a link that seldom loses a frame needs: one connection per (guest
address, guest port, host port), no window scaling, no options but MSS, no
urgent data, a fixed 1 s retransmission from the oldest unacknowledged byte
(go-back-N), a 1-byte probe into a zero window, a reset taken only inside
the receive window, and TIME-WAIT for TIME_WAIT_S after a close (a FIN
repeated then is acknowledged again).

Services on the host address:
- port 7, echo (RFC 862): every byte received is sent back;
- port 9, discard (RFC 863): bytes are counted and hashed;
- port 19, a byte source (after RFC 864's character generator): on
  connecting, SOURCE_BYTES bytes of a fixed pattern, then FIN;
- port 80, HTTP/1.0-style (RFC 9112's message framing, one request per
  connection, then FIN): GET /source?n=N answers N bytes of the port 19
  pattern; POST (any path) with a Content-Length body answers, as text,
  the body's length and SHA-256. Anything else answers 400. This is what
  a guest's own HTTP client (WinHTTP, .NET) can drive from a script.
Any other port answers RST. Every connection's end is logged ('tcp_close')
with the byte counts and the SHA-256 of what the guest sent, so a leg can
compare them with what the guest says it sent and received.
"""
import collections
import hashlib
import struct
import types

from axemu import HOST_IP, Network, checksum, ipv4

FIN, SYN, RST, PSH, ACK = 0x01, 0x02, 0x04, 0x08, 0x10
ECHO, DISCARD, SOURCE, HTTP = 7, 9, 19, 80
HTTP_HEAD_MAX, HTTP_BODY_MAX, HTTP_SOURCE_MAX = 8192, 64 << 20, 64 << 20
SOURCE_BYTES = 1 << 20
WINDOW = 65535
MSS = 1460
IN_FLIGHT = 16384              # bytes unacknowledged at most: well inside the adapter models' receive buffers
ECHO_QUEUE = 262144            # echo bytes held for the guest at most; the window offered shrinks to keep it
RTO = 1.0
TIME_WAIT_S, TIME_WAIT_MAX = 30.0, 64
MOD = 1 << 32


def source_pattern(offset, length):
    """The port 19 bytes: 0x20..0x7E repeating, from the stream offset."""
    return bytes(0x20 + (i % 95) for i in range(offset, offset + length))


def seq_lt(a, b):
    return (a - b) % MOD > MOD // 2


class Conn:
    def __init__(self, key, iss, peer_seq, peer_mss, now):
        self.key = key
        self.iss = iss
        self.snd_una = self.snd_nxt = iss          # SYN-ACK goes out from iss
        self.rcv_nxt = (peer_seq + 1) % MOD
        self.mss = min(MSS, peer_mss)
        self.peer_win = 0
        self.state = 'syn_rcvd'
        self.out = bytearray()                     # bytes from snd_una on, not yet acknowledged
        self.source_left = SOURCE_BYTES if key[2] == SOURCE else 0
        self.source_at = 0
        self.fin_queued = self.fin_sent = self.peer_fin = False
        self.rx_bytes = self.tx_bytes = 0
        self.rx_hash = hashlib.sha256()
        self.progress = now
        self.syn_sent = False
        self.http_head = bytearray()               # port 80: the request until its blank line
        self.http_body = None                      # then the body's hash and the bytes still to come
        self.http_left = self.http_got = 0
        self.http_done = False


class Host(Network):
    """axemu.py's simulated host, with TCP."""

    def __init__(self, log, now):
        super().__init__(log, now)
        self.conns = collections.OrderedDict()
        self.closed = collections.OrderedDict()    # TIME-WAIT: key -> (until, snd_nxt, rcv_nxt)
        self.isn = 0x1000
        self.tcp_ident = 0x5500

    # -- framing -------------------------------------------------------------
    def segment(self, conn, flags, seq, payload=b'', mss=False):
        peer_ip, peer_port, port = conn.key
        options = struct.pack('!BBH', 2, 4, MSS) if mss else b''
        head = struct.pack('!HHIIBBHHH', port, peer_port, seq, conn.rcv_nxt if flags & ACK else 0,
                           (5 + len(options) // 4) << 4, flags, self.window(conn), 0, 0) + options
        body = head + payload
        pseudo = HOST_IP + peer_ip + struct.pack('!BBH', 0, 6, len(body))
        body = body[:16] + struct.pack('!H', checksum(pseudo + body)) + body[18:]
        self.tcp_ident = (self.tcp_ident + 1) & 0xffff
        packet = ipv4(HOST_IP, peer_ip, 6, body, self.tcp_ident)
        return self.frame(conn.mac, 0x0800, packet)

    @staticmethod
    def window(conn):
        """The receive window offered: echo keeps at most ECHO_QUEUE bytes queued."""
        if conn.key[2] == ECHO:
            return max(0, min(WINDOW, ECHO_QUEUE - len(conn.out)))
        return WINDOW

    def reset(self, mac, peer_ip, peer_port, port, seq, ack, flags):
        head = struct.pack('!HHIIBBHHH', port, peer_port, seq, ack, 5 << 4, flags, 0, 0, 0)
        pseudo = HOST_IP + peer_ip + struct.pack('!BBH', 0, 6, len(head))
        head = head[:16] + struct.pack('!H', checksum(pseudo + head)) + head[18:]
        self.tcp_ident = (self.tcp_ident + 1) & 0xffff
        return self.frame(mac, 0x0800, ipv4(HOST_IP, peer_ip, 6, head, self.tcp_ident))

    # -- input ---------------------------------------------------------------
    def handle(self, frame):
        tcp = self.tcp_parts(frame)
        if tcp is None:
            return super().handle(frame)
        return self.tcp_input(frame[6:12], *tcp)

    def tcp_parts(self, frame):
        """(source address, segment) for a whole, correct IPv4 TCP datagram to
        the host; None for anything else (left to Network, which drops a bad
        header and logs the rest as ignored)."""
        if len(frame) < 54 or frame[12:14] != b'\x08\x00' or frame[14] >> 4 != 4:
            return None
        body = frame[14:]
        ihl = (body[0] & 15) * 4
        total = struct.unpack_from('!H', body, 2)[0]
        if body[9] != 6 or body[16:20] != HOST_IP or ihl < 20 or total < ihl + 20 or total > len(body) or \
                checksum(body[:ihl]) or struct.unpack_from('!H', body, 6)[0] & 0x3fff:
            return None
        seg, src = body[ihl:total], body[12:16]
        if checksum(src + HOST_IP + struct.pack('!BBH', 0, 6, len(seg)) + seg):
            self.refuse('tcp_checksum_bad', len(frame))
            return ()
        return src, seg

    def tcp_input(self, mac=None, src=None, seg=None):
        if src is None:
            return []
        peer_port, port, seq, ack, off, flags, window = struct.unpack_from('!HHIIBBH', seg)
        hlen = (off >> 4) * 4
        if hlen < 20 or hlen > len(seg):
            self.refuse('tcp_header_bad', len(seg))
            return []
        payload = seg[hlen:]
        key = (src, peer_port, port)
        conn = self.conns.get(key)
        now = self.now()
        if flags & RST:
            # RFC 9293 3.10.7.4 / RFC 5961: only a reset inside the receive window ends the connection
            if conn and (seq - conn.rcv_nxt) % MOD < max(1, self.window(conn)):
                self.close(conn, 'reset_by_guest')
            elif conn:
                self.log('tcp_reset_ignored', port=port, seq=seq)
            return []
        wait = self.closed.get(key)
        if wait and now >= wait[0]:
            del self.closed[key]
            wait = None
        if conn is None and wait and not flags & SYN:
            if flags & FIN:                        # our last ACK was lost: say it again
                ghost = types.SimpleNamespace(key=key, mac=mac, rcv_nxt=wait[2], out=b'')
                return [self.segment(ghost, ACK, wait[1])]
            return []
        if conn is None and wait:
            # RFC 9293 3.6.1 / RFC 6191: a SYN reopens the pair only when it starts beyond the old
            # connection; an older one (a delayed duplicate) leaves TIME-WAIT as it was
            if flags & ACK or not seq_lt((wait[2] - 1) % MOD, seq):
                return []
            del self.closed[key]
            if not seq_lt(wait[1], (self.isn + 0x10000) % MOD):
                self.isn = wait[1]                 # the new ISN (below: + 0x10000) lies beyond the old snd_nxt
        if conn is None:
            if flags & SYN and not flags & ACK and port in (ECHO, DISCARD, SOURCE, HTTP):
                mss = self.peer_mss(seg[20:hlen])
                self.isn = (self.isn + 0x10000) % MOD
                conn = Conn(key, self.isn, seq, mss, now)
                conn.mac = mac
                conn.peer_win = window
                self.conns[key] = conn
                self.log('tcp_open', port=port, peer=src.hex(), peer_port=peer_port, mss=conn.mss)
                return self.output(conn, now)
            if flags & ACK:
                return [self.reset(mac, src, peer_port, port, ack, 0, RST)]
            return [self.reset(mac, src, peer_port, port, 0, (seq + len(payload) + (1 if flags & SYN else 0) +
                                                               (1 if flags & FIN else 0)) % MOD, RST | ACK)]
        conn.mac = mac
        if flags & SYN:                            # a repeated SYN: the SYN-ACK again
            if conn.state == 'syn_rcvd':
                conn.snd_nxt = conn.snd_una
                conn.syn_sent = False
                return self.output(conn, now)
            return [self.segment(conn, ACK, conn.snd_nxt)]
        if not flags & ACK:
            return []
        if not seq_lt(conn.snd_nxt, ack) and seq_lt(conn.snd_una, ack):  # snd_una < ack <= snd_nxt
            done = (ack - conn.snd_una) % MOD
            if conn.state == 'syn_rcvd':
                done -= 1                          # the SYN
                conn.state = 'established'
                conn.snd_una = (conn.snd_una + 1) % MOD
            take = min(done, len(conn.out))
            del conn.out[:take]
            conn.tx_bytes += take
            done -= take
            if done and conn.fin_sent:             # the FIN acknowledged
                conn.state = 'fin_acked'
            conn.snd_una = ack
            conn.progress = now
        elif conn.state == 'syn_rcvd':
            return []
        conn.peer_win = window
        answer = False
        if payload or flags & FIN:
            if seq == conn.rcv_nxt and not conn.peer_fin:
                if payload and port == ECHO:
                    space = self.window(conn)
                    if len(payload) > space:       # beyond the window offered: not taken, nor its FIN
                        payload, flags = payload[:space], flags & ~FIN
                if payload:
                    conn.rx_bytes += len(payload)
                    conn.rx_hash.update(payload)
                    if port == ECHO:
                        conn.out += payload
                    elif port == HTTP:
                        self.http_input(conn, payload)
                    conn.rcv_nxt = (conn.rcv_nxt + len(payload)) % MOD
                if flags & FIN:
                    conn.peer_fin = True
                    conn.rcv_nxt = (conn.rcv_nxt + 1) % MOD
                    conn.fin_queued = True
            answer = True                          # in order or not: say where we are
        frames = self.output(conn, now, answer)
        if conn.peer_fin and conn.state == 'fin_acked':
            self.close(conn, 'closed')
        return frames

    def http_input(self, conn, data):
        """Port 80: collect one request; queue the answer and FIN once it is whole."""
        if conn.http_done:
            return
        if conn.http_body is None:
            conn.http_head += data
            end = conn.http_head.find(b'\r\n\r\n')
            if end < 0:
                if len(conn.http_head) > HTTP_HEAD_MAX:
                    self.http_answer(conn, 400, b'request head too long\r\n')
                return
            head, data = bytes(conn.http_head[:end]), bytes(conn.http_head[end + 4:])
            lines = head.decode('latin-1').split('\r\n')
            parts = lines[0].split(' ')
            fields = {}
            for line in lines[1:]:
                name, _, value = line.partition(':')
                fields[name.strip().lower()] = value.strip()
            if len(parts) != 3 or not parts[2].startswith('HTTP/1.'):
                self.http_answer(conn, 400, b'bad request line\r\n')
                return
            method, target = parts[0], parts[1]
            if method == 'GET' and target.startswith('/source?n='):
                try:
                    n = int(target[len('/source?n='):])
                except ValueError:
                    n = -1
                if not 0 <= n <= HTTP_SOURCE_MAX:
                    self.http_answer(conn, 400, b'bad length\r\n')
                    return
                self.log('http_request', method=method, target=target)
                self.http_answer(conn, 200, b'', 'application/octet-stream', n)
                conn.source_left, conn.source_at = n, 0
                return
            if method == 'POST':
                try:
                    n = int(fields.get('content-length', ''))
                except ValueError:
                    n = -1
                if not 0 <= n <= HTTP_BODY_MAX:
                    self.http_answer(conn, 400, b'bad content-length\r\n')
                    return
                self.log('http_request', method=method, target=target, content_length=n)
                conn.http_body, conn.http_left = hashlib.sha256(), n
            else:
                self.http_answer(conn, 400, b'unsupported\r\n')
                return
        take = data[:conn.http_left]
        conn.http_body.update(take)
        conn.http_left -= len(take)
        conn.http_got += len(take)
        if not conn.http_left:
            text = ('%d %s\r\n' % (conn.http_got, conn.http_body.hexdigest())).encode('ascii')
            self.log('http_body', length=conn.http_got, sha256=conn.http_body.hexdigest())
            self.http_answer(conn, 200, text)

    @staticmethod
    def http_reason(code):
        return {200: 'OK', 400: 'Bad Request'}[code]

    def http_answer(self, conn, code, body, kind='text/plain', length=None):
        """The answer's head and body, then FIN; with length, the body is the
        port 19 pattern that output() streams (source_left)."""
        conn.http_done = True
        conn.out += ('HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n'
                     % (code, self.http_reason(code), kind, len(body) if length is None else length)
                     ).encode('ascii') + body
        conn.fin_queued = True

    @staticmethod
    def peer_mss(options):
        i = 0
        while i < len(options):
            kind = options[i]
            if kind == 0:
                break
            if kind == 1:
                i += 1
                continue
            if i + 1 >= len(options) or options[i + 1] < 2:
                break
            if kind == 2 and options[i + 1] == 4 and i + 4 <= len(options):
                return struct.unpack_from('!H', options, i + 2)[0] or 536
            i += options[i + 1]
        return 536

    # -- output --------------------------------------------------------------
    def output(self, conn, now, answer=False):
        frames = []
        if conn.state == 'syn_rcvd':
            if not conn.syn_sent:
                conn.syn_sent = True
                conn.snd_nxt = (conn.iss + 1) % MOD
                conn.progress = now
                frames.append(self.segment(conn, SYN | ACK, conn.iss, mss=True))
            return frames
        if conn.source_left and len(conn.out) < IN_FLIGHT:
            n = min(conn.source_left, IN_FLIGHT * 2 - len(conn.out))
            conn.out += source_pattern(conn.source_at, n)
            conn.source_at += n
            conn.source_left -= n
            if not conn.source_left:
                conn.fin_queued = True
        sent = (conn.snd_nxt - conn.snd_una) % MOD
        if conn.fin_sent and conn.state != 'fin_acked':
            sent -= 1                              # the FIN in flight is no data byte
        limit = min(conn.peer_win, IN_FLIGHT)
        while sent < len(conn.out) and sent < limit:
            n = min(conn.mss, len(conn.out) - sent, limit - sent)
            if n <= 0:
                break
            frames.append(self.segment(conn, ACK | PSH, (conn.snd_una + sent) % MOD,
                                       bytes(conn.out[sent:sent + n])))
            sent += n
            conn.snd_nxt = (conn.snd_una + sent) % MOD
        if conn.fin_queued and not conn.fin_sent and sent == len(conn.out) and not conn.source_left:
            frames.append(self.segment(conn, FIN | ACK, conn.snd_nxt))
            conn.fin_sent = True
            conn.snd_nxt = (conn.snd_nxt + 1) % MOD
        if answer and not frames:
            frames.append(self.segment(conn, ACK, conn.snd_nxt))
        return frames

    def poll(self):
        """Retransmissions: anything unacknowledged for RTO goes again from
        the oldest byte. Returns the frames to send."""
        now = self.now()
        frames = []
        for conn in list(self.conns.values()):
            if conn.state != 'syn_rcvd' and conn.snd_nxt == conn.snd_una and not conn.peer_win and \
                    conn.out and now - conn.progress >= RTO:
                conn.progress = now                # the persist timer: one byte into the zero window
                self.log('tcp_window_probe', port=conn.key[2])
                frames.append(self.segment(conn, ACK, conn.snd_una, bytes(conn.out[:1])))
                conn.snd_nxt = (conn.snd_una + 1) % MOD
                continue
            if conn.snd_nxt != conn.snd_una and now - conn.progress >= RTO:
                conn.progress = now
                self.log('tcp_retransmit', port=conn.key[2], at=(conn.snd_una - conn.iss) % MOD)
                if conn.state == 'syn_rcvd':
                    conn.syn_sent = False
                else:
                    if conn.fin_sent:
                        conn.fin_sent = False
                    conn.snd_nxt = conn.snd_una
                frames += self.output(conn, now)
            elif conn.source_left or (conn.fin_queued and not conn.fin_sent):
                frames += self.output(conn, now)
        return frames

    def close(self, conn, why):
        del self.conns[conn.key]
        if why == 'closed':
            self.closed[conn.key] = (self.now() + TIME_WAIT_S, conn.snd_nxt, conn.rcv_nxt)
            while len(self.closed) > TIME_WAIT_MAX:
                self.closed.popitem(last=False)
        self.log('tcp_close', why=why, port=conn.key[2], peer_port=conn.key[1], received=conn.rx_bytes,
                 received_sha256=conn.rx_hash.hexdigest(), sent_acknowledged=conn.tx_bytes)

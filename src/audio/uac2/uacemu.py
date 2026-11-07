#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""SIMULATED UAC 2.0 audio devices over localhost usbredir (test aid only).

Original protocol model from SPICE usb-redirection-protocol.txt v0.7, USB
2.0 chapter 9 and the USB Audio 2.0 class and format documents. No
physical device, audio input or audio output is accessed.

Profiles (--profile):
  x4        the captured Creative X4 layout (testdata/sbx4); strings
            replaced by SIMULATED labels, serial index zero. Only IF4 alt 1
            (stereo 16-bit 48 kHz playback, explicit HS feedback) is
            functional, as in the first X4 runs.
  x4full    the same captured layout with every alternate functional:
            playback 2/6/8 channels at 16 and 24 bits (IF4, IF6), capture
            (IF5), feature-unit SET.
  x1        the captured Creative Sound Blaster X1 layout (testdata/sbx1),
            every alternate functional: HID, CDC ACM and a UAC 2 function
            (playback 2/6/8 channels 16/24-bit, async with explicit
            feedback; 1-channel capture 16/24-bit); strings replaced by
            SIMULATED labels, serial index zero. Its HID interface
            answers as below; its CDC ACM pair is not modelled.
  hs24      High Speed, one programmable clock (44.1 to 192 kHz), playback
            2 channels 16/24/32-bit (subslots 2, 3, 4), capture 2 channels
            16/24-bit; async playback with explicit feedback.
  multi     High Speed playback only: 8 channels 16-bit (IF1) and 6
            channels 24-bit (IF2), 48/96 kHz, async with feedback.
  mic       High Speed capture only: 1 channel 16/24-bit, 16/48/96 kHz.
  fs        Full Speed UAC 2.0: playback 2 channels 16-bit 44.1/48 kHz with
            explicit feedback, capture 1 channel 16-bit. The feedback
            endpoint is USB 2.0's 3-byte 10.14 form (5.12.4.2).
            --fs-feedback 4 makes it 4 bytes carrying 16.16 samples per
            frame, the form Windows 10's own UAC 2.0 driver
            (usbaudio2.sys) takes: it refuses the 3-byte one (Code 10,
            "could not find a feedback endpoint") and reads 4 bytes as
            16.16 (docs/testing/2026-11-06-inbox-reference.md).
  clocks    High Speed, two clock sources (44.1 kHz family, 48 kHz family)
            behind a two-input clock selector for playback; capture on a
            clock multiplier (1/1) of the 48 kHz source.
  adaptive  High Speed adaptive playback (no feedback endpoint, fixed 48
            kHz clock), and synchronous capture, 2 channels 16-bit.
  cx31993   the playback function of a Conexant/Synaptics CX31993 USB
            headphone DAC as a published lsusb -v listing describes it
            (06cb:1595, docs/testing/2026-10-28-uac2-swmidi.md): clock
            source 9, headphone output terminal, a feature unit with master
            mute and per-channel volume, one SYNCHRONOUS playback interface
            with 16/24/32-bit stereo alternates at the listed packet sizes
            (192/288/384 bytes, 384 kHz at bInterval 1). The rate list is
            not in that listing: 44.1 to 384 kHz, both families, assumed.
            Its HID interface is not in the descriptors.
  cx31993m  a user's CX31993 with a MAX97220 headphone amplifier
            (3302:12C3, testdata/cx31993m) replayed: its device and
            configuration descriptors as XHCITR printed them, strings
            replaced by SIMULATED labels, serial index zero. Feature Unit
            0x10 has per-channel volume only (no master, no mute) and an
            Extension Unit (0x04) sits between it and the headphones; one
            ADAPTIVE playback interface, 16/24/32-bit stereo. The device's
            volume RANGE and rate list are not in the report: --vol-range
            and --vol-cur (--vol-cur-at, --vol-cur-stall) set the one,
            the rates are cx31993's, assumed.
            The HID interface answers as below.
  alc4080   an ASSUMED layout for the Realtek ALC4080 on-board codec
            (docs/design/uac2.md section 15.4), not a capture: one
            programmable clock, three playback endpoints - 8-channel
            speaker, front headphone, S/PDIF - through Feature Units 29, 30
            and 32 as Linux's sound/usb/mixer_maps.c numbers an ALC4080
            board's, and two capture endpoints, line in and microphone;
            async playback with explicit feedback, 16/24-bit alternates.
            Pass --vid 0db0 --pid 419c for a board id uac2's table lists.
  alc4080cap the owner's capture of a board's Realtek ALC4080 (testdata/
            alc4080, docs/design/uac2.md section 15.1a) replayed: its
            device and configuration descriptors as captured, strings
            replaced by SIMULATED labels; adaptive playback on IF4 to IF6,
            async capture on IF1 to IF3, each terminal on its own clock
            (44.1 to 192 kHz; the headphones' to 384 kHz, as Linux listed).
            The HID interface answers as below.
  uac3dual  two configurations, as USB Audio 3.0 section 3.3 requires of a
            3.0 device: configuration 1 (index 0) is hs24's UAC 2.0
            function, configuration 2 a USB Audio 3.0 function (IAD
            01/01/30, AudioControl 01/01/30, one AudioStreaming 01/02/30 at
            alternate 0), standard descriptors only, no BOS. Only
            configuration 1 streams; the log records which configuration
            the host selects (docs/design/uac2.md section 16.6).

Every profile answers the standard requests and the clock source
(SAM_FREQ CUR/RANGE, CLOCK_VALID), clock selector (CUR), clock multiplier
(NUMERATOR/DENOMINATOR) and feature unit (MUTE, VOLUME CUR/RANGE)
requests of the entities its descriptors declare, read from the
descriptors themselves. A SET_CUR is refused where the descriptor's
bmaControls do not make the control read/write.

A HID interface in a captured layout (x4, x4full, x1, cx31993m,
alc4080cap) answers SET_IDLE and GET_DESCRIPTOR(Report) with a stand-in
report descriptor (a vendor-page collection with one input byte, padded
with Usage items to the length the interface's HID descriptor declares)
and sends no reports: none of the captures holds the real report
descriptor. Windows' HID driver resets the whole device when that
request stalls, and a reset while Windows 10's own UAC 2.0 driver starts
the audio function fails it (Code 10, docs/testing/2026-11-06-inbox-reference.md).

Feature units of the built profiles (--fu): 'full' (master and every
channel: mute and volume read/write, the default), 'master' (master
only), 'channels' (each channel only), 'readonly' (master and channels
readable, not writable) or 'none' (no feature unit: each output terminal
takes its input terminal directly). --vol-range MIN:MAX:RES (1/256 dB,
signed; default -24576:0:256, -96 to 0 dB in 1 dB steps) and --vol-cur
(1/256 dB, default 0) shape the volume control; --vol-cur-at CH=V sets
one logical channel's CUR at plug-in (0 the master), --vol-cur-stall N
stalls the first N volume GET CURs. --product TEXT sets iProduct's string
('' an empty descriptor), --no-product sets iProduct to 0. Playback is "rendered"
through the feature units on its path: every 0.25 s the log carries the
received peak per channel (dBFS) and the rendered peak (each packet with
the units' volume in force when it arrived; none while muted or at the
silence value 0x8000), so a leg reads a level change made in hardware
(rendered moves, received stays) or in the driver (received moves). A
volume SET_CUR must be a step of the range, or the silence value. Playback OUT packets are checked against the
active alternate (frame size, maximum packet) and counted; feedback runs
at the feedback endpoint's interval, 16.16 at High Speed and at Full
Speed by the endpoint's size (4 bytes 16.16, 3 bytes 10.14), with --fb-ppm, --fb-zero-every and --fb-stop-after to model
drift and loss. Capture IN endpoints stream a 1 kHz sine at the selected
rate and depth. Built profiles carry IDs FFFF:02xx, which no INF lists
(--vid/--pid change them, for the X4 profiles too).
"""
import argparse
import array
import collections
import hashlib
import json
import math
import select
import socket
import struct
import sys
import time
from pathlib import Path

FIXTURE = Path(__file__).resolve().parent / 'testdata/sbx4'
# connect_device_version (1), ep_info_max_packet_size (4), 64bits_ids (5),
# 32bits_bulk_length (6). QEMU's usb-redir refuses a qemu-xhci port without
# the last three; 64-bit ids are used once both hellos carry them.
CAP_VERSION, CAP_EP_MAX, CAP_ID64, CAP_BULK32 = 1, 4, 5, 6
CAPS = (1 << CAP_VERSION) | (1 << CAP_EP_MAX) | (1 << CAP_ID64) | (1 << CAP_BULK32)
CONFIG = (FIXTURE / 'config0.bin').read_bytes()
_device = bytearray((FIXTURE / 'device.bin').read_bytes())
_device[14:17] = bytes((1, 2, 0))
DEVICE = bytes(_device)
QUALIFIER = (FIXTURE / 'qualifier.bin').read_bytes()


def usb_string(text):
    raw = text.encode('utf-16le')
    return bytes((len(raw) + 2, 3)) + raw


def hid_interfaces(config):
    """{interface number: report descriptor length} of every HID
    interface (class 3) whose HID descriptor (HID 1.11 6.2.1) names a
    report descriptor."""
    found, current, off = {}, None, 9
    while off + 2 <= len(config) and config[off] >= 2:
        d = config[off:off + config[off]]
        if d[1] == 4 and len(d) >= 9:
            current = d[2] if d[5] == 3 else None
        elif d[1] == 0x21 and current is not None and len(d) >= 9:
            for k in range(d[5]):
                if 6 + 3 * k + 3 <= len(d) and d[6 + 3 * k] == 0x22:
                    found[current] = int.from_bytes(d[7 + 3 * k:9 + 3 * k], 'little')
        off += config[off]
    return found


def hid_report(length):
    """A stand-in report descriptor of exactly `length` bytes: Usage Page
    FF00 (vendor), Usage 1, Collection (Application), one 8-bit input,
    padded with Usage items inside the collection; None below 21 bytes."""
    head = bytes.fromhex('0600ff 0901 a101 1500 26ff00 7508 9501')
    tail = bytes.fromhex('0901 8102 c0')
    pad = length - len(head) - len(tail)
    if pad < 0 or pad == 1:
        return None
    fill = (b'\x0a\x01\x00' if pad % 2 else b'') + b'\x09\x01' * (pad // 2 - (1 if pad % 2 else 0))
    return head + fill + tail


STRINGS = {0: b'\x04\x03\x09\x04',
           **{i: usb_string('UAC2 X4 layout - SIMULATED') for i in range(1, 17)}}


def layout(config):
    """Extract standard interface/endpoint facts with descriptor bounds."""
    if len(config) < 9 or config[:2] != b'\x09\x02' or \
            int.from_bytes(config[2:4], 'little') != len(config):
        raise ValueError('configuration framing')
    result, current, off = {}, None, 9
    while off < len(config):
        size, kind = config[off:off + 2]
        if size < 2 or off + size > len(config):
            raise ValueError('descriptor framing')
        d = config[off:off + size]
        if kind == 4:
            if size < 9:
                raise ValueError('interface framing')
            current = (d[2], d[3])
            if current in result:
                raise ValueError('duplicate interface alternate')
            result[current] = (tuple(d[5:8]), [])
        elif kind == 5:
            if size < 7 or current is None:
                raise ValueError('endpoint framing')
            result[current][1].append((d[2], d[3] & 3, d[6],
                                      int.from_bytes(d[4:6], 'little') & 2047))
        off += size
    return result


LAYOUT = layout(CONFIG)


def audio_function(config):
    """The UAC2 entities and streaming alternates of a configuration
    (framing already checked by layout()): the AC interface number; clock
    sources, selectors and multipliers, terminals and feature units by ID;
    and per streaming (interface, alternate) the terminal link, channels,
    subslot, bit resolution and endpoints."""
    ac, entities, alts = None, {}, {}
    current, off = None, 9
    while off < len(config):
        size, kind = config[off], config[off + 1]
        d = config[off:off + size]
        if kind == 4:
            current = (d[2], d[3], d[5], d[6])
            if d[5] == 1 and d[6] == 1 and ac is None:
                ac = d[2]
            if d[5] == 1 and d[6] == 2 and d[3]:
                alts[(d[2], d[3])] = dict(endpoints=[])
        elif kind == 0x24 and current and current[2:] == (1, 1) and size >= 4:
            sub = d[2]
            if sub == 0x0a and size >= 8:
                entities[d[3]] = dict(kind='cs', attributes=d[4], controls=d[5])
            elif sub == 0x0b and size >= 7 and size >= 7 + d[4]:
                entities[d[3]] = dict(kind='cx', inputs=list(d[5:5 + d[4]]), controls=d[5 + d[4]])
            elif sub == 0x0c and size >= 7:
                entities[d[3]] = dict(kind='cm', source=d[4], controls=d[5])
            elif sub == 0x02 and size >= 17:
                entities[d[3]] = dict(kind='it', type=int.from_bytes(d[4:6], 'little'),
                                      clock=d[7], channels=d[8])
            elif sub == 0x03 and size >= 12:
                entities[d[3]] = dict(kind='ot', type=int.from_bytes(d[4:6], 'little'),
                                      source=d[7], clock=d[8])
            elif sub == 0x06 and size >= 10 and (size - 6) % 4 == 0:
                n = (size - 6) // 4
                entities[d[3]] = dict(kind='fu', source=d[4], channels=n - 1,
                                      controls=[int.from_bytes(d[5 + 4 * i:9 + 4 * i], 'little')
                                                for i in range(n)])
            elif sub == 0x04 and size >= 13 + d[4]:
                # Mixer Unit (4.7.2.6): bNrChannels, then bmMixerControls up
                # to bmControls and iMixer, the last two bytes.
                p = d[4]
                entities[d[3]] = dict(kind='mx', unit='mixer', inputs=list(d[5:5 + p]), channels=d[5 + p],
                                      mixer_controls=bytes(d[11 + p:size - 2]))
            elif sub == 0x05 and size >= 7 + d[4]:
                # Selector Unit (4.7.2.7): bmControls after the sources.
                p = d[4]
                entities[d[3]] = dict(kind='mx', unit='selector', inputs=list(d[5:5 + p]), controls=d[5 + p])
            elif sub in (0x04, 0x05) and size >= 5 and size >= 5 + d[4]:
                entities[d[3]] = dict(kind='mx', inputs=list(d[5:5 + d[4]]))
            elif sub in (0x08, 0x09) and size >= 7 and size >= 7 + d[6]:
                # Processing and Extension Units: pass-through on a path.
                entities[d[3]] = dict(kind='xu', inputs=list(d[7:7 + d[6]]))
        elif kind == 0x24 and current and current[2:] == (1, 2) and current[1] and size >= 4:
            a = alts[(current[0], current[1])]
            if d[2] == 1 and size >= 16:
                a.update(terminal=d[3], channels=d[10])
            elif d[2] == 2 and size >= 6 and d[3] == 1:
                a.update(subslot=d[4], bits=d[5])
        elif kind == 5 and current and current[2:] == (1, 2) and current[1]:
            wmax = int.from_bytes(d[4:6], 'little')
            alts[(current[0], current[1])]['endpoints'].append(dict(
                address=d[2], attributes=d[3], max=wmax & 2047, mult=1 + (wmax >> 11 & 3),
                interval=d[6]))
        off += size
    return ac, entities, alts


# ---------------------------------------------------------------------------
# Built profiles: original descriptors from the USB Audio 2.0 specification
# (4.7 AudioControl, 4.9 AudioStreaming, 4.10 endpoints) and the Audio 2.0
# Formats document (Type I).

# OUT packet gaps counted (milliseconds; at High Speed bInterval 1 a running
# stream's packets are 0.125 ms apart, at Full Speed 1 ms).
GAP_MS = (2, 5, 10, 20, 50)
CHANNEL_CONFIG = {1: 0x4, 2: 0x3, 6: 0x3f, 8: 0x63f}


class Builder:
    """One UAC2 function behind an IAD: AC interface 0, then one streaming
    interface per stream() call."""

    def __init__(self, speed, fs_feedback=3):
        self.speed = speed
        # Full Speed feedback endpoint size: 3 (10.14, USB 2.0 5.12.4.2) or
        # 4 (16.16, what Windows 10's own UAC 2.0 driver takes).
        self.fs_feedback = fs_feedback
        self.ac = b''
        self.streams = []
        self.interfaces = 1

    def clock_source(self, cid, attributes=3, controls=0x07):
        # bmAttributes D1..0 3: internal programmable; bmControls D1..0
        # frequency read/write, D3..2 validity read.
        self.ac += bytes((8, 0x24, 0x0a, cid, attributes, controls, 0, 0))

    def clock_selector(self, cid, inputs):
        self.ac += bytes((7 + len(inputs), 0x24, 0x0b, cid, len(inputs))) + bytes(inputs) + bytes((3, 0))

    def clock_multiplier(self, cid, source):
        # Numerator and denominator read only.
        self.ac += bytes((7, 0x24, 0x0c, cid, source, 0x05, 0))

    def input_terminal(self, tid, ttype, clock, channels):
        self.ac += bytes((17, 0x24, 0x02, tid)) + struct.pack('<HBBB', ttype, 0, clock, channels) + \
            struct.pack('<IBHB', CHANNEL_CONFIG.get(channels, 0), 0, 0, 0)

    def output_terminal(self, tid, ttype, source, clock):
        self.ac += bytes((12, 0x24, 0x03, tid)) + struct.pack('<HBBBHB', ttype, 0, source, clock, 0, 0)

    def feature_unit(self, uid, source, channels, fu='full'):
        """Mute (D1..0) and volume (D3..2): read/write (11b) on the master
        and every channel ('full'), the master only, the channels only, or
        readable only (01b); or fu is the bmaControls list, master first.
        'none' adds no unit: returns the source, the id the output terminal
        then takes."""
        if fu == 'none':
            return source
        if isinstance(fu, str):
            master, each = {'full': (0x0f, 0x0f), 'master': (0x0f, 0), 'channels': (0, 0x0f),
                            'readonly': (0x05, 0x05)}[fu]
            fu = [master] + [each] * channels
        self.ac += bytes((6 + 4 * (channels + 1), 0x24, 0x06, uid, source)) + \
            struct.pack('<%dI' % (channels + 1), *fu) + b'\0'
        return uid

    def packet(self, rate, channels, subslot):
        if self.speed == 'high':
            return (-(-rate // 8000) + 1) * channels * subslot
        return (rate // 1000 + 1) * channels * subslot

    def stream(self, terminal, direction, channels, formats, rate, sync='async', sizes=None):
        """formats: [(subslot, bits)], one alternate each; rate: the highest
        rate every alternate must carry; sizes: wMaxPacketSize per alternate
        instead of the computed one."""
        number = self.interfaces
        self.interfaces += 1
        body = bytes((9, 4, number, 0, 0, 1, 2, 0x20, 0))
        ep = (0x80 if direction == 'in' else 0) | number
        fb = direction == 'out' and sync == 'async'
        for alt, (subslot, bits) in enumerate(formats, 1):
            body += bytes((9, 4, number, alt, 2 if fb else 1, 1, 2, 0x20, 0))
            body += bytes((16, 0x24, 0x01, terminal, 0, 1)) + \
                struct.pack('<IBIB', 1, channels, CHANNEL_CONFIG.get(channels, 0), 0)
            body += bytes((6, 0x24, 0x02, 1, subslot, bits))
            attributes = 1 | {'async': 0x04, 'adaptive': 0x08, 'sync': 0x0c}[sync]
            size = sizes[alt - 1] if sizes else self.packet(rate, channels, subslot)
            if size > (1024 if self.speed == 'high' else 1023):
                raise ValueError('alternate exceeds one transaction')
            body += bytes((7, 5, ep, attributes)) + struct.pack('<HB', size, 1)
            body += bytes((8, 0x25, 0x01, 0, 0, 0, 0, 0))
            if fb:
                body += bytes((7, 5, 0x80 | (number + 8), 0x11)) + struct.pack(
                    '<HB', 4 if self.speed == 'high' else self.fs_feedback, 4 if self.speed == 'high' else 1)
        self.streams.append(body)

    def configuration(self):
        header = bytes((9, 0x24, 0x01)) + struct.pack('<HBHB', 0x0200, 0x08, 9 + len(self.ac), 0)
        ac = bytes((9, 4, 0, 0, 0, 1, 1, 0x20, 0)) + header + self.ac
        body = bytes((8, 0x0b, 0, self.interfaces, 1, 0, 0x20, 0)) + ac + b''.join(self.streams)
        return bytes((9, 2)) + struct.pack('<HBBBBB', 9 + len(body), self.interfaces, 1, 0, 0x80, 50) + body


FAMILY_44 = [44100, 88200, 176400]
FAMILY_48 = [48000, 96000, 192000]


def build(name, fu='full', fs_feedback=3):
    """(speed, configuration, {clock source id: rates}, product id). fu: the
    playback feature units' controls (Builder.feature_unit)."""
    if name == 'hs24':
        b = Builder('high')
        b.clock_source(0x10)
        rates = {0x10: sorted(FAMILY_44 + FAMILY_48)}
        b.input_terminal(1, 0x0101, 0x10, 2); b.output_terminal(3, 0x0301, b.feature_unit(2, 1, 2, fu), 0x10)
        b.input_terminal(4, 0x0201, 0x10, 2); b.feature_unit(5, 4, 2); b.output_terminal(6, 0x0101, 5, 0x10)
        b.stream(1, 'out', 2, [(2, 16), (3, 24), (4, 32)], 192000)
        b.stream(6, 'in', 2, [(2, 16), (3, 24)], 192000)
        pid = 0x0201
    elif name == 'multi':
        b = Builder('high')
        b.clock_source(0x10)
        rates = {0x10: [48000, 96000]}
        b.input_terminal(1, 0x0101, 0x10, 8); b.output_terminal(3, 0x0301, b.feature_unit(2, 1, 8, fu), 0x10)
        b.input_terminal(4, 0x0101, 0x10, 6); b.output_terminal(6, 0x0301, b.feature_unit(5, 4, 6, fu), 0x10)
        b.stream(1, 'out', 8, [(2, 16)], 96000)
        b.stream(4, 'out', 6, [(3, 24)], 96000)
        pid = 0x0202
    elif name == 'mic':
        b = Builder('high')
        b.clock_source(0x10)
        rates = {0x10: [16000, 48000, 96000]}
        b.input_terminal(1, 0x0201, 0x10, 1); b.feature_unit(2, 1, 1); b.output_terminal(3, 0x0101, 2, 0x10)
        b.stream(3, 'in', 1, [(2, 16), (3, 24)], 96000)
        pid = 0x0203
    elif name == 'fs':
        b = Builder('full', fs_feedback)
        b.clock_source(0x10)
        rates = {0x10: [44100, 48000]}
        b.input_terminal(1, 0x0101, 0x10, 2); b.output_terminal(3, 0x0301, b.feature_unit(2, 1, 2, fu), 0x10)
        b.input_terminal(4, 0x0201, 0x10, 1); b.output_terminal(5, 0x0101, 4, 0x10)
        b.stream(1, 'out', 2, [(2, 16)], 48000)
        b.stream(5, 'in', 1, [(2, 16)], 48000)
        pid = 0x0204
    elif name == 'clocks':
        b = Builder('high')
        b.clock_source(0x10)
        b.clock_source(0x11)
        b.clock_selector(0x12, [0x10, 0x11])
        b.clock_multiplier(0x13, 0x11)
        rates = {0x10: FAMILY_44, 0x11: FAMILY_48}
        b.input_terminal(1, 0x0101, 0x12, 2); b.output_terminal(3, 0x0301, b.feature_unit(2, 1, 2, fu), 0x12)
        b.input_terminal(4, 0x0201, 0x13, 2); b.output_terminal(5, 0x0101, 4, 0x13)
        b.stream(1, 'out', 2, [(2, 16), (3, 24)], 192000)
        b.stream(5, 'in', 2, [(2, 16)], 192000)
        pid = 0x0205
    elif name == 'adaptive':
        b = Builder('high')
        b.clock_source(0x10, attributes=1, controls=0x05)   # internal fixed, frequency read only
        rates = {0x10: [48000]}
        b.input_terminal(1, 0x0101, 0x10, 2); b.output_terminal(3, 0x0301, b.feature_unit(2, 1, 2, fu), 0x10)
        b.input_terminal(4, 0x0201, 0x10, 2); b.output_terminal(5, 0x0101, 4, 0x10)
        b.stream(1, 'out', 2, [(2, 16)], 48000, sync='adaptive')
        b.stream(5, 'in', 2, [(2, 16)], 48000, sync='sync')
        pid = 0x0206
    elif name == 'cx31993':
        b = Builder('high')
        b.clock_source(9)
        rates = {9: sorted(FAMILY_44 + FAMILY_48 + [352800, 384000])}
        b.input_terminal(1, 0x0101, 9, 2)
        b.feature_unit(2, 1, 2, [0x03, 0x0c, 0x0c])
        b.output_terminal(3, 0x0302, 2, 9)
        b.stream(1, 'out', 2, [(2, 16), (3, 24), (4, 32)], 384000, sync='sync', sizes=[192, 288, 384])
        pid = 0x0207
    elif name == 'alc4080':
        b = Builder('high')
        b.clock_source(40)
        rates = {40: sorted(FAMILY_44 + FAMILY_48)}
        b.input_terminal(1, 0x0101, 40, 8); b.output_terminal(13, 0x0301, b.feature_unit(29, 1, 8, fu), 40)
        b.input_terminal(2, 0x0101, 40, 2); b.output_terminal(14, 0x0302, b.feature_unit(30, 2, 2, fu), 40)
        b.input_terminal(3, 0x0101, 40, 2); b.output_terminal(15, 0x0605, b.feature_unit(32, 3, 2, fu), 40)
        b.input_terminal(4, 0x0603, 40, 2); b.feature_unit(27, 4, 2); b.output_terminal(16, 0x0101, 27, 40)
        b.input_terminal(5, 0x0201, 40, 2); b.feature_unit(28, 5, 2); b.output_terminal(17, 0x0101, 28, 40)
        b.stream(1, 'out', 8, [(2, 16), (3, 24)], 192000)
        b.stream(2, 'out', 2, [(2, 16), (3, 24)], 192000)
        b.stream(3, 'out', 2, [(2, 16), (3, 24)], 192000)
        b.stream(16, 'in', 2, [(2, 16), (3, 24)], 192000)
        b.stream(17, 'in', 2, [(2, 16), (3, 24)], 192000)
        pid = 0x0208
    else:
        raise ValueError('profile')
    return b.speed, b.configuration(), rates, pid


PROFILE_NAMES = ('x4', 'x4full', 'hs24', 'multi', 'mic', 'fs', 'clocks', 'adaptive', 'cx31993', 'alc4080',
                 'uac3dual', 'alc4080cap', 'x1', 'cx31993m')
ALC_FIXTURE = Path(__file__).resolve().parent / 'testdata/alc4080'
X1_FIXTURE = Path(__file__).resolve().parent / 'testdata/sbx1'
CXM_FIXTURE = Path(__file__).resolve().parent / 'testdata/cx31993m'

# Configuration 2 of uac3dual: a USB Audio 3.0 function by its standard
# descriptors (ADC 3.0 4.1: IAD, the AudioControl interface first, then the
# AudioStreaming ones), protocol 30h (IP_VERSION_03_00).
UAC3_CONFIG2 = bytes((9, 2, 35, 0, 2, 2, 0, 0x80, 50,
                      8, 0x0b, 0, 2, 1, 0x01, 0x30, 0,
                      9, 4, 0, 0, 0, 1, 1, 0x30, 0,
                      9, 4, 1, 0, 0, 1, 2, 0x30, 0))
FU_MODES = ('full', 'master', 'channels', 'readonly', 'none')
VOLUME_RANGE = (-0x6000, 0, 0x100)


class Profile:
    def __init__(self, name='x4', vid=None, pid=None, fu='full', vol_range=VOLUME_RANGE, vol_cur=0,
                 fs_feedback=3):
        if name not in PROFILE_NAMES or fu not in FU_MODES or fs_feedback not in (3, 4):
            raise ValueError('profile')
        lo, hi, res = vol_range
        if not -32768 <= lo <= hi <= 32767 or res < 0 or not -32768 <= vol_cur <= 32767:
            raise ValueError('volume range')
        self.name, self.fu, self.vol_range, self.vol_cur = name, fu, (lo, hi, res), vol_cur
        self.vol_at = {}        # logical channel: volume CUR at plug-in instead of vol_cur
        if name == 'alc4080cap':
            self.speed, self.config = 'high', (ALC_FIXTURE / 'config0.bin').read_bytes()
            base = (ALC_FIXTURE / 'device.bin').read_bytes()
            self.vid, self.pid = vid or 0x0b05, pid or 0x1b9b
            # Strings: index 1 product, 3 manufacturer in the capture; serial none.
            # Index 5 is the IAD's iFunction and every interface's iInterface;
            # Windows 10's own UAC 2.0 driver reads it and fails the function
            # (Code 10) when it stalls.
            self.device = base[:8] + struct.pack('<HH', self.vid, self.pid) + base[12:]
            self.qualifier = struct.pack('<BBHBBBBBB', 10, 6, 0x200, 0xef, 2, 1, 64, 1, 0)
            self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('ALC4080 capture - SIMULATED'),
                            3: usb_string('usb-wdm-free SIMULATED'),
                            5: usb_string('ALC4080 audio function - SIMULATED')}
            all44 = sorted(FAMILY_44 + FAMILY_48)
            self.rates = {1: all44, 3: all44, 4: all44, 5: all44, 6: all44 + [352800, 384000], 8: all44}
            self.allowed, self.fu_settable = None, True
        elif name == 'x1':
            self.speed, self.config, self.rates = 'high', (X1_FIXTURE / 'config0.bin').read_bytes(), {}
            base = bytearray((X1_FIXTURE / 'device.bin').read_bytes())
            base[14:17] = bytes((1, 2, 0))
            self.vid, self.pid = vid or 0x041e, pid or 0x3286
            self.device = bytes(base[:8]) + struct.pack('<HH', self.vid, self.pid) + bytes(base[12:])
            self.qualifier = (X1_FIXTURE / 'qualifier.bin').read_bytes()
            self.strings = {0: b'	',
                            **{i: usb_string('UAC2 X1 layout - SIMULATED') for i in range(1, 9)}}
            self.allowed, self.fu_settable = None, True
        elif name == 'cx31993m':
            self.speed, self.config = 'high', (CXM_FIXTURE / 'config0.bin').read_bytes()
            base = bytearray((CXM_FIXTURE / 'device.bin').read_bytes())
            base[14:17] = bytes((1, 2, 0))
            self.vid, self.pid = vid or 0x3302, pid or 0x12c3
            self.device = bytes(base[:8]) + struct.pack('<HH', self.vid, self.pid) + bytes(base[12:])
            self.qualifier = struct.pack('<BBHBBBBBB', 10, 6, 0x200, 0xef, 2, 1, 64, 1, 0)
            self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('usb-wdm-free SIMULATED'),
                            2: usb_string('UAC2 CX31993+MAX97220 layout - SIMULATED')}
            self.rates = {0x1e: sorted(FAMILY_44 + FAMILY_48 + [352800, 384000])}
            self.allowed, self.fu_settable = None, True
        elif name in ('x4', 'x4full'):
            self.speed, self.config, self.rates = 'high', CONFIG, {}
            self.vid, self.pid = vid or 0x041e, pid or 0x3278
            self.device = DEVICE[:8] + struct.pack('<HH', self.vid, self.pid) + DEVICE[12:]
            self.qualifier, self.strings = QUALIFIER, STRINGS
            self.allowed = {(4, 1)} if name == 'x4' else None
            self.fu_settable = name == 'x4full'
        else:
            self.speed, self.config, self.rates, default_pid = build('hs24' if name == 'uac3dual' else name, fu, fs_feedback)
            if name == 'uac3dual':
                default_pid = 0x0230
            self.vid, self.pid = vid or 0xffff, pid or default_pid
            self.device = struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x200, 0xef, 2, 1, 64,
                                      self.vid, self.pid, 0x0001, 1, 2, 0, 1)
            self.qualifier = struct.pack('<BBHBBBBBB', 10, 6, 0x200, 0xef, 2, 1, 64, 1, 0)
            self.strings = {0: b'\x04\x03\x09\x04', 1: usb_string('usb-wdm-free SIMULATED'),
                            2: usb_string('UAC2 %s - SIMULATED' % name)}
            self.allowed, self.fu_settable = None, True
        self.config2 = None
        if name == 'uac3dual':
            self.config2 = UAC3_CONFIG2
            self.device = self.device[:17] + bytes((2,))
        self.layout = layout(self.config)
        self.ac, self.entities, self.alts = audio_function(self.config)
        for cid, e in self.entities.items():
            if e['kind'] == 'cs':
                self.rates.setdefault(cid, [48000])
        self.interfaces = sorted({i for i, _ in self.layout})
        self.hid = hid_interfaces(self.config)

    def set_product(self, text):
        """iProduct's string: text (an empty string gives an empty
        descriptor), or None for no iProduct at all (index 0)."""
        device = bytearray(self.device)
        self.strings = dict(self.strings)   # never the module's shared table
        if text is None:
            device[15] = 0
        else:
            if not device[15]:
                device[15] = 2
            self.strings[device[15]] = usb_string(text)
        self.device = bytes(device)

    def stream_alt(self, interface, alternate):
        """The streaming facts of one alternate, or None."""
        a = self.alts.get((interface, alternate))
        if not a or 'subslot' not in a or 'channels' not in a:
            return None
        t = self.entities.get(a.get('terminal'), {})
        data = [e for e in a['endpoints'] if e['attributes'] & 0x30 != 0x10]
        fbs = [e for e in a['endpoints'] if e['attributes'] & 0x30 == 0x10]
        return dict(frame=a['channels'] * a['subslot'], channels=a['channels'], subslot=a['subslot'],
                    bits=a['bits'], clock=t.get('clock'), data=data[0] if data else None,
                    feedback=fbs[0] if fbs else None)


# ---------------------------------------------------------------------------
# The usbredir device.

class Model:
    def __init__(self, connection, trace, now=time.monotonic, pcm=None, profile=None,
                 fb_ppm=0.0, fb_zero_every=0, fb_stop_after=None, pcm_max=4, vol_stall=0):
        self.connection, self.trace, self.now = connection, trace, now
        self.p = profile or Profile('x4')
        self.fb_ppm, self.fb_zero_every, self.fb_stop_after = fb_ppm, fb_zero_every, fb_stop_after
        self.peer_caps = self.config = 0
        self.id64 = False
        self.alts = {i: 0 for i in self.p.interfaces}
        self.streams = set()
        self.fb, self.in_streams = {}, {}
        self.feedback_id = 0
        self.next_feedback = None
        self.feedback_packets = self.zero_feedback = 0
        self.out_packets = self.out_bytes = self.nonzero = 0
        self.in_packets = self.in_bytes = 0
        self.lengths = collections.Counter()
        self.digest = hashlib.sha256()
        self.first_out = self.last_out = None
        # Gaps between successive OUT packets of one endpoint's running
        # stream (its start or stop, or a reset, begins a new count): a host
        # that left the endpoint with nothing scheduled shows as a gap above
        # the service period. Counted over every OUT endpoint; GAP_MS are the
        # thresholds (gap_ms keys).
        self.gap_prev, self.gap_max = {}, 0.0
        self.gaps = {t: 0 for t in GAP_MS}
        self.next_report = self.now() + 0.25
        self.pcm, self.pcm_bytes, self.pcm_max = pcm, 0, pcm_max * 1024 * 1024
        self.vol_stall = vol_stall
        self.freq = {cid: (48000 if 48000 in r else r[0]) for cid, r in self.p.rates.items()}
        self.selector = {cid: 1 for cid, e in self.p.entities.items() if e['kind'] == 'cx'}
        # Selector Units' positions and Mixer Units' set crosspoints (1/256 dB).
        self.unit_sel = {uid: 1 for uid, e in self.p.entities.items() if e.get('unit') == 'selector'}
        self.mix = {}
        self.mute, self.volume = {}, {}
        self.peak = {}          # channel: largest |sample| received this window (full scale 1.0)
        self.rpeak = {}         # the same after the units in force at each packet
        self.ep_peak = {}       # (endpoint, channel): received, rendered - several outputs
        self.clipped = {}       # channel: rendered samples past full scale, since plug-in

    @property
    def clock(self):
        """The rate of the first playback alternate's clock (the X4's 33)."""
        for (interface, alternate) in sorted(self.p.alts):
            info = self.p.stream_alt(interface, alternate)
            if info and info['data'] and not info['data']['address'] & 0x80:
                return self.rate(info['clock'])
        return None

    def log(self, event, **fields):
        self.trace.write(json.dumps(dict(time=time.time(), monotonic=self.now(),
                                        event=event, simulated=True, **fields)) + '\n')
        self.trace.flush()

    def header(self, kind, length, ident):
        if self.id64:
            return struct.pack('<IIQ', kind, length, ident)
        return struct.pack('<III', kind, length, ident)

    def send(self, kind, ident=0, data=b''):
        self.connection.sendall(self.header(kind, len(data), ident) + data)

    def info(self):
        types, intervals, interfaces = bytearray([255] * 32), bytearray(32), bytearray(32)
        packets = [0] * 32
        types[0] = types[16] = 0
        packets[0] = packets[16] = 64
        numbers, classes, subclasses, protocols = (bytearray(32) for _ in range(4))
        for interface, alternate in self.alts.items():
            if self.config == 2:
                cls, endpoints = (1, 1 if interface == 0 else 2, 0x30), []
            else:
                cls, endpoints = self.p.layout[(interface, alternate)]
            numbers[interface] = interface
            classes[interface], subclasses[interface], protocols[interface] = cls
            for ep, kind, interval, maximum in endpoints:
                slot = (ep & 15) + (16 if ep & 128 else 0)
                types[slot], intervals[slot], interfaces[slot] = kind, interval, interface
                packets[slot] = maximum
        data = bytes(types + intervals + interfaces)
        if self.peer_caps & (1 << CAP_EP_MAX):
            data += struct.pack('<32H', *packets)
        self.send(5, data=data)
        self.send(4, data=struct.pack('<I', len(self.alts)) + numbers + classes + subclasses + protocols)

    def report(self, event):
        elapsed = 0 if self.first_out is None else self.last_out - self.first_out
        self.log(event, packets=self.out_packets, bytes=self.out_bytes,
                 nonzero_bytes=self.nonzero, lengths=dict(self.lengths),
                 first_out=self.first_out, last_out=self.last_out, span_seconds=elapsed,
                 feedback_packets=self.feedback_packets, zero_feedback=self.zero_feedback,
                 in_packets=self.in_packets, in_bytes=self.in_bytes, sha256=self.digest.hexdigest(),
                 gap_max_ms=round(self.gap_max * 1000, 3), gap_ms={str(t): n for t, n in self.gaps.items()},
                 **self.levels())
        self.peak, self.rpeak, self.ep_peak = {}, {}, {}

    # -- the rendered signal -------------------------------------------------
    def path_units(self, terminal):
        """The feature units from a playback input terminal to an output
        terminal, walking forward through units (the first path found)."""
        entities = self.p.entities

        def walk(eid, seen):
            if eid in seen or len(seen) > 32:
                return None
            for oid, e in sorted(entities.items()):
                if e['kind'] == 'ot' and e['source'] == eid:
                    return []
            for uid, e in sorted(entities.items()):
                feeds = (e['kind'] == 'fu' and e['source'] == eid) or \
                    (e['kind'] in ('mx', 'xu') and eid in e['inputs'])
                if feeds:
                    rest = walk(uid, seen | {eid})
                    if rest is not None:
                        return ([uid] if e['kind'] == 'fu' else []) + rest
            return None
        return walk(terminal, frozenset()) or []

    def channel_gain(self, units, channel):
        """(dB, muted) the units apply to logical channel `channel` (1-based);
        the volume silence value mutes."""
        db, muted = 0.0, False
        for uid in units:
            for ch in (0, channel):
                if ch > self.p.entities[uid]['channels']:
                    continue
                muted = muted or bool(self.mute.get((uid, ch), 0))
                if self.p.entities[uid]['controls'][ch] >> 2 & 1:
                    value = self.volume.get((uid, ch), self.p.vol_at.get(ch, self.p.vol_cur))
                    if value == -32768:
                        muted = True
                    else:
                        db += value / 256
        return db, muted

    def playback_units(self):
        """The feature units on the active playback alternate's path."""
        for (interface, alternate) in sorted(self.p.alts):
            info = self.p.stream_alt(interface, alternate)
            if info and info['data'] and not info['data']['address'] & 0x80 and \
                    self.alts.get(interface) == alternate:
                terminal = self.p.alts[(interface, alternate)].get('terminal')
                return self.path_units(terminal) if terminal else []
        return []

    def levels(self):
        """Received and rendered peaks of this window, per channel, dBFS,
        with the units' gain in force now."""
        if not self.peak:
            return {}
        units = self.playback_units()
        received, rendered, gains = [], [], []
        dbfs = lambda peak: round(20 * math.log10(peak), 2) if peak > 0 else None
        for ch in sorted(self.peak):
            db, muted = self.channel_gain(units, ch + 1)
            received.append(dbfs(self.peak[ch]))
            rendered.append(dbfs(self.rpeak.get(ch, 0.0)))
            gains.append(None if muted else round(db, 2))
        result = dict(received_peak_dbfs=received, rendered_peak_dbfs=rendered,
                      unit_gain_db=gains, units=units,
                      rendered_clipped=[self.clipped.get(ch, 0) for ch in sorted(self.peak)])
        if len({ep for ep, _ in self.ep_peak}) > 1:
            result['endpoint_peaks_dbfs'] = {
                '%02x' % ep: [[dbfs(self.ep_peak[(e, ch)][0]), dbfs(self.ep_peak[(e, ch)][1])]
                              for e, ch in sorted(self.ep_peak) if e == ep]
                for ep in sorted({e for e, _ in self.ep_peak})}
        return result

    def measure(self, payload, info, units, ep=None):
        """Track each channel's largest sample of one OUT packet, as
        received and as rendered by the units in force now."""
        subslot, channels = info['subslot'], info['channels']
        full = float(1 << (8 * subslot - 1))
        if subslot in (2, 4):
            samples = array.array('h' if subslot == 2 else 'i', payload)
            if sys.byteorder != 'little':
                samples.byteswap()
        else:
            samples = [int.from_bytes(payload[i:i + 3], 'little', signed=True)
                       for i in range(0, len(payload) - 2, 3)]
        for ch in range(channels):
            column = samples[ch::channels]
            if column:
                top = max(max(column), -min(column)) / full
                db, muted = self.channel_gain(units, ch + 1)
                out = 0.0 if muted else top * 10 ** (db / 20)
                if out > 1.0:
                    # Samples the units' gain (above 0 dB) takes past full scale.
                    limit = full / 10 ** (db / 20)
                    self.clipped[ch] = self.clipped.get(ch, 0) + sum(abs(s) > limit for s in column)
                self.peak[ch] = max(self.peak.get(ch, 0.0), top)
                self.rpeak[ch] = max(self.rpeak.get(ch, 0.0), out)
                if ep is not None:
                    r, o = self.ep_peak.get((ep, ch), (0.0, 0.0))
                    self.ep_peak[(ep, ch)] = (max(r, top), max(o, out))

    def reset_streams(self):
        self.streams.clear()
        self.gap_prev.clear()
        self.fb.clear()
        self.in_streams.clear()
        self.next_feedback = None

    # -- clocks ------------------------------------------------------------
    def rate(self, clock, depth=0):
        """The sample rate a clock entity delivers through selectors and
        multipliers (numerator/denominator 1/1), or None."""
        e = self.p.entities.get(clock)
        if e is None or depth > 8:
            return None
        if e['kind'] == 'cs':
            return self.freq.get(clock)
        if e['kind'] == 'cx':
            return self.rate(e['inputs'][self.selector[clock] - 1], depth + 1)
        if e['kind'] == 'cm':
            return self.rate(e['source'], depth + 1)
        return None

    def cluster(self, eid, depth=0):
        """The number of logical channels an entity's output carries."""
        e = self.p.entities.get(eid)
        if e is None or depth > 16:
            return 0
        if e['kind'] in ('it', 'fu') or e.get('unit') == 'mixer':
            return e['channels']
        if e['kind'] in ('mx', 'xu') and e['inputs']:
            return self.cluster(e['inputs'][0], depth + 1)
        return 0

    def owner(self, ep):
        for interface, alternate in self.alts.items():
            info = self.p.stream_alt(interface, alternate) if alternate else None
            if info and ep in {e['address'] for e in (info['data'], info['feedback']) if e}:
                return interface, alternate, info
        return None

    # -- control transfers -------------------------------------------------
    def control(self, ident, packet):
        if len(packet) < 10:
            raise ValueError('short control header')
        ep, request, rt, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload = packet[10:]
        if len(payload) != (0 if rt & 128 else length):
            raise ValueError('control length mismatch')
        p = self.p
        data, status = b'', 4
        if request == 6 and rt == 0x80:
            kind, number = value >> 8, value & 255
            data = p.device if kind == 1 and number == 0 else \
                p.config if kind == 2 and number == 0 else \
                p.config2 if kind == 2 and number == 1 and p.config2 else \
                p.qualifier if kind == 6 and number == 0 else \
                p.strings.get(number) if kind == 3 else None
            status, data = (0, data) if data is not None else (4, b'')
        elif request == 0 and rt in (0x80, 0x81, 0x82):
            status, data = 0, b'\0\0'
        elif request == 8 and rt == 0x80:
            status, data = 0, bytes((self.config,))
        elif request == 10 and rt == 0x81 and index in self.alts:
            status, data = 0, bytes((self.alts[index],))
        elif request == 1 and rt == 2 and value == 0:
            status = 0
        elif request == 6 and rt == 0x81 and value >> 8 == 0x22 and index in p.hid and self.config == 1:
            data = hid_report(p.hid[index])
            status, data = (0, data) if data is not None else (4, b'')
        elif request == 0x0a and rt == 0x21 and index in p.hid and self.config == 1:
            status = 0      # SET_IDLE (HID 1.11 7.2.4)
        elif rt in (0xa1, 0x21) and index & 255 == p.ac and self.config == 1:
            status, data = self.entity_request(request, rt == 0xa1, index >> 8, value >> 8,
                                               value & 255, payload)
        data = data[:length]
        actual = len(data) if rt & 128 else (0 if status else length)
        self.send(100, ident, struct.pack('<BBBBHHH', ep, request, rt, status,
                                        value, index, actual) + data)
        self.log('control', request=request, request_type=rt, value=value, index=index,
                 requested=length, status=status, actual=actual)

    def entity_request(self, request, get, entity, selector, channel, payload):
        """UAC2 5.2.5: CUR is request 1, RANGE request 2."""
        e = self.p.entities.get(entity)
        if e is None:
            return 4, b''
        if e['kind'] == 'cs' and not channel:
            rates = self.p.rates[entity]
            if selector == 1 and get and request == 2:
                return 0, struct.pack('<H', len(rates)) + b''.join(struct.pack('<III', r, r, 0) for r in rates)
            if selector == 1 and get and request == 1:
                return 0, struct.pack('<I', self.freq[entity])
            if selector == 1 and not get and request == 1 and len(payload) == 4 and e['controls'] & 3 == 3:
                rate = struct.unpack('<I', payload)[0]
                if rate in rates:
                    self.freq[entity] = rate
                    self.log('clock', entity=entity, rate=rate)
                    return 0, b''
            if selector == 2 and get and request == 1:
                return 0, b'\x01'
        elif e['kind'] == 'cx' and selector == 1 and not channel and request == 1:
            if get:
                return 0, bytes((self.selector[entity],))
            if len(payload) == 1 and 1 <= payload[0] <= len(e['inputs']):
                self.selector[entity] = payload[0]
                self.log('clock_selector', entity=entity, input=payload[0])
                return 0, b''
        elif e['kind'] == 'cm' and selector in (1, 2) and not channel and get and request == 1:
            return 0, struct.pack('<H', 1)
        elif e.get('unit') == 'selector' and selector == 1 and not channel and request == 1:
            # Selector Control (5.2.5.4): the selected input pin, 1-based.
            # Windows' own UAC 2.0 driver reads it (GET CUR) on every
            # Selector Unit of the topology.
            if get:
                return 0, bytes((self.unit_sel[entity],))
            if e['controls'] & 3 == 3 and len(payload) == 1 and 1 <= payload[0] <= len(e['inputs']):
                self.unit_sel[entity] = payload[0]
                self.log('selector_unit', entity=entity, input=payload[0])
                return 0, b''
        elif e.get('unit') == 'mixer' and selector == 1 and request in (1, 2):
            # Mixer Control (5.2.5.5): one 1/256 dB value per crosspoint,
            # numbered over input channels x output channels. Each is
            # readable; bmMixerControls marks the programmable ones (first
            # bit the MSB of the first byte). Windows' own UAC 2.0 driver
            # reads them (GET CUR) on every Mixer Unit of the topology.
            inputs = sum(self.cluster(i) for i in e['inputs'])
            if channel >= inputs * e['channels']:
                return 4, b''
            bits = e['mixer_controls']
            settable = channel // 8 < len(bits) and bits[channel // 8] >> (7 - channel % 8) & 1
            if get and request == 1:
                return 0, struct.pack('<h', self.mix.get((entity, channel), 0))
            if get and request == 2:
                return 0, struct.pack('<Hhhh', 1, -15360 if settable else 0, 0, 256 if settable else 0)
            if not get and request == 1 and settable and len(payload) == 2:
                value = struct.unpack('<h', payload)[0]
                if -15360 <= value <= 0 and value % 256 == 0 or value == -32768:
                    self.mix[(entity, channel)] = value
                    self.log('mixer', entity=entity, control=channel, value=value)
                    return 0, b''
        elif e['kind'] == 'fu' and channel <= e['channels'] and selector in (1, 2):
            # bmaControls: mute D1..0, volume D3..2; 1 read only, 3 read/write
            # (0 absent and 2 reserved stall).
            store, key = (self.mute if selector == 1 else self.volume), (entity, channel)
            access = e['controls'][channel] >> (2 * (selector - 1)) & 3
            lo, hi, res = self.p.vol_range
            if not access & 1:
                return 4, b''
            if get and request == 1:
                if selector == 2 and self.vol_stall > 0:
                    # --vol-cur-stall: this many volume GET CURs stall first.
                    self.vol_stall -= 1
                    self.log('feature_stalled', entity=entity, channel=channel)
                    return 4, b''
                return 0, struct.pack('<B' if selector == 1 else '<h',
                                      store.get(key, 0 if selector == 1 else
                                                self.p.vol_at.get(channel, self.p.vol_cur)))
            if get and request == 2 and selector == 2:
                return 0, struct.pack('<Hhhh', 1, lo, hi, res)
            if not get and request == 1 and len(payload) == selector and self.p.fu_settable:
                value = payload[0] if selector == 1 else struct.unpack('<h', payload)[0]
                silence = selector == 2 and value == -32768
                # RES 0 on a range wider than one value: steps of 1, as the
                # driver reads it (Linux does the same).
                step = res if res > 0 else (1 if hi > lo else 0)
                off_step = selector == 2 and (step > 0 and (value - lo) % step or step == 0 and value != lo)
                if access != 3 or (selector == 1 and value > 1) or \
                        (selector == 2 and not silence and (not lo <= value <= hi or off_step)):
                    self.log('feature_refused', entity=entity, selector=selector, channel=channel, value=value)
                    return 4, b''
                store[key] = value
                self.log('feature', entity=entity, selector=selector, channel=channel, value=value,
                         db=value / 256 if selector == 2 and not silence else None)
                return 0, b''
        return 4, b''

    # -- usbredir messages -------------------------------------------------
    def handle(self, kind, ident, packet):
        if kind == 3:
            if packet:
                raise ValueError('reset length')
            self.reset_streams()
            self.config = 0
            self.alts = {i: 0 for i in self.p.interfaces}
        elif kind in (6, 7):
            if len(packet) != (1 if kind == 6 else 0):
                raise ValueError('configuration length')
            status = 0
            if kind == 6:
                status = 0 if packet[0] in (0, 1) or (packet[0] == 2 and self.p.config2) else 4
                self.log('set_configuration', value=packet[0], status=status)
                if not status:
                    self.reset_streams()
                    self.config = packet[0]
                    if packet[0] == 2:
                        self.alts = {0: 0, 1: 0}
                    else:
                        self.alts = {i: 0 for i in self.p.interfaces}
                    self.info()
            self.send(8, ident, bytes((status, self.config)))
        elif kind in (9, 10):
            if len(packet) != (2 if kind == 9 else 1):
                raise ValueError('alternate length')
            interface = packet[0]
            alternate = packet[1] if kind == 9 else self.alts.get(interface, 0)
            known = interface in self.alts and (interface, alternate) in self.p.layout
            allowed = self.p.allowed is None or not alternate or (interface, alternate) in self.p.allowed
            status = 0 if known and allowed else 4
            if kind == 9 and not status:
                self.alts[interface] = alternate
                for ep in list(self.streams):
                    if self.owner(ep) is None:
                        self.stop(ep)
                self.info()
                self.log('alternate', interface=interface, alternate=alternate)
            self.send(11, ident, bytes((status, interface, alternate)))
        elif kind in (12, 13):
            if len(packet) != (3 if kind == 12 else 1):
                raise ValueError('iso stream header')
            ep = packet[0]
            own = self.owner(ep) if self.config == 1 else None
            status = 0 if own else 4
            if kind == 12 and not status:
                if not packet[1] or not packet[2]:
                    status = 4
                else:
                    self.start(ep, own)
            elif kind == 13:
                self.stop(ep)
                status = 0
                self.report('iso_stop')
                if self.pcm is not None:
                    self.pcm.flush()
            self.send(14, ident, bytes((status, ep)))
            self.log('iso_stream', start=kind == 12, endpoint=ep, status=status,
                     packets_per_urb=packet[1] if kind == 12 else 0,
                     urbs=packet[2] if kind == 12 else 0)
        elif kind == 100:
            self.control(ident, packet)
        elif kind == 102:
            self.iso_out(packet)
        elif kind in (15, 16):
            if len(packet) != 1:
                raise ValueError('interrupt header')
            # No audio-control interrupt notifications are generated.
            self.send(17, ident, bytes((0, packet[0])))
        elif kind == 21:
            if packet:
                raise ValueError('cancel length')
        elif kind == 24:
            pass
        else:
            raise ValueError(f'unsupported usbredir message {kind}')

    def period(self, endpoint):
        """Seconds between an endpoint's service opportunities."""
        unit = 0.000125 if self.p.speed == 'high' else 0.001
        return unit * (1 << (max(1, endpoint['interval']) - 1))

    def start(self, ep, own):
        interface, alternate, info = own
        self.streams.add(ep)
        self.gap_prev.pop(ep, None)
        now = self.now()
        if info['feedback'] and info['feedback']['address'] == ep:
            self.fb[ep] = dict(info=info, next=now, start=now, period=self.period(info['feedback']), count=0)
            self.feedback_id = 0
            self.next_feedback = now
        elif ep & 0x80:
            self.in_streams[ep] = dict(info=info, next=now, period=self.period(info['data']),
                                       ident=0, acc=0.0, phase=0)
        self.log('stream_start', endpoint=ep, interface=interface, alternate=alternate,
                 channels=info['channels'], subslot=info['subslot'], bits=info['bits'],
                 rate=self.rate(info['clock']), pcm_offset=self.pcm_bytes)

    def stop(self, ep):
        self.streams.discard(ep)
        self.gap_prev.pop(ep, None)
        self.in_streams.pop(ep, None)
        if self.fb.pop(ep, None) is not None and not self.fb:
            self.next_feedback = None

    def iso_out(self, packet):
        if len(packet) < 4:
            raise ValueError('short iso packet')
        ep, _, length = struct.unpack('<BBH', packet[:4])
        payload = packet[4:]
        own = None if ep & 0x80 or ep not in self.streams else self.owner(ep)
        if own is None or len(payload) != length:
            raise ValueError('OUT iso framing/state')
        data = own[2]['data']
        if length > data['max'] * data['mult'] or length % own[2]['frame']:
            raise ValueError('OUT iso framing/state')
        stamp = self.now()
        if self.first_out is None:
            self.first_out = stamp
        self.last_out = stamp
        if ep in self.gap_prev:
            gap = stamp - self.gap_prev[ep]
            self.gap_max = max(self.gap_max, gap)
            for t in GAP_MS:
                if gap * 1000 >= t:
                    self.gaps[t] += 1
        self.gap_prev[ep] = stamp
        self.out_packets += 1
        self.out_bytes += length
        self.nonzero += sum(byte != 0 for byte in payload)
        self.lengths[length] += 1
        self.digest.update(payload)
        terminal = self.p.alts[(own[0], own[1])].get('terminal')
        self.measure(payload, own[2], self.path_units(terminal) if terminal else [], ep)
        if self.pcm is not None:
            if self.pcm_bytes + length > self.pcm_max:
                raise ValueError('simulated PCM capture exceeds --pcm-max')
            self.pcm.write(payload)
            self.pcm_bytes += length
        if stamp >= self.next_report:
            self.report('iso_out_totals')
            self.next_report = stamp + 0.25

    # -- device-to-host streams ---------------------------------------------
    def feedback_value(self, info):
        rate = (self.rate(info['clock']) or 48000) * (1 + self.fb_ppm / 1e6)
        if self.p.speed == 'high':
            return struct.pack('<I', int(round(rate / 8000 * 65536)))     # 16.16, per microframe
        if info['feedback']['max'] >= 4:
            # A 4-byte Full Speed feedback endpoint: 16.16 samples per frame,
            # the form Windows 10's own UAC 2.0 driver reads there.
            return struct.pack('<I', int(round(rate / 1000 * 65536)))
        return struct.pack('<I', int(round(rate / 1000 * 16384)))[:3]     # 10.14, per frame

    def feedback(self):
        stamp = self.now()
        for ep, s in list(self.fb.items()):
            if stamp < s['next']:
                continue
            due = min(64, int((stamp - s['next']) / s['period'] + 1e-9) + 1)
            for _ in range(due):
                s['count'] += 1
                lost = (self.fb_stop_after is not None and s['next'] - s['start'] >= self.fb_stop_after) or \
                    (self.fb_zero_every and s['count'] % self.fb_zero_every == 0)
                data = b'' if lost else self.feedback_value(s['info'])
                self.send(102, self.feedback_id, struct.pack('<BBH', ep, 0, len(data)) + data)
                self.feedback_id = (self.feedback_id + 1) & 0xffffffff
                if lost:
                    self.zero_feedback += 1
                else:
                    self.feedback_packets += 1
                s['next'] += s['period']
            self.next_feedback = s['next']
            if stamp - s['next'] > 0.2:
                raise ValueError('simulated feedback scheduler overrun')

    def capture(self):
        stamp = self.now()
        for ep, s in self.in_streams.items():
            if stamp < s['next']:
                continue
            info = s['info']
            rate = self.rate(info['clock']) or 48000
            limit = info['data']['max'] * info['data']['mult'] // info['frame']
            for _ in range(min(64, int((stamp - s['next']) / s['period'] + 1e-9) + 1)):
                s['acc'] += rate * s['period']
                samples = min(int(s['acc']), limit)
                s['acc'] -= samples
                data = sine(samples, info, rate, s['phase'])
                s['phase'] += samples
                self.send(102, s['ident'], struct.pack('<BBH', ep, 0, len(data)) + data)
                s['ident'] = (s['ident'] + 1) & 0xffffffff
                self.in_packets += 1
                self.in_bytes += len(data)
                s['next'] += s['period']

    def next_event(self):
        times = [s['next'] for s in self.fb.values()] + [s['next'] for s in self.in_streams.values()]
        return min(times) if times else None

    def run(self):
        self.send(0, data=b'usb-wdm-free UAC2 SIMULATED'.ljust(64, b'\0') + struct.pack('<I', CAPS))
        buffer = bytearray()
        connected = False
        while True:
            self.feedback()
            self.capture()
            nxt = self.next_event()
            timeout = 0.05 if nxt is None else max(0, min(0.001, nxt - self.now()))
            readable, _, _ = select.select([self.connection], [], [], timeout)
            if not readable:
                continue
            data = self.connection.recv(65536)
            if not data:
                raise EOFError
            buffer.extend(data)
            while True:
                size = 16 if self.id64 else 12
                if len(buffer) < size:
                    break
                kind, length, ident = struct.unpack('<IIQ' if self.id64 else '<III', buffer[:size])
                if length > 65546:
                    raise ValueError('message bound')
                if len(buffer) < size + length:
                    break
                packet = bytes(buffer[size:size + length])
                del buffer[:size + length]
                if not connected:
                    if kind or ident or length < 64 or length > 4096:
                        raise ValueError('hello framing')
                    self.peer_caps = int.from_bytes(packet[64:68], 'little') if length >= 68 else 0
                    self.id64 = bool(self.peer_caps & CAPS & (1 << CAP_ID64))
                    self.info()
                    d = self.p.device
                    device = struct.pack('<BBBBHH', 2 if self.p.speed == 'high' else 1,
                                         d[4], d[5], d[6], self.p.vid, self.p.pid)
                    if self.peer_caps & (1 << CAP_VERSION):
                        device += d[12:14]
                    self.send(1, data=device)
                    self.log('connected', model='SIMULATED UAC2 %s' % self.p.name, speed=self.p.speed,
                             fu=self.p.fu, vol_range=self.p.vol_range, vol_cur=self.p.vol_cur,
                             vid='%04x' % self.p.vid, pid='%04x' % self.p.pid,
                             config_sha256=hashlib.sha256(self.p.config).hexdigest())
                    connected = True
                else:
                    self.handle(kind, ident, packet)


def sine(samples, info, rate, phase):
    """1 kHz at half full scale on every channel, left-justified in the subslot."""
    out = bytearray()
    shift = 8 * info['subslot'] - info['bits']
    peak = (1 << (info['bits'] - 2)) - 1
    mask = (1 << (8 * info['subslot'])) - 1
    for n in range(phase, phase + samples):
        value = int(round(peak * math.sin(2 * math.pi * 1000 * n / rate))) << shift
        out += ((value & mask).to_bytes(info['subslot'], 'little')) * info['channels']
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=44413)
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--profile', choices=PROFILE_NAMES, default='x4')
    ap.add_argument('--vid', type=lambda s: int(s, 16))
    ap.add_argument('--pid', type=lambda s: int(s, 16))
    ap.add_argument('--fb-ppm', type=float, default=0.0, help='feedback offset from the clock, ppm')
    ap.add_argument('--fs-feedback', type=int, choices=(3, 4), default=3,
                    help='fs profile: feedback endpoint bytes, 3 (10.14) or 4 (16.16, as Windows 10 takes)')
    ap.add_argument('--fb-zero-every', type=int, default=0,
                    help='every Nth feedback packet is zero-length (lost)')
    ap.add_argument('--fb-stop-after', type=float,
                    help='seconds into a stream after which every feedback packet is zero-length')
    ap.add_argument('--pcm', type=Path, help='optional bounded SIMULATED received PCM capture')
    ap.add_argument('--pcm-max', type=int, default=4, choices=range(1, 257), metavar='MIB',
                    help='the capture bound in MiB (default 4, at most 256)')
    ap.add_argument('--rates', help='comma-separated rates every clock source offers instead of the '
                    "profile's (each must be one of them), e.g. 44100 so KMixer opens the device at "
                    'the client rate')
    ap.add_argument('--fu', choices=FU_MODES, default='full',
                    help='playback feature units of a built profile (not x4, x4full or cx31993)')
    ap.add_argument('--vol-range', default='%d:%d:%d' % VOLUME_RANGE,
                    help='volume RANGE MIN:MAX:RES in 1/256 dB')
    ap.add_argument('--vol-cur', type=int, default=0, help='volume CUR at plug-in, 1/256 dB')
    ap.add_argument('--product', help="iProduct's string instead of the profile's ('' an empty one)")
    ap.add_argument('--no-product', action='store_true', help='iProduct 0: no product string')
    ap.add_argument('--vol-cur-at', action='append', default=[], metavar='CH=V',
                    help='volume CUR at plug-in of logical channel CH (0 master) of every feature unit')
    ap.add_argument('--vol-cur-stall', type=int, default=0, metavar='N',
                    help='the first N volume GET CUR requests stall')
    args = ap.parse_args()
    if args.fu != 'full' and args.profile in ('x4', 'x4full', 'cx31993'):
        ap.error('--fu applies to the built profiles other than cx31993')
    vol_range = tuple(int(v) for v in args.vol_range.split(':'))
    if len(vol_range) != 3:
        ap.error('--vol-range MIN:MAX:RES')
    if args.fs_feedback != 3 and args.profile != 'fs':
        ap.error('--fs-feedback applies to the fs profile')
    profile = Profile(args.profile, args.vid, args.pid, fu=args.fu, vol_range=vol_range, vol_cur=args.vol_cur,
                      fs_feedback=args.fs_feedback)
    if args.no_product:
        profile.set_product(None)
    elif args.product is not None:
        profile.set_product(args.product)
    for item in args.vol_cur_at:
        try:
            ch, value = (int(v) for v in item.split('='))
        except ValueError:
            ap.error('--vol-cur-at CH=V')
        if not 0 <= ch <= 255 or not -32768 <= value <= 32767:
            ap.error('--vol-cur-at CH=V')
        profile.vol_at[ch] = value
    if args.rates:
        try:
            only = [int(r) for r in args.rates.split(',')]
        except ValueError:
            ap.error('--rates R[,R...]')
        offered = set(r for rates in profile.rates.values() for r in rates)
        if any(r not in offered for r in only):
            ap.error('--rates: the profile offers %s' % ','.join(str(r) for r in sorted(offered)))
        for cid, rates in profile.rates.items():
            keep = [r for r in rates if r in only]
            if not keep:
                ap.error('--rates: clock %d offers none of %s' % (cid, args.rates))
            profile.rates[cid] = keep
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'SIMULATED UAC2 {args.profile} listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            connection.settimeout(3)
            pcm = args.pcm.open('xb') if args.pcm else None
            model = Model(connection, trace, pcm=pcm, profile=profile, fb_ppm=args.fb_ppm,
                          fb_zero_every=args.fb_zero_every, fb_stop_after=args.fb_stop_after,
                          pcm_max=args.pcm_max, vol_stall=args.vol_cur_stall)
            try:
                try:
                    model.run()
                except (EOFError, ConnectionResetError, BrokenPipeError):
                    model.report('disconnected')
                except Exception as error:
                    model.log('fatal', error=str(error))
                    raise
            finally:
                if pcm:
                    pcm.close()


if __name__ == '__main__':
    main()

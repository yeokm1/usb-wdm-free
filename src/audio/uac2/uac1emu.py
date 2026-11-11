#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""SIMULATED USB Audio 1.0 devices over localhost usbredir (test aid only).

Original protocol model from the USB Device Class Definition for Audio
Devices 1.0 (descriptors 4.3 to 4.6, requests 5.2), its Audio Data Formats
document (Type I, 2.2), USB 2.0 chapter 9 and 5.12.4.2 (Full Speed 10.14
feedback), and SPICE usb-redirection-protocol.txt v0.7. The usbredir side
(hello, endpoint info, iso streams, capture sine, feedback scheduling, the
received and rendered levels, packet gaps) is uacemu.py's, reused by
subclassing; what is UAC 1.0 is here. No physical device, audio input or
audio output is accessed. The shapes below are models, not captures: no
identity string or id of a real device is used.

Profiles (--profile):
  headset   Full Speed headset shape (a USB headset such as a 046D:0A64-class
            one): playback 2 channels 16-bit 48/44.1 kHz on an ADAPTIVE OUT
            endpoint; mono capture 16-bit 48/44.1/16 kHz on an
            ASYNCHRONOUS IN endpoint; a sidetone path (microphone, Feature
            Unit, Mixer Unit with the playback channels) into the speaker's
            Feature Unit (master mute, per-channel volume); the capture
            Feature Unit with mute, volume and AGC; a HID interface with a
            consumer-control report (Volume Up, Volume Down, Mute).
  dacd      Full Speed playback-only DAC, ADAPTIVE OUT, 16-bit stereo, a
            discrete rate table 32/44.1/48 kHz, master mute and volume.
  dacc      the same with a CONTINUOUS rate range (bSamFreqType 0) of 8 to
            48 kHz and per-channel volume only (no master, no mute).
  async     Full Speed playback-only, ASYNCHRONOUS OUT with its sync
            (feedback) endpoint: the data endpoint's bSynchAddress names it,
            it carries 3 bytes 10.14 (USB 2.0 5.12.4.2; UAC 1.0 3.7.2.2),
            bRefresh --refresh (default 2: polled every 4 ms) and bmAttributes
            --sync-attr (default 0x01, the UAC 1.0 form; 0x11 marks it a
            USB 2.0 feedback endpoint). Alternate 1 16-bit, alternate 2
            24-bit (3-byte subframe), 44.1/48 kHz. Its Feature Unit has
            2-byte bmaControls: master mute, volume, bass, treble, loudness.
  hs        High Speed UAC 1.0 (as USB Audio 1.0 descriptors at High Speed,
            bInterval 4: one packet per millisecond): playback ADAPTIVE,
            alternate 1 16-bit, alternate 2 24-bit (3-byte subframe), 44.1
            to 96 kHz (packets to 582 bytes, beyond Full Speed's 1023 only
            in sum); capture 2 channels 24-bit and 16-bit, ASYNCHRONOUS.
  mic       Full Speed capture-only microphone: mono 16-bit, 16/32/48 kHz,
            ASYNCHRONOUS IN; master mute, volume and AGC.
  cammic    a webcam's UAC 1.0 microphone shape: High Speed, device class
            EF/02/01, interface 0 a vendor-specific stand-in for the video
            function (no endpoints), the audio function behind an IAD at
            interfaces 1 (AudioControl) and 2 (AudioStreaming), mono 16-bit
            16/24/32/48 kHz at bInterval 4.
  dual      two configurations, as real dual-mode devices do it (e.g. a
            recorder interface listing configuration 1 as UAC 1.0, 48 kHz at
            most, and configuration 2 as UAC 2.0): High Speed, configuration
            1 is a UAC 1.0 function (playback adaptive 16/24-bit 44.1/48
            kHz, capture 16-bit 48 kHz, bInterval 4), configuration 2 is
            uacemu.py's hs24 UAC 2.0 function. The log records which
            configuration the host selects; each configuration answers its
            own class's requests. No real device is known that mixes UAC 1.0
            and UAC 2.0 alternates on one interface (the AudioControl
            interface fixes the function's class version), so none is
            modelled.
  play2     the owner's Creative Sound Blaster Play! 2 (041E:323D,
            testdata/play2) replayed: its device, configuration and HID
            report descriptors as read, strings replaced by SIMULATED
            labels, serial index zero; Full Speed, stereo capture and
            playback 16-bit 44.1/48 kHz, ASYNCHRONOUS OUT with its 3-byte
            sync endpoint (bRefresh 5), and the answers the unit gave
            Windows' own driver (volume ranges; Selector Unit 12 and Mixer
            Unit 9 stall).
  logi      the owner's Logi USB Headset (046D:0A64, testdata/logi0a64)
            replayed the same way: ADAPTIVE OUT 2 channels at 8 to 192
            kHz (768-byte packets) behind a 1-channel input terminal, mono
            capture 8 to 48 kHz, master-only units, a sidetone path through
            Mixer Unit 12 (which stalls), its HID report.
Formats and synchronisation beyond 16-bit PCM (Full Speed; uac1 0.1.0.1
refused the first three, uac1 runs them all since docs/design/uac2.md
26.12):
  pcm8      playback only, one alternate: wFormatTag 2 (PCM8), bSubframeSize
            1, bBitResolution 8 (unsigned), stereo 44.1/48 kHz, ADAPTIVE.
  float     the same with wFormatTag 3 (IEEE_FLOAT), bSubframeSize 4,
            bBitResolution 32.
  adsrc     capture only: an ADAPTIVE IN data endpoint 0x81 whose
            bSynchAddress names the sync OUT endpoint 0x01 (UAC 1.0 Table
            4-22; 3 bytes, bRefresh 2), mono 16-bit 16/44.1/48 kHz. What the
            host sends there is counted, checked against the rate and
            logged (synch_out); the source produces its samples at the Ff
            last received (the nominal before one, UAC 1.0 3.7.2.2).
  alaw      playback 2 channels A-law (wFormatTag 4, 1 byte) 44.1/48 kHz
            ADAPTIVE, capture 1 channel A-law 16/48 kHz ASYNCHRONOUS.
  mulaw     the same in mu-law (wFormatTag 5).
  pcm8in    capture only: mono PCM8 in a 2-byte subframe (bBitResolution
            8, left-justified), 16/44.1/48 kHz, ASYNCHRONOUS.
  floatin   capture only: stereo IEEE_FLOAT 44.1/48 kHz, SYNCHRONOUS.
  mpoin     capture only: stereo 16-bit PCM 44.1/48 kHz, SYNCHRONOUS, with
            MaxPacketsOnly (class endpoint bmAttributes D7): every packet
            is wMaxPacketSize (52 frames) with zero padding after the
            audio; at 48 kHz the audio follows Formats 1.0 2.2.1's 48, 47,
            49, 48 sizes. mpoin48: the same at 48 kHz only (98 SE's KMixer
            opens a capture pin at its lowest rate).
  mpoout    playback only: stereo PCM8 44.1/48 kHz ADAPTIVE with
            MaxPacketsOnly: each non-null packet must be wMaxPacketSize
            (50 frames), the audio first (counted as mpo_bad_length when
            not); the model reads the audio up to its last nonzero frame,
            and at least the cadence's whole frames (PCM8 0x00 is -1.0 as
            well as padding; counted as mpo_zero_audio).
Every playback stream's tone frequency (zero crossings of channel 1,
decoded from any of these formats) is logged as tone_hz.
Aliases (the names agent uac1 planned): headset1 = headset, speaker1 =
dacd, mic1 = cammic.

--speed full|high moves a built Full Speed profile to High Speed
(bInterval 4, a device qualifier) or back; a replayed device keeps its
speed.

Requests (UAC 1.0 5.2): SET_CUR 01h, GET_CUR 81h, GET_MIN 82h, GET_MAX 83h,
GET_RES 84h; SET_MIN/MAX/RES and GET_MEM/STAT stall. Feature Unit (wIndex
entity << 8 | AC interface, wValue control << 8 | channel): mute and AGC
(CUR, 1 byte), volume (CUR/MIN/MAX/RES, 2 bytes, 1/256 dB, 8000h silence),
bass, mid, treble (1 byte, 1/4 dB), delay (2 bytes), bass boost and
loudness (1 byte), each only where the unit's bmaControls has its bit; the
graphic equalizer and channel 0FFh (all channels) stall. A volume SET_CUR
outside MIN..MAX is clamped and one between steps rounded to a step (both
logged; --strict-volume refuses them instead). Mixer Unit (wValue input
channel << 8 | output channel): CUR/MIN/MAX/RES, SET only where bmControls
marks the crosspoint programmable. Endpoint (wIndex the endpoint address):
SAMPLING_FREQ_CONTROL (01h) SET_CUR/GET_CUR, 3 bytes, where the class
endpoint descriptor's bmAttributes D0 declares it (a rate outside the
alternate's table or range stalls), GET_MIN/MAX/RES for a continuous
range; PITCH_CONTROL (02h) CUR where D1 declares it.

Stream validation: every OUT packet is checked against the active
alternate (a whole number of frames, at most wMaxPacketSize); a misframed
packet is counted and logged, not fatal. Per OUT endpoint the model counts
frames per packet against the nominal (rate x service period; for an
asynchronous endpoint the rate the feedback carries), packets more than one
frame from it, zero-length packets, and runs a device FIFO of --fifo-ms
(default 4 ms) filled by the packets and drained by the nominal per packet:
below empty is an underrun, above full an overrun (each resets it to half).
Every iso_out_totals, iso_stop and disconnected line carries these as
"ostreams", and an "ostream_verdict" line closes each stream. Built IDs
FFFF:03xx, which no INF lists.
"""
import argparse
import collections
import hashlib
import json
import math
import socket
import struct
import sys
import time
from pathlib import Path

# uacemu.py sits beside this file; found even under python -I.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import uacemu  # noqa: E402

# UAC 1.0 A.9 request codes.
SET_CUR, GET_CUR, GET_MIN, GET_MAX, GET_RES = 0x01, 0x81, 0x82, 0x83, 0x84
# A.10.2 Feature Unit control selectors: (bmaControls bit, bytes).
FU_CONTROLS = {1: 1, 2: 2, 3: 1, 4: 1, 5: 1, 7: 1, 8: 2, 9: 1, 10: 1}
FU_NAMES = {1: 'mute', 2: 'volume', 3: 'bass', 4: 'mid', 5: 'treble', 6: 'graphic_eq', 7: 'agc',
            8: 'delay', 9: 'bass_boost', 10: 'loudness'}
# Ranges of the controls other than volume (UAC 1.0 5.2.2.4.3): bass, mid,
# treble 1/4 dB signed (-32 to +31.75 dB); delay 1/64 ms (0 to 1 s).
TONE_RANGE = (-128, 127, 1)
DELAY_RANGE = (0, 64 * 1000, 1)
CHANNEL_CONFIG = {1: 0x0004, 2: 0x0003}
SYNC = {'async': 0x04, 'adaptive': 0x08, 'sync': 0x0c}
PROFILE_NAMES = ('headset', 'dacd', 'dacc', 'async', 'hs', 'mic', 'cammic', 'dual', 'play2', 'logi',
                 'pcm8', 'float', 'adsrc', 'alaw', 'mulaw', 'pcm8in', 'floatin', 'mpoin', 'mpoout', 'mpoin48')
# The names agent uac1's plan used, for the same models.
ALIASES = {'headset1': 'headset', 'speaker1': 'dacd', 'mic1': 'cammic'}
BASE_PID = 0x0300


def u24(value):
    return struct.pack('<I', value)[:3]


class Builder1:
    """One USB Audio 1.0 function: an AudioControl interface, then one
    AudioStreaming interface per stream() call, then optional others."""

    def __init__(self, speed, first=0, iad=False):
        self.speed, self.first, self.iad = speed, first, iad
        self.ac, self.streams, self.tail = b'', [], b''
        self.collection = []
        self.interfaces = first + 1
        self.lead = b''

    # -- AudioControl (4.3.2) ------------------------------------------------
    def it(self, tid, ttype, channels):
        self.ac += bytes((12, 0x24, 0x02, tid)) + struct.pack('<HBBHBB', ttype, 0, channels,
                                                                CHANNEL_CONFIG.get(channels, 0), 0, 0)

    def ot(self, tid, ttype, source):
        self.ac += bytes((9, 0x24, 0x03, tid)) + struct.pack('<HBBB', ttype, 0, source, 0)

    def fu(self, uid, source, controls, size=1):
        """controls: bmaControls master first, then each channel."""
        self.ac += bytes((7 + len(controls) * size, 0x24, 0x06, uid, source, size)) + \
            b''.join(c.to_bytes(size, 'little') for c in controls) + b'\0'

    def mixer(self, uid, sources, inputs, channels, programmable):
        """sources: the input pins; inputs: their channels in total;
        programmable: (input channel, output channel) crosspoints, 1-based."""
        bits = bytearray((inputs * channels + 7) // 8)
        for i, o in programmable:
            index = (i - 1) * channels + (o - 1)
            bits[index >> 3] |= 0x80 >> (index & 7)
        self.ac += bytes((10 + len(sources) + len(bits), 0x24, 0x04, uid, len(sources))) + bytes(sources) + \
            struct.pack('<BHB', channels, CHANNEL_CONFIG.get(channels, 0), 0) + bytes(bits) + b'\0'

    # -- AudioStreaming (4.5, 4.6) -------------------------------------------
    def stream(self, terminal, direction, channels, formats, rates, sync, sync_attr=0x01, refresh=2,
               freq_control=True, format_tag=0x0001, mpo=False, frames=None):
        """formats: [(subframe, bits)], one alternate each; rates: a list
        (discrete) or a (low, high) tuple (continuous); format_tag: the AS
        general descriptor's wFormatTag (Formats A.1.1: 1 PCM, 2 PCM8, 3
        IEEE_FLOAT, 4 ALAW, 5 MULAW); mpo: MaxPacketsOnly (class endpoint
        bmAttributes D7); frames: wMaxPacketSize in frames instead of the
        top rate's plus one. An ASYNCHRONOUS OUT endpoint gets a sync IN endpoint
        (feedback); an ADAPTIVE IN endpoint a sync OUT endpoint (UAC 1.0
        Table 4-22: the host's synchronization data for an adaptive
        source), on the OUT direction of the same endpoint number."""
        number = self.interfaces
        self.interfaces += 1
        self.collection.append(number)
        body = bytes((9, 4, number, 0, 0, 1, 2, 0, 0))
        ep = (0x80 if direction == 'in' else 0) | (number - self.first)
        fb = direction == 'out' and sync == 'async'
        synch_out = direction == 'in' and sync == 'adaptive'
        top = rates[1] if isinstance(rates, tuple) else max(rates)
        for alt, (subframe, bits) in enumerate(formats, 1):
            body += bytes((9, 4, number, alt, 2 if fb or synch_out else 1, 1, 2, 0, 0))
            body += bytes((7, 0x24, 0x01, terminal, 1)) + struct.pack('<H', format_tag)
            if isinstance(rates, tuple):
                table = bytes((0,)) + u24(rates[0]) + u24(rates[1])
            else:
                table = bytes((len(rates),)) + b''.join(u24(r) for r in rates)
            body += bytes((7 + len(table), 0x24, 0x02, 1, channels, subframe, bits)) + table
            size = (frames or top // 1000 + 1) * channels * subframe
            if size > (1024 if self.speed == 'high' else 1023):
                raise ValueError('alternate exceeds one transaction')
            interval = 4 if self.speed == 'high' else 1
            synch = (0x80 | ep) if fb else (ep & 0x7f) if synch_out else 0
            body += bytes((9, 5, ep, 0x01 | SYNC[sync])) + struct.pack('<HBBB', size, interval, 0, synch)
            body += bytes((7, 0x25, 0x01, (0x01 if freq_control else 0) | (0x80 if mpo else 0), 0, 0, 0))
            if fb or synch_out:
                # 3 bytes 10.14 at Full Speed; at High Speed USB 2.0
                # 5.12.4.2's 4 bytes 16.16 (what uacemu's feedback sends there).
                body += bytes((9, 5, synch, sync_attr)) + struct.pack(
                    '<HBBB', 4 if self.speed == 'high' else 3, interval, refresh, 0)
        self.streams.append(body)

    def vendor(self, number):
        """A vendor-specific interface with no endpoints (a stand-in)."""
        self.lead += bytes((9, 4, number, 0, 0, 0xff, 0, 0, 0))

    def hid(self, report_length, ep):
        number = self.interfaces
        self.interfaces += 1
        self.tail += bytes((9, 4, number, 0, 1, 3, 0, 0, 0)) + \
            struct.pack('<BBHBBBH', 9, 0x21, 0x0111, 0, 1, 0x22, report_length) + \
            bytes((7, 5, ep, 0x03)) + struct.pack('<HB', 4, 10 if self.speed != 'high' else 7)

    def configuration(self, value=1):
        header = bytes((8 + len(self.collection), 0x24, 0x01)) + \
            struct.pack('<HHB', 0x0100, 8 + len(self.collection) + len(self.ac), len(self.collection)) + \
            bytes(self.collection)
        ac = bytes((9, 4, self.first, 0, 0, 1, 1, 0, 0)) + header + self.ac
        iad = bytes((8, 0x0b, self.first, self.interfaces - self.first, 1, 2, 0, 0)) if self.iad else b''
        body = self.lead + iad + ac + b''.join(self.streams) + self.tail
        return bytes((9, 2)) + struct.pack('<HBBBBB', 9 + len(body), self.interfaces, value, 0, 0x80, 50) + body


# The headset's HID report (HID 1.11, HUT Consumer page 0Ch): one byte,
# Volume Increment (E9h), Volume Decrement (EAh), Mute (E2h), 5 bits pad.
HID_CONSUMER = bytes.fromhex('050c 0901 a101 1500 2501 0ae900 0aea00 09e2 7501 9503 8102 9505 8101 c0')
FS_RATES_44_48 = [44100, 48000]


def build1(name, speed=None, sync_attr=0x01, refresh=2):
    """(speed, configuration, {feature unit id: (min, max, res, cur)},
    {mixer unit id: (min, max, res)}, hid report or None, iad device)."""
    fu, mx, hid, iad = {}, {}, None, False
    if name == 'headset':
        b = Builder1(speed or 'full')
        b.it(1, 0x0101, 2)                       # USB streaming in
        b.it(2, 0x0201, 1)                       # microphone
        b.fu(3, 2, [0x03, 0])                    # sidetone: master mute, volume
        b.mixer(4, [1, 3], 3, 2, [(3, 1), (3, 2)])
        b.fu(5, 4, [0x01, 0x02, 0x02])           # speaker: master mute, channel volume
        b.ot(6, 0x0301, 5)                       # speaker
        b.fu(7, 2, [0x43, 0])                    # capture: mute, volume, AGC
        b.ot(8, 0x0101, 7)                       # USB streaming out
        b.stream(1, 'out', 2, [(2, 16)], [48000, 44100], 'adaptive')
        b.stream(8, 'in', 1, [(2, 16)], [48000, 44100, 16000], 'async')
        b.hid(len(HID_CONSUMER), 0x83)
        fu = {3: (-11520, 0, 384, -11520), 5: (-11520, 0, 384, -3840), 7: (-1536, 5376, 384, 1536)}
        mx = {4: (-11520, 0, 384)}
        hid = HID_CONSUMER
    elif name in ('dacd', 'dacc'):
        b = Builder1(speed or 'full')
        b.it(1, 0x0101, 2)
        if name == 'dacd':
            b.fu(2, 1, [0x03, 0, 0])
            rates = [32000, 44100, 48000]
        else:
            b.fu(2, 1, [0, 0x02, 0x02])
            rates = (8000, 48000)
        b.ot(3, 0x0301, 2)
        b.stream(1, 'out', 2, [(2, 16)], rates, 'adaptive')
        fu = {2: (-24576, 0, 256, -2560)}
    elif name == 'async':
        b = Builder1(speed or 'full')
        b.it(1, 0x0101, 2)
        b.fu(2, 1, [0x0217, 0, 0], size=2)       # mute, volume, bass, treble, loudness
        b.ot(3, 0x0301, 2)
        b.stream(1, 'out', 2, [(2, 16), (3, 24)], FS_RATES_44_48, 'async', sync_attr, refresh)
        fu = {2: (-16384, 1536, 128, 0)}
    elif name == 'hs':
        b = Builder1(speed or 'high')
        b.it(1, 0x0101, 2); b.fu(2, 1, [0x03, 0x02, 0x02]); b.ot(3, 0x0302, 2)
        b.it(4, 0x0603, 2); b.fu(5, 4, [0x03, 0, 0]); b.ot(6, 0x0101, 5)    # line in
        b.stream(1, 'out', 2, [(2, 16), (3, 24)], [44100, 48000, 88200, 96000], 'adaptive')
        b.stream(6, 'in', 2, [(3, 24), (2, 16)], [48000, 96000], 'async')
        fu = {2: (-32512, 0, 256, 0), 5: (-6144, 3072, 256, 0)}
    elif name == 'mic':
        b = Builder1(speed or 'full')
        b.it(1, 0x0201, 1); b.fu(2, 1, [0x43, 0]); b.ot(3, 0x0101, 2)
        b.stream(3, 'in', 1, [(2, 16)], [16000, 32000, 48000], 'async')
        fu = {2: (-3072, 7680, 256, 0)}
    elif name == 'cammic':
        b = Builder1(speed or 'high', first=1, iad=True)
        b.vendor(0)
        b.it(1, 0x0201, 1); b.fu(2, 1, [0x03, 0]); b.ot(3, 0x0101, 2)
        b.stream(3, 'in', 1, [(2, 16)], [16000, 24000, 32000, 48000], 'async')
        fu = {2: (0, 6144, 128, 3072)}
        iad = True
    elif name == 'dual':
        b = Builder1(speed or 'high')
        b.it(1, 0x0101, 2); b.fu(2, 1, [0x03, 0, 0]); b.ot(3, 0x0301, 2)
        b.it(4, 0x0201, 2); b.ot(5, 0x0101, 4)
        b.stream(1, 'out', 2, [(2, 16), (3, 24)], FS_RATES_44_48, 'adaptive')
        b.stream(5, 'in', 2, [(2, 16)], [48000], 'async')
        fu = {2: (-24576, 0, 256, 0)}
    elif name in ('pcm8', 'float'):
        # Formats document 2.2: Type I PCM8 (wFormatTag 2, unsigned 8-bit
        # in a 1-byte subframe) or IEEE_FLOAT (3, 32-bit in 4 bytes).
        b = Builder1(speed or 'full')
        b.it(1, 0x0101, 2); b.fu(2, 1, [0x03, 0, 0]); b.ot(3, 0x0301, 2)
        if name == 'pcm8':
            b.stream(1, 'out', 2, [(1, 8)], FS_RATES_44_48, 'adaptive', format_tag=0x0002)
        else:
            b.stream(1, 'out', 2, [(4, 32)], FS_RATES_44_48, 'adaptive', format_tag=0x0003)
        fu = {2: (-24576, 0, 256, 0)}
    elif name == 'adsrc':
        # Capture from an ADAPTIVE source: the IN data endpoint's
        # bSynchAddress names a sync OUT endpoint (UAC 1.0 Table 4-22).
        b = Builder1(speed or 'full')
        b.it(1, 0x0201, 1); b.fu(2, 1, [0x03, 0]); b.ot(3, 0x0101, 2)
        b.stream(3, 'in', 1, [(2, 16)], [16000, 44100, 48000], 'adaptive')
        fu = {2: (-3072, 7680, 256, 0)}
    elif name in ('alaw', 'mulaw'):
        # Formats document 2.2.6.4: G.711 octets, one byte a sample.
        tag = 0x0004 if name == 'alaw' else 0x0005
        b = Builder1(speed or 'full')
        b.it(1, 0x0101, 2); b.fu(2, 1, [0x03, 0, 0]); b.ot(3, 0x0301, 2)
        b.it(4, 0x0201, 1); b.fu(5, 4, [0x03, 0]); b.ot(6, 0x0101, 5)
        b.stream(1, 'out', 2, [(1, 8)], FS_RATES_44_48, 'adaptive', format_tag=tag)
        b.stream(6, 'in', 1, [(1, 8)], [16000, 48000], 'async', format_tag=tag)
        fu = {2: (-24576, 0, 256, 0), 5: (-3072, 7680, 256, 0)}
    elif name == 'pcm8in':
        # PCM8 left-justified in a 2-byte subframe (Formats 2.2.5, 2.2.6.2).
        b = Builder1(speed or 'full')
        b.it(1, 0x0201, 1); b.fu(2, 1, [0x03, 0]); b.ot(3, 0x0101, 2)
        b.stream(3, 'in', 1, [(2, 8)], [16000, 44100, 48000], 'async', format_tag=0x0002)
        fu = {2: (-3072, 7680, 256, 0)}
    elif name == 'floatin':
        b = Builder1(speed or 'full')
        b.it(1, 0x0603, 2); b.fu(2, 1, [0x03, 0, 0]); b.ot(3, 0x0101, 2)
        b.stream(3, 'in', 2, [(4, 32)], FS_RATES_44_48, 'sync', format_tag=0x0003)
        fu = {2: (-3072, 7680, 256, 0)}
    elif name in ('mpoin', 'mpoin48'):
        b = Builder1(speed or 'full')
        b.it(1, 0x0603, 2); b.fu(2, 1, [0x03, 0, 0]); b.ot(3, 0x0101, 2)
        b.stream(3, 'in', 2, [(2, 16)], FS_RATES_44_48 if name == 'mpoin' else [48000], 'sync', mpo=True,
                 frames=52)
        fu = {2: (-3072, 7680, 256, 0)}
    elif name == 'mpoout':
        b = Builder1(speed or 'full')
        b.it(1, 0x0101, 2); b.fu(2, 1, [0x03, 0, 0]); b.ot(3, 0x0301, 2)
        b.stream(1, 'out', 2, [(1, 8)], FS_RATES_44_48, 'adaptive', format_tag=0x0002, mpo=True, frames=50)
        fu = {2: (-24576, 0, 256, 0)}
    else:
        raise ValueError('profile')
    return b.speed, b.configuration(), fu, mx, hid, iad


TESTDATA = Path(__file__).resolve().parent / 'testdata'
# Replayed captures of real devices (testdata/<dir>/README.md): descriptors
# as read, strings replaced by SIMULATED labels, serial index zero; the
# class-request answers the device gave Windows' own driver.
REPLAY = {
    'play2': dict(dir='play2', config='config0.bin', device='device.bin', hid='hidreport.bin',
                  fu={6: (-3840, 3840, 192, 0), 5: (-13312, 0, 256, -2048)}, stall_units=(9, 12),
                  strings={}),
    'logi': dict(dir='logi0a64', config='CONFIG.BIN', device='DEVICE.BIN', hid='HIDREPORT.BIN',
                 fu={2: (-7680, 0, 256, -3838), 5: (-12800, 0, 256, -4352), 11: (-14592, -3072, 384, -8832)},
                 stall_units=(12,), strings={6: 'Sidetone'}),
}


def consumer_report(length):
    """HID_CONSUMER padded to `length` bytes with Usage items inside its
    collection (a stand-in where the real report was not captured)."""
    pad = length - len(HID_CONSUMER)
    if pad < 0 or pad == 1:
        return None
    fill = (b'\x0a\x01\x00' if pad % 2 else b'') + b'\x09\x01' * (pad // 2 - (1 if pad % 2 else 0))
    return HID_CONSUMER[:-1] + fill + HID_CONSUMER[-1:]


def audio_function1(config):
    """The UAC 1.0 entities and streaming alternates of a configuration
    (framing already checked by uacemu.layout()): the AudioControl
    interface number; terminals and units by ID; per streaming (interface,
    alternate) the terminal link, format, rates and endpoints."""
    ac, entities, alts = None, {}, {}
    current, off = None, 9
    while off < len(config):
        size, kind = config[off], config[off + 1]
        d = config[off:off + size]
        if kind == 4:
            current = (d[2], d[3], d[5], d[6], d[7])
            if d[5] == 1 and d[6] == 1 and d[7] == 0 and ac is None:
                ac = d[2]
            if d[5] == 1 and d[6] == 2 and d[3]:
                alts[(d[2], d[3])] = dict(endpoints=[])
        elif kind == 0x24 and current and current[2:] == (1, 1, 0) and current[0] == ac and size >= 4:
            sub = d[2]
            if sub == 0x02 and size >= 12:
                entities[d[3]] = dict(kind='it', type=int.from_bytes(d[4:6], 'little'), channels=d[7])
            elif sub == 0x03 and size >= 9:
                entities[d[3]] = dict(kind='ot', type=int.from_bytes(d[4:6], 'little'), source=d[7])
            elif sub == 0x06 and size >= 7 and d[5] and (size - 7) % d[5] == 0:
                n = d[5]
                count = (size - 7) // n
                entities[d[3]] = dict(kind='fu', source=d[4], channels=count - 1, size=n,
                                      controls=[int.from_bytes(d[6 + n * i:6 + n * (i + 1)], 'little')
                                                for i in range(count)])
            elif sub == 0x04 and size >= 10 + d[4]:
                p = d[4]
                entities[d[3]] = dict(kind='mx', unit='mixer', inputs=list(d[5:5 + p]), channels=d[5 + p],
                                      mixer_controls=bytes(d[9 + p:size - 1]))
            elif sub == 0x05 and size >= 6 + d[4]:
                entities[d[3]] = dict(kind='mx', unit='selector', inputs=list(d[5:5 + d[4]]), controls=0)
            elif sub == 0x07 and size >= 7 + d[6]:
                entities[d[3]] = dict(kind='xu', inputs=list(d[7:7 + d[6]]))
            elif sub == 0x08 and size >= 7 + d[6]:
                entities[d[3]] = dict(kind='xu', inputs=list(d[7:7 + d[6]]))
        elif kind == 0x24 and current and current[2:4] == (1, 2) and current[1] and size >= 4:
            a = alts[(current[0], current[1])]
            if d[2] == 1 and size >= 7:
                a.update(terminal=d[3], delay=d[4], format_tag=int.from_bytes(d[5:7], 'little'))
            elif d[2] == 2 and size >= 8 and d[3] == 1:
                count = d[7]
                if count == 0 and size >= 14:
                    rates = (int.from_bytes(d[8:11], 'little'), int.from_bytes(d[11:14], 'little'))
                elif count and size >= 8 + 3 * count:
                    rates = [int.from_bytes(d[8 + 3 * i:11 + 3 * i], 'little') for i in range(count)]
                else:
                    rates = None
                a.update(channels=d[4], subslot=d[5], bits=d[6], rates=rates)
        elif kind == 0x25 and current and current[2:4] == (1, 2) and current[1] and size >= 7 and d[2] == 1:
            eps = alts[(current[0], current[1])]['endpoints']
            if eps:
                eps[-1]['cs_attributes'] = d[3]
        elif kind == 5 and current and current[2:4] == (1, 2) and current[1]:
            wmax = int.from_bytes(d[4:6], 'little')
            alts[(current[0], current[1])]['endpoints'].append(dict(
                address=d[2], attributes=d[3], max=wmax & 2047, mult=1 + (wmax >> 11 & 3), interval=d[6],
                refresh=d[7] if size >= 9 else 0, synch=d[8] if size >= 9 else 0, cs_attributes=0))
        off += size
    return ac, entities, alts


def patch_refresh(config, refresh):
    """The configuration with every sync endpoint's bRefresh set to
    `refresh` (a rig option for a replay: QEMU drops the stream of an iso
    IN endpoint the guest polls only every 32 frames or more, so a sync
    endpoint with bRefresh 5 never reaches the guest there)."""
    out, off = bytearray(config), 9
    sync = {d['synch'] for a in audio_function1(config)[2].values() for d in a['endpoints'] if d['synch']}
    while off < len(out):
        if out[off + 1] == 5 and out[off] >= 9 and out[off + 2] in sync:
            out[off + 7] = refresh
        off += out[off]
    return bytes(out)


class Profile1:
    """A UAC 1.0 profile in the shape uacemu.Model reads (uacemu.Profile)."""
    uac = 1

    def __init__(self, name='headset', vid=None, pid=None, speed=None, sync_attr=0x01, refresh=None,
                 strict_volume=False):
        name = ALIASES.get(name, name)
        if name not in PROFILE_NAMES or speed not in (None, 'full', 'high') or \
                sync_attr not in (0x01, 0x05, 0x11) or not (refresh is None or 1 <= refresh <= 9):
            raise ValueError('profile')
        if speed and name in ('hs', 'cammic', 'dual') and speed != 'high':
            raise ValueError('profile is High Speed only')
        if speed and name in REPLAY and speed != 'full':
            raise ValueError('a replayed device keeps its speed')
        self.name, self.fu, self.strict_volume = name, 'uac1', strict_volume
        self.stall_units = ()
        if name in REPLAY:
            r = REPLAY[name]
            base = TESTDATA / r['dir']
            self.speed, self.config = 'full', (base / r['config']).read_bytes()
            if refresh is not None:
                self.config = patch_refresh(self.config, refresh)
            self.fu_ranges, self.mx_ranges = dict(r['fu']), {}
            self.stall_units = tuple(r['stall_units'])
            device = bytearray((base / r['device']).read_bytes())
            self.vid = vid or int.from_bytes(device[8:10], 'little')
            self.pid = pid or int.from_bytes(device[10:12], 'little')
            device[8:12] = struct.pack('<HH', self.vid, self.pid)
            device[16] = 0                      # no serial number
            self.device = bytes(device)
            self.qualifier = None               # Full Speed only: the real ones stall it
            self.strings = {0: b'\x04\x03\x09\x04'}
            if device[14]:
                self.strings[device[14]] = uacemu.usb_string('usb-wdm-free SIMULATED')
            if device[15]:
                self.strings[device[15]] = uacemu.usb_string('UAC1 %s layout - SIMULATED' % name)
            for index, text in r['strings'].items():
                self.strings[index] = uacemu.usb_string(text)
            hid = uacemu.hid_interfaces(self.config)
            self.hid_report = (base / r['hid']).read_bytes() if r['hid'] else \
                consumer_report(next(iter(hid.values()))) if hid else None
        else:
            self.speed, self.config, self.fu_ranges, self.mx_ranges, self.hid_report, iad = \
                build1(name, speed, sync_attr, 2 if refresh is None else refresh)
            self.vid, self.pid = vid or 0xffff, pid or BASE_PID + PROFILE_NAMES.index(name)
            high = self.speed == 'high'
            cls = (0xef, 2, 1) if iad else (0, 0, 0)
            self.device = struct.pack('<BBHBBBBHHHBBBB', 18, 1, 0x200 if high else 0x110, *cls, 64,
                                      self.vid, self.pid, 0x0100, 1, 2, 0, 2 if name == 'dual' else 1)
            self.qualifier = struct.pack('<BBHBBBBBB', 10, 6, 0x200, *cls, 64, 1, 0) if high else None
            self.strings = {0: b'\x04\x03\x09\x04', 1: uacemu.usb_string('usb-wdm-free SIMULATED'),
                            2: uacemu.usb_string('UAC1 %s - SIMULATED' % name)}
        self.config2 = None
        if name == 'dual':
            # Configuration 2: uacemu's hs24 UAC 2.0 function, bConfigurationValue 2.
            self.uac2 = uacemu.Profile('hs24', self.vid, self.pid)
            self.config2 = self.uac2.config[:5] + b'\x02' + self.uac2.config[6:]
            self.uac2.config = self.config2
        self.layout = uacemu.layout(self.config)
        self.ac, self.entities, self.alts = audio_function1(self.config)
        self.rates = {}
        self.interfaces = sorted({i for i, _ in self.layout})
        self.hid = uacemu.hid_interfaces(self.config)
        self.allowed, self.fu_settable = None, True
        self.vol_range, self.vol_cur, self.vol_at = uacemu.VOLUME_RANGE, 0, {}

    def stream_alt(self, interface, alternate):
        """The streaming facts of one alternate, or None. The 'clock' is the
        data endpoint's address: a UAC 1.0 rate belongs to the endpoint."""
        a = self.alts.get((interface, alternate))
        if not a or 'subslot' not in a or not a['endpoints']:
            return None
        data = a['endpoints'][0]
        fbs = [e for e in a['endpoints'][1:] if e['address'] == data['synch']]
        return dict(frame=a['channels'] * a['subslot'], channels=a['channels'], subslot=a['subslot'],
                    bits=a['bits'], clock=data['address'], data=data, feedback=fbs[0] if fbs else None,
                    rates=a['rates'], sync=data['attributes'] & 0x0c, format_tag=a.get('format_tag', 1),
                    mpo=bool(data['cs_attributes'] & 0x80))

    def endpoint_rates(self, ep, alternate_info=None):
        """The rates an endpoint takes: its active alternate's, else every
        alternate's (discrete lists merged; a continuous range kept)."""
        infos = [alternate_info] if alternate_info else \
            [self.stream_alt(i, a) for i, a in self.alts if self.stream_alt(i, a)]
        merged, ranges = set(), []
        for info in infos:
            if info and info['data']['address'] == ep:
                if isinstance(info['rates'], tuple):
                    ranges.append(info['rates'])
                elif info['rates']:
                    merged.update(info['rates'])
        return sorted(merged), ranges


def rate_ok(rate, rates):
    merged, ranges = rates
    return rate in merged or any(lo <= rate <= hi for lo, hi in ranges)


def alaw_decode(a):
    """ITU-T G.711 A-law octet (even bits inverted) to 16-bit linear."""
    a ^= 0x55
    seg, t = (a >> 4) & 7, (a & 15) << 4
    t = t + 8 if seg == 0 else (t + 0x108) << (seg - 1)
    return t if a & 0x80 else -t


def mulaw_decode(u):
    """ITU-T G.711 mu-law octet (all bits inverted) to 16-bit linear."""
    u = ~u & 0xff
    t = (((u & 15) << 3) + 0x84) << ((u >> 4) & 7)
    return 0x84 - t if u & 0x80 else t - 0x84


ALAW = [alaw_decode(a) for a in range(256)]
MULAW = [mulaw_decode(u) for u in range(256)]


def g711_encode(table, value):
    """The octet whose decoded value is nearest (a test signal's encoder,
    by the decode table, independent of the driver's segment code)."""
    return min(range(256), key=lambda c: (abs(table[c] - value), c))


G711_CACHE = {}


def g711(table_name, value):
    key = (table_name, value)
    if key not in G711_CACHE:
        G711_CACHE[key] = g711_encode(ALAW if table_name == 4 else MULAW, value)
    return G711_CACHE[key]


def decode16(payload, tag, subslot, bits):
    """Samples of a Type I payload as 16-bit linear values (PCM's top 16
    bits; PCM8, ALAW and MULAW from the subframe's last byte; IEEE_FLOAT
    times 32768, clamped, NaN as 0)."""
    out = []
    if tag == 3:
        count = len(payload) // 4
        for v in struct.unpack('<%df' % count, payload[:4 * count]):
            out.append(0 if v != v else int(max(-1.0, min(1.0, v)) * 32767))
        return out
    for i in range(0, len(payload) - subslot + 1, subslot):
        if tag == 2:
            out.append((payload[i + subslot - 1] - 128) << 8)
        elif tag == 4:
            out.append(ALAW[payload[i + subslot - 1]])
        elif tag == 5:
            out.append(MULAW[payload[i + subslot - 1]])
        else:
            v = int.from_bytes(payload[i:i + subslot], 'little', signed=True)
            out.append(v >> (8 * subslot - 16))
    return out


def encode_sine(samples, info, rate, phase):
    """uacemu's 1 kHz half-scale sine in the alternate's format: PCM as
    uacemu.sine; the others from a 16-bit value of 16383 peak (-6 dBFS)."""
    tag, sub = info.get('format_tag', 1), info['subslot']
    if tag == 1:
        return uacemu.sine(samples, info, rate, phase)
    out = bytearray()
    for n in range(phase, phase + samples):
        v = int(round(16383 * math.sin(2 * math.pi * 1000 * n / rate)))
        if tag == 3:
            one = struct.pack('<f', v / 32768.0)
        else:
            byte = (v + 32896) >> 8 if tag == 2 else g711(tag, v)
            one = bytes(sub - 1) + bytes((min(255, byte),))
        out += one * info['channels']
    return bytes(out)


class Model1(uacemu.Model):
    """uacemu's usbredir device with UAC 1.0 class requests, endpoint
    sampling rates and OUT stream validation."""

    def __init__(self, connection, trace, now=time.monotonic, pcm=None, profile=None, fifo_ms=4.0,
                 fb_at_refresh=False, fb_lead_empty=0, **kw):
        self.p1 = profile or Profile1('headset')
        super().__init__(connection, trace, now, pcm=pcm, profile=self.p1, **kw)
        self.cfg_value = 0
        self.fifo_ms, self.fb_at_refresh = fifo_ms, fb_at_refresh
        self.fb_lead_empty, self.fb_lead_left, self.lead_empty_sent = fb_lead_empty, 0, 0
        self.bind(self.p1)
        self.ostate = {}            # OUT endpoint: validation state of its running stream
        self.synch_out = {}         # sync OUT endpoint (adaptive source): what the host sent
        self.float_bad, self.float_samples = {}, {}     # endpoint: float samples out of range, all
        self.closed = []            # verdicts of finished streams
        self.tone = {}              # OUT endpoint: zero crossings of channel 1 (26.12)

    def bind(self, profile):
        """Make one configuration's function the active one (its state reset)."""
        self.p = profile
        self.freq = {cid: (48000 if 48000 in r else r[0]) for cid, r in profile.rates.items()}
        self.selector = {cid: 1 for cid, e in profile.entities.items() if e['kind'] == 'cx'}
        self.unit_sel = {uid: 1 for uid, e in profile.entities.items() if e.get('unit') == 'selector'}
        self.mix, self.mute, self.volume = {}, {}, {}
        self.fu_other, self.pitch = {}, {}
        self.ep_freq = {}
        if getattr(profile, 'uac', 2) == 1:
            for (i, a) in sorted(profile.alts):
                info = profile.stream_alt(i, a)
                if info and info['data']['address'] not in self.ep_freq:
                    merged, ranges = profile.endpoint_rates(info['data']['address'])
                    self.ep_freq[info['data']['address']] = 48000 if 48000 in merged or \
                        any(lo <= 48000 <= hi for lo, hi in ranges) else (merged or [ranges[0][1]])[-1]
        self.alts = {i: 0 for i in profile.interfaces}

    @property
    def uac1(self):
        return getattr(self.p, 'uac', 2) == 1

    def rate(self, clock, depth=0):
        if self.uac1:
            return self.ep_freq.get(clock)
        return super().rate(clock, depth)

    def channel_gain(self, units, channel):
        """(dB, muted) of the UAC 1.0 feature units (mute D0, volume D1)."""
        if not self.uac1:
            return super().channel_gain(units, channel)
        db, muted = 0.0, False
        for uid in units:
            e = self.p.entities[uid]
            for ch in (0, channel):
                if ch > e['channels']:
                    continue
                if e['controls'][ch] & 1:
                    muted = muted or bool(self.mute.get((uid, ch), 0))
                if e['controls'][ch] & 2:
                    value = self.volume.get((uid, ch), self.p.fu_ranges.get(uid, uacemu.VOLUME_RANGE + (0,))[3])
                    if value == -32768:
                        muted = True
                    else:
                        db += value / 256
        return db, muted

    # -- feedback ------------------------------------------------------------
    def start(self, ep, own):
        super().start(ep, own)
        info = own[2]
        fb = info['feedback']
        if fb and fb['address'] == ep and not ep & 0x80:
            # A sync OUT endpoint (an adaptive source's): the host sends,
            # the model only counts what arrives (iso_out).
            self.fb.pop(ep, None)
            if not self.fb:
                self.next_feedback = None
            self.synch_out.setdefault(ep, dict(packets=0, empty=0, values=collections.Counter()))
        elif fb and fb['address'] == ep:
            # A real device answers each poll of its sync endpoint (every
            # 2^bRefresh ms, UAC 1.0 3.7.2.2) at once. QEMU's usb-redir
            # holds iso IN data until a prefill target is buffered and
            # answers the polls before that with zero bytes, so the model
            # sends a value every frame (the endpoint's bInterval);
            # --fb-at-refresh sends one every 2^bRefresh ms instead.
            if self.fb_at_refresh and fb.get('refresh') and self.p.speed == 'full':
                self.fb[ep]['period'] = 0.001 * (1 << fb['refresh'])
            self.fb_lead_left = self.fb_lead_empty
        if not ep & 0x80 and info['data']['address'] == ep:
            self.ostate[ep] = dict(info=info, started=False, packets=0, frames=0, hist=collections.Counter(),
                                   outside=0, zero=0, lead_zero=0, under=0, over=0, misframed=0,
                                   level=0.0, expected=0.0, low=None, high=None, mpo_bad=0,
                                   mpo_zero=0, mpo_acc=0.0)
            self.tone[ep] = dict(sign=0, crossings=0, samples=0, rate=self.rate(info['clock']) or 48000)
        if ep & 0x80 and info['data']['address'] == ep and ep in self.in_streams:
            self.in_streams[ep]['k'] = 0

    def feedback_value(self, info):
        if self.fb_lead_left > 0:
            # --fb-lead-empty: the first packets of a stream carry nothing.
            self.fb_lead_left -= 1
            self.lead_empty_sent += 1
            return b''
        return super().feedback_value(info)

    def stop(self, ep):
        s = self.ostate.pop(ep, None)
        if s is not None:
            verdict = self.summary(ep, s)
            verdict['ok'] = bool(s['packets']) and not (s['outside'] or s['under'] or s['over'] or
                                                          s['misframed'] or s['zero'] or s.get('mpo_bad', 0))
            self.closed.append(verdict)
            self.log('ostream_verdict', **verdict)
        super().stop(ep)

    def reset_streams(self):
        for ep in list(self.ostate):
            self.stop(ep)
        super().reset_streams()

    def nominal(self, info):
        """Frames per OUT packet the device consumes: the rate (for an
        asynchronous endpoint the one its feedback reports) times the
        endpoint's service period."""
        rate = self.rate(info['clock']) or 48000
        if info['feedback']:
            rate *= 1 + self.fb_ppm / 1e6
        return rate * self.period(info['data'])

    def summary(self, ep, s):
        info = s['info']
        return dict(endpoint=ep, rate=self.rate(info['clock']), nominal=round(self.nominal(info), 4),
                    packets=s['packets'], frames=s['frames'],
                    frames_per_packet={str(k): v for k, v in sorted(s['hist'].items())[:24]},
                    outside_pm1=s['outside'], zero_length=s['zero'], lead_zero=s['lead_zero'],
                    underruns=s['under'], overruns=s['over'], misframed=s['misframed'],
                    mpo_bad_length=s.get('mpo_bad', 0), mpo_zero_audio=s.get('mpo_zero', 0),
                    drift_frames=round(s['frames'] - s['expected'], 3),
                    fifo_low=None if s['low'] is None else round(s['low'], 2),
                    fifo_high=None if s['high'] is None else round(s['high'], 2))

    def log(self, event, **fields):
        if event in ('iso_out_totals', 'iso_stop', 'disconnected') and getattr(self, 'ostate', None):
            fields['ostreams'] = [self.summary(ep, s) for ep, s in sorted(self.ostate.items())]
        if event in ('iso_stop', 'disconnected') and getattr(self, 'synch_out', None):
            fields['synch_out'] = {'%02x' % ep: dict(packets=s['packets'], empty=s['empty'],
                                                     values={str(k): v for k, v in sorted(s['values'].items())},
                                                     off_rate=s.get('off', 0), expected=s.get('expected'))
                                   for ep, s in sorted(self.synch_out.items())}
        if event in ('iso_out_totals', 'iso_stop', 'disconnected') and getattr(self, 'tone', None):
            fields['tone_hz'] = {'%02x' % ep: round(s['crossings'] * s['rate'] / s['samples'], 1)
                                 for ep, s in sorted(self.tone.items()) if s['samples']}
        if event in ('iso_out_totals', 'iso_stop', 'disconnected') and getattr(self, 'float_samples', None):
            fields['float_out_of_range'] = {'%02x' % (ep or 0): [self.float_bad.get(ep, 0), n]
                                            for ep, n in sorted(self.float_samples.items(), key=lambda x: x[0] or 0)}
        super().log(event, **fields)

    def track_tone(self, payload, info, ep):
        """Zero crossings (negative to positive, zeros skipped) of channel 1."""
        s = self.tone.get(ep)
        if s is None:
            return
        values = decode16(payload, info.get('format_tag', 1), info['subslot'], info['bits'])
        for v in values[::info['channels']]:
            s['samples'] += 1
            if v > 0:
                if s['sign'] < 0:
                    s['crossings'] += 1
                s['sign'] = 1
            elif v < 0:
                s['sign'] = -1

    def measure(self, payload, info, units, ep=None):
        """PCM8 (unsigned, offset 128), ALAW, MULAW and IEEE_FLOAT samples
        measured as 16-bit PCM of the same level; PCM as uacemu does."""
        tag = info.get('format_tag', 1)
        if ep is not None:
            self.track_tone(payload, info, ep)
        if tag in (2, 4, 5):
            pcm = decode16(payload, tag, info['subslot'], info['bits'])
            return super().measure(struct.pack('<%dh' % len(pcm), *[max(-32768, min(32767, v)) for v in pcm]),
                                   dict(info, subslot=2), units, ep)
        if tag not in (2, 3):
            return super().measure(payload, info, units, ep)
        if True:
            count = len(payload) // 4
            values = struct.unpack('<%df' % count, payload[:4 * count])
            # A float stream carries -1.0 to 1.0: NaN, infinities and values
            # past full scale are counted (XP SP3's usbaudio sends 32-bit
            # integer PCM into a float format, most of it out of range).
            bad = sum(1 for v in values if v != v or abs(v) > 1.0)
            if bad:
                self.float_bad[ep] = self.float_bad.get(ep, 0) + bad
            self.float_samples[ep] = self.float_samples.get(ep, 0) + count
            # Clamped as floats first (infinities included); NaN as silence.
            pcm = [0 if v != v else int(max(-1.0, min(1.0, v)) * 32767) for v in values]
        as16 = dict(info, subslot=2)
        return super().measure(struct.pack('<%dh' % len(pcm), *pcm), as16, units, ep)

    def iso_out(self, packet):
        if len(packet) < 4:
            raise ValueError('short iso packet')
        ep, _, length = struct.unpack('<BBH', packet[:4])
        own = None if ep & 0x80 or ep not in self.streams else self.owner(ep)
        if own is None or len(packet) - 4 != length:
            raise ValueError('OUT iso framing/state')
        info = own[2]
        if info['feedback'] and info['feedback']['address'] == ep:
            # Synchronization data from the host to an adaptive source:
            # 10.14 samples a frame in 3 bytes at Full Speed (16.16 in 4 at
            # High Speed), counted and logged.
            sync = self.synch_out.setdefault(ep, dict(packets=0, empty=0, values=collections.Counter()))
            sync['packets'] += 1
            payload = packet[4:]
            if not payload:
                sync['empty'] += 1
            else:
                scale = 65536 if len(payload) >= 4 else 16384
                raw = int.from_bytes(payload[:4], 'little')
                value = raw / scale
                # The value the rate asks: rate x 16384 / 1000 (Full Speed),
                # rate x 65536 / 8000 (High Speed), rounded.
                rate = self.rate(info['clock']) or 48000
                want = round(rate * 16384 / 1000) if scale == 16384 else round(rate * 65536 / 8000)
                sync['expected'] = round(want / scale, 4)
                if abs(raw - want) > 1:
                    sync['off'] = sync.get('off', 0) + 1
                sync['last'] = value
                if len(sync['values']) < 32 or value in sync['values']:
                    sync['values'][round(value, 4)] += 1
                if sync['packets'] <= 4:
                    self.log('synch_out', endpoint=ep, length=length, value=round(value, 4))
            return
        s = self.ostate.get(ep)
        if info.get('mpo') and info['data']['address'] == ep and length:
            # MaxPacketsOnly (UAC 1.0 4.6.1.2): a non-null packet is
            # wMaxPacketSize, the audio first and zero bytes after it.
            if length != info['data']['max'] * info['data']['mult'] and s is not None:
                s['mpo_bad'] += 1
            payload = packet[4:]
            frames = length // info['frame']
            last = 0
            for f in range(frames, 0, -1):
                if any(payload[(f - 1) * info['frame']:f * info['frame']]):
                    last = f
                    break
            if info.get('format_tag', 1) == 2 and s is not None:
                # PCM8: a zero byte is a full-scale sample (-1.0) as well
                # as padding, so trailing zero frames are ambiguous. Up to
                # the cadence's whole frames (a running fraction, so 44.1
                # kHz gives nine 44 and one 45) are taken as audio (counted
                # as mpo_zero_audio, not a failure); padding is past them.
                s['mpo_acc'] += self.nominal(info)
                whole = int(s['mpo_acc'] + 1e-9)
                s['mpo_acc'] -= whole
                if last < min(frames, whole):
                    s['mpo_zero'] += min(frames, whole) - last
                    last = min(frames, whole)
            length = last * info['frame']
            packet = struct.pack('<BBH', ep, 0, length) + payload[:length]
        if length > info['data']['max'] * info['data']['mult'] or length % info['frame']:
            # Counted against the driver, not fatal.
            if s is not None:
                s['misframed'] += 1
                if s['misframed'] <= 20:
                    self.log('ostream_misframed', endpoint=ep, length=length, frame=info['frame'],
                             max=info['data']['max'])
            return
        super().iso_out(packet)
        if s is None:
            return
        frames = length // info['frame']
        if not s['started']:
            if not frames:
                s['lead_zero'] += 1
                return
            s['started'] = True
        nominal = self.nominal(info)
        capacity = self.fifo_ms / 1000 * (self.rate(info['clock']) or 48000)
        if s['packets'] == 0:
            s['level'] = capacity / 2
        s['packets'] += 1
        s['frames'] += frames
        s['expected'] += nominal
        s['hist'][frames] += 1
        if abs(frames - nominal) > 1:
            s['outside'] += 1
        if not frames:
            s['zero'] += 1
        s['level'] += frames - nominal
        if s['level'] < 0:
            s['under'] += 1
            s['level'] = capacity / 2
        elif s['level'] > capacity:
            s['over'] += 1
            s['level'] = capacity / 2
        s['low'] = s['level'] if s['low'] is None else min(s['low'], s['level'])
        s['high'] = s['level'] if s['high'] is None else max(s['high'], s['level'])

    # -- device-to-host streams (26.12) ----------------------------------------
    def capture(self):
        """uacemu's capture with the alternate's format, an adaptive
        source's samples at the Ff last sent to its synch endpoint, and
        MaxPacketsOnly padding (with Formats 2.2.1's sizes at 48 kHz)."""
        stamp = self.now()
        for ep, s in self.in_streams.items():
            if stamp < s['next']:
                continue
            info = s['info']
            rate = self.rate(info['clock']) or 48000
            limit = info['data']['max'] * info['data']['mult'] // info['frame']
            fb = info['feedback']
            ff = self.synch_out.get(fb['address'], {}).get('last') if fb and not fb['address'] & 0x80 else None
            unit = 0.000125 if self.p.speed == 'high' else 0.001
            per = ff * s['period'] / unit if ff else rate * s['period']
            jitter = info.get('mpo') and abs(per - round(per)) < 1e-9
            for _ in range(min(64, int((stamp - s['next']) / s['period'] + 1e-9) + 1)):
                s['acc'] += per
                k = s.get('k', 0)
                s['k'] = k + 1
                # The jitter (2.2.1's nav - 1, nav + 1) is added after the
                # accumulator takes its whole samples, so it never carries
                # into the next packet: 48, 47, 49, 48.
                base = int(s['acc'] + 1e-9)
                s['acc'] -= base
                samples = base + ((0, -1, 1, 0)[k % 4] if jitter else 0)
                samples = max(0, min(samples, limit))
                data = encode_sine(samples, info, rate, s['phase'])
                if info.get('mpo') and data:
                    data += bytes(info['data']['max'] * info['data']['mult'] - len(data))
                s['phase'] += samples
                self.send(102, s['ident'], struct.pack('<BBH', ep, 0, len(data)) + data)
                s['ident'] = (s['ident'] + 1) & 0xffffffff
                self.in_packets += 1
                self.in_bytes += len(data)
                s['next'] += s['period']

    # -- usbredir: configurations -------------------------------------------
    def handle(self, kind, ident, packet):
        if kind == 6:
            if len(packet) != 1:
                raise ValueError('configuration length')
            value = packet[0]
            status = 0 if value in (0, 1) or (value == 2 and self.p1.config2) else 4
            self.log('set_configuration', value=value, status=status)
            if not status:
                self.reset_streams()
                self.bind(self.p1.uac2 if value == 2 else self.p1)
                self.cfg_value = value
                # uacemu's checks read config == 1 as "configured".
                self.config = 1 if value else 0
                self.info()
            self.send(8, ident, bytes((status, self.cfg_value)))
            return
        if kind == 3:
            super().handle(kind, ident, packet)
            self.bind(self.p1)
            self.cfg_value = 0
            return
        if kind == 7:
            if packet:
                raise ValueError('configuration length')
            self.send(8, ident, bytes((0, self.cfg_value)))
            return
        super().handle(kind, ident, packet)

    def info(self):
        saved = self.config
        self.config = 1 if saved else 0     # never uacemu's UAC 3.0 configuration 2
        try:
            super().info()
        finally:
            self.config = saved

    # -- control transfers ---------------------------------------------------
    def control(self, ident, packet):
        if len(packet) < 10:
            raise ValueError('short control header')
        ep, request, rt, _, value, index, length = struct.unpack('<BBBBHHH', packet[:10])
        payload = packet[10:]
        if len(payload) != (0 if rt & 128 else length):
            raise ValueError('control length mismatch')
        p = self.p
        answer = None
        if request == 8 and rt == 0x80:
            answer = 0, bytes((self.cfg_value,))
        elif request == 6 and rt == 0x80:
            # Device-level descriptors are the device's, whichever
            # configuration is active (dual: configuration 2 is a UAC 2.0
            # function, its descriptors still the same device's).
            kind, number, d = value >> 8, value & 255, self.p1
            data = d.device if kind == 1 and number == 0 else \
                d.config if kind == 2 and number == 0 else \
                d.config2 if kind == 2 and number == 1 and d.config2 else \
                d.qualifier if kind == 6 and number == 0 else \
                d.strings.get(number) if kind == 3 else None
            answer = (0, data) if data is not None else (4, b'')
        elif request == 6 and rt == 0x81 and value >> 8 == 0x22 and index in p.hid and \
                getattr(p, 'hid_report', None):
            answer = (0, p.hid_report) if self.config else (4, b'')
        elif request == 6 and rt == 0x81 and value >> 8 == 0x21 and index in p.hid:
            answer = self.hid_descriptor(index)
        elif request == 0x09 and rt == 0x21 and index in p.hid and self.config:
            # SET_REPORT (HID 1.11 7.2.2): an output report (LEDs) taken and logged.
            self.log('hid_set_report', interface=index, report=value, data=payload.hex())
            answer = 0, b''
        elif self.uac1 and self.config and rt in (0x21, 0xa1) and index & 255 == p.ac:
            answer = self.entity_request1(request, rt == 0xa1, index >> 8, value >> 8, value & 255, payload)
        elif self.uac1 and self.config and rt in (0x22, 0xa2):
            answer = self.endpoint_request(request, rt == 0xa2, index & 255, value >> 8, value & 255, payload)
        elif self.uac1 and rt in (0x21, 0xa1, 0x22, 0xa2) and index & 255 not in p.hid:
            answer = 4, b''
        if answer is None:
            return super().control(ident, packet)
        status, data = answer
        data = data[:length]
        actual = len(data) if rt & 128 else (0 if status else length)
        self.send(100, ident, struct.pack('<BBBBHHH', ep, request, rt, status, value, index, actual) + data)
        self.log('control', request=request, request_type=rt, value=value, index=index,
                 requested=length, status=status, actual=actual)

    def hid_descriptor(self, interface):
        off, current = 9, None
        config = self.p.config
        while off + 2 <= len(config) and config[off] >= 2:
            d = config[off:off + config[off]]
            if d[1] == 4:
                current = d[2]
            elif d[1] == 0x21 and current == interface:
                return 0, bytes(d)
            off += config[off]
        return 4, b''

    def endpoint_request(self, request, get, ep, selector, low, payload):
        """UAC 1.0 5.2.3.2: SAMPLING_FREQ_CONTROL (1), PITCH_CONTROL (2)."""
        found = None
        for (i, a) in sorted(self.p.alts):
            info = self.p.stream_alt(i, a)
            if info and info['data']['address'] == ep:
                if found is None or self.alts.get(i) == a:
                    found = (i, a, info)
        if found is None or low:
            return 4, b''
        interface, alternate, info = found
        active = info if self.alts.get(interface) == alternate else None
        attributes = info['data'].get('cs_attributes', 0)
        merged, ranges = self.p.endpoint_rates(ep, active)
        if selector == 1 and attributes & 1:
            if get and request == GET_CUR:
                return 0, u24(self.ep_freq[ep])
            if get and request in (GET_MIN, GET_MAX, GET_RES) and ranges and not merged:
                lo, hi = ranges[0]
                return 0, u24({GET_MIN: lo, GET_MAX: hi, GET_RES: 1}[request])
            if not get and request == SET_CUR and len(payload) == 3:
                rate = int.from_bytes(payload, 'little')
                if rate_ok(rate, (merged, ranges)):
                    old = self.ep_freq[ep]
                    self.ep_freq[ep] = rate
                    self.log('ep_rate', endpoint=ep, rate=rate, previous=old, alternate=self.alts.get(interface))
                    return 0, b''
                self.log('ep_rate_refused', endpoint=ep, rate=rate)
        elif selector == 2 and attributes & 2 and request in (SET_CUR, GET_CUR):
            if get:
                return 0, bytes((self.pitch.get(ep, 0),))
            if len(payload) == 1 and payload[0] <= 1:
                self.pitch[ep] = payload[0]
                self.log('ep_pitch', endpoint=ep, value=payload[0])
                return 0, b''
        return 4, b''

    def entity_request1(self, request, get, entity, selector, channel, payload):
        e = self.p.entities.get(entity)
        if entity in self.p.stall_units:
            # The real device answers nothing for this unit.
            self.log('unit_stalled', entity=entity, request=request, selector=selector, channel=channel)
            return 4, b''
        if e is None or (get and request not in (GET_CUR, GET_MIN, GET_MAX, GET_RES)) or \
                (not get and request != SET_CUR):
            return 4, b''
        if e['kind'] == 'fu':
            return self.feature1(entity, e, request, get, selector, channel, payload)
        if e.get('unit') == 'mixer':
            return self.mixer1(entity, e, request, get, selector, channel, payload)
        if e.get('unit') == 'selector' and not channel and selector == 0:
            if get and request == GET_CUR:
                return 0, bytes((self.unit_sel[entity],))
            if get and request in (GET_MIN, GET_MAX, GET_RES):
                return 0, bytes(({GET_MIN: 1, GET_MAX: len(e['inputs']), GET_RES: 1}[request],))
            if not get and len(payload) == 1 and 1 <= payload[0] <= len(e['inputs']):
                self.unit_sel[entity] = payload[0]
                self.log('selector_unit', entity=entity, input=payload[0])
                return 0, b''
        return 4, b''

    def feature1(self, entity, e, request, get, selector, channel, payload):
        size = FU_CONTROLS.get(selector)
        if size is None or channel > e['channels'] or not e['controls'][channel] >> (selector - 1) & 1:
            if channel != 0xff:
                self.log('feature_absent', entity=entity, selector=selector, channel=channel, request=request)
            return 4, b''
        key = (entity, channel)
        if selector == 2:
            lo, hi, res, cur = self.p.fu_ranges.get(entity, uacemu.VOLUME_RANGE + (0,))
        elif selector in (3, 4, 5):
            (lo, hi, res), cur = TONE_RANGE, 0
        elif selector == 8:
            (lo, hi, res), cur = DELAY_RANGE, 0
        else:
            lo, hi, res, cur = 0, 1, 1, 0
        store = self.mute if selector == 1 else self.volume if selector == 2 else self.fu_other
        skey = key if selector in (1, 2) else (entity, channel, selector)
        boolean = selector in (1, 7, 9, 10)
        fmt = {1: '<B' if boolean or selector == 8 else '<b', 2: '<h' if selector == 2 else '<H'}[size]
        if get:
            if request == GET_CUR:
                if selector == 2 and self.vol_stall > 0:
                    self.vol_stall -= 1
                    self.log('feature_stalled', entity=entity, channel=channel)
                    return 4, b''
                return 0, struct.pack(fmt, store.get(skey, cur))
            if boolean:
                return 4, b''       # UAC 1.0: mute, AGC, bass boost, loudness have CUR only
            return 0, struct.pack(fmt, {GET_MIN: lo, GET_MAX: hi, GET_RES: res}[request])
        if len(payload) != size:
            return 4, b''
        value = struct.unpack(fmt, payload)[0]
        if boolean:
            if value > 1:
                self.log('feature_refused', entity=entity, selector=selector, channel=channel, value=value)
                return 4, b''
        elif not (selector == 2 and value == -32768):
            snapped = max(lo, min(hi, value))
            if res > 0:
                snapped = lo + round((snapped - lo) / res) * res
                snapped = min(snapped, hi)
            if snapped != value:
                if self.p.strict_volume:
                    self.log('feature_refused', entity=entity, selector=selector, channel=channel, value=value)
                    return 4, b''
                self.log('feature_adjusted', entity=entity, selector=selector, channel=channel,
                         value=value, kept=snapped)
                value = snapped
        store[skey] = value
        self.log('feature', entity=entity, control=FU_NAMES[selector], selector=selector, channel=channel,
                 value=value, db=value / 256 if selector == 2 and value != -32768 else None)
        return 0, b''

    def mixer1(self, entity, e, request, get, icn, ocn, payload):
        """Mixer Control (UAC 1.0 5.2.2.3): wValue input channel << 8 |
        output channel, 1-based, 1/256 dB."""
        inputs = sum(self.cluster(i) for i in e['inputs'])
        if not 1 <= icn <= inputs or not 1 <= ocn <= e['channels']:
            return 4, b''
        index = (icn - 1) * e['channels'] + (ocn - 1)
        bits = e['mixer_controls']
        settable = index // 8 < len(bits) and bits[index // 8] >> (7 - index % 8) & 1
        lo, hi, res = self.p.mx_ranges.get(entity, (-15360, 0, 256))
        # Default: each input channel to the output channel of the same
        # number at 0 dB (the streamed signal), every other crosspoint silent.
        default = 0 if icn == ocn else -32768
        if get:
            if request == GET_CUR:
                return 0, struct.pack('<h', self.mix.get((entity, index), default))
            return 0, struct.pack('<h', {GET_MIN: lo, GET_MAX: hi, GET_RES: res}[request])
        if settable and len(payload) == 2:
            value = struct.unpack('<h', payload)[0]
            if value == -32768 or lo <= value <= hi:
                self.mix[(entity, index)] = value
                self.log('mixer', entity=entity, input=icn, output=ocn, value=value)
                return 0, b''
        self.log('mixer_refused', entity=entity, input=icn, output=ocn)
        return 4, b''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=44414)
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--profile', choices=PROFILE_NAMES + tuple(ALIASES), default='headset')
    ap.add_argument('--speed', choices=('full', 'high'))
    ap.add_argument('--vid', type=lambda s: int(s, 16))
    ap.add_argument('--pid', type=lambda s: int(s, 16))
    ap.add_argument('--refresh', type=int, choices=range(1, 10), metavar='1..9',
                    help='bRefresh of the sync endpoints (polled every 2^N ms): async default 2; '
                         'on play2 it replaces the captured 5 (a rig option)')
    ap.add_argument('--sync-attr', type=lambda s: int(s, 16), default=0x01,
                    help='async: the sync endpoint bmAttributes, 01 (UAC 1.0), 05 or 11 (USB 2.0 feedback)')
    ap.add_argument('--fb-at-refresh', action='store_true',
                    help='send a feedback packet every 2^bRefresh ms instead of every frame')
    ap.add_argument('--fb-lead-empty', type=int, default=0, metavar='N',
                    help='the first N feedback packets of each stream are zero-length')
    ap.add_argument('--fb-ppm', type=float, default=0.0, help='feedback offset from the rate, ppm')
    ap.add_argument('--fb-zero-every', type=int, default=0)
    ap.add_argument('--fb-stop-after', type=float)
    ap.add_argument('--fifo-ms', type=float, default=4.0, help='the modelled OUT FIFO depth, ms')
    ap.add_argument('--strict-volume', action='store_true',
                    help='refuse (stall) a volume SET_CUR off the range or between steps')
    ap.add_argument('--vol-cur-stall', type=int, default=0, metavar='N')
    ap.add_argument('--pcm', type=Path)
    ap.add_argument('--pcm-max', type=int, default=4, choices=range(1, 257), metavar='MIB')
    args = ap.parse_args()
    try:
        profile = Profile1(args.profile, args.vid, args.pid, speed=args.speed, sync_attr=args.sync_attr,
                           refresh=args.refresh, strict_volume=args.strict_volume)
    except ValueError as error:
        ap.error(str(error))
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open('x', encoding='utf-8') as trace, socket.socket() as server:
        server.bind(('127.0.0.1', args.port))
        server.listen(1)
        print(f'SIMULATED UAC1 {args.profile} listening on 127.0.0.1:{args.port}', flush=True)
        connection, _ = server.accept()
        with connection:
            connection.settimeout(3)
            pcm = args.pcm.open('xb') if args.pcm else None
            model = Model1(connection, trace, pcm=pcm, profile=profile, fifo_ms=args.fifo_ms,
                           fb_at_refresh=args.fb_at_refresh, fb_lead_empty=args.fb_lead_empty,
                           fb_ppm=args.fb_ppm,
                           fb_zero_every=args.fb_zero_every, fb_stop_after=args.fb_stop_after,
                           pcm_max=args.pcm_max, vol_stall=args.vol_cur_stall)
            model.log('profile', uac=1, name=profile.name, speed=profile.speed,
                      config_sha256=hashlib.sha256(profile.config).hexdigest())
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

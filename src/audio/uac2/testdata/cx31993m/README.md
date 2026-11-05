# CX31993 with a MAX97220 headphone amplifier: descriptors

Test input for `uac2`'s host-side descriptor parser and the `cx31993m`
profile of `uacemu.py`. A user's USB headphone dongle (`3302:12C3`,
product string "USB Audio"; a Conexant/Synaptics CX31993 DAC driving a
MAX97220 headphone amplifier), read on 2026-11-04 under xHCI98 3.4.0.0 on
Windows XP x64 by XHCITR, whose report prints each descriptor's bytes in
hex. The bytes here are those hex lines, concatenated in report order and
checked against `wTotalLength`. Nothing was written to the device. The
Feature Unit's volume RANGE and the clock's rate RANGE are answers to
control requests, not descriptors, and the report does not hold them.

| File | What |
|---|---|
| `device.bin` | Device descriptor, 18 bytes: USB 2.00, class `EF/02/01`, `3302:12C3`, `bcdDevice` 0.01, one configuration |
| `config0.bin` | Configuration 1 at High Speed, 285 bytes (`wTotalLength`) |

Strings: 1 "TTGK Technology", 2 "USB Audio"; the serial number (index 3)
is not recorded.

SHA-256:

```
073e30fef717c65e445f40bab95b5c924b22f1b063297bc23789c66052ef1e9a  device.bin
9a0d314a5a47ee458ed16a73583d600c6a813380b48f312b29c01cd81ae90f69  config0.bin
```

What it holds, in short: interfaces 0-1 a UAC 2 function in an IAD
(`01/00/20`), interface 2 HID (interrupt IN `0x86`, OUT `0x05`).
Interface 0 is AudioControl (ADC 2.00, no interrupt endpoint): one
internal programmable clock source (`0x1E`, synchronized to SOF,
frequency read/write); a USB streaming input terminal (`0x0E`, 2
channels) to Feature Unit `0x10`, which has volume read/write on each of
its two channels and nothing on the master (no mute at all); an
Extension Unit (`0x04`, `wExtensionCode` `0x0BDA`, one input from `0x10`,
2 channels, its enable control read only) feeds the headphones output
terminal (`0x0F`, type `0x0302`). Interface 1 is playback only: stereo at
16, 24 and 32 bits (alternates 1-3, subslots 2, 3 and 4), ADAPTIVE OUT
`0x07` (`bmAttributes` `0x09`, no feedback endpoint), `wMaxPacketSize`
248, 372 and 496 bytes, `bInterval` 1.

# Sound Blaster X1 descriptors

Test input for `uac2`'s host-side descriptor parser and the `x1` profile of
`uacemu.py`. Read 2026-11-05 from the owner's unit on a Windows 11 host,
the device's functions bound to WinUSB through Zadig, through WinUSB's
`WinUsb_GetDescriptor` and a standard `GET_DESCRIPTOR` control request (the
whole configuration; USBCCGP gives a function only its own part). Nothing
was written to the device.

| File | What |
|---|---|
| `device.bin` | Device descriptor, 18 bytes: USB 2.00, class `EF/02/01`, `041E:3286`, `bcdDevice` 1.00, one configuration |
| `qualifier.bin` | Device qualifier, 10 bytes |
| `config0.bin` | Configuration 1 at High Speed, 707 bytes (`wTotalLength`) |
| `strings.txt` | String descriptors 1-6 and 8, language `0x0409`; the serial number (index 7) is not recorded |

SHA-256:

```
07672cfdda429d1169442e830e5d8020f35e6b3a4a96242ac44845ddd1b5e9dc  device.bin
f5aaffc6b16cc1c7a37e76ca4a227f3fec59f1933c8b29385c2b7a6a89568ff4  qualifier.bin
4ec053a80b67e1bf7b6f468fad80e09c8992109d4f6234ba95dff9c8f86531ad  config0.bin
```

What it holds, in short: interface 0 HID (interrupt IN `0x85`); interfaces
1-2 CDC ACM in an IAD (`02/02/00`); interfaces 3-5 a UAC 2 function in an
IAD (`01/00/20`). Interface 3 is AudioControl (ADC 2.00): two internal
programmable clock sources (33 for playback, 37 for capture), each behind a
one-input clock selector (34, 38); a USB streaming input terminal (3, 8
channels) through feature unit 1 to a speaker (5), and a microphone (4,
1 channel) through feature unit 2 to the USB streaming output terminal
(6); an interrupt endpoint `0x84`. Interface 4 is playback: 2, 6 and 8
channels at 16 and 24 bits (alternates 1-6), async OUT `0x01` with explicit
feedback `0x83`. Interface 5 is capture: 1 channel at 16 and 24 bits
(alternates 1-2), async IN `0x81` marked implicit feedback. Every
isochronous endpoint has bInterval 1 or 3 and one transaction per
microframe, at most 384 bytes.

The qualifier is byte-identical to the X4's (`testdata/sbx4`).

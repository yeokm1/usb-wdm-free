# Sound Blaster X4 descriptors

Test input for `uac2`'s host-side descriptor parser (`uac2.md` section
10). Read 2026-10-21 from the owner's unit on a Windows 11 host, the
device bound to WinUSB through Zadig, with libusb-1.0 (QEMU 11.1.0's
DLL): standard `GET_DESCRIPTOR` requests only, nothing written to the
device. The configuration read live was byte-identical to libusb's
cached copy taken before the replug.

| File | What |
|---|---|
| `device.bin` | Device descriptor, 18 bytes: USB 2.00, class `EF/02/01`, `041E:3278`, `bcdDevice` 10.70, one configuration |
| `qualifier.bin` | Device qualifier, 10 bytes (the other-speed configuration exists) |
| `config0.bin` | Configuration 1 at High Speed, 1143 bytes (`wTotalLength`) |
| `config0.txt` | A decode of `config0.bin`, for reading only; the binary is the input |
| `xpconf.bin` | 1044-byte partial configuration returned by XP SP3's USBCCGP to the audio function PDO (`MI_03`), captured through this project's M0 descriptor URB diagnostics and UACPROBE on 2026-10-21; no IAD, interface numbers 3-6 preserved |
| `strings.txt` | String descriptors 1-16, language `0x0409`; the serial number (index 9) is not recorded |

SHA-256:

```
6a340ec6e5a54666c2fdba954773e56eac4014561a89c2a21bd60c109ef2e884  device.bin
f5aaffc6b16cc1c7a37e76ca4a227f3fec59f1933c8b29385c2b7a6a89568ff4  qualifier.bin
89f0b2b70a9be2ddfd9bacb587db7426b8ac5d14727f7e42b6b21111fdafa7e7  config0.bin
e5f04f1787dcc1a351480546f0fa9d7147caf9ffcf9c1feb893cb5ffbaa6cfdf  xpconf.bin
```

What it holds, in short: interface 0 HID; interfaces 1-2 CDC ACM in an
IAD (`02/02/00`); interfaces 3-6 a UAC 2 function in an IAD
(`01/00/20`). Interface 3 is AudioControl (ADC 2.00, three internal
programmable clock sources each behind a one-input clock selector, an
interrupt endpoint `0x84`). Interface 4 is playback (2/6/8 channels,
16/24 bit, async OUT `0x01` with explicit feedback `0x83`), interface 5
capture (2 channels, 16/24 bit, async IN `0x81` marked implicit
feedback), interface 6 S/PDIF out (PCM, and a Type III encoded
alternate; explicit feedback `0x85`). All isochronous packets are at
most 1024 bytes, one transaction per microframe.

The XP partial configuration is byte-identical to a generated fixture
consisting of the original configuration header (with total length 1044
and interface count 4), followed by the original descriptors from
interface 3 onward, excluding the IAD. Host tests check that relationship
and parse both inputs. A parser cannot require an IAD to remain visible
after USBCCGP has split a function.

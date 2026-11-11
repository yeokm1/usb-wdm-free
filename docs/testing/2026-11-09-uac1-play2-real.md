# uac1 0.1.9 with the real Sound Blaster Play! 2 on 98 SE, 2026-11-09

The open item "`uac1`: the real Sound Blaster Play! 2 on 98 SE"
([open items](open-items.md)): the owner's Creative Sound Blaster Play! 2
(`041E:323D`, UAC 1.0, Full Speed), attached to development host A and
bound to WinUSB there, passed to 98 SE guests by QEMU `usb-host`
(claimed in the host's device log before, released after; the host's
binding was not changed). The earlier attempt (2026-11-07) heard nothing:
QEMU's isochronous OUT was refused on the host. These legs find what works
through passthrough, and why the sound did not reach the device.
Development legs in QEMU (TCG), not "Tested".

**Outcome.** Under xHCI98, with `usb-host`'s `isobsize=1`, every step
passed and the host refused no transfer: install, `uac1` bound, the device
listed, its mixer read, playback at 48 and 44.1 kHz, the volume and mute
ramp, recording at 48 and 44.1 kHz with the device's own input data (the
host's one error per stream end, on returning to alternate setting 0, is
below). On the owner's bench, the same release on a physical 98 SE machine
under xHCI98 played clean and recorded clean at 16-bit (below). Under
NUSB's UHCI companion, passthrough could
not carry both directions with any setting tried; the passthrough path
through QEMU's UHCI model is the likely limit, not proven (below).

## Rig

- **Driver:** `uac1` 0.1.9 as released (`free/audio/uac1/w98se`):
  `uac1wdm.sys` `2251016ca69c0daf28cb2ac1c31d9ae27111fb17e2eff946b8ab42f75338f222`,
  `uac1wdm.inf` `23408f99338321805900d1733fc6d0b803c7f1ce0efb8dabd963990e524768e3`,
  staged on `DevicePath` before the first plug (as xHCI98's installer does).
- **Emulator:** the usb4-emu fork `cb36497` (`3e7f6bf5...`), its libusb
  1.0.30 (WinUSB backend), `pc` machine, one vCPU, `qemu-xhci`,
  `usb-ehci` and `piix3-usb-uhci` at every boot.
- **Bases** (read-only, a fresh overlay per leg): `w98k` (98 SE, NUSB 3.3,
  xHCI98 `ca0001e1`, 3.4.2.0's unplug fix; the device on `qemu-xhci`) and
  `w98n2` (98 SE with NUSB 3.3; a Full Speed device on its UHCI
  companion), as in [uac1-98se](2026-11-07-uac1-98se.md).
- **Device:** `usb-host,vendorid=0x041e,productid=0x323d` plus the
  options below, with `pcap=` (QEMU's capture of the guest's transfers).
- **Guest tool:** `UACWAV.EXE` (`cf5adea0...`): `list`, `mix`, `play 2 16
  <rate> <s>` (a -20 dBFS sine), `ramp 1`, `rec 2 16 <rate> 3` (counts
  the samples that are not zero and the zero crossings).
- **Host check:** each step counts the libusb errors QEMU printed while
  it ran. The traced legs add `-trace usb_host_iso_*` (stream starts and
  stops, and `usb_host_iso_out_of_bufs`: an OUT packet QEMU dropped for
  want of a free host transfer).

## Why nothing reached the device (QEMU's defaults)

QEMU's `usb-host` sends isochronous OUT data in libusb transfers of 32
packets (`isobsize`, default 32). libusb's WinUSB backend accepts an
isochronous OUT transfer only if every packet but the last carries the
endpoint's maximum (its `winusbx_submit_iso_transfer`, libusb 1.0.30;
[uac2-winusb-boundary](2026-10-21-uac2-winusb-boundary.md)). 48 kHz stereo
16-bit is 192 bytes a frame, under the Play! 2's maximum, so every
playback transfer was refused before it reached Windows' USB stack:
`libusb_submit_transfer [iso]: -2 [INVALID_PARAM]` (156 in the 5 s play,
92 in 3 s, 186 in the ramp). The guest saw its buffers complete (QEMU
completes the guest's transfers either way), so `UACWAV` reported PASS
while the device got nothing. Recording (isochronous IN) is not affected.

With `isobsize=1` each OUT packet is a transfer of its own, which that
check accepts; `isobufs` (default 4) sets how many may be queued.

## Legs

Each leg: install from `DevicePath` (Setup's prompts for the KS files
answered from the CD), a cold start, then for each option set: the
driver key, `list`, `mix`, `play` 48 kHz 5 s and 44.1 kHz 3 s, `ramp`,
`rec` 48 kHz and 44.1 kHz 3 s.

### Under xHCI98 (`uac1-w98-xh-p2a`; `-p2f` and `-p2g` traced)

| Step | QEMU defaults | `isobsize=1,isobufs=32` |
|---|---|---|
| install, `uac1wdm.sys` bound, "Sound Blaster Play! 2" listed (one waveOut, one waveIn) | PASS | PASS |
| mixer read | PASS, no host error | PASS, no host error |
| ramp of volume and mute (UAC 1.0 Feature Unit requests) while playing | PASS; the playback's OUT transfers refused (186) | PASS, no transfer refused |
| play 48 kHz 5 s, 44.1 kHz 3 s | buffers complete; every host OUT transfer refused (above) | PASS, no transfer refused |
| record 48 kHz 3 s | PASS: 576000 bytes, 113460 samples not zero | PASS: 576000 bytes, 112245 not zero |
| record 44.1 kHz 3 s | PASS: 529184 bytes, 81109 not zero | PASS: 529184 bytes, 102299 not zero |

- The recording is the device's own input with nothing plugged in
  (noise: about 26000 zero crossings in 3 s), read over isochronous IN
  through the host: the device's data reached `uac1` and the application.
- QEMU's capture (`ev\uac1-w98-xh-p2a-b1-d3.pcap`): 13997 OUT packets of
  192 bytes (12 of 196) in the plays, mean 48.0009 frames a millisecond;
  the device's feedback endpoint answered 456 polls with 3 bytes
  (48.000 kHz and up) where the defaults' leg had 2.
- The traced repeats, `p2f` (`isobsize=1,isobufs=32`) and `p2g`
  (`isobsize=1,isobufs=64`), passed every step as `p2a` did, with no
  transfer refused. QEMU dropped 591 and 579 OUT packets (`usb_host_iso_out_of_bufs`)
  of about 16000 in the plays and the ramp, with 32 and with 64 host
  transfers alike: how those gaps sound is for the bench to hear.
- One `libusb_set_interface_alt_setting -1 [IO]` per stream end on the
  host, under every setting: returning to alternate setting 0 after the
  stream; the next stream started as usual.

### Under NUSB's UHCI companion (`uac1-w98-nusb-p2c`, `-p2d`, `-p2e`)

| Option set | install, bind, list, mixer | play | ramp | record |
|---|---|---|---|---|
| QEMU defaults (`p2d`) | PASS | buffers complete; every host OUT transfer refused | PASS (OUT refused) | PASS 48 and 44.1 kHz |
| `isobsize=4,isobufs=16` (`p2d`) | PASS | FAIL: buffers not completed; host OUT refused | PASS (OUT refused) | 48 kHz PASS, 44.1 kHz FAIL |
| `isobsize=1,isobufs=32` (`p2c`) | PASS | PASS, no transfer refused; 3963 OUT packets dropped by QEMU (`out_of_bufs`) | FAIL: 3 of 8 buffers | FAIL: 0 of 8 buffers |
| `isobsize=1,isobufs=64` (`p2e`) | PASS | FAIL: buffers not completed; 3328 OUT packets dropped | PASS | FAIL |

- The OUT check is the host's (above). With one packet a transfer QEMU's
  UHCI model, which runs frames in bursts, outran the host transfers and
  dropped packets, and isochronous IN starved: QEMU restarted the
  streams 16 to 32 times a leg (`usb_host_iso_start`/`stop`).
- Under xHCI98 the same device, host and options passed (above). The
  drops and stream restarts point at the passthrough path through QEMU's
  UHCI model on this host, but a change of stack also changes the
  driver's timing, so a part of `uac1` with NUSB's UHCI driver is not
  excluded here. NUSB's UHCI with a simulated UAC 1.0 device passed play
  and record in [uac1-98se](2026-11-07-uac1-98se.md).
- A first NUSB leg (`p2b`) is void: the guest stood still for about five
  minutes after the device arrived (98 SE's first-plug wedge, task
  36.41), and no wave device came up afterwards.

## The owner's bench (2026-11-09)

The owner ran the real Play! 2 under `uac1` 0.1.9 on 98 SE with xHCI98
3.4.2.0 on a ThinkPad P14s Gen 1 (a physical machine, no emulator),
through Sound Recorder and the Volume Control:

- **Playback clean:** 1 kHz tones, left and right, and 50 Hz to 15 kHz
  sweeps, at 48 and at 44.1 kHz.
- **Volume and mute** followed the Volume Control.
- **Recording** from a microphone on the Play! 2: at Sound Recorder's
  default, 22,050 Hz 8-bit mono, it had a lot of static; recorded again
  at PCM 44,100 Hz 16-bit stereo it was clean. The owner took the static
  for the 8-bit format's quantisation; that was not tested further.
- **Unplug and replug** with nothing playing, in the same port and in
  another: the Play! 2 stayed the preferred device for playback and
  recording, and a 48 kHz tone played clean.

Still owed: the Play! 2 under NUSB's UHCI companion, which needs a 98 SE
machine with UHCI ports (not readable through this host's passthrough).

## Rig notes for later legs

- The Play! 2's playback through QEMU `usb-host` on this Windows host
  with WinUSB needed `isobsize=1`; with the default 32 nothing was played,
  whatever the guest reported (WinUSB refuses an OUT transfer whose
  packets before the last are short of the endpoint's maximum, so a
  device whose packets fill the endpoint would not meet it). That held
  under `qemu-xhci`; under QEMU's UHCI model `isobsize=1` starved the
  Play! 2's recording instead.
- Count the host's libusb errors per step; a guest PASS alone does not
  show that the data left the host.

## Evidence

`E:\xhci98-vm\p48\wdma\play2\` (development host A): `ev\results.jsonl`
(one line per step, each with its host-error count), `ev\*.pcap`,
`g\<leg>\qemu-*.out` (QEMU's output: libusb errors, the traced legs'
`usb_host_iso_*` lines), `g\<leg>\serial-*.log`, `shots\`, `pids.txt`;
`E:\xhci98-vm\devices.txt` (the claim and the release).

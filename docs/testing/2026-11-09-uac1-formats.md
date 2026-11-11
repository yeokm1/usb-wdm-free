# uac1 0.1.1.0: the layouts 98 SE's own driver runs, 2026-11-09

Design [uac2.md](../design/uac2.md) section 26.12 (xHCI98 Phase 48, task
48.1, owner 2026-11-09): `uac1` runs PCM8, IEEE float, A-law and mu-law
through a 16-bit PCM pin, MaxPacketsOnly both ways, and feeds Ff to an
adaptive source's synch endpoint. These are the 98 SE legs of 26.12 under
xHCI98, with `uac1emu.py`'s profiles, and a `uac2` regression leg on the
same base. Development legs in QEMU (TCG) with the simulator, not a
release and not "Tested". `uac1` stays a 98 SE package for testing in a
virtual machine (README, owner 2026-11-09).

**Outcome.** Under xHCI98, every profile of 26.12 was bound by `uac1` and exposed a
wave device (no null start), and every playback and recording step
passed on its last run: playback in PCM8, IEEE float, A-law, mu-law and
MaxPacketsOnly PCM8, recording from an adaptive source at Full and High
Speed, in PCM8, IEEE float, A-law, mu-law and MaxPacketsOnly PCM at 44.1
and 48 kHz. The model read UACWAV's 440 Hz tone within 1% in every
playback (437.0 to 442.0 Hz) and at the same level as the 16-bit PCM
control within 0.3 dB; UACWAV read the model's 1 kHz tone within 0.3%
(999 to 1003 Hz) at -6.0 dBFS (-5.8 to -6.2 for G.711). The level
UACWAV plays at (-20 dBFS) reached the device 5.5 dB lower in every
stream, the PCM control included, so the absolute level criterion of
26.12 is not met on 98 SE (see "Results"). The adaptive
source's synch endpoint carried the rate's Ff in every packet (48.0 and
44.1 samples a frame at Full Speed, 6.0 and 5.5125 a microframe at High
Speed), none off the rate. Two things are not clean: the first A-law
session failed every step (not reproduced in the next run; see "A-law,
first session"), and 98 SE's first-plug wedge of an audio device (36.41
in xHCI98's records, a known 98 SE behaviour with `uac1` and `uac2`
alike) cost several plugs, three in a row for `floatin` in the first
leg. Under 98 SE's own UHCI (run as time allowed) the formats played
and recorded too, but that stack showed stalled streams and devices
WinMM did not list, not tied to a format and not settled ("Under 98
SE's own UHCI").

## Rig

- **Driver:** `uac1` 0.1.1.0 (build 2), usb-wdm-free `294c891`, x86 free,
  `uac1wdm.sys` `5e1e4effe99bb1d9...e306` (import gate PASS),
  `uac1wdm.inf` `23408f99...68e3`, staged on `DevicePath`. `uac2` for the
  regression leg: built from the same tree, `uac2wdm.sys`
  `44e4096badb66242...dd14` (equivalent to `main`'s, 26.11's
  comparison), its 98 SE INF.
- **Emulator:** the usb4-emu fork `cb36497` (`3e7f6bf5...`), one vCPU,
  `pc` machine, 256 MB.
- **98 SE:** the `uac1` kit's base `w98k` with xHCI98 (option B, as
  26.11's legs), the device on `qemu-xhci`; a fresh overlay per leg.
- **Simulator:** `uac1emu.py` at `294c891` (the profiles of 26.12).
- **Steps per profile:** first plug and install from `DevicePath`, a cold
  start, the device plugged again; the driver from the `MEDIA` class key
  (`NTMPDriver` `uac1wdm.sys`, `Uac2Build` 2); `UACWAV list` (the
  profile's output or input listed: no null start); playback `UACWAV
  play 2 16 48000 5`, `play 2 16 44100 3`, `ramp 1`; recording `UACWAV
  rec <ch> 16 48000 3` and `44100 3`. UACWAV `8d7702a0...` (its `rec`
  prints the first channel's peak level).

## Results

Playback: the model's reading of each stream (level of the received
samples, tone from channel 1's zero crossings, the stream's verdict:
packet sizes within one frame of the rate, no under- or overrun, no
misframed or, for MaxPacketsOnly, unpadded packet).

| Profile | Wire format | play 48k / 44.1k / ramp | Received (play) | Tone (Hz) | Verdict |
|---|---|---|---|---|---|
| `headset` (control) | PCM 16-bit, 2 ch | PASS / PASS / PASS | -25.5 dBFS | 438.0, 438.4 | ok |
| `pcm8` | PCM8, 2 ch | PASS / PASS / PASS | -25.24 dBFS | 438.9, 437.5 | ok |
| `float` | IEEE float, 2 ch | PASS / PASS / PASS | -25.5 dBFS (0 samples out of range) | 437.9, 438.2 | ok |
| `mpoout` | PCM8 MaxPacketsOnly, 2 ch | PASS / PASS / PASS | -25.24 dBFS | 437.9, 437.6 | ok, every packet padded |
| `alaw` (second run) | A-law, 2 ch | PASS / PASS / PASS | -25.4 dBFS | 437.0, 442.0 | ok |
| `mulaw` | mu-law, 2 ch | PASS / PASS / PASS | -25.42 dBFS | 437.9, 437.7 | ok |

UACWAV plays at -20 dBFS; on 98 SE every stream, the 16-bit PCM control
included, reaches the device 5.5 dB lower (-25.5 dBFS; the ramp's -6 dBFS
step -11.5), as `uac2`'s 98 SE legs read before
([uac2-volume](2026-10-28-uac2-volume.md): -11.52 dBFS on 98 SE where XP
reads -6.02). So 26.12's criterion as written, the tone within 1 dB of
-20 dBFS, is not met on 98 SE by any stream, the PCM control included.
Where the 5.5 dB is lost is not settled: the same offset with both drivers
and on no NT system points at 98 SE's KMixer path, but both drivers share
the wave code, so that is a hypothesis (settled since: 98 SE's own Wave
line defaults to about half, -5.5 dB, on this base, under `uac2` and
Windows' own driver alike;
[uac-98se-level](2026-11-09-uac-98se-level.md)). What these legs show is format
parity: each format read within 0.3 dB of the control (PCM8 and G.711
quantise). KMixer
opened every playback pin at 48 kHz, also for the 44.1 kHz client, so the
44.1 kHz wire rate was not run in playback.

Recording: UACWAV's reading (tone, first channel's peak, buffers).

| Profile | Wire format | rec 48k | rec 44.1k | Wire rates |
|---|---|---|---|---|
| `headset` (control) | PCM 16-bit, 1 ch | 999 Hz, -6.0 dBFS, 8/8 | 999 Hz, -6.0, 8/8 | 48000, 44100 |
| `adsrc` | PCM, adaptive source, synch OUT | 999 Hz, -6.0, 8/8 | 999 Hz, -6.0, 8/8 | 48000, 44100 |
| `adsrc-hs` | the same at High Speed | 999 Hz, -6.0, 8/8 | 999 Hz, -6.0, 8/8 | 48000, 44100 |
| `pcm8in` | PCM8 in a 2-byte subframe, 1 ch | 999 Hz, -6.0, 8/8 | 999 Hz, -6.0, 8/8 | 48000, 44100 |
| `floatin` (second run) | IEEE float, 2 ch | 999 Hz, -6.0, 8/8 | 999 Hz, -6.0, 8/8 | 48000, 44100 |
| `mpoin` | PCM MaxPacketsOnly, synchronous, 2 ch | 1000 Hz, -6.0, 8/8 | 1000 Hz, -6.0, 8/8 | 48000, 44100 |
| `mpoin48` | the same, 48 kHz only | 1000 Hz, -6.0, 8/8 | 1003 Hz, -6.0, 8/8 | 48000 |
| `alaw` (second run) | A-law, 1 ch | 1000 Hz, -6.2, 8/8 | 1003 Hz, -6.1, 8/8 | 48000, 16000 |
| `mulaw` | mu-law, 1 ch | 999 Hz, -5.8, 8/8 | 1003 Hz, -5.8, 8/8 | 48000, 16000 |

The adaptive source's synch endpoint (the model's log of every value it
received):

| Profile | Rate | Ff values | Off the rate |
|---|---|---|---|
| `adsrc` | 48000 | 48.0 in 3047 packets | 0 |
| `adsrc` | 44100 | 44.1 in 3068 packets | 0 |
| `adsrc-hs` | 48000 | 6.0 in 2616 packets | 0 |
| `adsrc-hs` | 44100 | 5.5125 in 2687 packets | 0 |

`uac2` regression, `uacemu.py --profile hs24` on the same base, `uac2wdm.sys` bound (`Uac2Build` 26): play at 48 and 44.1 kHz and the ramp PASS, the model receiving -25.5 dBFS (the ramp -11.52), as `uac1`'s PCM control; recording at 48 and 44.1 kHz 999 Hz, -6.0 dBFS, 8 of 8 buffers.

### A-law, first session

In the first playback leg the `alaw` device installed, was bound and
listed both its output and its input, and then every step failed:
`play` opened and completed 0 of 8 buffers in 15 s (no alternate of the
playback interface was ever selected: the model saw no SET_INTERFACE on
it), the volume and mute steps of `ramp` worked, and `rec` opened and
completed 0 of 8 buffers while the model streamed the capture endpoint
the whole time (48 and 16 kHz). The next device in the same leg,
`mulaw`, with the same descriptors but the tag, passed every step, and
the `alaw` device on a fresh overlay passed every step (and so in a
third run on another fresh overlay). Cause not settled: nothing A-law-specific runs before
the first sample is converted, and the playback stream never reached the
USB layer. Recorded as a watch item.

### First-plug wedges

98 SE stopped answering after a device's first plug of a boot (the
desktop frozen, the clock stopped) 9 times in these legs (`headset` in the harness's trial run on the build before review, `float` twice, `pcm8in` once, `floatin` four times, `alaw` once in its second run); the harness reset the
guest, cold-started it and plugged the device again, as 26.11's legs did
(36.41 in xHCI98's records: seen with `uac1` and `uac2` alike). In the
first capture leg `floatin` wedged three times in a row and the harness
gave up (recorded as an install failure); in its rerun it wedged once, then passed every step.

## Under 98 SE's own UHCI

The Full Speed profiles on the kit's base `w98p` (98 SE as installed, its
own UHCI driver only), the same build and steps (legs `uac1-w98-own-owna`
and `-ownb`). Every device but `mpoin48` (which never got past its
first plugs) was bound by `uac1` and started (the driver read its
controls), but this stack is not clean:

| Profile | Result |
|---|---|
| `headset` (control) | play, ramp, rec PASS (one first-plug wedge) |
| `pcm8`, `float` | play 48k, 44.1k and ramp PASS; the model as under xHCI98 (-25.24 and -25.5 dBFS, 437 to 439 Hz) |
| `mpoout` | no wave device listed after the plug: play and ramp FAIL ("no UAC2 output") |
| `alaw` | play 48k, ramp, rec 48k and 44.1k PASS (1000 and 1003 Hz, -6.2 and -6.1 dBFS); play 44.1k FAIL: 1 of 8 buffers, the stream stopped after 240 packets |
| `mulaw` | play, rec PASS (-25.42 dBFS; 999 and 1003 Hz, -5.8); ramp FAIL: 1 of 8 buffers |
| `adsrc` | rec 48k FAIL (0 of 8); rec 44.1k PASS (999 Hz, -6.0), the synch values 44.1 in every packet of that stream (48.0 in the 42 of the failed one), none off the rate (two first-plug wedges) |
| `pcm8in` | after the plug the guest answered only after about four minutes, and then listed no wave device: rec FAIL ("no UAC2 input") |
| `floatin` | rec 48k and 44.1k PASS (999 Hz, -6.0) (one wedge) |
| `mpoin` | rec 48k and 44.1k PASS (1000 Hz, -6.0) |
| `mpoin48` | three first-plug wedges in a row; the harness gave up |

Two kinds of failure, neither tied to a format: a device that started
but that WinMM did not list (`mpoout`, `pcm8in`, each after a first plug
from which the guest came back slowly), and a stream whose buffers
stopped completing (`alaw` play 44.1k, `mulaw` ramp, `adsrc` rec 48k),
the next stream of the same device passing. `uac1`'s earlier legs on this
stack (26.11) ran only 3 s steps and failed 3 of 8 plays and 3 of 7
recordings there too. Not settled whether the stall is the driver's on
this UHCI stack (as `uvc98`'s resync after `USBD_STATUS_BAD_START_FRAME`
was, [uvc98-fs-uhci](2026-11-09-uvc98-fs-uhci.md)), QEMU's UHCI model or
the loaded host; the stream counters (`Uac2UsbStats`) of a failed stream
are the next thing to read.

## Owed

- The same legs under NUSB's EHCI (UHCI for the Full Speed profiles in
  QEMU); under 98 SE's own UHCI, the stalled streams and the unlisted
  devices above (cause), and `mpoin48`.
- Failure injection on the synch pipe (8 failures in a row end the
  stream, 26.12).
- The A-law first-session failure: more runs to tell a rare fault from a
  one-off.
- Unplug while streaming for the new profiles (these legs ran with no
  unplug trials).
- Real devices of these layouts (none at hand).

Known limit (owner, 2026-11-09): MaxPacketsOnly capture from an
asynchronous source, and MaxPacketsOnly capture in PCM8, A-law or
mu-law, are not carried; such a device loads under `uac1` with no sound
device (no hang).

## Evidence

The kit's `ev\results.jsonl` (legs `uac1-w98-xh-play`, `-cap`, `-alaw2`,
`-cap2`, `uac2-w98-xh-u2hs24`, `uac1-w98-own-owna`, `-ownb`), the model logs `ev\<leg>-d<n>.jsonl` and
captures, the screens under `shots\`.

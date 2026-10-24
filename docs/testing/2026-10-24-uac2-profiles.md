# uac2 beyond the X4: simulator profiles on 98 SE, XP and 7, 2026-10-23/24

Builds 10 to 16 of `uac2` (branch `uac2-rel`) drive any UAC 2.0 function
the descriptor parser describes, not only the Sound Blaster X4's layout:
other channel counts and bit depths, the rates a clock's ranges hold,
several clock sources behind selectors (multipliers at 1/1), Full Speed,
and playback-only or capture-only functions. This record runs every
`uacemu.py` profile on 98 SE under xHCI98 and under NUSB 3.3's EHCI, and
the X4 layout with capture on XP SP3 and 7 x86 as a regression, then the
real Sound Blaster X4 on XP and 7 through the VM. Development legs in
QEMU, not "Tested". The final build is
**16** (`055629e`).

## What changed (builds 10 to 16)

| Build | Commit | Change |
|---|---|---|
| 10 | `18d6530`; Codex rounds 1-3 `20b10a8`, `753b694`, `630fdf6` | `uacplan.c` (formats, alternate pick, packet sizes, the direction's terminal); `uacusb.c` opens the function once (`UAC_DEV`) and creates a stream per direction for any format; the clock walk follows selectors (current input first, others if writable) and 1/1 multipliers; capture streams; Full Speed (10.14 feedback, 1 ms packets); WAVEFORMATEXTENSIBLE beyond stereo 16-bit; the INF matches `USB\Class_01&SubClass_00&Prot_20` and `SubClass_01&Prot_20` |
| 10 | `eaacf4c` | capture-only devices: the filter, not the INF, registers RENDER and CAPTURE (a RENDER interface on a capture-only filter faulted WDMAud) |
| 10 | `736397c` | the device's channel positions (`bmChannelConfig`) in `dwChannelMask`: KMixer mixed into an unassigned 8-channel layout as silence |
| 11 | `2178ac0` (Codex round 4) | partial layouts keep their named prefix; formats and the alternate pick carry the layout; a stream's declared positions must be the device's; stepped clock ranges; an unlocked external or read-only clock source leaves a selector's other inputs to try |
| 12, 13 | `c199008`, `9fbb739` (Codex rounds 5, 6) | the rate list collects every source's exact and common rates before stepped extras, clipped to 8..384 kHz; `SetFormat` keeps the mask declaration |
| 14 | `6baef5f` | data ranges declare 48 kHz first, then 44.1 kHz (KMixer, below) |
| 15 | `7207438` | capture ranges back to ascending |
| 16 | `055629e` | the second rate walk only when a clock offers a stepped range (the WDMAud fault, below) |

Codex: rounds 7, 8 and 9 clean (`9fbb739`, `6baef5f`, `055629e`).
Host tests (`test`, `ctest`, `mtest`, `rtest`, `streamtest`,
`uacsimtest.py`) and the four builds (x86 and x64, free and checked, with
the import gate) pass at `055629e`.

## Rig

Kit `E:\usb-wdm-free\nusb-legs\`; headless QEMU 11.1, TCG, 1 vCPU for
98 SE, 2 for XP and 7.

- **xHCI98**: overlay `x98u` of the 98 SE legs' `m98a`, xHCI98 3.1.0.0,
  `qemu-xhci`; monitor 49150, simulator 49151. Per build: a cold boot,
  `uac2wdm.sys` and `uac2wdm.inf` copied in before any audio arrival
  (snapshots `bootR4` to `bootR7`), then one X4-model arrival and unplug
  (snapshots `deskW7` to `deskW10`), so every profile leg is a later
  arrival in that boot (98 SE's first-arrival freeze, below). `prof.sh`
  restores the snapshot, hot-adds the profile on `xhci.0` port 1, runs
  `UACWAV list` and the profile's commands, exports the MEDIA class key,
  unplugs; `allprof.sh` runs the profiles in a row.
- **NUSB 3.3 EHCI**: overlay `nv1` of `w98-nusb.qcow2`, `usb-ehci`;
  monitor 49152, simulator 49153. QEMU cannot restore a snapshot of this
  guest (`msix` assertion with the APIC off), and two hot first arrivals
  after cold boots both froze the guest (build 14, `shots\nw2-n14a.png`,
  `nw2-n14b.png`), so each NUSB profile leg is a cold boot with the
  profile's simulator already on `ehci.0` port 2 (`nprof.sh`): the device
  is present at boot, as the 98 SE known-issue advice says. 98 SE plays
  its startup sound through the device during boot, so each NUSB leg has
  one extra playback stream at the simulator. Builds go in with
  `ninst.sh` (boot without a device, copy, shut down).
- **XP SP3 / 7 x86**: overlays `nxp1` of `wxp.qcow2` and `n7a` of
  `w786.qcow2`, `usb-ehci` with Windows' own EHCI driver; the X4 function
  installed with `MIDITEST install "USB\VID_041E&PID_3278&MI_03"
  A:\uac2wdm.inf` (`I3.BAT`; 7 from an elevated prompt), unsigned prompt
  accepted.
- `UACWAV.EXE` (`uacwav.c`, `wav.cmd`): `list`; `play <ch> <bits> <rate>
  <s>` (a 440 Hz sine through `waveOut`; PASS when every buffer
  completes); `rec <ch> <bits> <rate> <s>` (PASS when every buffer
  completes and samples are non-zero; the zero-crossing count and tone
  frequency it prints show whether the simulator's 1 kHz sine arrived).
- The simulator (`uacemu.py`, profiles from `d6bbd61`) logs every control
  request, alternate setting, stream start (the device-side format) and
  per-stream packet and non-zero byte counts.

The profiles: `x4` (the captured X4 layout), `x4full` (X4 with capture),
`hs24` (High Speed 2-channel 16/24/32-bit playback and 16/24-bit capture,
44.1 and 48 kHz families), `multi` (playback only: 8 channels 16-bit and 6
channels 24-bit), `mic` (capture only: mono 16/24-bit at 16, 48, 96 kHz),
`fs` (Full Speed: stereo playback with 3-byte feedback, mono capture),
`clocks` (two sources behind a selector, capture on a 1/1 multiplier),
`adaptive` (adaptive playback, synchronous capture, a read-only fixed
clock). Every non-X4 profile has VID `FFFF`.

## Binding: the class match ranks above `wdma_usb.inf`

On 98 SE every non-X4 profile bound `uac2wdm.sys` through
`USB\Class_01&SubClass_00&Prot_20` (MEDIA key `MatchingDeviceId`) under
xHCI98, and through `USB\Class_01&SubClass_01&Prot_20` under NUSB (the
audio control interface), not `wdma_usb.inf`'s `USB\CLASS_01`
(`USBAUDIO.sys`, still in the MEDIA class for an earlier device).
Evidence: `pf-b7mic\m.reg`, `pf-n9multi\m.reg`.

## 98 SE under xHCI98

`UACWAV` asks 2 channels 16-bit 48 kHz for playback; KMixer opens the
device in a format of its own choosing (the device-side column, from the
simulator's `stream_start`).

Build 16 (`055629e`), snapshot `deskW10`, `ev\allprof-g10.txt`,
`ev\allprof-f10.txt`:

| Profile | Command | Result | Device side | At the simulator |
|---|---|---|---|---|
| `x4` | play | PASS 8/8 | IF4 alt 1, 2 ch 16-bit 48 kHz | 16017 packets of 24 bytes, signal |
| `x4full` | rec 2/16/48k | PASS, 1 kHz tone (1999 crossings) | IF5 alt 1, 2 ch 16-bit 48 kHz | |
| | play | PASS 8/8 | IF4 alt 1 | 16011 packets, signal |
| `hs24` | play | PASS 8/8 | IF1 alt 1, 2 ch 16-bit 48 kHz | 16015 packets, signal |
| | rec 2/16/48k | PASS, tone | IF2 alt 1, 2 ch 16-bit 48 kHz | |
| `multi` | play | PASS 8/8 | IF1 alt 1, **8 ch** 16-bit 48 kHz | 16014 packets of 96 bytes; non-zero 373440 bytes (build 10 before `736397c`: 0, silence) |
| `mic` | rec 1/16/48k | PASS, tone, 2 of 2 legs | IF1 alt 1, 1 ch 16-bit 48 kHz | |
| | rec 1/24/96k | PASS, tone | IF1 alt 1, 1 ch 16-bit **16 kHz** (KMixer converts) | |
| `fs` | play | PASS 8/8 | Full Speed, IF1 alt 1, 2 ch 16-bit 48 kHz | 2008 packets of 192 bytes, signal |
| | rec 1/16/48k | PASS, tone | IF2 alt 1, 1 ch 16-bit 48 kHz | |
| `clocks` | play | PASS 8/8, signal (16015 packets of 24 bytes) | 2 ch 16-bit 48 kHz: the selector switched to input 2 (48 kHz source) | |
| | rec 2/16/48k | PASS, tone | 2 ch 16-bit 48 kHz on the multiplier | |
| `adaptive` | play | PASS 8/8, signal | adaptive, 2 ch 16-bit 48 kHz, no feedback endpoint | |
| | rec 2/16/48k | PASS, tone | synchronous capture 48 kHz | |

Build 11 (`2178ac0`, `ev\allprof-b7.txt`) passed the same table, every
command, with playback opened at 44.1 kHz on `hs24`, `fs` and `clocks`
(below). Build 14 and 15 (`ev\allprof-c8.txt`, `allprof-d9.txt`) passed
it except `mic` (below).

**KMixer opens the device at the first data range it can, not at the
client's rate.** Up to build 13 each format listed its rates ascending,
so `hs24`, `fs` and `clocks` played a 48 kHz client at 44.1 kHz (build
11). From build 14 playback lists 48 kHz first, then 44.1 kHz; those
profiles now play at 48 kHz. Capture stays ascending (build 15), so
`mic` still opens at 16 kHz for a 48 or 96 kHz client and KMixer
converts.

**The capture-only WDMAud fault.** With builds 14 and 15 the `mic`
profile's hot arrival faulted WDMAud before any program ran ("A fatal
exception 0E has occurred at 0028:FF0BF60C in VXD wdmaud(07) +
00002F3C", `shots\c8mic-plug.png`, `c8bmic-plug.png`, `d9mic-plug.png`):
3 of 3 legs. Build 11 from its own snapshot passed 3 of 3 (`b7mic`,
`e7mic`, and build 10's `mic4`/`mic5`), and build 14 passed under NUSB
with the device present at boot. Build 15 (capture ranges ascending
again, as in build 11) still faulted, so the range order is not the
cause. The only difference the device saw before the fault was the
second RANGE walk per format that build 12 added (`ev\x98-c8mic.jsonl`
against `x98-b7mic.jsonl`: four RANGE pairs, then nothing, against two
pairs and the stream). Build 16 makes that walk only for stepped ranges;
`mic` passed 2 of 2 (`f10mic`, `g10mic`). The fault offset is the one
build 10 met before `eaacf4c`; what WDMAud does there was not read, so
a capture-only device on 98 SE stays the least certain layout.

Feedback loss and drift, build 10, `x4` profile (`--fb-*`,
`ev\prof-fbz.txt`, `prof-fbp.txt`, `prof-fbs.txt`):

| Simulator option | Result |
|---|---|
| `--fb-zero-every 3` (every third feedback packet zero-length) | PASS 8/8 |
| `--fb-ppm 2000` (device clock 0.2 % fast) | PASS 8/8; 28-byte packets as the driver follows the feedback |
| `--fb-stop-after 1.0` (feedback stops after 1 s) | the stream stops: 6/8 buffers, the feedback watchdog, by design; guest alive |

### Not driven through WinMM

KMixer opens the **first format** of a direction (16-bit where a
profile lists it first), so these alternates are parsed, planned and host
tested (`desctest`, `uacsimtest.py`) but no guest leg streamed them: the
24- and 32-bit playback alternates of `hs24` and `clocks`, the 24-bit
capture alternates of `hs24` and `mic`, and `multi`'s 6-channel 24-bit
interface. A client that opens the pin directly (KS) can reach them.

## 98 SE under NUSB 3.3's EHCI

Cold boot per profile with the device present (`nprof.sh`). `fs` has no
NUSB leg: a Full Speed device on QEMU's `usb-ehci` needs a companion
controller this image does not have.

| Profile | Build | Result | Record |
|---|---|---|---|
| `x4` | 14, 15, 16 | play PASS 8/8, signal | `ev\allprof-n8.txt`, `allprof-n9.txt`, `allprof-n16.txt` |
| `x4full` | 14, 15 | rec PASS, 1 kHz tone; play PASS | same |
| `hs24` | 14, 16 | play PASS (48 kHz); rec PASS, tone | `allprof-n8.txt`, `allprof-n16.txt` |
| `multi` | 14, 15 | **FAIL**: play 0/8 buffers in 12 s. The 8-channel stream 98 SE's startup sound opened at boot sent about 29000 packets in 8 s, then the simulator got no more packets and no stop (no alternate 0); no stream statistics were recorded (they are written when a stream stops) | `prof-n8multi.txt`, `prof-n9multi.txt`, `x98-n9multi.jsonl` |
| `mic` | 14, 15, 16 | rec 1/16/48k and 1/24/96k PASS, tone | `allprof-n8.txt`, `allprof-n9.txt`, `allprof-n16.txt` |
| `clocks` | 14 | play PASS; rec PASS, tone | `allprof-n8.txt` |
| `adaptive` | 14 | play PASS; rec PASS, tone | `allprof-n8.txt` |

The `multi` failure under NUSB was also seen with build 10 (the stream
stalled, `UACWAV` hung); under xHCI98 the same profile passes. Whether it
is NUSB's EHCI, QEMU's `usb-ehci` with `usb-redir`, or the driver's wait
for the stuck stream is not settled; a real EHCI machine would decide.

## The first arrival after a boot (98 SE)

98 SE's first WDM audio device arrival after a boot freezes the guest
often, whichever driver is loaded (Msgsrv32 not responding, VMM EIPs
`C000A881`, `C00036xx`). Counted under xHCI98 with build 10 from a
restored fresh-boot desktop: 8 of 12 first arrivals froze, 0 of 12 later
arrivals (`ev\f1..f12-cyc.txt`). Tonight's warm-ups: under xHCI98 3 of 8
first arrivals froze (`warm.sh` retries), under NUSB 2 of 2 hot first
arrivals; every NUSB cold boot with the device present came up (14 of
14). The README's known-issue entry and `2026-10-23-brio-null.md` (on
`working`) carry the full count and the advice (plug in before switching
on).

## XP SP3 and 7 x86 (regression, `x4full`, build 16)

| Step | XP SP3 | 7 x86 |
|---|---|---|
| Install for `MI_03` | success (unsigned prompt accepted) | success (elevated prompt, unsigned prompt accepted) |
| `UACWAV list` | `waveIn` 1, `waveOut` 1 | `waveIn` 1 (Microphone), `waveOut` 1 (Speakers) |
| rec 2/16/48k | **FAIL**: 8/8 buffers and non-zero samples, but no 1 kHz sine (0 zero crossings; the simulator streamed IF5 alt 1); 3 of 3, also mono 48 kHz (0 crossings) and 44.1 kHz (12); every capture takes about 4.2 s for 2 s of audio (playback 2.17 s) | PASS, 1 kHz tone (1999 crossings), 2 of 2 |
| play 2/16/48k | PASS 8/8 | PASS 8/8 |
| play 2/16/44.1k | PASS 8/8 | PASS 8/8 |

7's elapsed times (about 1470 ms for 2 s of audio) are the TCG tick-count
artifact of the earlier 7 legs. XP's capture content is a new finding:
the driver's capture statistics show 385632 bytes delivered in 1070 URBs
(`nt16\c16.reg`, `Uac2UsbStatsIn`), so the device stream ran. The
non-zero count is the sine's (92000 of 96000 frames, its zero samples
included) with every sample of one sign, and capture runs at about half
speed: the copy into the capture buffer or the position PortCls reads on
XP is suspect, perhaps XP's EHCI driver filling isochronous IN packets
differently from 7's; not read. A recheck (`ev\xp16r\`) gave the same.
Evidence: `nt16\`, `nt16b\`,
`ev\xp16-x4full.jsonl`, `ev\w716-x4full.jsonl`, `shots\xp16*.png`,
`shots\w716*.png`.

## XP SP3 and 7 x86 with the real Sound Blaster X4 (build 16)

The release-blocking checks of 2026-10-23, rerun on build 16: the X4
passed through with `usb-host` (the pinned "orphan" QEMU,
`launchnt.ps1 -Orphan`, `isobufs=16`), TCG, 2 vCPU; the X4's own
instance installed with `I3.BAT` as above. No input was connected, so
capture records the X4's noise floor, not a tone.

| Step | XP SP3 | 7 x86 |
|---|---|---|
| play 2/16/48k, 2 s | PASS 3 of 3 (2141-2172 ms) | PASS (1391 ms by the TCG tick count) |
| play 30 s (the soak e2 failed before) | PASS 8/8, 30016 ms | PASS 2 of 2 (8/8; 20969 and 20828 ms by the tick count) |
| rec 2/16/48k | 8/8 buffers in 4187 ms: half speed, as with the simulator | 8/8 buffers |
| unplug during a 30 s play | survives: the buffers come back, no bugcheck | - |
| replug, play 2 s | PASS | - |
| driver statistics, a 30 s stream | - | 6.19 MB submitted in 8076 URBs, 32062 good feedback packets, 121 zero-length, 136 data packets late (`0xC0020000`) of 257496 |

Verdicts: **XP's STOP 0x7E does not recur**, and **XP playback with the
real X4 now passes**: builds up to 8 failed it on zero-length feedback;
build 9's 1 s first-feedback window covers the X4's slow start. **The
7 x86 30 s soak passes** 2 of 2 under TCG: the earlier e2 failure
(build 6) does not reproduce, so it was the vehicle's pacing or a defect
builds 7 to 16 removed. REAL CHECK NEEDED (owner's bench): plug the X4
into an EHCI port of a 7 x86 PC, install `uac2wdm.inf` for `MI_03`, play
a 30 s tone and listen for gaps. Evidence: `ev\w7x4-soak1.reg`,
`w7x4-soak2.reg`, `ev\xpx4-b16.reg`, `nt16\o*.txt`, `nt16b\o*.txt`,
`shots\xpx4*.png`, `shots\w7x4*.png`.

## Summary: what passes

| Layout | 98 SE xHCI98 | 98 SE NUSB (QEMU) | XP SP3 | 7 x86 |
|---|---|---|---|---|
| X4 playback (`x4`) | PASS | PASS | PASS (simulator and real X4) | PASS (simulator and real X4, 30 s) |
| X4 with capture (`x4full`) | PASS | PASS | playback PASS; capture FAIL (no signal) | PASS |
| 2-channel 16/24/32-bit, two rate families (`hs24`) | PASS (16-bit opened) | PASS | - | - |
| 8-channel playback only (`multi`) | PASS | FAIL (stream stalls) | - | - |
| mono capture only (`mic`) | PASS from build 16 (14, 15: WDMAud fault) | PASS | - | - |
| Full Speed (`fs`) | PASS | no leg | - | - |
| selector and multiplier (`clocks`) | PASS | PASS | - | - |
| adaptive playback, synchronous capture (`adaptive`) | PASS | PASS | - | - |

## Evidence

`E:\usb-wdm-free\nusb-legs\ev\` (`allprof-*.txt`, `prof-<tag>.txt`,
`x98-<tag>.jsonl` simulator logs), `pf-<tag>\` (UACWAV output, MEDIA
exports), `shots\`; QEMU PIDs in `pids.txt`.

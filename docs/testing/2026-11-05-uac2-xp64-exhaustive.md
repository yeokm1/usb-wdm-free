# uac2 build 24 on XP x64 with real devices: VLC device swaps, plug stress, quality, rates and the mixer, 2026-11-05

A user reported that "swapping to a different UAC2 dongle needs VLC
restarted; UAC1 does not". The owner then widened the question to an
exhaustive XP x64 leg of `uac2` build 24 (`899b9f4`). The legs were run
by worker uwf-audio64 in QEMU (TCG):

- The host's real devices, passed through with QEMU `usb-host`:
  - Creative Sound Blaster X4 `041E:3278` (UAC2)
  - Creative Sound Blaster X1 `041E:3286` (UAC2)
  - Logi USB Headset `046D:0A64` (UAC1, Full Speed): the control, on
    Windows' `usbaudio`.
- The simulator `uacemu.py` with the `cx31993`, `hs24`, `x4full` and
  `x1` profiles.

The real devices were heard by nobody: playback is judged by the bytes
the device took (QEMU's usbmon capture of the passed-through device), by
the simulator's received PCM, and by WinMM's results. Raw logs,
screenshots and scripts are in `E:\xhci98-vm\p46\uwf-audio64\`
(`STATE.md` indexes them).

## Rig

- **Guest:** XP x64 SP2, an overlay of the shared q35 XP x64 disk that
  already had xHCI98 3.4.0.0 (released, `nt-x64`) and `uac2` 0.1.6.0.
  Fork QEMU `cb36497` (`usb-host`, `usb-redir`, isochronous real-time
  fix), 2 vCPUs.
- **Host devices:** bound to WinUSB by the owner. Each was attached with
  QMP `device_add usb-host,vendorid=..,productid=..,bus=xh.0,port=N,pcap=<file>`
  and removed with `device_del`, one VM owning a host device at a time.
- **uac2 build 24 x64** (`uac2wdm.sys` `688b158a...`). The kit's copy of
  the INF carries DriverVer 0.1.8.24 (see the NT6 x64 record for why). It
  was staged with `SetupCopyOEMInf`.
  - The X4 had been seen before with 0.1.6.0's INF. Its first plug here
    still installed 0.1.6.0 (`oem3.inf`), even with 0.1.8.24 staged
    (`oem6.inf`).
  - It was moved to build 24 with a test tool that calls
    `UpdateDriverForPlugAndPlayDevices` with `INSTALLFLAG_FORCE` on
    `USB\Class_01&SubClass_00&Prot_20`.
  - The X1, a new device, took 0.1.8.24 by itself.
- **VLC:** 3.0.21 win32 does not start on XP x64: "The procedure entry
  point wcscat_s could not be located in the dynamic link library
  msvcrt.dll". VLC 3.0.11 win32 runs; both SHA-256 values match
  VideoLAN's published ones.
  - In VLC 3.0.11's log, `libmmdevice_plugin.dll` and
    `libwasapi_plugin.dll` fail to load on XP (error 127). Its
    DirectSound and waveOut outputs are the ones left.
  - The file was a 3-minute 48 kHz 16-bit stereo tone, played with
    `--loop`.

## Each device alone

| device | driver | 5 s play | record | other |
|---|---|---|---|---|
| X4 (real) | uac2 0.1.8.24 | PASS, 5109 ms | 2 ch 16 and 24-bit PASS, "signal present" | stats 65 words, WorkerPriority 16, WorkerThrottles 0 |
| X1 (real) | uac2 0.1.8.24 | PASS, 5109 ms | 1 and 2 ch PASS, "signal present" | stats 65 words, WorkerPriority 16, WorkerThrottles 0 |
| Logi (real, UAC1) | usbaudio | VLC plays (about 350 KB/s through the capture) | - | Windows names it "Logi USB Headset" |

The X1 is a working "VM passthrough" device on XP x64: it installs,
plays and records.

## VLC device swaps

Each case:

1. VLC plays on the first device.
2. That device is removed (`device_del`) and the second one is attached.
3. The rate of the bytes the new device takes is read, and Windows'
   sound panel is looked at.

Rates are bytes per second of the device capture. 1.63 MB/s is the X4 or
X1 playing; about 350 KB/s is the Logi playing.

| case | output | before | new device at +50 s / +80 s | after pause/resume |
|---|---|---|---|---|
| X4 -> X1 | DirectSound | 1.63 MB/s | 0 / 0 | 0 |
| X4 -> Logi | DirectSound | 1.63 MB/s | 0 / 0 | 0 |
| Logi -> X4 | DirectSound | 352 KB/s | 0 / 0 | 0 |
| Logi -> Logi (replug) | DirectSound | 352 KB/s | 0 / 0 | 0 |
| X4 -> X4 (replug) | DirectSound | 1.63 MB/s | 0 / 0 | 0 |
| X1 -> X4 | DirectSound | 1.63 MB/s | 0 / 0 | 0 |
| X4 -> X1 | waveOut | 1.63 MB/s | 0 / 0 | 0 |
| X1 -> X4 | waveOut | 1.63 MB/s | 0 / 0 | 0 |
| X4 -> Logi | waveOut | 1.63 MB/s | 0 / 0 | 0 |
| Logi -> X4 | waveOut | 352 KB/s | 0 / 0 | 0 |
| Logi -> Logi (replug) | waveOut | 352 KB/s | 0 / 0 | 0 |
| X4 -> X4 (replug) | waveOut | 1.63 MB/s | 0 / 0 | 0 |

- Pause/resume was tried two ways: the space key, and, in a second set
  of seven cases, two clicks on VLC's play/pause button. Neither brought
  the sound back.
- In every case VLC's timeline kept running and no error was shown.

Longer runs, with no action for 6 minutes after the swap:

| case | new device every 30 s |
|---|---|
| X4 -> X1 (UAC2 -> UAC2) | 0 until +120 s, then 1.62-1.63 MB/s from +150 s on |
| X4 -> Logi (UAC2 -> UAC1), nothing else attached | 0 until +120 s, then 350 KB/s from +150 s on |
| X4 -> X1 with the Logi attached throughout | 0 for 6 minutes (Windows' default device stayed the Logi) |

- **When VLC recovers.** VLC's log shows the reason. "closing audio
  device" and "Opening DirectSound Audio Output" appear once each time
  the file starts again, and never at a device change. So VLC reaches the
  new device at the next file or loop (here, the 3-minute loop point), or
  at a restart. On XP, VLC 3.0.11 has no output module that follows a
  device change.
- **UAC1 versus UAC2.** On this system the UAC1 headset under
  `usbaudio` behaved exactly like the UAC2 devices under `uac2`, so the
  reported difference was not reproduced. Two runs did follow sooner:
  - A hand-driven X4 -> X1 run reached the X1 after pause/resume clicks
    a few minutes after the swap. Meanwhile, a USB stick and a tablet had
    been plugged in and the VLC menus opened.
  - X4 -> Logi with the X1 still attached reached the Logi within 30 s.

  Neither could be separated from the loop point or from a change of
  Windows' default device.
- **Default device.** A device attached while another sound device is
  present does not become Windows' default (Sounds and Audio Devices >
  Audio > Default device). VLC's "Default" stays on the old one.
- **Names.** Both UAC2 dongles showed the same name, "UAC2 USB audio
  (experiment)" (one DriverDesc in the INF). VLC's Audio > Audio Device
  menu listed "UAC2 USB audio (experiment)" and "UAC2 USB audio
  (experiment) 2", the second being the removed device, until VLC was
  restarted. UAC1 devices show their product names. The per-device
  product name is in build 26 (owner, 2026-11-05).

## Plug and unplug stress

Each cycle:

1. One device is attached.
2. It is removed at a random moment: idle, mid-play (2-7 s into a 10 s
   play), mid-open (0-1 s after the play line lands), or a rate change
   (a 96 kHz 24-bit play, then a 44.1 kHz one).
3. After a random 1-5 s it is attached again.
4. 25 s later a 3 s play must pass, and the guest must answer. In the
   final run a failed check was tried once more 30 s later.

Final run, 14 cycles:

| device | cycles | moments | first check (at 25 s) | second check (+30 s) |
|---|---|---|---|---|
| X4 (real) | 3 | mid-play x3 | 3 PASS | - |
| X1 (real) | 4 | mid-open x2, rate change, mid-play | 4 PASS | - |
| cx31993 (sim) | 4 | rate change, idle, mid-play x2 | 2 PASS, 2 "no UAC2 output" | 2 PASS |
| hs24 (sim) | 3 | idle x3 | 1 PASS, 2 "no UAC2 output" | 2 PASS |

- No bugcheck, no hang, and QEMU stayed up in every cycle. Every device
  came back and played.
- The simulator starts afresh with each attach, so its time to come back
  includes its own start.
- Earlier cycles, 8 before the fixes and 2 after the first fix:
  - Real devices: 5 PASS. The one fail, an X4 cycle, was a harness race:
    the X4 was attached to port 1 before the simulator's `device_del` had
    finished ("usb port 1 ... in use"). Re-run by hand (X4 removed 3 s
    into a play, 3 s gap, attached again), it played.
  - One X1 cycle (idle, 1 s gap) had "no UAC2 output" at 25 s. It ran
    before the second check existed and stays unexplained. The X1 passed
    its other 7 cycles.
  - Simulator cycles (4) failed at the first plug. The reason was a
    Found New Hardware wizard for the X4's driverless function, left open
    behind a window. XP installs one new device at a time, so the
    simulators' new IDs were queued behind it. In addition, the harness
    reused the simulator's log name on the next attach, so the replugged
    simulator never started (uacemu refuses an existing log).
  - With the wizard answered and the harness fixed, the simulators
    installed and played. A hand replug of the cx31993 with a 30 s wait
    played.
- UACWAV prints "PASS: all buffers played" for a play whose device was
  removed part-way: its buffers complete. So a pass on a cut play is not
  evidence that it played.

## Quality, rates and the mixer (simulators, received PCM)

For each condition, a fresh simulator with `--pcm`. `UACWAV vol` or
`mute` (WinMM mixer, "Master Volume" and "Master Mute"), then
`UACWAV play` (a 440 Hz sine at 0.1 of full scale, -20 dBFS). The
capture was read per 1 s window: level, THD (harmonics 2-9), THD+N.
Each capture was also fitted to a sine per 10 ms.

| profile | conditions (all PASS) | received | level | THD (median window) | full-scale samples | FU SET_CUR received |
|---|---|---|---|---|---|---|
| cx31993 | vol 100 / 50 / 10 %, mute, 44.1 kHz 16-bit, 48 kHz 24-bit, 96 kHz 24-bit | 2 ch, 16-bit, 48 kHz | -20.0 dBFS | -82 dB (16-bit clients), -99 dB (24-bit clients) | 0 | 2-4 when the value changes |
| hs24 | same | 2 ch, 16-bit, 48 kHz | -20.0 dBFS | -82 / -99 to -100 dB | 0 | 1-3 |
| x4full | same | 2 ch, 16-bit, 48 kHz | -20.0 dBFS | -82 / -99.8 dB | 0 | 1-4 |
| x1 | same | 2 ch, 16-bit, 48 kHz | -20.0 dBFS | -82 / -98 to -99 dB | 0 | 1-4 |

- **No distortion measured.** In each capture every 10 ms block after
  the first second is within -40 dB of a pure sine. The first 0.86-1.13 s
  of each capture holds a different sound, about 200 Hz, -13 to
  -15 dBFS, before the test tone. That is most likely Windows' device
  arrival sound, since a new device becomes the default when it is the
  only one. It is the "worst window" and the peak in the per-window
  figures.
- **Volume and mute are the device's.** The received samples were the
  same at 100 %, 50 % and 10 % and with mute on. `uac2` sent the feature
  unit `SET_CUR` requests (class, interface) instead of scaling samples.
  Setting 32768 read back 32845 on the simulators and 31401 on the real
  X4: the device's steps.
- **Formats are converted by Windows.** Every client format reached the
  device as 2 channels, 16-bit, 48 kHz on XP x64. Windows' mixer
  converted 44.1 and 96 kHz and 24-bit clients before `uac2`, as the
  NT6 x86 record found for its systems.

## Real devices: rates, the mixer and recording

Per device: an `XHCITR64 /snapshot` before and after, then these
WinMM operations:

- plays: 16-bit at 44.1, 48 and 96 kHz; 24-bit at 44.1, 48 and 96 kHz;
- the mixer: volume 32768, a play, mute on, a play, mute off, volume
  65535, then `UACWAV mix`;
- records: 16 and 24-bit, 48 kHz.

| device | plays (6) | mixer | record | Ring Underrun during the set |
|---|---|---|---|---|
| X4 | all PASS, 5094-5109 ms | volume set 32768 read 31401, mute 1 and 0 read back, mix PASS | 2 ch 16-bit and 24-bit PASS, signal present | +16 (12169 -> 12185) |
| X1 | all PASS, 5094-5125 ms | volume set 32768 read 32786, mute read back, mix PASS | 1 ch 16-bit and 24-bit PASS, signal present | +19 (12186 -> 12205) |

- **Ring Underrun.** The before/after counts span about 15 stream starts
  and stops each, with no load. The counter's lifetime total (12169)
  holds the earlier swap and stress runs.
- **Mute was already on.** The X1 reported mute on (1) before the test
  set it; the cause was not looked into.

## Not covered

- Real devices at their own other rates: WinMM clients on XP are
  converted to 48 kHz.
- Listening.
- Recording quality analysis.
- The distortion investigation of the CX31993 with a MAX97220, which
  another worker has with its own profile.

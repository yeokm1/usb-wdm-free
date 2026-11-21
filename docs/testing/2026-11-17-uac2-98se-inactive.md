# uac2's failed state on Windows 98 SE: the changed binaries with the feature inactive (2026-11-17)

The failed state of docs/design/uac2.md 29.9 and 29.17 is active only on
Windows 2000 and later (WDM 1.10 and above) and only when
`Uac2FailOnTimeout` reads 1; on 98 SE and ME it must stay inactive and the
driver must behave as before. These are the development legs of the
changed `uac2` x86 binary on 98 SE with that setting absent, 0 and 1,
under NUSB 3.3's EHCI and under xHCI98, and one leg of `uac1`, which is
built from the same shared sources: under NUSB's EHCI only, with the
setting absent only. `uac1` did not run under xHCI98 or with the setting
0 or 1 (below). Every device here is a simulated one (`uacemu.py`,
`uac1emu.py`); no physical device was used, and nothing here says how a
real adapter behaves. No README "Tested by the author" cell changes.

## What ran

- **Binaries**, built at `1fa677e` (tree clean) with the packages'
  `build.cmd` (`wdmx86`), SHA-256:
  - `uac2wdm.sys` free
    `89D6EE92A0024788780A5A862E0FAB03B78FA39C59AD2B398AC51B450ADD13AA`;
    `uac2wdm.inf`
    `7FE359AC89183D33ED2D6DFD6C64D824B3BF67237B60F48A08B3C2D6E2E66735`
    (byte-identical to the released one);
  - `uac1wdm.sys` free
    `78D92E35C77356E71A822635E26732B5CD019913D1C1167DE5A70DD6C2A43C57`;
    `uac1wdm.inf`
    `E069FDB4C6231E323CC8AD71E0581312DA3BE5220864C4A2ED0986A270177C41`;
  - `uac2wdm.sys` trace
    `04F085D4F2231FBE4C818E391E0F5880F59A51237050A752DE0A793675952FAD`:
    built, not run (below).
  - Control: the released `free/audio/uac2/w98se-me/uac2wdm.sys`
    `934db4cf...38bb` (`SHA256.TXT`).
- **Emulator:** QEMU 11.1.0, TCG, `pc`, `-cpu pentium3,-apic`, one
  processor, 256 MB, headless; the 98 SE CD attached; the files on a
  `vvfat` floppy. The host was heavily loaded.
- **Bases** (read-only, a fresh overlay per guest):
  - NUSB: `w98-nusb-hub` (98 SE with NUSB 3.3 and 98 SE's `USBHUB.SYS`;
    xHCI98 3.1.0.0 underneath). `qemu-xhci` and `usb-ehci`; the device on
    `usb-ehci`. In this PCI layout each new overlay first raises the
    wizard for the EHCI controller; it was answered (search) and installed
    NUSB's `USB2.INF`, after which `USB\ROOT_HUB20` was present.
  - xHCI98: `w98-hcd310` (98 SE with xHCI98 3.1.0.0). `qemu-xhci`; the
    device on it.
- **Install:** the driver folder copied to `C:\U2` (`C:\U1` for `uac1`)
  and `DevicePath` set to `C:\WINDOWS\INF;C:\U2`, then a restart and the
  plug. 98 SE chose the INF by itself and asked for the disk; "Copy files
  from" was answered `C:\U2`. On the control guest, staged without the
  restart, the wizard offered 98 SE's own `WDMA_USB.INF`; "Display a
  list", Have Disk `C:\U2` installed `uac2` instead.
- **Setting:** `Uac2FailOnTimeout` written into the device's driver key
  `HKLM\System\CurrentControlSet\Services\Class\MEDIA\0000` as four bytes,
  the form the INF gives its other values on 9x (`hex:00,00,00,00` and
  `hex:01,00,00,00`), with `REGEDIT /S`, and read back with `REGEDIT /E`
  each time. A start reads it only after a new start (a replug under
  xHCI98, a restart under NUSB).
- **Devices:** `uacemu.py --profile hs24` (High Speed, playback and
  capture, explicit feedback, a feature unit) and `uac1emu.py --profile hs`
  (High Speed UAC 1.0, playback and capture); the forced stop
  `--stop-after-s 6` (no request answered from 6 s after the first
  isochronous stream). Over `usb-redir`.
- **Program:** a scratch test program (not tracked): a 10 s play of a
  tone at 48 kHz, 16-bit stereo, reading `waveOutGetPosition` each second;
  an 8 s record reading `waveInGetPosition` and counting returned buffers;
  three timed open/close pairs; a 5 s play; each call timed. Then the
  driver key exported.

## What the code says (read, not run)

`Start` sets the gate from `IoIsWdmVersionAvailable(1, 0x10) &&
GetDwordRaw(fdo, L"Uac2FailOnTimeout") == 1`; on 98 SE the first test is
false, so the value is never read, and the notifier thread is started only
when the gate is active. The trace flavour's port 0xE9 trace has no line
for the gate or the notifier (the stream events only), so a trace log
could not show their absence; the trace binary was built and not run. On
98 SE the notifier's absence is read from the code and from what it would
have done (write `Uac2Failed`), which no leg saw.

## What was seen

"Positions advanced" means every one-second reading grew; "0" a call that
returned `MMSYSERR_NOERROR`.

| Leg | Stack | Setting | Seen | Verdict |
|---|---|---|---|---|
| 1 | NUSB | absent | install as above; start: `Uac2M0Stage` 0x1E, status 0, tray volume icon; 12 s play: open 0, position 19200 to 1977600 bytes (about 192 kB/s), 109 buffers, reset and close 0; 10 s record: position advanced, 99 buffers, reset and close 0; three opens 0 (119, 24, 8 ms); a 6 s play again 0, position advanced. Simulator: 96840 packets of 24 bytes over 12.26 s for the first play. No `Uac2Failed`, no `Uac2FailOnTimeout`. **Unplug (idle): the guest hung** (below) | play, record, stop, reopen PASS; unplug FAIL, also with the released binary |
| 1, again | NUSB | absent | cold start, plug, start (icon); unplug at once, nothing played: the guest hung again | the same |
| 2 (`A2`) | NUSB | absent | a fresh guest (`n2`) whose `uac2` was installed from the release, then the branch's `uac2wdm.sys` copied over it before a restart; plug, start; 10 s play: position to 1768704 at second 9, 100 buffers, reset 1005 ms, close 0; 8 s record: 79 buffers, 0; three opens 0; 5 s play 0. No `Uac2Failed`. Shut down with the device attached: clean | PASS |
| 2 (`Z2`) | NUSB | 0 | after a restart: play to 1622784 at second 9, 93 buffers, reset 571 ms; record 79 buffers; opens 0 (20, 8, 7 ms); play 0. `Uac2FailOnTimeout` 00000000, no `Uac2Failed`. Shut down with the device attached: clean | PASS |
| 2 (`O2`) | NUSB | 1 | first plug: arrival wedge (below); after a reset: play to 1595904 at second 9, 92 buffers, reset 1536 ms, close 0; record 79 buffers, 0; opens 0 (36, 11, 7 ms); play 0. `Uac2FailOnTimeout` 01000000, **no `Uac2Failed`**. Shut down with the device attached: clean | arrival FAIL (the first plug wedged); after the reset, the second plug's start, play, record, stop, reopen and the shutdown with the device attached PASS |
| 3 | NUSB | 1 | `--stop-after-s 6`; three hot plugs wedged at arrival, so the device was attached at boot. 98 SE's startup sound was the first stream; at its stop, `SET_INTERFACE(1, 0)` was the first request unanswered. Then the 14 s play: open 0 in 175 ms, the first writes took 5.2 s, the position stayed 0 every second (no stream ran); a volume SET through the mixer API returned 0 after 25138 ms. The simulator logged a clock RANGE request and the feature unit's volume GET_CUR and SET_CUR unanswered, one every about 5 s, 15 requests in 75 s: requests went on reaching the stopped device. Three opens then returned 0 in 8 to 20 ms and each close took 15.4 s (no request reached the simulator meanwhile). `Uac2M0Stage` 0x1F, `Uac2M0Status` `0xC00000BB`, **no `Uac2Failed`**; no immediate refusal anywhere. Shut down with the stopped device attached: clean | arrival FAIL (three hot plugs wedged); with the device attached at boot, the start, the behaviour before section 29 (requests kept reaching the stopped device, no failed state, no record) and the shutdown PASS |
| 4 (`X1`) | xHCI98 | absent | install as above (`MatchingDeviceId` `USB\Class_01&SubClass_00&Prot_20`); 10 s play: position advanced, 465792 at second 9 (about a quarter of real time: this rig's pace under xHCI98), 26 buffers, close 0; record advanced, 23 buffers, 0; opens 0; play 0. **Unplug (idle): the guest carried on**, the volume icon went; replug: start; the same steps again 0, positions advanced | PASS |
| 4 (`X0`) | xHCI98 | 0 | unplug, replug: play to 425856 at second 9, record 21 buffers, opens 0, play 0; `Uac2FailOnTimeout` 00000000, no `Uac2Failed` | PASS |
| 4 (`X2`) | xHCI98 | 1 | unplug, replug: play to 442752 at second 9, record 21 buffers, opens 0 (22, 10, 9 ms), play 0; `Uac2FailOnTimeout` 01000000, **no `Uac2Failed`**. Shut down with the device attached: clean | PASS |
| 5 | NUSB | absent | `uac1`, `uac1emu.py --profile hs`: "USB Composite Device" installed from 98 SE's `USB.INF`, then the function chose `uac1wdm.inf` and asked for the disk (`C:\U1`); "USB Audio 1.0 (wdm-free)" started (`Uac2M0Stage` 0x1F, status 0). 10 s play: open 0, position advanced, close 0; 8 s record advanced, 0; opens 0; 5 s play 0. No `Uac2Failed`. Unplug: the guest carried on, the icon went. Shutdown: hung at "Windows is shutting down" (below) | play, record, stop, reopen, unplug PASS; shutdown FAIL; attribution unresolved (below) |

## What else was seen

- **Unplug under NUSB hangs the guest, with the released `uac2` too.**
  Under NUSB's EHCI an idle unplug of the `hs24` device hung 98 SE three
  times out of three: twice with the branch's binary (once after plays,
  once with nothing played since the start) and once with the released
  `uac2wdm.sys` `934db4cf...` on a fresh guest. The volume icon stayed,
  the clock stopped and the Start menu no longer opened. The same
  symptom reproduced with the released binary; its cause and any branch
  effect on its frequency remain undetermined. It was not looked into
  further. Under xHCI98 the
  same unplugs passed (three of three), and under NUSB the `uac1`
  device (split by 98 SE's composite parent) unplugged cleanly. Because of
  it, "unplug, replug and play" under NUSB was run as a restart and a new
  plug.
- **Arrival wedges** (98 SE's first-plug wedge, task 36.41): of the hot
  plugs of the branch's binary under NUSB after a cold start, nine
  in all, four wedged
  at arrival (the simulator saw the same configuration and control
  requests as in the starts that came up); the released binary wedged in
  three of four. A device attached at boot came up. Counts this small
  do not tell whether either binary wedges more often than the other.
- **Shutdown hangs** on the `uac1` guest: twice at "Windows is shutting
  down", the first in a session where nothing of this repository had been
  loaded or plugged (only NUSB's EHCI driver just installed), the second
  after `uac1` had run and its device had been unplugged. Verdict for
  `uac1`'s shutdown: FAIL; attribution unresolved. The hang in the
  unloaded session shows that this guest's shutdown can hang without
  anything of this repository; it does not clear the second hang. All
  nine shutdowns of the `uac2` guests completed, four of them with a
  working device attached and one with the stopped device.
- **`uac1` played faster than real time** in this rig: 160 buffers in a
  10.8 s play, 186 in a 5 s play, by the guest's own clock; the simulator
  received 48-frame packets with no underrun, overrun or misframing. No
  control run with the released `uac1` was made, and High Speed UAC 1.0
  under NUSB had not run before, so this is recorded and not judged.

## What did not run, and why

- `uac1` under xHCI98: not run. `uac1` ran leg 5 only (NUSB's EHCI,
  setting absent).
- `uac1` with the settings 0 and 1, and `uac1` with the forced stop: not
  run, for time.
- Leg 3 (the forced stop) under xHCI98: not run, for time.
- A replug and play under NUSB: the unplug hangs the guest (above).
- The trace build: its trace cannot show the gate or the notifier (above).
- Windows ME: not run.
- `Uac2FailOnTimeout` was not deleted after being written; "absent" was
  run before it was first written on each guest.

## Evidence

`E:\usb-wdm-free\uacwedge\w5\` (development host): `STATE.md` (every
step, time and process id), `stage\` (the program logs `*.LOG`, the
exported driver keys `*M.REG`), `logs\` (the simulators' logs), `shots\`,
and `..\pids.txt`.

# uac2's failed state, default off: development legs on Windows 7 x86 with the simulator (2026-11-17)

Development legs of the integrated failed state (docs/design/uac2.md 29.9
and 29.17) as revised the same day. Every device here is
`uacemu.py`'s simulated device; no physical device was used, and nothing
here says how a real adapter behaves. One system ran: Windows 7 SP1 x86
(build 7601) under QEMU (TCG, one processor, headless), Windows' own EHCI
driver. No leg ran under xHCI98, on 98 SE, ME, 2000, XP, Vista or x64
(below). No README "Tested by the author" cell changes; the feature is
default off and not complete (29.17).

## What ran

- **Binary.** `uac2wdm.sys`, `wdmx86` free, built from commit `e8515e4`
  (tree clean), SHA-256
  `0E1713D165C65E71B0EBCB2AFE498D679D8F879DA481AD1D72BEC9E5C616BC19`,
  copied over the installed `uac2wdm.sys` of a guest that already had
  `uac2` installed for the simulated device's VID/PID.
- **Setting.** `Uac2FailOnTimeout` written as `REG_DWORD` 1 in each `uac2`
  driver key of the guest (legs 1 to 9), then 0 (leg 10).
- **Device.** `uacemu.py` (as of `7acc067`) `--profile hs24 --vid 3302
  --pid 336a`: High Speed, one programmable clock, playback and capture
  (so duplex), asynchronous playback with feedback, a feature unit with
  volume and mute. The VID/PID made Windows reuse the installed instance
  (`usb-redir` on the EHCI's port 5 each time). Its forced stop:
  `--stop-after-s S` (no control request answered from S seconds after
  the first isochronous stream) or `--stop-at-request N`.
- **Programs.** A scratch test program (not tracked) that plays a quiet
  tone through `waveOut`, records through `waveIn`, opens and closes
  repeatedly, and logs positions and each call's time; the Sound panel's
  volume mixer (`sndvol`) for volume SETs that reach the driver (on 7 the
  legacy mixer API sets the program's session volume and sends nothing).
- **Read.** The simulator's log (every control request, and
  `unanswered` for each one after the stop), the program's log, `reg
  query` of `Uac2Failed`, `Uac2M0Stage` and `Uac2M0Status`, and `wmic`'s
  `ConfigManagerErrorCode`.

`Uac2Failed` below lists the words in order: Version, Count, TimeLow,
TimeHigh, Kind, Setup0, Setup1, Status, UrbStatus, LateMs,
MsSinceAltChange, AltChanges10s, Streams, Build. TimeLow, TimeHigh and the
three after LateMs read `0xFFFFFFFF` in every record (not recorded in this
step); Version 1 and Build 29 in every record.

## What was seen

| Leg | Case | Seen |
|---|---|---|
| 0 | healthy, setting 1 | playback 12 s and capture 10 s: both positions advanced; a volume change in `sndvol` sent SET_CUR (feature unit 2, volume) |
| 1 | playback running; failure by a volume SET (`--stop-after-s 10`) | the SET_CUR was the only request after the stop; the isochronous OUT stream went on about 6.3 s after it reached the simulator, then ended; the program's `waveOutWrite` returned 6 (`MMSYSERR_NODRIVER`) and its position calls failed from then on; opens afterwards returned 2 (`MMSYSERR_BADDEVICEID`) in 0 ms; Code 43. A second volume SET pressed a second later sent nothing. Record: Count 2, Kind 1, Setup0 `0x02000121`, Setup1 `0x00020200`, Status `0xC0000120`, UrbStatus `0xC0010000`, LateMs 3 |
| 2 | capture running; failure by a playback open (`--stop-after-s 8`); also the device's START after leg 1's failure and unplug | the capture ran and its position advanced (a new START after the failed device was unplugged); the playback open's clock RANGE request was the only request after the stop; that open returned 1 (`MMSYSERR_ERROR`) after 5928 ms, the next in 16 ms (2); the capture position failed from then on and its buffers stopped returning; its reset and close returned at once; Code 43. Record: Count 3, Kind 1, Setup0 `0x010002A1`, Setup1 `0x00021000`, Status `0xC0000120`, UrbStatus `0xC0010000`, LateMs 1 |
| 3 | duplex running; failure by a volume SET (`--stop-after-s 8`) | both positions advanced until the failure; the SET_CUR was the only request after the stop; the OUT stream went on about 6.5 s after it, then ended; both positions failed and `waveOutWrite` returned 6; resets and closes returned 0 at once; a second SET sent nothing; Code 43. Record: Count 4, Kind 1, Setup0 `0x02000121`, Setup1 `0x00020200`, Status `0xC0000120`, UrbStatus `0xC0010000`, LateMs 2 |
| 5 | SET_INTERFACE as the expired request (`--stop-after-s 6`) | not arranged: the stream's stop sent SET_INTERFACE(1, 0), the first request after the stop, unanswered; the program's close took 5444 ms, and no failure was recorded for it: Windows' stack completed the request before `uac2`'s 5 s deadline (as in the earlier leg of the day). The next request, a later open's clock RANGE, expired (open 1 after 5257 ms, then 2 in 16 ms); Code 43. Record: Count 5, Kind 1, Setup0 `0x010002A1`, Setup1 `0x00021000`, Status `0xC0000120`, UrbStatus `0xC0010000`, LateMs 1 |
| 7 | a failed START with the notifier running (`--stop-at-request 15`) | request 15, the string descriptor 0 read of `uac2`'s START (`ReadName`), was the only request after the stop; Code 10, the device named by the INF ("USB Audio 2.0 (wdm-free)"); `Uac2M0Stage` 13, `Uac2M0Status` `0xC00000B5`. Record (written while the device never started): Count 6, Kind 2, Setup0 `0x03000680`, Setup1 `0x00400000`, Status `0xC0000120`, UrbStatus `0xC0010000`, LateMs 0 |
| 6 | unplug while a request is outstanding (`--stop-after-s 1`) | a capture open's clock RANGE request unanswered; the device unplugged (QMP `device_del`) about 2 s later; the open returned 1 after 5179 ms; no failure recorded (the record kept Count 6): the request ended through the removal before the deadline. The devnode went away; the guest carried on. (A first attempt, leg 6a, became a plain unplug during playback with nothing outstanding: the guest carried on.) |
| 1 to 3 | unplug of a failed device | each time the devnode went away and the guest carried on; the next plug started healthy |
| 8 | power-down with a failed device present (`--stop-after-s 1`) | Code 43 with the simulated device still attached; QMP `system_powerdown`: QEMU exited about 10 s later. Record (read after the reboot): Count 7, Kind 1, Setup0 `0x010002A1`, Setup1 `0x00021000`, Status `0xC0000120`, UrbStatus `0xC0010000`, LateMs 1 |
| 9 | after the reboot, healthy | two streams (the arrival sound, and an 8 s play), non-zero data |
| 10 | setting 0 (inactive) with the forced stop (`--stop-after-s 1`) | three opens each returned 1 after about 5.1 s; every open's request went out to the stopped device (four unanswered requests); no Code 43 (0); `Uac2Failed` unchanged: the behaviour before section 29 |

In legs 1 to 3, 5 and 8 the Status word is the lower stack's own
completion status, `STATUS_CANCELLED`, and UrbStatus
`USBD_STATUS_CANCELED`: the request expired, was cancelled, and the stack
completed it as cancelled a few milliseconds later. In each failing leg
the expired request was the last request the simulated device saw.

## What did not run, and why

- **Contending opens around the failure** were partly run: a second
  volume SET (legs 1, 3) and opens after the failure (legs 1, 2, 5). An
  open or a RUN waiting on the Wave mutex while the expired request held
  it was not arranged: the capture open meant for it in leg 1 went to the
  wrong window. On 7 a program's playback open while the engine's pin
  runs sends nothing to the driver.
- **A PnP stop** (IRP_MN_STOP_DEVICE) was not arranged; unplug and replug
  and a reboot were the restarts run.
- **Sleep and resume** (S3) were not run: no sleep was tried on this
  rig's machine options; a power-down and boot were run instead (legs 8,
  9).
- **An expired request outstanding at an unplug or a power-down**: the
  cancellation of an expired request completed within milliseconds under
  this stack, so no leg held one outstanding across a teardown; leg 6 held
  an unexpired one.
- **xHCI98** as host controller driver: not run.
- **98 SE and ME** (the inactive regression with this binary): not run.
  No guest of the rig had `uac2` installed on 98 SE or ME; the bases with
  NUSB's EHCI and with xHCI98 exist without it, and installing it there
  through the hardware wizard was not attempted in this session. 98 SE
  ran later the same day:
  [uac2-98se-inactive](2026-11-17-uac2-98se-inactive.md).
- **Windows 2000 and XP** (inactive smoke, active failure and unplug):
  not run; the installed bases have no `uac2`, and its install there was
  not attempted in this session.
- **x64** (Windows 7 x64): not run; the base has no `uac2` installed.
- **Vista**: not run.

## After the merge with main (2026-11-17)

The branch was merged with main at `63ea5ee` (main's playback level
engine as its own module, `uaclevel.c`, and the capture-graph and route
modules with their host tests); the merged result is `be9b461`. It has
been built and host-tested only. Built: `uac2` `wdmx86` and `wdm64`,
each free, checked and trace, and `uac1` `wdmx86` free and checked, each
with the default level engine and with `UAC_LEVEL=retained`, every build
passing its import gate. Host-tested: both packages' `test.cmd`,
`streamtest.cmd`, `testgate.ps1` (21 cases), `uacsimtest.py` (34) and
`uac1simtest.py` (51), every one passing. No merged binary has run in a
guest or on any system.

No leg above, and none of
[uac2-98se-inactive](2026-11-17-uac2-98se-inactive.md), carries over to
the merged binaries. Before they are relied on, these are to be run on
them, each recorded with its result, a failure or a leg not run named as
such:

- 98 SE under NUSB's EHCI and under xHCI98, `uac2` with the default
  engine: level and mute SET, playback and capture, stop and reopen,
  unplug and replug, shutdown; the setting absent, 0 and 1.
- `uac1` on 98 SE: the legs it has not had, under xHCI98 and with the
  setting 0 and 1, besides its legs under NUSB's EHCI.
- Windows 7: the failure legs (1 to 3, 5, 7, 8), the startup latch, the
  immediate refusal after a failure, and the inactive setting (10).
- A level SET waiting behind an operation whose request expires,
  including a SET queued to the level work item and the teardown that
  waits for it.
- The retained build (`UAC_LEVEL=retained`), if it is to be used: its
  level SET and playback legs as well.

Run on main's binaries at `65dd131` (this branch's merge with stage 1 of
the capture wiring) on 2026-11-17 and 18, with what did not run or did
not pass: [uac-merged-legs](2026-11-18-uac-merged-legs.md).

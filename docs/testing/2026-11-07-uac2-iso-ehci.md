# uac2: playback under Windows' own EHCI and UHCI, and Full Speed on 2000's UHCI, 2026-11-07

The legs of [uac2.md](../design/uac2.md) section 25. Playback of the
asynchronous profiles (explicit feedback) stopped under Windows' own
EHCI and UHCI drivers on XP and 7 while the same build played under
xHCI98, and a Full Speed device under 2000's own UHCI was never
configured. Development legs in QEMU (TCG) with the simulator, not a
release and not "Tested".

## Verdict

Three `uac2` defects, each found with a trace build and fixed on branch
`w47-u2iso` (from `w47-rel019` `0699ecc`):

1. **A URB the stack failed whole ended the stream.** When the host
   controller falls behind its schedule, Windows' stack completes the
   URBs whose frames passed with every packet failed
   (`USBD_STATUS_ISO_NOT_ACCESSED_BY_HW` per packet on EHCI,
   `USBD_STATUS_NOT_ACCESSED` on UHCI) and the URB with
   `USBD_STATUS_ISOCH_REQUEST_FAILED` (IRP `STATUS_UNSUCCESSFUL`); the
   URBs after it are scheduled and complete. `uac2` treated it as fatal
   and stopped the stream for good. QEMU's EHCI and UHCI skip frames
   under load (stock 11.1.0 often, the usb4-emu fork `cb36497` less),
   which is how the legs met it; `usb.h` defines the same completion
   for any stack whose packets all failed, late ones included.
2. **The feedback deadline counted wall time.** On 7 under Windows'
   EHCI the stack held every queued URB, feedback and data alike, for
   0.6 to 1.2 s at a time and then completed them; the 200 ms deadline
   ended the stream inside such a hold though no unusable feedback had
   come back.
3. **2000's USB 1.1 stack answers no USBDI interface.** On 2000 a
   device under the UHCI controller's hub driver (`usbhub`; EHCI devices
   are under `usbhub20`) gets no answer to the bus interface query
   (`STATUS_NOT_SUPPORTED`, read with the trace build), and `uac2`
   refused an unknown speed on 2000, so it never selected a
   configuration.

What the legs also show is the emulator's part: the stock QEMU stops
were more frequent than the fork's, and on 7 under Windows' EHCI a
`multi` play (8 channels at High Speed) still took about 14 s for 4 s
of audio on the fork because the stack held the queue most of the time;
`uac2` kept every URB submitted and did not stop. That pace is the
rig's, not settled further here.

## Rig

- **Builds:** branch `w47-u2iso`: `cb354c0` (the trace flavour,
  `build.cmd wdmx86 trace`, test-only), `40e72fc` (fixes 1 and 3),
  `92e04f1` (fix 2), `c72f09c` (the design section renumbered to 25,
  comments only; the code of `92e04f1`). Trace legs ran the trace
  flavour of the commit current at the time (below); the release legs
  ran `build.cmd wdmx86 free` of `92e04f1`, `uac2wdm.sys` SHA-256
  `8a982f6b8781a41a...` (0.1.10.0; the version is not raised here).
  Control: the release folders of `w47-rel019` (`uac2wdm.sys`
  `8e2b8729d66a78af...`).
- **Host tests:** `test.cmd` (desctest 10015 assertions, watchtest
  500099 checks, topology, volume, INF checks), `rtest.cmd` (98579),
  `streamtest.cmd`, `playtest.cmd` pass. `uac1` (branch `w47-uac1`,
  which moves the shared transport into `src/audio/common`): with this
  branch merged in a scratch tree (one conflict in `UacUsbOpen`, the
  speed lines inside `#else`), `uac1` builds (`wdmx86 free`) and its
  `test.cmd` (1144 checks) passes; `uac2` builds and its tests pass
  there too. Nothing under `src/audio/common` changes on this branch.
- **x64:** `build.cmd wdm64 free` of `c72f09c` builds
  (`4df1c4b4515f0d51...`); unsigned, not run. `uac2wdm.sys` x64 changes
  with this branch (`uacusb.c`, `uacplan.c`, `uac2wdm.cpp`,
  `uacwatch.h`), so it needs the owner's signature again.
- **Simulator:** `uacemu.py` of this branch (`ff8e0f50c8ce7c3c...`),
  profiles `multi` (High Speed, 8 channels 16-bit, feedback),
  `x4full`, `hs24`, `fs` (Full Speed, 3-byte feedback).
- **Emulators:** stock QEMU 11.1.0 (`usb-ehci`, `piix3-usb-uhci`,
  `usb-redir`) and the usb4-emu fork `cb36497` (`3e7f6bf5b98fde98...`),
  q35 overlays of the NT matrix's frozen bases (XP SP3, 7 x86, 2000;
  `-win` without xHCI98, `-xh` with it), one vCPU. `UACWAV.EXE`
  `0121d2cd0dc0b44d...`: `play <ch> 16 48000 4`, `rec <ch> 16 48000 3`.
  Each leg set: install, plug, list, play, record; the first profile
  also unplugged and replugged, and at the end plugged across a
  restart. 2000: the device installed by hand
  (UpdateDriverForPlugAndPlayDevices with the release INF), then a cold
  start, as the INF's `Reboot` asks.

## Trace legs (development)

| Leg set | System, stack, QEMU | Build | Result |
|---|---|---|---|
| `11071753` | XP, Windows' EHCI/UHCI, stock | trace, no fix | `multi` 8/8, replug 8/8, restart 7/8; `fs` 0/8: a whole-URB failure (`C0000B00`, every packet `C000000F`) 30 ms in, the stream stopped; the alternate stayed 1 until the unplug |
| `11071812` | XP, stock | trace + fix 1 | `multi` 8/8 x3 (4 URBs lost whole and passed over at one point); `fs` 1/8 (the stream ran 360 ms with 8 lost URBs passed over, then the client paused it; not seen again) |
| `11071828` | XP, fork | trace + fix 1 | `multi` 8/8 x3; `fs` 8/8 with 54 URBs lost whole and passed over (each would have ended the stream before) |
| `11071845` | XP, stock | trace + fix 1 | `fs` 8/8, replug 8/8, restart 0/8: the feedback deadline (unusable feedback on stock UHCI) |
| `11071908` | 2000, its UHCI, stock | trace + fixes 1, 3 | bus interface query `C00000BB`, the `usbhub` rule took Full Speed, configured; `fs` play 8/8, record 8/8, replug and restart PASS |
| `11071900` | 7, Windows' EHCI, fork | trace + fix 1 | `x4full` 0/8 and replug 1/8: the feedback deadline |
| `11071911` | 7, fork | trace + fix 1 | `x4full` 0/8: every URB with the stack for 180 ms, none back, then the deadline |
| `11071935` | 7, fork | trace + fixes 1, 2 (first form) | `x4full` 8/8, replug, restart PASS; `multi` 2/8: no failure, the stack held all 10 URBs for 11 of 16 s |
| `11072013` | 7, fork | trace of `c72f09c` | `multi` 8/8 in 14.1 s, no failure (the stack held the queue in 91 periods of 100 ms); replug and restart PASS |
| `11072130` | 2000, its UHCI, stock | trace of `c72f09c` | `fs` play 8/8 in 4.6 s, record 8/8 in 3.4 s; no failure, no held queue |

## Release legs (`92e04f1` free)

| Leg set | System, stack, QEMU | Profiles | Result |
|---|---|---|---|
| `11071953` | XP, Windows' EHCI/UHCI, fork | `multi`, `fs` | `multi` play, replug, restart PASS; `fs` play and record PASS |
| `11071956` | 7, Windows' EHCI, fork | `x4full`, `multi` | `x4full` play, record, replug, restart PASS; `multi` 1/8 (not traced; the trace leg above passed in 14 s) |
| `11072148` | 2000, its UHCI, stock, alone | `fs` (as above, `bcb748a5...`, the same code built at `c72f09c`) | play 8/8 in 4.0 s, record 8/8 in 3.1 s; replug: play 8/8 in 4.1 s, record 8/8 in 3.1 s |
| `11072010` | XP, xHCI98 (no-regression), stock | `x4full`, `hs24`, `fs` | every play and record PASS, `x4full` replug and restart PASS (plays take about 12 s for 4 s on stock QEMU, as in the matrix's stock legs) |
| `11072032` | 2000, its UHCI, stock | `fs` (installed by hand, cold start) | first plug: play 6/8 and record 2/8 in 14 s; replug: play 8/8 in 8.2 s, record FAIL; after the restart: play 8/8 in 4.0 s and record 8/8 in 3.1 s |
| `11072110` | 2000, its UHCI, stock, alone | `fs` | first plug: play 6/8, record 3/8 in 14 s (stopped there) |
| `uac2-w98-xh-u2` | 98 SE, xHCI98 (no-regression), the usb4-emu fork | `hs24`, `fs` | `hs24`: play, record, unplug while playing, replug play and restart play PASS (the shutdown before the restart hung, as in many 98 SE legs of the matrix, every driver); `fs`: installed, then after the cold start the guest's clock stopped before its desktop answered (the 98 SE stop of [uac2-fs-feedback](2026-11-07-uac2-fs-feedback.md), "Open"); not run further |

On 2000 the release build was slow at the first plug in two of three
sets (`11072032`, `11072110`): play and record alike fell short in 14 s,
where the third set and both trace sets of the same code ran in real
time. Recording takes no part in the feedback deadline and is changed
here only in passing over a URB lost whole, so the slow sets are not
read as this change's; their cause (the host's load, the emulator's
UHCI on 2000, or something in the driver that the trace's timing
hides) is not settled. No set ended in a driver failure that a trace
showed.

## Not covered

- Real hardware under Windows' own EHCI or UHCI, on any system.
- Vista and 8 under Windows' own stacks; XP x64 and 7 x64 (the x64
  image is unsigned).
- 98 SE under NUSB's EHCI (the older `multi` stall of
  [uac2-profiles](2026-10-24-uac2-profiles.md) is the same shape as
  fix 1's case but was not rerun).
- Why a client paused the `fs` stream at 360 ms in one stock XP leg.
- The slow first plugs on 2000 above.
- 8 x86 under Windows' own stack (an intermittent `x4full` stop seen
  in the matrix, the same family as 7's; not run here).

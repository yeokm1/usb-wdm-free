# Status, legs and layout

Moved from `README.md` on 2026-10-24 so the README stays short. Where
`AGENTS.md` or a design document points at a `README.md` section
("Status", "Known issue", "Tested by the author", "Legs run so far",
"Layout", "Where the code comes from", "Installing"), that section is
here. The README keeps a summary of the targets and of how each driver
was evaluated.

## About the drivers

Free-software USB class drivers for Windows 98 SE through Windows 8,
written for this project under GPL-2.0-or-later ([LICENSE](../LICENSE)). They are WDM
(Windows Driver Model) drivers, the one driver model every one of those
systems shares, so one driver can serve Windows 98 SE, ME, 2000, XP,
Vista, 7 and 8, in 32-bit and, from XP on, 64-bit builds; each driver's
folders under `free/` name the systems it was built and tested for.
Windows 8.1 and later may load them too, but are not tested and not a
target. It holds both the
source code and the compiled drivers built from it, so a user can install
from here without building anything.

Where a driver needs it, a package may also carry user-mode code (a codec,
a print port monitor, a configuration tool), a VxD, or be an INF alone that
binds a driver already in Windows, but only when that piece exists to make
a driver here work or to fill a gap Windows leaves.

Third-party drivers that cannot be relicensed are kept apart, in
`usb-wdm-nonfree`.

This repository is standalone: everything needed to build, check and
install its drivers is in it or named in it, and it depends on no other
repository at build, release or run time. Its drivers are meant to work
under any USB host controller driver that offers the standard USB driver
interface.

**These are generic USB class drivers, not tied to xHCI98.** They were
written alongside the [xHCI98](https://github.com/yeokm1/xhci98) project,
a USB 3 host controller driver for the same systems, and its release
downloads can carry them as an option, but they talk to the device through
Windows' standard USB driver interface, so they work just as well on an
ordinary USB 2.0 (EHCI) port: under Windows' own USB 2.0 drivers on 2000
and later, or under NUSB's on Windows 98 SE, with no xHCI98 installed at
all. Each driver is checked under both an EHCI host controller driver and
xHCI98, and is not called ready until both pass on Windows 98 SE, the
lowest system every driver supports.

## Status

Release 0.1.0 (owner, 2026-10-24): `uac2`, `usbmidi`, `uvc98`, `usbcom`,
`cdcnet`, `rtl815x`, `xpad98`, `usbflop` and `libusb0` in `free/`, x86
unsigned and x64 test-signed, every folder "Built, not tested" (no leg on a
real PC yet); `STAGE.TXT` says which INFs xHCI98's installer may stage.
`free/tools/camcheck/` carries the CamCheck webcam preview (owner,
2026-10-24), a tool, not a driver package.
Release 0.1.1 (owner, 2026-10-25): 0.1.0 plus `ax88179` (98 SE and ME
staged, 2000 copy only; [release 0.1.1](testing/2026-10-25-release-0.1.1.md)).
Release 0.1.4 (owner, 2026-10-27): `rtl815x` 0.2.0.0 serves the Realtek
RTL8153D, RTL8156, RTL8156B, RTL8157 and RTL8159 on 98 SE, ME and 2000,
and these chips on XP to 7 where Realtek's driver in the non-free set does
not (`RTLXP7.INF`); simulator legs only
([release 0.1.4](testing/2026-10-28-release-0.1.4.md)).
Release 0.1.3 (owner, 2026-10-26): `usbcom` matches the CDC-ACM class on
98 SE, ME and 2000, where Windows has no CDC-ACM driver of its own, and
`cdcnet`'s `CDCRNDIS.INF` leaves 7's own RNDIS match alone
([release 0.1.3](testing/2026-10-26-release-0.1.3.md); the class matches per
system: [build.md section 7.1](design/build.md)).
Release 0.1.2 (owner, 2026-10-26): every package re-released under
GPL-2.0-or-later, `rtl815x` with its rewritten chip core
([release 0.1.2](testing/2026-10-26-release-0.1.2.md)).
Real PC (owner, reported 2026-10-29): `uac2`, `uvc98` and `ax88179` ran
on a real PC under Windows 98 SE with real devices (footnote 10 of the
table below).
Earlier: `noaud` 1.0.0.0, an INF-only Windows 98 SE workaround
(below and [docs/release-notes.md](release-notes.md)). Wave 1 has experimental source builds: UAC2 X4 M1
integration (simulated playback passes; physical playback pending), the USB floppy INFs and the NT CDC-ACM INF. Build and test
status: [wave 1 work log](testing/2026-10-21-wave1.md).
Wave 2's `cdcnet` 0.1.0.0 (CDC-ECM and RNDIS) has QEMU `usb-net` legs on
98 SE, ME, 2000, XP and 7 x64
([cdcnet work log](testing/2026-10-22-cdcnet.md),
[98 SE and ME legs](testing/2026-10-22-cdcnet-9x.md)). `xpad98` passes
with a simulated pad on 98 SE, ME, 2000, XP and Vista
([xpad98 legs](testing/2026-10-23-xpad98-legs.md)); `libusb0` loads on 98 SE
([sweep legs](testing/2026-10-23-sweep-legs.md)).
Webcam work includes portable descriptor/payload cores, a
[synthetic camera miniport](testing/2026-10-21-uvc-miniport.md) that
loads on Windows 2000, and [CamCheck](testing/2026-10-21-camcheck.md),
which displays a simulated UVC feed through Windows 7's native driver.
The [USB MIDI prototype](testing/2026-10-21-usbmidi-core.md) has
Windows 2000 loopback passes; reconnect testing continues.
These experiments are not released camera or MIDI packages.
The packages and their design documents:

| Package | What | Design | 98 SE now |
|---|---|---|---|
| `uvc98` | USB Video Class webcams, 98 SE to 2000, with `uvcdec` (`src/video/uvcdec/`), its MJPEG codec for VfW and DirectShow, for 1080p | [docs/design/uvc.md](design/uvc.md) | xHCI98: 160x120 PASS, 320x240 FAIL with the Brio via VM, both PASS with the simulator, 640x480 PASS after the stride fix (QEMU-paced, not sustained); NUSB EHCI: binds, FAIL in QEMU with the Brio via VM and with the simulator |
| `uac2` | USB Audio Class 2.0 playback and capture, 98 SE to 8; playback volume and mute from 0.1.4.0 | [docs/design/uac2.md](design/uac2.md) | 0.1.4.0, simulated devices: volume and mute PASS on 98 SE, XP x86, XP x64, Vista x86 and 7 x86 ([uac2-volume](testing/2026-10-28-uac2-volume.md)); build 16, simulated devices: xHCI98: all eight layouts PASS (X4, X4 with capture, 2-channel 16/24/32-bit, 8-channel playback only, mono capture only, Full Speed, clock selector and multiplier, adaptive); NUSB EHCI in QEMU: the same except 8-channel playback (FAIL, the stream stalls) and Full Speed (no leg); a capture-only device's hot arrival faulted WDMAud with builds 14 and 15 |
| `usbmidi` | USB MIDI 1.0 devices, 98 SE to 8 | [docs/design/usbmidi.md](design/usbmidi.md) | UHCI, NUSB EHCI and xHCI98 legs PASS on first plug; a replugged device works again only after a restart (98 SE limitation); under NUSB the composite parent `USBHUB.SYS` must be present |
| `cdcnet`, `rtl815x`, `ax88179` | USB Ethernet: CDC-ECM, CDC-NCM and RNDIS, 98 SE to 7 (RNDIS on XP to 7 as an INF that binds Windows' own driver); Realtek RTL8152/8153 and ASIX AX88179, 98 SE, ME and 2000 | [docs/design/ethernet.md](design/ethernet.md) | cdcnet: ECM and RNDIS PASS under NUSB EHCI and xHCI98 with QEMU `usb-net`, NCM PASS under both with the SIMULATED `ncmemu.py`; no real device; ax88179: xHCI98 legs (98 SE, ME) with the real dongle; rtl815x: no real device; with the SIMULATED RTL8153 (`rtlemu.py`) 98 SE PASS under NUSB EHCI and xHCI98 |
| `usbcom` | USB serial (CDC-ACM) as COM ports: a driver for 98 SE and ME, an INF for 2000 to 8 | [docs/design/serial.md](design/serial.md) | 98 SE driver: VM legs PASS under NUSB's EHCI, xHCI98 and the in-box UHCI; ME under xHCI98 (simulated device); no real board |
| `xpad98` | Xbox gamepads as game controllers: Xbox One and Series pads, 98 SE to Vista; the Xbox 360 wireless receiver on 98 SE and ME; on 98 SE to 2000, Xbox 360-protocol pads outside the 25 IDs of Masahiko Morii's `x360c`, such as 8BitDo pads and current clones | [docs/design/gamepad.md](design/gamepad.md) | simulated pads (360, Xbox One, receiver): JOYTEST PASS under NUSB EHCI, xHCI98 and UHCI after the 2026-10-23 fixes; the receiver fails under xHCI98 3.1.0.0 (8 endpoints refused); no real pad |
| `usbprn` | USB printers and USB-to-parallel cables as `USBnnn` printer ports on 98 SE: a driver and a port monitor. Low priority: built after the other drivers | [docs/design/printing.md](design/printing.md) | not built |
| `usbflop` | USB floppy drives (UFI) by class, 98 SE to 8: an INF that binds Windows' own `usbstor.sys` | [docs/design/floppy.md](design/floppy.md) | real Mitsumi via VM: UHCI and xHCI98 read PASS (ME too); NUSB EHCI: REAL CHECK NEEDED (Full Speed drive) |
| `noaud` | An INF only, released (1.0.0.0, opt-in): no driver for a listed webcam's UAC 1.0 microphone on 98 SE, so `usbaudio.sys` never loads for it. On 98 SE, if plugging a webcam freezes Windows, install NOAUD98.INF for its microphone; the camera keeps working and the microphone is ignored | [docs/release-notes.md](release-notes.md) | QEMU legs: 0 of 30 plugs froze with it, 16 of 30 without (UHCI and xHCI98) |
| `libusb0` | libusb-win32 0.1.12.2, driver and library, rebuilt for 98 SE and ME (2000 and later: upstream libusb-win32) | [docs/design/libusb.md](design/libusb.md) | install, load and device listing PASS under NUSB EHCI, xHCI98 and UHCI (QEMU `usb-storage` bound by a test copy of the INF); transfers and real devices owed |

Every driver works on Windows 98 SE at minimum (Owner, 2026-10-22): the
webcam (`uvc98`) and UAC2 audio (`uac2`) above all, and every other
driver too. A driver is not ready until its 98 SE legs pass under NUSB's
EHCI and under xHCI98. The "98 SE now" column above shows where each
stands; [docs/testing/hcd-matrix.md](testing/hcd-matrix.md) has the
legs.

Every driver works with EHCI host controllers (Owner, 2026-10-20):
Microsoft's in-box EHCI stack on 2000 and later, and NUSB's EHCI stack on
98 SE. Each package's tests include an EHCI leg on every tested system
(see "Tested by the author") that has an EHCI stack.

No installer for now (Owner, 2026-10-20): each driver is installed by hand
from its `free/` folder (see "Installing"). The deferred installer design,
`INSTALL.BAT` with a small helper, is kept in
[docs/design/installer.md](design/installer.md).

The toolchains, build profiles and release gates are drafted in
[docs/design/build.md](design/build.md).

Order, after the survey of existing drivers
([docs/design/existing.md](design/existing.md)):

- **Wave 1** (Owner, 2026-10-21): `uac2`; with it the INF-only pieces,
  `usbflop` and `usbcom`'s 2000-to-8 INF (`USBCDC.INF`), which bind
  Windows' own drivers.
- **Wave 2**: `uvc98` (Brio 100 held for testing), `usbcom`'s 98
  SE/ME driver, `usbmidi`, `cdcnet` and `xpad98`.
- **Later**: the installer and its helper (deferred); `libusb0`; `rtl815x` and `ax88179`, after `cdcnet`; `usbprn`,
  at low priority.

Other classes considered, and which come next, are surveyed in
[docs/design/candidates.md](design/candidates.md).

### Known issue: 98 SE may freeze when the first USB audio device is plugged in after a boot

Windows 98 SE may freeze when the first USB audio device is plugged in
after a boot: a sound card, a headset, a USB MIDI or audio interface, or
a webcam with a microphone. The mouse may still move, but the Start menu
does not open, and Ctrl+Alt+Del lists "Msgsrv32 [Not responding]". It is
98 SE's own: it happens with Windows' own USB audio driver
(`usbaudio.sys`) and with this repository's `uac2`, under Windows' own
USB 1.1 driver, NUSB's EHCI and xHCI98 alike, in about half of the first
plugs or more in QEMU; once one device has come through in a boot, later
plugs in that boot did not freeze
([record](testing/2026-10-23-brio-null.md)).

What helps:

- **Plug the device in before switching on**, and leave it plugged in
  until shutdown. A device present when Windows started came up with
  Windows in every boot read (7 of 7) and worked; unplugging it and
  plugging it back during that session froze 5 times in 7.
- If Windows freezes at a plug, restart with the device plugged in.
- For a webcam whose microphone you do not need, `NOAUD98.INF` (`noaud`,
  [release notes](release-notes.md)) gives the microphone no driver:
  no audio driver starts and the camera keeps working.

Loading the audio stack at boot does not prevent it (a USB audio device
running from boot did not protect the next plug), so no boot-time driver
is offered. Read in QEMU only; real-PC readings are owed.

## Tested by the author

"Tested" means the author installed the driver and used it on that system
with real hardware; such a cell names the device(s), the USB host stack
(Microsoft's in-box stack, NUSB's, or xHCI98) and the date, or links to
where they are recorded. Any other cell says nothing either way about
whether the driver works there: "Built, not tested" means a release folder
exists for that system, "Planned" that one is designed, "Not a target" that
none is.

Which systems are tested (Owner, 2026-10-20): 98 SE and three milestone
systems, each within the package's own range:

- **98 SE**, the main target: its own driver loader (NTKERN), and both
  Microsoft's USB 1.1 stack and NUSB's EHCI stack;
- **2000 SP4**, the oldest NT;
- **XP SP3, 32-bit**, the first NT with IAD-aware composite splitting and
  decorated INF models;
- **7 x64**, for x64 driver signing, the driver store and NT 6.

The package's other systems get release folders built from the same
source and stay "Built, not tested" until someone runs them. The one
exception is `usbflop` on ME, whose storage files differ from 98 SE's;
it is tested there too.

| Package | 98 SE | ME | 2000 | XP | XP x64 | Vista | Vista x64 | 7 | 7 x64 | 8 | 8 x64 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `uvc98` | Tested on a real PC (10) | Built, not tested | Built, not tested | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target |
| `uac2` | Tested on a real PC (10) | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Built, not tested |
| `usbmidi` | Built, not tested | Built, not tested (1) | Built, not tested | Built, not tested (1) | Built, not tested (1) | Built, not tested (1) | Built, not tested (1) | Built, not tested (1) | Built, not tested (1) | Built, not tested (1) | Built, not tested (1) |
| `cdcnet` | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Built, not tested | Not a target | Not a target |
| `rtl815x` | Built, not tested | Built, not tested | Built, not tested | Not a target (7) | Not a target (7) | Not a target (7) | Not a target (7) | Not a target (7) | Not a target (7) | Not a target | Not a target |
| `ax88179` | Tested on a real PC (10) | Built, not tested | Built, not tested | Not a target (7) | Not a target (7) | Not a target (7) | Not a target (7) | Not a target (7) | Not a target (7) | Not a target | Not a target |
| `usbcom` | Built, not tested | Built, not tested | Built, not tested (2) | Built, not tested (2) | Built, not tested (2) | Built, not tested (2) | Built, not tested (2) | Built, not tested (2) | Built, not tested (2) | Built, not tested (2) | Built, not tested (2) |
| `xpad98` | Built, not tested | Built, not tested | Built, not tested | Built, not tested (3) | Built, not tested (3) | Built, not tested (3) | Built, not tested | Not a target (8) | Not a target (8) | Not a target (8) | Not a target (8) |
| `usbprn` | Planned | Planned (4) | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target |
| `usbflop` | Built, not tested (5) | Built, not tested (6) | Built, not tested (6) | Built, not tested (6) | Built, not tested (6) | Built, not tested (6) | Built, not tested (6) | Built, not tested (6) | Built, not tested (6) | Built, not tested (6) | Built, not tested (6) |
| `noaud` | VM legs only (9) | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target |
| `libusb0` | Built, not tested | Built, not tested | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target | Not a target |

1. Listed devices only: the generic USB MIDI match is on 98 SE and 2000;
   elsewhere Windows' own driver serves the rest.
2. An INF that binds Windows' own `usbser.sys`; no driver of ours.
3. Xbox One/Series pads only. XBCD, a free (GPL-2.0-or-later) driver,
   serves Xbox 360-protocol pads on XP and later.
4. Only if testing shows that ME's own USB printing support cannot print.
5. An INF that binds the `usbstor.sys` of a USB storage driver set for
   98 SE, which must already be installed.
6. An INF that binds Windows' own `usbstor.sys`; no driver of ours.
7. The vendors' own drivers serve these chips there: Realtek's XP 5.23,
   Vista 6.27 and 7 7.61.20, and ASIX's AX88179 drivers, XP and Vista
   1.4.2.0 (x86) and 1.6.2.0 (x64), 7 1.12.11.0 (x86) and 1.14.11.0 (x64).
   ASIX's do not cover the AX88179A/B, which `cdcnet` serves through
   CDC-NCM.
8. Microsoft's own driver serves Xbox One/Series pads, and XBCD the Xbox
   360-protocol pads.
9. Released 2026-10-23 for 98 SE only (`free/audio/noaud/w98se/`). Its
   legs are QEMU guests (98 SE under 98 SE's own UHCI driver and under
   xHCI98, QEMU `usb-audio` and a Logitech Brio 100 passed through; 0 of
   30 plugs froze with the INF, 16 of 30 without), so the cell is not
   "Tested": no real PC yet.
10. The owner, reported 2026-10-29: run on a real PC under Windows 98 SE
   with a real device. The device, the host controller driver (xHCI98 or
   an EHCI stack) and what was exercised are not recorded yet, so the
   rule above that a driver is ready only when both host controller
   drivers pass is not yet shown.

### Legs run so far, by kind of device

None of the runs below makes a cell above "Tested": none was on a real PC
with the system installed on it. They are development legs, each recorded
in `docs/testing/`. The "Device" column says which kind of device the leg
used, with one of three labels:

- **emulated**: a QEMU device model, or this repository's simulator or
  synthetic source (named in the row);
- **real device via VM**: a physical device passed through from the
  author's host to a QEMU guest with QEMU `usb-host` (named in the row);
- **real PC**: a physical machine running the system. **None yet - owner's
  bench session**, for every driver.

"Host controller driver" is the guest's USB host controller driver: Windows'
own UHCI or EHCI driver, NUSB 3.3's EHCI driver on 98 SE, or xHCI98. The
emulated controller under it is always QEMU's (PIIX UHCI, `usb-ehci` or
`qemu-xhci`). Only legs in which this repository's driver (or INF) ran are
listed; runs of Windows' own class drivers (usbvideo, usbaudio) with the
same devices are references, not legs. Per host controller driver, the
same legs are in [docs/testing/hcd-matrix.md](testing/hcd-matrix.md).

**`uac2`**

| System | Host controller driver | Device | Verdict | Record |
|---|---|---|---|---|
| XP SP3 x86 | Windows' EHCI | real device via VM: Creative Sound Blaster X4 `041E:3278` | M0 starts (problem 0), no WinMM device; one STOP 0x7E | [uac2-vm](testing/2026-10-21-uac2-vm.md) |
| 7 x86 | Windows' EHCI | emulated: `uacemu.py`, a simulated X4 over `usb-redir` (simulated playback) | PASS (8/8 buffers; first try 3 ms short, two repeats pass) | [uac2-vm](testing/2026-10-21-uac2-vm.md), "Captured-layout simulation isolation" |
| 7 x86 | Windows' EHCI | real device via VM: Sound Blaster X4 | 2 s gates r1, r2, e1 PASS; 30 s soaks s1, c2, e2 and the 2 s c1 FAIL (OUT data shortfall) | [uac2-vm](testing/2026-10-21-uac2-vm.md), [handoff](../handoff.md) |
| XP SP3 x86 | Windows' EHCI | emulated: `uacemu.py`, a simulated X4 over `usb-redir`, build 6 (INF with `[Models.NTx86]`) | binds `MI_03` only (`usbccgp` keeps the parent; the whole-device id finds no driver); playback PASS | [98se-nusb-legs](testing/2026-10-22-98se-nusb-legs.md) |
| 7 x86 | Windows' EHCI | emulated: `uacemu.py`, a simulated X4 over `usb-redir`, build 6 (INF with `[Models.NTx86]`), TCG | binds `MI_03` only, as on XP; 8/8 buffers and 96000 samples in 3 plays, the program's tick-count check fails under TCG (clock artifact) | [98se-nusb-legs](testing/2026-10-22-98se-nusb-legs.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: `uacemu.py`, a simulated X4 over `usb-redir` | starts (every PortCls step), unplug clean; WinMM lists no wave device, playback not reached: FAIL | [98se-legs](testing/2026-10-22-98se-legs.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: `uacemu.py`, a simulated X4 over `usb-redir`, build 4 (SUM node) | SysAudio lists the device, `waveOut=1`; 2 s playback PASS (440 Hz capture), unplug idle, replug and unplug during playback PASS | [uac2-98se](testing/2026-10-22-uac2-98se.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: `uacemu.py`, a simulated X4 over `usb-redir`, build 6 (after the binding change) | 9 of 10 plays PASS (feedback 99 % delivered), unplug idle, replug and play, unplug during playback PASS | [98se-nusb-legs](testing/2026-10-22-98se-nusb-legs.md) |
| 98 SE | NUSB 3.3 EHCI | emulated: `uacemu.py`, a simulated X4 over `usb-redir`, build 5 (whole-device binding) | build 4 did not bind (no `MI_03` under NUSB); build 5 binds and starts, `waveOut=1`; playback 5 of 7 PASS at QEMU's default EHCI, 0 of 3 with `maxframes=512`; build 6 diagnostics: NUSB schedules the feedback endpoint but only 6 % of its frames bring data (QEMU's EHCI skips frames and `usb-redir` prefills), against 99 % under xHCI98 with the same driver; unplug, replug and unplug during playback PASS: FAIL in QEMU, real-EHCI leg owed | [98se-nusb-legs](testing/2026-10-22-98se-nusb-legs.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: `uacemu.py`, a simulated X4 over `usb-redir`, the 2026-10-22 build, HID and CDC functions given no driver | arrival count from a restored desktop: 98 SE froze (Msgsrv32 not responding) in 9 of 13 arrivals, the same freeze as with Windows' `usbaudio.sys`; 0 of 9 restores with no device: FAIL (98 SE's audio arrival, not the driver's code, as far as read) | [brio-null](testing/2026-10-23-brio-null.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: `uacemu.py` profiles `x4`, `x4full`, `hs24`, `multi`, `mic`, `fs`, `clocks`, `adaptive` (UAC 2.0 layouts beyond the X4), build 16 | every profile PASS: playback 8/8 buffers with signal at the simulator, capture with the simulator's 1 kHz tone; 8 channels carry signal (build 10 before the channel-mask fix: silence); builds 14 and 15 faulted WDMAud at the capture-only `mic` arrival (3 of 3), build 16 PASS 2 of 2; 24-bit and 6-channel alternates not opened by KMixer | [uac2-profiles](testing/2026-10-24-uac2-profiles.md) |
| 98 SE | NUSB 3.3 EHCI | emulated: the same profiles except `fs`, cold boot with the device present, builds 14 to 16 | `x4`, `x4full`, `hs24`, `mic`, `clocks`, `adaptive` PASS; `multi` FAIL (8-channel stream stalls after about 8 s, playback 0/8) | [uac2-profiles](testing/2026-10-24-uac2-profiles.md) |
| XP SP3 x86 | Windows' EHCI | emulated: `uacemu.py` `x4full`, build 16 | installs for `MI_03`; playback 48 and 44.1 kHz PASS; capture FAIL (buffers complete at about half speed, no tone; the real X4 also captures at half speed) | [uac2-profiles](testing/2026-10-24-uac2-profiles.md) |
| 7 x86 | Windows' EHCI | emulated: `uacemu.py` `x4full`, build 16, TCG | playback 48 and 44.1 kHz PASS; capture PASS, 1 kHz tone, 2 of 2 | [uac2-profiles](testing/2026-10-24-uac2-profiles.md) |
| XP SP3 x86 | Windows' EHCI | real device via VM: Sound Blaster X4, build 16, TCG | no STOP 0x7E; 2 s plays 3 of 3 and a 30 s play PASS; unplug during playback survives; replug and play PASS | [uac2-profiles](testing/2026-10-24-uac2-profiles.md) |
| 7 x86 | Windows' EHCI | real device via VM: Sound Blaster X4, build 16, TCG | 2 s play PASS; 30 s soak PASS 2 of 2 (e2's failure not reproduced; a real 7 PC settles it) | [uac2-profiles](testing/2026-10-24-uac2-profiles.md) |
| ME | xHCI98 3.1.0.0 | emulated: `uacemu.py` `x4full` over `usb-redir`, the released 0.1.0 `uac2wdm.sys` and `uac2wdm.inf` (`w98se-me`) | the INF in ME's INF folder before the first arrival: installed at the plug (KS file prompts skipped), `waveOut=1`, `waveIn=1`; playback 48 and 44.1 kHz 8/8 PASS, signal at the simulator; capture PASS (1 kHz tone); unplug clean; no freeze at the first arrival | [release 0.1.1](testing/2026-10-25-release-0.1.1.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: `uacemu.py` `x4`, `cx31993` (a CX31993 headphone DAC's playback layout) and `mic`, Windows' SW MIDI synth (`SWMIDI`) | the 0.1.4 INF gives WinMM no MIDI output (`SubClasses` `wave,mixer`): no SW synth, a `.mid` "cannot play", `.wav` plays; with the `midi` subclass (`90ac079`) the SW synth is listed and a `.mid` reaches the device at 16-bit 48 kHz with signal; capture-only `mic` unaffected (rec PASS) | [uac2-swmidi](testing/2026-10-28-uac2-swmidi.md) |
| XP x64 | Windows' EHCI | emulated: `uacemu.py` `x4`, `cx31993`, `cx31993` at 44.1 kHz only, the SW MIDI synth | 0.1.4 INF: no MIDI output, `.mid` "cannot play"; fixed INF: SW synth listed, `town.mid` and `flourish.mid` play with signal at 48 and 44.1 kHz; `usbaudio.sys` binds a UAC 2.0 device first, and installing `uac2` over it hung until a restart (2 of 2) | [uac2-swmidi](testing/2026-10-28-uac2-swmidi.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: `uacemu.py` `cx31993`, `hs24` with and without a Feature Unit; 0.1.4.0 volume and mute (`93970fc`; the `hs24` hardware leg and the GUI on `4117302`) | PASS: "Master Volume" and "Master Mute" on the Speaker line; SET_CUR per channel (`cx31993`) or master (`hs24`) at 0, -6, -12, -24 dB and mute, the level moving at the device; without a Feature Unit the driver scales and mutes the samples; Volume Control slider, "Mute all", tray popup and balance work | [uac2-volume](testing/2026-10-28-uac2-volume.md) |
| XP SP3 x86 | Windows' EHCI | emulated: `cx31993`, `hs24` without a Feature Unit; 0.1.4.0 (`93970fc`) | PASS: per-channel SET_CUR and master mute; the software level without a unit; Volume Control slider and "Mute all" | [uac2-volume](testing/2026-10-28-uac2-volume.md) |
| XP x64 | Windows' EHCI | emulated: `hs24` with and without a Feature Unit, `cx31993`; 0.1.4.0 x64 unsigned (`93970fc`) | PASS: master or per-channel SET_CUR and mute; the software level without a unit; Volume Control, tray popup and Sounds and Audio Devices slider and Mute drive both; levels restored at the next start; `usbaudio.sys` binds first and updating to `uac2` hung (2 of 2), so the legs moved `wdma_usb.inf` aside | [uac2-volume](testing/2026-10-28-uac2-volume.md) |

No 2000 leg; ME under xHCI98 only. Real PC: none yet - owner's bench session.

**`noaud`**

| System | Host controller driver | Device | Verdict | Record |
|---|---|---|---|---|
| 98 SE | Windows' UHCI | emulated: QEMU `usb-audio` (UAC 1.0, Full Speed) | PASS: 0 of 10 plugs froze with the INF, 6 of 10 without; the function installs with no driver | [brio-null](testing/2026-10-23-brio-null.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: QEMU `usb-audio` | PASS: 0 of 10 froze with the INF, 8 of 10 without | [brio-null](testing/2026-10-23-brio-null.md) |
| 98 SE | xHCI98 3.1.0.0 | real device via VM: Logitech Brio 100 `046D:094C` | PASS: 0 of 10 froze with the INF (microphone function with no driver, video still on `uvc98`'s binding), 2 of 10 without; video not captured | [brio-null](testing/2026-10-23-brio-null.md) |

No ME leg. Real PC: none yet - owner's bench session.

**`uvc98` and `uvcdec`**

| System | Host controller driver | Device | Verdict | Record |
|---|---|---|---|---|
| 2000 SP4 | none (root-enumerated, no USB) | emulated: synthetic camera M0 (`ROOT\UVCSIM98`) | loads and removes; no streaming by design | [uvc-miniport](testing/2026-10-21-uvc-miniport.md) |
| 2000 SP4 | none (root-enumerated, no USB) | emulated: synthetic camera M1 (`ROOT\UVCSIM1`, worker frames) | PASS: three changing synthetic frames | [uvc-worker](testing/2026-10-21-uvc-worker.md) |
| 2000 SP4 | Windows' EHCI (2000 SP4 `usbehci.sys`) | real device via VM: Logitech Brio 100 `046D:094C` | M2: 160x120, 176x144, 320x240 YUY2 PASS; 640x480 FAIL (high-bandwidth packets fail on the host side of the passthrough) | [uvc-m2](testing/2026-10-22-uvc-m2.md) |
| 2000 SP4 | Windows' EHCI (2000 SP4 `usbehci.sys`) | emulated: `uvcsim.py` (the control for the NUSB leg) | M2 `a8bf2bc`: 160x120 (frames byte-exact), 320x240 and 640x480 at 30 fps (3 x 800, high bandwidth) PASS; 7-23 failed packets against NUSB's ~1000 per run | [uvcsim-98se](testing/2026-10-23-uvcsim-98se.md) |
| 98 SE | xHCI98 3.1.0.0 | real device via VM: Logitech Brio 100 | M2: 160x120 PASS (3 runs, one after a replug); 320x240 FAIL (every payload error-flagged); unplug clean | [98se-legs](testing/2026-10-22-98se-legs.md) |
| 2000 SP4 | xHCI98 3.1.0.0 | real device via VM: Logitech Brio 100 | M2: 160x120 FAIL (stream nearly all empty packets); unplug clean | [98se-legs](testing/2026-10-22-98se-legs.md) |
| 2000 SP4 | xHCI98 3.1.0.0 and batch 10 | real device via VM: Logitech Brio 100, stock QEMU | 160x120 FAIL, as on 2026-10-22: nearly all packets empty, on either xHCI98 build | [uvc98-2k-xhci](testing/2026-10-24-uvc98-2k-xhci.md) |
| 2000 SP4 | xHCI98 batch 10 | real device via VM: Logitech Brio 100, the pinned `run-orphan` QEMU (fixed `usb-host` isochronous path, as the 98 SE legs used) | 160x120 PASS in the first session; 320x240 and a later 160x120 FAIL (payloads error-flagged by the camera), and every stop's alternate-0 select STALLs: not uvc98's code (the simulator passes), QEMU's xHCI `usb-host` path or xHCI98, owed to a real PC | [uvc98-2k-xhci](testing/2026-10-24-uvc98-2k-xhci.md) |
| 2000 SP4 | xHCI98 3.1.0.0 and batch 10 | emulated: `uvcsim.py` as `FFFF:00E0` (no Brio ids, no Brio quirk) | 160x120, 320x240 and 640x480 PASS | [uvc98-2k-xhci](testing/2026-10-24-uvc98-2k-xhci.md) |
| 2000 SP4 | Windows' UHCI | emulated: `uvcsim.py` as `FFFF:00E0`, Full Speed; class-matched INF (rel-fixes 4511c85), whole camera bound | 160x120 PASS | [uvc98-2k-xhci](testing/2026-10-24-uvc98-2k-xhci.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: `uvcsim.py` as `FFFF:00E0`; class-matched INF (rel-fixes 4511c85) | 160x120 and 320x240 PASS; unplug, replug and 160x120 PASS | [uvc98-2k-xhci](testing/2026-10-24-uvc98-2k-xhci.md) |
| 98 SE | NUSB 3.3 EHCI | real device via VM: Logitech Brio 100 (whole device bound; no split, so no microphone function and no freeze) | M2: 160x120 FAIL (4 runs: no frame, hundreds of failed packets; QEMU's `usb-host` restarted the stream 275 times in one capture); 640x480 hung the guest | [98se-nusb-legs](testing/2026-10-22-98se-nusb-legs.md) |
| 98 SE | NUSB 3.3 EHCI | emulated: `uvcsim.py`, a simulated UVC camera over `usb-redir` (whole device bound) | M2: 160x120, 320x240 and 640x480 (5 fps) FAIL in QEMU: about a third of the packets never reach the guest, thousands of zero-length (QEMU underruns) and hundreds of failed ones; no frame. 640x480 at 30 fps (high bandwidth) hung the guest | [uvcsim-98se](testing/2026-10-23-uvcsim-98se.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: `uvcsim.py` | M2: 160x120 PASS (frames byte-exact); 320x240 PASS (3 frames, 8 dropped); 640x480 at 30 fps FAIL (uvc98's 4096-byte packet stride refused by xHCI98), PASS after the fix `a8bf2bc` (3 byte-exact frames; QEMU lets about 18 % of microframes through); 640x480 at 5 fps FAIL in QEMU (qemu-xhci runs a fifth of the TDs); unplug during capture and replug PASS | [uvcsim-98se](testing/2026-10-23-uvcsim-98se.md) |
| 2000 SP4 | Windows' EHCI (2000 SP4 `usbehci.sys`) | emulated: two `uvcsim.py` cameras; CamCheck through the VfW mapper | the released `uvcm2.sys` (0.1.0 re-cut): live preview (RGB24 160x120 and 320x240), camera switching, and unplugging the open camera then Stop PASS, free and trace builds | [uvc98-vfw](testing/2026-10-24-uvc98-vfw.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: two `uvcsim.py` cameras; CamCheck through the VfW mapper | the released `uvcm2.sys` (0.1.0 re-cut): live preview (RGB24 160x120), camera switching, and unplugging the open camera then Stop PASS, free and trace builds | [uvc98-vfw](testing/2026-10-24-uvc98-vfw.md) |
| 98 SE | none (codec only, no USB) | emulated: test vectors and MJPEG AVIs made from them; no USB device | `uvcdec`: JDTEST PASS (pentium3 MMX SSE, core2duo MMX SSE SSE2; SIMD equals C); INF install and removal, VfW and DirectShow playback PASS; INF now leaves an existing `VIDC.MJPG` alone | [uvcdec-guest](testing/2026-10-22-uvcdec-guest.md) |
| 2000 SP4 | none (codec only, no USB) | emulated: test vectors and MJPEG AVIs made from them; no USB device | `uvcdec`: JDTEST PASS (pentium3); INF install and removal, VfW and DirectShow playback PASS | [uvcdec-guest](testing/2026-10-22-uvcdec-guest.md) |

On Windows 2000's own USB stack and under NUSB on 98 SE the whole camera
binds to `uvc98` and its microphone has no driver (owner, 2026-10-24;
[design 4](design/uvc.md)); under xHCI98 the camera is split and the
microphone keeps its own function.

`uvcdec` with a camera (MJPEG through `uvc98`): not started. No ME leg of
either. `uvc98`'s 98 SE NUSB leg fails in QEMU below the driver
(above), with the Brio passed through and with the simulator. Real PC: none yet - owner's bench session.

**`usbmidi`**

| System | Host controller driver | Device | Verdict | Record |
|---|---|---|---|---|
| 2000 SP4 | Windows' UHCI | emulated: the MIDI simulator `midiemu.py` over `usb-redir` (one cable, `FFFF:0001`), earlier experiment build | loopback PASS; live hot-replug not passed | [usbmidi-vm](testing/2026-10-21-usbmidi-vm.md) |
| 2000 SP4 | Windows' UHCI | emulated: the MIDI simulator, `multi` profile | M1 PASS (loopback, unplug under traffic after a fix) | [usbmidi-m1](testing/2026-10-22-usbmidi-m1.md) |
| 98 SE | Windows' UHCI (`USB.INF`) | emulated: the MIDI simulator, `multi` and `lone` profiles | M1 PASS after the thread-wait fix; after any replug every MIDI open fails (MMSYSERR_ALLOCATED) until a restart, a 98 SE limitation; guest freezes on the first WDM audio arrival, also without `usbmidi` | [usbmidi-m1](testing/2026-10-22-usbmidi-m1.md) |
| ME | Windows' UHCI | emulated: the MIDI simulator, `multi` profile | start failed (C0000182) until a SELECT_INTERFACE fallback; then loopbacks, open, unplug and replug PASS | [usbmidi-m1](testing/2026-10-22-usbmidi-m1.md) |
| 7 x86 | Windows' UHCI | real device via VM: Roland UM-ONE mk2 (TAB, `0582:0168`) | starts; WinMM open/close only (no DIN loopback, no MIDI bytes) | [umone2](testing/2026-10-21-umone2.md) |
| 2000 SP4 | xHCI98 3.1.0.0 | emulated: the MIDI simulator, `multi` profile, Full and High Speed | PASS: loopbacks, isolation, stress, unplug under traffic, replug, new port after restart | [98se-legs](testing/2026-10-22-98se-legs.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: the MIDI simulator, `multi` profile | loopbacks, stress and unplug PASS; same-port replug FAIL (MMSYSERR_ALLOCATED) | [98se-legs](testing/2026-10-22-98se-legs.md) |
| 98 SE | NUSB 3.3 EHCI | emulated: the MIDI simulator, `multi` profile, High Speed | loopbacks and open PASS, unplug clean; same-port replug FAIL (the 98 SE limitation) | [usbmidi-m1](testing/2026-10-22-usbmidi-m1.md) |
| 98 SE | NUSB 3.3 EHCI | emulated: the MIDI simulator, `multi` profile, High Speed, on the `w98-nusb` image after its missing composite parent `USBHUB.SYS` was supplied | loopbacks, isolation and unplug PASS; stress lost 6-168 bytes in 3 of 4 runs (below the driver: QEMU EHCI's IN cancels); same-port replug FAIL (the 98 SE limitation) | [98se-nusb-legs](testing/2026-10-22-98se-nusb-legs.md) |
| 2000 SP4 | Windows' EHCI (SP4) | emulated: the MIDI simulator, `multi` profile, High Speed | loopbacks and open PASS; unplug and replug PASS | [usbmidi-m1](testing/2026-10-22-usbmidi-m1.md) |
| XP SP3 | Windows' EHCI | emulated: the MIDI simulator, `multi` profile, High Speed, test INF installed over Windows' `usbaudio` | loopbacks paired by name and open PASS; replug PASS; WinMM numbers the output ports in reverse | [usbmidi-m1](testing/2026-10-22-usbmidi-m1.md) |

Known limitation: on 98 SE, restart Windows after replugging a USB MIDI
device. Once any USB MIDI device has been removed, 98 SE's own MIDI path
refuses every MIDI open on a device that arrives afterwards (the same one
or another), before the driver is reached; devices plugged in all along keep
working ([design 18](design/usbmidi.md); no driver fix,
[2026-10-23 record](testing/2026-10-23-usbmidi-98se-replug.md)).

Real PC: none yet - owner's bench session.

**`cdcnet`**

| System | Host controller driver | Device | Verdict | Record |
|---|---|---|---|---|
| 2000 SP4, XP SP3 x86 | Windows' UHCI | emulated: QEMU `usb-net` (Full Speed) | 2026-10-21 M0/M1 spike, ECM: PASS (superseded by 0.1.0.0) | [cdcnet 10-21](testing/2026-10-21-cdcnet.md) |
| 2000 SP4 | Windows' UHCI | emulated: QEMU `usb-net` (Full Speed) | 0.1.0.0 ECM and RNDIS PASS; free build under Driver Verifier PASS | [cdcnet](testing/2026-10-22-cdcnet.md) |
| 98 SE | Windows' UHCI (`USB.INF`; NUSB 3.3 installed, not used) | emulated: QEMU `usb-net` (Full Speed) | ECM and RNDIS PASS | [cdcnet](testing/2026-10-22-cdcnet.md) |
| XP SP3 x86, 7 SP1 x64 | Windows' UHCI | emulated: QEMU `usb-net` (Full Speed) | ECM PASS; `CDCRNDIS.INF` binds Windows' RNDIS driver, which then fails on QEMU's model (Code 10 on XP, stopped on 7 x64) | [cdcnet](testing/2026-10-22-cdcnet.md) |
| 98 SE | NUSB 3.3 EHCI | emulated: QEMU `usb-net`, High Speed (test patch) | ECM and RNDIS: traffic and removal under traffic PASS after `8438c07` | [cdcnet-9x](testing/2026-10-22-cdcnet-9x.md) |
| 98 SE | xHCI98 3.1.0.0 | emulated: QEMU `usb-net`, Full and High Speed | ECM (Full, High Speed) and RNDIS (High Speed) PASS after `2e6b56a` | [cdcnet-9x](testing/2026-10-22-cdcnet-9x.md) |
| ME | xHCI98 3.1.0.0 | emulated: QEMU `usb-net`, High Speed (test patch) | ECM: install, traffic and removal under traffic PASS; at shutdown ME powers off without halting the miniport | [cdcnet-9x](testing/2026-10-22-cdcnet-9x.md) |
| ME | xHCI98 3.1.0.0 | emulated: QEMU `usb-net`, Full Speed | RNDIS: install (preset reached), NETTEST 12 x 1 MiB, removal under traffic, same-port replug and NETTEST PASS | [sweep-legs](testing/2026-10-23-sweep-legs.md) |
| 98 SE | xHCI98 3.2.0.0 | emulated: QEMU `usb-net`, Full Speed | the released `CDCNET.INF` (0.1.0 re-cut, gadget id): staged install, DHCP and ping PASS | [release 0.1.0](testing/2026-10-24-release-0.1.0.md) |
| XP SP3 x86 | Windows' UHCI | emulated: QEMU `usb-net`, Full Speed | the released `CDCNET.INF` (0.1.0 re-cut, gadget id): install, DHCP and ping PASS | [release 0.1.0](testing/2026-10-24-release-0.1.0.md) |
| XP SP3 x86 | Windows' EHCI | emulated: QEMU `usb-net`, High Speed (test patch) | ECM: NETTEST, removal under traffic, replug and NETTEST PASS | [sweep-legs](testing/2026-10-23-sweep-legs.md) |
| 2000 SP4 | Windows' EHCI (SP4 `usbehci`) | emulated: QEMU `usb-net`, High Speed (test patch) | ECM: install, NETTEST twice, idle removal, removal under traffic, replug and NETTEST PASS | [sweep-legs](testing/2026-10-23-sweep-legs.md) |

The other rows ran the test INFs (`CDCT98.INF`, `CDCTEST.INF`), which bind
the same device with the same install sections; until the 0.1.0 re-cut the
released INFs did not match QEMU `usb-net` at all.

No real device (no NCM or RNDIS hardware). Real PC: none yet - owner's
bench session.

**`ax88179`**

| System | Host controller driver | Device | Verdict | Record |
|---|---|---|---|---|
| 98 SE | xHCI98 3.1.0.0 | real device via VM: the owner's AX88179 (`0B95:1790`), passthrough on `qemu-xhci`, SuperSpeed | install, DHCP, ping 0-4000 bytes and removal under traffic PASS after `d6f514e`; a `ping -t` running across a replug recovers | [ax88179-9x](testing/2026-10-22-ax88179-9x.md), [ax88179-fcs](testing/2026-10-22-ax88179-fcs.md) |
| ME | xHCI98 3.1.0.0 | real device via VM: the same AX88179, passthrough on `qemu-xhci` | install, DHCP, ping and removal under traffic PASS (same open point) | [ax88179-9x](testing/2026-10-22-ax88179-9x.md) |
| 98 SE | NUSB 3.3 EHCI | real device via VM: the same AX88179 behind a USB 2.0 hub, passthrough on `usb-ehci`, High Speed | install, DHCP, ping 0-4000 bytes and removal under traffic PASS; the FCS safety net fired in the first session after plug-in; with the settle cycle 0 FCS frames in 3 sessions and a `ping -t` across a replug recovers (first-after-plug case not reproducible in QEMU) | [ax88179-9x](testing/2026-10-22-ax88179-9x.md), [ax88179-fcs](testing/2026-10-22-ax88179-fcs.md) |

ME under an EHCI driver: later, on an ME image with SweetLow's USB 2.0
stack.

**`rtl815x`**

No device available: the owner has no RTL8152/8153/8156, and QEMU has no
model of the chip. Host builds and host tests only
([ax88179-rtl815x](testing/2026-10-22-ax88179-rtl815x.md)).

Earlier, not legs, for `ax88179`: the owner's AX88179 (`0B95:1790`) read
by a Windows 2000 guest through QEMU EHCI with no driver of ours,
descriptors only ([ax88179-2k-descriptors](testing/2026-10-21-ax88179-2k-descriptors.md)),
and a host-side test on the author's host with the portable
framing code, not a Windows driver ([ax88179-physical](testing/2026-10-21-ax88179-physical.md)).
Real PC: none yet - owner's bench session.

**`usbcom`**

| System | Host controller driver | Device | Verdict | Record |
|---|---|---|---|---|
| 2000 SP4 (`USBCDC.INF`) | Windows' UHCI | emulated: the CDC-ACM simulator `acmemu.py` over `usb-redir` | binding PASS; the 65-byte echo fails because 2000's `usbser.sys` stops reading after the simulator's flush ZLP; without that ZLP every echo (1 to 1537 bytes) PASS. REAL CHECK NEEDED: a real board on 2000 | [usbcdc-vm](testing/2026-10-21-usbcdc-vm.md), [sweep-legs](testing/2026-10-23-sweep-legs.md) |
| 7 SP1 x64 (`USBCDC.INF`) | Windows' UHCI | emulated: the CDC-ACM simulator | echoes 1 to 1537 bytes PASS | [usbcdc-vm](testing/2026-10-21-usbcdc-vm.md) |
| XP SP3 x86 (`USBCDC.INF`) | Windows' EHCI | real device via VM: the Sound Blaster X4's CDC-ACM function (`MI_01`) | binding and open/close only, no data | [uac2-vm](testing/2026-10-21-uac2-vm.md) |
| 98 SE (`USBCOM.SYS`, `USBCOMV.VXD`) | xHCI98 3.1.0.0 (QEMU `qemu-xhci`) | emulated: the CDC-ACM simulator, Uno and Pico profiles, Full and High Speed | `COMTEST` 58/58, pull while open and replug, then 97 (free) and 161 (trace) opens with 64 KiB echoes: PASS | [usbcom98-legs](testing/2026-10-22-usbcom98-legs.md) |
| ME (`USBCOM.SYS`, `USBCOMV.VXD`) | xHCI98 3.1.0.0 (QEMU `qemu-xhci`) | emulated: the CDC-ACM simulator, Uno and Pico profiles, High Speed | `COMTEST` 58/58, pull while open and replug, 40 opens, 16 x 64 KiB echo: PASS (free and trace) | [usbcom98-legs](testing/2026-10-22-usbcom98-legs.md) |
| 98 SE (`USBCOM.SYS`, `USBCOMV.VXD`) | Windows' UHCI (QEMU `piix3-usb-uhci`) | emulated: the CDC-ACM simulator, Uno profile, Full Speed | `COMTEST` 58/58, pull while open and replug, 40 opens, 16 x 64 KiB echo: PASS (free and trace) | [usbcom98-legs](testing/2026-10-22-usbcom98-legs.md) |
| 98 SE (`USBCOM.SYS`, `USBCOMV.VXD`) | NUSB 3.3 EHCI (QEMU `usb-ehci`) | emulated: the CDC-ACM simulator, High Speed | `COMTEST` 58/58, pull while open and replug: PASS; a 64 KiB echo fails in about one run in six, below the driver | [usbcom98-legs](testing/2026-10-22-usbcom98-legs.md) |

The 98 SE / ME driver: NUSB does not install on ME, so ME has no EHCI leg.
No real board (Uno R3, Pico) yet; real PC: none yet - owner's bench session.

**`usbflop`**

| System | Host controller driver | Device | Verdict | Record |
|---|---|---|---|---|
| 98 SE | Windows' UHCI (from the 98 SE CD; NUSB 3.3 installed, its EHCI not used) | real device via VM: Mitsumi USB floppy `03EE:6901` | read PASS: three files, 133557 bytes, no medium writes | [usbflop-media](testing/2026-10-21-usbflop-media.md), [usbflop-nt-followup](testing/2026-10-21-usbflop-nt-followup.md) |
| ME | Windows' UHCI | real device via VM: Mitsumi floppy | binding (parent and child working), no medium | [usbflop-more](testing/2026-10-21-usbflop-more.md) |
| 2000 SP4 | Windows' UHCI | real device via VM: Mitsumi floppy | binding, no-medium and idle removal | [usbflop-vm](testing/2026-10-21-usbflop-vm.md) |
| XP SP3 x86, 7 x64 | Windows' UHCI | real device via VM: Mitsumi floppy | binding, no medium | [usbflop-nt-followup](testing/2026-10-21-usbflop-nt-followup.md) |

| 98 SE | xHCI98 3.1.0.0 (NUSB 3.3's `USBSTOR.SYS`) | real device via VM: Mitsumi floppy, native `USBSTOR.INF` set aside | read PASS: the same three files and CRCs | [sweep-legs](testing/2026-10-23-sweep-legs.md) |
| ME | xHCI98 3.1.0.0 | real device via VM: Mitsumi floppy, native `USBSTOR.INF` set aside | read PASS (ME's first medium read) | [sweep-legs](testing/2026-10-23-sweep-legs.md) |
| 2000 SP4 | Windows' UHCI | emulated, simulator: the UFI floppy simulator `ufiemu.py` over `usb-redir`, an unlisted drive (`FFFF:0020` CBI, `FFFF:0021` CB), Full Speed, FAT12 medium | class match binds through the wizard; read PASS for CBI and CB (three files, CRC-32s match); Windows writes the root directory's access dates during the read | [usbflop-nt-xpad-x64](testing/2026-10-24-usbflop-nt-xpad-x64.md) |
| XP SP3 x86 | Windows' UHCI | emulated, simulator: the same | read PASS for CBI and CB, the same | [usbflop-nt-xpad-x64](testing/2026-10-24-usbflop-nt-xpad-x64.md) |
| 7 SP1 x64 | Windows' UHCI | emulated, simulator: the same | INF staged with `pnputil`, then the plug installs it without a prompt; read PASS for CBI and CB | [usbflop-nt-xpad-x64](testing/2026-10-24-usbflop-nt-xpad-x64.md) |

EHCI legs: REAL CHECK NEEDED (a Full Speed floppy is not carried by
QEMU's EHCI). Real PC: none yet - owner's bench session.

**`libusb0`**

| System | Host controller driver | Device | Verdict | Record |
|---|---|---|---|---|
| 98 SE | xHCI98 3.1.0.0 | emulated: QEMU `usb-storage` (`46F4:0001`), SuperSpeed, bound by a test copy of `LIBUSB0.INF` | install, driver load, `TESTLIBU` listing (descriptors and strings) from another folder, idle unplug and replug PASS | [sweep-legs](testing/2026-10-23-sweep-legs.md) |
| 98 SE | NUSB 3.3 EHCI | emulated: the same, High Speed | the same PASS | [sweep-legs](testing/2026-10-23-sweep-legs.md) |
| 98 SE | Windows' UHCI | emulated: the same, Full Speed | the same PASS | [sweep-legs](testing/2026-10-23-sweep-legs.md) |

No bulk, interrupt, timeout, reset or open-handle removal check yet; no
USBasp or AVRISP mkII; no ME leg. Real PC: none yet - owner's bench session.

**`xpad98`**

| System | Host controller driver | Device | Verdict | Record |
|---|---|---|---|---|
| 98 SE | xHCI98 3.1.0.0 | emulated: the pad simulator `xpademu.py` over `usb-redir`, `x360` (2DC8:3106) and `xone` (045E:02EA) profiles, Full Speed | `JOYTEST` PASS for both after the 2026-10-23 fixes (the 360 pad through the interface-subset select); the `receiver` profile FAIL (8 endpoints refused below the driver) | [xpad98-legs](testing/2026-10-23-xpad98-legs.md) |
| 98 SE | Windows' UHCI | emulated: the same, `x360` | `JOYTEST` PASS after the fixes (first run: "not connected correctly", Rx/Ry and the 0..65535 range) | [xpad98-legs](testing/2026-10-23-xpad98-legs.md) |
| 98 SE | NUSB 3.3 EHCI | emulated: `x360`, `xone` and `receiver`, High Speed | `JOYTEST` PASS (the receiver: slot 0) | [xpad98-legs](testing/2026-10-23-xpad98-legs.md) |
| 2000 SP4 | Windows' EHCI | emulated: `x360` and `xone` (045E:02EA, GIP) profiles, High Speed | `JOYTEST`: every button, axis and POV direction PASS (also rerun with the fixed release build); GIP init and Guide acknowledgement as designed | [xpad98-legs](testing/2026-10-23-xpad98-legs.md) |
| XP SP3 x86 | Windows' EHCI | emulated: `xone`, High Speed | `JOYTEST` PASS; unplug clean | [xpad98-legs](testing/2026-10-23-xpad98-legs.md) |
| Vista SP2 x86 | Windows' EHCI | emulated: `xone`, High Speed | `JOYTEST` PASS; unplug clean | [xpad98-legs](testing/2026-10-23-xpad98-legs.md) |

| ME | xHCI98 3.1.0.0; Windows' UHCI | emulated: `x360`, `xone` (xHCI98), `x360` and `receiver` (UHCI) | `JOYTEST` PASS (the receiver: the connected slot) | [xpad98-legs](testing/2026-10-23-xpad98-legs.md) |
| 98 SE | xHCI98 test build (43.67, per-device cap 16) | emulated: `x360`, `receiver` | full SELECT_CONFIGURATION accepted (7 and 16 endpoints); `JOYTEST` PASS | [xpad98-legs](testing/2026-10-23-xpad98-legs.md) |
| XP x64 | Windows' EHCI | emulated, simulator: `xone`, High Speed; the x64 build, unsigned | `JOYTEST` PASS; unplug clean | [usbflop-nt-xpad-x64](testing/2026-10-24-usbflop-nt-xpad-x64.md) |
| Vista SP2 x64 | Windows' EHCI | emulated, simulator: `xone`, High Speed; the x64 build signed with a throwaway test certificate made in the guest, test signing on | `JOYTEST` PASS; unplug clean | [usbflop-nt-xpad-x64](testing/2026-10-24-usbflop-nt-xpad-x64.md) |

98 SE passes under NUSB's EHCI, xHCI98 and UHCI with simulated pads, and
ME under xHCI98 and UHCI; XP and Vista were rerun with the fixed build and
pass. The receiver is unsupported under xHCI98 3.1.0.0 (8 endpoints
refused) and works with xHCI98 43.67. No x64 or real-pad leg.
Real PC: none yet - owner's bench session.

**`usbprn`**

Not built.
Real PC: none yet - owner's bench session.

## Reporting a problem

These drivers are in beta. If you find a bug in one of them, please open
an issue at <https://github.com/yeokm1/usb-wdm-free/issues>, also when you
got the driver with xHCI98's download. Say which driver and version, which
Windows, which USB host controller driver (Windows' own, NUSB's or
xHCI98's), the device (its name and its VID/PID if you can find them), and
what happened. Problems with xHCI98 itself go to xHCI98's own issues page.

## Layout

| Path | What |
|---|---|
| `src/<class>/<package>/` | A driver's source code, its INFs and its build. Classes: `video`, `audio`, `net`, `serial`, `hid`, `print` (printers), `storage` (mass storage, floppies), `generic` (drivers for no one device class, such as `libusb0`) and `install` (the installer) |
| `src/install/wdminst/` | The installer's helper program, `WDMINST.EXE` and `WDMIN64.EXE` (installer deferred) |
| `src/<class>/common/` | Source shared by the packages of one class (for example `src/audio/common/`, `src/net/common/`), compiled into each package's binary; never released on its own |
| `free/<class>/<package>/<systems>/` | The compiled driver as released, one folder per operating system or range of systems, each one complete install; folder names below |
| `docs/design/` | Design documents, one per planned driver or family, the survey of candidates and the survey of existing drivers |
| `SHA256.TXT` | The hash of every file under `free/` |
| `tools/`, `vm/`, `out/` | Git-ignored: the driver kits and the QEMU test guests, installed per host as [docs/toolchain.md](toolchain.md) records, and build output. On the author's host the guest images live in `E:\usb-wdm-free\vm\` instead of `vm/` |
| `NOTICE.md` | Every file that carries code adapted from another project, with its origin and licence |

The layout under `free/` mirrors `usb-wdm-nonfree`'s `non-free/`.
Names are kept to 8.3 (eight characters, a dot, three), so they read the
same in DOS and on Windows 98's real-mode side.

### `<systems>` folder names

One scheme for every package: `w`, then the first system, then `-` and the
last system when the folder serves a range, then `-x86` or `-x64` when the
folder holds one architecture's binary for systems that come in both (XP
x64, and Vista onward). The systems are written `98se`, `me`, `2k`, `xp`,
`v` (Vista), `7` and `8`; `xp` alone means 32-bit XP. A folder with no
architecture suffix holds 32-bit binaries for systems up to XP, or no
binary at all (an INF only).

| Folder | Systems |
|---|---|
| `w98se`, `wme`, `w2k`, `wxp` | One system each (`wxp`: XP 32-bit) |
| `w98se-me` | 98 SE and ME |
| `w2k-xp` | 2000 and XP 32-bit |
| `wxp-x64` | XP x64 |
| `wv-7-x86`, `wv-7-x64` | Vista and 7, 32-bit or 64-bit |
| `wv-8-x86`, `wv-8-x64` | Vista, 7 and 8, 32-bit or 64-bit |
| `w7-x64`, `w8-x86` | One system and one architecture |
| `w2k-8`, `wxp-8` | 2000 (or XP) to 8, an INF only, both architectures |

## Where the code comes from

- Published specifications: the USB 2.0 specification and the USB-IF
  device class specifications (Video Class, Audio Class 2.0, MIDI, CDC).
- Microsoft's published driver kit documentation and headers (the Windows
  98, 2000 and XP DDKs, and the WDKs for Vista through Windows 8). The DDK
  sample sources are not read.
- The export tables of Microsoft's in-box binaries, read for function
  names and ordinals only, to know what each system offers a driver.
- xHCI98's own code, by the same author, from files tagged
  GPL-2.0-or-later.
- Code from open-source projects (Linux, ReactOS and others) under
  GPL-2.0-or-later or a licence already used here alongside it,
  adapted with its copyright notices and licence tags kept, each such file
  listed in `NOTICE.md`. From files under GPL-2.0 only, facts only (ids,
  register values, protocol behaviour), no code.

Never a Microsoft or hardware vendor's driver code, its disassembly, or
code under a licence that does not permit this use.

## Installing

Point the Add New Hardware Wizard, or Update Driver on the device, at the
`free/` folder for your device and your system.

`STAGE.TXT` lists every INF under `free/` for xHCI98's installer: `STAGE`
lines it may place where Windows finds them by itself, `COPY` lines it
only copies. `noaud`'s `NOAUD98.INF` is `COPY`: install it by hand, only
if plugging a webcam freezes 98 SE (its `README.TXT`).

## Checking the files

```sh
sha256sum -c SHA256.TXT
```

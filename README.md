# usb-wdm-free

Free (GPL-2.0-or-later) WDM USB class drivers for Windows 98 SE to Windows 8,
source and built drivers together. They use Windows' standard USB driver
interface, so they run under Windows' own USB 2.0 drivers, NUSB's on
98 SE, or [xHCI98](https://github.com/yeokm1/xhci98), whose releases carry
them in their `-free` download.

Device makers' own drivers for old Windows, collected as released, are in
the companion repository [usb-wdm-nonfree](https://github.com/yeokm1/usb-wdm-nonfree).

These drivers are experimental. They may not work with every device and can make Windows unstable. Report problems at https://github.com/yeokm1/usb-wdm-free.

## What each driver is for

| Driver | Device class | Windows |
|---|---|---|
| `uac1` | USB Audio Class 1.0, playback and recording, with volume and mute; for testing 98 SE in a virtual machine; on a real PC keep Windows' own driver ([why](#uac1-is-for-testing-in-a-virtual-machine)) | 98 SE |
| `uac2` | USB Audio Class 2.0, playback and recording, with volume and mute | 98 SE to 8; x64 from XP |
| `usbmidi` | USB MIDI 1.0 | 98 SE to 8; x64 from XP (1) |
| `uvc98` | USB Video Class 1.0, 1.1 and 1.5 webcams, with the `uvcdec` MJPEG codec | 98 SE, ME, 2000 |
| `usbcom` | USB CDC-ACM serial, as COM ports | 98 SE, ME; 2000 to 8 and x64 as an INF for Windows' `usbser.sys` |
| `cdcnet` | USB CDC-ECM, CDC-NCM and RNDIS networking | 98 SE to 7; x64 from XP (2) |
| `rtl815x` | Realtek RTL8152, 8153, 8156, 8157 and 8159 USB Ethernet | 98 SE, ME, 2000. Newer chips also XP to 7, x64 too (6) |
| `ax88179` | ASIX AX88179 and AX88178A USB Gigabit Ethernet | 98 SE, ME, 2000 |
| `ax88772` | ASIX AX88772B and AX88772C, CoreChips SR9800 USB Fast Ethernet | 98 SE, ME; the SR9800 also 2000 to 7, x64 from XP (7) |
| `sr9700` | CoreChips SR9700 USB Ethernet | 98 SE to 7; x64 from XP |
| `xpad98` | Xbox-compatible game pads | 98 SE to Vista; x64 on XP and Vista (3) |
| `usbflop` | USB floppy drives (UFI), an INF for Windows' `usbstor.sys` | 98 SE to 8; x64 from XP (4) |
| `noaud` | `NOAUD98.INF`, opt-in: no driver for a listed webcam's microphone | 98 SE |
| `libusb0` | libusb-win32 0.1.12.2, driver and library | 98 SE, ME |
| `usbprn` | USB printers and USB-to-parallel cables, as printer ports USB001, USB002, ... | 98 SE, ME (5) |

1. ME, and XP onward: listed devices only; Windows' own driver serves the rest.
2. RNDIS on XP to 7 is an INF for Windows' own driver.
3. XP and Vista: Xbox One and Series pads only. Xbox 360-type pads outside
   the `x360c` driver's list: 98 SE to 2000. The Xbox 360 wireless
   receiver: 98 SE and ME.
4. 98 SE needs a USB storage driver set (such as NUSB's) installed first.
5. 98 SE: a driver and a port monitor. ME: an INF for ME's own USB printing
   driver. Add the printer by hand (Add Printer, its model, port USB001); most
   current inkjets and host-based printers have no Windows 98 or ME driver.
6. XP and Vista: the RTL8153D, 8156 and 8156B. XP, Vista and 7: the
   RTL8157 and 8159. Realtek's own drivers serve the other chips there.
7. The AX88772B and AX88772C on 2000 and later: ASIX's own driver, in
   usb-wdm-nonfree. The AX88772 and AX88772A: ASIX's driver on every
   Windows.

Windows 8.1 and 10 are not targets and were not evaluated.

### `uac1` is for testing in a virtual machine

On a real PC, keep Windows 98 SE's own USB audio driver ("USB Audio
Device", `usbaudio.sys`), which xHCI98's installer leaves in place. `uac1`
is for running 98 SE in a virtual machine (QEMU), where Windows' driver
stops Windows as soon as it plays to QEMU's emulated USB audio device and
`uac1` plays it.

On a real PC `uac1` has no advantage: playback, recording, volume and mute
work with both; unplugging while it plays can stop Windows with both, and
the first plug of a boot freezes as often with both (Known issues). From
0.1.1.0 it also runs the layouts Windows' driver runs: 8-bit, floating point,
A-law and mu-law devices, MaxPacketsOnly endpoints and microphones that need a
synchronisation endpoint. Not supported: MaxPacketsOnly recording from an
asynchronous device, or in 8-bit, A-law or mu-law; such a device loads under
`uac1` with no sound device (no hang), and Update Driver back to "USB Audio
Device" gives it Windows' driver again.

In Device Manager `uac1` names the device "USB Audio 1.0 (uac1)"; Windows'
own driver names it "USB Audio Device".

## How each driver was evaluated

<a name="tested-by-the-author"></a>
`uac2`, `uvc98` and `ax88179` have run on a real PC with a real device
under Windows 98 SE (the owner, reported 2026-10-29) (l), and `uac1` with
the real Sound Blaster Play! 2 (the owner's bench, 2026-11-09) (r). Every
other cell is a virtual machine.
"Real device (VM)" means a real device passed through to Windows running
in a virtual machine; "Simulator only" means a simulated device. Cells are
32-bit Windows unless they say x64; "-" means the driver is not meant for
that Windows.

| Driver | 98 SE | ME | 2000 | XP | Vista | 7 | 8 |
|---|---|---|---|---|---|---|---|
| `uac1` | Real PC (r); real device (VM); simulator (o) | - | - | - | - | - | - |
| `uac2` | Real PC (l); simulator (a) | Simulator only | Not evaluated | Real device (VM) (b); real device (VM), x64 (q) | Simulator only; real device (VM), x64 (q) | Real device (VM); real device (VM), x64 (q) | Real device (VM), x64 (q) |
| `usbmidi` | Simulator only | Simulator only | Simulator only | Simulator only | Not evaluated | Real device (VM), open and close only | Not evaluated |
| `uvc98` | Real PC (l); real device (VM) (c) | Not evaluated | Real device (VM) (d) | - | - | - | - |
| `usbcom` | Simulator only | Simulator only | Simulator only (e) | Real device (VM), open and close only | Not evaluated | Simulator only, x64 | Not evaluated |
| `cdcnet` | Simulator only | Simulator only | Simulator only | Simulator only (f) | Not evaluated | Simulator only, x64 (f) | - |
| `rtl815x` | Real device (VM), RTL8157 (p); simulator (j) | Simulator only (j) | Real device (VM), RTL8157 (p); simulator (j) | Real device (VM), RTL8157 (p); simulator, x86 and x64 (k) | Simulator only, x86 and x64 (k) | Simulator only, x86 and x64 (k) | - |
| `ax88179` | Real PC (l); real device (VM) | Real device (VM) | Not evaluated | - | - | - | - |
| `ax88772` | Simulator only | Simulator only | Simulator only, SR9800 (m) | Simulator only, SR9800, x86 and x64 (m) | Not evaluated | Simulator only, SR9800, x86 and x64 (m) | - |
| `sr9700` | Simulator only (n) | Simulator only | Simulator only (m) | Simulator only, x86 and x64 (m) | Not evaluated | Simulator only, x86 and x64 (m) | - |
| `xpad98` | Simulator only (g) | Simulator only | Simulator only | Simulator only, x86 and x64 | Simulator only, x86 and x64 | - | - |
| `usbflop` | Real device (VM) | Real device (VM) | Real device (VM) (h) | Real device (VM) (h) | Not evaluated | Real device (VM), x64 (h) | Not evaluated |
| `noaud` | Real device (VM) | - | - | - | - | - | - |
| `libusb0` | Simulator only (i) | Not evaluated | - | - | - | - | - |
| `usbprn` | Simulator only | Simulator only | - | - | - | - | - |

- (a) Under NUSB's EHCI, 8-channel playback stalls and Full Speed was not run; the other layouts pass.
- (b) Playback works; recording fails (half speed, no sound).
- (c) 160x120 works under xHCI98, 320x240 fails; under NUSB's EHCI no frame arrived in the VM.
- (d) Up to 320x240 under Windows' EHCI (640x480 fails in the VM). Under xHCI98 the real camera gives a picture at 160x120 in QEMU, but a later session or 320x240 fails with errors the camera reports. A simulated camera works at every size, so this is open in QEMU's USB passthrough or xHCI98, not in uvc98; a real PC settles it. On Windows 2000's own USB stack and under NUSB on 98 SE the whole camera binds to uvc98 and its microphone has no driver.
- (e) Windows 2000's `usbser.sys` stops reading after a zero-length packet from the device.
- (f) ECM passes; RNDIS through Windows' own driver failed on the simulated device.
- (g) The Xbox 360 wireless receiver fails under xHCI98 3.1.0.0 and works under 3.2.0.0.
- (h) The real drive was recognised but no disk was read; reading passed with a simulated drive.
- (i) Install and device listing only; no data transfers.
- (j) The RTL8153D, 8156, 8156B, 8157 and 8159 were run on 98 SE, ME and 2000 under xHCI98, and the 8156 and 8156B also on 98 SE under NUSB's EHCI. No firmware patch is loaded. Without its patch the RTL8159 links at 5 Gb/s at most.
- (k) The newer chips only (footnote 6), at High Speed under Windows' EHCI. On XP and XP x64 the 8156 and 8156B also ran under xHCI98, at 2.5 Gb/s.
- (l) The owner's report: each ran on a real PC under Windows 98 SE with a real device. The device, the host controller driver and what was exercised are not recorded here yet.
- (m) Under xHCI98.
- (n) Under xHCI98 only. The SR9700 is a USB 1.1 device, and NUSB's EHCI does not take one; 98 SE's own USB 1.1 stack was not run.
- (o) Under xHCI98: the real Logi USB Headset (`046D:0A64`), passed through: install, playback at 48 and 44.1 kHz, volume and mute, recording at 48 kHz (`docs/testing/2026-11-07-uac1-98se.md`); a simulator replaying the real Creative Sound Blaster Play! 2 (`041E:323D`) from its captured descriptors (the real Play! 2 itself on a real PC: (r)); and simulated headsets and DACs. Under 98 SE's own USB 1.1 driver: a simulated headset. Unplugging a device while it plays can stop Windows (Known issues).
- (p) A real RTL8157 (`0BDA:8157`) under xHCI98 at 5 Gb/s: link, pings, two-way TCP with every byte checked, unplug under traffic and replug, and Device Manager's disable and enable (`docs/testing/2026-11-07-rtl8157-hardware.md`, `2026-11-07-rtl8157-performance.md`).
- (q) A real Creative Sound Blaster X4 (`041E:3278`) and X1 (`041E:3286`) on XP x64, Vista x64, 7 x64, 8 x64 and 8.1 x64: playback on both, and recording on the X1 (`docs/testing/2026-11-05-uac2-build26-nt6x64.md`).
- (r) The owner's bench, 2026-11-09: the real Creative Sound Blaster Play! 2 (`041E:323D`) on a ThinkPad P14s Gen 1 under Windows 98 SE, xHCI98 3.4.2.0 and `uac1` 0.1.9: playback clean at 48 and 44.1 kHz (a 1 kHz tone, left and right, a 50 Hz to 15 kHz sweep), volume and mute, and recording from a microphone clean at 44.1 kHz, 16-bit stereo (Sound Recorder's default of 22.05 kHz, 8-bit mono is noisy by its format). Unplugged and plugged in again with nothing playing (the same port and another): still the preferred device, and playback clean.

Each run, with its device, host controller driver, date and result, is in
[docs/status.md](docs/status.md) (with the layout, sources and install
notes) and [docs/testing/hcd-matrix.md](docs/testing/hcd-matrix.md).

## Known issues

- **98 SE may freeze when the first USB audio device is plugged in after
  a boot** (an audio or MIDI interface, a headset, a webcam with a
  microphone), with `uac1`, `uac2` and Windows' own audio driver alike.
  Plug the device in before switching on and leave it in; if Windows
  freezes, restart with it plugged in. For a webcam whose microphone you do not need, install
  `NOAUD98.INF`.
- **On Windows 98 SE, stop audio playback or recording before
  unplugging a USB audio device.** Unplugging while it plays can stop
  Windows; unplugging while it records can leave the recording program
  not responding until the next restart. This happens with Windows' own
  USB audio driver too.
- **98 SE: after a USB MIDI device is unplugged**, MIDI devices plugged
  in afterwards cannot be opened until Windows is restarted.
- **98 SE: a USB network adapter plugged into a new port or at a new
  speed is a new adapter to Windows**, and after five such adapters a
  new one gets no TCP/IP (Windows' own limit). Remove unused adapters
  in Network properties, or add TCP/IP to the new one.
- **XP and XP x64 install their own USB audio driver for a `uac2` device
  first** ("USB Audio Device"). Changing it to `uac2` then hangs, and so
  do disabling it and shutting down after unplugging it; after a restart
  (reset the PC if the shutdown hangs) `uac2` takes the device. To change
  it without the hang (read on 32-bit XP): unplug the device, restart Windows, run
  `cmd /c "set devmgr_show_nonpresent_devices=1&& devmgmt.msc"`, choose
  View, Show hidden devices, update the greyed "USB Audio Device" to
  `uac2`, then plug the device in. Under xHCI98 from 3.4.0.0 Windows'
  driver no longer takes such a device. From 0.1.5 the co-installer
  avoids this on 2000 to 7: see Installing.
- **2000: enabling a disabled `uac2` device can stop Windows** (STOP
  0x0000001E in Windows' `usbhub20.sys`). Unplug the device instead of
  disabling it. From 0.1.6 changing a device's driver to `uac2` on 2000
  asks for a restart and no longer stops.

## Installing

The built drivers and their INFs are committed in this repository under
`free/`, so no build is needed: download the whole repository (on GitHub,
Code > Download ZIP, or the `v0.1.10` tag's source zip) and unzip it.
Then point the Add New Hardware Wizard, or Update Driver, at the
driver's folder under `free/` for your Windows (release 0.1.10). `SHA256.TXT` lists
every file's hash. The x86 drivers are unsigned. The x64 drivers carry
the project's own test signature: on Vista and later x64, add
`usb-wdm-free-test.cer` to the Trusted Root and Trusted Publishers
stores and turn test signing on (`bcdedit /set testsigning on`).
0.1.9 and 0.1.10 are signed with the test certificate
`CN=usb-wdm-free test signing SHA-1 (not trusted)`, thumbprint
`7A3C6E6289EC46E6E128D87BB54E87196B824959`; 7 x64 needs no update for
it. Trust the `usb-wdm-free-test.cer` of this release; 0.1.4's
certificate (`E6A2749B78E9E4074260CF79972A57EEB6E76C0A`) no longer
applies.

`uac2` on 2000 to 7: before plugging a UAC 2.0 device in, put `uac2`
where Windows looks (`pnputil -a uac2wdm.inf` on Vista and 7; its
folder on `DevicePath` on 2000 and XP) and install the folder's
`UACCI.INF` once (right-click, Install). Then the first plug installs
`uac2` and not Windows' USB Audio 1.0 driver. 8 and 8.1 install `uac2`
without the co-installer; their x64 `pnputil` wants a signed catalog,
which this package has none of
([release notes](docs/release-notes.md), 0.1.5).

`usbmidi` on 2000: Windows 2000 gives a class-compliant MIDI keyboard
or interface to its own audio driver, which can stop Windows. `usbmidi`
takes such a device over only if its INF lists it. To add yours, before
you plug it into the 2000 PC:

1. On a PC with Windows XP or later, plug the device in. In Device
   Manager, under Sound, video and game controllers, open it (on XP:
   "USB Audio Device"), Details, Hardware Ids. Copy the line of the form
   `USB\VID_xxxx&PID_yyyy&MI_nn` (the one without `REV_`). If no line
   has `&MI_`, these steps do not apply to the device.
2. Copy `free/audio/usbmidi/w2k/` to the 2000 PC, say to `C:\USBMIDI`.
   In its `usbmidi.inf`, under `[Models]`, add your line after the
   UM-ONE line, in this form:

   ```
   %UmOneTab%=UM,USB\VID_0582&PID_0168&MI_00
   %Manual%=UM,USB\VID_xxxx&PID_yyyy&MI_nn
   ```

3. Put the folder on `DevicePath`: run `regedt32` (not Regedit, which
   can change the value's type), open
   `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion` and
   add `;C:\USBMIDI` to the end of `DevicePath`, keeping what is there.
4. Plug the device in. Answer Yes to the signature question, and
   restart if Windows asks.

Do not plug the device in before step 3: Windows 2000's audio driver
takes it and can stop Windows. If that happens, restart without the
device and do the steps. The edited INF no longer matches `SHA256.TXT`.

`uac1`, `uac2` and `usbmidi` on Windows 98 SE: if no WDM sound device was installed before, Windows sets up its own audio components once and may ask for the Windows 98 Second Edition CD-ROM; point it at the CD's `\WIN98` folder.

`uac1` on 98 SE is for a virtual machine (above). Without it, Windows' own
USB audio driver keeps every USB Audio 1.0 device, as before. To use `uac1`, point the
wizard (or Update Driver, for a device Windows' driver already has) at
`free/audio/uac1/w98se/`. After that, USB Audio 1.0 devices plugged in
for the first time get `uac1` too. To go back for one device, use Update
Driver and pick Windows' own USB audio driver from the list.

`usbprn` on 98 SE and ME: plug the printer in, point the wizard at
`free/print/usbprn/w98se/` or `wme/`, then add the printer with Add
Printer: its model and the port `USB001` (`USB002` for a second one).

`free/tools/camcheck/` holds CamCheck (`CAMCHECK.EXE`), a webcam preview
for 98 SE, ME and 2000 that works with `uvc98` and any other Video for
Windows capture driver; run it, nothing is installed.

## License

GPL-2.0-or-later: the GNU General Public License version 2
([LICENSE](LICENSE)) or, at your option, any later version. Code adapted
from other projects keeps its own licence and is listed in
[NOTICE.md](NOTICE.md).

The documentation (every Markdown file outside `src/video/uvcdec/turbo/`,
this README included) is licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), not the GPL.
[LICENSE-docs.md](LICENSE-docs.md) gives the scope, what is excluded and how
to credit. Third-party material quoted in the documentation keeps its own
terms.

If you use anything from this project in your work, please credit this
repository. I would be interested to know how it has helped.

## AI usage

This project was developed with substantial help from AI coding agents:

- Claude Fable 5.1 for planning
- Claude Opus 5.5 for coding
- Codex GPT 6 Astra as an adversarial reviewer

The agents wrote and revised code, tests and documentation under the guidance in [AGENTS.md](AGENTS.md).

## Changelog

Each tag is a release. [docs/release-notes.md](docs/release-notes.md) has the detail.

- **v0.1.8** (2026-11-05):
  - `uac2` 0.1.8.26 pops far less under heavy CPU load (a delay of more than 16 ms in the USB host controller's driver still interrupts the sound): more sound is queued (`Uac2QueueMs`, 32 ms by default) and its worker runs at real-time priority (`Uac2Realtime`). The volume tops out at 0 dB, a likely cause of distortion on some devices (`Uac2VolAboveZero` 1 offers the whole range again). Each device is named by its own USB product name.
  - `uac2`, `cdcnet` and `usbflop` are also staged on XP x64, and `ax88179` on 2000.
- **v0.1.7** (2026-11-01):
  - `usbprn` 0.1.0.0, new: USB printers and USB-to-parallel printer cables on 98 SE and ME, as printer ports USB001, USB002 and so on.
- **v0.1.6** (2026-10-31):
  - `uac2` 0.1.6.0 fixes garbled sound after seeking, and on Windows 2000 asks for a restart at install, so changing a device's driver to `uac2` no longer stops 2000.
- **v0.1.5** (2026-10-30):
  - `uac2` 0.1.5.0 gives each output and input of a device its own Windows sound device, starts on 98 SE's and ME's own USB 1.1 stack (Full Speed), and ships the co-installer `UACCI1.DLL` with `UACCI.INF` (in the 2000 to 8.1 folders; needed on 2000 to 7): installed by hand before the first plug, it keeps Windows' USB Audio 1.0 driver off a UAC 2.0 device.
  - `usbcom` 0.1.5.0 adds chip modules for the FTDI FT-X, FT232H and FT4232H, the Prolific PL2303 and the Silicon Labs CP2105 and CP2108 on 98 SE and ME.
- **v0.1.4** (2026-10-28):
  - `rtl815x` 0.2.0.0 adds the Realtek RTL8153D, RTL8156, RTL8156B, RTL8157 and RTL8159. It is now also copied for XP to 7, 32-bit and x64.
  - `uac2` 0.1.4.0 adds playback volume and mute. Windows' software MIDI synth can play through the device.
  - Every x64 image is signed with a new test certificate (thumbprint `E6A2749B78E9E4074260CF79972A57EEB6E76C0A`).
  - The free and non-free `rtl815x` exclude each other on every system.
- **v0.1.3** (2026-10-26): class drivers match their USB class where Windows has no driver of its own. `usbcom` now does on 98 SE, ME and 2000. `cdcnet`'s RNDIS INF leaves Windows 7's own RNDIS match alone.
- **v0.1.2** (2026-10-26): the licence is now GPL-2.0-or-later and every package is re-released. `rtl815x` has a rewritten chip core.
- **v0.1.1** (2026-10-25): `ax88179` for ASIX AX88179 and AX88178A Gigabit Ethernet adapters on 98 SE, ME and 2000. `uac2` is staged on ME too.
- **v0.1.0** (2026-10-24): the first release: `uac2`, `usbmidi`, `uvc98`, `usbcom`, `cdcnet`, `rtl815x`, `xpad98`, `usbflop` and `libusb0`, and the CamCheck webcam preview.

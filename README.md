# usb-wdm-free

Free (GPL-2.0-or-later) WDM USB class drivers for Windows 98 SE to Windows 8,
source and built drivers together. They use Windows' standard USB driver
interface, so they run under Windows' own USB 2.0 drivers, NUSB's on
98 SE, or [xHCI98](https://github.com/yeokm1/xhci98), whose releases carry
them in their `-free` download.

These drivers are experimental. They may not work with every device and can make Windows unstable. Report problems at https://github.com/yeokm1/usb-wdm-free.

## What each driver is for

| Driver | Device class | Windows |
|---|---|---|
| `uac2` | USB Audio Class 2.0, playback and recording, with volume and mute | 98 SE to 8; x64 from XP |
| `usbmidi` | USB MIDI 1.0 | 98 SE to 8; x64 from XP (1) |
| `uvc98` | USB Video Class 1.0, 1.1 and 1.5 webcams, with the `uvcdec` MJPEG codec | 98 SE, ME, 2000 |
| `usbcom` | USB CDC-ACM serial, as COM ports | 98 SE, ME; 2000 to 8 and x64 as an INF for Windows' `usbser.sys` |
| `cdcnet` | USB CDC-ECM, CDC-NCM and RNDIS networking | 98 SE to 7; x64 from XP (2) |
| `rtl815x` | Realtek RTL8152, 8153, 8156, 8157 and 8159 USB Ethernet | 98 SE, ME, 2000. Newer chips also XP to 7, x64 too (6) |
| `ax88179` | ASIX AX88179 and AX88178A USB Gigabit Ethernet | 98 SE, ME, 2000 |
| `xpad98` | Xbox-compatible game pads | 98 SE to Vista; x64 on XP and Vista (3) |
| `usbflop` | USB floppy drives (UFI), an INF for Windows' `usbstor.sys` | 98 SE to 8; x64 from XP (4) |
| `noaud` | `NOAUD98.INF`, opt-in: no driver for a listed webcam's microphone | 98 SE |
| `libusb0` | libusb-win32 0.1.12.2, driver and library | 98 SE, ME |
| `usbprn` | USB printers and USB-to-parallel cables (not built yet) | 98 SE (5) |

1. ME, and XP onward: listed devices only; Windows' own driver serves the rest.
2. RNDIS on XP to 7 is an INF for Windows' own driver.
3. XP and Vista: Xbox One and Series pads only. Xbox 360-type pads outside
   the `x360c` driver's list: 98 SE to 2000. The Xbox 360 wireless
   receiver: 98 SE and ME.
4. 98 SE needs a USB storage driver set (such as NUSB's) installed first.
5. ME only if its own USB printing proves unable to print.
6. XP and Vista: the RTL8153D, 8156 and 8156B. XP, Vista and 7: the
   RTL8157 and 8159. Realtek's own drivers serve the other chips there.

Windows 8.1 and 10 are not targets and were not evaluated.

## How each driver was evaluated

<a name="tested-by-the-author"></a>
No driver has been evaluated on a real PC with a real connection yet.
"Real device (VM)" means a real device passed through to Windows running
in a virtual machine; "Simulator only" means a simulated device. Cells are
32-bit Windows unless they say x64; "-" means the driver is not meant for
that Windows.

| Driver | 98 SE | ME | 2000 | XP | Vista | 7 | 8 |
|---|---|---|---|---|---|---|---|
| `uac2` | Simulator only (a) | Simulator only | Not evaluated | Real device (VM) (b) | Simulator only | Real device (VM) | Not evaluated |
| `usbmidi` | Simulator only | Simulator only | Simulator only | Simulator only | Not evaluated | Real device (VM), open and close only | Not evaluated |
| `uvc98` | Real device (VM) (c) | Not evaluated | Real device (VM) (d) | - | - | - | - |
| `usbcom` | Simulator only | Simulator only | Simulator only (e) | Real device (VM), open and close only | Not evaluated | Simulator only, x64 | Not evaluated |
| `cdcnet` | Simulator only | Simulator only | Simulator only | Simulator only (f) | Not evaluated | Simulator only, x64 (f) | - |
| `rtl815x` | Simulator only (j) | Simulator only (j) | Simulator only (j) | Simulator only, x86 and x64 (k) | Simulator only, x86 and x64 (k) | Simulator only, x86 and x64 (k) | - |
| `ax88179` | Real device (VM) | Real device (VM) | Not evaluated | - | - | - | - |
| `xpad98` | Simulator only (g) | Simulator only | Simulator only | Simulator only, x86 and x64 | Simulator only, x86 and x64 | - | - |
| `usbflop` | Real device (VM) | Real device (VM) | Real device (VM) (h) | Real device (VM) (h) | Not evaluated | Real device (VM), x64 (h) | Not evaluated |
| `noaud` | Real device (VM) | - | - | - | - | - | - |
| `libusb0` | Simulator only (i) | Not evaluated | - | - | - | - | - |
| `usbprn` | Not built | Not built | - | - | - | - | - |

- (a) Under NUSB's EHCI, 8-channel playback stalls and Full Speed was not run; the other layouts pass.
- (b) Playback works; recording fails (half speed, no sound).
- (c) 160x120 works under xHCI98, 320x240 fails; under NUSB's EHCI no frame arrived in the VM.
- (d) Up to 320x240 under Windows' EHCI (640x480 fails in the VM). Under xHCI98 the real camera gives a picture at 160x120 in QEMU, but a later session or 320x240 fails with errors the camera reports. A simulated camera works at every size, so this is open in QEMU's USB passthrough or xHCI98, not in uvc98; a real PC settles it. On Windows 2000's own USB stack and under NUSB on 98 SE the whole camera binds to uvc98 and its microphone has no driver.
- (e) Windows 2000's `usbser.sys` stops reading after a zero-length packet from the device.
- (f) ECM passes; RNDIS through Windows' own driver failed on the simulated device.
- (g) The Xbox 360 wireless receiver fails under xHCI98 3.1.0.0 and works under 3.2.0.0.
- (h) The real drive was recognised but no disk was read; reading passed with a simulated drive.
- (i) Install and device listing only; no data transfers.
- (j) The RTL8153D, 8156, 8156B, 8157 and 8159 were run on 98 SE, ME and 2000 under xHCI98 only. No firmware patch is loaded. Without its patch the RTL8159 links at 5 Gb/s at most.
- (k) The newer chips only (footnote 6). Every leg ran at High Speed. Faster links were not run.

Each run, with its device, host controller driver, date and result, is in
[docs/status.md](docs/status.md) (with the layout, sources and install
notes) and [docs/testing/hcd-matrix.md](docs/testing/hcd-matrix.md).

## Known issues

- **98 SE may freeze when the first USB audio device is plugged in after
  a boot** (an audio or MIDI interface, a headset, a webcam with a
  microphone), with Windows' own audio driver too. Plug the device in
  before switching on and leave it in; if Windows freezes, restart with
  it plugged in. For a webcam whose microphone you do not need, install
  `NOAUD98.INF`.
- **98 SE: after a USB MIDI device is unplugged**, MIDI devices plugged
  in afterwards cannot be opened until Windows is restarted.
- **XP x64 installs its own USB audio driver for a `uac2` device first.**
  Changing it to `uac2` can hang. If it does, restart Windows.

## Installing

The built drivers and their INFs are committed in this repository under
`free/`, so no build is needed: download the whole repository (on GitHub,
Code > Download ZIP, or the `v0.1.4` tag's source zip) and unzip it.
Then point the Add New Hardware Wizard, or Update Driver, at the
driver's folder under `free/` for your Windows (release 0.1.4). `SHA256.TXT` lists
every file's hash. The x86 drivers are unsigned. The x64 drivers carry
the project's own test signature: on Vista and later x64, add
`usb-wdm-free-test.cer` to the Trusted Root and Trusted Publishers
stores and turn test signing on (`bcdedit /set testsigning on`).
0.1.4 is signed with a new test certificate. Trust the
`usb-wdm-free-test.cer` of this release, not an earlier one. Its
thumbprint is `E6A2749B78E9E4074260CF79972A57EEB6E76C0A`.

`free/tools/camcheck/` holds CamCheck (`CAMCHECK.EXE`), a webcam preview
for 98 SE, ME and 2000 that works with `uvc98` and any other Video for
Windows capture driver; run it, nothing is installed.

## License

GPL-2.0-or-later: the GNU General Public License version 2
([LICENSE](LICENSE)) or, at your option, any later version. Code adapted
from other projects keeps its own licence and is listed in
[NOTICE.md](NOTICE.md).

## AI usage

This project was developed with substantial help from AI coding agents:

- Claude Fable 5.1 for planning
- Claude Opus 5.5 for coding
- Codex GPT 6 Astra as an adversarial reviewer

The agents wrote and revised code, tests and documentation under the guidance in [AGENTS.md](AGENTS.md).

## Changelog

Each tag is a release. [docs/release-notes.md](docs/release-notes.md) has the detail.

- **v0.1.4** (2026-10-28):
  - `rtl815x` 0.2.0.0 adds the Realtek RTL8153D, RTL8156, RTL8156B, RTL8157 and RTL8159. It is now also copied for XP to 7, 32-bit and x64.
  - `uac2` 0.1.4.0 adds playback volume and mute. Windows' software MIDI synth can play through the device.
  - Every x64 image is signed with a new test certificate (thumbprint `E6A2749B78E9E4074260CF79972A57EEB6E76C0A`).
  - The free and non-free `rtl815x` exclude each other on every system.
- **v0.1.3** (2026-10-26): class drivers match their USB class where Windows has no driver of its own. `usbcom` now does on 98 SE, ME and 2000. `cdcnet`'s RNDIS INF leaves Windows 7's own RNDIS match alone.
- **v0.1.2** (2026-10-26): the licence is now GPL-2.0-or-later and every package is re-released. `rtl815x` has a rewritten chip core.
- **v0.1.1** (2026-10-25): `ax88179` for ASIX AX88179 and AX88178A Gigabit Ethernet adapters on 98 SE, ME and 2000. `uac2` is staged on ME too.
- **v0.1.0** (2026-10-24): the first release: `uac2`, `usbmidi`, `uvc98`, `usbcom`, `cdcnet`, `rtl815x`, `xpad98`, `usbflop` and `libusb0`, and the CamCheck webcam preview.

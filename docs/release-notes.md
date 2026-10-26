# Release notes

Per package, newest first. A release is a commit and a tag
(`<package>-<version>`); its files are under `free/` and listed in
`SHA256.TXT`, and `STAGE.TXT` says which INFs xHCI98's installer may stage.

## Release 0.1.2 (2026-10-26): GPL-2.0-or-later

The repository moved from GPL-2.0-only to GPL-2.0-or-later (owner,
2026-10-25 and 2026-10-26). Every package whose released files carried
the old licence text is re-released: `uac2`, `usbmidi`, `cdcnet`,
`usbcom`, `usbflop`, `uvc98`, `ax88179` and `rtl815x` at 0.1.2.0
(`DriverVer` 10/26/2026), `noaud` at 1.0.1.0, and CamCheck's texts. Their
INFs change only in the licence line and `DriverVer`; their images are
0.1.1's, byte for byte (the six x64 images still the owner's signed
0.1.0 builds), except `rtl815x`. `xpad98` and `libusb0` are unchanged.

- **`rtl815x` 0.1.2.0**: its chip core is rewritten from the chips'
  register facts (`docs/design/ethernet.md` section 6.4); against the
  earlier core, under one register model, every control request, sleep
  and result is the same. Simulator only: RTL8153 under xHCI98 and NUSB's
  EHCI, RTL8153B and RTL8152 under xHCI98 on 98 SE, DHCP and ping
  ([rewrite](testing/2026-10-25-rtl815x-rewrite.md),
  [0.1.2](testing/2026-10-26-rtl815x-0.1.2.md)). Still copy only in
  `STAGE.TXT`.
- **Status**: unchanged for every other package; "Built, not tested" as
  before. [Release record](testing/2026-10-26-release-0.1.2.md).
- **Signing**: no new signature: the x64 images are the signed 0.1.0
  ones; x86 unsigned; no catalog.

## Release 0.1.1 (2026-10-25): `ax88179`

Release 0.1.0 plus `ax88179`, the ASIX AX88179 / AX88178A USB Gigabit
Ethernet driver (`AX179F.SYS`, NDIS 5.0 with a WDM lower edge), in
`free/net/ax88179/w98se-me/` (`AX179F98.INF`) and
`free/net/ax88179/w2k/` (`AX179F.INF`). Every other file under `free/`
is 0.1.0's, byte for byte. `ax88179` is version 0.1.1 (`DriverVer`
10/25/2026,0.1.1.0); the other drivers keep their 0.1.0 versions.
`uac2`'s `w98se-me` INF is now staged on ME too (owner, 2026-10-25),
after its first ME leg passed: the released INF in ME's INF folder, a
simulated X4 (`uacemu.py x4full`) under xHCI98, playback at 48 and
44.1 kHz and capture with the simulator's tone. `usbmidi`'s ME INF
stays copy only: it lists no device (`USBMIDI_BY_HAND`), and ME's own
`usbaudio.sys` serves USB MIDI.

- **Status**: "Built, not tested", as every folder: the owner's AX88179
  passed through to Windows in QEMU, never on a real PC. On 98 SE under
  xHCI98 and under NUSB's EHCI, and on ME under xHCI98: install, DHCP,
  pings to 4000 bytes, removal under traffic and replug passed, with
  trace builds made before the settle cycle at start. The settle cycle
  was then run on 98 SE only, under both, with no frame carrying an
  FCS; the released source itself (with the FCS report's final form)
  ran under NUSB's EHCI only. `STAGE.TXT` stages `AX179F98.INF` on
  98 SE and ME; the 2000 INF is copy only (no leg on 2000).
  [Release record](testing/2026-10-25-release-0.1.1.md).
- **Signing**: x86 only, unsigned, no catalog.
- **Known limitations**:
  - In the first session after the dongle was plugged in, the chip
    delivered every received frame with its Ethernet FCS (seen once
    under NUSB's EHCI on 98 SE, and in a host-side run before). The
    driver checks each frame and strips a valid FCS, so traffic works,
    and it writes one NDIS error-log entry (hardware failure) per adapter
    when it happens. What the chip's state is, and whether the settle
    cycle at start avoids it, is not known: QEMU cannot power the dongle
    off, so the first session after a physical plug is owed on a bench
    ([ax88179-fcs](testing/2026-10-22-ax88179-fcs.md)).
  - Windows 2000 not evaluated; ME under xHCI98 only; no bulk
    throughput test; standby and resume, cable unplug, disable and
    enable and `NetworkAddress` not run.
  - The listed AX88179 / AX88178A adapters only, by their VID/PID; the
    driver is bound whole to the device.

## Release 0.1.0 (2026-10-24): the first driver folders

`uac2`, `usbmidi`, `uvc98`, `usbcom`, `cdcnet`, `rtl815x`, `xpad98`,
`usbflop` and `libusb0`, in the systems folders their designs name
(`STAGE.TXT` lists every INF), with `noaud` unchanged.
Every driver is version 0.1.0 (`DriverVer` 10/24/2026,0.1.0.0); `libusb0`
keeps its libusb-win32 lineage version 0.1.13.0.

- **Status**: every folder is "Built, not tested": the legs ran in QEMU,
  with simulators or real devices passed through, never on a real PC.
  `STAGE.TXT` stages an INF only where such a leg passed; each line says
  which. [Release record](testing/2026-10-24-release-0.1.0.md).
- **Signing**: x86 drivers are unsigned. x64 drivers carry the project's
  own test signature (`usb-wdm-free-test.cer`, thumbprint
  `814CF778B45159EC2659C39C16C7E75EB3C37145`, SHA-1, no timestamp, no
  catalog). On Vista and later x64, trust the certificate (Root and
  Trusted Publishers) and turn test signing on; a signed in-box driver
  that already took a device keeps it until the driver is updated by hand.
- **Known limitations**:
  - `uac2`: XP capture delivers no signal (playback works); 8-channel
    playback stalls under NUSB's EHCI in QEMU; 98 SE may freeze at the
    first audio arrival after a boot (README); x64 folders copy only.
  - `uvc98`: YUY2 cameras only (offered to Video for Windows as RGB24
    too, converted in the driver); up to 320x240 proven with a real
    camera; VfW preview (CamCheck) proven with simulated cameras only; on 2000
    and under NUSB an unsplit camera binds whole and its microphone stays
    without a driver; FAIL on 98 SE under NUSB and on 2000 under xHCI98
    in QEMU.
  - `usbcom`: the 2000 and XP INF is copy only; no real board.
  - `cdcnet`: no real ECM, NCM or RNDIS adapter; RNDIS INF copy only;
    RTL8153 adapters are left to the Realtek drivers. Its INFs bind the
    Linux Ethernet gadget (`0525:A4A2`, also QEMU `usb-net`) as a whole
    device: a composite gadget that reuses that id loses its other
    functions to `cdcnet`.
  - `rtl815x`: no device and no legs beyond a simulator; copy only.
  - `xpad98`: the 2000 INF is copy only while it shares the Play and
    Charge Kit id with the non-free `x360wc`; the wireless receiver on
    98 SE needs an xHCI98 newer than 3.1.0.0.
  - `usbflop`: 98 SE needs NUSB 3.3's USB storage drivers installed
    first; no catalog.
  - `libusb0`: listed devices only; no transfers tested.

## `noaud` 1.0.0.0 (2026-10-23): `free/audio/noaud/w98se/NOAUD98.INF`

On 98 SE, if plugging a webcam freezes Windows, install NOAUD98.INF for
its microphone; the camera keeps working and the microphone is ignored.

- **What it is**: an INF only, for Windows 98 SE. It gives the USB Audio
  Class 1.0 function of the devices it lists no driver at all, so 98 SE's
  own `usbaudio.sys` never loads for them. It lists the Logitech Brio 100's
  microphone (`USB\VID_046D&PID_094C&MI_02`) and QEMU's emulated
  `usb-audio` (`USB\VID_46F4&PID_0002&MI_00`), never a class.
- **Opt-in**: `STAGE.TXT` marks it `COPY`. xHCI98's installer copies it
  with the free set and never stages it; you install it by hand
  (`README.TXT` beside it). Staged, it would take the microphone away
  from everyone whose machine does not freeze.
- **Why**: on 98 SE, a USB audio device arriving can freeze the machine
  as its driver loads (the Ctrl+Alt+Del list shows "Msgsrv32 [Not
  responding]"). In QEMU: 16 of 30 plugs froze without the INF, 0 of 30
  with it, under 98 SE's own UHCI driver and under xHCI98, with QEMU's
  `usb-audio` and with a Brio 100 passed through
  ([record](testing/2026-10-23-brio-null.md)).
- **Known limitations**: the microphone does not work while the INF is
  installed. The freeze is not `usbaudio.sys`'s alone: this repository's
  own `uac2` driver froze 98 SE in 9 of 13 counted arrivals in the same
  rig, so a driver of ours for the microphone would not avoid it (same
  record). Tested only in QEMU (TCG), not on a real PC; the Brio's video
  was not captured with the INF installed; the Update Driver route in
  `README.TXT` was not run in a guest.

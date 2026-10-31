# Release notes

Per package, newest first. A release is a commit and a tag
(`<package>-<version>`); its files are under `free/` and listed in
`SHA256.TXT`, and `STAGE.TXT` says which INFs xHCI98's installer may stage.

## Release 0.1.6 (2026-10-31): `uac2` seeking, Windows 2000 install

`uac2` 0.1.6.0 fixes garbled sound after seeking and asks for a restart
when it is installed on Windows 2000. Every other file is 0.1.5's.

- **uac2, seeking**: after a seek in a player (VLC, or Windows Media
  Player with FLAC, say), playback through `uac2` could carry short
  pieces of the sound from before the seek, again and again, so it
  sounded garbled. `uac2` now tells Windows how far it has taken the
  sound for the USB transfers rather than how far the device has
  played, so Windows writes new sound ahead of what the transfers
  take, and the sound taken is cleared: sound written too late is
  heard as a short gap rather than as old sound. Rarely, a block
  written at the moment of a seek can still play late. The playback
  position Windows shows now runs up to 16 ms (32 ms on some devices)
  ahead of the device, and a pause and resume skips that much instead
  of repeating it. [uac2 design, section 17](design/uac2.md).
- **uac2 on Windows 2000, install**: changing a device's driver to
  `uac2` (Update Driver, for instance from the "USB Audio Device"
  Windows installs by itself) could stop Windows (STOP 0x0000001E in
  Windows' `usbhub20.sys`). Every `uac2` install on 2000 now asks for a
  restart, and `uac2` starts after it. XP and later install as before:
  this change adds no restart there, and changing Windows' own USB
  audio driver to `uac2` on XP still behaves as the README's known
  issues describe. [uac2 design, section 18](design/uac2.md).
- **Known issue, Windows 2000**: enabling a disabled `uac2` device can
  still stop Windows the same way. Unplug the device instead of
  disabling it.
- **Signing**: the x64 `uac2wdm.sys` is signed with 0.1.4's test
  certificate (`usb-wdm-free-test.cer`). The x86 images are unsigned,
  as always.

## Release 0.1.5 (2026-10-30): audio devices with several outputs, serial bridges

`uac2` 0.1.5.0 gives each output and input of a device that has several
its own Windows sound device, and ships a co-installer that keeps
Windows' USB Audio 1.0 driver off a UAC 2.0 device. `usbcom` 0.1.5.0
serves the common USB serial bridges on 98 SE and ME. Every other file
is 0.1.4's.

- **uac2, several outputs and inputs**: a device whose outputs and
  inputs each have their own clock (an on-board USB audio codec, say)
  gets one playback or recording device per output and input (on Vista
  and later named by its kind: Speakers, Headphones, Line, Microphone).
  Before, `uac2` served only the first. A device with more than 64
  streaming alternates now starts too. Known issue: on Vista and later
  an input of the kind "analog connector" was not listed as a recording
  device in the legs (on Vista x64 its endpoint read as disabled); not
  investigated further.
- **uac2 co-installer** (`UACCI.INF`, `UACCI1.DLL`, in the 2000 to 8.1
  folders, x86 and x64): Windows' signed USB Audio 1.0 driver outranks
  `uac2` on 2000 to 7, and on XP changing it to `uac2` afterwards
  waits until a restart. Before plugging the device in, put `uac2`
  where Windows looks (`pnputil -a uac2wdm.inf` on Vista and later; its
  folder on `DevicePath` on 2000 and XP) and install `UACCI.INF` once
  (right-click, Install): then the first plug installs `uac2`. USB Audio
  1.0 devices keep Windows' driver. 8 and 8.1 install `uac2` without the
  co-installer; their x64 `pnputil` wants a signed catalog, which this
  package has none of.
- **uac2 on 98 SE and ME with Windows' own USB 1.1 stack**: `uac2` now
  starts there (Full Speed). ME plays and records; 98 SE does too when
  the known issue below does not strike. Give the wizard
  `uac2`'s folder at the first plug: a copy in `INF\OTHER` is not seen.
- **Known issue, 98 SE with Windows' own USB 1.1 stack**: the first
  start of a Full Speed UAC 2.0 device can hang the system. Use NUSB or
  xHCI98.
- **usbcom chip modules** (98 SE and ME): the FTDI FT-X, FT232H and
  FT4232H, the Prolific PL2303 and the Silicon Labs CP2105 and CP2108,
  each port with its own rate and framing limits. Later bridges need no
  wizard once `USBCOM.INF` is installed. Known issue: under NUSB's EHCI
  on 98 SE a 64 KiB echo lost received bytes once on each of four
  simulated bridges; the cause (the driver or below it) is not read.
- **Legs**: simulator only (and the real Sound Blaster X4 through a VM).
  `uac2`: XP under xHCI98 with a replayed capture of an on-board codec
  (three outputs, three inputs), the X4 gate, the co-installer on 2000,
  XP, Vista, 7, 8 and 8.1 (x86 and x64), 98 SE and ME on their own
  stacks; the x64 legs ran this source signed in the guest with a
  throwaway certificate. `usbcom`: 98 SE and ME under xHCI98 and NUSB.
  [uac2 design, section 16](design/uac2.md),
  [chip legs](testing/2026-10-29-usbcom-chips.md).
- **Signing**: the x64 `uac2wdm.sys` and `UACCI1.DLL` are signed with
  0.1.4's test certificate (`usb-wdm-free-test.cer`). The x86 images
  are unsigned, as always.

## Release 0.1.4 (2026-10-28): newer Realtek chips, uac2 volume

`rtl815x` 0.2.0.0 adds the Realtek RTL8153D (1 Gb/s), RTL8156 and
RTL8156B (2.5 Gb/s), RTL8157 (5 Gb/s) and RTL8159 (10 Gb/s). `uac2`
0.1.4.0 adds playback volume and mute. Its INF lets Windows' software
MIDI synth play through the device. Every other file is 0.1.3's.

- **uac2 volume and mute**: the Volume Control, the tray volume and
  Sounds and Audio Devices now set the device's level. The driver uses
  the volume and mute controls the device offers. A device without them
  gets the same controls in software: the driver scales the sound itself.
  Until you change it the device keeps its own level. After that the
  driver remembers the level and sets it again at the next start.
- **uac2 MIDI**: the INF registers WinMM's MIDI output, so `.mid` files
  play through Windows' software synth. Before it a `.mid` file would not
  play when a `uac2` device was the only sound device.
- **uac2 legs**: simulator only. Volume and mute on 98 SE under xHCI98,
  XP x86, XP x64, Vista x86 and 7 x86, with and without a volume control
  on the device. On Vista and 7 the Sounds panel's Levels slider and mute
  and the tray volume set the device's own controls. One simulated device
  has a CX31993 DAC's layout.
  [Volume record](testing/2026-10-28-uac2-volume.md),
  [MIDI record](testing/2026-10-28-uac2-swmidi.md).
- **uac2 on XP x64**: Windows installs its own USB audio driver first.
  Changing the driver to `uac2` hung here every time, and only a restart
  helped. The legs moved Windows' `wdma_usb.inf` aside before plugging
  the device in. On Vista and 7 Windows' driver takes the device first
  too (and fails to start); changing it to `uac2` worked at once there.

- **98 SE, ME and 2000**: `RTL81598.INF` and `RTL815X.INF` add
  `0BDA:8155`, `8156`, `8157` and `815A`. The driver serves the chip
  that the adapter reports, whatever listed id it has.
- **XP to 7**: a new `RTLXP7.INF` in `wxp` and `wv-7-x86`. It lists only
  chips that Realtek's driver in the non-free set does not serve on that
  system. XP and Vista get the RTL8153D, 8156 and 8156B. XP, Vista and 7
  get the RTL8157 and 8159. Windows 8 and later are not targets.
- **x64 (XP x64, Vista x64, 7 x64)**: built (NDIS 5.1), in `wxp-x64`
  and `wv-7-x64`. The XP x64, Vista x64 and 7 x64 legs ran a trace
  build of `088aabd`, signed in the guest with a throwaway certificate,
  not this image.
- **Firmware**: none is loaded. Linux runs these chips without their
  patches too. Without its patch an RTL8159 links at 5 Gb/s at most.
- **Legs**: simulator only. No adapter with any of these chips has run.
  98 SE, ME and 2000 under xHCI98, with each chip at its top speed and a
  lower one: DHCP, pings, a cable pull and unplug. The XP to 7 legs ran
  at High Speed. [Release record](testing/2026-10-28-release-0.1.4.md).
- **Signing**: 0.1.4 is signed with a new test certificate. Trust the
  `usb-wdm-free-test.cer` of this release, not an earlier one. Its
  thumbprint is `E6A2749B78E9E4074260CF79972A57EEB6E76C0A`. Every x64 image is signed
  again with it, the unchanged ones too. Their image digests are 0.1.3's.
  The x86 images are unsigned, as always.

## Release 0.1.3 (2026-10-26): class matches

Each class driver matches its USB class on the systems where Windows has
no driver of its own for that class, and lists devices elsewhere, so
Windows' own class driver stays the default there (owner, 2026-10-26;
`docs/design/build.md` section 7.1 has the table and its sources). `uac2`
(USB Audio Class 2.0, every target), `usbmidi` (98 SE and 2000), `cdcnet`,
`usbflop` and `uvc98` already did. New: `usbcom` 0.1.3.0 matches any
CDC-ACM function a bus presents on its own (`USB\Class_02&SubClass_02`)
on 98 SE and ME (`USBCOM.INF`) and on 2000 (`USBCDC.INF`, Windows' own
`usbser.sys`), as it did on XP and later; the listed boards keep their
lines. `cdcnet`'s `CDCRNDIS.INF` 0.1.3.0 no longer claims
`USB\Class_EF&SubClass_04&Prot_01` on 7, which Windows 7's own RNDIS INF
matches. Every other file is 0.1.2's.

- **Legs**: on 98 SE under xHCI98, a simulated UAC 2.0 device and a lone
  MIDIStreaming interface at ids no INF lists got `uac2` and `usbmidi` by
  their class lines over Windows' `usbaudio.sys` (`USB\CLASS_01`) at the
  first arrival, with both attached at boot (a hot plug after the desktop
  hit the known 98 SE audio freeze). `usbcom`'s new class line: ranking
  read from the INFs, no leg. [Release record](testing/2026-10-26-release-0.1.3.md).
- **Signing**: no image changed; nothing signed.

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

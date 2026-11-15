# The project's mark in every device name, "(wdm-free)", 2026-11-15

The owner's decision of 2026-11-15, on a user's request: every name the
project's drivers show carries "(wdm-free)", so that a device on one of
them can be told from one on a maker's driver from usb-wdm-nonfree.
Where a name had the driver's short name in brackets, the mark replaces
it ("USB Audio 2.0 (uac2)" is "USB Audio 2.0 (wdm-free)", "Xbox 360
compatible pad (xpad98)" is "Xbox 360 compatible pad (wdm-free)"); a
name with another bracket takes it inside that bracket ("USB Ethernet
(CDC-ECM, wdm-free)"); any other name has it appended ("USB MIDI device
(wdm-free)"). Development legs in QEMU with QEMU's own devices and the
simulators, not a release and not "Tested".

## What changed

- **INFs** (`src/`, every package that names a device): the device
  names of `uac1`, `uac2` (x86 and x64 copies), `usbmidi`, `noaud`,
  `libusb0`, `absptr` (three INFs), `xpad98` (three), `ax88179`,
  `ax88772`, `rtl815x` (three), `sr9700`, `cdcnet` (`CDC98.INF`,
  `CDCNET.INF`, `CDCRNDIS.INF`), `usbprn` (two), `usbcom` (`USBCOM.INF`,
  `USBCDC.INF`, `USBCOMP.INF`), `usbflop` (three: "(CBI, usb-wdm-free)"
  is "(CBI, wdm-free)") and `uvc98` (two). Not changed: `Provider`, disk
  names, service display names (`uac2` and `uvc98`, whose service took
  the device's string, have a `Service` string of their own with the old
  text), hardware ids, test-only INFs and models (`CDCTEST.INF` and the
  other `cdcnet` test INFs, `UVCSIM.INF`, `UVCSIM1.INF`, `wdmsmoke.inf`,
  `usbmidi.inf`'s `Simulation1` to `Simulation3`), the codec `uvcdec`
  (not a device), and `DriverVer` (the release sets it).
- **Names the drivers write themselves:**
  - `uac2` and `uac1` (`UacName`, `uacdesc.c`): the wave and mixer
    devices (the `KSCATEGORY_AUDIO` interfaces' `FriendlyName`) and, on
    Vista and later, the devnode's `FriendlyName`: the product string, the
    mark, a later pair's " N" ([uac2.md](../design/uac2.md) 21.6).
  - `usbmidi` (`UmPortName`, `msdesc.c`): each MIDI port's interface
    `FriendlyName` ([usbmidi.md](../design/usbmidi.md) 5.4).
  - `usbcom` (`usbcom.c`): the COM port child's device text and its
    `FriendlyName`, "USB Serial Port (wdm-free) (COMn)".
- **Left as they are:** `xpad98`'s and `absptr`'s HID product and
  manufacturer strings (`IOCTL_HID_GET_STRING`: the pad's or tablet's own
  identity, which 98 SE's Game Controllers list does not show; Windows
  names the HID children itself), the network drivers'
  `OID_GEN_VENDOR_DESCRIPTION` (already "usb-wdm-free ..."), the images'
  version resources, and `STAGE.TXT`'s package labels.

## Builds and host tests

Branch `wdm-free-names`. Every touched package's build passed with its
gates (`uac2` `wdmx86` and `wdm64`, `uac1`, `usbmidi` `wdmx86` and
`wdm64`, `absptr` and `xpad98` both profiles, `ax88179`, `ax88772`,
`cdcnet`, `rtl815x` and `sr9700` `ndis5` and where built `ndis51`,
`usbcom` `w98` and `inf`, `usbprn`, `usbflop`, `libusb0` `w98`, `uvc98`
`m2build`). Host tests: `uac2` `test.cmd` (`desctest` with the marked
names, `infchk` with the mark read from `uacdesc.h`), `rtest`,
`playtest`, `streamtest`; `uac1` `test.cmd`; `usbmidi` `test.ps1`
(`umtest` with the marked port names); `absptr` and `xpad98` `test.ps1`;
`usbcom` `validate.ps1` and `gate98.ps1`; `usbprn` `inftest.ps1`: all
pass. `free/` is not changed: a release copies the built files there
(AGENTS.md).

## Legs

QEMU 11.1.0 (`e470268ff4`), headless; the drivers from this branch's
`out\` before the last review fixes (those touched only `usbcom`, the
`uac2` INFs' service display name and the test tools), on a
throwaway overlay of each base of [toolchain.md](../toolchain.md).

| Image | SHA-256 |
|---|---|
| `uac1wdm.sys` | `8fe1a21816ae9b29c13425b804d56803daa23c57b936b1b49f6f1b928c12fc60` |
| `uac2wdm.sys` x86 | `141c293a070a19c602a37f40b79afe02baf3d94cedfcf7b2c5b5a3003ebbf9c5` |
| `usbmidi.sys` x86 | `fedda54a84b65aff20effc14193ed036fecfd2c5adc0c784b19cfa9e6d33e4ac` |
| `absptr.sys` x86 | `1b019dcd72fb3bbfd9a4a89650a252584a0b2161c15895deff95b048418c0b54` |
| `cdcnet.sys` (`ndis5`) | `e49e685970703552039809d30dba7e95a2e80191381bae38120e48e71fa63ee8` |
| `uvcm2.sys` | `c6b5f0dafd976192e7d9eca6e6854a0dfd19de7b5175ffec82e52dd4fa520829` |

**98 SE** (`w98se.qcow2`, TCG, `pc`, 256 MB, Windows' own UHCI; the 98 SE
CD for Windows' files). Each INF was put in `C:\WINDOWS\INF` (`uac1` by
Have Disk, `usbmidi` by Update Driver), then the device plugged:

| Driver | Device | Shown |
|---|---|---|
| `uac1` | QEMU `usb-audio` | Have Disk list and Device Manager "USB Audio 1.0 (wdm-free)"; `UACWAV list`: out 0 "QEMU USB Audio (wdm-free)"; `UACWAV play 2 16 48000 3`: PASS, 8 of 8 buffers |
| `absptr` | QEMU `usb-wacom-tablet` | New Hardware Found and Device Manager (HIDClass) "Wacom PenPartner tablet (wdm-free)", started after a restart |
| `cdcnet` | QEMU `usb-net` | New Hardware Found and Network adapters "USB Ethernet (Linux USB gadget, wdm-free)" |
| `uvc98` | `uvcemu.py` (`FFFF:0002`) | the wizard and Device Manager "USB Video Class camera (wdm-free)" |
| `usbmidi` | `midiemu.py --profile multi --pid 0242` | the Have Disk list "Roland UM-ONE mk2 (TAB, wdm-free)", "USB MIDI device (class compliant, wdm-free)", "USB MIDI device (wdm-free)"; Device Manager the second; after a restart `MIDITEST`: in and out 0 "MIDI 2x2 SIM Port A (wdm-free)", 1 "MIDI 2x2 SIM Port B (wdm-free)" |

**XP SP3 x86** (`wxp.qcow2`, WHPX, 2 vCPUs, 512 MB, `usb-ehci` and
`piix3-usb-uhci`). The driver folders on `DevicePath`, and `UACCI.INF`
installed before the first `uac2` device (README, Installing; without it
XP's own USB audio driver took the first simulator, the known issue):

| Driver | Device | Shown |
|---|---|---|
| `uac2` | `uacemu.py --profile hs24` at High Speed: `--no-product`, `--product "Sound Blaster X1"`, `--product "Creative Sound Blaster X4"` | Found New Hardware and Device Manager "USB Audio 2.0 (wdm-free)" (XP shows the INF's name); `UACWAV list`: in and out "USB Audio 2.0 (wdm-free)" (no product string), "Sound Blaster X1 (wdm-free)", "Creative Sound Blast (wdm-free)" (the product cut at 20); `UACWAV play`: PASS, 8 of 8 |
| `absptr` | QEMU `usb-wacom-tablet` | XP's own HID driver took it first (signed); Update Driver offered "Wacom PenPartner tablet (wdm-free)" from `DevicePath`; Device Manager the same, no problem |
| `cdcnet` | QEMU `usb-net` | Found New Hardware and Network adapters "USB Ethernet (Linux USB gadget, wdm-free)" |

Not run: 2000 (the only NT system of `uvc98`), Vista and later (the
devnode name of 21.6), `xpad98`, the other network drivers, `usbcom`,
`usbprn`, `usbflop`, `libusb0` and `noaud`; their change is the INF's
string, or for `usbcom` the same string in the driver, and their builds
and INF gates passed.

## The maker's name off first (owner, 2026-11-15, later)

The owner, on the XP leg's "Creative Sound Blast (wdm-free)": "It should
be called Sound Blaster X4. Chop the manufacturer name instead." A name
the driver builds that does not fit with the mark now first loses the
device's maker's name (`iManufacturer`) from the front of its product
string, and only then is cut from the end
([uac2.md](../design/uac2.md) 21.7, [usbmidi.md](../design/usbmidi.md)
5.4). Builds `uac2` `wdmx86` and `wdm64`, `uac1`, `usbmidi` `wdmx86` and
`wdm64` with their gates; `uac2` and `uac1` `test.cmd`, `usbmidi`
`test.ps1` (`desctest` and `umtest` with the new vectors) and
`uacsimtest`: all pass. The simulators take `--manufacturer` (`uacemu.py`,
`uac1emu.py`; `uac1emu.py` also `--product`).

| System | Image | Device | Shown | Play |
|---|---|---|---|---|
| XP SP3 x86 (the guest above, `uac2wdm.sys` replaced by the new build and compared) | `uac2wdm.sys` `53a6b5291ab922a454c54335e1fab670fac8529ef9e7133d8bff3b561d88c241` | `uacemu.py --profile hs24 --product "Creative Sound Blaster X4" --manufacturer "Creative Technology Ltd"` | `UACWAV list` in and out 0 "Sound Blaster X4 (wdm-free)"; Sounds and Audio Devices, Audio tab, playback and recording "Sound Blaster X4 (wdm-free)" | PASS, 8 of 8 |
| 98 SE (the guest above, `uac1wdm.sys` replaced and compared) | `uac1wdm.sys` `ff8fb6d52bf1ae849c40e6c6d0fc6637760e55ee9088dfd9a3549cf1b1299892` | `uac1emu.py --profile headset --product "Creative Sound BlasterX G1" --manufacturer "Creative Technology Ltd"` | `UACWAV list` in and out 0 "Sound BlasterX G1 (wdm-free)"; Multimedia, Audio tab, playback and recording the same | PASS, 8 of 8 |

The MIDI rule was read on the host only (`umtest`). Evidence:
`evxp-x4-maker-*` and `ev98-uac1-maker-*` in the folder below.

## Settings kept by name

98 SE kept the preferred playback device by its name: after the `uac1`
leg, `HKCU\Software\Microsoft\Multimedia\Sound Mapper` held
`UserPlayback`="QEMU USB Audio (wdm-free)". A device installed before
this change and updated to it shows a new name, so such a stored choice
no longer matches it ([uac2.md](../design/uac2.md) 21.6). On the XP
guest the key was absent after the `uac2` installs. Whether the mixer
levels (volume, mute) are kept by name was not read.

## Seen on the way (not caused by this change)

- 98 SE: with `uac1wdm.inf` installed, a USB MIDI function that has an
  AudioControl interface (the `multi` simulator) is bound to `uac1`
  ("USB Audio 1.0 (wdm-free)") at its first plug, not to `usbmidi`;
  Update Driver to `usbmidi` then works.
- 98 SE, the first start with the MIDI simulator plugged in: Windows'
  own components asked for the CD, then the desktop stayed empty until
  a key was pressed.

## For the release

- New images (the maker's-name rule included): `uac2wdm.sys` (x86 and x64), `uac1wdm.sys`, `usbmidi.sys`
  (x86 and x64) and `USBCOM.SYS` (98 SE, ME). The x64 `uac2wdm.sys` and
  `usbmidi.sys` need the owner's signature. Every other package changes
  only its INFs (and `noaud` its `README.TXT`), so its images can stay
  those of 0.1.10. No catalog ships, so no catalog is affected.
- Every changed INF needs a new `DriverVer` (and the changed images a new
  version), so that an installed driver is replaced
  ([installer.md](../design/installer.md), the identical-INF trap; 98 SE
  and XP keep "the best driver ... already installed" otherwise).
- A system that already has a driver keeps the old name until the
  driver is updated from the new INF.

## Evidence

Development host A, `E:\usb-wdm-free\names\vm\`: `ev98-*.png`,
`ev98-uac1-uacwav.txt`, `ev98-usbmidi-miditest.txt`, `evxp-*.png`,
`evxp-serial-uac2.txt`; the guests' serial logs and every screenshot in
`s98\` and `sxp\`; simulator logs `*.jsonl` there.

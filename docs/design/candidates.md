# Other drivers worth writing: a survey

Status: design draft, 2026-10-20. No code yet.

This is a survey, not a design. It looks at USB device classes beyond
the designs already planned (see "Already planned"), asks what each of
Windows 98 SE, ME, 2000 and XP already has for them, what a driver from this repository would plug into on each, what
it would cost and who would use it, and ranks them. Each candidate that
makes the cut gets its own design document later.

## How the per-system facts were checked

Three kinds of source, named on every claim below:

- **Install-media listing.** The file names inside the setup cabinets of
  the Windows 98 SE CD (`WIN98\*.CAB`), the Windows ME CD (`WIN9X\*.CAB`),
  the Windows 2000 SP4 CD (`I386\*`, `I386\DRIVER.CAB`, `I386\SP4.CAB`) and
  the Windows XP SP3 CD (`I386\*`, `I386\DRIVER.CAB`, `I386\SP3.CAB`),
  listed with 7-Zip, and the hardware ids in the in-box INFs' model lines.
  This settles whether a file and a binding exist; it says nothing about
  how the file works, and nothing here reads a Microsoft binary beyond its
  name. Files Microsoft shipped later (service packs for 98 SE, Windows
  Update downloads, the Smart Card Base Components) are not on these discs
  and are marked so.
- **Driver kit documentation and headers.** The Windows 98 DDK (its
  `HELP_WIN98DDK.CHM`, `HELP_OTHER.CHM`, `HELP_SCDDK.CHM` and its `INC`
  headers), and the WDK 7.1 headers (`inc\ddk\usbprint.h`,
  `inc\ddk\usbscan.h`).
- **Reference code.** Linux v6.12 (line counts below are of that tag),
  QEMU's USB device models, and the user-space projects named per
  candidate, each with its licence as its own header states it.

"Unverified" marks what none of these settled, with the reading that would.

## What each system has in the box

| Function | 98 SE | ME | 2000 SP4 | XP SP3 |
|---|---|---|---|---|
| USB printer class (`07`) | none: no `usbprint.sys`, no INF matching `Class_07` | `usbprint.sys` (copied by `usb.inf`), `usbmon.dll` | `usbprint.sys`, `usbprint.inf` matches `USB\Class_07` | the same, plus `dot4.inf` for `07/01/03` |
| Still image (`usbscan.sys`, STI) | `usbscan.sys`, `sti.dll`, `sti.inf`; no WIA, no PTP | WIA (`wiaservc.dll`, `wiatwain.ds`), `ptpusd.dll`, `ptpusb.dll` | `usbscan.sys`, STI | WIA; `ptpusb.inf` matches `USB\Class_06&SubClass_01&Prot_01` |
| Smart card (CCID) | none (`smclib` not on the CD) | none | `smclib.sys`, `scardsvr.exe`; no CCID reader driver | the same; no CCID reader driver on the CD |
| Mass storage | none | `usbstor.sys`; Bulk-Only by class, CBI by 6 VID/PIDs | `usbstor.sys`; Bulk-Only by class, CBI by 4 VID/PIDs | `usbstor.sys`; Bulk-Only by class, CBI by 19 VID/PIDs |
| IrDA | serial and FIR-chip IrDA (`ircomm.vxd`, `fastir`, `nscirda`, `smcirda`); no USB dongle driver | `irda.sys` | `irda.sys`, `irenum.sys` | the same; no USB IrDA driver on the CD (unverified for later updates) |
| Bluetooth | none | none | none | `bthport.sys`, `bthusb.sys` |

Sources: install-media listing for every cell. The CBI counts are
`usbstor.inf`'s model lines installed with `USBSTOR_CBI` (on XP also
`USBSTOR_CBI_NR`), counted from the INFs on the CDs
([floppy.md](floppy.md), section 2.2); none of the three systems matches `USB\Class_08&SubClass_04` by class.

## Already planned

Designed in their own documents, not surveyed or ranked here:

- UVC webcams ([uvc.md](uvc.md)), UAC 2 audio ([uac2.md](uac2.md)), USB
  MIDI ([usbmidi.md](usbmidi.md)) and USB Ethernet
  ([ethernet.md](ethernet.md)): the first four designs of this
  repository.
- USB serial (CDC-ACM, FTDI, CP210x, CH340/CH341, PL2303):
  [serial.md](serial.md).
- Game controllers (Xbox 360, One and Series pads as HID):
  [gamepad.md](gamepad.md).

## What a class driver from here sits on

The class drivers in this repository are WDM drivers over the standard
USBD interface, and must work under Microsoft's UHCI, OHCI and EHCI stacks,
under NUSB's back-ported EHCI stack on 98 SE (fully supported, Owner,
2026-10-20) and under xHCI98. Two host-side facts matter to this survey:

- Composite devices. Microsoft's composite handling on 98 SE, ME and 2000
  predates the Interface Association Descriptor; how each groups a
  multi-interface function is unverified (reading: plug an IAD-grouped
  composite into each and read the device ids Device Manager shows).
  xHCI98 groups by IAD and by the legacy rules
  (`xhci98\docs\contributing\roadmap-hcd.md`, items 25.6 and 26-A.7).
  Owner, 2026-10-20: INFs here match interface-level ids only, which exist
  where the stack split the device; a whole multi-function device is never
  bound generically, and an unsplit one is served under xHCI98 or by a
  VID/PID line added on request (the one exception is serial,
  [serial.md](serial.md) section 5.2).
- Polling. xHCI98 lists interrupt polling above 1000 Hz as untested
  (`xhci98\README.md`, "Known issues", "Untested"). No candidate below
  needs more than 1000 Hz.

Every candidate below except the display adapter is bulk and interrupt
only, so none of them depends on isochronous support.

## The candidates

### 1. USB printers on Windows 98 SE

**Devices people use today.** USB to parallel printer cables (the usual
way to drive a parallel laser or dot-matrix printer from a machine with
no LPT port; these present printer class 07), and USB printers that speak
a language 98's own drivers know (PCL 5, PCL 6, PostScript laser
printers). Most current inkjets and host-based printers have no Windows
98 printer driver at all, so a port driver alone does not help them; that
limit should be said plainly in the user documentation.

**In the box.** 98 SE has no printer class driver and no INF matching
`Class_07`; ME, 2000 and XP have `usbprint.sys` (table). So this is a
98 SE-only driver.

**The OS-facing model.** Two pieces. A WDM function driver: bulk OUT for
data, bulk IN where the device has it, the class requests
`GET_DEVICE_ID` (IEEE 1284 ID) and `GET_PORT_STATUS`, and the
`IOCTL_USBPRINT_*` interface (WDK `usbprint.h`) so its device interface
looks like the later systems'. And a port monitor DLL for 98's spooler:
the 98 DDK documents the port monitor entry points
(`InitializeMonitorEx`, `AddPort`, `OpenPort`, `WritePort`,
`ClosePort` and the rest; `HELP_OTHER.CHM`, "Required Functions" in the
printer section; the DDK's sample port monitor source is not read), and the
monitor writes the
job to the WDM device through `CreateFile` and `WriteFile`. Plug and Play
printer install from the 1284 ID (a child device whose id the printer
class installer matches against printer INFs) is unverified on 98 SE
(reading: the 98 DDK's printer Plug and Play pages and `LPTENUM`'s
documented id format). The port monitor is user-mode code. Owner,
2026-10-20: user-mode code is in scope when it exists to make a driver here
work, so the monitor belongs here.

**Linux reference.** `drivers/usb/class/usblp.c` (1474 lines,
GPL-2.0+): the class requests, 1284 ID read, protocol selection
(07/01/01, /02, /03).

**Size.** About 1,500 lines for the driver and 1,000 for the monitor.
Estimate.

**Value.** Medium. A real gap on 98 SE only; the user base is people
with parallel printers and modern machines. No QEMU printer model;
testing needs a mainstream USB-to-parallel cable and a printer (Owner,
2026-10-20: real devices only, no gadget rig).

### 2. Still cameras and phones over PTP

**Devices people use today.** Digital cameras (PTP is the USB still image
class, `06/01/01`), phones in "PTP" or photo-transfer mode (Android's PTP
option; iPhones present a PTP interface). MTP-only phones speak an
extension of PTP; photo import via the PTP subset usually works on them
(unverified per phone).

**In the box.** 98 SE has `usbscan.sys` and STI but no WIA and no PTP
support; ME has WIA with `ptpusd.dll`; XP binds PTP by class in
`ptpusb.inf`; 2000 has STI only (table).

**The OS-facing model.** On 98 SE there is no kernel work: `usbscan.sys`
is the in-box generic bulk driver for still image devices, documented in
the 98 DDK (`HELP_WIN98DDK.CHM`, still image pages, e.g.
`IOCTL_GET_USB_DESCRIPTOR`; `SCAN_DDK\SRC_SCANCAM_INC_USBSCAN.H`). An
INF of ours binds `USB\Class_06&SubClass_01&Prot_01` to it as an `Image`
device; the rest is user mode: either a small PTP import program (list
objects, copy files), or a TWAIN data source so imaging programs of the
period can acquire. A STI user-mode still image driver (USD) would add
the "camera connected" event (from the 98 DDK's still image
documentation; its sample USD source is not read). Showing
a camera as a drive letter is a different and much larger project (a
synthetic file system under IOS) and is not proposed.

**Reference.** No Linux kernel driver; libgphoto2's `camlibs/ptp2`
(LGPL-2.0-or-later per `ptp.c`'s header)
holds the PTP protocol and per-vendor quirks. QEMU has `usb-mtp`
(`hw/usb/dev-mtp.c`), an MTP responder over a host directory, but it is
not in this host's Windows QEMU 11.1 build (`-device help` does not list
it); a Linux host would run it.

**Size.** INF trivial; an import program about 1,500 to 2,500 lines; a
TWAIN source more. Estimate.

**Value.** Medium-low. Real but narrow; a card reader does the same job
on 98 SE once mass storage works. All user mode, which is in scope;
but Owner, 2026-10-20: not now.

### 3. INF-only quick wins over in-box drivers

One gap closes with an INF and no driver, because the system's own
driver is already there and simply not matched:

- **USB floppy drives (UFI) on ME, 2000 and XP.** Their `usbstor.inf`
  handles CBI floppies only by listed VID/PIDs (6, 4 and 19 CBI lines); a
  drive not listed stays without a driver. An INF matching
  `USB\Class_08&SubClass_04` to the in-box `usbstor.sys` through the
  same `DriverFlags` value the `USBSTOR_CBI` section sets would cover the
  rest. Whether `usbstor.sys` drives an unlisted UFI device correctly is
  unverified (reading: a TEAC or Y-E Data drive not in the list). On 98
  SE the same INF serves once a USB storage driver set is installed
  (item 11).

This costs a day and ships nothing of Microsoft's. Owner, 2026-10-20:
INF-only packages that bind Microsoft's in-box drivers are in scope when
they fill a gap Windows leaves, and this one gets a full design
([floppy.md](floppy.md)).

### 4. A generic USB access driver (libusb-win32 compatible)

Hobbyist tools on Windows (avrdude, EPROM and microcontroller
programmers, logic analysers, SDR front ends) often talk to their device
through libusb-win32's kernel driver. That driver is GPL-2.0-or-later
(`libusb/src/driver/driver_api.h` header). Whether its last builds still
run on 98 SE and ME is unverified (reading: its old release notes, then a
test on 98 SE). If they do, the work is packaging, not writing; if not, a
port is a medium job. Value: medium for a technical audience, which
overlaps strongly with this project's. Owner, 2026-10-20: investigate
first ([libusb.md](libusb.md)). Owner, 2026-10-20, after it: libusb-win32
0.1.12.2 is rebuilt from source for 98 SE and ME as the package `libusb0`;
2000 and later users are pointed to upstream.

### 5. CCID smart card readers

**In the box.** 98 SE and ME: nothing on the CD; the smart card stack
for 9x was the separate Smart Card Base Components, and the 98 Smart
Card DDK ships `smclib` both as a VxD and as a WDM library
(`SMRT_DDK\SRC_SMARTCRD_VXD_BIN_FREE_SMCLIB.VXD`,
`SRC_SMARTCRD_WDM_BIN_FREE_SMCLIB.SYS`; `HELP_SCDDK.CHM`). 2000 and XP:
`smclib.sys` and the resource manager `scardsvr.exe`, but no CCID driver
on the CD (Microsoft's `usbccid.sys` came later through Windows Update;
unverified for XP SP3 media beyond its absence from the CD).

**Model.** A smart card reader driver on `smclib` (WDM on 2000 and XP;
on 98 SE it also needs the separately distributed resource manager,
which is Microsoft's and not redistributable here). Reference: the CCID
driver of pcsc-lite (`LudovicRousseau/CCID`, LGPL-2.1-or-later); QEMU has
`usb-ccid` with an emulated card (in this host's build).

**Value.** Low. Few people use smart cards on 98 or 2000, and on 98 the
middleware is the larger missing piece.

### 6. USB to parallel adapters

Adapters that present printer class are covered by candidate 1 on 98
SE and by `usbprint.sys` elsewhere. Emulating an LPT port for programs
that drive the port registers directly (dongles, EPROM programmers,
bit-banging tools) is not possible over USB: those programs touch I/O
ports, and the latency of a USB round trip per register access defeats
them even where a VxD could trap the ports. Linux's `uss720.c` (834
lines) drives one old chip as a `parport`, which shows the limit: it
works for printing, not for timing-sensitive use. Not proposed beyond
candidate 1.

### 7. IrDA dongles

98 SE ships an IrDA stack for serial and FIR-chip adapters but no USB
dongle driver; ME, 2000 and XP ship `irda.sys` (table). USB IrDA bridges
(class `FE/02`, SigmaTel STIr4200, KingSun) had drivers in Linux under
`drivers/net/irda/` until that subsystem was removed in Linux 4.17; the
code is GPL-2.0 in older tags. A 98 SE driver would be an NDIS miniport
under the 98 IrDA protocol stack, whose miniport interface is
unverified in the 98 DDK (reading: `NET_DDK` and the IrDA pages, if
any). Value: low; the devices on the other end (old phones, PDAs,
printers) are rare now. Linux's `drivers/usb/serial/ir-usb.c` (487 lines)
treats some dongles as raw SIR serial, which the serial driver
([serial.md](serial.md)) could host if anyone asks.

### 8. USB TV and radio tuners

Current USB tuners are digital (DVB-T2, ATSC, ISDB). Windows exposes
digital tuners through BDA, which none of 98 SE, ME or 2000 has
(unverified for ME; reading: the ME CD listing for `bdasup.sys` and
`ksproxy`'s BDA support), and the tuner and demodulator chips need
per-chip firmware and drivers (Linux `drivers/media/usb/dvb-usb-v2/`).
Analog USB capture (EM28xx) is a WDM video capture driver like the UVC
one, plus a tuner and crossbar (98 SE has `kstvtune.ax` and `ksxbar.ax`
on the CD). Value: low against a large, chip-specific effort. Not
proposed; an analog capture stick could follow the UVC work if asked.

### 9. Bluetooth

Only XP SP2 and later have a Bluetooth stack (`bthport.sys`,
`bthusb.sys` on the XP SP3 CD). For the others a driver means writing the
HCI transport, L2CAP, SDP, and per-profile drivers (HID, RFCOMM serial,
audio), a stack Linux spreads over `net/bluetooth/` and many thousands of
lines, plus pairing UI. Not proposed.

### 10. DisplayLink and other USB display adapters

Not proposed. On 98 SE a display needs a display driver (a 16-bit
display minidriver and VDD, or the DirectDraw HAL), not a USB class
driver, and every frame would have to be compressed and sent over USB.
The open protocol work covers only the oldest chips: Linux's `udlfb.c`
(1958 lines) and `drivers/gpu/drm/udl` drive the DL-1x0 and DL-1x5
families; DL-3xxx and later need DisplayLink's closed user-space
component on Linux too. A second display on 98 is also a multi-monitor
problem in GDI. Too large for the value.

### 11. Mass storage, UAS and UFI on 98 SE

Already decided: a free storage driver for 98 SE (MSC98) was considered
and dropped; USB storage on 98 SE is served outside this repository
(a third-party storage driver set such as NUSB 3.3's), and USB floppies
on 98 SE ride on that: the floppy INF ([floppy.md](floppy.md)) serves 98
SE once such a set is installed, and its readme says so. No driver here
depends on it. Not reopened here.

### 12. exFAT on flash drives

Not a driver of this repository: exFAT is a file system (an IFS driver
on 9x), not a USB class. Users should format sticks FAT32 for 98 SE.

### 13. Hub and keyboard edge cases, HID quirks

Hub behaviour, keyboard boot protocol, and composite splitting under
xHCI98 belong to xHCI98, not to a class driver here. Polling-rate
changes for HID devices are what SweetLow's `hidusbf` already does (its
licence is unverified; it is not proposed for this repository). HID
game controllers that need a start-up sequence belong to
[gamepad.md](gamepad.md).

### 14. USB Wi-Fi adapters

Not a free driver: the makers' drivers and their gaps on 98 SE to 7 are
surveyed in [wifi-coverage.md](wifi-coverage.md) (task 44.71).

## Ranking

USB serial and game controllers are planned ([serial.md](serial.md),
[gamepad.md](gamepad.md)) and not ranked here.

| Rank | Candidate | Systems it adds | Effort | Value | Verdict |
|---|---|---|---|---|---|
| 1 | USB printer class with a 98 port monitor | 98 SE | medium | medium | designed ([printing.md](printing.md)) |
| 2 | INF-only: UFI floppies by class | ME, 2000, XP to 8; 98 SE with USB storage drivers installed | tiny | medium | designed ([floppy.md](floppy.md)) |
| 3 | libusb-win32-compatible generic driver | 98 SE, ME | a rebuild with fixes | medium for developers | designed ([libusb.md](libusb.md)) |
| 4 | PTP photo import over `usbscan.sys` | 98 SE (and 2000) | small to medium, user mode | medium-low | not now |
| 5 | CCID smart card | 2000, XP (98 needs non-free middleware) | medium | low | no |
| 6 | IrDA dongles | 98 SE | medium | low | no |
| 7 | TV and radio tuners | - | large, per chip | low | no |
| 8 | Bluetooth | 98 SE, ME, 2000 | very large | low | no |
| 9 | DisplayLink | - | very large, closed protocol | low | no |
| - | Mass storage and UAS | - | - | - | decided (MSC98 dropped) |

## Decisions

Owner, 2026-10-20:

1. **Next full designs**: USB printing on 98 SE (`printing.md`: a WDM
   driver plus a port monitor) and the UFI floppy INF for ME, 2000 and XP
   (`floppy.md`).
2. **libusb-win32**: an investigation first (`libusb.md`), finding out
   whether its own GPL driver still runs on 98 SE and ME; packaging it here
   would count if it does. After the investigation: libusb-win32 0.1.12.2
   is rebuilt from source for 98 SE and ME with the fixes `libusb.md`
   lists, and 2000 and later users are pointed to upstream.
3. **PTP photo import**: not now.
4. **Scope** (the questions this survey raised): user-mode code (a port
   monitor DLL, an import program, a TWAIN source), VxDs, and INF-only
   packages that bind Microsoft's in-box drivers are all allowed when they
   exist only to make a driver here work or to fill a gap Windows leaves
   (`AGENTS.md`, "Sources").
5. **Test hardware**: real devices, one or two mainstream ones per class.

## Open questions

None at present.

## Not verified

- How Microsoft's composite handling on 98 SE, ME and 2000 groups a
  multi-interface function, with and without an IAD.
- Plug and Play printer install from a USB 1284 ID on 98 SE.
- Whether `usbstor.sys` drives unlisted UFI floppies under a class INF.
- Whether libusb-win32's last builds run on 98 SE and ME (`libusb.md`,
  M0).
- `hidusbf`'s licence; ME's BDA support; the 98 IrDA miniport interface.
- The line-count and effort figures above are estimates.

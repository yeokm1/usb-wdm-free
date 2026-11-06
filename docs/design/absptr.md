# Absolute pointing devices on Windows 98 SE and ME (absptr)

Status: design of 2026-11-06, from the readings of section 3 (QEMU guests,
simulated and QEMU-modelled devices), reviewed and revised; first build
in `src/hid/absptr/` (sections 5 to 9).

## 1. Purpose and devices served

An absolute pointing device reports where on its surface the pointer is,
not how far it moved: graphics tablets, touch screens, the pen of a pen
display, and the "absolute mouse" that KVM switches, remote consoles and
virtual machines present. Windows' mouse path is built for relative mice;
how far each system carries absolute positions is what section 3 reads.

The driver here, `absptr`, serves the devices the systems' own HID support
leaves without a working pointer, on the systems no maker's driver covers
(section 2):

- **The Wacom PenPartner** (`056A:0000`), a USB pen tablet of 1998 to
  2000, which needs a mode switch before it reports positions (section
  5.2). QEMU models it as `usb-wacom-tablet`.
- **USB HID touch screens and pen digitizers** that report on the HID
  Digitizer usage page (Touch Screen, Pen) with no Generic Desktop mouse
  collection beside it, which 98 SE and ME do not map to the pointer
  (section 3; 2000 and XP Home and Professional are read in section 3
  too; XP Tablet PC Edition, which has pen support of its own, is not a
  target).
- **Absolute mice whose report shape Windows' mapper misreads**: a
  Generic Desktop absolute X/Y in fields of other than 16 bits on 98 SE
  and XP (section 3: 12-bit fields land at a sixteenth of the screen), and
  on XP a 16-bit X/Y with Logical 0..65535 (values above 32767 land at 0).

Not served (left to Windows, which already handles them, section 3):
absolute Generic Desktop mice with 16-bit X/Y and Logical Maximum 32767
(QEMU's `usb-tablet`, most KVM absolute mice), and touch screens that also
offer such a mouse collection. Not served at all: touch screens with a vendor protocol (not
HID), the ones Linux's `usbtouchscreen.c` drives (eGalax/EETI in their
own mode, 3M MicroTouch EX II, ITM, ETurboTouch, Gunze, DMC TSC-10,
IRTOUCH, IdealTEK, General Touch, GoTop, JASTEC, e2i, Zytronic, NEXIO,
ELO's 2700 series): a possible follow-up (section 13).

## 2. Makers' drivers (owner policy of 2026-11-06)

Checked on 2026-11-06 at the makers' own sites; nothing was downloaded or
run.

| Maker, devices | What the maker offered for old Windows | Can it be carried in usb-wdm-nonfree? |
|---|---|---|
| Wacom: PenPartner and the tablets of its time (Graphire, Intuos 1 and 2, ArtPad, UltraPad, Cintiq of the period) | Driver 4.78-6 (2004/2005) for 95, 98, ME, NT4, 2000 and XP (`pc478-6-X_en.exe`, USB tablets on 98, ME, 2000 and XP); drivers 5.x from 2000 on | No: Wacom's own URLs for 4.78-6 (`downloadeu.wacom.com/pub/WINDOWS/`) and the 4.9x drivers (`cdn.wacom.com/U/drivers/IBMPC/...`) answer HTTP 403 and 404, and Wacom's driver page lists Windows 10 and 11 only; copies exist only on third-party sites, which the policy excludes |
| Elo Touch Solutions (Elo USB touch screens, `04E7:xxxx`) | USB driver SW500917 for 98 and ME (also 2000), sw500756 for 2000 and XP, per Elo's driver archive pages | Not read: Elo's support pages answer HTTP 403 to this host's non-browser client, so neither the files nor their terms were read (owner question Q1) |
| EETI (eGalaxTouch) | eGalaxTouch 5.14 for 2000 to 11, 5.12 (PS/2) for 2000, XP, Vista; nothing for 98 or ME on EETI's driver page; no licence or terms on that page | Terms not stated (owner question Q1) |
| 3M Touch Systems (MicroTouch) | TouchWare 5.x for 95 to XP, MT 7 for 2000 and XP, per 3M's manuals | Not read: `3m.com/touch` answers HTTP 403 to this host's client |

Who covers what, then:

- **Wacom PenPartner**: no maker driver available for any system (Wacom
  no longer offers its 4.x drivers). absptr serves it on 98 SE, ME, 2000
  and XP.
- **EETI (eGalax) touch screens**: EETI's own driver, still offered, on
  2000 and XP (EETI's controllers only; its redistribution terms are not
  stated, so it is not carried, Q1). Nothing from EETI for 98 SE and ME.
- **Elo touch screens**: Elo lists drivers for 98, ME, 2000 and XP; their
  files and terms could not be read from here (Q1).
- **3M MicroTouch**: 3M's pages could not be read from here (Q1).
- **Other HID touch screens and pens**: no maker driver known.

absptr aims at, and is tested first on, 98 SE and ME, where no maker
driver is available for these devices and Windows has no digitizer
support. It builds the same binary for 2000 and XP: there it serves the
PenPartner and digitizers no maker driver covers; for an EETI or Elo
screen whose maker's driver the user has, that driver is the one to use,
and absptr's legs on those systems are lower priority (owner policy of
2026-11-06). Whether absptr's INFs should leave such screens alone on 2000
and XP is owner question Q7.

QEMU's `usb-tablet` has no maker driver; section 3 shows it needs none on
98 SE and ME.

## 3. How the systems handle absolute pointers today (readings)

Rig: QEMU (usb4-emu fork build `cb36497`, its `usb-redir`), q35 guests of
the shared clean bases with xHCI98 3.4.1.0 as published (`INSTALL.BAT
/console /yes /nopause`, then a cold start), and the 98 SE + NUSB 3.3
guest of this repository's kit (pc machine, `usb-ehci`). `PTRTEST.EXE`
(`src/hid/absptr/ptrtest.c`) logs `GetCursorPos` and the button states to
COM1; the leg script moves the device to six known points (fractions of
its range) and presses each button. A point passes when Windows puts the
cursor within 1 % of where it belongs. Record:
`docs/testing/2026-11-06-absptr.md`.

Devices: QEMU's own `usb-tablet` and `usb-wacom-tablet`, and the profiles
of `absemu.py` (section 11): `tablet` (QEMU's shape: Generic Desktop
Mouse, X/Y 0..32767 in 16 bits), `kvm16` (X/Y 0..65535), `kvm12` (X/Y
0..4095 in 12-bit fields), `pointer` (top-level usage Pointer), `touch`
(Digitizer Touch Screen, one finger), `touchmouse` (that plus a Generic
Desktop absolute mouse collection), `mtouch` (a Windows 7 style
two-contact touch screen), `pen` (Digitizer Pen), `penpartner` (QEMU's
PenPartner model, at either speed).

Windows' own drivers (no absptr):

| Device | 98 SE, xHCI98 | 98 SE, NUSB EHCI | ME, xHCI98 | XP SP3, xHCI98 |
|---|---|---|---|---|
| QEMU `usb-tablet` | 6/6, buttons | 6/6 (`usb_version=2`) | 6/6, buttons | 6/6, buttons |
| `tablet`, `pointer`, `kvm16` | 6/6 | 6/6 | not run | see the record |
| `kvm12` | 1/6: positions at 1/16 of the screen | the same | not run | see the record |
| `touchmouse` | 5/6, through its mouse collection | 6/6, the same | not run | see the record |
| `touch`, `mtouch`, `pen` | no pointer movement | the same | not run | see the record |
| QEMU `usb-wacom-tablet`, `penpartner` | no movement (the power-on mode's reports do not match the model's own descriptor) | the same (`penpartner`) | not run | see the record |

2000 was read under Windows' EHCI on the q35 base, which has no EHCI
driver installed (its Found New Hardware wizard found none without the
2000 CD), so that reading is void; 2000 under xHCI98 is owed (section
12).

What follows from it:

1. **98 SE and ME map an absolute Generic Desktop X/Y to the pointer
   themselves**, and so does XP. On 98 SE and ME the HID mouse
   collection (`HID_DEVICE_SYSTEM_MOUSE`) gets `mouhid.vxd` from
   `msmouse.inf` (`[HIDMouse]`: `DevLoader=*ntkern`,
   `NTMPDriver=mouhid.vxd`). `msmouse.inf` of both systems binds the ELO
   USB touch screen (`USB\VID_04E7&PID_0001`) to `hidusb.sys` in a
   `[STDUSB_ABSOLUTE_POINTER]` section that sets only wider double-click
   rectangles: the in-box absolute path is Microsoft's own design for the
   touch screens of 1998.
2. **The 16-bit field with Logical 0..32767 is the safe shape.** 98 SE
   maps 0..65535 too, XP does not (it puts values above 32767 at 0); a
   12-bit field fails on both.
3. **Nothing on 98 SE or ME takes the HID Digitizer page.** A touch screen
   or pen whose only collection is a Digitizer one is no pointer.
4. **The PenPartner needs its tablet mode**, which only a driver of its
   own sets (section 5.2).
5. Found with absptr's first build (section 6): on 98 SE a report that
   changes a button and the position together does not move the pointer.

## 4. The decision on the OS-facing model

**Decision: one WDM HID minidriver (as `xpad98`, gamepad.md section 3),
registered with `HidRegisterMinidriver`, the function driver of the USB
device, presenting a report descriptor of its own: a Generic Desktop
Mouse with three buttons and absolute X/Y, Logical 0..32767 in 16-bit
fields, the shape section 3 shows 98 SE, ME and XP map.** The device's
own reports are translated into that one report, and Windows' own HID
class driver and mouse mapper do the rest. The same binary is built for
2000 and XP; for 2000 the reading of section 3 is owed, and if 2000's
mouse mapper does not take this shape as absolute, the `w2k-xp` folder
stays XP-only until the NT side has a route of its own (option 3).

Options considered:

1. **HID minidriver with a synthesized absolute mouse (chosen).** Small,
   one binary, and everything above it is Windows' own and already
   carries QEMU's tablet. It reuses `xpad98`'s minidriver and USB code.
2. **A driver that calls VMOUSE itself.** The Windows 98 DDK documents
   `VMD_Post_Absolute_Pointer_Message` (`HELP_OTHER.CHM`, "Absolute
   Pointing Devices", `mouse_9ywn.htm`, and the service page
   `mouse_9yrd.htm`: ESI and EDI the absolute coordinates, AL the button
   status, "VMOUSE simply passes the information to the Windows USER
   module through the Mouse_Event callback function", "does nothing if
   called from a non-system VM"; `INC_WIN98_VMD.INC` lists it as the last
   VMOUSE service). `Mouse_Event` with `SF_ABSOLUTE` (8000h) takes BX and
   CX as "normalized absolute coordinates in the range 0 through 65,535"
   (`mouse_9ypf.htm`). A WDM driver can reach a VxD service through the
   VMM's INT 20h dynalink from a thread of the system VM, as xHCI98's
   connection manager does with CONFIGMG. This works on 98 SE and ME only,
   needs a second path for 2000 and XP, and replaces a mapper that already
   works. Kept as the documented fallback should a device ever need what
   `mouhid.vxd` does not do (for instance a second pointer). Not chosen.
3. **A replacement mouse mapper** (a driver on the HID collection, in
   `mouhid.vxd`'s or `mouhid.sys`'s place, feeding VMOUSE on 9x or the
   mouse class driver on NT). More code for the same result where the
   in-box mapper works; the route for 2000 if section 3's 2000 reading
   shows its mapper ignores absolute X/Y. Not chosen for v1.
4. **A filter between `hidclass.sys` and the USB device** rewriting the
   report descriptor and the reports. 98 SE's and ME's support for lower
   filters on a USB device node is not documented in the 98 DDK; not
   chosen.

## 5. Front ends: what each device sends and how it becomes the report

The core (`apcore.c`, portable C, host-tested by `aptest.c`) keeps one
state: X and Y normalised to 0..32767 and the three buttons. Each front
end parses the device's report into that state; the report builder writes
the 6-byte report of section 6.

### 5.1 Generic HID digitizers and absolute mice

At start the driver reads the device's own HID report descriptor (GET
_DESCRIPTOR, type 22h, from the interface) and parses it with its own
parser (section 5.4), from the HID 1.11 specification (6.2.2, items;
8.4, report format) and the HID Usage Tables 1.12 (Generic Desktop page
01h, Button page 09h, Digitizers page 0Dh). It picks one source, in this
order:

1. A top-level collection on the Digitizer page whose usage is Touch
   Screen (04h), Pen (02h) or Digitizer (01h): within it the first
   Finger (22h) or Stylus (20h) logical or physical collection (the
   fields before any of them count too); its Generic Desktop X (30h) and
   Y (31h), absolute; Tip Switch (42h); In Range (32h), Barrel Switch
   (44h), Secondary Barrel Switch (5Ah), Eraser (45h), Confidence (47h)
   if present. A touch screen's Finger collections are also read as
   contact slots (section 5.3).
2. A top-level Generic Desktop Mouse (02h) or Pointer (01h) collection
   with absolute X and Y and Button page buttons 1 to 3.

Every field used must be in the same input report as X. A device with
neither source fails its start (`STATUS_DEVICE_CONFIGURATION_ERROR`), as
`xpad98` does for a device it cannot drive.

Mapping (an owner decision, Q3; the defaults):

- X and Y: `(v - LogicalMin) * 32767 / (LogicalMax - LogicalMin)`.
  Arithmetic contract: a field of 1 to 32 bits (with an unsigned Logical
  Maximum of at most 2^31 - 1; a larger one reads as negative and the
  source is refused) is sign-extended at its
  own width when Logical Minimum is negative (HID 1.11 5.8: signed when
  an extent is negative); Logical Maximum is read signed only when
  Logical Minimum is negative (as Linux `hid-core.c`'s parser does, a
  fact of its behaviour); a source whose Logical Maximum is not above its
  Logical Minimum is refused; the value is clamped to the logical range
  first, the range and the offset are taken as unsigned 32-bit
  differences and halved together until the range is at most 65535, so
  the product stays below 2^31. No calibration in v1 (Q4).
- Touch (finger): the pointer moves while Tip Switch is down (and
  Confidence, when present, is 1); Tip Switch is the left button.
- Pen: the pointer follows the pen while it is in range (In Range, or Tip
  Switch when there is no In Range); Tip Switch or Eraser is the left
  button (the eraser end acts as a tip); Barrel Switch the right button;
  Secondary Barrel Switch the middle button.
- Absolute mouse: X and Y as above; buttons 1 to 3 as left, right,
  middle.
- Out of range or no touch: the last position is kept and every button
  is reported up.

The Device Mode feature: when the device has one (Digitizer page 52h,
Variable, in a Device Configuration collection, 0Eh), the driver reads
that feature report with GET_REPORT, writes 2 (multi-input) into the
field when its logical range allows 2, else 1 (single input), keeps every
other bit (a Device Identifier among them), and sends it back with
SET_REPORT. A device left in mode 0 sends its mouse collection's reports
instead, which the chosen digitizer source does not read. Any failure of
the two requests leaves the device as it is and the start goes on. Facts
from Microsoft's published digitizer guidance and Linux's
`hid-multitouch.c` (`mt_set_modes`, facts only).

### 5.2 The Wacom PenPartner (`056A:0000`)

Facts from Linux `drivers/hid/wacom_wac.c` and `wacom_sys.c`
(GPL-2.0-or-later; facts used, no code taken): the tablet starts in a mode
that sends mouse-like reports; `_wacom_query_tablet_data` switches a pen
tablet of type PENPARTNER to its tablet mode with a SET_REPORT of feature
report 2 carrying `02 02` and reads it back with GET_REPORT, retrying while
the answer's second byte is not 2 (`wacom_set_device_mode`). Its reports
(`wacom_penpartner_irq`): byte 0 is the report id; for id 2, X is
bytes 1-2 and Y bytes 3-4 (little-endian), byte 5 bit 20h the eraser and
bit 40h the side switch, byte 6 the pressure as a signed byte (-127 is
none); the pen touches when byte 6 is above -80. Report id 1 carries the
same fields, with byte 5 bit 80h meaning "in proximity"; with it clear,
the pen has left. The range is 0..5040 by 0..3780
(`wacom_features_0x00`).

QEMU's model (`hw/usb/dev-wacom.c`, MIT licence; read for its behaviour):
it takes the first byte of a SET_REPORT's data as its new mode (so `02 02`
gives mode 2), answers GET_REPORT with `00 mode`, and in mode 2 sends the
7-byte report above with the pressure byte 0 while the left button (pen
tip) or the middle button (eraser) is down and -127 otherwise.

The front end: at start, the mode switch (up to three tries; the start
fails if the tablet never answers mode 2); after a return to D0, the
same switch again from a work item before the IN transfers restart (the
tablet may have reset). Report 2: X, Y, tip (the
pen or the eraser touching) as the left button, side switch as the
right. Report 1 with bit 80h set: the same; with it clear: every button
up, the position kept. Anything else (the 4-byte mouse-mode reports) is
ignored.

### 5.3 Multi-touch screens

A touch screen with two or more Finger collections, or one with a Contact
Identifier (Digitizer 51h), is read as contact slots (up to 10): Tip
Switch, Contact Identifier, X, Y and Confidence per slot, and the Contact
Count (54h). The pointer follows one contact, by its identifier (by slot
number without one), from its touch to its release; other fingers are
ignored; no gestures. Following Microsoft's description of the parallel,
hybrid and serial reporting modes:

- A report with a Contact Count of n starts a frame; its first n slots
  are valid (null slots beyond them are not read). When n exceeds the
  slots, the frame continues in the next reports, which carry a count of
  0 and the remaining contacts (hybrid mode).
- A count of 0 with no frame in progress means no contact at all: the
  followed contact is released.
- A followed contact that reports Tip Switch 0 is released; one missing
  from a whole frame is released at the next frame's start.
- With no Contact Count field each report is a frame.

### 5.4 The report descriptor parser

Own code, from HID 1.11: short items (long items skipped), the global
stack (Push/Pop), Usage Page and extended (32-bit) usages, Usage
Minimum/Maximum ranges, Report ID, Report Size and Count, Logical
Minimum/Maximum, Collection/End Collection nesting, and the Input, Output
and Feature main items, each with its own bit offset per report id (so a
feature field's report, offset and length are known, section 5.1).
Constant and Array items are skipped (their bits counted). Limits:
descriptor up to 4 KB, 64 levels of nesting and of Push, 256 local usages
per main item, 255 bytes per report; beyond them the start fails. Host
tests run it over the simulator's descriptors, QEMU's tablet descriptor
and hand-written edge cases (signed ranges, extended usages, Push/Pop,
long items, unbalanced and truncated descriptors).

## 6. The report presented to Windows

One top-level collection, no report id, 6 bytes, byte for byte the
`tablet` profile's descriptor that section 3 measured:

```
05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 03 15 00 25 01 95 03 75 01
81 02 95 01 75 05 81 01 05 01 09 30 09 31 15 00 26 ff 7f 35 00 46 ff 7f
75 10 95 02 81 02 09 38 15 81 25 7f 35 00 45 00 75 08 95 01 81 06 c0 c0
```

Byte 0 buttons (bit 0 left, 1 right, 2 middle), bytes 1-2 X, 3-4 Y
(little-endian, 0..32767), byte 5 a wheel, always 0.

States wait in a small queue (16) for hidclass's reads. A move replaces
the newest queued state only when that state is itself a move; every
button change stays a report of its own, in order and at the position it
happened, so a tap between two reads is not lost and a move after a press
does not merge into the press. When a report changes
the buttons and the position together (a touch landing somewhere new),
the move is queued first with the old buttons, then the button change:
98 SE's mapper does not move the pointer on a report that also changes a
button (found with the first build: taps landed where the pointer was).

`IOCTL_HID_GET_DEVICE_ATTRIBUTES` returns the device's own VID, PID and
bcdDevice; `IOCTL_HID_GET_STRING` the product string read from the device,
or "absptr pointer".

## 7. USB side

From `xpad98`'s `usbio.c` (this repository, GPL-2.0-or-later), cut down:

- the device descriptor, the configuration descriptor, the one HID
  interface (class 03h) with an interrupt IN endpoint and its HID
  descriptor (21h) with the report descriptor's length;
- SELECT_CONFIGURATION of that interface at alternate 0;
- for a boot-subclass interface (subclass 01h), SET_PROTOCOL(1), the
  report protocol (HID 1.11 7.2.6: the host sets the protocol it wants);
  then SET_IDLE(0); a device may refuse either;
- the report descriptor with URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE
  (the interface number as the request's wIndex);
- the front end's SET_REPORT / GET_REPORT (URB_FUNCTION_CLASS_INTERFACE);
- the interrupt IN kept armed twice, each transfer one wMaxPacketSize (a
  HID report that fills its packet then ends the transfer; HID devices
  send no zero-length packet), or as many whole packets as the source
  report needs when it is longer; driver-owned IRPs and the outstanding
  count as `xpad98`, with one change: the decision to resubmit is taken
  under the lock and counted until the transfer is in the USB stack, and
  stop and power-down wait for that count to be zero before they abort the
  pipe or cancel, so no transfer is sent after the abort;
- below D0: no transfer is sent, those in the USB stack are cancelled and
  (at PASSIVE_LEVEL) waited for, up to a second, before the power request
  goes down; at D0 a work item restores the protocol, the idle rate and
  the front end's mode (SET_PROTOCOL, SET_IDLE, the PenPartner switch or
  the Device Mode feature) and then restarts the IN transfers.

The device is bound whole (single-interface HID devices are not split).
A composite device (a touch screen with a keyboard function) is not
served in v1: its start fails when the configuration has any interface
besides the HID one, as `xpad98` refuses composites (Q5).

Only control and interrupt transfers: the driver runs under every host
controller driver offering the standard USB interface (Windows' own,
NUSB's, xHCI98).

## 8. INF and install per system

- **98 SE and ME** (`ABSPTR.INF`, `w98se-me`): class `HIDClass`, models
  by VID/PID, `DevLoader=*ntkern`, `NTMPDriver=absptr.sys`, the file to
  `10,system32\drivers` (as `xpad98`'s 9x INF). A VID/PID match outranks
  `hiddev.inf`'s class match, so the listed devices get absptr and nothing
  else changes (read on 98 SE: the wizard offers "The updated driver
  (Recommended) Wacom PenPartner tablet (absptr)").
- **2000 and XP** (`ABSPTR2K.INF`, `w2k-xp`): `Include=input.inf`,
  `Needs=HID_Inst.NT`, a demand-start kernel service (as `xpad98`'s
  2000 INF).
- **Which ids** (Q2): the PenPartner `USB\VID_056A&PID_0000`, and a
  "Generic" model with an id no device has (`USB\VID_FFFF&PID_FFFF&
  REV_FFFF`), the entry Update Driver's "Have Disk" offers for any other
  HID touch screen or pen. `testinf.ps1` writes test copies of both INFs
  with the simulator's ids (`FFFF:0031`, `0032`, `0034`, `0036`, `0037`) for the
  legs; they are never released.
- **hidclass.sys and hidparse.sys on 98 SE** are Microsoft's and are not
  shipped. 98 SE's own `hiddev.inf` and `msmouse.inf` copy them from the
  Windows files when the first HID device is installed. Whether they are
  present when absptr's device is the first HID device a 98 SE ever saw
  is read by the first-HID-install leg (section 12) on the q35 98 SE base
  (which never had a USB stack before xHCI98). If they are not, the INF
  gets a CopyFiles of them from the Windows media through
  `LayoutFile=LAYOUT.INF`, and that is read again.
- No registry values in v1.

## 9. Files

`src/hid/absptr/`: `absptr.c` (DriverEntry, PnP, power, HID IOCTLs, the
state queue), `usbio.c` (section 7), `apcore.c` and `apcore.h` (the
parser, the front ends, the report), `absptr.h`, `absptr.rc`,
`absptr.inf`, `absptr2k.inf`, `build.cmd`, `chkimp.ps1`,
`imports-wdmx86.txt`, `aptest.c` and `test.ps1` (host tests of the
core), `testinf.ps1` (the test INF copies), `ptrtest.c` and `ptrtest.cmd`
(the guest reading tool), `absemu.py` (the simulator).

Released folders: `free/hid/absptr/w98se-me/` (98 SE, ME) and
`free/hid/absptr/w2k-xp/` (2000, XP 32-bit). No x64 build in v1 (Q6).

## 10. What is adapted, and the NOTICE.md rows

`usbio.c` and `absptr.c` are adapted from `xpad98` (same licence, same
author). Linux's `wacom_wac.c`, `wacom_sys.c` and `hid-multitouch.c` were
read for facts (section 5); no code, names or structure taken. QEMU's
`dev-wacom.c` and `dev-hid.c` were read for their behaviour, and two
report descriptors are carried as data: QEMU's PenPartner descriptor in
`absemu.py` (the `penpartner` profile) and QEMU's tablet descriptor in
`aptest.c` (a host test vector). Both files keep QEMU's MIT notice for
those bytes and have a NOTICE.md row. The driver itself carries no outside
code.

## 11. The simulator (`absemu.py`)

A usb-redir device in the style of `xpademu.py`: one HID interface, the
profiles of section 3, Full or High Speed, driven by the leg script over a
control port (JSON lines: move, tip, buttons, range, pressure, barrel,
eraser, quit). The descriptors are its own, written from HID 1.11 and the
HID Usage Tables, except the `penpartner` profile's, QEMU's (MIT), so the
PenPartner can be read at High Speed under NUSB's EHCI, which QEMU's own
Full Speed model cannot reach. Its `mtouch` profile reports one finger
in slot 0 with a Contact Count of 1 while touching and 0 when lifted.

## 12. Test plan

The legs, each on a fresh overlay with the package installed by its INF
(the test copy for the simulator's ids):

| System | Host controller drivers | Devices |
|---|---|---|
| 98 SE | xHCI98 3.4.1.0; NUSB 3.3 EHCI | QEMU `usb-wacom-tablet` (xHCI98), `penpartner` (both), `touch`, `mtouch`, `pen`, `kvm12` (both); QEMU `usb-tablet` stays on Windows' own path (regression) |
| ME | xHCI98 3.4.1.0; Windows' own UHCI | the same as 98 SE under xHCI98; under UHCI QEMU's `usb-wacom-tablet` and `touch` (NUSB does not install on ME, so ME has no EHCI stack to test) |
| 2000 SP4 | xHCI98 3.4.1.0 (the in-box reading of section 3 first) | `penpartner`, `touch`, `pen` |
| XP SP3 | xHCI98 3.4.1.0; Windows' EHCI where the base has its driver | `penpartner`, `touch`, `pen`, `kvm16` |

The first-HID-install leg: on a fresh 98 SE q35 overlay with xHCI98
installed and no HID device ever plugged in, the PenPartner as the first
HID device (section 8).

Each leg: install, the six points and the buttons (PTRTEST; taps for touch
profiles), a click on a desktop icon, unplug and replug, a cold boot with
the device plugged in. Not covered: a real PenPartner, a real touch screen
or pen (the owner's bench), standby and resume. 98 SE is the readiness
system (AGENTS.md); ME, 2000 and XP stay "built, not tested" for any host
controller driver whose leg is missing.

## 13. Follow-ups, not in v1

- Vendor-protocol USB touch screens (section 1's list, Linux
  `usbtouchscreen.c`, facts only).
- Calibration and axis swap/inversion (Q4), multi-monitor.
- Later Wacom tablets (Graphire, Intuos), each with its own protocol in
  `wacom_wac.c`.
- A mouse mapper of its own for 2000, if section 3's 2000 reading shows
  the in-box one ignores absolute X/Y (section 4, option 3).
- x64 builds for XP x64 (Q6).

## 14. Records

`docs/testing/2026-11-06-absptr.md`: the readings of section 3 and the
legs of section 12.

## 15. Owner questions

- **Q1. Makers' drivers whose terms could not be read** (Elo SW500917 and
  sw500756, EETI eGalaxTouch 5.x, 3M TouchWare): leave them out, or read
  their terms from a browser and decide? Recommended: leave them out of
  usb-wdm-nonfree for now, listed as "Not shipped" with their makers'
  pages.
- **Q2. Which ids the release INFs list**: (a) the PenPartner and the
  "Have Disk" Generic entry; (b) also HID touch screens and pens by
  VID/PID from public lists (Linux `hid-multitouch.c`'s table, facts
  only), none of them tried. Recommended: (a).
- **Q3. Pen buttons**: barrel switch as the right button and the eraser
  as a pen tip (default above), or another mapping? Recommended: as above.
- **Q4. Calibration**: none in v1 (the full logical range is the whole
  screen), or a registry value per device with a small tool? Recommended:
  none in v1.
- **Q5. Composite touch screens** (a HID touch interface next to other
  functions): refused in v1, or bound per interface (`&MI_xx`)?
  Recommended: refused in v1.
- **Q6. x64**: none in v1, or an XP x64 build? Recommended: none in v1.
- **Q7. Screens a maker's driver covers on 2000 and XP** (EETI, Elo):
  absptr's INFs list no such ids, so they are bound only by "Have Disk";
  leave it so, or say in the release notes which maker covers which
  screen? Recommended: leave the INFs so and name the makers in the
  release notes.

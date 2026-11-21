# Phones and cameras over MTP and PTP on Windows 98 SE and ME (usbmtp)

Status: reading and design of 2026-11-16, fourth pass (the driver's
concurrency contract replaced by one lock, one request per slot and one
completion point, with a host-built model of it; section 7 checked
against the still image and MTP 1.1 specifications; the driver no longer
reads Microsoft OS descriptors); the owner's decisions of section 14
recorded; no package code; M0's probe and simulator work may start
(section 12). 2026-11-17: the driver's standby rework and its readings
(5.4), Q18 decided with no driver change, and the library stage, with
the program's refusal of a standby and the device list, designed to the
point of review (7.9, 3.2.1, 12); no library code. Owner, 2026-11-16: MTP
on 98 SE and ME is taken up ([candidates.md](candidates.md), Decisions, 7), and what the
user sees is a GUI file explorer program, a windowed Win32 program that
runs on 98 SE and ME and browses a device's storages and folders and
copies files to and from it, on top of whatever binding or kernel driver
the reading shows is needed. A shell view, a drive letter and WIA
integration are not asked for. The earlier sizing is
[mtp-estimate.md](mtp-estimate.md); this document replaces its route
choice and keeps its facts.

Every size below is an estimate, not a measurement. "Not run" means not
run; "unverified" names the reading that would settle a point. What
has been read is cited to its testing record (M0, the driver's load and
standby readings, the phone): the driver and its slot model have been
run, emulated and on one phone read only. The library, the program and
the device list are designs and have not been written or tested; their
tests are planned work (7.9, 11, 12).

## 1. Conclusion

- **The program:** a windowed file explorer, working name "MTP Explorer"
  (`MTPEXP.EXE`), with a folder tree on the left and a file list on the
  right, as the systems' own Explorer has (section 8). Files are dragged
  in from Explorer, and out to it through staged real files, so the
  program's own progress and Cancel apply (8.3).
- **The driver:** a small WDM transport driver of ours, `USBMTP.SYS`,
  rather than Windows' own still image driver `usbscan.sys` (section 4).
  - 98 SE's `usbscan.sys` can abort its own pipes, but it cannot send the
    still image class requests (Cancel, Get Device Status, Device Reset).
    So after a stopped or failed transfer it cannot bring the device back
    to a known state, short of reading the rest of a large object or a
    replug.
  - The driver is a transport only, with an explicit handle contract
    (section 5). The MTP/PTP protocol lives in user mode, in a library the
    program links, which is also built and tested on the build host
    (section 7).
- **The INF:** `USBMTP.INF` binds the driver to the still image interface
  class `06/01/01`, which neither 98 SE nor ME binds itself, and to listed
  phones whose MTP interface is vendor class `FF` (section 3).
- **Systems:** 98 SE and ME only (owner, 2026-11-16), one folder
  `free/image/usbmtp/w98se-me/`.
- **Testing:**
  - QEMU's own `usb-mtp` model, after a small patch that lets it build
    on Windows (section 11.1).
  - A usb-redir simulator of this repository's kind, `mtpemu.py`, for
    what `usb-mtp` lacks.
  - Fixtures written by hand from the specifications, independent of
    both (section 11.3).
  - Legs on 98 SE under NUSB's EHCI and xHCI98, both required, and on ME
    under xHCI98. ME may be called ready on its xHCI98 half alone; its USB
    2.0 half is recorded as not tested, and the user documents say plainly
    that on ME the package was tested under xHCI98 only (owner,
    2026-11-16; section 11.5).
  - Real phones and cameras are the owner's bench.
- **Before code starts:** M0 (section 12) settles four things on 98 SE and
  ME:
  - composite ownership and ids;
  - how NTKERN carries the handle contract (blocking calls, cancel,
    error codes, close on exit);
  - how each stack retires cancelled bulk, interrupt, control and reset
    requests (5.6), which decides the bounds 5.4 may promise.

## 2. Devices and what they present

| Device | Interface in file-transfer mode | Protocol |
|---|---|---|
| Digital camera | `06/01/01` (USB still image class); bulk IN, bulk OUT, interrupt IN | PTP |
| Android phone, "PTP" mode | `06/01/01` | PTP, with MTP's operations on many phones (unverified per phone) |
| Android phone, "File transfer" mode | expected `06/01/01` with interface string "MTP" on current Android, and vendor class `FF/FF/00` on phones whose MTP function predates that; in both shapes a Microsoft OS descriptor (string `0xEE`, then an Extended Compat ID naming `MTP`) | MTP |
| Media players (2005 to 2012 and some current ones) | either shape | MTP |
| iPhone | `06/01/01` for the camera roll | PTP, read-only |

Which shape a given phone uses is unverified here. The reading is the
phone's descriptors on the owner's bench, or Android's MTP sources
(Apache-2.0, facts only, not on this host). Android phones are usually
composite: MTP beside ADB (`FF/42/01`), and sometimes beside RNDIS or MIDI.
A phone also asks its user to allow access to its data before it shows any
storage, which no emulator reproduces (section 11.6).

## 3. Binding on 98 SE and ME

### 3.1 Ids

What the hub gives a device node:

- A single-interface device: hardware ids `USB\VID_v&PID_p&REV_r` and
  `USB\VID_v&PID_p`; compatible ids `USB\CLASS_06&SUBCLASS_01&PROT_01`,
  `USB\CLASS_06&SUBCLASS_01` and `USB\CLASS_06`.
- One interface of a composite device split by the stack:
  `USB\VID_v&PID_p&MI_nn`, and that interface's class ids.
- Every stack here gives a composite's interfaces nodes of their own with
  `&MI_nn` ids:
  - NUSB's and xHCI98's hubs on 98 SE;
  - stock 98 SE, through its USB hub driver's generic parent;
  - ME, through its composite parent driver.

  The two stock cases are as Microsoft's documentation of USB composite
  devices and of the standard USB identifiers describes them. That
  documentation is not read here; M0 confirms both cases on the guests.
- That support does not mean every layout works. M0 reads, on every
  stack, with the simulator's composite profile and with the bench
  phones' descriptors once captured:
  - the ids;
  - the configuration descriptor the interface's node is given;
  - which selection its parent takes from the child (3.3).

  A phone that groups MTP with another interface by an Interface
  Association Descriptor is a reading of its own.

### 3.2 What the INF matches

- **`06/01/01`, by class.** Neither system matches the class itself
  ([mtp-estimate.md](mtp-estimate.md) 3, from the install media):
  - 98 SE has `usbscan.sys` and STI but no INF for the class;
  - ME's `sti.inf` carries a PTP section but no model line for the class.
  - ME's `kdk2x0.inf` lists Kodak cameras by VID/PID. Those keep ME's own
    driver, since a hardware id outranks our compatible id, and the
    program does not see them (said in the readme).
  - What M0 read of ME's WIA (2026-11-16): ME's own driver search finds
    no driver for a `06/01/01` device (QEMU's `usb-mtp`: "Windows was
    unable to locate the software"), so nothing of ME's takes such a
    device by itself; with our INF installed the device binds to our
    driver. Whether ME's WIA service would contend for a device bound to
    our driver was not read; the design does not rely on WIA in any way,
    and a camera ME's own `kdk2x0.inf` lists keeps ME's driver.
- **Vendor class `FF`: never by class.** `USB\Class_FF` is every
  vendor-specific device. 98 SE and ME do not read Microsoft OS
  descriptors, so no compatible id says "MTP".
  - Owner, 2026-11-16 and 17: such devices are matched by a list of
    device ids, and only by it: a large list converted from libmtp's
    device list, with the rows read here overriding it (3.2.1).
  - No model is offered for picking by hand (Update Driver, Have Disk).
    M0 (2026-11-16) read that 98 SE's Have Disk lists all of an INF's
    models for a device whose ids match none of them, but a pick of one
    did not bind: the wizard closed, the function's node disappeared and
    came back on the next plug as new hardware. ME's Have Disk was not
    read. Only listed devices are supported.
- **Under xHCI98:** if xHCI98's hub reported a compatible id from the
  Microsoft OS descriptor, as XP and later do (`USB\MS_COMP_MTP`), the INF
  could match it on every phone. This is a question for xHCI98
  (section 14), not a dependency.

### 3.2.1 The device list (design of 2026-11-17, with the owner's decisions)

Android-style devices whose MTP interface is vendor class `FF/FF/00`,
which 98 SE and ME cannot recognise by descriptor, are matched by a list
of device ids in `USBMTP.INF`. Owner, 2026-11-16: the list shipped is a
large one converted from libmtp's device list; a device not in it is
added by hand by the user, as the readme says, with no function for it
in the program. Owner, 2026-11-17: one INF, its libmtp-derived lines
converted to the GPL; the table under LGPL-2.1-or-later, a copy of it in
the release folder; rule R3 below; the readme instruction as drafted,
corrected after its leg; the probe tool shipped, so that reports can
carry its `/pnp` output (14, Q20). The tracked INF does not change until
the generator and the table come, as one reviewed change (7.9, order of
work, step 6).

**What a row is.** libmtp's list gives a vendor id, a product id, a
maker's and a model's name and quirk flags, and no interface class,
count or number. So every row taken from it is a device whose MTP shape
this repository has not read. The readings cover one phone, a Fairphone
3 ([2026-11-16-mtp-m1-phone.md](../testing/2026-11-16-mtp-m1-phone.md)):
in its MTP mode `18D1:4EE1`, one `FF/FF/00` interface, on 98 SE under
both stacks and on ME; in its PTP mode `18D1:4EE5`, one `06/01/01`
interface, matched by the class line. The readme and the release notes
say so.

**What a line matches** (the readings of
[2026-11-16-mtp-m0.md](../testing/2026-11-16-mtp-m0.md), the simulator's
`android` and `androidff` composites, and the phone record):

- **An interface line** (`USB\VID_v&PID_p&MI_nn`) matches that
  interface's node under all three configurations (M0's probe INF bound
  by such lines). Under 98 SE's USB 2.0 stack the node is described by
  the product string, under xHCI98 by the interface string.
- **A whole-device line** (`USB\VID_v&PID_p`) matches a single-interface
  device under all three (the phone's scratch INF bound by one).
- **A whole-device line on a composite** would match the composite's
  parent under 98 SE's USB 2.0 stack: a hardware id outranks the
  compatible id `USB\COMPOSITE`, so the whole device would come to
  `USBMTP.SYS`, which refuses it (`CW_WHOLE`, 3.3) without handing it
  back: MTP does not work and the phone's other functions are lost with
  it. Under xHCI98 the parent's ids are xHCI98's own
  (`USB\XHCI98_COMPOSITE&...`), so the line matches nothing. Inferred
  from the ranking rule; read by L1 below.
- So a line's form is chosen per id; for libmtp's rows by a rule over
  names (R3), not a reading. Never both forms for one VID/PID.
- A `06/01/01` interface, alone or in a composite, needs no row: the
  class line matches it.
- Not read: a real phone as a composite (L3); interface numbers of 10
  and more (the list takes `MI_00` to `MI_09` only until read); an MTP
  interface grouped by an Interface Association Descriptor.

**When a listed id is not an MTP device after all.** `Start` runs
`CfgWalk` and fails the start on anything but `CW_OK` (3.3): `CW_WHOLE`
(a composite's parent), `CW_ABSENT` (the `&MI_nn` is not in the
configuration), `CW_CLASS` (neither `06/01/01` nor `FF`), `CW_ENDPOINTS`
(not exactly one bulk IN, one bulk OUT and one interrupt IN; Android's
ADB interface is refused here). The node then keeps `USBMTP.SYS` with a
failed start until the line is removed and the device installed again.
A vendor interface of another purpose with exactly that endpoint set
starts; the library's GetDeviceInfo then ends in a protocol error or a
time-out, the program shows "This device does not answer as a phone or
camera (PTP/MTP)." (13). No mutating MTP operation (SendObjectInfo,
SendObject, DeleteObject, SetObjectPropValue, Android's edits) is sent
to a device before GetDeviceInfo and OpenSession have succeeded and
GetDeviceInfo has listed that operation; those two commands, and the
class requests, are themselves outbound writes, and are the only ones
before that point.

**How a device gets on the list.**

1. **libmtp's rows**, by rule R3: libmtp 1.1.22's
   `src/music-players.h` (7.1's archive and SHA-256; licence "GNU Lesser
   General Public License ... version 2 ... or (at your option) any later
   version", the archive's `COPYING` LGPL 2.1). Read 2026-11-16: 1,479
   entries, 1,472 distinct VID/PID; model names state a mode for some (220
   MTP alone, 136 MTP plus ADB alone, 96 other composites, 13 PTP, 1,014
   nothing). R3: names that say MTP plus ADB alone as `&MI_00`, the 96
   named composites and the 13 PTP rows left out, every other id as a
   whole device: about 1,363 rows by the draft's count, the generator's
   count replacing it. **R3 is not a sound classifier of a device's
   interface layout**: it guesses from libmtp's names, which were not
   written to say it. Its risks: a composite libmtp does not name as one
   gets a whole-device line, and under 98 SE's USB 2.0 stack the whole
   phone is then captured (above); `CfgWalk`'s refusal does not give the
   device back to the composite parent, so that loss is not made
   harmless, only visible. That risk is the owner's accepted choice (Q20),
   not something the design removes. A phone whose MTP is not interface 0
   under an "MTP plus ADB" name gets interface 0 bound and refused. A
   phone's modes (MTP, MTP with debugging, PTP, tethering) are separate
   enumerations with separate PIDs: a line for one PID says nothing of
   another mode's PID, and a VID/PID alone never establishes an
   interface number.
   **Duplicates, resolved deterministically:** entries of libmtp's list
   with one VID/PID collapse to one candidate; if R3 gives them different
   shapes, or any of them is left out by R3, the id is left out. Across
   sources a read row wins over a reported row, and either over
   libmtp's, for the same VID/PID whatever the shapes; two rows of one
   source for one VID/PID are refused by the gate.
2. **Rows read here** (`mtpids.txt`, GPL-2.0-or-later): `VVVV:PPPP shape
   read:<testing record> label`, overriding libmtp's row for the same
   VID/PID (`18D1:4EE1` whole, read).
3. **Rows reported by users**: a report that carries the shipped probe's
   `/pnp` output gives the ids and the interface classes; a row is added
   to `mtpids.txt` as `reported:<date>`, its shape taken from that
   output, in a reviewed change. A report without it is not listed.
4. **A user's own line**, added by hand to the INF on the user's machine
   by the readme's steps (below); local, never in the release.

**The generator** (as `src/serial/usbcom`'s `boardids.txt`, `boards.ps1`
and `geninf.ps1`):

- `mtpids-libmtp.txt` (LGPL-2.1-or-later, owner 2026-11-17): `VVVV:PPPP
  shape libmtp-R3 <maker and model as in libmtp>`, with the SPDX line,
  libmtp's copyright lines (2005-2007 Richard A. Low, 2005-2013 Linus
  Walleij, 2006-2007 and 2015-2018 Marcus Meissner, 2007 Ted Bullock,
  2012 Sony Mobile Communications AB, as read), its licence notice and
  warranty disclaimer verbatim, and a header saying it is a modified copy
  of libmtp 1.1.22's `src/music-players.h` (rows selected, names kept, a
  shape column added), generated by `libmtpids.py` on the date given.
  The LGPL 2.1 text goes into the repository beside it.
- `libmtpids.py` (GPL-2.0-or-later) reads `music-players.h` from the
  pinned archive in the git-ignored `docs/references/`, checks its
  SHA-256 against 7.1's, applies R3 and writes the table. The build never
  reads the archive; a later libmtp release is a new pin, a regenerated
  table and a reviewed diff.
- `mtpids.ps1` returns the model lines: the class line first, then every
  id in VID, PID, interface order, a read or reported row replacing
  libmtp's for the same VID/PID.
- `geninf.ps1` builds `USBMTP.INF` from a fixed, tracked template,
  `usbmtp.inf.in`, which is the whole INF but for two marked places: the
  licence block (below) and the body of `[Models]`; it fills both and
  changes nothing else. `-Leg` writes a copy under `out/` with the
  simulator's ids (`FFFF:0060` to `0065`) added, for the legs only; it
  is never tracked or released (the gate refuses VID `FFFF` in the
  tracked INF).

**The licence block of the generated INF** (comment lines above
`[Models]`; the section 3 conversion must carry the notices, not only
name them):

- the SPDX line `GPL-2.0-or-later` for the file, and this repository's
  copyright line for the INF's own text;
- that the list lines below are derived from libmtp 1.1.22's
  `src/music-players.h` (archive, SHA-256 and path as 7.1), with
  **libmtp's copyright lines copied verbatim**, as they stand in that
  file;
- **the converted permission notice**: libmtp's notice with every
  reference to the GNU Lesser General Public License altered to the GNU
  General Public License, version 2 or (at your option) any later
  version, and its warranty disclaimer kept, as LGPL-2.1 section 3
  requires of a converted copy ("you must alter all the notices that
  refer to this License");
- **the change and provenance notice**: that this copy was modified
  (rows selected by rule R3, names dropped, each row written as an INF
  model line), generated by `libmtpids.py` and `geninf.ps1` on the date
  given from `mtpids-libmtp.txt` at the commit given, and converted to
  the GPL under LGPL-2.1 section 3 on that date; and that the rows'
  LGPL source is `mtpids-libmtp.txt`, shipped beside the INF.

**`USBMTP.INF`'s sections** (one INF, as now, with the list):

| Section | What |
|---|---|
| licence block | as above, from the template's marked place |
| `[Version]`, `[ClassInstall]`, `[UsbMtp.ClassReg]` | as now (9) |
| `[Manufacturer]` | `%Provider%=Models` |
| `[Models]` | generated: `%Device%=UsbMtp,USB\Class_06&SubClass_01&Prot_01`, then `%Phone%=UsbMtp,USB\VID_vvvv&PID_pppp` or `...&MI_nn`, one line per id (38 or 44 bytes with CR LF) |
| `[UsbMtp]`, `[UsbMtp.AddReg]`, the copy sections, `[DestinationDirs]`, `[SourceDisksNames]`, `[SourceDisksFiles]` | one install section for every line: the driver, and `MTPEXP.EXE` with its Start menu entry (8.6, Q5) |
| `[Strings]` | `Provider`, `ClassName`, `Device`, `Phone`, `Disk` (13) |

One string for every listed device keeps the INF small: per-model names
(13's "<maker> <model> (MTP, wdm-free)") would add about 40 bytes a line,
some 55 KB, past the cap below. The listed devices show one name in
Device Manager, "MTP device (wdm-free)" (owner, 2026-11-17; Q21 (a); the
string a draft).

**This departs from 7.1** (owner, 2026-11-17): 7.1 keeps adapted code
under its LGPL; for these list lines in the INF, LGPL-2.1 section 3 is
applied instead ("Once this change is made in a given copy, it is
irreversible for that copy"), and the table stays LGPL. 7.1's rule for
code is unchanged.

**INF size.** No Microsoft document stating a size limit for an INF on
98 SE or ME was found; community reports speak of 64 KB, and nothing was
measured. 60 KiB for the whole INF and 128 characters a line are a
provisional cap for the gate, not a known Windows limit. R3's model lines
come to about 52.6 KB, with the rest of the INF (about 1.2 KB) and the
licence block (about 3 KB with the notices in full, an estimate) about
4.6 KB below the cap. L2 replaces the
cap with readings; if it reads a lower limit, the list is narrowed (for
example to libmtp's rows flagged Android), which is the owner's call.

**The gate** (`gate.ps1`'s INF check, run by `build.ps1`) and
`inftest.ps1`:

- **The template.** With the licence block and the body of `[Models]`
  taken out, the INF equals `usbmtp.inf.in` byte for byte; and the
  template itself is checked as an INF (below), so a change to it is a
  reviewed change the gate also validates.
- **Sections and directives**, on the whole generated INF: every
  section header is well formed and appears once; the required
  sections are present (`[Version]`, `[ClassInstall]`,
  `[UsbMtp.ClassReg]`, `[Manufacturer]`, `[Models]`, `[UsbMtp]`,
  `[UsbMtp.AddReg]`, each copy section, `[DestinationDirs]`,
  `[SourceDisksNames]`, `[SourceDisksFiles]`, `[Strings]`) and no
  other; `[Version]` has `Signature="$CHICAGO$"`, the class, the class
  GUID in its form and `DriverVer` in its form; `[Manufacturer]` names
  `[Models]`; every model line names `UsbMtp`; every `CopyFiles` and
  `AddReg` names a section that exists; every copy section has a
  `[DestinationDirs]` entry; every file in a copy section is in
  `[SourceDisksFiles]` with a disk in `[SourceDisksNames]`, and every
  entry there is copied by some section; every `%token%` used resolves
  in `[Strings]`, and every string there is used.
- **Encoding and line ends:** bytes 09h, 20h to 7Eh, 0Dh and 0Ah only
  (ASCII: the one string for listed devices needs nothing else); no
  byte-order mark, no NUL; every line ends CR LF, the last one too; no
  lone CR or LF; no trailing blank or tab.
- `[Models]` equals `mtpids.ps1`'s lines, line for line and in order.
- Every list id matches `USB\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}` with an
  optional `&MI_0[0-9]`, upper case; no `&REV_`; no VID or PID `0000`;
  no VID `FFFF`; no id twice; no VID/PID in both forms.
- No other class line in any form (`USB\Class_FF` and every
  `USB\Class_FF&...`, `USB\Class_06` alone, `USB\COMPOSITE`, any
  `USB\XHCI98_` id).
- The INF within the provisional cap; no line over 128 characters.
- The table: its SPDX line, libmtp's copyright lines and licence notice
  verbatim, the generation header, the row syntax, no VID/PID twice.
  The INF's licence block: libmtp's copyright lines equal to the
  table's, the converted permission notice equal to the text the
  generator holds for it (with no "Lesser" left), the warranty
  disclaimer, the change and provenance notice naming the table's
  commit.
- `inftest.ps1` makes each mutation and requires the gate to refuse it:
  an `FF` class line, a repeated line, lower case, a three-digit VID,
  `&MI_1A`, both forms for one id, a line in no table, a table row
  missing, two lines swapped, `&REV_`, `USB\COMPOSITE`, VID `FFFF`,
  padding past the cap, a 129-character line; outside `[Models]`: a
  changed template byte, a missing install section, a section header
  twice, a misspelt section header, a `CopyFiles` naming no section, a
  file in a copy section missing from `[SourceDisksFiles]`, an
  unresolved `%token%`, a missing `[DestinationDirs]` entry, a missing
  `Signature`; encoding: an LF line end, a lone CR, a byte-order mark, a
  byte above 7Eh, a NUL, a missing final CR LF; licence: a missing
  libmtp copyright line, a notice still naming the Lesser GPL, a
  missing warranty disclaimer, a missing provenance notice; on the
  table, a missing copyright line, a missing licence notice, a changed
  SPDX line, a VID/PID twice.

**In the release folder** (9): the INF, and beside it a copy of the
table as the lines' plain-text source with the LGPL 2.1 text, and the
probe for reports: `MTPIDS.TXT`, `LGPL21.TXT`, `NOTICE.TXT`, and
`MTPPROBE.EXE` built with `/pnp` only (owner, 2026-11-17; Q21 (b), (c)).

**A device not in the list: the readme** (owner: as drafted, corrected
after its leg). Draft for the owner:

> **If your phone is not listed**
>
> Windows asks for a driver for your phone but finds none in this
> folder. You can add your phone to `USBMTP.INF` yourself. The list in
> this release was converted from libmtp's device list; only one phone (a
> Fairphone 3) has been tried with this driver.
>
> 1. Connect the phone, unlocked, in "File transfer" mode. When the Add
>    New Hardware Wizard opens, click Cancel.
> 2. Run `regedit` and open `HKEY_LOCAL_MACHINE\Enum\USB`. Find the key
>    named `VID_xxxx&PID_yyyy` for your phone (the one whose `DeviceDesc`
>    is your phone's name). Write down `xxxx` and `yyyy`.
> 3. Look for keys that start with the same `VID_xxxx&PID_yyyy` and end
>    with `&MI_00`, `&MI_01` and so on.
>    - **If there are none**, your phone is one device. Add this line
>      under `[Models]` in `USBMTP.INF`:
>      `%Phone%=UsbMtp,USB\VID_xxxx&PID_yyyy`
>    - **If there are**, your phone has several functions. Find the
>      `&MI_nn` key whose `DeviceDesc` says "MTP" (and not "ADB
>      Interface"), and add this line instead:
>      `%Phone%=UsbMtp,USB\VID_xxxx&PID_yyyy&MI_nn`
>      **Do not add the line without `&MI_nn` for such a phone**: on
>      Windows 98 SE with the USB 2.0 driver it takes the whole phone away
>      from Windows, and its other functions (a memory card, for example)
>      stop working.
> 4. Unplug the phone, plug it in again, and when the wizard asks for a
>    driver, choose this folder.
>
> To undo a wrong line: remove it from `USBMTP.INF`, remove the device in
> Device Manager, then unplug the phone and plug it in again.
>
> Please send us the line you added, your phone's name and the file that
> `MTPPROBE /pnp` writes, so that it can be listed in a later release.

Not read: that 98 SE's and ME's `Enum\USB` keys and `DeviceDesc` values
look as these steps say for a real phone; that an interface's
description says "MTP" under 98 SE's USB 2.0 stack (there it is the
product string, above, so step 3 holds as written only under xHCI98);
the undo steps. A leg reads them with the simulator's composite, and
the text is corrected before the readme ships. Whether steps 2 and 3
use the shipped probe's report instead of `regedit` (it lists every
node's hardware and compatible ids under every stack, so an interface of
class `FF/FF/00` can be found as a **candidate**; that class is every
vendor-specific function, not MTP alone, and only the driver's
descriptor checks and the library's GetDeviceInfo show whether the
candidate is MTP): they do (owner, 2026-11-17; Q21 (d)); the readme's
draft above is to be rewritten that way before its leg reads it.

**Readings this asks for** (none needs the driver to change; each is a
leg of 12, "M1's library stage", except L3):

- L1: a whole-device line for the `android` simulator's composite id
  (the leg INF) on 98 SE under NUSB's EHCI and under xHCI98: the parent
  taken, start refused `CW_WHOLE`, on the first; nothing matched on the
  second; then the line removed and the device restored, which records
  how a user recovers.
- L2: the INF's size on 98 SE and ME (one stack each), with INFs made by
  the real generator in the release's encoding (ANSI, CR LF): model
  sections of 100, 800, 1,500 and 3,000 lines with no padding, and a
  100-line section padded by comments to 32, 60, 70 and 130 KB; in each
  the test device's id as the first, a middle and the last model line;
  for each the wizard finds the line and binds, then the device is
  plugged in again and the INF installed a second time.
- L3 (the owner's bench): the Fairphone 3 with USB debugging on: its
  PID, its interfaces and MTP's interface number, on all three
  configurations.

### 3.3 Which interface the driver owns, and identification

- **The interface is the one the device node stands for, never "the first
  that looks right".**
  - An interface node's hardware id ends in `&MI_nn`. It is read with
    `IoGetDeviceProperty(DevicePropertyHardwareID)`, which `libusb0` and
    `uac1` already import on 98 SE. The driver serves interface `nn`
    only.
  - A node without `&MI_` is a whole device. The driver serves it only if
    its active configuration has exactly one interface.
  - A whole device with more than one interface (a composite the stack
    did not split) is refused: start fails, and siblings are never
    claimed. Such a phone needs xHCI98 or NUSB on 98 SE, or a stack that
    splits; the readme says so.
- **Configuration:**
  - For an interface node: the configuration descriptor its PDO returns,
    and a selection listing only interface `nn`, which the stack's
    composite parent passes on. Whether each of the four stacks takes
    such a selection from a child is read in M0.
  - For a single-interface device: a normal selection of configuration 1.
  - If M0 shows that a stack wants another form (for example
    `SELECT_INTERFACE` alone on a configured child), the driver follows
    that stack's form, and this section records it.
- **Identification: from descriptors only.** No protocol runs in driver
  start.
  - Every descriptor byte is untrusted. Each read must succeed (the IRP
    and `USBD_STATUS_SUCCESS`) and return exactly the length asked; the
    configuration descriptor is then walked whole by the driver's own
    bounded walk (`sys/cfgwalk.c`, a pure function tested on the build
    host with truncated and malformed configurations: `cfgtest.c`): every
    record's `bLength` checked against what remains before any field of
    it is read, the header's `wTotalLength` equal to what arrived, its
    `bConfigurationValue` not 0 and its `bmAttributes` with bit 7 set and
    the reserved bits 4 to 0 clear (USB 2.0 9.6.3; both go to USBD in the
    selection), the
    alternate-0 interfaces equal to `bNumInterfaces`, each (interface,
    alternate) once and every interface with an alternate 0, no endpoint
    outside an interface, every interface's `bNumEndpoints` equal to its
    endpoint records; on the owned interface, no endpoint 0, no reserved
    address bits and no address twice, no reserved `bmAttributes` bits
    (bulk: bits 7 to 2; interrupt: bits 7, 6, 3 and 2, and bits 5 and 4,
    the usage type, above 01b at SuperSpeed or set below it: USB 2.0
    table 9-13, USB 3.2 table 9-26), and packet sizes valid for the type
    and the speed. Every count is checked against its limit before it is
    raised: a fourth endpoint record on the owned interface, more than 30
    in one setting, more than 512 in a configuration or more than 64
    settings are refused. The device descriptor is checked the same way
    (18 bytes, its type, `bMaxPacketSize0`, at least one configuration).
  - **The speed** comes from what the device states, not from packet
    sizes alone. SuperSpeed is a device descriptor with `bcdUSB` 0300h or
    more and `bMaxPacketSize0` 09h (USB 3.2 9.6.1: at SuperSpeed 09h
    encodes 512; at the other speeds the field is 8 to 64). At SuperSpeed
    each owned endpoint must be followed at once by its 6-byte SuperSpeed
    endpoint companion (type 30h); bulk is 1,024 with `bMaxBurst` up to 15,
    and a bulk endpoint's MaxStreams is ignored, since no stream is opened
    (98 SE's and ME's USBD have no streams interface); interrupt is 1 to
    1,024 (1,024 when `bMaxBurst` is more than 0, at most 2) with a zero
    `bmAttributes` and a `wBytesPerInterval` of 1 to the packet size times
    (`bMaxBurst` + 1). Below SuperSpeed no companion may be there; high speed
    is a bulk size of 512 (the only one high speed allows and full speed
    does not), with interrupt 1 to 1,024 and the high-bandwidth
    transactions where USB 2.0 table 9-14 allows them (one more for 513 to
    1,024 bytes, two more for 683 to 1,024); full speed is bulk 8 to 64 and
    interrupt 1 to 64. Reserved bits are refused. The interrupt endpoint's
    `bInterval` is 1 to 255 at full speed and 1 to 16 at high speed and
    SuperSpeed. The device descriptor's `bMaxPacketSize0` must fit the speed
    the configuration establishes: 64 at high speed, 8 to 64 at full speed,
    09h at SuperSpeed. `GET_INFO` reports the speed (3 at SuperSpeed). The
    64 KiB segment and `MaximumTransferSize` hold whole packets at every
    speed, and the interrupt poll reads one interval's worth (at
    SuperSpeed the companion's `wBytesPerInterval`, else the packet size
    times its transactions; at most 3,072 bytes). Under 98 SE's added USB 2.0 stack the same phone runs at
    high speed and gives that speed's descriptor set (512-byte bulk, no
    companions); under xHCI98 a SuperSpeed phone gives the SuperSpeed set.
    Neither is assumed: the first real-phone reading shows what each sends.
  - The test reads every byte through a bounds-checking
    accessor, so a read past the end fails it, and asserts the rule each
    malformed variant is refused by. Only a configuration the walk accepted reaches
    USBD's `USBD_CreateConfigurationRequestEx`, with a list of
    `bNumInterfaces` + 1 zeroed entries naming the owned interface alone.
  - The owned interface has exactly one bulk IN, one bulk OUT and one
    interrupt IN endpoint, and its class is `06/01/01` or `FF`. An `FF`
    interface is only ever bound through a listed or hand-picked model
    (3.2), so the INF line has already named it.
  - The driver does not read Microsoft OS descriptors. Microsoft's page
    "Microsoft OS 1.0 Descriptors Specification" lists the compatible id
    "MTP" there. But the specification it offers for download comes under
    a licence agreement whose grant for drivers names only Windows Vista
    and 7, and reading string `0xEE` disturbs some devices (Microsoft's
    page "Microsoft OS Descriptors for USB Devices"). An owner question
    (Q13) asks whether that should change.
  - Anything else fails start. Device Manager then shows the device with
    a problem, rather than a driver that does nothing.
  - Whether a device answers PTP at all is the program's business. It
    reads GetDeviceInfo (an MTP device names the vendor extension
    `microsoft.com`), and shows "This device does not answer as a phone
    or camera (PTP/MTP)." on the device's node when it does not.

## 4. The transport: `usbscan.sys` or a driver of ours

### 4.1 What `usbscan.sys` offers

Read from the driver kit headers only (`AGENTS.md`, "Sources"):

- The 98 DDK's `SCAN_DDK\SRC_SCANCAM_INC_USBSCAN.H` has these controls:
  - `IOCTL_GET_VERSION`;
  - `IOCTL_CANCEL_IO`: a local abort of a pipe's pending transfers;
  - `IOCTL_WAIT_ON_DEVICE_EVENT`;
  - `IOCTL_READ_REGISTERS` and `IOCTL_WRITE_REGISTERS` (vendor requests);
  - `IOCTL_GET_CHANNEL_ALIGN_RQST`, `IOCTL_GET_DEVICE_DESCRIPTOR`;
  - `IOCTL_RESET_PIPE`, `IOCTL_GET_USB_DESCRIPTOR`.
- It has no control that sends a class request to the interface.
- The WDK 7.1 header adds `IOCTL_SEND_USB_REQUEST`,
  `IOCTL_GET_PIPE_CONFIGURATION` and `IOCTL_SET_TIMEOUT`. Those are the
  later systems' driver.
- Which version ME's `usbscan.sys` is, and whether it answers
  `IOCTL_SEND_USB_REQUEST`, is unverified. A guest call would read it;
  the binary is not read.

### 4.2 What MTP needs that 98 SE's `usbscan.sys` lacks: protocol recovery

- A local abort (`IOCTL_CANCEL_IO`) stops the host's transfer, but the
  device does not know. It is still in its data phase:
  - after an aborted GetObject it goes on sending the rest of the object;
  - after an aborted SendObject it waits for the rest.
- Telling the device needs the class requests of the USB Still Image
  Capture Device Definition 1.0:
  - Cancel Request (`bRequest` 64h, class, interface, host to device,
    6 bytes: the cancel code 4001h and the transaction id);
  - Get Extended Event Data (65h);
  - Device Reset Request (66h);
  - Get Device Status (67h, device to host).

  Without them the choices left after an abort are to drain the rest of
  the object (possibly gigabytes), or to replug. The values are those of
  the specification's 5.2, with the errata's `wIndex` (7.5). QEMU's
  `usb-mtp` answers 64h with 4001h (`hw/usb/dev-mtp.c`), which agrees.
- A hung read can be stopped with `IOCTL_CANCEL_IO` from another thread,
  if NTKERN lets that call through while a read blocks (the M0 question of
  section 5.6). Per-call timeouts (`IOCTL_SET_TIMEOUT`) are a later
  addition and absent on 98 SE.
- A device bound to `usbscan.sys` is an STI device, and on ME WIA's
  service may open it beside the program. This is unverified; it does not
  arise with a driver of ours.

### 4.3 The options

| Option | What it is | Cost | Verdict |
|---|---|---|---|
| (a) INF binding `06/01/01` to `usbscan.sys`, protocol in the program | no kernel code | an INF, about 60 lines | A local cancel works; the device's side does not recover without draining or a replug; class `FF` phones still need model lines; on ME possible contention with WIA. Not taken |
| (b) `USBMTP.SYS`, a WDM transport of ours | bulk IN/OUT, interrupt IN, the four class requests, timeouts, abort, an exclusive claim | 2,000 to 2,500 lines, plus its import gate and legs | **Taken** (owner, 2026-11-16). Same behaviour on 98 SE and ME and under every stack; cancel and recovery as section 7.5 |
| (c) Integration into Windows: a WIA driver on ME, a shell namespace view, a drive letter | [mtp-estimate.md](mtp-estimate.md) 4, routes C to E | 4 weeks to 6 months each | Not asked for (owner, 2026-11-16); left out |

The kernel driver does not parse MTP: it moves bytes. A protocol fault
cannot stop Windows, and the protocol is tested on the build host.

## 5. `USBMTP.SYS`: the handle and transfer contract

### 5.1 Handles

- **Interface version.** The controls below are interface 1.0, with
  1.1 adding only `GET_INFO`'s power fields (5.3; the standby rework of
  5.4).
  `IOCTL_USBMTP_GET_INFO` reports it, and the library refuses a driver
  whose major version differs from its own. A new minor version only
  adds controls or fields at the end of a structure, and is announced in
  `cbSize`.
- **Every open starts as an info handle.** It may call `GET_INFO` and
  `GET_STRING` only. Any number of info handles may be open.
- **Owner handle:** a handle on which `IOCTL_USBMTP_CLAIM` succeeded.
  - There is at most one per device. A second claim fails with
    `STATUS_SHARING_VIOLATION`, so two programs never share a PTP session.
  - The claim returns a token, the device's generation (5.2) and the
    claim's epoch. The token is made fresh for every claim. The epoch is a
    per-device 32-bit counter, raised by every claim and never reused, so
    a handle bound under an earlier claim of the same device object can
    never act under a later one. It does not wrap: when it would pass
    FFFFFFFFh, `CLAIM` fails until the device is plugged in again (a new
    device object starts at 1). No program reaches that many claims.
  - A claim made while the previous one is still releasing (below) fails
    with `STATUS_DEVICE_BUSY`; the program retries.
  - The owner handle may make every call.
  - A handle claims once: after its claim is released, a second `CLAIM`
    on the same handle fails (`STATUS_ACCESS_DENIED`); the program opens
    a new handle to claim again.
- **Bound control handle:** an info handle on which `IOCTL_USBMTP_BIND`
  succeeded with the owner's token, generation and epoch, while the claim
  is active.
  - A handle binds once, to one claim; a second `BIND` on it fails.
  - It may call `ABORT`, `EVENTS`, `CLASS_REQUEST`, `RESET_PIPE` and
    `WAIT_EVENT`, besides the info calls.
  - It never reads or writes the bulk pipes.
  - It lets another thread of the owning program cancel, recover and read
    events while the owner handle is blocked in a transfer.
- **The token's purpose:** it keeps a second program from disturbing a
  session by accident. It is a 64-bit value made from a counter and the
  tick count, not a security boundary: 98 SE and ME have none between
  programs. On every call of a bound handle the driver checks that the
  claim is active and that the handle's epoch is the claim's.
- **The claim's states:** FREE, ACTIVE, RELEASING. They change only under
  the device's one spin lock (5.4). Admission of every call (rights,
  epoch, claim ACTIVE, slot IDLE, taking the slot) happens under that
  same lock, in one step. So a call is either admitted before revocation,
  and then stopped by the release like any pending request, or refused
  after it. Nothing is submitted for a call that was not admitted. The
  same holds for the calls that act without a slot of their own: `ABORT`
  and `EVENTS` stop (`SlotStopEpoch`), `EVENTS` start (`SlotPollStart`,
  with the started gate), `SET_TIMEOUTS` and a `WAIT_EVENT` of 0 ms check
  the handle's claim in the very hold of the lock in which they act, so a
  handle of an earlier claim reaches nothing of a later one
  (`slotmodel.c` checks it for `ABORT`).
- **Release** is one routine, idempotent, entered by whichever comes first
  of:
  - `IRP_MJ_CLEANUP` of the owner handle;
  - `IRP_MJ_CLOSE` of the owner handle, if no cleanup came before it;
  - removal.

  It does not wait for anything, so it may run while the owner's own
  calls are still pending, in whatever order 98 SE's and ME's NTKERN
  deliver cleanup and close:
  1. **Revoke, at once and first:** ACTIVE becomes RELEASING. From that
     moment `BIND`, every owner call and every bound call fail
     (`STATUS_ACCESS_DENIED`), so nothing new is submitted or bound.
  2. **End what is pending:** the "Release, removal" event of 5.4 on every
     slot, with `STATUS_CANCELLED`. That covers the owner's reads, writes
     and class requests, every bound handle's `WAIT_EVENT`, and the
     interrupt poll. Their lower IRPs are cancelled.
  3. **Finish later:** `SlotFinish` sets the claim FREE when the last
     slot becomes IDLE. No thread waits for that.

  A second entry (close after cleanup, removal after either) finds
  RELEASING or FREE and returns.
- **A bound handle's own cleanup** ends its own pending `WAIT_EVENT`
  (matched by file object) and nothing else.
- **Every caller request has a finite timeout** (5.3: no "none" timeout
  for the bulk pipes, `WAIT_EVENT` at most 60 s). A timeout bounds the
  call only as far as the stack honours the cancel it sends (5.4, "Waits
  and bounds"). Where M0 shows it does, a system that holds back cleanup
  and close until a process's pending calls end still releases the claim
  within the longest timeout. Where it does not, release waits on the
  stack, and the record says so.
- **M0 records the order** in which NTKERN sends cancel, cleanup and close
  for a handle with calls outstanding (5.6, item 3). If it differs from
  what this routine expects, this section changes before code.

### 5.2 Instances and generations

- Each FDO gets a generation number at `AddDevice`: a 32-bit counter of
  the driver, which never repeats within a boot. It does not wrap: past
  FFFFFFFFh, `AddDevice` fails until Windows is restarted.
- The device is named `\Device\UsbMtp<n>` and linked as
  `\DosDevices\USBMTP<n>`, n 0 to 15, the lowest free. The program opens
  `\\.\USBMTP<n>`, as `usbprn`'s monitor opens `\\.\USBPRN<n>`
  ([printing.md](printing.md) 3.2). No device interfaces, no SetupDi.
- After an unplug and a quick replug, the new device may get the same n.
  - The program keeps, per device, the generation it read. On every
    rescan it reads `GET_INFO` again; a different generation is a
    different device.
  - A `BIND` carrying an old generation fails.
  - Handles opened on the old FDO fail every call with
    `STATUS_DELETE_PENDING`; they cannot reach the new device.

### 5.3 Controls

All controls are `FILE_DEVICE_UNKNOWN`, `METHOD_BUFFERED`,
`FILE_ANY_ACCESS`. Input and output lengths are exact unless a range is
given; any other length fails with `STATUS_INVALID_PARAMETER` before
anything is sent to the device. Structures are packed to 4 bytes and
little-endian.

| Control | Rights | Input | Output | Notes |
|---|---|---|---|---|
| `GET_INFO` | any | 0 | 40 to 52, as the caller asks (52 from interface 1.1, the standby rework of 5.4; 1.0 drivers report 44): `cbSize` (the driver's own size), interface major and minor (2 x UINT16), driver version (UINT32), generation, n, VID, PID, bcdDevice, interface number, class, subclass, protocol, wMaxPacketSize of bulk IN, bulk OUT and interrupt IN (the size bits), speed (1 full, 2 high, 3 SuperSpeed), flags (claimed; removal pending), 2 reserved; then the extension past the 40-byte base: the endpoint addresses of bulk IN, bulk OUT and interrupt IN, 1 reserved; then (1.1, offset 44) the power gate's reasons (1 system, 2 device, 4 a system standby prepared or accepted, 8 draining, 16 another component's device query prepared or accepted), what is still out while draining (0 unless draining; then bit 0 bulk IN, 1 bulk OUT, 2 control, 3 interrupt, 4 WAIT_EVENT, 5 completion tails still returning requests; nonzero exactly while draining), that mask at the last refused query, 1 reserved, the refused queries (UINT16) and the sets passed down with requests out (UINT16), both counted modulo 65536: a reader compares with its own earlier reading (5.4, "What user mode can read") | the extension serves 7.5's check of Get Device Status's endpoint parameters; a later one grows the structure the same way (5.1) |
| `GET_STRING` | any | 4: index (1 to 255), reserved, language id (0: the first the device lists) | 256 (buffer); the call returns the descriptor's own `bLength` bytes, checked to be even, at least 2 and no more than what arrived | |
| `CLAIM` | info | 0 | 16: token (UINT64), generation, epoch | fails if claimed, releasing or removal pending |
| `BIND` | info | 16: token, generation, epoch | 0 | once per handle, only while the claim is active; fails `STATUS_ACCESS_DENIED` on any mismatch |
| `CLASS_REQUEST` | owner, bound | 12: request (1 Cancel, 2 Get Extended Event Data, 3 Device Reset, 4 Get Device Status), transaction id (Cancel only, else 0), data length | Cancel and Reset: 0. Get Device Status: data length 4 to 64. Get Extended Event Data: 8 to 512 | The driver builds `bmRequestType`, `bRequest`, `wValue` 0, `wIndex` the owned interface and `wLength` itself, and the Cancel data. No caller field reaches the setup packet except the transaction id and the length within these caps. Slot CTL. A 2 s timeout cancels the lower IRP; it bounds the call only as 5.4, "Waits and bounds", says |
| `RESET_PIPE` | owner, bound | 4: pipe (1 bulk IN, 2 bulk OUT, 3 interrupt IN) | 0 | clears a halt (USBD's reset pipe); slot CTL; fails `STATUS_DEVICE_BUSY` unless the target pipe's slot is IDLE (the interrupt pipe: `EVENTS` stop first); its 2 s timeout as `CLASS_REQUEST` |
| `ABORT` | owner, bound | 8: pipe mask (bit 0 bulk IN, 1 bulk OUT), wait in ms (0 to 5,000) | 4: pipes still not quiet (the same bits) | cancels the pending transfers on those pipes, waits up to the given time for them to finish, and reports which have not; it never waits longer (5.4) |
| `EVENTS` | owner, bound | 8: 1 stop or 2 start, wait in ms (0 to 5,000) | 4: 1 if the poll is stopped and its URB has finished | stop cancels the interrupt poll and waits up to the given time; queued events are kept. Start re-arms it |
| `SET_TIMEOUTS` | owner | 8: bulk IN, bulk OUT, in ms, each 100 to 600,000 | 0 | defaults 10,000 and 10,000. There is no interrupt wait: `WAIT_EVENT` carries its own time-out per call |
| `WRITE_ZLP` | owner | 0 | 0 | one zero-length bulk OUT packet |
| `WAIT_EVENT` | owner, bound | 4: timeout in ms, 0 to 60,000 | 64: length (UINT32), flags (bit 0: events were lost since the last call), 56 bytes of event data | returns at once if an event is queued; one wait pending per device, a second fails `STATUS_DEVICE_BUSY`. A time-out of 0 only looks at the queue: `STATUS_IO_TIMEOUT` at once when it is empty. The Win32 error is the same (121) as a real time-out; the library tells them apart by the time-out it passed. A zero-length interrupt transfer is queued as an event of length 0, which the library reads as "no container" and skips, not as an error |
| `ReadFile` | owner | length the bulk IN wMaxPacketSize to 1,048,576, a multiple of it | the bytes read | ends at a short or zero-length packet (0 bytes is a valid result); one read pending at a time. The library's first read of a data phase is exactly one packet (7.2) |
| `WriteFile` | owner | length 1 to 1,048,576 | - | never adds a zero-length packet; one write pending at a time |

- Reads are a whole number of packets, so a device can never send more
  than the buffer holds (no babble). The library then does the container
  framing (7.2).
- The driver splits a read or write into URBs of at most 64 KiB; the
  configuration is selected with `MaximumTransferSize` 64 KiB.
- A short packet ends a read early, and no further URB of it is sent.
- **The interrupt pipe** is read continuously from `CLAIM` to release,
  with one URB pending, except while `EVENTS` has stopped it (for a pipe
  reset or a device reset, 7.5).
  - Completed transfers go into a queue of 16 events.
  - When the queue is full, the oldest event is dropped and the overflow
    flag set. The library answers an overflow with a full refresh of that
    device (7.4).
  - Between claims, nothing reads the interrupt pipe.

### 5.4 Lifetime: one lock, one request per slot, one completion point

The driver moves one transaction at a time per device, and speed does not
matter. So the scheme is kept small enough to model and test as a table.

**One lock.** Each device has one spin lock. Every state change below
happens under it, and nothing else is ever held with it. The driver never
calls down (`IoCallDriver`, `IoCancelIrp`) and never completes an IRP
while holding it, because a lower driver may complete inline and re-enter.

**Slots.** A device has five slots, each holding at most one request:

| Slot | Requests it takes | Lower request |
|---|---|---|
| BIN | `ReadFile` | bulk IN URB |
| BOUT | `WriteFile`, `WRITE_ZLP` | bulk OUT URB |
| CTL | `CLASS_REQUEST`, `RESET_PIPE`, `GET_STRING` | control or pipe URB |
| INT | the driver's interrupt poll (no caller IRP) | interrupt IN URB |
| WAIT | `WAIT_EVENT` | none |

- A second request for a busy slot is refused at once with
  `STATUS_DEVICE_BUSY`.
- Each slot that has a lower request owns one lower IRP and URB. They are
  allocated when the device starts (`IoAllocateIrp`) and reused, and
  freed only after removal has seen the slot idle. So an `IoCancelIrp` is
  always on a live IRP. Whether it is still this request's transfer is a
  slot state read under the lock, not a sequence number.

**The contract is code.** The transitions are written as
`src/image/usbmtp/sys/slot.c`. It is pure C with no kernel calls: the
driver calls its functions with the lock held, gives it the few
primitives it needs under the lock (`KeSetTimer`, `KeCancelTimer`,
`IoSetCancelRoutine`, `Irp->Cancel`) through a table, and performs the
actions it returns (`IoCallDriver`, `IoCancelIrp`, `IoCompleteRequest`)
with the lock released. The counts and gates the driver's lifetime rests
on (the open count, the dispatch pins, the admissions, the power worker,
the power gate) are in it as well, so the model covers them too. `src/image/usbmtp/slotmodel.c`
drives the same file through every interleaving of its events (below).
Where this section and `slot.c` differ, `slot.c` and its model decide,
and this section is corrected.

**How `slot.c` meets each point:**

- **Pins.** A pin is a count in the slot, taken under the lock before
  every outside call made for a request, and dropped under the lock
  after the call returns. It is taken for the admitter's `IoCallDriver`
  and its re-check, a completion path's next-segment `IoCallDriver` and
  re-check, and every `IoCancelIrp`. A slot with a pin
  cannot finish, so it cannot take the next request. So an `IoCancelIrp`
  made by a release, `ABORT` or `EVENTS` aimed at request N can never
  reach request N+1.
- **The lock is released before every `IoCancelIrp`, `IoCallDriver` and
  `IoCompleteRequest`**, at every place `slot.c` returns one: admission,
  re-check, lower completion, cancel routine, timer DPC, release,
  removal, `ABORT`, `EVENTS`.
- **Inline completion inside a submit.** The submitter's pin is held across
  `IoCallDriver` and its re-check. A completion that runs inline cannot
  finish the request, and removal cannot free the slot, before the
  submitter has re-taken the lock and dropped the pin.
- **One completion point.** `Finish` captures the IRP's number, outcome and
  byte count together under the lock and returns them as one action. The
  IRP is completed outside the lock from those captured values only; the
  completion touches no slot or device state.
- **One immutable outcome.** The outcome is written once, by the transition
  that moves the state to DONE.
- **The wait slot** has its own transitions without a lower request: an
  event, its time-out, its cancel and release each fix the outcome if it
  is still open, and each retires the timer and the cancel routine as far
  as it can (below).
- **Retirement only on the final outcome.** The timer is retired when
  `KeCancelTimer` returns TRUE, or else by the DPC clearing its flag as its
  last act. The cancel routine is retired when `IoSetCancelRoutine(NULL)`
  returns the routine, or else by the routine clearing its flag as its last
  act. Neither happens while another segment is still to be sent.
- **An IRP already cancelled at admission** is a handshake, not "undo and
  refuse". The routine is set first; then `Irp->Cancel` is read. If
  `IoSetCancelRoutine(NULL)` then takes the routine back, the IRP is
  completed cancelled at once. If not, the routine is running: the outcome
  is fixed as cancelled, and the IRP is completed only when the routine
  has cleared its flag.
- **`RESET_PIPE`** is admitted on the control slot only if its target slot
  is idle, and reserves the target (state RESERVED) until the reset has
  finished; the target refuses requests while reserved.
- **No fallback abort.** The design kept a per-slot abort IRP sent by a
  worker for a stack that ignored `IoCancelIrp`. M0 read `IoCancelIrp`
  retiring a pending bulk IN, bulk OUT, interrupt IN and a class request
  held by the device on both of 98 SE's stacks and on ME
  ([2026-11-16-mtp-m0.md](../testing/2026-11-16-mtp-m0.md)), so the driver
  has none: a stop is `IoCancelIrp` alone, and no worker of the slots
  exists. A pending pipe reset's cancel was not run (M0, "Not run"): a
  reset that a stack never retires holds every wait for quiet (release,
  STOP, the power-down, REMOVE's rundown) until it does, which "Waits and
  bounds" already says of `RESET_PIPE`. The probe (`MTPPRB.SYS`) keeps its own copy of the earlier
  `slot.c` with the abort, for its `ABORT_PIPE` readings.
- **The completion tail.** `Finish` hands the IRP's pointer, outcome and
  byte count by value to a tail and frees the slot at once, so the next
  request can be admitted. The tail calls `IoCompleteRequest`, touching no
  slot, then `SlotTailDone` under the lock: its last device access.
  Removal waits for every tail. The last device access of the cancel
  routine and the DPC is their End transition, and of a lower completion
  routine `SlotLowerDone` or `SlotPollDone`. After it they only return.
- **A request's result travels with it.** What a tail needs beyond the
  outcome and byte count of its `SA_COMPLETE` (the lower `NTSTATUS` behind
  an `SO_ERROR`) is stored by the lower completion, under the lock and
  before the slot can be freed, in the caller's IRP that the tail will own
  (its `IoStatus.Status`). The tail reads no slot storage: the slot may
  already hold the next request. Bytes reach the caller's buffer only from
  a URB that succeeded (the IRP's and the URB's status), by the URB's own
  count, checked against the space left.
- **The started gate.** Admission is refused (`STATUS_DEVICE_NOT_READY`),
  under the same hold of the lock as `SlotAdmit`, until a successful
  `IRP_MN_START_DEVICE`, after a failed one, and from the start of
  `IRP_MN_QUERY_STOP_DEVICE`, `IRP_MN_STOP_DEVICE` or
  `IRP_MN_REMOVE_DEVICE`. QUERY_STOP is refused while a request is out
  (`CANCEL_STOP` reopens the gate). STOP is never failed: it closes the
  gate, stops every slot and stays pending, at `PASSIVE_LEVEL` and with no
  time limit, until every slot is idle and every tail done; only then does
  it go down. So a later START, which configures with the gate closed and
  opens it on success, never changes a pipe under a transfer or an abort.
  A request's own fields (its length, a control slot's request kind, a
  reset's target) are written only after its admission, under that same
  hold of the lock, so a refused request changes nothing of an admitted
  one. Waits are measured in elapsed ticks, not in counted delays.
- **Power on 98 SE and ME.** `DO_POWER_PAGABLE` is set in `AddDevice`
  before `DO_DEVICE_INITIALIZING` is cleared (Microsoft requires it of WDM
  drivers on 98 and ME). A power IRP is an IRP without a handle: it is
  admitted like the others (below) and handled at `PASSIVE_LEVEL` under its
  admission, `PoStartNextPowerIrp` then `PoCallDriver`. One that arrives
  above `PASSIVE_LEVEL` is queued on the device by the IRP's own list entry
  for the device's one preallocated work item, carrying its admission, so
  nothing on the power path allocates and nothing on it can fail
  (`SlotPowerQueue`). The worker forwards each queued IRP and gives that
  IRP's admission up in the hold of the lock that takes the next one
  (`SlotPowerNext`); the hold that finds the list empty is its last access
  to the device.
  - **Two lifetimes at an accepted query** (the third review of the
    standby rework, 2026-11-17). An accepted query is passed down with a
    completion routine that takes the lower drivers' answer, and the
    dispatch does not wait for it. That routine may run inside the
    `PoCallDriver`, or on another processor before the dispatch or the
    worker takes the lock again, so it cannot share their protection: had
    the query's one admission been handed to it (the stage-2 plan before
    that review: the worker then giving none up, `SlotPowerNext(...,
    !kept)`), its answer could give it up while the worker still had its
    next hold to make, and REMOVE could drain the admissions, detach and
    delete the device under the worker (`g_Running` keeps the image for
    `DriverUnload`, not the device). So the two are kept apart. The
    dispatch or worker keeps the forwarding IRP's admission to its own last
    device access, as for every power IRP (`ActiveDone` after the
    dispatch's return from `PowerForward`; `SlotPowerNext(..., 1)` on the
    worker, an accepted query included). The query has an admission of its
    own, taken in the very hold that decides to forward it
    (`SlotQueryDecide`'s 1; safe without `SlotActiveEnter`'s checks because
    the caller's admission keeps REMOVE's drain from having ended) and
    given up by the completion routine's one hold (`SlotQueryEnd`, whatever
    the query's state by then), its last device access. Neither waits for
    the other, and nothing waits for the query's completion but REMOVE's
    drain, as before. `slotmodel.c` plays the lower answer as an actor of
    its own, before or after the dispatch's or worker's last step, and
    catches both halves removed: the worker riding on the query's
    admission (`MUT_WORKER_RIDES_QUERY`) and the completion holding none
    (`MUT_QUERY_UNPROTECTED`), each as "device touched after its free".
  - **A power IRP that is not admitted** (the device detached, closed to
    such IRPs, unpublished or deleted) is completed `STATUS_DELETE_PENDING`
    from the driver's globals, touching nothing of the device, and always
    at `PASSIVE_LEVEL`, which 98 SE and ME require of power IRP completion
    (Microsoft's "Differences in WDM Versions"). One that came at
    `PASSIVE_LEVEL` is completed in its dispatch. One that came above it
    (not expected: the device object is `DO_POWER_PAGABLE`) goes to the
    **late hand-off**: a driver-wide list, a static work item and a count
    (`g_LateRunning`), under the driver-global lock. The IRP is put on the
    list, and the count raised, in the same hold of that lock in which the
    dispatch found the device deleted or unpublished, or, when its
    admission is refused, while it still holds its pin. So the count is
    raised before the deleter can publish the deletion (it waits for the
    pins and publishes in the hold that finds none), and so before
    `DriverUnload` can read it. The late worker completes each IRP at
    `PASSIVE_LEVEL` (`PoStartNextPowerIrp`, `IoCompleteRequest`) and
    lowers the count as its last act. The deleter never waits for it.
    `DriverUnload` waits for the count, unless it runs on the late
    worker's own thread: a completion the worker makes may close a
    zombie's last handle, and that close deletes the last device and runs
    `DriverUnload` inside the worker's own call, which then must not wait
    for itself (the dependency of 2026-11-16's fifth review). The worker
    then returns through its own code, as every deleter returns after
    `IoDeleteDevice` (the image-lifetime point below). `slotmodel.c`
    models the hand-off and its four mutants (below).
  - **The power gate** has five reasons to be closed, kept apart
    (revised 2026-11-17 with the standby rework below): the
    system's (`SLOT_PD_SYS`, a system state below working has been set),
    the device's (`SLOT_PD_DEV`, a device state below D0), a system
    standby being prepared or accepted (`SLOT_PD_QUERY`), another
    component's device query below D0 being prepared or accepted
    (`SLOT_PD_DQUERY`), and draining (`SLOT_PD_DRAIN`: a request the gate
    stopped has not yet come back from the stack). It is open only when
    none holds. Closing it for any of the first four (`SlotPowerDown`,
    `SlotQueryBegin`) adds that reason and DRAIN and stops every slot
    (`STATUS_CANCELLED`) in one hold of the lock: from then admission is
    refused (`STATUS_DEVICE_NOT_READY`, as is a `CLAIM` or an `EVENTS`
    start), no next segment is sent, the poll does not re-arm, and no
    submission not yet committed is ever sent (the submission, below).
    SYS ends only when an S0 has been forwarded, waited for at
    `PASSIVE_LEVEL` and succeeded; DEV the same with a D0 (`SlotPowerUp`);
    QUERY and DQUERY as the standby rework below says, each by its own
    kind of IRP only: no system IRP ends the device query's reason, and no
    device IRP the system's. DRAIN is ended by no power IRP: only by
    `slot.c`'s own check, in the hold of the lock in which the last slot
    becomes idle with no completion tail out (in the closing hold itself
    when nothing was out). So no S0, D0, refusal or lapse can reopen the
    gate over a request the stack still holds. Before 2026-11-16's sixth
    review one flag served both the system and the device, so any
    successful S0 opened the gate for a device that might still be in D3;
    before the standby rework a successful S0 or D0 reopened it whatever
    was still out.
  - **Standby: bounded quiescence at the query, with a veto, and a
    bounded, quarantined set (2026-11-17: adopted on the
    owner's delegation for this rework and confirmed by him the same day; revised
    that day after the review of `8557bdb`).**
    - **Why.** In one standby of five taken during a transfer on 98 SE
      under NUSB's EHCI the gate cancelled its requests at the system's
      SET_POWER and the stack returned none of them; the driver's wait for
      quiet had no bound, so it held the system IRP and the standby never
      finished (the third dependency below; which request was held is not
      known). Microsoft's contract gives a function driver one point at
      which it may refuse a system sleep, the query ("Failing a System
      Query-Power IRP in a Filter or Function Driver": allowed when the
      transition would lose data, and to be uncommon), and none at the
      set, which it must pass down and may not fail ("Handling
      IRP_MN_SET_POWER for System Power States"). After a query, failed or
      not, the system may send a set to the queried state, to another
      state or to the current one, or another query ("Handling
      IRP_MN_QUERY_POWER for System Power States"), and a set may come
      with no query at all (a failing battery). So the drain moves to the
      query, where it may end in a refusal, and a set is never failed and
      never waited on without a bound. No design both guarantees that
      nothing of the driver's is below when the system sleeps and handles
      a set in bounded time while a stack keeps a request for good; the
      set's fallback (below) is where that rule is given up, and the
      record says so.
    - **The bounds**, each measured in elapsed ticks from the hold of the
      lock that closed the gate (or found it closed), before the cancels
      that hold returned are sent: a query (system or device) waits at
      most `QUIET_QUERY_MS` (3,000 ms) for quiet; a set into sleep, system
      or device, at most `QUIET_SET_MS` (3,000 ms), and then also until no
      submitter is inside a committed call (the submission, below), which
      waits on the submitter's own `IoCallDriver` returning, not on any
      request's completion. The values are choices, not measurements: the
      trace builds used so far time only their waiting lines, the one
      failure never returned in more than 1,200 s, and how long the gate's
      cancels take where they come back at all is for the rework's guest
      legs to read (15); under the emulator every one came back within
      41 ms (the load record, the rework's legs), which says nothing of a
      stack that keeps a request. They bound waiting for cancellation; they give a
      transfer no time to finish: every query below working stops the
      active slots at once, so a transfer is interrupted even when the
      standby is then refused (below). The bounds bound the driver's waits
      only: a lower dispatch, cancel routine or power handler that never
      returns is not bounded by anything here. `QUERY_HOLD_MS` (30,000 ms)
      is the lapse, a local recovery heuristic (below), not a bound on any
      wait. A test-only trace build takes both waits at 0 ms
      (`build.ps1 -Flavour standbytest`, `USBMTP_STANDBY_TEST`; never
      released): QEMU's usb-redir retires every URB the guest cancels at
      once, so under the emulator the 3,000 ms bounds are always met, and
      only a zero bound lets the guest legs reach a refusal, or a set
      passed down with requests out, with requests that come back a few
      milliseconds late. It shows those paths and their bookkeeping, not a
      stack that keeps a request (that stays the model's).
    - **Quiet** is `slot.c`'s: every slot idle with nothing outstanding
      (no lower IRP with the stack, no timer or cancel routine, no pin,
      so no submitter inside its call) and no completion tail out, the
      condition in which DRAIN ends.
    - **The submission** (the review of `8557bdb`, its first finding). An
      `SA_SUBMIT` that `slot.c` returns is an intention, performed after
      the lock is released; between the hold that emitted it and the
      submitter's `IoCallDriver` a stop, the gate and even a set's bound
      may come. Neither stopping the slot nor DRAIN retracts an action
      already emitted, so without more a set going down at its bound could
      be followed by the old submission passing a URB down to a sleeping
      device (the model's baseline found exactly that, 2026-11-17, below).
      The protocol: the submitter, with the lock held and before it
      rebuilds the lower IRP, calls `SlotSubmitCommit`. If the slot's
      request is still ACTIVE, the call is committed (`Calling` set): the
      submitter releases the lock, rebuilds the IRP, calls `IoCallDriver`,
      and clears `Calling` in `SlotSubmitReturned`. If a stop came in
      between, the submission is withdrawn: the IRP is never passed down,
      the submitter's pin is given back, and the request ends with the
      stop's outcome (a cancel the stop sent meanwhile reaches an IRP no
      driver holds, and keeps its own pin until `IoCancelIrp` returns).
      Every stop is decided under the lock, and the gate stops every slot
      in the hold that closes it, so after that hold no commit succeeds
      until the gate reopens: a submission not committed when the gate
      closes is **never sent**. One committed before it is **sent**, and a
      set into sleep (system, or another component's D3) goes down only in
      a hold that finds `SlotCalling` 0, so that URB has reached the stack
      before the sleep IRP does; if the stack does not return it, it is
      quarantined like any other (below). The same holds for a next
      segment and a poll re-arm, which are `SA_SUBMIT`s of their own. The
      re-check after the call (a stop decided while the IRP was being
      passed down cancels again) stays.
      **The rebuild** (the second review of the rework). Between the commit
      and the call the submitter reinitialises the very IRP that a stop's
      `IoCancelIrp` would touch (`IoInitializeIrp` rewrites its cancel
      fields; the URB, the stack location and the completion routine are
      written after it). The pin keeps the IRP alive but does not make that
      reconstruction exclusive, and the post-call re-check repairs a lost
      cancel, not an overlapping one. So the commit also starts a
      rebuilding phase (`Rebuilding`): while it lasts a stop marks the slot
      STOPPING with its reason and defers its cancel (no `SA_CANCEL_LOWER`
      is emitted). When the IRP is whole the submitter takes the lock and
      calls `SlotSubmitBuilt`: still ACTIVE, the phase ends and the IRP is
      handed to `IoCallDriver`; stopped during the rebuild, the submission
      is withdrawn like one stopped before its commit, the deferred cancel
      being honoured by never passing the IRP down (it is never also
      cancelled). A stop after `SlotSubmitBuilt` cancels as before: the IRP
      is whole, and an `IoCancelIrp` racing the `IoCallDriver` that follows
      is the I/O manager's ordinary case, which the post-call re-check
      covers when the cancel came before the stack had the IRP. No cancel
      of an earlier stop can be running into a rebuild: a commit succeeds
      only on an ACTIVE request, every cancel is made for a STOPPING one,
      and a slot does not take its next request while a cancel's pin is
      out. `slotmodel.c` plays the rebuild and the hand-off as steps of
      their own and fails an `IoCancelIrp` that overlaps a rebuild, a
      rebuild while an `IoCancelIrp` on the IRP is in progress, and a
      withdrawn IRP that is also cancelled.
    - **The states** of the system's query (`slot.c`'s `Query` field with
      the gate's reasons; each transition is one hold of the device lock;
      every path, refusal, expiry and rollback included, runs and completes
      the IRP at `PASSIVE_LEVEL`, on the dispatch or the power worker
      alike, through the one `PowerForward`). The device query has the
      same states in `DQuery`, with its own events (below):

      | State | Gate | Entered | Left |
      |---|---|---|---|
      | working | open, unless DEV or DRAIN stands | start; a refusal (ours or the lower drivers'); any S0; the lapse | a system query below working (preparing); a set into sleep (asleep) |
      | preparing | QUERY, DRAIN | `SlotQueryBegin`: every slot stopped | `SlotQueryDecide` at quiet or at the bound: quiet, so forwarded; not quiet, so refused (working, and DRAIN stays until the last request returns) |
      | forwarded | QUERY | quiet at the decision | `SlotQueryEnd` with the lower drivers' final answer: success, accepted; failure, working (the rollback is the driver's own: it does not wait for an S0) |
      | accepted | QUERY | the lower drivers accepted | the set into sleep (asleep); an S0, whatever its result (working); a repeated query (preparing again, quiet at once); the lapse (working, `SlotQueryExpire`) |
      | asleep | SYS (and DEV, DRAIN where they stand) | a system set below working, after the bounded drain | an S0: QUERY ends whatever its result, SYS on its success only (a failed S0 leaves SYS, and the gate closed) |

    - **A system query below working** (`IRP_MN_QUERY_POWER`, system
      type): one hold closes the gate for QUERY and stops every slot
      (`SlotQueryBegin`); the cancels go down; the driver waits until quiet
      or `QUIET_QUERY_MS`, logging `power-query-waiting` once a second; one
      hold decides by the state then, not by how the wait ended
      (`SlotQueryDecide`). Quiet: the query is passed down and the driver
      observes its final completion before anything is marked accepted (an
      immediate `STATUS_PENDING` from the dispatch is not the lower
      drivers' answer); its completion routine takes that answer in one
      hold (`SlotQueryEnd`, which also gives up the query's own admission)
      and lets the IRP complete on up with the lower status. The dispatch
      marks the IRP pending and returns `STATUS_PENDING` without waiting
      for that routine (Microsoft: a power dispatch routine must not wait
      for an event that code handling the same IRP sets). Not quiet: the query is
      **refused**: completed `STATUS_DEVICE_BUSY` (an unsuccessful status:
      `NT_SUCCESS` is false), not passed down; QUERY ends in that hold, the
      refusal is counted with the slots still out (below), and the trace
      logs `power-veto` with those slots and the time waited. A refused
      standby may have interrupted a
      transfer: the call in progress ends cancelled (995) once the stack
      returns it, the session must be recovered (7.5), and a write's
      outcome on the device is uncertain; the driver replays nothing. What
      98 SE and ME do with a refused query was read in the guests (the load
      record, the rework's legs, with a test-only build whose bounds are
      0 ms): each shows a message box and sends neither a set nor an S0
      after it; the system stays up. 98 SE: "Advanced Power Management",
      "Your computer cannot go on standby because a device driver or
      program won't allow it. ...", OK; ME: "Power Management", "Your
      computer cannot hibernate or standby because the MTP or PTP device
      (wdm-free) cannot enter into a low-power state.", Close and Help.
      Neither retried. **On 98 SE a refusal by this driver cost the
      serial ports in every guest tested** (the load record, "2026-11-17,
      follow-up", read with a probe program on both stacks, emulated
      only): both systems send this driver its
      query only after they have told programs that the machine is
      suspending (`PBT_APMSUSPEND`), and after the refusal they send
      programs `PBT_APMRESUMESUSPEND` although nothing slept (so the
      library, which takes a resume as a lost session, does so here too,
      as the gate's stop requires anyway). After that, on 98 SE, no
      program could open a COM port in the tested guests: the first open
      tried had not returned when it was last read, 20 to 220 s later
      (14 of 14 guests, both stacks), and every COM port tried after it
      waited too. Of what was tried, only a restart restored access, in
      the two guests where one was tried (one per stack): a second,
      accepted standby and its wake did not, nor did disabling
      and enabling the port in Device Manager; a printer port still
      opened, and a port held open across the refusal kept working (the
      rework's legs). A program's own refusal of the same standby
      (`BROADCAST_QUERY_DENY` to `PBT_APMQUERYSUSPEND`, which 98 SE
      answers with the same message box, before any `PBT_APMSUSPEND`)
      left the ports working, as did every accepted standby. On ME the
      ports kept working after this driver's refusal (one guest). Why 98
      SE's ports stop opening is not known. **A hypothesis only:** an
      incomplete rollback of power preparation in the serial path after
      a late refusal. By the time this driver's query comes, programs
      have been told the machine is suspending, and a serial component
      may already have stopped, closed or barred new opens in preparing
      for the standby; when the refusal aborts the transition, the
      recovery from that preparation may not restore it, and a first
      open blocked there may hold a lock that later opens wait behind.
      What was observed: the order of the broadcasts and of this
      driver's query and refusal; the ports opening after a program's
      refusal, which comes before `PBT_APMSUSPEND`; a printer port
      opening when tried first; the restart. What was not: what the
      serial port's driver was sent and when, its state, whether it had
      been prepared before this driver's query, and whether a refusal by
      another WDM driver does the same. So it is not shown that the
      lateness of the refusal is the cause, nor that the fault is in the
      serial driver; and the program-refusal controls ran with no
      transfer, so they do not isolate every condition. This driver's
      refusal on 98 SE is kept as a backstop behind the program's own
      refusal (14, Q18, decided). A query for S0 is passed
      down untouched (none is expected: the power manager sends none
      before S0).
    - **Another component's device query below D0** (`IRP_MN_QUERY_POWER`,
      device type; the review's third finding). Microsoft's "Handling
      IRP_MN_QUERY_POWER for Device Power States": a driver that succeeds
      it must first finish outstanding I/O, queue incoming I/O and start
      nothing that would interfere with the set that follows; it may fail
      it when the state would force it to abandon an operation and lose
      data, rarely. Passing it down untouched, as the text of `8557bdb`
      did, met none of that. It now takes the system query's path with its
      own reason and state: `SlotQueryBegin(SLOT_QD)` closes the gate for
      DQUERY and stops every slot ("queue incoming I/O" is the gate's
      refusal: this driver queues nothing, a request arriving is refused
      `STATUS_DEVICE_NOT_READY` and the library retries after the power
      change); a bounded wait of `QUIET_QUERY_MS`; quiet, so passed down
      and completed with the lower drivers' answer; not quiet, so refused
      `STATUS_DEVICE_BUSY`, counted and traced as a system refusal is. Its
      rollback is its own: its D3 replaces it (`SlotPowerDown` with DEV
      ends DQUERY), a D0 ends it whatever the D0's result, the lower
      drivers' refusal ends it, and so does its own lapse. A system S0 does
      not end it, and a device D0 does not end the system's QUERY
      (`slotmodel.c` plays both kinds at once, and a seeded fault that lets
      S0 end the device query is caught). After a refusal the sender may
      still send its D3: that is a set without preparation (below). A
      device query for D0 is passed down untouched. No device query has
      been seen in any standby leg, the rework's included (ME sent its D3
      with no query before it in every one); whether some other component
      sends one is not known (15).
    - **Between an accepted query and its set** the gate stays closed by
      QUERY: nothing is admitted, the poll does not re-arm, and so the set
      finds quiet and goes down at once. A repeated query in this state
      stops nothing and is accepted again at once if the lower drivers
      accept it. **Another device's refusal** after ours accepted: the
      system may send S0, its current state ("Handling IRP_MN_QUERY_POWER
      for System Power States" lists a set to the current state among
      what may follow a query), which ends QUERY whatever its result.
    - **The lapse: a local recovery heuristic, not a cancellation of the
      power transaction** (the review's fifth finding). The contract
      lists what may follow a query; it does not promise that anything
      does. In the rework's two-device legs (the load record) 98 SE and ME
      sent the accepted device its set before they queried the other one,
      so no accepted query was left standing when the other refused: ME
      then sent the first device S0 and D0, and 98 SE, which had removed
      it at the set, found it again as a new device; the lapse never came
      into play. Other orders and systems are not read (15). Should nothing come, an accepted query would keep the
      device unusable until a set, an S0 or removal. So an accepted query
      lapses `QUERY_HOLD_MS` (30,000 ms) after its acceptance (the tick of
      `SlotQueryEnd`'s success): the next admission, `CLAIM`, `EVENTS`
      start or `GET_INFO` calls `SlotQueryExpire` in the same hold of the
      lock as its own decision, which ends QUERY (or DQUERY) if
      `(unsigned long)(Now - At) >= QUERY_HOLD_MS`, right across the tick
      count's wrap (the model's self-test checks the arithmetic at the
      boundary and across the wrap, and a broken copy comparing without
      the wrap is caught). What it means, and what it does not:
      - it ends this driver's own preparation only. It says nothing about
        whether the system abandoned the standby (other devices may still
        be delaying it), and it releases nothing of the lower drivers,
        which may keep their own preparation until a set; it forfeits this
        driver's claim that an accepted query means the set finds quiet;
      - **a set after the lapse** is a set without preparation: the gate
        closes for SYS, what was admitted since the lapse is stopped and
        drained within `QUIET_SET_MS`, and the set goes down once, with
        whatever is still out quarantined. So a transfer begun after a
        lapse may be interrupted by the late set, and a lapse can cost a
        drain but never an unbounded wait;
      - **no set ever**: after the lapse the device is usable again at the
        first call that looks;
      - **an S0 without a set**, before the lapse: QUERY ends at the S0
        whatever its result (the rollback the contract describes); after
        the lapse the S0 finds nothing to end, and SYS, which was never
        set, is untouched;
      - the device query's lapse is the same, against its own acceptance;
        both reasons may lapse at one call.
      `slotmodel.c` plays a repeated query, the lapse with new traffic
      admitted after it and a delayed set, and an S0 instead of a set; it
      has no time of its own, so a lapse is an explicit step that moves
      its one clock (and so lapses every accepted query past its hold).
    - **A system set below working.** One hold closes the gate for SYS
      (ending QUERY: the set replaces the preparation) and stops every
      slot (`SlotPowerDown`); after a successful preparation nothing is out
      and the IRP goes down at once. **Without one** (no query, a query
      that lapsed or was refused, a critical sleep) the driver waits until
      quiet or `QUIET_SET_MS`, then until no submitter is inside a
      committed call, and passes the set down, once, in the hold of
      `SlotSetForward`, in either case. It never fails a set and never
      waits on one without a bound but the lower dispatch's own return. A
      set passed down with requests still out is counted (`Forced`) and
      logs `power-down-held` with their slots.
    - **The quarantine.** A request the gate stopped and the stack has
      not returned stays exactly as it is: its slot keeps it (STOPPING),
      with its lower IRP, URB, transfer buffer, completion context, the
      caller's IRP and buffer and its cancel routine; the time-out
      transfers nothing back. It is not completed early, not freed, not
      resubmitted and not replayed; DRAIN keeps the gate closed over it, so
      no S0, D0, refusal or lapse reopens the device while it is out, and
      nothing new is admitted (no other slot either: a new request on a
      quarantined device is refused `STATUS_DEVICE_NOT_READY`). Removal
      keeps its rundown requirement: REMOVE waits for that request with no
      bound, as it always did (step 4 of "Removal"); nothing is freed,
      detached or deleted on a time-out.
    - **Late and absent completions**, each handled exactly once by the
      slot's existing transitions (a stop decided first wins over the
      URB's own result). "Exactly once" means on the paths that return: a
      lower request that never comes back leaves its caller's call pending
      for good, by design (below):
      - before the bound: quiet is reached, the ordinary path;
      - after a refusal, before any set: the slot ends with the gate's
        cancel as its outcome, its tail completes the caller's IRP once
        (995), a read's bytes are not reported, the poll does not re-arm;
        when the last one returns DRAIN ends and the gate opens, QUERY
        being already gone;
      - after a set went down with it out, during the sleep or after the
        wake: the same, but the gate stays closed until S0 (and D0 where
        DEV stands) has also succeeded; on 98 SE the device is removed as
        the system sleeps (below), and REMOVE's rundown is what waits for
        it. So on the measured 98 SE paths a set without preparation that
        meets a request the stack never returns may only move the hang from
        the set into REMOVE, before the standby finishes; the fallback
        cannot promise that the machine sleeps. That is why the drain is
        made at the query, which the system sent before its set in every
        standby leg whose record names it (the load record, the NUSB hang
        included), and where a held request ends in a refusal instead;
      - never: DRAIN never ends, so every call is refused 21 until the
        device is removed, the caller's call stays pending (its process
        cannot end until then: the caller's IRP is the slot's), and
        REMOVE's rundown waits for it, logging `remove-rundown-waiting`;
        this is the cost of not freeing what the stack may still write;
      - a late success of a write: the data may have reached the device,
        the call still ends cancelled, and nothing is sent again; of the
        interrupt poll while the gate is closed: the data is not queued
        as an event (`SlotPollAccept` is 0) and not delivered, and
        `slot.c` sets `Lost`, which the driver takes into its queue's
        lost flag (`SlotTakeLost`), so the library refreshes the device
        (7.4) rather than take an event of the interrupted session as
        current (modelled: a poll's data with the gate closed is checked
        flagged lost, never delivered).
    - **Another component's D3** takes the set's path: the gate closes
      for DEV (ending DQUERY), a bounded drain of `QUIET_SET_MS`, no
      committed call, then down once, quarantine as above. So no device
      IRP brings back an unbounded wait. Its D0, and S0, end only their
      own reasons; DRAIN stays while anything is out.
    - **Removal during preparation** (the review's second finding). An
      **admitted** query (system or device) keeps its dispatch's (or the
      worker's) admission through its preparation, its decision and the
      dispatch's own last device access, and, once accepted, an admission
      of its own from the decision to the completion routine's hold that
      takes its lower answer (above, "Two lifetimes at an accepted
      query"); REMOVE waits for both (5.4, the admissions); the wait ends
      within `QUIET_QUERY_MS` plus the lower drivers' own answer. A query
      **refused admission** because removal has begun follows the
      removal path of Microsoft's "Handling a System Query-Power IRP in a
      Filter or Function Driver" (when the remove lock cannot be taken,
      the driver does not continue and completes the IRP with the failure
      status): it is completed `STATUS_DELETE_PENDING` from the driver's
      globals, at `PASSIVE_LEVEL` (on its dispatch, or by the late
      hand-off when it came above it), touching nothing of the device,
      like every power IRP refused admission. The text of `8557bdb`
      completed such a system query `STATUS_SUCCESS` on the reasoning that
      a device going away does not object; nothing in the documentation
      authorises a success that the lower drivers never gave, so that is
      withdrawn (a seeded fault restoring it is caught). The cost: a
      standby queried while this device is being removed is refused; on
      the measured 98 SE paths the removal comes after the set, not during
      the query.
    - **Power IRP ownership.** Each power IRP is either passed down once
      (`PoCallDriver`) or completed by this driver without being passed
      down, never both, and completed exactly once, at `PASSIVE_LEVEL`: a
      refused query and an IRP refused admission locally with a failure
      status; an accepted query by this driver with the lower drivers'
      status, after their final answer; a set into sleep skipped down and
      completed by the lower drivers; S0 and D0 passed down, waited for and
      completed with their status. A query never succeeds without the
      lower drivers' success. `slotmodel.c` gives every power IRP an
      identity and counts both events where they happen (below).
    - **What user mode can read** (the review's sixth finding).
      `GET_INFO` grows to 52 bytes (interface 1.1, 5.1's rule: fields at
      the end, announced in `cbSize`), all taken in one hold of the lock
      (`slot.c`'s fields), and only what the driver knows:
      - the gate's reasons now: 1 system, 2 device, 4 a system standby
        prepared or accepted, 8 draining, 16 another component's device
        query prepared or accepted;
      - what is still out while draining (`SlotInfoHeld`): 0 unless
        draining, then bit 0 bulk IN, 1 bulk OUT, 2 control, 3 interrupt,
        4 `WAIT_EVENT`, and 5 when completion tails are still returning
        requests to their callers (draining can stand on those alone, with
        every slot idle); it is nonzero exactly while draining (checked by
        the model in every state);
      - the same mask at the last refused query;
      - the refused queries (system and device) and the sets passed down
        with requests out, each a UINT16 counted modulo 65536, not
        saturating: a count is history, not news, so a reader takes its
        own baseline (the library: at its `CLAIM`, and again after it has
        reported a refusal) and reports a refusal when the count differs
        from it; a difference of exactly 65,536 refusals between two
        readings would go unseen.
      So "draining now" (gate bit 8 with the mask) is told apart from "a
      refusal happened" (the count against the baseline). `GET_INFO` takes
      no slot, so it answers while the gate is closed. `MTPTEST` prints
      them; the library turns them into the program's messages (13, owner
      drafts), which say what the driver did and what is uncertain, not
      what Windows did: the refusal count says this driver refused a
      query, not that the system stayed up, and a stopped transfer's
      outcome is uncertain until its request has come back.
    - **Not changed:** STOP's wait for quiet keeps no bound (a PnP stop,
      not a standby; QUERY_STOP refuses while a request is out, so only a
      STOP without one meets it); the driver still requests no device power
      IRP and handles the D-IRPs others send (below).
  - **No device power IRP is requested (revised 2026-11-17).** A
    compatibility measure for the stacks measured (98 SE under xHCI98
    3.5.0.0 and NUSB 3.3's EHCI, ME under xHCI98 3.5.0.0), not a general
    design without a power policy owner: the driver handles the device
    IRPs another component sends and requests none itself. No other
    power policy owner has been identified for this function; what the
    runs show per stack is below.
    - **Why.** The sixth review asked who requests this function's device
      power transitions, and `3044286` made the driver the policy owner:
      D3 requested at a system sleep, D0 after S0 (`PoRequestPowerIrp`).
      Its standby legs
      ([2026-11-16-mtp-m1-load.md](../testing/2026-11-16-mtp-m1-load.md))
      showed, under NUSB's EHCI on 98 SE, a guest that never came back:
      after the wake an unrequested D2 and D0, then REMOVE, whose pass to
      the stack below never returned, with nothing of the driver's
      outstanding (no requested IRP, no kept system IRP, the power worker
      ended, every admission drained). With no D3 requested (the earlier
      driver, and this driver with only the request removed), NUSB sent
      QUERY_REMOVE and REMOVE as the system went to sleep, REMOVE returned
      at once, and the guest came back; with no MTP driver bound it came
      back too. Requesting D3 but not D0, or not passing the unrequested
      D2 down, did not help. The inference drawn, and only an inference:
      under that stack a D3 passed down during a standby, followed by
      REMOVE, is what the stack below does not survive (its code was not
      read, and why it sent the D2 is not known: the stack's capabilities
      report D2 as its device wake state, which names the value, not the
      reason). Under xHCI98 (98 SE and ME) the D3 path worked. One rule
      must serve both stacks, and a stack-dependent
      rule would rest on telling them apart, which nothing in the
      driver's contract allows. The readings are in the load record; the
      stack's own code was not read.
    - **What happens instead, per stack (the load record).** On 98 SE,
      under both stacks measured, the stack removes the device as the
      system goes to sleep (REMOVE after the system IRP went down), so on
      those configurations the device object does not sleep: it is gone,
      and under xHCI98 a new device object arrives after the wake (under
      NUSB's EHCI none, and a device plugged after the wake was not
      enumerated either, also with no MTP driver installed). That removal
      is an observed substitute for powering the function down, not
      evidence of a delegated power policy owner. On ME under xHCI98
      another component sends the device D3 during the standby and D0
      after it, and the device object is kept; its identity and the rest
      of its responsibilities are not known. After that wake the stack
      reset the device's port (QEMU's trace: a port reset written by the
      guest, then the device's own reset), and the device answered
      Session Not Open: the device object survived, the device's protocol
      state did not. The library takes the session as lost after any
      standby either way (7.3).
    - **Not covered: a device object that survives a standby and
      receives neither REMOVE nor a device IRP.** No stack measured did
      this. For it the driver has no established contract: S0 alone
      reopens the system's reason, and the next request goes to a device
      whose state after the sleep nobody restored.
    - **Another component's D3 during a sleep, then REMOVE.** The driver
      forwards such a D3 as it must, so the sequence the NUSB runs could
      not survive after the driver's own D3 can still reach a stack from
      another sender; requesting none avoids the measured trigger and
      guarantees nothing about other senders. Read once on ME under
      xHCI98 (the D3 and D0 around a standby, the unplug after the wake):
      REMOVE's pass returned, and the zombie was deleted at the last
      close. On 98 SE no such D3 was seen (the device is removed instead).
      `slotmodel.c` plays it both ways (below).
    - **System into sleep, S0, another component's D3 and D0:** as the
      standby rework above says. **A failed wake** (a D0 failing, or
      refused because removal has begun) leaves the device's reason
      standing: the gate stays closed, every request is refused
      `STATUS_DEVICE_NOT_READY`, and the device is usable again only
      after a new device arrives.
    - The power worker (an IRP that came above `PASSIVE_LEVEL`) forwards
      each IRP the same way; nothing is kept pending for later, and no
      wait on it is unbounded but STOP's (which never comes to it).
    - A set's wait for quiet logs `power-down-waiting`, a query's
      `power-query-waiting`, STOP's `stop-waiting`, once a second while
      it lasts. REMOVE logs `remove-lower` when the stack below has
      returned it, and `remove-rundown-waiting` once a second while its
      rundown waits.
    - `slotmodel.c` holds the stack to a conservative compatibility
      constraint inferred from the NUSB runs, not a proven property: a D3
      passed down during a system sleep, then REMOVE, is taken never to
      return (the model fails the REMOVE; it does not play a REMOVE held
      below, so it cannot explain the hang or its dependencies). It does
      not say that every D3-plus-REMOVE sequence hangs or that NUSB
      generally needs removal instead of a device sleep. It catches the
      design of `3044286` as the mutant `MUT_SLEEP_REQUESTS_D3`, and plays
      another component's D3 during a sleep followed by REMOVE: with the
      constraint applied to that D3 the current driver meets it (an
      expected-failure scenario), and with it left off (ME's one reading)
      the scenario passes.

    The sixth review's rule that power IRPs are completed only at
    `PASSIVE_LEVEL`, and the gate's two reasons, stay; only the requests
    are withdrawn. The sixth review's second finding (who requests the
    device's power transitions) is not answered in general: the readings
    above say what each measured stack does, and nothing more.
  `MTPPRB.SYS` (5.6) follows the earlier rules (a drained power count).
- **Nothing outstanding when the image can unload.** No work item, timer,
  DPC or completion routine of the driver may be outstanding when its image
  can unload, and the deleter waits on nothing that calls out (below,
  "What is drained"). Timers, completion routines, completion tails, the
  dispatches without a handle and the power worker are covered by the
  rundown and the admission drain that REMOVE waits for before its detach,
  all before any deletion. The power worker's routine is also counted in
  one driver-wide count from its queueing to its last act, and
  `DriverUnload` (only that) waits until it is 0. The late worker (above)
  is counted the same way, and `DriverUnload` waits for it from any
  thread but its own. What is left uncovered is each routine's return
  after its decrement, as with any work item on 98 SE and ME
  (`IoQueueWorkItem`, which would hold the device object, is not in their
  DDK), and the late worker's remaining steps when `DriverUnload` ran
  inside one of its completions; the model's list of what it does not
  cover says how that is taken.
- **`IoDeleteDevice` is the last act.** On 98 SE, NTKERN calls
  `DriverUnload` inside `IoDeleteDevice` of the driver's last device
  object (M0, 2026-11-16: the probe logged its unload before
  `IoDeleteDevice` returned, twice). So every drain comes before it, and
  nothing of the driver runs after `IoDeleteDevice` but the return.
- **The device object outlives its handles (a zombie).** M0 read two
  things (2026-11-16). On 98 SE under xHCI98, after a device object was
  deleted with no handle open, the next device created got the same
  address. Separately, IRPs for handles kept open after REMOVE keep
  arriving: IOCTLs, reads, `IRP_MJ_CLEANUP` and `IRP_MJ_CLOSE`, on 98 SE
  after `IoDeleteDevice` and, with the zombie below, on 98 SE and ME to the
  retained object. That such an IRP would reach a new device once the old
  object's memory is reused is an inference from the two readings; it was
  not observed. The rule removes the case: `IoDeleteDevice` is not called
  while a handle is open:
  - at `IRP_MN_REMOVE_DEVICE` the driver runs down as above, deletes its
    symbolic link and detaches from the stack, and then keeps the object
    as a zombie when handles are open;
  - a zombie refuses everything (`STATUS_DELETE_PENDING`) except
    `IRP_MJ_CLEANUP` and `IRP_MJ_CLOSE`; power IRPs are completed (the
    power gate is closed);
  - the open count is `slot.c`'s, kept under the device lock from
    `IRP_MJ_CREATE` (`SlotOpen`, refused once removal has begun) to
    `IRP_MJ_CLOSE` (`SlotClose`);
  - **one exclusive deleter.** `slot.c` sets `FreeReady` once, in the same
    hold of the lock as it emits `SA_FREE_READY`, when the rundown is done
    and no handle is open. The action itself is only noted (it may run at
    any IRQL, in a completion tail for instance). The deletion is claimed
    in `slot.c`, under the lock, by whichever of two calls first finds the
    device detached and `FreeReady` set, setting `Deleting`:
    `SlotDetach`, REMOVE's right after its `IoDetachDevice`, or the
    `SlotClose` of a close that came after the detach. Every close calls
    `SlotClose` and acts on its answer, whatever it saw of the removal
    before (a close that passed the zombie check before REMOVE detached
    still claims the deletion when its `SlotClose` comes after the
    detach). Exactly one claims it; REMOVE never deletes on its own count
    of handles; a close before the detach never deletes. `slotmodel.c`
    checks every order of the closes and the detach. After the hold of the
    lock in which it called `SlotDetach` or `SlotClose`, a party that did
    not claim the deletion touches nothing of the device: what REMOVE logs
    is read before the detach is published, and `SlotClose`'s actions
    (only the two ready notes) need no performing.
    The claimant, at `PASSIVE_LEVEL`, completes its own IRP (a close),
    unpublishes the device, waits for the dispatch pins (below) and calls
    `IoDeleteDevice` as its last act; it waits for nothing else;
  - each device has a name of its own (`\Device\UsbMtpN`), so a new
    device can be created while a zombie of the same driver exists; the
    symbolic link names the newest device;
  - each handle carries its device's number in `FileObject->FsContext`;
    an IRP that comes on another device's handle is refused and counted;
  - **IRPs without a handle** (PnP, power, create, every other major) are
    not kept safe by an open handle. On entry each pins the device
    (`SlotEnter`) under the driver-global lock, after checking there that
    the device object is not the one already deleted; under the pin it
    reads whether the device is detached or closed to such IRPs and, if
    not, takes an admission (`ActiveOut`) under the device lock; then the
    pin is left. One that is not admitted is completed from the driver's
    globals without touching the device (`STATUS_DELETE_PENDING`; a power
    IRP through the driver's power path, at `PASSIVE_LEVEL`). The deleter,
    after its claim, unpublishes the device (`SlotGone`) under that lock
    and waits until no pin is held (`SlotPinned`). The hold of that lock
    that finds no pin also publishes the object as the deleted one
    (`g_Deleted`), which every entry checks in the same lock before it
    pins, so no dispatch can enter between that last check and
    `IoDeleteDevice`, whatever `SlotEnter` decides (before 2026-11-16's
    sixth review the publication came in a later hold, and only
    `SlotGone` closed that gap). Because a pin covers only those few
    reads, it is never held across a call down, a completion or anything
    else that can re-enter the driver, so a completion that closes the last
    handle and deletes in its own call chain never waits on it (the rule
    for a pin is the rule for a lock).
  - The admission (`SlotActiveEnter`, `SlotActiveLeave` in `slot.c`)
    keeps the device for the dispatch: REMOVE closes the admission
    (`SlotActiveClose`) and waits for every admitted dispatch (power IRPs
    queued for the worker included) before it forwards REMOVE and
    detaches, and no device is deleted before its detach. It is the
    driver's own remove lock. The deleter never waits on it; only REMOVE
    does. So a close in an admitted dispatch's call chain (a power IRP's
    completion closing the last handle) cannot claim the deletion, since
    the device is not detached, and waits on nothing. The dispatch gives
    its admission up as its last access (`ActiveDone`). An accepted query
    passed down with a completion routine holds a second admission of its
    own, which that routine gives up (5.4, "Two lifetimes at an accepted
    query").
  - **An assumption, not a proof: REMOVE does not come inside an
    admitted call chain.** REMOVE's wait for the admissions holds only if
    no admitted dispatch's call chain sends REMOVE or waits for it. The
    PnP manager sends a device one PnP IRP at a time, so no other PnP
    dispatch of this device is in progress. That is the evidence; a power
    IRP's or another major's chain (a lower driver's dispatch, a
    completion routine above) is merely not expected to wait for REMOVE.
    A remove lock prevents deletion while it is held; it does not prevent
    REMOVE from arriving. Should the assumption fail, REMOVE's wait never
    ends: the removal hangs, the device stays in Device Manager, and the
    trace build logs `remove-drain-waiting` once a second with the
    admissions still out. No such case was seen on 98 SE or ME, and none
    is known to be produced by them.
  - **A second assumption, of the same kind: no slot activity reaches a
    wait for quiet synchronously.** The power-down's wait (a system or
    device IRP into sleep, on its dispatch or the power worker) and
    STOP's wait for every slot to be idle and every tail done hold only
    if the thread that waits holds none of what it waits for: a
    completion tail across its `IoCompleteRequest`, a submitter's pin
    across its `IoCallDriver`, a cancel's pin across its `IoCancelIrp`
    (`Run`), a lower completion, timer DPC or cancel routine. Each of
    those calls out of the driver, and a call chain from one of them that
    sent this device a power IRP into sleep, or STOP, and processed it
    synchronously would wait for itself. That is not expected: the system
    and device power IRPs come from the power manager and from other
    components (this driver requests none, 5.4, "Power"), STOP
    from the PnP manager, and none of them from a USB completion or a
    user request's completion. The PnP manager's one-IRP-at-a-time rule
    covers only PnP. STOP after a successful `QUERY_STOP` has nothing to
    wait for (that query found every slot idle, no tail and no claim, and
    closed admission in the same hold), so only a STOP without one, or a
    sleep, could meet it. Should it fail for STOP, the wait never ends:
    the stop hangs, and the trace build logs `stop-waiting` once a
    second. For a power IRP the waits are bounded since the standby
    rework: a query in that state is refused at its bound, and a set
    goes down at its bound with that activity quarantined, ending when
    its own thread returns. `slotmodel.c` plays it (the sleep reached
    inside a completion tail: before the rework the run had to end
    stuck; now it must pass, and with the unbounded wait restored as a
    mutant it ends stuck again).
  - **A third dependency, read once: the stack retires the driver's
    cancels during a standby.** The waits for quiet end only when the
    stack below returns every request the gate cancelled. M0 read the
    stacks retiring cancelled bulk, interrupt and class requests, but not
    during a standby. In one of five standbys taken during a transfer on
    98 SE under NUSB's EHCI (2026-11-17, the load record), the system's
    SET_POWER reached the driver, the gate closed and cancelled, and no
    completion came back: `power-down-waiting` was logged for more than
    1,200 s of guest time, the standby never completed, and an unplug
    changed nothing. Which request was held (the interrupt poll's is the
    one every other run showed retiring at the gate) was not established;
    four repeats with a trace build that names the busy slots in that
    line did not reproduce it. Until the standby rework above that wait
    had no bound, so a standby during traffic under NUSB's EHCI could
    hang in it. Since the rework the dependency costs a refused standby
    (a held request at the query) or a quarantined request (one held at
    a set without preparation), not a held system IRP; the device stays
    unusable until the stack returns it or the device is removed, and a
    REMOVE still waits for it (the rundown).
  - `slotmodel.c` checks an IRP without a handle racing the removal, the
    detach and the last close; one, admitted or refused, whose completion
    closes the last handle and deletes in its own call chain (with the
    pin held across the completion, a mutant, that run hangs); two that
    come above `PASSIVE_LEVEL`, the admitted ones through the power
    worker and the refused ones through the late hand-off, one completion
    closing the last handle and so running `DriverUnload` inside the late
    worker (with the refused IRP completed by a driver-global worker whose
    count the deleter drains, the design before the fifth review and a
    mutant, that run hangs too); the same with the last close a host's,
    so that `DriverUnload` runs on another thread and must wait for the
    late worker; and a sleep and a wake with another component's D3 and
    D0, its D0 successful and failed, with and without a removal after
    them.

  `slot.c` keeps the open count (`SlotOpen`, `SlotClose`) and returns
  `SA_FREE_READY` only when the rundown is done and no handle is open;
  `slotmodel.c` checks that no IRP of an open handle reaches the device
  after its free. M0 confirms the route on both of 98 SE's stacks and on
  ME (5.6, item 6). Should 98 SE or ME not tolerate an object kept after
  REMOVE, the fallback is one driver-global zombie object that takes
  over the open handles, and this section will say so.
- **Decisions by the IRP status, never a URB status's class alone.** A
  cancelled URB's status is `C0010000h` under NUSB's EHCI driver and
  `00010000h` under xHCI98 (M0, 2026-11-16): xHCI98 uses the value the
  Windows 2000 DDK's `usbdi.h` gives `USBD_STATUS_CANCELED`, which lies in
  that header's success class; later headers moved it to `C0010000h`.
  Both are by design. The driver decides by the IRP's status first; a
  transfer counts as successful only when the IRP succeeded and the URB
  status is `USBD_STATUS_SUCCESS`; any other URB status is a failure,
  whatever its class bits, and is logged.
- **Limits:**
  - an admission number is never 0 (the counter skips it);
  - at most `SLOT_MAX_SEGS` (16) segments, more is refused;
  - at most `SLOT_MAX_PINS` (8) pins can be out;
  - an action list holds `SLOT_MAX_ACTIONS` (17), at least
    3 x `SLOT_COUNT` + 2 (asserted); an overflow, which the sizing
    rules out, is counted in `SlotDropped`, never silent;
  - a claim's epoch is never reused (5.1);
  - timers are one-shot.
- **`GET_STRING`** is a control transfer on the control slot, with the same
  lifetime as `CLASS_REQUEST`.
- **IRP reuse on 98 SE and ME.** `IoReuseIrp` does not exist there.
  Following Microsoft's page "Reusing IRPs" (the Windows 98/Me
  work-around):
  - each lower IRP is allocated with `IoAllocateIrp(StackSize, FALSE)`
    (*ChargeQuota* FALSE);
  - after full retirement it is re-initialised with `IoInitializeIrp`
    with the saved size and stack count;
  - every submission then rebuilds the status, the next stack location,
    the URB and the completion routine.

  `slot.c` returns `SA_SUBMIT`, the only point where the lower IRP may
  be rebuilt, only when it has come back from the stack (`slot.h`).

**States** (`slot.h`): IDLE, ACTIVE, STOPPING (a cancel, time-out, release
or removal decided; no further segment), DONE (the outcome fixed; waiting
for every activity to end), and RESERVED (held by a `RESET_PIPE`).

**The transitions** (each a `Slot*` function of `slot.h`, called under the
lock; the actions it returns are performed after the lock is released):

| Event | Function | Who calls it |
|---|---|---|
| Admission, two parts under one hold of the lock | `SlotAdmit`, `SlotAdmitCheck` | dispatch |
| The submitter's `IoCallDriver` has returned | `SlotSubmitReturned` | the admitter, or the completion path that sent the next segment |
| The lower IRP's completion routine | `SlotLowerDone` | the stack (the routine returns `STATUS_MORE_PROCESSING_REQUIRED`; the IRP is the slot's) |
| An `IoCancelIrp` has returned | `SlotPinReturned` | whoever made the call |
| The completion tail's `IoCompleteRequest` has returned | `SlotTailDone` | the tail |
| The cancel routine | `SlotCancelBegin`, `SlotCancelEnd` | the I/O manager (the cancel spin lock released first) |
| The timer DPC | `SlotTimerBegin`, `SlotTimerEnd` | the timer |
| `ABORT`, `EVENTS` stop | `SlotStop` | a bound or owner handle |
| The interrupt poll: start; its completion, delivering an event to the wait slot | `SlotPollStart`, `SlotPollDone` | claim or `EVENTS` start; the stack |
| Claim, release, removal | `SlotClaim`, `SlotRelease`, `SlotRemove` | `CLAIM`; 5.1's release; `IRP_MN_REMOVE_DEVICE` |
| A handle opened; a handle closed (the free waits for the last close) | `SlotOpen`, `SlotClose` | `IRP_MJ_CREATE`; `IRP_MJ_CLOSE` |
| REMOVE's detach (it may claim the deletion) | `SlotDetach` | `IRP_MN_REMOVE_DEVICE` |
| A dispatch pin; the deleter's unpublish and wait | `SlotEnter`, `SlotLeave`; `SlotGone`, `SlotPinned` | every IRP without a handle; the deleter |
| An admission taken, given up; REMOVE's close of it and its wait | `SlotActiveEnter`, `SlotActiveLeave`; `SlotActiveClose`, `SlotActiveOut` | every IRP without a handle; `IRP_MN_REMOVE_DEVICE` |
| A power IRP queued for the worker; the worker's step | `SlotPowerQueue`; `SlotPowerNext` | the power dispatch above `PASSIVE_LEVEL`; the power worker |
| The submitter's commit, before it rebuilds the lower IRP (withdrawn if a stop came); the rebuild finished, before the hand-off (withdrawn if a stop came during it, its cancel deferred) | `SlotSubmitCommit`, `SlotSubmitBuilt` | whoever performs an `SA_SUBMIT` |
| The power gate closed (every slot stopped), opened | `SlotPowerDown`, `SlotPowerUp` | a `SET_POWER` into sleep; one back to S0 or D0, after it completed (S0 also ends QUERY, D0 DQUERY, whatever their result) |
| A set into sleep passed down (what is out counted) | `SlotSetForward` | after the set's bounded wait, in a hold that finds `SlotCalling` 0 |
| A query (system, or another component's device query): prepared, decided, answered below, lapsed | `SlotQueryBegin`, `SlotQueryDecide`, `SlotQueryEnd`, `SlotQueryExpire` | a `QUERY_POWER` below working or below D0; its decision at quiet or the bound (an acceptance takes the query's own admission); the lower drivers' final answer, in the completion routine (giving that admission up); the hold of the next admission, `CLAIM`, `EVENTS` start or `GET_INFO`, at `QUERY_HOLD_MS` after the acceptance |
| A poll's data while the gate is closed (not queued, flagged lost) | `SlotPollAccept`, `SlotTakeLost` | the poll's lower completion; the driver's next hold |
| Draining ended (DRAIN) | inside `slot.c` | the hold in which the last slot becomes idle with no tail out |

**The model, `slotmodel.c`.** Built and run on the build host by
`slotmodel.ps1` with MSVC 6.0, the driver's compiler; `slot.c` is held to
`/W4 /WX`. It plays the parties around the driver as actors whose steps
interleave in every order:

The actors are listed at the head of `slotmodel.c`. In short:

- hosts making successive requests on a slot (bulk IN or OUT, wait,
  `RESET_PIPE`). The admission is in its two parts. The I/O manager may
  cancel the IRP any number of times, before, during or after admission.
- the lower IRP's Cancel bit is cleared by its rebuild at submission, and
  set by `IoCancelIrp`, which takes the stack's cancel routine while the
  stack holds the IRP.
- the stack completes a lower IRP full, short, zero-length, failed, or
  failed part-way. In "needs cancel" scenarios it ends a stopped request
  only after its cancel routine was taken while it held the IRP.
- each request's one-shot timer and DPC, and the cancel routine, with
  their last transition and their return as separate steps.
- completion tails: `IoCompleteRequest`, then `SlotTailDone`, as separate
  steps, with the host free to admit its next request between them.
- the interrupt poll delivering events, also during removal; release, a
  new claim with an old handle's stale epoch, removal and the free after
  it; `ABORT`; the admission counter wrapping.
- IRPs without a handle (power): the pin, the admission, the pin left,
  then the IRP forwarded inline or queued for the power worker, or
  refused and completed from the driver's globals; each completion may
  close the last handle in its own call chain and so claim the deletion.
  REMOVE closing the admission and forwarding only after its drain; the
  deleter's `IoDeleteDevice` with `DriverUnload` inside it; the worker's
  driver-wide count.
- a system sleep and a wake at any time: the gate, the stops, the wait for
  quiet, the device asleep until the wake has opened the gate. Since the
  standby rework the system's IRPs come one at a time as the power
  manager sends them: a standby query, decided at quiet or at its bound
  (the model has no time, so the bound may be taken at any moment),
  accepted (the lower drivers may refuse it) or refused; then its set, an
  S0 (another device's refusal, or after ours) or nothing and the lapse;
  a second query; or a set with no query at all, down at quiet or at its
  bound. A stack that may keep a request the gate stopped for good, or
  return it at any later time. Since the review of `8557bdb`: every
  `SA_SUBMIT` performed as a commit under the lock, then the call; a
  query repeated from its acceptance or after a refusal; a set after a
  refused query, or after the lapse with traffic admitted since; an S0
  that fails (QUERY ends, SYS stays); a query above `PASSIVE_LEVEL`,
  prepared, decided and completed on the power worker, and one refused
  admission completed by the late hand-off; another sender's device query
  before its D3, accepted or refused, then its D3, a D0 or nothing and the
  lapse, also alongside the system's query and S0; a control request and
  a `RESET_PIPE` kept by the stack at a standby; one clock, moved only by
  a lapse, starting just below the tick count's wrap. Since the third
  review of the rework: an accepted query's lower answer as an actor of its
  own (the completion routine's hold, then the IRP's completion on up),
  which may come at any moment after the `PoCallDriver`, before or after
  the dispatch's or the worker's own last device access.

The checks are also listed in the file's head comment:

- each IRP completed once, by its tail, only when no actor holds anything
  of its request (an independent scan of every actor's pending steps and
  the stack);
- the outcome is the first stop decided while the request was open, or
  else its final lower completion's result, and never changes;
- the byte count equals the lower completions' sum;
- an `IoCancelIrp` reaches only its own request;
- a lower IRP is passed down again only after it came back;
- the reservation's owner is exact;
- pins, segments and admission numbers stay within the limits;
- no action is dropped from a full list (`SlotDropped` stays 0; a call
  emits at most three actions per slot and one `SA_REMOVE_READY`, and
  `SLOT_MAX_ACTIONS` is asserted at compile time to hold them);
- a stale epoch is refused;
- nothing touches the device after its free; no admission is out at the
  free; no power IRP is passed down after REMOVE; no work routine runs
  after `DriverUnload`, and the work item is never queued again before its
  routine started; no URB is passed down while the device sleeps, and
  none is with the stack then but one the gate stopped (the quarantine);
- the standby rework: a query is accepted only when nothing is out (an
  independent scan of the stack, the requests and the actors); nothing is
  admitted between an accepted query and its set, nor while a request
  the gate stopped is still out; a lower IRP with the stack always belongs
  to its slot; a caller's IRP is never completed while its lower IRP is
  with the stack; a request the stack keeps is never completed and keeps
  the gate closed to the end; with nothing kept and no sleep, failed wake
  or removal left, the gate is open at the end;
- since the review of `8557bdb`: no lower IRP is passed down without its
  commit, and none while the device sleeps (so a submission emitted
  before the gate closed is withdrawn or reaches the stack before the
  set); every power IRP has an identity, and is passed down at most once,
  completed exactly once, never both completed by this driver as not
  passed down and passed down, never passed down after its completion; a
  query succeeds only after it was passed down and the lower drivers
  answered success; one refused admission is completed with a failure and
  never passed down; every set not refused admission is passed down; a
  query is passed down only when nothing is out (an independent scan);
  nothing is admitted between an accepted device query and its set; each
  query's gate reason agrees with its state; `GET_INFO`'s mask of what is
  out is nonzero exactly while draining, within its documented bits; the
  refusal and forced-set counts equal the model's own counts, and a set
  reports what is out exactly when the independent scan finds something
  out; a poll's data with the gate closed is never delivered and always
  flagged lost; a self-test runs `slot.c`'s lapse arithmetic at its
  boundary and across the tick count's wrap, the counters' wrap past
  FFFFh, the mask's tails bit, and the reasons kept apart;
- since the third review of the rework: an accepted query takes exactly
  one admission of its own and a refused one changes none; the self-test
  also checks that an answer coming after a set gives that admission up
  without reviving the query, and that no query is accepted at the
  admission count's limit;
- every run ends: a deleter (or REMOVE, or `DriverUnload`) waiting on
  something its own call chain holds shows as an actor that cannot move.
  A scenario that must meet an expected outcome (the stack constraint,
  REMOVE's rundown held by a request the stack keeps) counts each
  terminal state where it is met and goes on exploring every other
  branch, all of which must pass (before the review of `8557bdb` such a
  scenario stopped at the first match).

States are stored whole and compared byte for byte on a hash match.
Interleavings are counted in 64-bit integers; a count that overflows is
reported as a lower bound.

**No recursion, bounded tails.**

- A completion that finds a next segment or a poll re-arm to send while
  the submitter is still inside its `IoCallDriver` records it. The
  submitter sends it after its call returns, so at most one
  `IoCallDriver` per slot is on a stack. A cancel asked for at the pin
  limit is coalesced and repeated when a pin comes back.
- Admission is refused `SO_BUSY` while the device has `TailLimit` (32)
  completion tails out.

**Run of 2026-11-16 (the fallback abort removed; the admissions, the
power worker, REMOVE's forward, `DriverUnload` and the power gate added),
twenty-three scenarios: PASS.** 7,806,840 states, 33,892,164 steps, more
than 9.8 x 10^18 complete interleavings (two scenarios' counts overflowed
64 bits). Each host holds a handle, opened at the start; in the removal
scenarios it closes after its last request, and the free waits for the
last close. REMOVE's forward and `DriverUnload` are separate steps in the
scenarios with IRPs without a handle or a sleep, the only actors that can
observe them; elsewhere each is folded into the step before it (no
outcome changes, and the largest scenario stays within the state store).
The scenarios:

- one read of 2 segments with three cancels, a timer, release and callback
  returns;
- two reads with every completion kind, a stack that needs the cancel,
  release;
- two reads with `ABORT`, removal and its free, callback returns;
- bulk IN and OUT together;
- one read of 16 segments, any of them completing inline;
- two reads of 2 segments with `ABORT` and release;
- the wait slot fed by the poll, with release, and with removal during
  delivery; twelve poll events completing inline;
- `RESET_PIPE` reserving bulk IN against a read, with removal and the
  handles' closes; and against two reads (the second reusing the slot
  after the reservation's release), split into one scenario with cancel
  and one with timers, since together with the closes they filled the
  state store;
- claim turnover: stale reads refused, a new handle's reads served; with
  an `ABORT` from the first claim's handle at any time;
- completion tails at a bound of 1, with re-entrant admission;
- the admission counter wrapping;
- the zombie: two handles (bulk IN, bulk OUT) kept open across the
  removal, closing in any order after it, their IRPs refused, the free
  at the last close;
- a power IRP racing the removal, the detach and the last close, admitted
  and forwarded inline or refused; the same with its completion closing
  the last handle and deleting in its own call chain; two power IRPs
  above `PASSIVE_LEVEL`, admitted ones through the power worker and
  refused ones through the late hand-off, one completion closing the last
  handle (`DriverUnload` inside the late worker); the same with the last
  close a host's (`DriverUnload` on another thread);
- a system sleep and a wake against two reads of 2 segments (cancel, a
  stack that needs the cancel, release), against the poll and two waits
  with removal, and against bulk IN and OUT with removal and the closes;
  another component's D3 and then its D0 failing; a sleep and a wake with
  another component's D3 and D0 at any time (2026-11-17); the same with a
  read and a removal at any time, twice: with the stack constraint
  applied to that D3, where the run must meet it (the current driver
  forwards the D3, so a REMOVE after it is what the NUSB runs could not
  survive after the driver's own), and with the constraint left off for
  another component's D3, where it must pass;
- the sleep reached synchronously inside a completion tail: the second
  assumption's failure mode, which had to end stuck (and did) until the
  standby rework, and since it must pass (the set's bound);
- the standby rework (2026-11-17): a query against two reads of 2
  segments, accepted only on quiet, nothing admitted before its set; a
  query with a stack that keeps a stopped request for good or returns it
  at any time, against a read and the poll with a wait, S0 after a
  refusal; a set without preparation with such a stack, bulk IN and a
  bulk OUT of 2 segments, across the wake; query rollback: two queries,
  each accepted then another device's refusal (S0) or no set (the
  lapse), or refused by the lower drivers; removal during a query's
  preparation; an expected hang: such a stack, then removal, whose
  rundown must keep its requirement; another sender's D3 and D0 with
  such a stack; two bulk OUT writes of 2 segments across a standby, never
  sent again;
- since the review of `8557bdb` (2026-11-17): the submission (a set
  without preparation against a read of 2 segments and the poll, such a
  stack); a query repeated from its acceptance, then S0 or the lapse, new
  traffic after the lapse, a delayed set; a refused query, then a
  committed set and no S0; failed S0s; a query on the power worker, with
  removal and the late hand-off; the same with such a stack; a standby
  query against a `RESET_PIPE` and against a class request, each with
  such a stack; another sender's device query with such a stack and the
  lower drivers' refusal; the device query alongside the system's query
  and S0s; the device query with removal.

The run of 2026-11-17 (no device power IRP requested): 27 scenarios
PASS (one of them the expected hang). With the two removal scenarios
after another component's D3 (later on 2026-11-17): 29 scenarios PASS,
the expected-constraint one meeting the constraint (349 states) and its
counterpart passing. That run was of the file before a last change to
one scenario's name and to comments only; the program rebuilt after it
did not start on the build host, so the committed file's own build is
not yet run. The broken copies below were run against the 27;
they were not run again against the 29. The committed 29-scenario file
was built and run by `slotmodel.ps1` later the same day: 29 PASS in
43 s. Its earlier failure to start was the build host's, not the
model's: on that host newly linked programs (and at times a few
others) were held at their start for long periods, the same program
running normally later.

**The standby rework's model (2026-11-17).** `slot.c` gains the gate's QUERY, DQUERY and DRAIN reasons, the
queries' transitions, the submission's commit and rebuild phase,
`SlotSetForward`,
`SlotInfoHeld` and the lost flag; `slotmodel.c` the system's IRPs one at a
time, another sender's device IRPs one at a time, a stack that may keep a
stopped request, the power IRPs' identities, the checks and scenarios
listed above and seeded faults, each of which must be caught for its
reason. `slotmut.ps1` builds and runs every one of them (the model's
compile switches, and broken copies of `slot.c` written from the file
by text replacement) and checks the failure text it expects among the
failing scenarios' first failures; a broken copy that does not build is
an error, not a catch. `MUT_SLEEP_NO_QUIET` stays withdrawn: with a bound,
going down at once is the bound taken at zero, which a model without time
cannot tell from a longer one; what the bound buys is the guests' to read.

**The run of 2026-11-17 (the review of `8557bdb`).** The model as
committed in `8557bdb`, built and run first: 37 scenarios, 9 FAIL, eight
of them "a URB passed down while the device sleeps" (the submission race
the review predicted: an `SA_SUBMIT` emitted before the gate closed,
performed after the set went down at its bound) and one "a host never
finished" (a scenario defect: a `WAIT_EVENT` without a time-out, which the
driver never admits; the scenario now gives it one). After the changes
above: the self-test and 48 scenarios PASS, 13,563,665 states,
54,069,573 steps, more than 1.6 x 10^19 complete interleavings (counts
overflowed 64 bits), 60 s on the build host. The seeded faults (the
model's switches and eleven broken copies of `slot.c`), each built and
run by `slotmut.ps1` on the committed files: every one caught for its
intended reason, but three that must pass. The standby rework's seven:
`MUT_UNBOUNDED_WAIT` (a stuck actor), `MUT_QUERY_NO_QUIET` (a query
passed down with a request outstanding), `MUT_VETO_KEEPS_GATE` (the gate
closed with no reason standing), `MUT_DEADLINE_COMPLETES` (a completion
with its lower IRP in the stack), `MUT_DEADLINE_FREES` (a lower IRP with
the stack while its slot is free), `MUT_RESUBMIT_HELD` (a lower IRP passed
down while the stack still had it), `MUT_WAKE_RELEASES` (an admission
while a request the gate stopped is out, and a set reporting other than
what is out). The review's: `MUT_SUBMIT_NO_COMMIT` and
`MUT_SET_IGNORES_CALLS` (a URB passed down while the device sleeps),
`MUT_REMOVAL_QUERY_SUCCESS` (a query refused admission completed with
success), `MUT_DQUERY_PASSTHROUGH` (a query passed down with a request
outstanding), `MUT_S0_ENDS_DQUERY` (a system S0 ended the device query),
`MUT_VETO_ALSO_FORWARDS` (a power IRP passed down after its completion),
`MUT_LOWER_ANSWER_IGNORED` (a query completed with success without the
lower drivers' success). The earlier switches as in the table
below (`MUT_PUBLISH_LATE` and `MUT_ENTER_IGNORES_GONE` caught together,
each alone passing, as argued). Broken copies of `slot.c`: the commit
ignoring the stop (a URB while the device sleeps), `Calling` never set (a
lower IRP passed down without its commit), the lapse compared without the
wrap (an admission between an accepted query and its set), DRAIN never
ended (`GET_INFO`'s mask disagreeing with draining: DRAIN with nothing
out), DRAIN ended with tails out (a query passed down with a request
out), the decision ignoring DRAIN (the same), S0 or D0 clearing DRAIN (an
admission over what is out), a device set leaving the device query
standing (that), a refusal not counted (the count), the poll delivering
with the gate closed (neither queued nor flagged lost). One broken copy
passes, as expected: a late completion of a stopped request starting its
next segment, which the commit now withdraws (the request is no longer
ACTIVE), a second guard like the deleter's two layers. In a first run
of the faults `MUT_S0_ENDS_DQUERY` was caught only by the refusal count,
and four broken copies whose replacement was a constant did not build
(the runner then took a failed build as a catch); the model gained the
direct checks that no system IRP changes the device query and no device
IRP the system's, and that a set ends its own kind's query, the runner
refuses a failed build, and the copies were written so that they build;
then every fault was run again (the expected text of "DRAIN never ended"
corrected to the check that catches it first, and that copy run again
alone).

**Again after the second review (the rebuild phase, 2026-11-17).** With
the rebuild and the hand-off as steps of their own: the self-test and 48
scenarios PASS, 15,138,469 states (the largest scenario 2,604,426),
59,431,166 steps, counts overflowing 64 bits, about 60 s. Every seeded
fault run again by `slotmut.ps1`: all caught for their intended reasons
as above, and the new ones: a cancel during the rebuild not deferred (an
`IoCancelIrp` overlapping the rebuild), a deferred cancel delivered twice
(the IRP withdrawn and cancelled), a deferred cancel lost together with
the post-call cancel (a stuck actor). A deferred cancel lost alone (the
IRP handed off although stopped) passes, as expected: the post-call
handshake cancels it, a second guard. The commit ignoring the stop is now
caught earlier, by its cancel overlapping the rebuild (the expected text
corrected, that copy run again alone); "DRAIN ended with tails out" now
requires its own reason (a query passed down with a request out).
`MUT_SLEEP_REQUESTS_D3` meets its intended failure (the stack
constraint), but in other scenarios the variant fills the model's state
store or its power-IRP records: its result shows that the model is
sensitive to it, not a complete exploration of that variant.

**Again after the third review (two lifetimes at an accepted query,
2026-11-17).** With the accepted query's own admission in `slot.c` and
its lower answer an actor of its own: the self-test (now also the
query's admission taken at the decision, given up by an answer that
comes after a set without reviving the query, and refused at the count's
limit) and 48 scenarios PASS, 14,914,034 states (the largest scenario
2,602,806; slightly fewer in all than the run above), 58,679,760 steps,
counts overflowing 64 bits, about 80 s. Every seeded fault run again by
`slotmut.ps1`: all caught for their intended reasons as above, and the
new ones: `MUT_WORKER_RIDES_QUERY` (the stage-2 plan before the review)
caught in the query on the power worker with removal as "device touched
after its free": the answer gave the one admission up, REMOVE drained,
detached and the deleter freed the device, and then the worker made its
next hold; `MUT_QUERY_UNPROTECTED` caught as "device touched after its
free" by the answer's own hold, in the four removal scenarios with a
query; the broken copies "an accepted query takes no admission of its
own" ("an accepted query took no admission of its own") and "the lower
answer keeps the query's admission" ("an admission was never given up").
The broken copy "decide ignores DRAIN" had to be rewritten for the
decision's new condition (its old text was no longer in `slot.c`, which
the runner reported as a missing pattern); run again alone, it is caught
as before. The three that must pass still pass.

The table below is of the runs before the standby rework (against 27
scenarios, with the slot.c of then); its broken copies were not all run
again since, and those that `slotmut.ps1` does not carry are not claimed
for the present files. Twenty-seven broken copies of `slot.c` and
thirteen of the model's own driver steps (compile switches in
`slotmodel.c`):

| Broken | Caught as |
|---|---|
| no re-check after the submit | "an actor is stuck: a lost cancel or a hang" |
| no pin on `IoCancelIrp` | "pins above SLOT_MAX_PINS" (the count underflows) |
| the cancel routine taken as retired without `IoSetCancelRoutine`'s answer | "completed while an actor still holds a step of the request" |
| the timer taken as retired without `KeCancelTimer`'s answer | "completed with its timer or DPC alive" |
| completion tails not counted | "device touched after its free" |
| admission number 0 not skipped | "a slot holds admission number 0" |
| no bound on completion tails | "admitted with the completion tails at their bound" |
| a next segment sent from inside the submitter's call (recursion) | "two IoCallDriver of one slot on stacks at once (recursion)" |
| the free decided without the open count | "free with a handle open" |
| a stale epoch's `ABORT` not refused | "an ABORT of an earlier claim reached the device" |
| a close never claims the deletion | "a removal never reached its free" |
| the claim taken before the detach | "free before the detach" |
| the deleter not waiting for the pins | "free with a dispatch pin held" |
| a pin never left | "an actor is stuck: a lost cancel or a hang" |
| the admission taken although the device is detached or closed to it | "a power IRP passed down after REMOVE" |
| REMOVE not draining the admissions | "device touched after its free" |
| an admission never given up | "an actor is stuck: a lost cancel or a hang" |
| the worker giving the IRP's admission up before forwarding it | "a power IRP passed down after REMOVE" |
| the worker not counted as running (its item queued again) | "device touched after its free" |
| the power gate not checked at admission | "a URB passed down while the device sleeps" |
| the power-down not stopping the slots | "outcome is not the first decided one (precedence)" |
| the gate never opened at the wake | "the device's reason stayed after a successful D0" |
| the power-down recording no reason | "a URB passed down while the device sleeps" |
| the power-up clearing every reason (one flag for both) | "a URB passed down while the device sleeps" |
| model: the pin held across the completion | "an actor is stuck: a lost cancel or a hang" |
| model: a refused IRP completed by a driver-global worker the deleter drains (the design before this one) | "an actor is stuck: a lost cancel or a hang" |
| model: `DriverUnload` not waiting for the worker's count | "a work routine ran after DriverUnload" |
| model: the sleep IRP going down without waiting for the slots | "a URB with the stack while the device sleeps" |
| model: the deleter's check and its publication of `g_Deleted` in two holds, with `SlotEnter` ignoring `SlotGone` (both layers gone) | "free with a dispatch pin held" |
| model: `DriverUnload` waiting for the late worker on its own thread | "an actor is stuck: a lost cancel or a hang" |
| model: `DriverUnload` not waiting for the late worker | "the late worker ran after DriverUnload on another thread" |
| model: a refused power IRP above `PASSIVE_LEVEL` completed in its dispatch | "a power IRP completed above PASSIVE_LEVEL" |
| model: its late-list entry a later step than its refusal | "the late worker ran after DriverUnload on another thread" |
| model: the wake's S0 ending the device's reason too | "a URB passed down while the device sleeps" |
| model: D3 requested at a system sleep and D0 after S0 (the design of `3044286`) | "REMOVE after a D3 went down during a system sleep: the stack below does not return it (read under NUSB's EHCI)" |

Three broken copies of `slot.c` are not caught: each is redundant
under the modelled valid calls (a stream of new IRPs starving the
deleter, the case the second item's unpublish guards against, lies
outside the bounded exploration). Two of the model's switches are not
caught alone (the second item):

- the deletion's claim not made exclusive (`Deleting` not set): the two
  conditions of the claim (detached, `FreeReady`) become true together
  exactly once, at whichever of the detach or the last close comes
  second, and neither can happen twice, so the flag only guards against a
  close after the count reached 0, which the count itself rules out;
- a pin taken although the device is unpublished (`SlotGone` ignored):
  the deleter's last pin check and the publication of `g_Deleted` are one
  hold of the driver-global lock, and every entry checks `g_Deleted` in
  that lock before `SlotEnter`, so a pin taken despite the unpublish is
  either seen by that check (and waited for) or refused by `g_Deleted`.
  The unpublish is a second layer: it keeps a stream of new IRPs from
  holding the deleter for ever, which a bounded run cannot show. The model
  represents the deleter's holds as the driver has them (the check that
  publishes, then `IoDeleteDevice` as a later step) and catches the two
  layers removed together: with `MUT_PUBLISH_LATE` (the check, and the
  publication in a later hold, the order before the sixth review) and
  `MUT_ENTER_IGNORES_GONE` both set, "free with a dispatch pin held";
  with either alone, nothing, as argued;
- the poll re-armed although the gate is closed: `SlotPowerDown` stops the
  poll in the same hold and `SlotPollStart` refuses while the gate is
  closed, so no active poll can meet a closed gate; the check is a second
  guard.

The model covers the slot transitions. It does not cover:

- the stacks' own behaviour (M0, 5.6);
- what NTKERN does with the object's memory: M0 read it (5.6, item 6),
  and the zombie rule above keeps the object until the last close;
- whether REMOVE can come inside an admitted call chain (the assumption
  above): the model sends REMOVE only from its own actor;
- when NTKERN frees the image after `DriverUnload`: a routine's return
  after its last counted act (a dispatch's, a completion routine's, a
  DPC's, the workers') is taken to be safe, as the deleter itself returns
  into its own code after `IoDeleteDevice` (M0 ran that path on both
  systems without a fault). The late worker goes one step further when
  `DriverUnload` ran inside one of its completions: it finishes the IRPs
  still on its list (all for deleted devices) before it returns, on the
  same assumption that the image stays mapped until that call has
  returned;
- IRQL as such: the model knows only that a refused power IRP came above
  `PASSIVE_LEVEL` (a scenario's flag, and since the review of `8557bdb` a
  system query in the scenarios that say so, which the power worker then
  prepares, decides and completes) and checks that such an IRP is never
  completed in its dispatch; that 98 SE and ME send power IRPs to a
  `DO_POWER_PAGABLE` device at `PASSIVE_LEVEL` is Microsoft's statement,
  not a reading;
- the stack below: the model holds it to the conservative constraint
  inferred from the NUSB standby runs (a D3 passed down during a system
  sleep, then REMOVE: REMOVE taken never to return), and to nothing
  more; it does not hold a REMOVE outstanding below, so it neither
  explains nor validates the stack's hang; a device IRP from another
  component comes from a sender that waits for its IRP's completion
  before it sends the next (its D0 only after its D3);
- time: the bounds are taken at any moment, so the model shows what
  happens at any expiry, not that 3 s is long enough for a stack that
  would have returned; that is the guests' to read. Where the stack keeps
  a stopped request (the third dependency above, seen to fail once under
  NUSB's EHCI) the model plays it in the scenarios that say so; elsewhere
  its stack always ends a cancelled request. The lapse is a step that
  moves the model's one clock; the tick arithmetic of `SlotQueryExpire`
  is checked by the self-test at its boundary and across the wrap, and in
  the exploration only at the values the scenarios reach. That the driver
  calls `SlotQueryExpire` in the same hold as each admission, `CLAIM`,
  `EVENTS` start and `GET_INFO`, and passes the real tick count, is the
  driver's to do and its review's to check;
- the driver's own completion of power IRPs: the model counts each power
  IRP's forwarding and completion where its steps are, which shows that
  the transitions leave no IRP completed twice, forwarded after its
  completion or never completed; that `usbmtp.c`'s `PowerForward`,
  `Done` and the late worker perform exactly those steps is the driver's
  review's to check;
- the submission's last wait: a set waits for `SlotCalling` to be 0, which
  depends on the lower dispatch returning from `IoCallDriver`; the model
  takes every dispatch to return.


**Waits and bounds.**

- `ABORT` and `EVENTS` stop mark their slots STOPPING, cancel the lower
  IRPs, and wait at most their given time for the slots to become IDLE.
  Then they report which have not; a slot that has not stays out of use
  until it does.
- Release never waits (5.1).
- A caller's own timeout bounds a call only as far as the stack honours
  the cancel that the timeout sends. For bulk and interrupt URBs that is
  M0 item 5 (5.6). For control URBs (`CLASS_REQUEST`) and pipe resets
  (`RESET_PIPE`) M0 reads the same.
- Until M0 shows a stack retiring a cancelled control or reset request,
  the design promises no bound for those calls on that stack: such a call
  takes as long as the stack takes. The program never waits for one in
  the window thread, and treats a recovery call that passes the deadline
  as a stuck device (7.5).

**Removal:**

- 98 SE and ME send `IRP_MN_REMOVE_DEVICE` without a preceding
  `IRP_MN_SURPRISE_REMOVAL` (Microsoft's documentation of surprise
  removal says these systems do not send it). The driver handles a bare
  remove.
- The remove:
  1. under the lock, marks the device removing (`SlotRemove`), so every
     Admit is refused with `STATUS_DELETE_PENDING`, and stops every slot
     (the "Release, removal" event of the table): each request's own
     cancel goes down;
  2. closes the admission of IRPs without a handle and waits until every
     admitted dispatch is done (5.4: power IRPs included, so none reaches
     the lower stack after REMOVE), then passes REMOVE down;
  3. removes the symbolic link;
  4. waits until every slot is IDLE and every tail done
     (`SA_REMOVE_READY`), then frees the slots' lower IRPs and calls
     `IoDetachDevice`;
  5. then, under the lock, calls `SlotDetach`: it marks the device
     detached and, if `FreeReady` is set and no one has claimed the
     deletion, claims it. The claimant unpublishes the device, waits for
     the dispatch pins (publishing the object as deleted in the hold that
     finds none) and calls `IoDeleteDevice` as its last act. Otherwise the device stays as a
     zombie (5.4), and the `SlotClose` of the close that ends the count
     claims the deletion in the same way.

  The wait in step 4 has no limit; what ends it is the driver's own
  cancels of step 1, not the stack's completion at the unplug, which M0
  found missing on xHCI98 (5.6, findings). It is the driver's own event,
  set when the last slot becomes IDLE during removal;
  `IoAcquireRemoveLockEx` is not relied on.
- **One party deletes, once, after the detach.** `SA_REMOVE_READY` and
  `SA_FREE_READY` only say that the slots are idle and no handle is open;
  they do not finish steps 4 and 5, which run outside the lock. So the
  deletion is decided by the fields, not by which thread happens to
  perform an action: both conditions (detached, `FreeReady`) are read
  and the claim (`Deleting`) is set in one hold of the lock, by REMOVE
  after its detach and by every close after its `SlotClose`. A close that
  ends the count while REMOVE is still between its rundown and its detach
  finds the device not yet detached and leaves the deletion to REMOVE's
  step 5; a close after the detach finds both and claims it. Nothing
  touches the device after `IoDeleteDevice`, and the image does not
  unload before (5.4). `MTPPRB.SYS` (`src/image/usbmtp/probe/`) follows
  this order.
- n becomes free only after the device object is deleted.
- **Power:** at a standby's query (or, without one, its set; or
  another component's device query below D0, or a
  device state below D0), the power gate closes and every slot is
  stopped with `STATUS_CANCELLED`: the owner's pending transfers, the
  bound handles' `WAIT_EVENT` and the interrupt poll; the standby is
  accepted only when they have ended, refused if they have not within
  the bound, and a set without preparation goes down at its bound with
  what is still out quarantined (5.4, "Standby"). Calls are refused
  (`STATUS_DEVICE_NOT_READY`) until the resume has completed and nothing
  is out, or, after a refusal, until nothing is out. The claim stays. After the resume the library treats the session
  as lost, since the device may have been re-enumerated or have reset its
  session across the standby: it opens a new session, refreshes the
  device (7.3), and starts the interrupt poll again with `EVENTS` start.
  Read 2026-11-17 with a transfer running (the load record): on ME the
  transfer in progress ended 21 at the gate, Cancel was refused 21 until
  the wake, then accepted, the device answered Session Not Open, and a
  new session moved and checked the data again; on 98 SE the old handle
  read 5 (the removal) and the new device worked. Standby with the
  explorer open is a leg (11.5).

**What is drained, and why none of it can wait on its own caller.** The
rule: no lock, pin, count that a deleter waits on, or shared state is held
across a call that can re-enter the driver or complete an IRP. Below is
every lock, count, event or wait that the deleter, a close, REMOVE, STOP,
the power path or `DriverUnload` waits for, re-derived from the code of
2026-11-16's sixth review (each row names where it is), what holds it,
and why no code holding it can reach that wait. "Modelled" means `slotmodel.c` plays it
(above); the rest is argued here.

| What is waited for (where in `usbmtp.c`) | Who waits | What holds it, and across what | Why the holder never reaches the wait |
|---|---|---|---|
| the device spin lock; the driver-global lock | everyone, briefly | `Slot*` calls, a few reads, a list insert; no call is made under either | nothing is called while it is held (modelled: the admission's hold) |
| the dispatch pins (`Entered`; `DeleteDev`'s loop on `SlotPinned`) | the deleter, after unpublishing (`SlotGone`) | `Late` for an IRP without a handle, from `SlotEnter` to `Leave`: its reads, `SlotActiveEnter`, and for a refused power IRP above `PASSIVE_LEVEL` its late-list entry; no call | no call is made under a pin, so no close and no deletion runs inside one; the hold that finds no pin publishes `g_Deleted`, so no pin is taken after it (modelled; the pin held across the completion, and the check and the publication in two holds with `SlotGone` ignored, are mutants, caught) |
| the admissions (`Active`; `ActiveDrain`) | REMOVE only, before it forwards REMOVE and detaches | `DispCreate` (an allocation, `SlotOpen`); `DispPnp` (`ForwardWait`, `IoCallDriver`, `Start`'s `Sync` reads, STOP's wait for quiet); `DispPower` (`PoCallDriver`, `PowerWait`, the wait for quiet); `DispOther` (`IoCallDriver`); a power IRP queued for the device's power worker until the worker has handled it; an accepted query's own admission, from its decision to its completion routine's hold (`QueryDone`), across the lower drivers' handling of it | a close in such a chain cannot claim the deletion (no detach until the admissions are 0), so it waits on nothing; the deleter never waits on an admission (modelled). REMOVE inside such a chain is the first assumption above |
| the device's power worker | nobody, as such: its IRPs hold admissions | `PowerWorker` handles each IRP under the admission it carries (`PowerForward`) and gives it up in the hold that takes the next, an accepted query's included (the query's completion gives up only the query's own); after the hold that finds the list empty it touches nothing of the device | covered by the admissions (modelled; a worker giving the admission up before forwarding, and a worker kept only by an accepted query's admission that its completion gives up, mutants, are caught) |
| the device workers' count (`g_Running`; `Unload`) | `DriverUnload` only | each device's power worker, from its queueing (`Power` above `PASSIVE_LEVEL`) to its last act | `DriverUnload` runs only inside the deleter's `IoDeleteDevice`, and the deleter is never in such a worker's chain: it calls out only under an admission, and no device is deleted while one is out (modelled; the deleter draining a worker that completes refused IRPs, the design before the fifth review, hangs) |
| the late worker's count (`g_LateRunning`; `Unload`) | `DriverUnload`, unless it runs on the late worker's own thread | the late worker, from the hold that refused its first IRP to its last act; it completes refused power IRPs at `PASSIVE_LEVEL` and touches no device | on its own thread `DriverUnload` is inside one of the worker's completions (a zombie's last close) and does not wait; from another thread the worker never waits on that thread. The count is raised in the refusing hold, or under the pin, so `DriverUnload` cannot have read it before (modelled; waiting on its own thread, not waiting at all, completing inline, and the list entry as a later step are mutants, caught) |
| the rundown (`RemoveReady`, REMOVE's loop) | REMOVE, before its detach | a slot's lower IRP with the stack, its timer and DPC, its cancel routine, its pins (`Run`: the submitter's `IoCallDriver`, each `IoCancelIrp`), a completion tail's `IoCompleteRequest` | a close in those chains cannot claim the deletion (no detach before the rundown) (modelled); REMOVE inside a lower completion, a DPC or a cancel routine falls under the first assumption |
| quiet: every slot idle, every tail done (`QuietWait`, no bound) | STOP only | as for the rundown | the second assumption above. STOP after a successful `QUERY_STOP` holds nothing. The activity ends by the driver's own cancels, which M0 read retiring bulk, interrupt and class requests; a pending pipe reset's cancel is unread |
| quiet, up to `QUIET_QUERY_MS` or `QUIET_SET_MS` (the standby rework) | a system query below working; a system or device set into sleep (its dispatch, or the power worker) | as for the rundown | bounded: at the bound a query is refused and a set goes down with what is out quarantined (modelled: the sleep inside a completion tail now passes; the unbounded wait restored, a mutant, ends stuck with a stack that keeps a cancelled request). Once under NUSB's EHCI a cancel made at a standby's gate was never returned (the third dependency above): what made that a hang |
| idle, up to `WaitMs` (at most 5 s; `WaitSlotsIdle`) | `ABORT`, `EVENTS` stop | as for the rundown | bounded: it returns which slots are still busy, and never hangs |
| the open count (`Opens`) | nobody waits: the last close claims the deletion | each open handle | not a wait (modelled: exactly one deleter in every order) |
| the IRP's own lower completion (`ForwardWait`, `PowerWait`, `Sync`) | START, CANCEL_STOP, S0 and D0, `Start`'s descriptor reads | the lower stack, for that one IRP | the wait is for the IRP's own completion, which the lower stack owes it; no count of this driver's is involved |



The driver uses the `wdmx86` profile (Windows 2000 DDK, MSVC 6.0), as
`usbprn` does, with a second compile against the 98 DDK's own WDM and USB
headers. Its imports come only from `NTOSKRNL.EXE`, `HAL.DLL` and
`USBD.SYS`. `NTKERN.VXD` has no PE export table to read (build.md 5.1),
so the gate requires every import to be in the package's allowlist and
declared in the 98 DDK's `WDM.H` (`USBD.SYS`'s in its `USBDLIB.H`), and a
`USBD.SYS` import also to be in ME's `USBD.SYS` export table as recorded
in `exports98/` (names and ordinals only; the same reading `libusb0`
records). 98 SE's `USBD.SYS` table is not recorded; there, as for NTKERN,
the guest load test decides. The gate shows only that the names exist;
whether they behave as needed is what the M0 and M1 legs show.

### 5.6 What M0 must show about NTKERN, before the program is written

A probe build of the driver's transport part and a probe program
(`MTPPROBE.EXE`, not shipped) read the following on 98 SE and ME, under
each stack.

1. With one thread blocked in a `ReadFile` on the owner handle, does an
   `ABORT` on a bound handle from a second thread complete? Does the read
   then return with the reason's status?
2. The Win32 view of each driver status:
   - `GetLastError` after `STATUS_CANCELLED`, `STATUS_IO_TIMEOUT`,
     `STATUS_DELETE_PENDING`, `STATUS_SHARING_VIOLATION`,
     `STATUS_ACCESS_DENIED` and `STATUS_INVALID_PARAMETER`;
   - and the byte counts `ReadFile` reports for short, zero-length and
     aborted reads.
3. **The order of cancel, `IRP_MJ_CLEANUP` and `IRP_MJ_CLOSE` with calls
   outstanding:** an owner handle with a `ReadFile` blocked in one thread,
   a bound handle with a `WAIT_EVENT` pending in another. The probe
   driver logs every entry with a timestamp. The cases:
   - `CloseHandle` of each handle from a third thread;
   - `ExitProcess` with both calls pending;
   - the program ended from the Close Program dialog;
   - `TerminateProcess` from another program.

   For each case the log records whether NTKERN cancels the pending IRPs
   itself, whether cleanup comes, whether close comes before or only after
   the pending calls end, and how long it all takes. Idle closes are
   recorded as well, as the control. Also recorded: whether NTKERN still
   delivers IRPs for handles left open after `IoDeleteDevice`, and
   whether the device extension stays valid until the last of them
   closes, as `slotmodel.c` assumes (5.4).
4. Two threads calling `DeviceIoControl` on two handles at once, and
   whether NTKERN serialises calls on one handle.
5. On each stack and both systems, for a pending bulk IN, bulk OUT,
   interrupt IN, control (class request) and pipe reset URB:
   - does `IoCancelIrp` complete it, and after how long;
   - if it does not, does `URB_FUNCTION_ABORT_PIPE` (bulk and interrupt)
     retire it;
   - for control and reset URBs, which nothing can abort, does it end on
     its own, and after how long (a device that never answers EP0 is
     modelled by the simulator);
   - does an aborted bulk IN URB report the exact bytes it received?
6. **What arrives for handles open across a removal, and whether a
   removed object may be kept.** Added 2026-11-16 after M0 read that 98
   SE gives a deleted device object's memory to the next device created:
   handles kept open across an unplug and a plug of the same and of
   another device, on both of 98 SE's stacks and on ME, with the counters
   showing whether any IRP of an old handle reached the new device.

**M0 findings the driver and the library must tolerate** (2026-11-16;
the record is [2026-11-16-mtp-m0.md](../testing/2026-11-16-mtp-m0.md)):

- **Removal differs by stack.** Under NUSB's EHCI the stack completes a
  pending bulk IN when the device goes (`STATUS_DEVICE_NOT_CONNECTED`,
  URB `C0007000h`, Win32 1167), 0.3 to 0.4 s before REMOVE; a new read
  issued in that gap is admitted and REMOVE then retires it. Under
  xHCI98 3.5.0.0 pending requests (bulk IN, bulk OUT, a class request)
  stay pending until REMOVE, where the driver's cancel retires them
  (Win32 `ERROR_ACCESS_DENIED` from `STATUS_DELETE_PENDING`). On fresh
  guests REMOVE came within about a second of the unplug, with any of
  those pending.
- **REMOVE late after an End Task.** When a program with calls pending on
  the device had been ended by End Task (the Close Program dialog), the
  next unplug's REMOVE came late: 2.5 to 10 minutes, on 98 SE under both
  NUSB's EHCI and xHCI98 3.5.0.0 and on ME under xHCI98 3.5.0.0, and a
  device plugged meanwhile was not enumerated until then. In every case
  seen it arrived with the guest's next input or activity (a typed
  command, a program closing its handles). The mechanism inside Windows
  was not established. Without an End Task no unplug was late (the
  cancel cases included). Meanwhile a cancel still retired a pending
  request at once, and a new request failed at once with the device gone
  (`STATUS_DEVICE_NOT_CONNECTED`, URB `C0007000h`). The library treats a
  device whose calls fail with the device gone as unplugged at once, and
  never waits for REMOVE (7.5).
- **A cancelled URB's status** is `C0010000h` under NUSB and `00010000h`
  under xHCI98 (5.4: decisions by the IRP status).
- **`CloseHandle` does not cancel.** With a `ReadFile` pending on a handle,
  `CloseHandle` of it from another thread blocks until the read ends (52
  to 86 s here, until it was cancelled from elsewhere); no cleanup or
  close reaches the driver meanwhile. The program cancels its own calls
  (an `ABORT` on its control handle) before it closes a handle.
- **Calls across handles.** In one process, a `ReadFile` on a second
  handle reached the driver at once while a read was pending on the first
  (the probe refused it busy), and every `DeviceIoControl` observed on
  another handle got through (the state reads, the stops and a pending
  interrupt read). A `ReadFile` from a second process was held until the
  first process's pending read ended (98 SE under both stacks, ME), also
  while the first process sat in a blocked `CloseHandle`; the IOCTLs
  observed from the second process were not held. A second program
  reading the same device is not supported.
- **Calls on one handle are serialised.** With an interrupt-read IOCTL
  pending on a handle, a second and a third call on the same handle (a
  state IOCTL, a class request) reached the driver only after the first
  ended. The control handle beside the I/O handle is therefore required:
  every stop, `ABORT` and status call goes on a handle with nothing
  pending.
- **The software wait** (`WAIT_EVENT`, no lower request) behaves like a
  read: `CloseHandle` of its handle blocks until it ends, and at process
  exit NTKERN calls its cancel routine.
- **End Task** from the Close Program dialog (after its "not responding"
  question) ends the program like `TerminateProcess`: the cancel routine
  of each pending IRP, then cleanup and close of every handle.
- **A pending bulk OUT** is retired by `IoCancelIrp` and by
  `URB_FUNCTION_ABORT_PIPE` within a tick, like a bulk IN; its URB's
  length field still shows the requested length after the failure, so
  only a successful URB's count is taken (5.4).
- **Termination cancels.** At `ExitProcess` and `TerminateProcess` NTKERN
  calls the cancel routine of each pending IRP, then sends cleanup and
  close for every handle about 0.1 s later; at a process's end, handles
  with nothing pending are closed first.
- **Handles across a removal.** IOCTLs, reads, `IRP_MJ_CLEANUP` and
  `IRP_MJ_CLOSE` all arrive for handles kept open after REMOVE: on 98 SE
  after `IoDeleteDevice` (the first probe builds), and with the zombie
  kept (5.4) on 98 SE and ME to the retained object, where none reached
  the new device (counted). 98 SE and ME accept a
  device object kept after REMOVE and deleted at its last close.

The probe confirms each request pending in the stack (through its state
control) before it stops it, and logs per request the IRP's and URB's
status, the length the URB reports and the bytes it copied (only from a
URB that succeeded). Its times come from the tick count (10 to 55 ms
steps) and every log line is written synchronously to port E9h, so the
latencies it records are coarse and include the logging's own cost; the
M0 record says so with each. A known limit of the probe's own removal:
REMOVE waits for every admitted power forwarding to return from
`PoCallDriver` before it forwards REMOVE, so a lower driver that waited
synchronously for REMOVE inside its power dispatch would deadlock it. A
guest that hangs at removal after a `power-drain-waiting-ms` line is that
case, and is itself a reading.

The program's design follows from the answers:

- **If 1 holds:** synchronous I/O. The device's I/O thread makes the
  blocking calls on the owner handle, and other threads use a bound
  handle. This is the design of section 8.
- **If 1 does not hold:**
  - Overlapped I/O is tried next, and used only if the probe shows it
    carries cancel and byte counts correctly on both systems.
  - Failing that, **bounded reads**. The cancel request cannot reach a
    blocked read, so cancel takes effect when the read ends:
    - The bulk IN timeout is 2 s while a data phase is flowing, and 10 s
      for the first packet of a phase (a device may take that long to
      start a large object).
    - A timeout aborts the transfer. It is never "continued": bytes may
      be lost in the abort, unless item 5 shows the stack reports them
      exactly, and even then the library does not resume a phase.
    - A timeout in a data phase therefore ends the transaction through
      the cancel procedure (7.5), whether the user asked to cancel or the
      device stalled.
    - The cancel latency is then up to the timeout running at the moment
      (2 s, or 10 s at the start of a phase). The progress window says
      "Cancelling..." for that time.

## 6. Why not more in the kernel

The transaction framing (7.2) could live in the driver, and that would
save calls. It stays in user mode:

- it is the part most exposed to device quirks;
- it is testable on the build host with fixtures;
- a fault there ends the program, not Windows.

## 7. The protocol library (user mode, C)

One library, `mtplib`, linked into the program and into a host test. It
holds the containers and datasets, the transaction sequence, cancel and
recovery, and the name, date and size rules.

### 7.1 Sources and the values used

Downloaded on 2026-11-16 (owner's approval) into the git-ignored
`docs/references/`, each archive in its own folder and unpacked there,
read as text only, never built or run, nothing of them tracked:

| Source | Archive, SHA-256 | Licence, as read |
|---|---|---|
| USB Still Image Capture Device Definition, Revision 1.0, July 11, 2000 (USB-IF), with its errata of 2007-03-16 | `usb_still_img10.zip` (`usb_still_img10.pdf`, `errata-SIC1_0-20070316.pdf`), `798ffd5c263838a4fdd4327428492126e8ed4dbfd779911be608f7bafe3f5e60` | "A license is hereby granted to reproduce and distribute this specification for internal use only" |
| Media Transfer Protocol, Revision 1.1, April 6, 2011 (USB-IF) | `MTPv1_1.zip` (`MTPforUSB-IFv1.1.pdf` and an adopters agreement), `2c388361870939be2eba65a57332803269712a425323f6d3f442ebba14496d8a` | the USB-IF adopters agreement in the archive (not signed here) |
| libmtp 1.1.22 (GitHub release) | `libmtp-1.1.22.tar.gz`, `8cc96989a2291281e7d7dd920bf60bf9e75d1be637557a392e6834cc43ed04e7` | `src/libmtp.c`, `ptp.c`, `libusb1-glue.c`, `util.c`: "GNU Lesser General Public License ... either version 2 of the License, or (at your option) any later version" |
| libgphoto2 2.5.31 (GitHub release) | `libgphoto2-2.5.31.tar.gz`, `bc382b764082b49fd90d69eed96b8329de52a5663e32256a698ed2082ab7a990` | `camlibs/ptp2/usb.c`, `ptp.c`, `ptp.h`, `ptp-pack.c`, `library.c`: the same LGPL "version 2 ... or any later version" line |

- Microsoft's OS descriptor specification was not at its old download
  address. Microsoft's pages "Microsoft OS Descriptors for USB Devices"
  and "Microsoft OS 1.0 Descriptors Specification" (learn.microsoft.com,
  both updated 2025-07-18) were read instead, on the web. The second lists
  the compatible id "MTP"; its download comes under a licence agreement
  (3.3).
- **libmtp and libgphoto2's `camlibs/ptp2`: code may be adapted** (owner,
  2026-11-16; until then they were facts only). The conditions:
  - Per file, before adapting, its own licence header is read. Code is
    adapted only from a file under LGPL "version 2 ... or any later
    version" (or 2.1 or later), or another licence `AGENTS.md` lists. A
    file under LGPL or GPL without "or later", or with a different or
    unclear licence, is skipped. That includes the device and quirk
    tables, the data files, libmtp's `music-players.h` and the vendor
    tables.
  - An adapted file stays under its LGPL, taken as LGPL-2.1-or-later,
    which `AGENTS.md` lists alongside GPL-2.0-or-later; it is not
    converted to GPL. It keeps its copyright lines and licence notice,
    gains an SPDX line, and marks this repository's changes with a date,
    as the LGPL asks. It gets a `NOTICE.md` row.
  - Adapted code lives in source files of its own, never mixed into files
    written fresh, so each file has one licence.
  - Where it helps: the per-device quirk and id tables, container parsing
    and dataset unpacking that copes with real phones' deviations, and the
    Android extensions.
  - It must build with MSVC 6.0 for 98 SE and ME and pass the import gate.
    It does not change this design's safety rules (bounds checked before
    use, the framing and cancel rules of 7.2 and 7.5): adapted parsing is
    hardened to them, not trusted as it is.
  - Every adapted file is listed with its source file, version and licence
    line.
  - **The device list** (owner, 2026-11-17; 3.2.1): rows of libmtp's
    `src/music-players.h`, whose notice reads "version 2 ... or any
    later version", are taken into a table that stays LGPL-2.1-or-later;
    for the model lines generated from it into `USBMTP.INF`, LGPL-2.1
    section 3 is applied, so those lines are under the GPL in that copy.
    This is the one exception to "not converted", and it is for data
    lines, not code.

  Nothing is adapted yet. Android's `frameworks/av/media/mtp` (Apache-2.0,
  not on `AGENTS.md`'s list) is not downloaded; Android's behaviour stays
  a bench reading.
- **QEMU `hw/usb/dev-mtp.c`** ("licensed under the terms of the GNU GPL,
  version 2 or later", QEMU 11.1): the responder used in the legs.
- No Microsoft or vendor binary is read beyond export tables; no DDK or
  WDK sample.

**Checked against the specifications (2026-11-16).** Every operation,
response, event, format, property and datatype code in 7.2 to 7.5 was
compared with MTP 1.1's tables, and all agree. Those tables are:

- operations: GetDeviceInfo 1001h to GetPartialObject 101Bh, MoveObject
  1019h, GetObjectPropValue 9803h, SetObjectPropValue 9804h,
  GetObjectPropList 9805h;
- responses: OK 2001h, Parameter_Not_Supported 2006h, Partial_Deletion
  2012h, Specification_By_Format_Unsupported 2014h, Device_Busy 2019h,
  Invalid_ParentObject 201Ah, Invalid_Parameter 201Dh,
  Session_Already_Open 201Eh, Transaction_Cancelled 201Fh,
  Specification_By_Depth_Unsupported A808h;
- events: the G.1 summary, 4001h to 400Eh, C801h, C802h;
- properties: StorageID DC01h, Object Format DC02h, Object Size DC04h,
  Object File Name DC07h, Date Modified DC09h, Parent Object DC0Bh;
- datatypes: UINT16 0004h, UINT32 0006h, UINT64 0008h, STR FFFFh;
- the Association format, 3001h.

The Android extension codes 95C1h to 95C5h are libmtp's (`src/ptp.h`).
The procedures were checked against the sections named in each place
below; the marks "(q)" of earlier drafts are dropped.

### 7.2 Containers and framing

- **Container:** UINT32 length, UINT16 type (1 command, 2 data,
  3 response, 4 event), UINT16 code, UINT32 transaction id, then
  parameters (up to 5 UINT32) or data. Little-endian.
- **Checked before use**, every container received:
  - length at least 12;
  - type as the phase expects;
  - code: in a data container, the command's code; in a response, any
    code, kept;
  - in a data or response container, a transaction id equal to the one
    sent (7.3);
  - a response's length 12 + 4k, k 0 to 5;
  - an event container (from the interrupt pipe): length 12 + 4k, k 0 to
    3, type 4;
    its transaction id is checked by the event's own rule (7.4), never
    against the transaction in progress.
  - Anything else is a protocol error. The library does not guess: it
    runs the cancel procedure (7.5) and reports the error.
- **Sizes:** all arithmetic on lengths and offsets is 64-bit unsigned and
  checked for overflow (MSVC 6.0 `unsigned __int64`).
  - A data container's length field is `payload + 12`, when that is at
    most FFFFFFFEh.
  - When `payload + 12` is FFFFFFFFh or more, the field holds the
    sentinel FFFFFFFFh: "the length is not given here; the phase ends at
    a short packet". This already applies to payloads from FFFFFFF3h
    bytes, so to files just under 4 GiB (a 4 GiB - 2 file is sent with
    the sentinel).
  - ObjectInfo's 32-bit ObjectCompressedSize holds sizes up to FFFFFFFEh.
    FFFFFFFFh there means "see the 64-bit ObjectSize property".
- **Two forms of a data phase.** Ordinary PTP sends the 12-byte header and
  the payload as one stream of packets: the header begins the first
  packet. MTP 1.1 (its USB appendix) also allows the header to travel as
  a packet of its own, a short packet of exactly 12 bytes, with the
  payload starting in the next packet, and a device that does so does it
  in both directions. The library calls these the combined and the split
  form.
  - **Detecting the form** from the device: the first read of a data
    phase returns exactly 12 bytes, as a short packet, and the length
    field is more than 12. Then the phase is split. Otherwise it is
    combined, as it is when the length is exactly 12 (no payload). With
    bulk packets of 8 bytes the header spans two packets; 7.9, "Framing
    on interface 1.0", gives the rule in packet terms for every size.
  - The form seen is remembered for the device. From then on, data sent
    to that device uses the same form. Until a split phase has been seen,
    the library sends the combined form.
  - Checked against MTP 1.1, Appendix H, "Splitting the Header and Data
    during the Data Phase":
    - a device that splits must do so for all data phases, in both
      directions;
    - operations, responses and events are never split;
    - the initiator finds out by observed behaviour, the suggested way
      being an operation with a known data phase such as GetDeviceInfo. So
      the library reads GetDeviceInfo first, before any data is sent.
    - An empty data phase is the 12-byte header alone, with no zero-length
      packet after it unless the container is a whole number of packets.
- **A response in place of the data container.** A device that fails an
  operation may send its response where the data phase was expected.
  libgphoto2's `camlibs/ptp2/usb.c` handles this case, as a fact read
  there. A first read of a data phase that yields a valid response
  container (type 3, the transaction's id, its length all read) ends the
  transaction with that response, without a data phase.
- **The sentinel** FFFFFFFFh may stand only in a data container (MTP 1.1,
  Appendix H, "Sending >4GB Binary Objects"). In a command, response or
  event container it is a protocol error.
- **Data phase from the device, finite length L (L < FFFFFFFFh):**
  1. Combined: read until exactly L bytes of the container have arrived.
     Split: the 12-byte header packet, then read until L - 12 payload
     bytes have arrived.
  2. A short packet before that count is a truncation: a protocol error.
     The 12-byte header packet of the split form is not one.
  3. More than the count cannot arrive within the container, since reads
     are sized to what is left, rounded up to whole packets; any excess
     in the last packet is a protocol error.
  4. The zero-length rule counts what was sent as one run of packets:
     the whole container (combined), or the payload alone (split). A
     zero-length payload in the split form is no run at all: the 12-byte
     header ends the phase, and no zero-length packet follows (Appendix
     H). Otherwise, if that count is a whole number of packets, the device
     may send a zero-length packet. The first read of a phase is exactly
     one packet, and each later read is what is left, rounded up to whole
     packets, so a read never runs past the container into the response.
     After the last read:
     - returns 0 bytes: consumed, and the response is read next;
     - returns data: that is the response container, kept and used, not
       discarded.
- **Data phase from the device, sentinel:** read until a short or
  zero-length packet, but in the split form the 12-byte header packet
  does not end it. The payload is what arrived after the 12 header bytes.
  It is checked against the 64-bit size the library expected, when it
  knows one (an object's ObjectSize).
- **Data phase to the device:**
  - Combined: the 12-byte header with `payload + 12` or the sentinel,
    then the data, as one run. If the whole run is a whole number of
    packets, `WRITE_ZLP` follows.
  - Split: the header alone (a 12-byte write), then the payload as its
    own run. If the payload is not empty and is a whole number of packets,
    `WRITE_ZLP` follows. An empty payload is the header alone, with
    nothing after it.
- **Boundary points**, each a host-test and fixture point (11.4):
  - payload FFFFFFF2h: length field FFFFFFFEh, the last finite length;
  - payload FFFFFFF3h: the first payload sent with the sentinel;
  - ObjectCompressedSize FFFFFFFEh: the last size given in ObjectInfo;
  - ObjectCompressedSize FFFFFFFFh: size read from ObjectSize.

  A file of FFFFFFF2h bytes (4 GiB - 14) and one of FFFFFFF3h bytes
  (4 GiB - 13) both fit under the FAT32 ceiling of 7.8, so both
  directions are legs (11.5).
- **Datasets:** every field is read through a bounds-checked reader over
  the container's payload.
  - A string: a UINT8 count of UTF-16 units including the NUL, 0 or 1
    to 255, fitting in what is left.
  - An array: a UINT32 count, and count times the element size fitting in
    what is left.
  - Bytes left over after the fields the library knows are ignored.
  - A dataset that fails a check makes that object "unreadable" in the
    list, with its handle, and stops nothing else.
- **Caps:**
  - a dataset or property list data phase: at most 64 MiB;
  - an object handle array: at most 1,048,576 handles;
  - a property list: at most 4,194,304 elements.

  A device that announces more gets a protocol error, and the program
  says the folder is too large to list. Object contents are streamed to
  the file and have no cap but their 64-bit size.

### 7.3 Sessions and transaction ids

- **Before a session:** GetDeviceInfo carries transaction id 0 (MTP 1.1,
  D.2.1: outside a session "both the SessionID and TransactionID ... must
  be 0x00000000"). OpenSession carries 0 too. MTP 1.1 does not say so in
  as many words, and libmtp's `ptp_opensession` does the same (a fact
  read there).
- **In a session:** the first transaction carries 1, and each next one 1
  more. FFFFFFFFh is never used; the id after FFFFFFFEh is 1 (MTP 1.1,
  4.3.3).
- **Session id:** always 1. SessionAlreadyOpen carries the open session's
  id in its first response parameter (D.2.2); it is logged.
- **One transaction at a time per device.** Only the device's I/O thread
  sends transactions. The program's other requests are queued to it as
  jobs: the window's requests, copies, and refreshes caused by events.
- **OpenSession answered SessionAlreadyOpen (201Eh):**
  - The driver's claim guarantees that no other program on this PC holds
    the device. So an open session can only be a stale one: this
    program's own from before a crash, or one left by an earlier claim.
  - Recovery: CloseSession, then OpenSession. If that fails: Device Reset
    (7.5), then OpenSession once more. If that fails too, the device is
    shown as needing a replug.
- **Other OpenSession failures:**
  - DeviceBusy (2019h): retried 3 times, 500 ms apart: the first
    OpenSession and three retries, 4 in all (CQ6). A fourth DeviceBusy
    ends the open (or the resume job) with `MTPE_DEVICE`, response
    2019h, and no Device Reset;
  - anything else: Device Reset once, then the replug message. No
    further OpenSession follows that reset (CQ28): the extra OpenSession
    after a reset belongs to SessionAlreadyOpen's recovery above only.
    The open returns `MTPE_DEVICE` with the refused OpenSession's code.
- **A lost session**, after a Device Reset, a standby, or an error that
  ends it, invalidates every object handle, every storage id and every
  cache of that device. After a standby the session is always taken as
  lost, whether or not a call failed (5.4, "Power"): the device may have
  been re-enumerated or have reset its session meanwhile. The library
  then opens a new session and calls `EVENTS` start, since the driver
  stopped the interrupt poll at the standby. The program reloads the device's tree from the
  storages down, and keeps the tree's open path where the names still
  exist.
- **The session count is raised when the session is taken as lost**,
  in that hold, not when the new session opens: at a Device Reset of
  7.5 step 6, a DeviceReset event's routing, a call classified as
  stopped (7.9, "Classifying a stopped call": the session "taken as
  lost"), a revoked claim or removal, and a resume's ending routine
  (7.9, which raises it again if it was already raised, harmlessly).
  Every job queued before that hold carries the old count and is
  dropped at its dequeue with `MTPE_SESSION`; nothing of the old session
  runs in the new one.
- **`EVENTS` start comes once, after the new session's OpenSession
  succeeded** (CQ24), whatever lost the session (a standby, a Device
  Reset of 7.5 step 6, a DeviceReset event, a quarantine's end): the
  resume job of 7.9 (during `MtpOpen`, the open's own transactions)
  sends OpenSession and then `EVENTS` start, once.
  7.5's step 6 no longer starts the poll itself (7.9's order holds over
  this section's). If that OpenSession fails, the poll stays stopped.
- **An operation after the loss, in the same job** (CQ4): a job carries
  the session count at which it was queued; every operation compares it,
  at its start and in the hold that reads the cancel flag, with the
  device's count and whether a session is open. A difference, or no
  session open, returns `MTPE_SESSION` at once with nothing sent (or,
  when the job's cancel flag is also set, `MTPE_CANCELLED`, which comes
  first in 7.9's order of codes), even
  after a new session has been opened: the job's handles, storage ids
  and listings belong to the lost session, and reopening never makes
  them valid again. The rest of that `run` is the program's to end.

### 7.4 Operations, properties and events

| Operation | Code | Use | Needed |
|---|---|---|---|
| GetDeviceInfo | 1001h | maker, model, version, serial; the operations, events and formats the device supports; vendor extension string (`microsoft.com: 1.0;`, and on Android `android.com: 1.0;`) | always; before the session |
| OpenSession / CloseSession | 1002h, 1003h | session 1 (7.3) | always |
| GetStorageIDs, GetStorageInfo | 1004h, 1005h | storages: description, label, capacity and free space (UINT64), access (read-write, read-only, read-only with delete); a storage id whose low 16 bits are 0 is absent and not shown | always |
| GetObjectHandles | 1007h | children of a folder: storage id, format 0, parent (FFFFFFFFh: the storage's root) | always. A camera that refuses a parent filter (ParameterNotSupported 2006h, SpecificationByFormatUnsupported 2014h, or InvalidParentObject 201Ah) is read whole once (parent 0), and the tree is built from each object's parent |
| GetObjectInfo | 1008h | name, format, size (32-bit, 7.2), parent, folder type (association, GenericFolder 0001h), capture and modification dates | always; the fallback for everything below |
| GetObjectPropList | 9805h | a folder's children in one transaction (below) | when listed in GetDeviceInfo |
| GetObjectPropValue | 9803h | the 64-bit ObjectSize (DC04h) of an object whose ObjectInfo says FFFFFFFFh | when listed |
| GetObject | 1009h | copy to the PC | always |
| SendObjectInfo + SendObject | 100Ch, 100Dh | copy to the device; a new folder is SendObjectInfo alone, format Association (3001h), size 0 | when listed and the storage is writable |
| DeleteObject | 100Bh | delete. A folder is first deleted as one call; so is each child folder met inside a PartialDeletion before it is descended into (CQ36). If the device reports PartialDeletion (2012h), its children are listed again and go first, depth first, then the folder; any other refusal is reported (7.9, "Uploads and deletion") | when listed and the storage allows delete |
| SetObjectPropValue on ObjectFileName (DC07h) | 9804h | rename | when listed, and GetObjectPropDesc says DC07h is settable |
| GetPartialObject, MoveObject, Android's 95C1h to 95C5h | 101Bh, 1019h, 95C1h to 95C5h | not used in the first version; the program works without them | - |

**GetObjectPropList for a folder:**

- **Request:** the folder's handle, format 0 (all), property FFFFFFFFh,
  group 0, depth 1. Per MTP 1.1, E.2.1:
  - property FFFFFFFFh means all properties except those of group
    FFFFFFFFh, which are slow and fetched separately;
  - the depth counts from and includes the object named, so a folder's
    own elements come back with its children's, and the library drops the
    elements of the folder's own handle.
- **The root of a storage** has no folder handle, and the operation has
  no storage parameter. Handle 0 means "all objects at the root level"
  (E.2.1), of every storage. With depth 1 that is the roots' objects, and
  the results are filtered by each object's StorageID (DC01h). Handle 0
  with depth 0 is an empty set by the specification, and is never sent.
- **Used from each element:**

  | Property | Code | Expected datatype |
  |---|---|---|
  | StorageID | DC01h | UINT32 (0006h) |
  | ObjectFormat | DC02h | UINT16 (0004h) |
  | ObjectSize | DC04h | UINT64 (0008h) |
  | ObjectFileName | DC07h | STR (FFFFh) |
  | DateModified | DC09h | STR |
  | ParentObject | DC0Bh | UINT32 |

  An element whose datatype is not the expected one is ignored, and that
  object is read with GetObjectInfo. Elements of unknown properties are
  skipped by their datatype's size; an unknown datatype ends the list's
  parsing, and the objects not yet seen fall back.
- **Fallback:** a non-OK response, a protocol error or a cap reached
  makes the library list that folder with GetObjectHandles and
  GetObjectInfo. It remembers the refusal for the rest of the session.
  Only a refusal (a complete non-OK response) is remembered (CQ38): a
  protocol error or a cap reached falls back for that listing alone, and
  the next folder tries GetObjectPropList again. What is remembered is
  session state: a new session (any loss of 7.3) forgets it, with every
  other refusal remembered for the session.
  Whether Android answers depth 1 is unverified.

**Events** (from `WAIT_EVENT`; each one becomes a job in the device's
queue, 7.3, except CancelTransaction and DeviceReset, which act on the
engine's state at once; parameters, session tags and coalescing in 7.9,
"Events"):

- ObjectAdded and ObjectRemoved (4002h, 4003h): refresh that object's
  folder, if read.
- StoreAdded and StoreRemoved (4004h, 4005h), and StorageInfoChanged
  (400Ch): refresh the device's storages. An Android phone that was
  locked may gain its storage this way (unverified).
- ObjectInfoChanged (4007h), and MTP's ObjectPropChanged (C801h):
  refresh the row.
- DeviceInfoChanged (4008h): GetDeviceInfo again (CQ32): the refresh
  job, before it calls the program's event callback with the device as
  its target, sends GetDeviceInfo with the session's next transaction
  id; an OK, validated DeviceInfo replaces the kept one (its strings and
  the operations, events and formats listed) in one hold, and the kept
  one is used until then. The split form and the endpoint addresses
  learned at the open stay. If that GetDeviceInfo fails, the kept one
  stays, the failure is traced, and the callback is still called.
  Otherwise `MtpDeviceInfo` returns the DeviceInfo kept from the open
  and sends nothing.
- DeviceReset (400Bh): MTP 1.1, G.2.12, says it is sent to open sessions
  other than the one that caused it and means "the sessions are about to
  be closed". The session is taken as lost (7.3): every handle, storage
  id and cache of the device goes, and a new session is opened.
- CancelTransaction (4001h): "cancel the transaction identified by the
  TransactionID in the event dataset. If the transaction has already
  completed, this event shall be ignored" (G.2.2). So it counts only if
  its id is that of the transaction in progress; then the I/O thread runs
  the device-initiated case of 7.5. With any other id it is logged and
  ignored.
- For the other events, the transaction id is not used. MTP 1.1, 4.8.3.3,
  fills it only when the event belongs to a transaction; the still image
  specification's extended event data (5.2.2) uses 0 when none applies.
  An event's id is never compared with the transaction in progress.
- An event with the overflow flag, or one that fails the container
  checks: a full refresh of the device.
- Other events are ignored.

### 7.5 Cancel and recovery

The procedure follows the still image class specification, 7.2.1.1
"Rule Set":

- **Host-initiated.** "The host shall cancel a data transfer by no longer
  issuing tokens on the Bulk-Pipe and then issuing a class-specific Cancel
  Request to the device. The host shall then poll the device with
  Get_Device_Status Requests and when the device returns OK status
  commands may be resumed."
- **Device-initiated.** The device stalls the bulk endpoints. The host
  learns the reason and the stalled endpoint numbers from Get Device
  Status, clears the halts with Clear_Feature, and then the device
  returns OK.
- "Normally only the Data phase is cancelled as the Command phase and the
  Response phase are short" (7.2.1).
- The class requests (5.2) go to the interface. The errata of 2007-03-16
  change their `wIndex` from 0 to the interface number, which the driver
  uses.
- Get Device Status data (5.2.4.2, Table 5.2-7): `wLength` (UINT16, the
  total length), the code (UINT16: 2001h OK, 2019h Device Busy, 201Fh
  Transaction Cancelled), then parameters, which "for device initiated
  cancels" each contain an endpoint number. The table leaves the
  parameter size open. This design takes 4 bytes each (step 4).
- libmtp (`libusb1-glue.c`, a fact read there) reads only the first 4
  bytes and notes that Linux's MTP implementation returns two 16-bit
  fields, so a status of `wLength` 4 is the common case.

**The transaction's phase.** The I/O thread keeps the phase of the
transaction in progress, under a lock that the cancel path also takes:

- COMMAND: the command block is being sent;
- DATA-OUT or DATA-IN: the data phase;
- RESPONSE: the data phase (if any) is over and the response is awaited
  or being read;
- IDLE.

**Host-initiated cancel** (the user's Cancel, or a protocol error the
library found):

1. **Phase check and stop.** Under the lock:
   - In IDLE or RESPONSE, nothing is cancelled. The host does not cancel a
     response. The I/O thread reads the response to its end, within its
     normal timeout, completes the transaction, and then drops the rest of
     the job. If the response does not come, that is a stuck device: step
     6. No `ABORT` is issued and the cancel thread is not signalled in
     these two phases (CQ20).
   - **A response that does not come**, with or without a cancel (CQ35),
     is a stuck device: the read's time-out (or 7.2's framing time-out)
     goes to step 6 with no Cancel Request, under step 6's own rules: no
     reset is sent when the device was found removed (`MTPE_GONE`), is
     given up, its gate is closed (7.9, "Recovery under a closed gate"),
     or step 6.3 cannot establish quiet (then step 7). The session is
     lost there;
     the jobs queued under it are dropped with `MTPE_SESSION` at their
     dequeue; a mutating operation whose response was lost is
     `MTPF_UNCERTAIN` and is never sent again. The call returns
     `MTPE_TIMEOUT` with `MTPF_SESSION_LOST`, or, when the job's cancel
     flag was set before the time-out (a cancel in RESPONSE, CQ26),
     `MTPE_CANCELLED` with `MTPF_SESSION_LOST`, by the order of 7.9's "One
     code, and flags"; the reset runs either way.
   - In COMMAND, DATA-OUT or DATA-IN, the phase becomes CANCELLING. The
     I/O thread submits nothing more for this transaction.
2. **Settle the race.** `ABORT` of bulk IN and bulk OUT, waiting up to
   1 s. `ABORT` returns only once the slots are IDLE, so the byte counts
   it leaves are final (5.4).
   - If the data phase in fact completed before the abort took effect
     (every byte of it moved), the transaction is in RESPONSE, and step
     1's first case applies: no class cancel.
   - If `ABORT` reports a pipe still not quiet: step 7. No cancel or
     reset is sent while a transfer may still be in the stack.
3. **Cancel Request** with the transaction id. A failure goes to step 6.
4. **Status: act only on these combinations.** Get Device Status, read
   into 64 bytes:

   | Bytes returned and `wLength` | Code | Parameters | Action |
   |---|---|---|---|
   | 4 | 2001h OK | none | ready: step 5 |
   | 4 | 2019h Device Busy | none | query again after 50 ms |
   | 4 | 201Fh Transaction Cancelled | none | query again after 50 ms (the cancel is acknowledged, nothing halted) |
   | 8 or 12 | 201Fh Transaction Cancelled | 1 or 2 UINT32, each naming this interface's bulk IN or bulk OUT endpoint, no endpoint twice (matching below) | `RESET_PIPE` on each named pipe, then query again |

   **Matching an endpoint parameter.** The specification says only "an
   endpoint number", of a size it leaves open, so this is the design's
   policy. A value with bit 7 clear is read as a bare number, never as an
   OUT address, so 01h is endpoint number 1 whichever direction has it
   (CQ23); each parameter is resolved by these steps, in order:
   1. Bits 8 to 31 not all zero: not one of the two (step 6).
   2. Bit 7 set (an IN address): it names bulk IN when it equals bulk
      IN's address (81h is bulk IN 1); otherwise not one of the two.
   3. Bit 7 clear: bits 4 to 6 not all zero, or a number of 0: not one of
      the two. Otherwise the number names the bulk pipe(s) whose address
      has that number in bits 0 to 3: bulk IN alone, bulk OUT alone, or,
      when bulk IN and bulk OUT share the number, both, which are then
      both reset. Both are quiet at that point (step 2), so resetting the
      other is harmless. Neither: not one of the two.
   4. After every parameter is resolved: a pipe named twice (by two
      parameters, or by a shared bare number and another parameter) is a
      repeated endpoint (step 6).
   - Anything else is not one of the two: step 6.

   Anything else goes to step 6 without touching a pipe:
   - a `wLength` that differs from the bytes returned;
   - any other length or code;
   - an endpoint that is not one of the two;
   - a repeated endpoint;
   - OK with parameters.

   Each query is a `CLASS_REQUEST` (5.4's bounds apply).
5. **Ready.** The next transaction runs with a fresh id. A response
   container carrying the cancelled id, if one is the first thing read,
   is accepted and dropped, once.
6. **Device Reset**, for a step that failed, a malformed status, or the
   deadline:
   1. `EVENTS` stop, waiting up to 1 s.
   2. `ABORT` of both bulk pipes, waiting up to 1 s.
   3. **Only if both report quiet:** the interrupt poll stopped with its
      URB finished, and both bulk slots IDLE. If either is not, the reset
      is not attempted: step 7. A reset with a transfer still in the
      stack could race it.
   4. Device Reset Request.
   5. `RESET_PIPE` on bulk IN, bulk OUT and the interrupt pipe.
   6. Get Device Status until OK, by step 4's table.
   7. (Withdrawn: `EVENTS` start now follows the new session's
      OpenSession, once, 7.3 and CQ24.)
   8. The session is lost (7.3): every handle, storage id and cache of the
      device goes, and a new session is opened (by the resume job, 7.9,
      which then starts `EVENTS`).

   **A step of 6 that fails**: the Device Reset
   Request or a `RESET_PIPE` failing with any code but 21 or an
   uncancelled 995, or a Get Device Status that never reaches OK by step
   4's table, goes to step 7 (no second reset is tried). 21 or an
   uncancelled 995 stops recovery as 7.9's "Recovery under a closed
   gate" says. Step 6's own deadline passing does the same as a failure:
   step 7.
7. **Given up.** The device's node says "The device stopped responding.
   Disconnect it and connect it again.", and nothing more is sent to it.
   Requests still pending in the driver keep their contexts until the
   stack completes them, or until removal (5.4). The program does not
   wait for them.

**Device-initiated cancel:**

- The device halts its bulk endpoints mid-transaction, and may send the
  CancelTransaction event (7.4).
- The library sees a stalled read or write, or the event.
- Get Device Status (validated as in step 4) is expected to report
  TransactionCancelled (201Fh) with the halted endpoints.
- Each listed endpoint is reset, and the status is queried until OK; then
  step 5. The transaction is reported as cancelled by the device:
  `MTPE_DEVICE` with response code 201Fh (7.9, "A bulk failure of no
  named Win32 code"); the job is not cancelled and goes on.
- A malformed status, or no OK, goes to step 6.

**REMOVE is never waited for.** M0 read that after an End Task an unplug
may be reported to the driver minutes late, on both of 98 SE's stacks and
on ME (5.6, findings), while a cancel still retires a pending
request at once and a new request fails with the device gone. So what
frees the user after an unplug is the library's own deadlines and its
explicit cancels (`ABORT` on the control handle), never the arrival of
REMOVE; a device whose calls fail with the device gone is treated as
unplugged at once, and its node goes away when Windows reports it.

**One deadline.** The whole procedure has one absolute deadline: 10 s
after the cancel began, read from `GetTickCount` before every step.

- A step about to start after the deadline goes to step 6.
- Step 6 itself has a second deadline, 10 s after it began. Past it, step
  7.
- **The outer bound** (CQ19): the observer (7.9) gives the device up at
  the earlier of step 6's own deadline (when step 6 was entered) and 20 s
  after the procedure began, so a procedure whose I/O thread is blocked
  in its first class request, and so never reaches step 6, is given up
  20 s after it began. This holds alike for a user's Cancel and for
  automatic recovery (a failed call, an event's CancelTransaction or
  DeviceReset, a lost response); "began" is the hold in which the phase
  became CANCELLING, or in which the failed call's result was published.
  These are overlapping absolute budgets measured from their own starts,
  not allowances that add up the time-outs of the calls inside them:
  20 s is the budget; the observer, which checks every 500 ms, may act
  on it up to 500 ms later, and no later.
- The procedure runs on the device's recovery thread, never the window's.
  That thread checks the deadlines between its calls, but it cannot
  notice one while it is blocked in a driver call.
- **The independent observer** (7.9 moves the guard into the library:
  the cancel thread watches every deadline, also of automatic recovery,
  open, close and the command-line mode, and gives the device up; the
  window's timer below remains only for the progress window) was first
  drawn as the window thread. When Cancel is
  pressed, it notes the time and runs a `WM_TIMER` every 500 ms. If the
  recovery thread has not reported by 20 s after the Cancel (plus the read
  timeout of 5.6 in the bounded-read case), the window thread:
  - marks the device given up (step 7): no further job is sent to it, and
    its node says "stopped responding";
  - closes the progress window.

  The window thread never waits on the recovery thread. When the
  recovery thread's call returns later (when the stack ends it, or at
  removal), it finds the given-up mark and stops.
- How long a single call can take is bounded only as 5.4, "Waits and
  bounds", says. For `ABORT` and `EVENTS` that is their wait argument.
  For class requests and `RESET_PIPE` it is unknown until M0 has read how
  the stacks retire a cancelled control or reset request.

**What the user sees.** The progress window's Cancel changes at once to
"Cancelling..." and greys out. It closes when the device is ready again
(step 5) or has been given up (step 7). At the latest that is when the
window thread's observer gives up, 20 s after the Cancel (plus the
bounded-read timeout), whatever a driver call is doing. After that the device is either usable or marked "stopped
responding". The legs measure the actual times (11.5).

Errors during a transfer enter at step 2. Interface 1.0 does not report
a stall apart from other bulk failures, so a bulk failure that is not a
cancel, a time-out or the device gone reads Get Device Status before any
Cancel Request and takes the case it reports (7.9, "Errors from the
driver"). How a cancel is submitted and settled against a call in flight
is 7.9's "Submitting a call, and cancelling it".

### 7.6 Names

MTP names are UTF-16, up to 255 code units including the NUL. 98 SE and
ME are ANSI systems: their Win32 file functions take the system code page
(`CreateFileW` is exported but a stub), and their list and tree controls
are ANSI.

- **Shown:**
  - Converted with `WideCharToMultiByte(CP_ACP)`; a character the code
    page lacks shows as `?`.
  - The object's identity is its handle, so two names that both show as
    `??.jpg` are still two files.
  - The Properties dialog shows the exact name, with `U+XXXX` for
    characters outside the code page.
- **Copied to the PC.** Each name is mapped in this order:
  1. Characters outside the code page, and characters Windows forbids in
     names (`\ / : * ? " < > |` and 01h to 1Fh), become `_`. A `\` or `/`
     inside a device name therefore never makes a path step.
  2. Trailing dots and spaces are dropped.
  3. An empty result, `.` or `..` becomes `_`.
  4. A reserved device name becomes `_` plus the name, with or without an
     extension and in any case: `CON`, `PRN`, `AUX`, `NUL`, `CLOCK$`,
     `COM1` to `COM9`, `LPT1` to `LPT9`. So `con.txt` becomes `_con.txt`.
  5. If the full path would pass MAX_PATH (260) less 13 (room for the
     temporary name of 7.8), the name is shortened, keeping its extension.
  6. Clashes are checked last, after every change above:
     - against the folder's existing files, case-insensitively and
       including their 8.3 aliases;
     - and against the other names of the same copy.
     - A clash gets " (2)", " (3)" and so on before the extension, and is
       checked again.
  - Every name changed this way is listed in the copy's summary.
  - **Containment:** the destination path is built only from mapped
    names, joined under the folder the user chose. Before the file is
    created, `GetFullPathName` of the result must start with that
    folder's own full path and a `\`; otherwise the file is skipped as an
    error.
- **Copied to the device:** the ANSI long name, converted with
  `MultiByteToWideChar(CP_ACP)`.
- **A character outside the BMP** (a surrogate pair in a device name) is
  one character for the rules above (Q27, decided (a)): one `_` when
  copied to the PC, one `?` when shown; the pair is kept as such in the
  library's copy of the name, and the Properties dialog shows it as
  `U+XXXXX` (U+1F600, say). An unpaired surrogate is U+FFFD (7.9,
  "Strings"). The alternative that was weighed, each UTF-16 unit mapped
  apart (two `_`, two `?`), is not used. This mapping is part of rule 1
  and so comes before rule 6's clash check: two device names that both
  map to `x_y.txt` are still two files, and the second gets " (2)".

### 7.7 Dates

- PTP dates are strings, `YYYYMMDDThhmmss`, with optional tenths (`.s`)
  and an optional `Z` (UTC) or `+hhmm`/`-hhmm` (MTP 1.1, 3.2.5).
- Without a zone the date is taken as local time; with one it is
  converted. A date outside 1980 to 2107 (FAT's range) or unreadable is
  not used.
- Copied to the PC: set as the file's modification time (`SetFileTime`;
  FAT keeps 2 s steps). A missing date leaves the time of the copy.
- Copied to the device: the PC file's modification time, sent as local
  time without a zone. Whether Android keeps a date sent in
  SendObjectInfo is unverified.

### 7.8 Copying safely, and large files

**Copy to the PC: what is guaranteed.** While Windows keeps running,
every step below is checked, and at every point:

- the existing file's contents are under its own name or under a backup
  name the program reports;
- a copy the program reports as done has its data under the target name;
- a copy that fails after its data was complete either keeps the data
  under a temporary name the program reports, or is reported as failed
  with the temporary file deleted (step 4.4's first case). It is never
  reported as done in that case.

Nothing is promised across a crash or a power loss. FAT has no journal,
and a crash can leave a temporary or backup file in the folder (the
readme names them) or lose data the disk had not yet written. 98 SE and
ME have no atomic replace (`MoveFileEx` is a stub there).

1. **Write to a temporary file.** The data goes to a file in the
   destination folder created with `CREATE_NEW`: `~MTPnnnn.TMP`, nnnn
   hex, the next nnnn on a clash. Every write is checked. At the end, in
   order and each checked: the file's size against the object's,
   `FlushFileBuffers`, `CloseHandle`. The original is not touched before
   all three have succeeded.
2. **On failure or cancel** before that, the temporary file is deleted.
   If even the delete fails, its path is reported. The target is
   untouched.
3. **No file of the target name:** `MoveFile` from temporary to target.
   If it fails (for example, another program has meanwhile created the
   target), the temporary file is kept and its exact path reported.
4. **Replacing a file** (the user chose Replace):
   1. `MoveFile` the target to an unused backup name `~MTPnnnn.OLD`.
      `MoveFile` refuses an existing name, so the next nnnn is tried on a
      clash. If this step fails for another reason, nothing has changed:
      the temporary file is deleted and the error reported.
   2. `MoveFile` the temporary file to the target name.
   3. If step 2 worked: delete the backup. If the delete fails, the backup
      stays and its exact path is reported.
   4. If step 2 failed: `MoveFile` the backup back to the target name. If
      that works, the error is reported, and the temporary file is
      deleted (or its path reported). If it fails too, both files stay,
      and both exact paths are reported: "Your original file is now
      <backup path>; the copied file is <temporary path>."
5. Temporary and backup files left by a crash are named in the readme.
   They are not swept automatically, since the program cannot tell
   whether a backup is the only copy of someone's file.

**Sizes on the PC's disk:**

- **FAT32 on disk** holds a file of up to 2^32 - 1 bytes. Whether 98 SE's
  and ME's file system code reaches that, or stops lower (4 GiB - 2 is
  often cited), is not measured here.
  - Until M4 measures it, the program uses 4 GiB - 2 bytes on FAT32, as
    a conservative ceiling.
  - A larger file is refused before the copy, with a message (owner,
    2026-11-16: refused, not split).
- **FAT16 (`FAT`):** its volumes are at most 2 GB on these systems, so
  free space is the limit that matters. No per-file rule is applied.
- **Any other file system** (a network drive, a third-party file system;
  names from `GetVolumeInformation`) has no limit known to the program.
  - Nothing is refused in advance on it.
  - `GetDiskFreeSpaceEx` is consulted where it answers.
  - A write or seek that fails mid-copy (disk full, a size limit) fails
    the copy cleanly, by step 2 above.
- Files of 2 to 4 GiB are written with 64-bit sizes and
  `SetFilePointer`'s high part. The signed boundary 2^31 is a test point
  (11.5).

**Sizes on the device:**

- 64-bit throughout: ObjectSize, capacities, free space.
- Copying to the device is limited by what the PC's disk can hold, so up
  to the FAT32 ceiling above. The length sentinel of 7.2 covers files
  just under 4 GiB.
- A partly sent object left on the device after a failed or cancelled
  SendObject is deleted only when it is the upload's own reserved handle
  and SendObject's data phase had not begun; otherwise the outcome is
  uncertain and the user decides (7.9, "Uploads and deletion"). Whether
  devices keep one is unverified per device.

### 7.9 The library stage (plan of 2026-11-16; revised 2026-11-17 for interface 1.1, the standby decision and the device list)

The design that the library's code is reviewed against; no library code
is written before this section has been reviewed. The driver is
unchanged for this stage: its load and standby readings are
[2026-11-16-mtp-m1-load.md](../testing/2026-11-16-mtp-m1-load.md), and
Q18 was decided with no driver change (14). Nothing here changes the
driver or its interface 1.1 (5.1 to 5.4). The stage also brings the
device list into `USBMTP.INF` (3.2.1), and it is accepted by the legs of
section 12, "M1's library stage". Before any library code, the contracts
below (lifetime, jobs, submission and cancel, the power hold,
operations, framing, recovery, interface 1.1, parser ownership) are
written out as host test cases (the host test, below), and those cases
are reviewed with this section. Where this section and 7.2 to 7.8
differ, this section holds and the earlier text says so.

**Files.** All written here, GPL-2.0-or-later, in `src/image/usbmtp/lib/`
unless named:

| File | What | Lines (estimate) |
|---|---|---|
| `mtplib.h` | the interface below, the only header a program includes | 300 |
| `mtpio.c`, `mtpio.h` | the transport: `\\.\USBMTP<n>`, the controls of 5.3, `ReadFile` and `WriteFile`, behind a table of functions (`MTPIO_OPS`) that the host test replaces with a simulated device | 450 |
| `ptprd.c`, `ptprd.h` | the bounded reader and writer: bytes, UINT16 to UINT64, strings, arrays, each read checked against what is left; the decoding budget | 350 |
| `ptpframe.c` | containers and data phases (7.2, and the framing rules below) | 600 |
| `ptpsess.c` | the device's lifetime, the transaction engine, sessions and ids (7.3), the submission handshake, cancel and recovery (7.5) | 1,000 |
| `ptpdata.c` | DeviceInfo, StorageInfo, ObjectInfo, handle arrays, property lists | 700 |
| `ptpops.c` | the operations of the first cut, each with its contract (below) | 500 |
| `mtpevent.c` | event containers, their parameters, routing and coalescing (7.4, below) | 250 |
| `mtpnames.c`, `mtpdates.c` | 7.6 and 7.7, pure functions | 300 |
| `mtppower.c` | the power thread and its hidden window, over the devices a program attaches (below, "Standby") | 200 |
| `src/image/usbmtp/hosttest.c`, `simdev.c`, `hosttest.ps1`, `testdata/` | the host test, the simulated device, fixtures and expected results (11.3, 11.4) | 1,800 |
| `src/image/usbmtp/hostredir.c` | host only: an `MTPIO_OPS` that speaks usb-redir to `mtpemu.py` on the build host (below, "What the build host can test") | 600 |
| `src/image/usbmtp/mtpids.txt`, `mtpids-libmtp.txt`, `libmtpids.py`, `mtpids.ps1`, `geninf.ps1`, `usbmtp.inf.in` | the device list, its generator and the INF's fixed template (3.2.1) | 500 and the table |

About 4,650 lines of library, where section 10 had 3,500 before this
revision: the lifetime, submission, power and parsing rules below are
what was added. A
directory `lib/lgpl/` exists only if code is ever adapted (below).

**The interface** (`mtplib.h`; C, ANSI, MSVC 6.0; no global state, every
call on an opaque `MTP_DEV`):

- Results: `MTPE_OK`; `MTPE_DEVICE` (a response other than OK, its code
  kept); `MTPE_PROTOCOL` (malformed data, with a reason code that the
  program shows in "The device sent data this program cannot read (%s).");
  `MTPE_GONE` (the device was removed, or replaced: below); `MTPE_INUSE`
  (claimed by another program); `MTPE_TIMEOUT`; `MTPE_CANCELLED`;
  `MTPE_GIVENUP` (7.5 step 7); `MTPE_SESSION` (the session was lost
  and the call's handles are no longer valid); `MTPE_POWER` (the call
  was found stopped while the driver's power gate was closed: 7.9's
  "Classifying a stopped call"; a standby, refused or accepted, or
  another component's power change, which one not always known, and the
  cause inferred from the timing, not proven);
  `MTPE_STOPPED` (the call was stopped by the driver and the library
  found no evidence of why: the conservative fallback); `MTPE_HELD`
  (not started: the program held the device for a power change, and
  nothing of the call was sent); `MTPE_CLOSING`
  (the device is being closed); `MTPE_TOOLARGE` (a cap of 7.2 or of the
  decoding budget); `MTPE_NOMEM`; `MTPE_ARG` (a bad argument, or a call
  that "Callbacks" does not allow); `MTPE_SOURCE` (the caller's source
  callback failed, or delivered fewer bytes than ObjectInfo announced:
  CQ25). The earlier `MTPE_STANDBY` is renamed
  `MTPE_POWER`, and the earlier code `MTPE_UNCERTAIN` becomes a flag
  (below): a result has one code.
- **A bulk failure of no named Win32 code** ("Errors from the driver",
  below) is `MTPE_DEVICE` (CQ25): the response code field carries 201Fh
  when 7.5's Get Device Status showed a device-initiated cancel, else 0,
  and the Win32 field the failing call's code. A device-initiated cancel
  that no bulk failure began (an event's CancelTransaction) is reported
  the same way: `MTPE_DEVICE`, response code 201Fh, the device's own
  report (7.5). `MTPE_PROTOCOL` is only for data the
  device sent that fails a check, and `MTPE_STOPPED` only for a call the
  driver stopped (995 or 21) with no evidence of why.
- **One code, and flags.** Every call returns its `MTPE_*` and fills an
  `MTP_RESULT` when the caller passes one: the code; the flags; the
  response code (`MTPE_DEVICE`); the protocol reason (`MTPE_PROTOCOL`);
  the last driver call's Win32 error (0 if none); the bytes moved, from
  published results only (below, the handshake). The flags:
  `MTPF_UNCERTAIN`, the device may have acted on a mutating operation
  whose outcome is not known (below); `MTPF_SESSION_LOST`, the session
  ended with this call; `MTPF_STANDBY`, with `MTPE_POWER` only: the
  reading right after the stop showed a system reason (1 or 4), a
  standby being prepared, accepted or set; `MTPF_POWER_SEEN`, the
  refused-query or forced-set count changed during the job, with no
  claim that this stopped the call; `MTPF_EDIT_OPEN`, an Android edit
  may still be open on the device: no complete response to an
  EndEditObject of it was read, because its cleanup was not admitted or
  its response was lost (CQ9; "Edit lifetime"); `MTPF_UNVERIFIED`, an
  upload settled OK but its readback was not made or did not complete
  (CQ21; "Readback"). When more
  than one code would apply, the first of this order is the code, and
  what the others add is in the flags: `MTPE_GONE`, `MTPE_GIVENUP`,
  `MTPE_INUSE`, `MTPE_CLOSING`, `MTPE_POWER`, `MTPE_HELD`, `MTPE_CANCELLED`,
  `MTPE_SOURCE`, `MTPE_STOPPED`,
  `MTPE_TIMEOUT`, `MTPE_SESSION`, `MTPE_DEVICE`, `MTPE_PROTOCOL`,
  `MTPE_TOOLARGE`, `MTPE_NOMEM`, `MTPE_ARG`. So a job stopped by the
  close's latched cancel, or cancelled while queued and then dropped by
  the close's step 1, gets `MTPE_CLOSING` (CQ15); a transaction a
  DeviceReset event cancels gets `MTPE_CANCELLED` with
  `MTPF_SESSION_LOST` (CQ5); a cancel in RESPONSE whose response is then
  lost gets `MTPE_CANCELLED` with `MTPF_SESSION_LOST` (CQ26; 7.5 step 1).
- **The bytes moved** (CQ22) are object payload bytes, never container
  bytes: the sum of the published results' counts of the data phase less
  its 12-byte header, for `MtpGetObject`, `MtpGetPartial`,
  `MtpSendObject` (SendObject's payload only; neither SendObjectInfo's
  dataset nor the readback counts) and `MtpAndroidSendPartial`. Every
  other operation reports 0. The transport's own counts (containers,
  headers, zero-length packets) are the trace's.
- **The reason** field is filled for `MTPE_PROTOCOL` and `MTPE_ARG` only
  (CQ12, CQ30), with one of these named values; 0 otherwise. A case
  names the reason it expects:

  | Reason | Code | Meaning |
  |---|---|---|
  | `MTPR_SHORT` | PROTOCOL | a field does not fit in what is left of the dataset or container |
  | `MTPR_COUNT` | PROTOCOL | a count times its element's minimum wire size does not fit in what is left (checked before the cap: CQ39) |
  | `MTPR_STRING` | PROTOCOL | a string's count does not fit in what is left |
  | `MTPR_CLEN` | PROTOCOL | a container's length field below 12, or not matching what arrived (the data ending short of it, or going past it in its last packet), or not 12 + 4k (k 0 to 5) for a response, (0 to 3) for an event |
  | `MTPR_CTYPE` | PROTOCOL | a container of the wrong type (a data container where a response is due counts as a response in place of the data phase, 7.2, not this) |
  | `MTPR_CCODE` | PROTOCOL | a data container whose code is not the operation's |
  | `MTPR_CTID` | PROTOCOL | a container whose transaction id is not the one in progress (the stale response of 7.5 step 5 excepted) |
  | `MTPR_DLEN` | PROTOCOL | a data phase whose length differs from the size it must have (GetObject against ObjectInfo or the 64-bit size; more than the maximum asked) |
  | `MTPR_RPARAMS` | PROTOCOL | an OK response with fewer parameters than the operations table requires |
  | `MTPR_RVALUE` | PROTOCOL | an OK response's parameter that fails its check (SendObjectInfo's storage, parent or handle; GetPartialObject's count) |
  | `MTPR_DATATYPE` | PROTOCOL | a value of a datatype other than the one required (GetObjectPropValue), or unknown |
  | `MTPR_LIST` | PROTOCOL | a property list or handle array that fails a check other than the above (a sentinel handle where no fallback applies) |
  | `MTPR_STATUS` | trace only | a malformed Get Device Status (7.5 step 4: step 6, never returned) |
  | `MTPR_EVENT` | trace only | an event that fails its checks (a full refresh, never returned) |
  | `MTPR_ARG_VALUE` | ARG | a null pointer, a size or offset out of range, an object not of the device |
  | `MTPR_ARG_CONTEXT` | ARG | a call that "Callbacks" does not allow on this thread or in this callback |
  | `MTPR_ARG_UNLISTED` | ARG | an operation the device's DeviceInfo does not list (GetPartialObject64 without 95C1h, CQ30, CQ31) |
  | `MTPR_ARG_EDIT` | ARG | an Android operation outside the edit's rules: a second BeginEditObject, another handle, another operation inside the edit, an EndEditObject with no edit (CQ30, CQ18) |
  | `MTPR_ARG_UNQUALIFIED` | ARG | 95C2h to 95C5h on a device that is not a simulator, without the test flag (CQ1) |
  | `MTPR_READBACK_SIZE`, `MTPR_READBACK_NAME`, `MTPR_READBACK_PARENT` | PROTOCOL | an upload's readback that completed and differs in that field from what was sent (Q25) |
  | `MTPR_DELETE_BOUND` | PROTOCOL | a folder's deletion meeting its depth or object bound (Q26) |
  | `MTPR_DELETE_HANDLE` | PROTOCOL | a folder's deletion meeting a handle it refuses (Q26) |

  The program's message for `MTPE_PROTOCOL` shows the reason's name
  (13's "(%s)"). New reasons are added to this table before code uses
  them.
- **Uncertainty covers every mutating operation**, with or without a
  data phase: SendObjectInfo (a new folder is one alone), SendObject,
  DeleteObject (each step of a folder's deletion), SetObjectPropValue
  (rename, M3), and Android's SendPartialObject, TruncateObject,
  BeginEditObject and EndEditObject. Such an operation is
  `MTPF_UNCERTAIN` once its command container may have reached the
  device (its write was submitted), unless a complete response was then
  read: an OK, or a refusal code, either of which settles it. A response
  lost to a time-out, a cancel, the power gate, a stall or a removal
  leaves it uncertain; so does a reading operation's lost response only
  in that the next transaction id moves on (nothing to settle). **No
  replay:** the library never sends an uncertain operation again on its
  own; the caller reads the device's state (the object's info, the
  folder's listing) and the user decides (below, "Uploads", "Android's
  writing operations").
- **Jobs.** The library owns each device's I/O thread (7.3). The program
  does not call a transfer itself: it queues a job, `MtpQueue(dev, run,
  drop, ctx, &id)`, from any thread, and the I/O thread calls `run(dev,
  ctx)` when the job reaches the head of the queue (below, "Standby": not
  while the device is held). Inside `run` the operations below are
  called, synchronously (below, "Callbacks", for exactly where). A job is
  one user operation, whole: a copy of a selection calls `MtpGetObject`
  once per file inside one `run`. Each job carries the session count at
  which it was queued (below, "Events"). The ownership of `ctx`, the
  threads `run` and `drop` run on, and cancel are in "Jobs and cancel",
  below.
- **Notices.** `MtpOpen` takes the program's `MTP_HOST`: a notice
  callback, called on the library's threads with a device, a notice code
  and a value, which only posts a message (it makes no library call and
  takes no lock of the library's): given up, gone, session lost, closed,
  draining began and ended (below, "Interface 1.1"); an event callback,
  run as a job on the I/O thread with the event's coalesced target
  (below, "Events"), from which the program refreshes what it shows;
  and the transport (`MTPIO_OPS`: the driver by default, `simdev.c` or
  `hostredir.c` in the host tests). The allocation and trace hooks are
  not per device: `MtpInit(hooks)` sets them once for the process,
  before the first `MtpOpen` or `MtpPowerStart`, and they stay valid
  until the process ends (a device's last free may come after its
  CLOSED notice, below).
- Devices: `MtpEnum` (info handles on `USBMTP0` to `USBMTP15`, `GET_INFO`
  of 52 bytes only; returns n, generation, VID, PID,
  bcdDevice, interface and class, speed, flags, the interface version,
  and, from a 1.1 driver, the power gate's reasons, what is held, the
  mask at the last refusal and the two counts: below, "Interface 1.1";
  any thread, the program's scan thread in practice; a device whose
  reading is 40 to 51 bytes is listed too, with the fields it returned,
  the missing ones 0, the bytes returned, and "cannot be opened" set for
  40 to 43 bytes, which `MtpOpen` refuses: CQ17; a reading that fails or
  is shorter than 40 bytes leaves that n out); `MtpNames(n,
  generation)` (strings 1 to 3, once per generation, admitted through
  the power registry: below, "The registry"); `MtpOpen(n, generation,
  host, power, &dev)` (blocking; any thread but the window's; registers
  the device with `power` before its first driver call); `MtpClose` (any
  thread; returns at once); `MtpQueue`; `MtpPower` (the power thread's:
  a standby query, which it answers, a failed query, a suspend and a
  resume: below, "Standby: the program refuses during a transfer");
  `MtpPowerStart`, `MtpPowerStop` (`mtppower.c`'s power thread and
  registry, over an `MTP_POWER` the program creates: no hidden global
  state).
- On the I/O thread, inside a job:
- Information: `MtpDeviceInfo`, `MtpStorageIds`, `MtpStorageInfo`.
- Objects: `MtpChildren` (a folder's children through GetObjectPropList
  with 7.4's fallback; the whole validated list is returned at once, not
  child by child: below), `MtpObjectHandles`, `MtpObjectInfo`,
  `MtpObjectSize64`.
- Transfers: `MtpGetObject` (streams to a sink callback, with progress),
  `MtpGetPartial` (Android's GetPartialObject64 for any offset, low ones
  included, whenever the device lists 95C1h; otherwise GetPartialObject
  when the device lists 101Bh and the offset and the maximum each fit in
  32 bits; otherwise
  `MTPE_ARG`, `MTPR_ARG_UNLISTED`, nothing sent: CQ31),
  `MtpSendObject` (SendObjectInfo, then SendObject from a source
  callback, as one reserved pair: below; returns the new handle),
  `MtpNewFolder`, `MtpDelete` (the deletion rules below).
- Android: `MtpAndroidSendPartial`, `MtpAndroidTruncate`,
  `MtpAndroidBeginEdit`, `MtpAndroidEndEdit` (below; disabled on real
  devices until qualified).
- From any thread: `MtpCancel(dev, id)` (a queued or running job, 0 for
  whichever is running; makes no driver call: below, "Jobs and cancel");
  `MtpRetain`, `MtpRelease` (below, "Who keeps the device alive"). The earlier `MtpEventWait` is withdrawn: events
  reach the program only through the event callback, so no program
  thread competes with the library's event thread for `WAIT_EVENT`.

**Threads and the device's lifetime.**

- **Threads per device.** The I/O thread makes every transaction call
  and runs recovery (7.3, 7.5). The event thread makes only
  `WAIT_EVENT`. The cancel thread makes only the cancel's `ABORT`s. The
  window thread makes no driver call at all: `MtpCancel` marks the
  cancel and signals the cancel thread, so neither a repeated Cancel nor
  a recovery call on the control handle can block the window (M0: calls
  on one handle are serialised, so an `ABORT` from the window could wait
  behind a recovery's class request). The deadline observer is the
  cancel thread (below); the window keeps 7.5's timer only to close its
  progress window, not as the guard of any deadline.
- **Jobs and cancel.**
  - `MtpQueue` returning `MTPE_OK` (with a job id, nonzero and never
    reused for that device) means the job is accepted: exactly one of
    `run` and `drop` is then called, once, and after it returns the
    library touches `ctx` no more. Any other return (`MTPE_CLOSING`,
    `MTPE_GIVENUP`, `MTPE_ARG`, `MTPE_NOMEM`) leaves `ctx` the caller's:
    neither is called.
  - `run` runs on the I/O thread. `drop(ctx, code)` runs on the I/O
    thread too (a job of an old session, `MTPE_SESSION`; a job cancelled
    while queued, `MTPE_CANCELLED`; the close's step 1, `MTPE_CLOSING`),
    except for jobs still queued when the device goes GIVENUP, which the
    thread that made it GIVENUP drops (the deadline observer), with
    `MTPE_GIVENUP`. Neither ever runs on the thread of `MtpQueue` or
    `MtpClose`, nor with a library lock held.
  - The program learns that `run` finished from `run` itself: its last
    act is to post the program's own message, after which it touches
    `ctx` no more; the library calls nothing of the job after `run`
    returns.
  - **The cancel latch.** Each job has a cancelled flag, under the
    section. `MtpCancel(dev, id)`, for a queued job, marks it, and the I/O
    thread drops it when it reaches the head; for the running job, it
    sets the flag and, if the phase is COMMAND, DATA-IN or DATA-OUT,
    starts 7.5 as above. For an id that is neither (finished, unknown)
    it does nothing, so a cancel aimed at job N never reaches job N+1:
    the hold that publishes `run`'s end and the dequeue of the next job
    each change which job is running. **The flag stops new work; it does
    not cut short work already admitted.** It is read at the start of
    every operation (in the same hold as the job's session tag: a
    changed session returns `MTPE_SESSION`, or `MTPE_CANCELLED` when the
    flag is set too, by the order of codes: 7.3, CQ4),
    and by the submission handshake's step 1 before a
    call that would begin a new transaction (the first write of a
    command container), in the same hold: so a cancel that comes in the
    IDLE phase, between two files or two transactions, is not lost; the
    next operation, or the next transaction of the current one, returns
    `MTPE_CANCELLED` without submitting. Inside a transaction already
    admitted, the flag acts by phase as 7.5 does: in COMMAND, DATA-IN or
    DATA-OUT the phase becomes CANCELLING and 7.5 runs from step 2 (the
    settlement; if every byte had moved, the transaction is in RESPONSE
    after all); in RESPONSE, and for the read after a data phase, the
    response is read to its end and the transaction completes, then the
    operation returns with its outcome and the next is refused.
    Recovery's own calls (7.5 steps 3 to 6) and the stale response read
    of step 5 are always made: they finish or recover admitted work. The
    only new transactions the flag lets through are two cleanups, each
    under its own admission conditions, which respect HELD, a lost
    session and GIVENUP: an Android edit's cleanup EndEditObject ("Edit
    lifetime", below) and an upload's cleanup DeleteObject of its
    reserved handle ("Uploads", below; Q24). A
    cancel that comes after the job's last operation has finished
    changes nothing that was done. An event's CancelTransaction cancels
    the transaction, not the job.
  - **The cancel work carries its identity.** A transaction's cancel is
    recorded with its job id and transaction id. The cancel thread,
    under the section before each `ABORT`, checks that this work is
    still the current one, and the deadline it arms carries the same
    identity; the hold that publishes the settlement retires it. The
    next job is not dequeued while any cancel work is outstanding (the
    cancel thread has not yet published its outcome), so no `ABORT` and
    no deadline action of job N can act during job N+1.
- **Callbacks: which thread, and what each may call.** No callback is
  ever called with a library lock held.

  | Callback | Thread | May call |
  |---|---|---|
  | `run`, and the event callback (run as a job) | the I/O thread | the operations, `MtpQueue`, `MtpCancel`, `MtpClose`, `MtpRetain`, `MtpRelease` |
  | a sink, source or progress callback | the I/O thread, inside an operation | `MtpCancel` only |
  | `drop` | the I/O thread, or the deadline observer | nothing of the library |
  | the notice callback | any library thread | nothing of the library; it posts a message |
  | the trace and allocation hooks | any thread | nothing of the library; they must not block |

  It is enforced, not only stated, with three markers. `run` and the
  event callback are the **job entry contexts**: the I/O thread sets
  "in job" around each. "In operation" is set while an operation runs.
  A thread-local "in nested callback" is set around every other
  callback and hook in the table (sink, source, progress, `drop`,
  notice, trace, allocation), and never around `run` or the event
  callback. An operation is accepted only on the I/O thread with "in
  job" set and neither "in operation" nor "in nested callback"; the
  other calls are checked against the table by the same markers. Any
  call the table does not allow returns `MTPE_ARG` at once, and a
  recursive operation (one called from a sink, say) is one of them.
- **The deadline observer** is the cancel thread. Deadlines are armed
  under the section: a cancel's and an automatic recovery's (7.5: 10 s,
  then 10 s for its reset, within an outer bound of 20 s from the
  procedure's start: CQ19), the open's (`OPEN_DEADLINE_MS`, 40,000 ms,
  for its transactions with their retries and a reset), and the close's
  (`CLOSE_DEADLINE_MS`, 30,000 ms, armed by `MtpClose` itself, so a job
  still running at the close request counts against it). While
  one is armed the cancel thread wakes every 500 ms; while it watches it
  makes no driver call (its `ABORT`s are only the cancel's, each bounded
  by its own wait). At a deadline it sets GIVENUP, drops the queued
  jobs and posts GIVENUP; the stuck thread finds GIVENUP when its call
  returns. So automatic recovery, opening, closing and the command-line
  mode are bounded the same way as a Cancel pressed in the window, with
  no window involved. `MtpOpen` makes its calls that take no slot
  (opening the handles, `GET_INFO`, `CLAIM`, `BIND`, `SET_TIMEOUTS`) on
  its caller's thread, then starts the threads and lets the I/O thread
  run the open's transactions as the first job, and waits for it at
  most the open's deadline and 1 s more: past that it returns
  `MTPE_GIVENUP` and no device, the device being retained, attached and
  Busy, until its stuck call returns, then cleaned up (its CLOSED notice
  carries the cookie of a device the program never had).
- **States**, under the device's critical section: OPENING, OPEN,
  RECOVERING, CLOSING, CLOSED, GIVENUP.
- **What the library keeps per device**, every field under that one
  critical section, which no thread holds across a driver call, a
  callback or a wait:

  | Field | What | Set and read by |
  |---|---|---|
  | state | OPENING to GIVENUP, above | every thread's entry check |
  | phase | IDLE, COMMAND, DATA-OUT, DATA-IN, RESPONSE, CANCELLING (7.5) | the I/O thread; the cancel |
  | in call; the published result | the handshake below: set before a call, cleared with its Win32 code and returned count after it | the I/O thread; the cancel thread |
  | the cancel work | its identity (job id, transaction id), the last `ABORT` mask, and whether *in call* cleared | the cancel thread; the I/O thread's settlement and dequeue |
  | `Busy`, HELD, the hold's tick | below, "Standby" | the I/O thread's dequeue; `MtpPower` |
  | the job queue | jobs with their session count; the refresh targets queued (at most 64, below, "Events") | `MtpQueue`; the event thread; the I/O thread |
  | the session | open or not, its count (raised at every new session), the next transaction id (7.3) | the I/O thread |
  | the device's form | the packet sizes and endpoint addresses from `GET_INFO`, the split form learned from GetDeviceInfo (7.2), the operations it lists, the refusals remembered for the session (7.4) | the I/O thread |
  | the upload reservation | storage, parent and handle of an OK SendObjectInfo, with its session count and cache generation (below, "Uploads") | the I/O thread |
  | the Android edit | the handle of an open BeginEditObject, if any | the I/O thread |
  | the 1.1 reading | whether the driver has the power fields; the last gate reasons and held mask; the refusal and forced-set counts at the running job's start (its window's baseline); "take the session as lost when the gate opens" | the I/O thread (below, "Interface 1.1") |
  | references | the callers inside the library (below) | every entry and exit |
- **Callers are counted.** Every library call of a device enters by
  taking an entry reference and, unless it is `MtpCancel`, `MtpPower`,
  `MtpClose`, `MtpRetain` or `MtpRelease`, by checking the state under
  the critical section: in CLOSING, CLOSED or GIVENUP it returns
  `MTPE_CLOSING` or `MTPE_GIVENUP` at once and submits nothing. It
  leaves by dropping that reference. **`MtpRelease` works in every
  state**, CLOSED and GIVENUP included, as long as its caller owns the
  reference it gives back: it drops that reference under the section
  and, if it was the last, frees the device outside the locks; it never
  touches the device afterwards. `MtpRetain` works in every state too,
  but only for a caller that already owns a reference (it copies one,
  so the device cannot be freed while it runs). The
  `MTP_DEV` and everything it owns (handles, buffers, events) are freed
  only by the release of the last reference, so a late return of a stuck
  call never touches freed memory.
- **Who keeps the device alive.** One count, under the section, of
  these references and no others:
  - **the program's owning reference**, given by a successful `MtpOpen`
    and consumed by `MtpClose`. After calling `MtpClose` the program does
    not pass the pointer again; a program thread that may call on the
    device while another calls `MtpClose` (the window's Cancel, the
    command-line mode's timer) takes its own reference first
    (`MtpRetain`) and drops it after (`MtpRelease`);
  - **entry references**, above, held only for the length of one call;
  - **one per library thread** of the device (I/O, event, cancel), taken
    when the thread is created and dropped as its last act, after which
    it touches nothing of the device and returns. Thread stacks are the
    system's (`CreateThread`) and end with their threads; the thread
    handles are closed by whoever frees the `MTP_DEV` (closing the
    handle of a thread still returning is allowed), so no thread frees
    its own stack;
  - **one per queued job**, taken by `MtpQueue` and dropped after its
    `run` or `drop` has returned;
  - **the power registry's** attachment reference and the power
    thread's snapshot references, each added and dropped under the
    device's section in the lock order of "The registry" (below).

  Notices carry no reference. Each carries the program's own cookie for
  the device (given in `MTP_HOST`) and is looked up in the program's own
  table. CLOSED is a device's last notice, posted after the library's
  last callback for it.
- **What the program passes, and how long it must live.** `MTP_HOST`,
  its cookie, its callbacks and their contexts: from `MtpOpen` until the
  program has received CLOSED, the device's last callback (for a device
  given up, that is when its cleanup has run, which may be never: then
  until the process ends). CLOSED may come before the `MTP_DEV` is
  freed (a snapshot or retained reference may still be out); that free
  uses only the process-wide hooks, never `MTP_HOST`. A job's `ctx`:
  until its `run` or `drop` has returned. An `MTP_RESULT`, and a sink's
  or source's buffer: for the length of the call. The allocation and
  trace hooks: until the process ends (`MtpInit`, above).
- **Open is all or nothing.** `MtpOpen` first registers the device with
  the program's `MTP_POWER` (below, "The registry"), then records each
  resource as it gets it: the owner handle, `GET_INFO` (major version 1, else refused; the
  generation must be `MtpEnum`'s, the endpoint addresses and packet sizes
  kept), `CLAIM`, the control and event handles and their `BIND`s,
  `SET_TIMEOUTS`, the three threads, GetDeviceInfo with transaction id 0
  (which also learns the split form, 7.2), OpenSession and 7.3's
  recovery. A failure at any step undoes the steps done, in reverse, and
  returns no `MTP_DEV`; the caller sees a device only when every step
  succeeded. A `CLAIM` refused as busy (170: the previous claim still
  releasing, 5.1) is retried 10 times, 100 ms apart (the first `CLAIM`
  and ten retries, 11 in all: CQ6), then reported as
  `MTPE_INUSE`. A bind's failure releases the claim with the owner
  handle's close. A registration that waits for a hold does so within
  the open's one deadline of 40,000 ms from the call, which also covers
  the transactions after it: no fresh budget follows the wait (CQ3,
  answered by the waiters table below and kept). **The code of each
  failed step** (CQ2), the undo the same for all:

  | Step that failed | Code |
  |---|---|
  | the owner handle does not open; a reading with the removal flag; a generation other than `MtpEnum`'s | `MTPE_GONE` |
  | `CLAIM` refused 32, or 170 after its 11 tries | `MTPE_INUSE` |
  | `GET_INFO` failing, or a failed reading (below 40 bytes, `cbSize` below the bytes returned, a major version not 1), or 40 to 43 bytes | `MTPE_DEVICE`, response code 0, the Win32 code kept |
  | `CLAIM` refused otherwise; the control or event handle not opening; a `BIND` or `SET_TIMEOUTS` failing | `MTPE_DEVICE`, response code 0, the Win32 code kept |
  | an allocation, an event or thread not created | `MTPE_NOMEM` |
  | GetDeviceInfo or OpenSession refused (after 7.3's retries and recovery) | `MTPE_DEVICE` with the response code |
  | a transaction's time-out | `MTPE_TIMEOUT` |
  | DeviceInfo malformed | `MTPE_PROTOCOL` with its reason |
  | a transaction stopped (995 not the library's, or 21) | by "Classifying a stopped call", the open's reading after `CLAIM` as the baseline |
  | the open's deadline | `MTPE_GIVENUP` (below) |
  | a hold that outlasts the deadline | `MTPE_HELD`, nothing sent |
- **Close** (`MtpClose`, which returns at once; the program learns of the
  end by the CLOSED notice, and its window never waits in `CloseHandle`).
  `MtpClose`, on the caller's thread and in one hold of the section, sets
  CLOSING, latches the cancel of the running job (below, "Jobs and
  cancel"), **arms the close's deadline from this moment**, and puts the
  close at the head of the queue; it then drops the program's owning
  reference and returns. **The I/O thread executes the close**, as soon
  as the running job's `run` has returned: the latched cancel stops new
  work and settles the transaction in flight (below), so `run` returns
  promptly unless a driver call of it is stuck, and then the deadline,
  already running, gives the device up (below, "The deadline observer")
  without the close ever starting. The close is taken from the queue
  whether or not the device is held (held, it sends no transaction:
  step 5). The I/O thread is the close's only executor and the device's
  only cleanup owner:
  1. The queued jobs are dropped (each `drop` on the I/O thread, with
     `MTPE_CLOSING`, a job cancelled while queued included: CQ15); new
     calls are refused.
  2. No job is running by now (`run` has returned, and the latched
     cancel has settled its transaction by 7.5 or let its response be
     read whole). Any cancel work still outstanding is waited for, until
     the cancel thread has published its outcome, before anything else
     is sent.
  3. The event thread is told to stop: it issues no further `WAIT_EVENT`,
     and its current one ends by its own time-out (1,000 ms); the close
     joins it. `EVENTS` stop ends the interrupt poll, not a `WAIT_EVENT`
     (5.3), so it is not what ends that thread.
  4. The close waits for the **entry references** only: calls by other
     threads that are inside the library on this device (an `MtpQueue`,
     an `MtpCancel`), each of which returns without waiting on the close.
     It never waits for the thread, job, registry or owning references,
     which can be released only after the close.
  5. CloseSession, one try within its normal time-out, if a session is
     open, the device is not given up, and it is not held: a held device
     sends no transaction, and its session is left open on the device.
     That is not without cost: the next OpenSession may meet
     SessionAlreadyOpen and need 7.3's recovery (CloseSession and
     OpenSession, then a Device Reset), and if that fails the device is
     shown as needing to be unplugged and plugged in again; the program
     does not promise that reopening is transparent. Steps 6 and 7
     send no transaction either (`EVENTS` stop and `ABORT` only cancel).
  6. `EVENTS` stop and `ABORT` of both bulk pipes on the control handle,
     each with a wait of 1,000 ms, and their outputs read: the close goes
     on only if both report quiet (`EVENTS` 1: stopped and its URB
     finished; `ABORT` 0: no pipe still busy).
  7. In this order, each outside every lock unless named: the cancel
     thread, which watched the close's deadline, is told to end and is
     joined (it waits on nothing of the I/O thread's); the event, control
     and owner handles are closed; the detachment sequence of "The
     registry" runs (registry lock, then the section: CLOSED and
     DETACHED set, the device unlinked; both released); the CLOSED
     notice is posted; the attachment reference is dropped; and the I/O
     thread drops its own reference as its last access to the device
     and returns. Nobody joins the I/O thread: dropping its reference
     and returning is enough, and closing its thread handle, done by
     whoever frees the `MTP_DEV`, does not free its running stack.

  **GIVENUP.** If the deadline passes (at any point from the close
  request on), a call is still pending: closing its handle would block
  (M0). The observer sets GIVENUP, drops the queued jobs and posts the
  GIVENUP notice. If instead step 6 finds an output not quiet, the I/O
  thread, which is not stuck, sets GIVENUP and posts the notice itself
  and goes straight to the quiescence loop below. The handles, buffers and
  `MTP_DEV` are kept: a bounded, intentional retention of one device's
  resources until that call returns, recorded as such and not as a leak
  (12). **The cleanup still has one owner, the I/O thread**, the only
  thread whose calls can stay out without bound (the event thread's
  `WAIT_EVENT` ends by the driver's own time-out, 5.3; the cancel
  thread's `ABORT`s end by their wait argument). When its stuck call
  returns, the I/O thread finds GIVENUP and requires **quiescence**
  before closing anything: the event thread ended (told to stop and
  joined: within its 1,000 ms wait), the cancel thread ended (joined),
  no call of its own out, and then `EVENTS` stop and `ABORT` of both
  bulk pipes reporting quiet (the interrupt URB and the bulk slots
  retired). If not yet quiet, it reads them again every second, the
  device staying GIVENUP and retained, until they are quiet or the
  process ends. Once quiet, it runs step 7 from its handle closing on.
- **Removal frees nothing of a pending call.** The device's removal (5
  or 1167 on other calls, or Windows' notice) changes no reference: the
  buffers of a call still pending in the driver, and the `MTP_DEV` its
  thread will touch on return, stay until that call has returned.
- **Program exit.** The program hides its window, stops starting opens
  and name reads, calls `MtpClose` on every device it has, and waits for
  their CLOSED notices for at most `EXIT_WAIT_MS` (5,000 ms) in all,
  dispatching messages meanwhile; the power thread keeps answering until
  then, so a standby during the closes is still refused. **5 s is the
  graceful-wait budget, not a bound on when the process has ended.** At
  its end any of these may still be outstanding, and each keeps its
  context until the process ends; nothing is freed on the budget's
  expiry:
  - a device still CLOSING (its close not yet through) or GIVENUP: its
    `MTP_DEV`, handles and threads stay as they are;
  - an open still in progress on one of the program's worker threads
    (OPENING, or held and waiting): the device it registered stays
    attached; `MtpOpen`'s caller is the program's worker thread, which
    is never waited for;
  - a name read still out: its context and its reference on the
    `MTP_POWER`.

  Then `MtpPowerStop` stops the power thread's answers and ends that
  thread; it frees the registry only if nothing is attached and no name
  read is out, else leaves it allocated. Then the process ends
  (`ExitProcess`), and the system's process termination replaces
  graceful cleanup for everything still outstanding. A stuck call keeps
  its request in the driver's slot, whose caller IRP may keep the
  process from ending fully until the stack returns it (5.4; M0's End
  Task readings, 5.6); standby meanwhile is the driver's backstop (Q18,
  decided: the driver's refusal stays for when the program is not
  running). The command-line mode does the same before it returns its
  exit code.
- **The exit sequence and the first-sight warning's set stay the
  program's** (CQ16, answered so and kept): the library does not carry
  them. So that the host test can drive them, the program keeps both in
  one source file with no window code (its scan, warning set and exit
  steps as plain functions over the library's calls and a posted-message
  hook), which `MTPEXP.EXE` and `hosttest.c` both compile; the host test
  supplies the message hook and the fake clock, and its `exit` and
  `scan` steps call those functions.
- `MtpCancel` and `MtpClose` may come at any time, from any thread, and
  together: a cancel in CLOSING or later does nothing; a second
  `MtpClose` does nothing; neither waits for the other.

**The three handles.** M0 read that NTKERN serialises the calls on one
handle (2026-11-16-mtp-m0.md, "Calls on ONE handle"). A `WAIT_EVENT`
pending on the control handle would therefore hold back an `ABORT` on it.
So `MtpOpen` opens:

- the owner handle: `CLAIM`, then `SET_TIMEOUTS`, reads, writes and
  `WRITE_ZLP`, from the I/O thread only;
- a control handle, bound: `ABORT`, `CLASS_REQUEST`, `RESET_PIPE`,
  `EVENTS`, used by one thread at a time: the cancel thread for a
  cancel's `ABORT`s, then, once it has reported (below), the I/O thread
  for recovery, and the I/O thread for the close's and the GIVENUP
  cleanup's quiet checks, made only after the cancel work has been
  published (close, step 2) or the cancel thread joined (cleanup);
- an event handle, bound: `WAIT_EVENT` only, from the event thread.

5.1 allows any number of bound handles and one pending `WAIT_EVENT`.
This refines 5.1's "a bound control handle"; the driver is unchanged.
The three handles solve the serialisation of `WAIT_EVENT`, and only that;
the rules below solve the races.

**Submitting a call, and cancelling it.** Every call of the I/O thread
that moves data or a container follows one handshake, under the device's
critical section:

1. **Before the call:** if the phase is CANCELLING, or the state is
   CLOSED or GIVENUP (OPENING, RECOVERING and CLOSING make transactions
   of their own: the open's, recovery's stale response read, the
   close's CloseSession), or the call would begin a new transaction while the job's
   cancel flag is set ("Jobs and cancel"), the call is not made; a call
   inside a transaction in RESPONSE is made whatever the flag says, and
   so are an Android edit's cleanup EndEditObject and an upload's cleanup
   DeleteObject, each under its own admission conditions ("Edit
   lifetime"; "Uploads and deletion").
   Otherwise the thread sets *in call* and
   leaves the section. *In call* is set before the call can reach the
   driver, so a cancel never misses a call about to be submitted.
2. **After the call:** it takes the section again, clears *in call*, and
   publishes the call's result: the Win32 code and the byte count the
   call returned, never the count it asked for. When the published
   results complete the data phase (step 6: every byte, and the
   `WRITE_ZLP` where one is due), the phase becomes RESPONSE in this same
   hold, so a cancel that comes after it finds RESPONSE. The read after
   an IN data phase that is a whole number of packets (the zero-length
   packet, "Framing") is made in RESPONSE.
3. **The cancel** (`MtpCancel`, or an event's CancelTransaction routed as
   below): under the section, a phase of COMMAND, DATA-IN or DATA-OUT
   becomes CANCELLING (IDLE and RESPONSE as 7.5 step 1); a phase already
   CANCELLING or a state RECOVERING is left alone, so a repeated cancel
   does nothing. Then, only when this step made the phase CANCELLING,
   the cancel thread is signalled (CQ20): in IDLE and RESPONSE it is not,
   no `ABORT` is issued, and the response is not disturbed; the flag
   alone acts, at the next admission.
4. **The cancel thread** issues `ABORT` of both bulk pipes (wait 1,000
   ms). If *in call* is still set when it returns, it issues `ABORT` again
   every 100 ms until *in call* clears or 7.5's deadline passes, so a call
   that reached the driver after an earlier `ABORT` is caught too. It then
   publishes its outcome: the last `ABORT`'s mask, and whether *in call*
   cleared.
5. **Settlement:** recovery (7.5 steps 2 to 6, on the I/O thread) starts
   only when both have been published: the I/O thread's last result, and
   the cancel thread's outcome. Only then does the I/O thread use the
   control handle. A mask not 0, or *in call* not cleared by the
   deadline, is 7.5 step 7.
6. **"Every byte moved"** (7.5 step 2's race) is decided from published
   results only: the data phase is complete when the successful calls'
   returned counts add up to every byte of the container (7.2) and, where
   7.2 requires one, a `WRITE_ZLP` returned success. A failed or cancelled
   call contributes nothing, whatever length it asked for.

**Framing** (interface 1.1 changes nothing here). 7.2's rules, with these made exact:

- **Small packets.** A bulk packet may be as small as 8 bytes (USB 2.0
  full speed; the driver and `simdev.c` allow it), so 7.2's "every bulk
  `wMaxPacketSize` here is more than 12" does not hold, and a first read
  of one packet may hold only part of the header. The library therefore
  accumulates: while fewer than 12 bytes of a container have arrived and
  the last packet was full, it reads one more packet. The split form is
  then told as 7.2 says, in packet terms: the read that completes the
  12-byte header ends on a short packet (4 bytes at 8-byte packets, 12 at
  16 and more) and the length field is more than 12; in the combined form
  that packet is full and carries payload. A response is accumulated the
  same way to its length field before it is checked: at 16-byte packets a
  response of up to 32 bytes spans more than the first read.
- **Reads sized to the container.** After the header, each read is what
  is left of the container, rounded up to whole packets, at most 1 MiB;
  so no read runs into the response.
- **The response read** is the smallest whole number of packets longer
  than 32 bytes (the largest response is 32), so a response that fills
  its packets exactly is ended by the zero-length packet after it, and
  nothing is left in the pipe. This also applies to a response that
  arrives where a data phase was expected (7.2) and to a stale response
  read and dropped after a cancel (7.5 step 5): each is read whole, by the
  same rule, before the next transaction. A full-speed device with packets
  of 32 bytes or less that omits that zero-length packet ends the read by
  its time-out (a `simdev.c` case). That read failed, so what it may have
  received is never used (a failed call contributes nothing, as the
  handshake's step 6 says): the call is `MTPE_TIMEOUT`, and the lost
  response is 7.5 step 1's stuck device (CQ13, CQ35).
- **The read after a data phase.** When the data phase's count (7.2's
  zero-length rule) is a whole number of packets, one more read, of the
  response's size above, follows the last data read: 0 bytes is the
  zero-length packet, and the response read comes next; data is the
  response itself, kept. When the count is not a whole number of packets
  the response read follows directly.
- **Data out.** Bytes from the source callback are gathered into the
  write buffer, 1 MiB (the same bound as the reads), so that every write
  but the last of a run is 1 MiB, a whole number of packets: in the
  combined form the first write carries the 12-byte header and 1,048,564
  bytes of payload; in the split form the header is a write of its own
  and each data write carries 1 MiB of payload. A short intermediate
  write would end the run early
  on the device's side. The last write may be short; `WRITE_ZLP` follows
  a run that is a whole number of packets (7.2). A source that delivers
  fewer bytes than ObjectInfo announced, or fails, ends the run: the
  transaction is cancelled (7.5) and the upload is uncertain (below);
  the call returns `MTPE_SOURCE` with `MTPF_UNCERTAIN` (CQ25).

**Errors from the driver**, by the Win32 codes M0 read:

- 995 is the cancel's (or the abort's) end; 121 a time-out; 1167 the
  device gone; 32 another program's claim.
- 21 (`STATUS_DEVICE_NOT_READY`) is a closed gate: the power gate (a
  standby, read on all three configurations in
  2026-11-16-mtp-m1-load.md: a pending read ended 995 by the gate's
  stop, the next refused 21; with a transfer running, the transfer's
  next call refused 21) or the started gate (before START, at
  QUERY_STOP, STOP or REMOVE). It does not say the device is gone, nor
  that a standby happened. The library takes the transaction and the
  session as lost (7.3) and retries only after the gate reopens, which
  it learns from `GET_INFO` (below, "Interface 1.1"). After
  a standby, on 98 SE, the device's removal follows (5 on the old
  handles); on ME the device is kept and its session was read lost.
- **5, and the replacement check.** 5 is a revoked claim or a deleted
  device. The library opens a new info handle and reads `GET_INFO`: if
  the open fails, the removal flag is set, or the generation differs from
  the one `MtpOpen` claimed (a device replugged under the same n is a
  replacement, 5.2), the original device is gone: `MTPE_GONE`. With the
  same generation and no removal flag, the claim was revoked. Either way
  the old handles are finished: the session and every cache of it are
  invalid, and the device is used again only through new handles and a
  new `MtpOpen` (new claim, new binds), never by binding the old handles
  again. A revoked claim returns `MTPE_INUSE` with `MTPF_SESSION_LOST`
  (CQ14); the library does not reopen the device itself, and only a new
  `MtpOpen` (once the other program lets the claim go) brings it back.
- **A bulk failure that is none of these** (the driver passes on the
  lower IRP's status, or `STATUS_UNSUCCESSFUL`, 5.4: a stall is not told
  apart from other failures on interface 1.0). The library does not
  assume a stall: it enters 7.5 at step 2 (settle), then reads Get Device
  Status before any Cancel Request. TransactionCancelled with endpoints
  runs the device-initiated case; OK or Device Busy runs the
  host-initiated case from step 3; anything else, step 6. The Win32 code
  each stack gives for a stalled bulk transfer is read in the legs (11.5)
  and recorded; nothing depends on it.
- **`ABORT` and `EVENTS` succeed with an output.** Their success says
  only that the call ran; quiet is `ABORT`'s mask of pipes still busy
  being 0, and `EVENTS` stop's output being 1 (5.3). Every caller reads
  the output; a pipe not quiet is 7.5 step 7.
- **Standby when idle.** With no call pending, a standby fails nothing
  the library would see, and after it the interrupt poll stays stopped
  (5.4, "Power"), so events stop without an error. The program forwards
  `WM_POWERBROADCAST` to `MtpPower` (below, "Standby: the program refuses
  during a transfer"); on a resume the library takes the session as lost
  (7.3), opens a new one and calls `EVENTS` start. A call failing with
  the power gate's code does the same. That 98 SE and ME deliver
  `WM_POWERBROADCAST` to a hidden top-level window was read with a probe
  program (the load record, "2026-11-17, follow-up"); that they deliver
  it to the program's own is read in its standby legs, not assumed.

**Interface 1.1: what the library reads and what it does with it** (5.3;
5.4, "What user mode can read").

- **The reading.** `GET_INFO` is asked for 52 bytes; the driver returns
  as much of its structure as was asked (the smaller of the two), and
  `cbSize` is its own size. A reading is used only when the call
  succeeded and returned at least 40 bytes, `cbSize` is at least the
  bytes returned, and the major version is 1; anything else is a failed
  reading (at `MtpOpen`, the open fails; later, the gate is treated as
  unknown and the call is retried after 250 ms, within the deadline). A
  field past the bytes actually returned is never read, whatever
  `cbSize` says. Then by size:
  - **40 to 43 bytes**: refused at `MtpOpen` (`MTPE_DEVICE`, traced):
    40 is the original 1.0 form, without the endpoint addresses, and 41
    to 43 an incomplete extension; 7.5 checks Get Device Status's
    endpoint parameters against those addresses, and the library does
    not guess them. The addresses and packet sizes are taken at
    `MtpOpen` only. After the open, a reading shorter than 44 bytes, or
    shorter than the open's reading, is a failed reading (the gate
    unknown, retried after 250 ms within the deadline); a later reading
    never replaces the addresses;
  - **44 to 51 bytes** (1.0 with the endpoint extension, or a short
    answer): the addresses (offsets 40 to 42) are used; the power fields
    are absent: everything below that needs them is off, the gate is
    learned only from failed calls (21) and timed retries, and the trace
    says so once;
  - **52 bytes or more, with `cbSize` at least 52 and the minor version
    at least 1**: the power fields as well: the gate's reasons (offset
    44), the held mask (45), the mask at the last refused query (46),
    the refused queries (48, UINT16) and the sets passed down with
    requests out (50, UINT16). 52 bytes from a driver reporting minor 0
    are read as 44.
- **When it is read.** `GET_INFO` takes no slot and answers while the
  gate is closed (5.4). The I/O thread reads it on the owner handle,
  between its own calls (calls on one handle are serialised, and the I/O
  thread is the owner handle's only user): at `MtpOpen` before and right
  after `CLAIM`; as each job starts (its window's baseline, below);
  after a call fails 21; after a
  call ends 995 that the library did not cancel; every 250 ms while it
  waits for the gate to open; and as the first step of the resume job.
  The scan thread reads it on its own info handles every 2 s (below,
  "Standby").
- **The gate's reasons**, as the I/O thread acts on them. Several may
  stand at once; they are taken in this order, and the first that
  applies decides: draining (8), with or without others, is the
  quarantine and nothing reopens the device while it stands; then any
  of 1, 4, 2 and 16 means waiting; only when none stands is the gate
  open. Every power reason seen, at any point of the wait, owes the
  session as lost.
  - none: open; the job goes on, or the new session is opened if one is
    owed;
  - 1 (a system sleep set) or 4 (a standby prepared or accepted): a
    standby is under way; the session is owed as lost when the gate
    opens; no call is made meanwhile. An accepted query that is followed
    by nothing lapses after `QUERY_HOLD_MS` (30,000 ms), and the driver
    applies the lapse in `GET_INFO`'s own hold, so the library's 250 ms
    reading is itself what reopens a device whose standby never came;
  - 2 or 16 (another component's device power change or query; ME, 5.4):
    the same, the session owed as lost (on ME the device answered Session
    Not Open after such a change: the load record);
  - 8, draining, with the held mask: **the quarantine.** Requests the
    gate stopped have not come back; the driver admits nothing until
    they do, and REMOVE waits for them (5.4). The library makes no call,
    posts the notice "draining began" (the program shows 13's draining
    string), and reads every second until the bit clears; then "draining
    ended", the session is lost, and a new one is opened. If it never
    clears, the device stays so until it is removed; the program never
    says that unplugging will help (13). While any device drains, the
    program also refuses a standby (below, "Standby"): the driver would
    find those requests out at its query.
- **What the counts say, and what they do not.** The refused-query
  count covers the driver's refusals of system queries **and** of
  another component's device queries (5.4); the forced-set count, sets
  passed down with requests out. A change says that this driver, for
  this generation, refused a query (or passed a set down with requests
  out) at some time between the two readings. It does not say which kind
  of query, and it does not say that a particular stopped call was
  stopped by it.
- **Classifying a stopped call, in a window.** Each job has an
  observation window: the I/O thread reads `GET_INFO` as the job starts
  (its baseline, which also consumes any change that came before) and
  again right after a call of the job ends 995 without the library's
  cancel, or fails 21. What that establishes is kept apart from what it
  does not: "the driver refused a query (or forced a set) during this
  job" is one fact; "this call was stopped by a power change" is a
  stronger claim, and no reading proves it. The call is classified so:
  - **a power reason (1, 2, 4, 16, or draining 8) in the reading right
    after the stop**: the power gate was closed when the call was found
    stopped. That is a coincidence in time, the only evidence the
    driver's interface gives, and the result says no more:
    `MTPE_POWER`, "stopped while the driver's power gate was closed",
    with `MTPF_STANDBY` when that reading showed 1 or 4;
  - **no power reason then, but a count changed within the window**:
    `MTPE_STOPPED`, with `MTPF_POWER_SEEN` ("this driver refused a query
    or forced a set during this job; whether that stopped this call is
    not known"): a device query can change the count early in a long job
    and be over before an unrelated stop;
  - **21 with no power reason**: the started gate (5.4): `MTPE_GONE` if
    the reading shows removal pending, else `MTPE_STOPPED`;
  - **neither, and the next call fails 5**: the release or removal path
    (above, "5, and the replacement check"). The stopped call does not
    wait for that next call (CQ29): it returns `MTPE_STOPPED` from the
    evidence it has, and the 5, when a later call meets it, is
    classified then (`MTPE_GONE` and the GONE notice, or a revoked
    claim) for that call and the device;
  - **neither otherwise**: the conservative fallback, `MTPE_STOPPED`;
  - **a driver without the power fields**: always `MTPE_STOPPED` (21
    cannot be told from the started gate, nor 995 from anything else),
    even when its 44-byte reading's flags show removal pending (CQ7,
    answered so and kept); a later call failing 5 or 1167 still finds
    the device gone.

  In every case the session is taken as lost and the uncertainty rule
  of the results applies (`MTPF_UNCERTAIN` for a mutating operation). A
  change that came before the job's baseline is never taken as
  evidence for a later stop. The mask at the last refusal goes to the
  trace.
- **Telling the user** that the driver refused is the scan thread's
  (below, "Standby"), so the message comes once, for claimed and
  unclaimed devices alike; the library's counts serve only its own
  classification.
- **Recovery under a closed gate.** If a step of 7.5 fails 21, or ends
  995 without the library's cancel, the recovery stops there: no Device
  Reset is sent into a gate that may be closed. The result is
  classified by the rule above, from a reading right after that step,
  like any stopped call (`MTPE_POWER` only with a power reason in it,
  else `MTPE_STOPPED` or `MTPE_GONE`); the session is owed as lost, and
  after the gate opens 7.3's OpenSession,
  with its own failure handling (CloseSession and OpenSession, then
  Device Reset), brings the device back. An `ABORT` or `EVENTS` stop acts
  without a slot (5.1), so `MtpClose`'s quiet checks still answer while
  the gate is closed.

**Standby: the program refuses during a transfer** (Q18, decided by the
owner on 2026-11-17: option (c), with the program's own refusal as the
normal path; design only, nothing of it written).

- **Why.** A standby that reaches the driver's query while a transfer
  runs interrupts the transfer (the gate stops every slot, 5.4), and if
  the stack keeps a stopped request past `QUIET_QUERY_MS` the driver
  refuses the query, which on 98 SE cost the serial ports in every guest
  tested (5.4). A program is asked earlier: 98 SE sends programs
  `PBT_APMQUERYSUSPEND` first, and a `BROADCAST_QUERY_DENY` ends the
  standby there, with the same message box, before `PBT_APMSUSPEND` and
  before any power IRP reaches a driver; the serial ports kept working
  after it (four guests on 98 SE, the load record's follow-up). So the
  program refuses a standby itself while it has a transfer active, and
  the driver's refusal stays as the backstop (below). A program's
  refusal on ME was not read; a leg reads it (11.5).
- **"A transfer active"** is the device's `Busy`, kept by the library
  under the device's critical section and true while any of these holds:
  - a job of the I/O thread is running: from the hold in which the I/O
    thread takes it from the queue, before its first submission (the
    submission handshake's step 1 runs inside it), to the hold in which
    it publishes its result. A job is one user operation, whole: a copy
    of a selection (every file of it), the staging of a drag (8.3), a
    folder's deletion, the upload pair, a listing with its fallback, a
    refresh. Whether standby is refused for the whole operation or only
    while an operation is in flight was the owner's choice: the whole
    job (owner, 2026-11-17; Q19 (a)), as recommended, since a standby agreed between two
    files would end the operation anyway, its session being lost at the
    resume (7.3). The other answer is designed too, and differs only
    here:
    - **operation-level protection.** `Busy` is set in the hold in which
      a library operation (`MtpGetObject`, `MtpChildren`, ...) begins its
      first submission and cleared in the hold in which that operation
      publishes its result; the upload pair (SendObjectInfo with its
      SendObject) and an Android edit (BeginEditObject to
      EndEditObject) each count as one operation, so a hold never
      splits them. The admission boundary is each operation's start: it
      reads HELD in the hold that would set `Busy`, and if the device is
      held the operation sends nothing and returns `MTPE_HELD` (with no
      uncertainty: nothing of it was sent). The job is then
      interrupted: `run` sees the code and ends; what was completed
      stays (files already copied are whole, 7.8; the file not begun is
      not); the job's result is `MTPE_HELD` with the count done ("%d of
      %d files copied", 13); after the resume the session is lost and
      nothing is resumed or replayed on its own: the user gives the
      command again. OPENING, RECOVERING, CLOSING and a stuck GIVENUP
      call stay `Busy` as below;
  - the state is OPENING (`MtpOpen`'s transactions), RECOVERING (7.5
    steps 2 to 6, whatever began them: a Cancel, a failed call, an
    event's CancelTransaction or DeviceReset) or CLOSING before its step
    7 (waiting for cancel work, CloseSession, the quiet checks), unless
    the device is held: a held device's close sends no transaction and
    only cancels (step 5 of "Close"), so it is not `Busy`;
  - the state is GIVENUP and its cleanup has not yet found the device
    quiet (a call of the library still pending in the driver, or
    `EVENTS` stop and `ABORT` not yet reporting quiet: "Close",
    "GIVENUP"): the driver would find that request out at its query and
    might refuse (5.4), so the program refuses first.

  Not a transfer: the event thread's `WAIT_EVENT`, the interrupt poll,
  and jobs queued but not started. The driver stops the first two at its
  query and, under the emulator, they came back within milliseconds in
  every accepted standby with a claim held (the load record).
- **The answer** (`MtpPower(dev, MTP_PWR_QUERY)`), in one hold of the
  section: `Busy` returns `MTP_PWR_BUSY`; otherwise the device becomes
  HELD and the call returns `MTP_PWR_AGREED`. While HELD no job starts:
  the I/O thread's dequeue reads HELD in the same hold in which it would
  set `Busy`, so either the job started first and the answer is BUSY, or
  the answer came first and the job waits in the queue. The program asks
  every device it has open: if any is busy, it ends the hold of every
  device that agreed, by the one routine of "Ending the hold" (below),
  and returns
  `BROADCAST_QUERY_DENY`; otherwise `TRUE`. It also refuses while the
  registry counts a name read out (below, "strings"), and while the
  scan thread's last reading of any device, claimed or not, shows the
  gate draining (above, "Interface 1.1"): the driver would refuse with
  those requests out. That reading is at most 2 s old, so a drain that
  begins after it is left to the driver's backstop. `lParam`'s bit that
  allows a program to ask the user is not used: the program never
  prompts here. `mtppower.c` holds this answer over its registry of
  devices (below); the scan thread reports the draining devices to it;
  the power thread asks the devices under their own sections.
- **The registry, and I/O outside jobs.**
  - **Registration comes first.** `MtpOpen` takes the program's
    `MTP_POWER` (the program and `MTPTEST` always pass one) and
    registers the new `MTP_DEV` before its first driver call. It first
    allocates and initialises the device, unpublished, outside every
    lock (the allocation hook is a callback, and no callback runs under
    a library lock): state OPENING, its lock and events created, its
    count holding the caller's reference and the attachment reference
    the registry will own. Then it takes the registry lock, and the new
    device's section in the lock order, links the device in and reads
    the registry's power state in that same hold; nothing is allocated
    or called there. If a query is being answered or a hold stands, the device
    is inserted HELD and not yet OPENING-busy, and the open makes no
    driver call until the hold ends: it waits on the registry's "no
    hold" event ("Ending the hold", below), within the open's own
    deadline, else `MtpOpen` returns `MTPE_HELD` and nothing was sent. So an opening device is always one the power
    thread can see: either Busy (OPENING) or held.
  - **The lock order** (the one rule for the two kinds of lock): the
    registry lock may be held while a device's section is taken, never
    the other way round. A thread holding a device's section never takes
    the registry lock; a thread holding the registry lock takes one
    device's section at a time and releases it before the next. Neither
    is held across a driver call, a callback, a wait or a free. Every
    reference count is changed under its device's section; a release
    that brings it to zero frees the `MTP_DEV` after both locks are
    released (the device is by then detached: the attachment reference
    is the registry's, below, and is dropped only after detachment).
  - **The answer over the registry.** The power thread takes the
    registry lock; marks a query in progress; for each attached device,
    takes its section, adds a snapshot reference, releases the section;
    and releases the registry lock. It then asks each device under its
    own section alone (`MtpPower`). It takes the registry lock again and
    records the outcome there (below, "Ending the hold"): all agreed, the
    hold stands; one refused, it ends; and it clears the mark. Last, it
    drops each snapshot reference under that device's section, outside
    the registry lock. A device that was detached after the snapshot
    shows DETACHED in its section and is skipped. A registration that
    meets the mark waits as for a hold, and proceeds if the answer was a
    refusal.
  - **Detachment**, wherever it happens (a failed open, step 7 of the
    close, a GIVENUP device's cleanup), is one sequence: take the
    registry lock; take the device's section; set DETACHED (and CLOSED,
    at a close); unlink it from the registry; release the section;
    release the registry lock. Then, outside both, the CLOSED notice is
    posted, and the attachment reference is dropped under the section
    alone. No notice or callback is made inside either lock.
  - **A failed open** undoes its steps in reverse; it is detached from
    the registry only after the last driver call of the undo has
    returned (the owner handle closed, the threads ended), and only then
    is the attachment reference dropped.
  - **Detachment at close** is part of step 7 of "Close" (below), after
    the handles are closed, when no protected work can remain; CLOSED is
    set in the same sequence. A device that went GIVENUP stays attached,
    and Busy, until its cleanup has found it quiet and run step 7; then
    it is detached.
  - **I/O that is not a job** is admitted by the same rules or takes no
    slot:
    - the session reopened after a resume, a quarantine's end or a
      DeviceReset event, and `EVENTS` start after it, run as an internal
      job (the resume job) through the same dequeue, so HELD defers them
      and Busy covers them;
    - the cancel thread's `ABORT`s belong to a running job (Busy);
    - the I/O thread's 250 ms `GET_INFO` readings and the scan thread's
      take no slot (5.4: answered under the device lock, inline);
    - the event thread's `WAIT_EVENT` is not a transfer and is stopped by
      the driver at its query, as above;
    - **strings** (`GET_STRING` goes through the driver's control slot,
      5.3) are not read by the scan thread at all. `MtpNames(n,
      generation)` reads strings 1 to 3 once per generation, on a name
      thread of the program, and caches them. It is admitted through
      the registry: under the registry lock it is not started while a
      query is in progress or a hold stands, and otherwise it raises the
      registry's count of name reads out, in the same hold. **While that
      count is above zero the answer refuses, until the read has
      actually returned**, with no time limit: the request is in the
      driver's control slot, and the driver would find it out at its
      query and could refuse late, which is what the program's own
      refusal is there to prevent; a read that never returns is treated
      as a stuck GIVENUP call is (below), not given up on a timer. (The
      earlier 5 s limit, which dropped that protection while the request
      could still be out, is withdrawn; so no availability choice is
      added to Q23.) A read that has not returned within 5 s is traced,
      and no further name read is started for that generation. **What
      `MtpNames` returns** (CQ8): names already read for that generation
      come from the cache, `MTPE_OK`, with no driver call, whatever the
      hold; otherwise, while a query is in progress or a hold stands, or
      while an earlier read of that generation is still out, it returns
      `MTPE_HELD` at once, nothing started and no second read of a stuck
      generation, and the program asks again at its next scan. The read
      runs on its own info handle, in a small context of its own (the
      handle, the result buffer, a reference on the `MTP_POWER`); the
      registry is not freed while a name read is out (`MtpPowerStop`
      leaves it allocated, "Program exit"), and a stuck read's context
      lives until it returns or the process ends. A device the program
      opens takes its names from GetDeviceInfo instead (8.1), so name
      reads are made only for devices it cannot claim.
  - **Draining readings** are kept per n and generation with the tick of
    the scan that read them. One counts for the answer only while
    younger than `DRAIN_FRESH_MS` (6,000 ms, three scans); a stale one
    never refuses. A reading is evicted when its n is absent from a
    scan, when a new generation appears for its n, and when it goes
    stale. The scan thread calls only `GET_INFO`, which takes no slot,
    so nothing of its own can keep its readings from being refreshed;
    should it stop all the same, its readings go stale and ordinary
    standby is no longer refused because of them.
- **The hold's two phases.** QUERY-HELD runs from the agreement to
  `PBT_APMSUSPEND`; SUSPENDING from `PBT_APMSUSPEND` to a resume notice.
  Each keeps the tick of the broadcast that set or last refreshed it:
  `PBT_APMQUERYSUSPEND` answered with an agreement sets QUERY-HELD (a
  repeated one refreshes it); `PBT_APMSUSPEND` moves QUERY-HELD to
  SUSPENDING (and, arriving with no hold, a suspend nobody asked about,
  by the second sequence of "Entering the hold", below,
  sets SUSPENDING); no other broadcast (power status, battery, OEM
  events) touches the hold or its tick. Elapsed time is always
  `(DWORD)(GetTickCount() - Tick)`, unsigned, so it stays right across
  the tick count's wrap at about 49.7 days; no two ticks are compared as
  absolute values. Whether 98 SE's and ME's tick count advances during a
  sleep is not read, and nothing below depends on it: a sleep ends with
  a resume notice, which ends the hold.
- **Entering the hold: two sequences, both on the power thread**, which
  handles one broadcast at a time (its window procedure runs each to
  its end), so neither overlaps an answer in progress:
  - **an agreement**: the answer of "The answer over the registry" (the
    registry hold that marks the query resets "no hold"; each device's
    HELD is set by `MtpPower` in its own section; the outcome recorded
    as QUERY-HELD with its tick);
  - **`PBT_APMSUSPEND` with no hold standing** (a suspend nobody asked
    the program about): take the registry lock; reset "no hold"; set the
    state to SUSPENDING with the broadcast's tick; for each attached
    device, take its section, set HELD, release it; release the
    registry lock. A device with a job running keeps running it: HELD
    stops new dequeues, it cannot make that job's traffic disappear (a
    suspend cannot be refused). The job's outstanding and remaining
    transactions stay subject to the driver's query, or to its set
    without a query, and to Q18(c)'s backstop (5.4; "A forced
    transition", below): stopped there, the job ends as any stopped
    call does ("Classifying a stopped call"), and on 98 SE a refusal
    there carries the serial-port risk. Nothing new starts on any
    device. A
    registration or a name read that looks after this waits as for any
    hold. With QUERY-HELD standing, `PBT_APMSUSPEND` only moves the
    state to SUSPENDING and takes the new tick, under the registry lock.
- **The hold ends** at `MTP_PWR_FAILED` (`PBT_APMQUERYSUSPENDFAILED`:
  this or another program refused; it ends either phase), at
  `MTP_PWR_RESUME` (`PBT_APMRESUMESUSPEND`, `PBT_APMRESUMEAUTOMATIC` or
  `PBT_APMRESUMECRITICAL`, whichever comes first; the others do nothing),
  and otherwise **by rule (A), held until an explicit recovery action
  (owner, 2026-11-17; Q23)**; rule (B) below is the alternative that was
  weighed. Closing a held
  device does not end the hold; it sends no transaction while the hold
  stands (below, "Close", step 5). A missing failure or resume notice does not show that
  the transition ended, and a finite time-out is no evidence of it: the
  driver's gate is open both before its query and after its end, so its
  readings cannot tell the two apart either. The two rules, each
  designed to be implemented:
  - **(A) Closed until an explicit recovery action** (recommended). The
    hold has no lapse. Past `POWER_HOLD_NOTICE_MS` (10,000 ms) in either
    phase, the program's status line says that it is waiting for Windows
    to finish a power change, with a button to use the device now (13,
    drafts). The hold is the process's, so the button's scope is every
    device the program has: pressing it is the recovery action, and it
    ends the hold by the one routine below. Commands given meanwhile stay
    queued and run after it. In the command-line mode (8.4) there is no
    user: a command whose job is still held at `/powerwait` (default
    60 s) ends not done, with its own exit code; the mode then closes
    the device, and that close sends no transaction while the hold
    stands (step 5 of "Close"), so nothing new reaches the device (a
    job already running at an unsolicited suspend is the exception of
    "Entering the hold"; the held command itself never started). If
    Windows does go on after the action, the driver's query meets
    whatever was started, and Q18(c) applies.
  - **(B) Time-out reopening, accepted as a heuristic.** QUERY-HELD
    lapses `POWER_QUERY_HOLD_MS` (10,000 ms) after its tick, SUSPENDING
    `POWER_SUSPEND_HOLD_MS` (30,000 ms) after its tick. The power thread
    keeps a 500 ms timer while a hold stands and ends a lapsed hold by
    the one routine below (the dequeue does not judge the lapse itself);
    the trace logs `power-hold-lapsed` with the phase.
    A job started after a lapse can still meet the driver's query if
    Windows was only slow (another program delaying the query sequence,
    or a suspend whose driver queries had not reached this device): it
    is then interrupted, and if the stack keeps a stopped request the
    driver refuses, with Q18(c)'s cost on 98 SE, the serial ports, as in
    the tested guests (5.4). The lapse is a choice of availability over
    that protection, not a safety property.

  **Ending the hold: one routine, on the power thread.** The hold's
  state (none, a query in progress, QUERY-HELD, SUSPENDING; its tick)
  lives in the registry; each attached device carries HELD, a copy for
  its own dequeue. Every end of the hold (a failed query, a refusal in
  the answer, a resume notice, (A)'s button, (B)'s lapse) runs one
  routine on the power thread: the button posts a message to the power
  thread's window, the lapse is the power thread's own timer. The
  routine takes the registry lock, sets the state to none, and for
  each attached device takes its section and, in that one hold: **on a
  resume, first invalidates the old session** (the session count is
  raised, so every job queued under the old count is dropped at its
  dequeue with `MTPE_SESSION`, and the resume job is put at the head of
  the queue), and only then clears HELD and signals that device's I/O
  thread (its queue's event, so a waiting dequeue looks again); then it
  releases the section. It then sets the registry's manual-reset event
  "no hold" and releases the registry lock. So no dequeue can see HELD
  clear while a job of the old session is still taken as current. Starting
  an answer resets that event: the power thread resets it in the same
  registry hold in which it marks a query in progress, before any
  device is asked or any HELD set, so a registration or a name read
  that looks after that waits. The waiters and what wakes them:

  | Waiter | Waits for | Bounded by |
  |---|---|---|
  | a job at the head of a held device's queue | its I/O thread's queue event, signalled by the routine | (A): nothing but the action; (B): the lapse |
  | an open inserted held (no I/O thread yet) | the registry's "no hold" event; then, under the registry lock and its own section, it rereads the state and, if no query or hold stands, clears its HELD and becomes OPENING (`Busy`) in that hold, else waits again | the open's deadline: past it, `MTPE_HELD`, nothing sent |
  | a name read not yet started (below) | the next scan after "no hold" | - |
  | the command-line mode's held command | its job's end | `/powerwait` |

  **What a hold keeps from the device, under either rule while it
  stands:** no new job, no open, no name read (none starts during a
  hold, and one still out when a query comes makes the answer a
  refusal, below), no CloseSession; a close's `EVENTS` stop and `ABORT`
  only cancel. **The device is quiet at the driver's query only after
  a successful agreement**: the program agreed because nothing was
  `Busy`, so nothing of its own is out. A hold entered by an
  unsolicited `PBT_APMSUSPEND` ("Entering the hold") promises less: it
  stops new dequeues, but a job already running when the suspend came
  goes on until the driver's query or set stops it, under Q18(c)'s
  backstop. What no hold covers: such a running job; another client of
  the driver; the
  program after its exit (past the graceful budget of "Program exit",
  the process ends and the driver's refusal is the backstop, Q18); and
  a request already stuck in the driver, while which the answer refuses
  instead of agreeing (below).

  With either rule, after a resume the session is taken as lost (7.3):
  the jobs of the old session are dropped by their tag, and a user
  command queued during the hold ends as not done, with the program's
  message (13), rather than being run on handles of a session that no
  longer exists. Everything else in this design is the same under (A)
  and (B); what differs is this bullet, the status line and its button
  (A only), `/powerwait` (A) or the two lapse constants (B), and the
  tests that say so (the races table).
- **No new transfer while the hold stands**: that is HELD, above. Under
  (A) that covers every agreement until a failure, a resume or the
  user's action; under (B) it stops at a lapse. A job already running
  at an unsolicited suspend is not new and is not stopped by the hold
  ("Entering the hold"). Nothing
  else is cancelled at the agreement or at `PBT_APMSUSPEND`: after an
  agreement no job is running, so there is nothing to cancel, and the
  driver's own query stops
  the event wait and the poll. An early cancel at `PBT_APMSUSPEND` of a
  job the program did not know to refuse (a lapse, a suspend with no
  query) is not made: its class requests could not complete under the
  driver's closing gate, and the 198 to 442 ms between the broadcast
  and the driver's query (the load record) is an observation, not a
  window the design may count on.
- **Which thread answers.** The program's power thread: started before
  any device is opened and ended after the last is closed, it owns one
  hidden top-level window (the probe's hidden top-level window received
  every broadcast on 98 SE and ME: the load record) and runs its own
  message loop. It makes no driver call, no file or registry call, and
  waits on no other thread; it takes the registry lock for the snapshot,
  the outcome, the draining readings and the routine that ends the hold,
  and each device's section in the lock order of "The registry", each
  for a short hold with nothing called inside. No thread holds a device's section across a
  driver call or a wait (the submission handshake above; the host test's
  fake transport fails a call entered with the section held). So the
  answer waits neither on the window thread (a common dialog, OLE's drag
  loop of 8.3 or 8.3's drop-time filling, a 5,000-row list being drawn)
  nor on a stuck driver call. The window thread learns of power events
  by messages the power thread posts. The command-line mode (8.4) starts
  the same thread, so the legs driven by `MTPEXP /c` refuse as the
  window does. How long 98 SE and ME wait for a program's answer is not
  read; nothing depends on it, since the answer takes only the holds.
- **The driver's refusal stays as the backstop** (5.4, unchanged) for
  what the program cannot cover: another client of the driver (`MTPTEST`,
  a second copy of a program, a program not built on the library); the
  program not running, or exiting with requests still in the driver (a
  close that went GIVENUP, 7.9 "Close"); requests a closed or ended
  client left in the stack; and a request the stack keeps after the
  program agreed. An idle program agrees, and then the driver alone
  decides.
- **After an identifiable driver refusal.** The scan thread (the 2 s
  poll of 8.2, made a thread of its own: it opens info handles on
  `\\.\USBMTP0` to `15` and calls `GET_INFO` only)
  keeps, per n and generation, the refused-query count of interface 1.1
  (5.3) as its baseline, taken at first sight and again after each
  report. When the power thread posts a failed query or a resume, the
  scan thread reads every device at once. A count that differs from its
  baseline says that this driver refused a query, system or device, for
  that generation since the baseline (another driver's or a program's
  refusal leaves it unchanged); Windows' own box cannot say which
  component refused. Paired with the failed query or resume the power
  thread has just seen, it is the program's evidence that the driver
  refused that standby; the message (13) says "a power change", since
  the count cannot tell a system query from a device query. The program
  then shows the refusal message of 13: on 98 SE (`GetVersionEx`, 4.10)
  with the sentence that a restart is needed before a serial port is
  used, on ME (4.90) without it (owner, 2026-11-17; Q19 (c)). Not
  visible this way: a refusal before the device was plugged in again (a
  new device object starts at 0), and a driver without the power fields.
- **A count already nonzero at first sight** says only that this device
  object's driver refused some query since the object arrived. It does
  not show that the refusal happened while this program was absent: an
  earlier run of the program may have seen it and warned already, and
  on 98 SE it does show that no standby was accepted since (98 SE
  removes the device at an accepted one, 5.4), but on ME, which keeps
  the device, it does not. So it is grounds for a conservative warning
  (13's first-sight wording), on 98 SE only, and it is shown (owner,
  2026-11-17; Q19 (d)). Its scope: the program keeps, for its own
  run, the set of (n, generation, count) it has warned for, and warns
  again only for a new generation or a count that rose; a device that
  leaves the scans and comes back with the same generation (5.2: the
  generation never repeats within a boot) is not warned again in that
  run. The set is not kept across runs (the generation counter restarts
  at every boot, and the program has no reliable boot identity), so a
  new run of the program warns again while the count stays nonzero.
- **A forced transition** (a system set with no query: a critical
  suspend) asks no program, and the driver never fails a set (5.4): it
  drains within `QUIET_SET_MS` and passes the set down with what is
  still out quarantined. A job running then is interrupted, ends 995 or
  21, and is recovered after the resume with the session lost (7.3,
  7.5), a write's outcome uncertain (above). Neither layer can prevent
  it. No rig here produces one by documented means; not read.
- **ME.** The same order of broadcasts and driver query was read once on
  ME (the load record), so the program refuses the same way there: the
  refusal protects the transfer on both systems. After an identifiable
  driver refusal the message has no serial sentence on ME, by the one
  guest whose ports kept working, which does not show that ME is immune.
- **Races, and the test that shows each.** The host test drives the
  library's threads by a step hook at each hold, with `simdev.c` and the
  fake clock; the legs are rows of 11.5.

  | Race or case | The rule | Host test | Leg |
  |---|---|---|---|
  | a query as a job starts | HELD and `Busy` decided in one hold | the query at every step from the dequeue to the first submission's step 1; no submission after an agreement in any interleaving | a standby chosen as `MTPEXP /c get` starts, the simulator's bulk IN delayed: refused by the program (no `power` line in the driver's trace), or agreed with nothing of the program's out |
  | a query as a job ends | `Busy` clears in the hold that publishes the result | the query at every step from the last response to the publication | a standby just after a copy ends, repeated |
  | a query during recovery | RECOVERING is `Busy` | the query at each step of 7.5 | a standby during "Cancelling..." |
  | a query during open or close | OPENING and CLOSING are `Busy` (a held device's close is not: it sends no transaction) | the query at each step of `MtpOpen` and `MtpClose`, and a close requested during a hold | a standby as the program starts or a device is closed |
  | a query with a call stuck after GIVENUP | `Busy` | a fake transport call that never returns, then the query | not reachable under usb-redir (every cancel retired); the host test only |
  | several devices, one busy | all or nothing; the agreed ones released | two simulated devices | two devices, a copy on one |
  | a device registered while a query is answered or a hold stands; a failed open; a close and a GIVENUP device during an answer | registration and the answer in registry holds; the open waits; detachment only after the last call | the registration at every step of the answer; each failed step of `MtpOpen` with a query at each point of its undo; a detach during the snapshot | a standby as the program opens a device |
  | a name read when a query comes; a name read stuck | admitted through the registry; refuses until the read has returned | the query at each step of `MtpNames`; a fake `GET_STRING` that never returns, then queries at 1 s, 6 s and 60 s (each refused), then the read returning and the next query agreed; program exit with the read still out (its context and the registry kept) | not reachable under usb-redir; the host test only |
  | a draining reading gone stale; the device removed; a new generation on the same n | a stale or evicted reading never refuses | the scan stopped while a reading says draining, queries at 1 s and 7 s; the n gone; a new generation | not reachable (no rig keeps a request); the host test only |
  | an agreement, then `PBT_APMQUERYSUSPENDFAILED` | the hold ends; queued jobs run | the failure at every point of the hold | `MTPTEST pbwatch deny` refusing after the program agreed: the copy requested meanwhile then runs |
  | an agreement, then nothing | (A): held until the action; (B): the lapse | the fake clock past 10 s and past 30 s in each phase; (A) the action at every point of the hold; the tick across its wrap | not reachable (both systems always sent a suspend or a failure) |
  | **QUERY-HELD delayed:** an agreement, then no `PBT_APMSUSPEND` for more than 10 s (another program slow to answer the query), then the suspend and the driver's query | (A): no job started, nothing out at the query; (B): QUERY-HELD lapses 10 s after the agreement, a job started then is stopped by the query, and with the zero-bound build refused | `simdev.c`'s gate closing 12 s after the agreement with the suspend at 11.5 s, a job queued at 0 s, at 9 s and at 11 s | `MTPTEST pbwatch delay 12000` answering `PBT_APMQUERYSUSPEND` 12 s late after the program agreed (whether 98 SE and ME wait for a slow answer is read by this leg), a copy requested at once: (A) it waits; (B) it starts at 10 s and the driver's query meets it |
  | **SUSPENDING delayed:** `PBT_APMSUSPEND` received, after an agreement, or with no hold but no job running, then the driver's query more than 30 s later (another program slow to handle the suspend notice) | (A): held, no job started after the suspend, nothing of the program's out at the query (true here only because nothing was running: the next row is the case where something was); (B): SUSPENDING lapses 30 s **from the suspend broadcast**, a job started then is met by the query | the gate closing 35 s after the suspend, jobs queued at 0 s, 29 s and 31 s after it; the suspend with no hold standing and no job running (every device's HELD set, "no hold" reset) | `MTPTEST pbwatch suspend-delay 35000` returning from `PBT_APMSUSPEND` 35 s late (whether 98 SE and ME wait for it is read by this leg), a copy requested after the suspend: (A) it waits; (B) it starts 30 s after the suspend and the driver's query meets it |
  | **A job already running at an unsolicited suspend** (`PBT_APMSUSPEND` with no hold standing while a copy runs: the program was never asked) | HELD stops new dequeues only; the running job goes on until the driver's query, or a set without one, stops it; Q18(c)'s backstop applies. Expected under (A): no other job starts (queued ones wait for the action or the resume); the running job's call ends 995 or 21 and is classified by the reading after it (`MTPE_POWER` with a power reason seen, else `MTPE_STOPPED`), `MTPF_UNCERTAIN` if it was mutating; the driver accepts once its cancels return (production bounds) or refuses (the zero-bound build; on 98 SE, 5.4's serial-port result); after the resume the session is lost and nothing is replayed; the program never reports the device as having been quiet | `simdev.c`: the unsolicited suspend with a job in COMMAND, DATA-IN, DATA-OUT, RESPONSE and during recovery, then the gate closing with its 995 or 21, then the resume; a second job queued and not dequeued until the resume (or the action); the step hook at every point between the suspend sequence and the gate's closing | none by documented means: 98 SE and ME sent the query broadcast first in every standby read, and `pbwatch` cannot make Windows skip it; the host test only until a rig produces a suspend without a query (recorded as not run) |
  | a resume while jobs of the old session are queued | the session invalidated before HELD is cleared | the resume at every point of the routine; no old-session job's `run` called | the existing standby-with-explorer-open row |
  | the interval from the agreement to the driver's query (198 to 442 ms observed) | nothing depends on it | the suspend at 0 ms and after 60 s | the order of broadcasts and driver lines, recorded in every standby leg |
  | a suspend with a job running and no agreement (a lapse, a forced transition, another client) | the driver stops it; the job ends 995 or 21; recovery after the resume | the gate's 995 and 21 at each phase, then the resume | `MTPTEST xferout` transferring while `MTPEXP` idles: `MTPEXP` agrees, the driver stops the transfer and accepts |
  | a driver refusal | identified by the count | `GET_INFO` faked | the `standbytest` build, `MTPTEST xferout` transferring, `MTPEXP` idle: the driver refuses, `MTPEXP` shows the 98 SE message; on ME the message without the serial sentence |
  | the window thread blocked | the power thread answers | the window thread held by a test hook for 60 s while broadcasts are sent | a copy with a modal dialog of the program open: refused |
  | idle | agreed; the driver accepts | the query with no job | the existing row: standby with the explorer open, no copy |
  | the system's idle timer during a copy | a query like any other | - | not read; a leg if the guests' idle standby can be set |

  `MTPTEST` gains `pbwatch [deny] [delay <ms>] [suspend-delay <ms>]`: a
  hidden top-level window that logs every `WM_POWERBROADCAST` to the
  trace port and answers `TRUE`, or `BROADCAST_QUERY_DENY` to
  `PBT_APMQUERYSUSPEND`, after the delay given, and returns from
  `PBT_APMSUSPEND` after its own delay; the tracked successor of the
  scratch probe the load record used.

**Events.** The event thread loops on `WAIT_EVENT` with 1,000 ms, checks
each container by 7.2, and checks each recognised event's parameters by
MTP 1.1, G.2 (the count of parameters the length gives must be at least
the event's own):

| Event | Code | Parameters | Route |
|---|---|---|---|
| CancelTransaction | 4001h | none (the transaction id is the container's) | engine state, at once |
| ObjectAdded, ObjectRemoved, ObjectInfoChanged | 4002h, 4003h, 4007h | ObjectHandle | refresh job |
| StoreAdded, StoreRemoved, StorageInfoChanged | 4004h, 4005h, 400Ch | StorageID | refresh job |
| DeviceInfoChanged | 4008h | none | refresh job |
| DeviceReset | 400Bh | none | engine state, at once |
| ObjectPropChanged | C801h | ObjectHandle, ObjectPropCode | refresh job |

- **Routed to the engine, not queued.** CancelTransaction and DeviceReset
  must act while the I/O thread may be blocked in a transfer, so the
  event thread applies them to the device's state under the critical
  section: CancelTransaction with the id of the transaction in progress
  starts the device-initiated case (the cancel thread's `ABORT`s, then
  the I/O thread's recovery, as above); with another id it is logged and
  dropped (7.4). DeviceReset marks the session lost and cancels the
  transaction in progress, whose call returns `MTPE_CANCELLED` with
  `MTPF_SESSION_LOST` (CQ5).
- **Tagged with the session.** The library counts its sessions; every
  job carries the count at which it was made, and a job of an earlier
  session is dropped when it reaches the I/O thread (its `drop` is
  called with `MTPE_SESSION`: the object handles it names belong to a
  session that no longer exists). A refresh job runs the program's event
  callback with its target; after a new session the program is told the
  session was lost and reloads from the storages down (7.3).
- **Session-specific state is cleared at every new session**, in the
  hold that raises the session count: the refusals remembered (7.4's
  fallback, CQ38), the upload reservation, the Android edit, and every
  cached handle, storage id and listing. The upload reservation also
  carries the cache generation: it is raised in the hold in which the
  event thread asks for a full refresh (an event that fails a check, the
  lost flag, the 64-job cap passed), before that refresh job is queued
  or runs. A reservation whose session count or cache generation differs
  from the device's is no longer the library's to delete ("Uploads").
- **Bounded.** Refresh jobs are coalesced by what they refresh (a
  folder, the storages, a row, the device): a second job for the same
  target while one is queued is dropped. Past 64 queued refresh jobs, they
  are replaced by one full refresh. So draining the driver's queue of 16
  never builds an unbounded queue in the program.
- An event that fails a check, or a lost flag, asks for a full refresh;
  other events are ignored (7.4). After a new session the library calls
  `EVENTS` start (5.4, "Power").

**Reset:** 7.5 step 6, on the control handle, by the I/O thread.

**The operations of the first cut.** Each row is a contract: the command
parameters sent (their number exact), the data phase's direction, and
what an OK response must carry. A response other than OK may carry fewer
parameters, or none, and is never a protocol error for that reason; it
is returned as `MTPE_DEVICE` with its code, and its parameters are read
only where noted.

| Operation | Code | Command parameters | Data | OK response carries |
|---|---|---|---|---|
| GetDeviceInfo | 1001h | none; transaction id 0, before the session | in: DeviceInfo | nothing required |
| OpenSession | 1002h | 1: session id (1, 7.3) | none | nothing required; SessionAlreadyOpen (201Eh) may carry the open session's id, logged if present |
| CloseSession | 1003h | none | none | nothing required |
| GetStorageIDs | 1004h | none | in: storage id array | nothing required |
| GetStorageInfo | 1005h | 1: storage id | in: StorageInfo | nothing required |
| GetObjectHandles | 1007h | 3: storage id, format 0, parent (7.4) | in: handle array | nothing required |
| GetObjectInfo | 1008h | 1: handle | in: ObjectInfo | nothing required |
| GetObject | 1009h | 1: handle | in: the object; its length checked against ObjectInfo's size, or the 64-bit size (7.2) | nothing required |
| GetPartialObject | 101Bh | 3: handle, offset (32 bits), maximum bytes | in | 1: the bytes sent, which must equal the bytes that arrived, and be at most the maximum asked |
| SendObjectInfo | 100Ch | 2: storage id, parent | out: ObjectInfo | 3: storage id, parent, the new handle (below) |
| SendObject | 100Dh | none | out: the object | nothing required |
| DeleteObject | 100Bh | 2: handle, format 0 | none | nothing required; PartialDeletion (2012h) is handled below |
| GetObjectPropList | 9805h | 5: handle, format 0, property FFFFFFFFh, group 0, depth 1 (7.4) | in: property list | nothing required |
| GetObjectPropValue | 9803h | 2: handle, property code | in: the value, of the property's datatype | nothing required |
| Android GetPartialObject64 | 95C1h | 4: handle, offset low, offset high, maximum bytes | in | nothing required (libmtp reads none); the bytes that arrived must be at most the maximum asked |
| Android SendPartialObject | 95C2h | 4: handle, offset low, offset high, length | out: exactly `length` bytes, split form always (below) | nothing required |
| Android TruncateObject | 95C3h | 3: handle, offset low, offset high | none | nothing required |
| Android BeginEditObject, EndEditObject | 95C4h, 95C5h | 1: handle | none | nothing required |

- Every offset and length is checked in 64-bit arithmetic before it is
  sent: `offset + length` within the object's known size where one is
  known, the maximum asked within the caller's buffer.
- The Android parameters are facts read in libmtp 1.1.22's `src/ptp.h`
  and `src/ptp.c` (the code is not taken). Each Android operation is sent
  only when GetDeviceInfo lists it (the Fairphone 3 lists all five).
- Rename (SetObjectPropValue on DC07h) waits for M3, as section 12 has
  it; MoveObject is not planned (7.4).

**Android's writing operations** (95C2h to 95C5h):

- **Framing.** libmtp sends SendPartialObject's header as a packet of its
  own whatever the device's form, because, as its comment says, Android's
  responder writes data that arrives with the header packet at the wrong
  offset (a fact read in `src/ptp.c`). The library does the same: 95C2h
  is always sent in the split form, whatever 7.2 learned of the device.
  Advertising 95C2h is not taken as evidence that the operation is safe.
- **Edit lifetime.** BeginEditObject opens an edit of one object; only
  SendPartialObject and TruncateObject on that handle may follow, then
  EndEditObject. One edit at a time per device. After a cancel or a
  failure inside an edit, the object's contents are reported as unknown
  (`MTPF_UNCERTAIN`), and EndEditObject is sent once as **the edit's
  cleanup transaction**, the one exception to the cancel flag's refusal
  of new transactions ("Jobs and cancel"; the handshake's step 1 lets
  it through by name). It is admitted only when all of these hold, in
  the same hold as the handshake's step 1:
  - the edit's own operation is still running (the edit is one
    operation, so `Busy` stands; nothing else of the job follows it);
  - any interrupted transaction has settled and 7.5, if it ran, reached
    step 5 (ready) with no recovery step failed and no Device Reset; a
    cancel that came between two transactions, or in RESPONSE, starts no
    7.5 and so meets this condition at once;
  - the session is the one the edit began in (its count unchanged) and
    no power reason was seen since the cancel (a gate reason means the
    session is owed as lost);
  - the device is not HELD (an unasked suspend may have set it: "Entering
    the hold"), not GIVENUP and not CLOSED; CLOSING is allowed, so a
    close requested during an edit still ends it on the device.

  Otherwise it is not sent: the edit stays open on the device, which
  the result says, and what a device does with an edit left open across
  a lost session is not known (the object is read back before anything
  else is done with it). EndEditObject is itself mutating: if its own
  response is lost it is `MTPF_UNCERTAIN` and is not sent again. A lost
  session sends nothing.

  Made exact (CQ9, CQ18, CQ30): the cleanup is the library's, sent
  inside the interrupted call before it returns, never left to the job.
  The interrupted call's result carries `MTPF_UNCERTAIN` and, when no
  complete response to the cleanup was read (not admitted, or its
  response lost), `MTPF_EDIT_OPEN`. Either way the library's edit
  record is cleared before the call returns, so at most one cleanup
  reaches the wire: a later `MtpAndroidEndEdit` (or any edit operation
  on that handle) by the job returns `MTPE_ARG`, `MTPR_ARG_EDIT`,
  nothing sent (or `MTPE_SESSION` first if the session changed, or
  `MTPE_CANCELLED` if the cancel flag is set: both are read before the
  edit state). A second BeginEditObject, an operation on another handle
  or any other operation while an edit is open, and an EndEditObject
  with no edit open, return `MTPE_ARG` with `MTPR_ARG_EDIT` and send
  nothing; an operation the device does not list returns `MTPE_ARG` with
  `MTPR_ARG_UNLISTED`.
- **No replay.** A partial write or a truncate whose outcome is uncertain
  (a cancel or failure after the command was sent, a lost response) is
  `MTPF_UNCERTAIN` and is never sent again by the library; the caller
  reads the object back to learn its state.
- **Disabled on real devices** until qualified: the library refuses
  95C2h to 95C5h unless the device is a simulator (its strings say
  SIMULATED) or the program passes an explicit test flag; on real
  devices they stay disabled until a leg qualifies them (Q17, decided),
  and the write leg Q14 allows does not include them. GetPartialObject64
  (95C1h) only reads and is not covered by this rule; it is in the
  library only for now (Q17), not used by the program. Made exact (CQ1):
  the refusal is `MTPE_ARG` with `MTPR_ARG_UNQUALIFIED`, nothing sent;
  "its strings say SIMULATED" means GetDeviceInfo's Model string (MTP
  1.1, 5.1.1), as decoded at the open (or at a DeviceInfoChanged
  refresh), contains the nine UTF-16 units of `SIMULATED` (upper case,
  compared exactly). The USB string descriptors are not used: the
  library reads none for a device it opens. The test flag is a field of
  `MTP_HOST`, read at `MtpOpen`.
- **Tests.** In `simdev.c`, with an Android-like responder that puts
  header-packet data at offset 0 (the fault libmtp describes): a partial
  write at a nonzero offset into a larger object, then a check that the
  bytes before the offset and after the end are unchanged; a truncate;
  each interrupted at every phase.

**Uploads and deletion: what the library may remove.**

- **The upload pair is reserved.** SendObjectInfo and its SendObject run
  as one job: no other transaction comes between them, and a cancel or
  failure between them abandons the pair (the device holds a reservation
  the library does not reuse).
- **Ownership.** The only object the library may delete after a failed
  upload is the one named by a SendObjectInfo whose OK response carried
  three parameters that passed the checks (storage and parent equal to
  those sent, a handle not 0 and not FFFFFFFFh), in the same session (the
  session count above) and the same cache generation. A name match, a
  later listing or a size is never evidence that an object belongs to the
  upload. This replaces 7.8's last bullet ("deleted when the device lists
  it"). An OK response whose parameters fail those checks (too few, or
  not matching what was sent) is `MTPE_PROTOCOL` (`MTPR_RPARAMS` or
  `MTPR_RVALUE`) with no `MTPF_UNCERTAIN`: the complete OK settled the
  mutation (CQ33). It establishes no ownership, so nothing of it is ever
  deleted, and SendObject is not sent.
- **A failure after SendObjectInfo's OK and before SendObject's data
  phase began** (CQ10; Q24, decided by state):
  - **(i) SendObject's command never submitted** (a cancel or failure
    between the pair, a source that fails before any byte, the device
    held or its session lost before SendObject's admission) and **(iii)
    SendObject refused by a complete response before any data**: the
    library deletes the reserved handle once, by DeleteObject with that
    handle, as **the upload's cleanup transaction**. It is admitted in
    the same hold as the handshake's step 1, only when all of these
    hold: the upload's own operation is still running; any interrupted
    transaction has settled and 7.5, if it ran, reached step 5 with no
    Device Reset; the session count and the cache generation are those
    of the reservation and no power reason was seen since; the device
    is not HELD, GIVENUP or CLOSED (CLOSING allowed). It is the one
    further exception to the cancel flag's refusal, let through by name.
    An OK leaves nothing on the device, and the result is the failure's
    code without `MTPF_UNCERTAIN`. A cleanup not admitted, refused or
    whose response is lost leaves the reservation: the result then
    carries `MTPF_UNCERTAIN`, and the DeleteObject is never sent again
    (a lost one is itself uncertain).
  - **(ii) SendObject's command submitted, its data not yet** (a cancel
    or failure in COMMAND, the command's write ending without its count
    published): the command may have reached the device, so the pair is
    abandoned: nothing more is sent, and the result is the failure's
    code with `MTPF_UNCERTAIN` and the reserved handle, for the program
    to show.
  - The alternative that was weighed (abandoning in every state, or
    deleting in state (ii) too) is not used.
- **Uncertain completion.** If SendObject fails or is cancelled after its
  data phase began, or its response is lost, the outcome is
  `MTPF_UNCERTAIN`: the library neither replays the write nor deletes on
  its own; the program shows the object as possibly partial and the user
  decides. Only states (i) and (iii) above let the library delete the
  reserved handle (DeleteObject with that handle, once).
- **Byte accounting.** The bytes sent are the published results' sum
  (the handshake above); a source callback that delivers fewer bytes than
  ObjectInfo announced, or fails, makes the upload fail, as above.
- **Readback.** After an OK SendObject the library reads the new
  object's ObjectInfo (and its 64-bit size where 7.2 needs it) and checks
  the size, name and parent; a mismatch is reported. The legs also compare
  contents (11.5). The readback is a new transaction (CQ21): when the
  job's cancel flag is set by then, the latch refuses it, and the upload
  returns `MTPE_OK` (the complete OK of SendObject settled it), the new
  handle, its payload bytes and `MTPF_UNVERIFIED`; the job's next
  operation returns `MTPE_CANCELLED`. A readback whose own transaction
  fails returns that failure's code by the order of "One code, and
  flags", with `MTPF_UNVERIFIED`, the handle still returned, and no
  `MTPF_UNCERTAIN` for the upload (it was settled). A readback that
  completes and differs is a failed verification (Q25, decided (b)):
  `MTPE_PROTOCOL` with `MTPR_READBACK_SIZE`, `MTPR_READBACK_NAME` or
  `MTPR_READBACK_PARENT` (the first that differs, in that order), no
  `MTPF_UNCERTAIN` (the complete OK settled the upload), the new handle
  still returned; nothing is sent again and nothing is deleted. The
  alternative that was weighed, `MTPE_OK` with a mismatch flag, is not
  used. A readback whose failure also loses the session keeps
  `MTPF_SESSION_LOST` in the result; the handle returned belongs to the
  lost session and is not usable in the new one (7.3).
- **Deleting a folder** (7.4's rule, made exact):
  - A refusal is classified by its code: ObjectWriteProtected,
    StoreReadOnly, AccessDenied, StoreNotAvailable, InvalidObjectHandle
    and any other code end the deletion and are reported; only
    PartialDeletion (2012h) leads to deleting the children. A device that
    refuses a non-empty folder with another code has the deletion
    reported as refused; the legs record which code each device uses
    (this narrows 7.4's rule, under which any refusal led to deleting the
    children).
  - After PartialDeletion, the folder's children are listed again before
    anything else is deleted.
  - The traversal is bounded (depth 64, 100,000 objects per request),
    refuses a handle of 0 or FFFFFFFFh, a handle seen twice and a parent
    chain that loops, and deletes only handles listed in this session
    under the folder the user chose. Meeting a bound or a refused handle
    stops the whole deletion (Q26, decided (a)): each listing is checked
    whole before any object of it is deleted, and from the listing that
    fails on, nothing more is deleted; the call returns `MTPE_PROTOCOL`
    with `MTPR_DELETE_BOUND` (depth 64, 100,000 objects) or
    `MTPR_DELETE_HANDLE` (0, FFFFFFFFh, a handle seen twice, a loop, a
    handle not under the folder chosen), and `MtpDelete` returns the
    count of objects whose DeleteObject was answered OK before it. The
    alternative that was weighed, skipping that handle, is not used.
  - A child folder met inside a PartialDeletion is first tried as one
    DeleteObject, like the folder the user chose, and is descended into
    only on its own PartialDeletion (CQ36; 7.4).
  - **The deletion's own listing.** The children are listed with
    `MtpChildren`'s transaction (GetObjectPropList, or GetObjectHandles
    where the fallback is remembered). The queried folder's own
    elements, which 7.4 drops from every GetObjectPropList answer, are
    removed first; the deletion then checks the rest of that listing by
    its own rules, and they override the listing's
    per-object fallback and completeness check: a handle of 0 or
    FFFFFFFFh, a handle already seen in this deletion, a parent that is
    not the folder being emptied, or a parent chain that loops stops the
    deletion at once (no GetObjectInfo of that object, no GetObjectHandles
    comparison). Only a listing that passes is used, and its objects are
    deleted in the order the listing gives them. A listing the device
    refuses, or one malformed as a whole, ends the deletion with that
    error, reported as the listing's.

**Parsing: every length and count the device sends is checked.**

- Every parser is a pure function of a byte range and an output
  structure, reading only through `ptprd.c`. Each read checks first, in
  64-bit arithmetic, that the field fits in what is left.
- **Before a count is used:** the count times the element's minimum wire
  size must fit in what is left (a UINT32 array: 4 bytes each; a string:
  1 byte; a property list element: a handle, a property code, a datatype
  and a value of at least 1 byte, 9 bytes), and the count must be within
  its cap, before any allocation. The fit is checked first: a count that
  fails both is `MTPE_PROTOCOL` (`MTPR_COUNT`), and only a count that fits
  and is over its cap is `MTPE_TOOLARGE` (CQ39).
  Allocation sizes are narrowed to 32 bits by a checked conversion.
- **A decoding budget per call:** the bytes allocated for decoded
  results (objects, property values and arrays, copies, StorageIDs) are
  counted against a cap of 64 MiB per call, and the elements visited
  against a work cap (twice the element cap of 7.2); past either, the call
  returns `MTPE_TOOLARGE`. A dataset read with the sentinel length (7.2)
  is checked against the caps as it arrives, not after. Made exact, so
  that the host cases can name the element at which a call stops: each
  decoded handle or storage id charges 4 bytes; each object of a listing
  64 bytes, charged when its first element is met, before its value; a
  string 2 bytes per unit, its NUL included; an array value its element
  size times its count; a scalar value its size; an element skipped or
  dropped (an unknown property, the folder's own) nothing. The work
  counter adds 1 per element or handle visited and 1 per element of an
  array value, read or skipped. The caps are 67,108,864 bytes charged and
  8,388,608 visits; the call stops at the first charge or visit past
  either.
- **Strings:** a count of 0 to 255 UTF-16 units must fit; the whole
  counted string is consumed even when a NUL comes before its end, so the
  next field starts where the count says; the decoded copy has room for
  a terminator the library adds. A count above 0 whose last unit is not
  NUL is accepted and terminated by the library, and counted as a
  deviation in the trace (Q15). An unpaired surrogate decodes as U+FFFD,
  always the same way.
- **Datasets:** DeviceInfo, StorageInfo and ObjectInfo field by field in
  the order of MTP 1.1's datasets (5.1.1, 5.2.2, 5.3.1); bytes after the
  known fields are ignored (7.2).
- **Property lists** (MTP 1.1, 3.2: INT8 to UINT128, their arrays with a
  UINT32 count, and STR):
  - an unknown datatype stops the parse, and the result is discarded,
    not used as a partial listing: the folder falls back to
    GetObjectHandles and GetObjectInfo (7.4);
  - a property given twice for one object with two different values, a
    required property missing (StorageID, ObjectFormat, ObjectFileName,
    ParentObject), a handle of 0 or FFFFFFFFh, or a parent that is not the
    folder asked for: that object falls back to GetObjectInfo;
  - completeness: the handles of the list must equal those of
    GetObjectHandles for the folder when the list's count is suspect
    (fewer objects than a previous listing, or any fallback above); if
    they differ, GetObjectHandles' list is the folder's.
- **Nothing partly validated is published.** `MtpChildren` returns the
  folder's list only after every element has been checked; no callback
  sees an object before that, so nothing unvalidated can be chosen for a
  deletion.
- **Partial reads:** for GetPartialObject and GetPartialObject64, the
  bytes that arrived must be at most the maximum asked, `offset + length`
  is checked in 64-bit arithmetic, and GetPartialObject's response
  parameter must equal the bytes that arrived (the table above).
- A response's parameter count comes from its length (7.2); each
  operation's OK response needs the count of the table above, and fewer
  is a protocol error only for an OK response.
- An event's length is 12 + 4k, k 0 to 3 (7.2), and its parameters as in
  the events table; the event's own rule decides on its transaction id
  (7.4).

**The host test.**

- **`simdev.c`:** a simulated device behind `MTPIO_OPS`, in memory, at
  the packet level: the packet size (8 to 1,024), combined and split data
  phases, zero-length packets, stalls, the class requests' answers, Get
  Device Status sequences, events, and a fake clock for 7.5's deadlines.
  It carries the fault list of `mtpemu.py`'s `faults` profile (11.2), so
  the host test and the legs run the same cases.
- **Expected results written independently.** Each fixture's expected
  decoding is written by hand from the specification's tables, in a file
  of its own (`testdata/*.expect`), not produced by the library or by
  `simdev.c`, so the two cannot share a mistake unseen. The legs' own
  readings (the stacks' short and zero-length packet counts, which M0
  deferred) are not replaced by the host test.
- **Fixtures** (11.3): written by hand from the specifications' tables,
  with the section each comes from; and real devices' datasets, starting
  with the Fairphone 3's GetDeviceInfo (355 bytes) and two StorageInfos,
  captured by a trace build of the test program on the bench, the serial
  number replaced and the whole fixture checked for other personal data
  (names of storages, owner strings) before it is tracked; the fixture's
  source and capture are recorded with it; each such fixture's readable
  fields are shown to the owner before it is committed (Q16, decided).
- **Malformed datasets:** every fixture truncated at every byte; each
  count set to its cap, one past it, and to values whose product with the
  element size passes 2^32; string counts past the end; property lists
  with unknown and wrong datatypes, duplicates, missing required
  properties, sentinel handles and parent loops.
- **The parsers are fuzzable.** `hosttest.c` runs each parser over its
  fixtures and over a fixed number of deterministic mutations of them (a
  seeded generator: bit flips, byte values 00h, FFh and 80h, cuts and
  insertions). Each input is copied between two `PAGE_NOACCESS` pages
  (`VirtualAlloc`, `VirtualProtect` on the build host): once ending at the
  upper guard page and once starting at the lower one, so a read past
  either end faults at once, without a sanitiser, which MSVC 6.0 lacks.
  Outputs get canaries before and after, checked after each call. Every
  case must end in `MTPE_OK`, `MTPE_PROTOCOL`, `MTPE_TOOLARGE` or
  `MTPE_NOMEM`, never a fault, and within an execution budget (elements
  visited, checked by a counter, not by time).
- **Allocation:** the library allocates through a hook; the host test
  fails the nth allocation for every n of each case, and counts
  allocations and frees. Each call's own temporary allocations must be
  freed by its return. A device's allocations must balance only once
  every reference to it has been released: after CLOSED, a snapshot or
  retained reference (`MtpRetain`) may still hold it, and a GIVENUP
  device is retained until its cleanup has found it quiet (below, 12);
  the test releases those references, lets the cleanup run, and then
  requires the balance.
- **Semantic assertions:** for every fixture that decodes, the results
  equal the `.expect` file's; for every mutation that decodes OK, the
  results satisfy the invariants (counts within caps, strings
  terminated, handles neither 0 nor FFFFFFFFh in a listing).
- **The engine:** 11.4's list (framing in both forms and directions, ids,
  cancel in every phase, completion winning the race, each malformed
  status, the device-initiated case, both deadlines), and named cases:
  cancel before submission (the cancel between the phase check and the
  call), cancel during the final zero-length packet, `MtpClose` during a
  transfer and during recovery, `MtpCancel` and `MtpClose` together, a
  failed bind and every other failed step of `MtpOpen`, a revoked claim,
  a replacement device with a new generation, an event flood past the
  driver's 16 and the coalescing cap, CancelTransaction while a transfer
  blocks, late completions after given up, the response read with 8-,
  16-, 32- and 64-byte packets, and a source callback that underflows;
  the three-handle rule (a fake transport that refuses a second call on a
  handle with one pending).
- **Jobs and the power hold:** an operation called off the I/O thread
  (`MTPE_ARG`); `drop` called once for every job that does not run
  (close, given up, an old session) and `run` never after it; the races
  table of "Standby" above, each row at every step of the step hook; the
  fake transport failing any call entered with the device's section
  held; `WM_POWERBROADCAST` sent to `mtppower.c`'s hidden window on the
  build host with a device busy, idle, held, draining, and with the
  window thread blocked.
- **Interface 1.1** (`simdev.c` answers `GET_INFO` with power fields it
  is told to show): each gate reason and its action; the quarantine
  beginning and ending, and never ending (the device stays draining,
  nothing is sent, the notices come once each); the lapse reached by the
  library's own readings (`simdev.c` applies it in `GET_INFO`, as the
  driver does); a stopped call classified `MTPE_POWER` only by a power
  reason in the reading right after the stop (each reason, and
  `MTPF_STANDBY` for 1 and 4 only), a count change alone giving
  `MTPE_STOPPED` with `MTPF_POWER_SEEN` (a device query early in a long
  job, then an unrelated 995), 21 without a power reason giving the
  started gate's result; a write stopped
  in its data phase, a DeleteObject and a new folder whose responses are
  lost, each `MTPF_UNCERTAIN` and none sent again; the code precedence
  when several apply; a recovery step failing 21 or ending 995 (no
  Device Reset sent, its result by the same rule); `GET_INFO` failing,
  and answering 0, 39, 40, 41, 42 and 43 bytes (each refused at open),
  44 and 51 bytes (features off), a later reading shorter than the
  open's (a failed reading), 52 bytes with minor
  0, and 52 with a `cbSize` smaller than the bytes returned; combined
  gate reasons, draining taking precedence; a count change before a
  job's baseline not taken as the cause of a later stop, and the
  fallback classification; the counts across their wrap at 65,536.
- `hosttest.ps1` builds with the repository's MSVC 6.0, as
  `cfgtest.ps1`, and runs it; `build.ps1` runs it too. The library's
  imports are checked against 98 SE's `KERNEL32.DLL` with the program
  (section 8's gate).

**What the build host can test, and what only a guest can.**

| What | Where | Needs |
|---|---|---|
| parsers, framing, names, dates, 7.8's file steps | `hosttest.c` with the fixtures, the fuzzing and the guard pages | nothing |
| the engine, lifetime, jobs, submission handshake, cancel and recovery, events, close, the power hold and its races, interface 1.1's handling | `hosttest.c` with `simdev.c` (an in-memory device behind `MTPIO_OPS` that imitates the driver's interface: whole-packet reads ending at a short packet, `WRITE_ZLP`, `ABORT`'s mask, the 21 and 995 of a closed gate, `GET_INFO`'s power fields), the step hook and the fake clock | nothing |
| the library against the tree's simulator: the profiles and `--ctl` faults the guest legs use, in real time, with real threads, built as shipped (MSVC 6.0, x86) | `hostredir.c`: an `MTPIO_OPS` that connects to `mtpemu.py` on `127.0.0.1` as a usb-redir host (the simulator listens and takes one connection; `serve` in `mtpemu.py`) and imitates the driver's interface over it as `simdev.c` does: reads of whole packets, the class requests' setup packets built as the driver builds them (5.3), the interrupt endpoint read into a queue of 16 with the lost flag, `ABORT` as cancelled packets | Python and `mtpemu.py`; no guest, no QEMU |
| `mtppower.c`'s window and answer | `hosttest.c`, sending the broadcasts itself | nothing |
| the driver itself | its slot table is `slotmodel.c`'s (5.4); the rest only in a guest | - |
| NTKERN's serialisation per handle, the stacks' retirement and timing, Windows' broadcasts, boxes and their order, the gate's real values, the INF and the install, the serial ports after a refusal | the guest legs (11.5; 12, "M1's library stage") | a guest |

`simdev.c` and `hostredir.c` imitate the driver, and `mtpemu.py` and the
library come from the same reading: a difference between an imitation
and the driver shows only in the legs, and the independent oracle stays
11.3's fixtures and QEMU's `usb-mtp`. `hostredir.c` is host-only test
code, never part of the program.

**Code from elsewhere.** No code is adapted in the first cut.

- libgphoto2's `camlibs/ptp2/ptp-pack.c` (and libmtp's copy) unpacks the
  same datasets, but around its own `PTPParams`, `iconv` and free
  allocation; hardening it to 7.2's rules would cost about as much as the
  bounded parsers written from the specification, which already fix every
  field.
- Taken as facts, as before: the Android codes and parameters, and
  SendPartialObject's forced split header (libmtp), the response in place
  of a data phase (libgphoto2's `usb.c`), the 4-byte Get Device Status of
  7.5 (libmtp).
- No MIT or BSD MTP code is used.
- If code is adapted later (libmtp's per-device quirk flags are the
  likeliest), it goes into `lib/lgpl/` as files of their own under 7.1's
  conditions: the licence header read first, LGPL-2.1-or-later kept, the
  copyright lines and notice kept, the changes dated, a `NOTICE.md` row.

**Order of work**, each step with its host test before the next:

1. The host test cases above written and reviewed, with the `.expect`
   files: no library code yet.
2. `ptprd.c`, `ptpframe.c`, `ptpdata.c`, `mtpevent.c`, the fixtures and the
   fuzzing: no driver needed.
3. `mtpio.c`, `simdev.c`, the lifetime, jobs and the engine in
   `ptpsess.c`, the operations.
4. Cancel, recovery, events and interface 1.1's handling against
   `simdev.c`'s faults; `mtppower.c` and the power hold.
5. `hostredir.c`, and the library against `mtpemu.py` on the build host,
   every profile and fault.
6. The device list (3.2.1): `libmtpids.py`, the tables, `mtpids.ps1`,
   `geninf.ps1`, the gate and `inftest.ps1`; the generated INF.
7. `MTPTEST` moved onto the library (with `pbwatch`), rerun on the three
   configurations with the simulators, and read-only with the Fairphone 3
   on the owner's bench; then `MTPEXP /c` with the power and scan
   threads, and the acceptance legs of section 12, "M1's library stage".

**Before any write leg on a real phone** (Q14, decided: the leg is
allowed with exactly these limits, and runs only when it is started as
such): an exact list of the
operations allowed (SendObjectInfo, SendObject, DeleteObject of the
leg's own objects; nothing else); backed-up, disposable data in a folder
the leg creates, and nothing outside it touched; the target's ownership
proved as above before any deletion; the interrupted and uncertain
outcomes of each operation run first on `simdev.c`; and every upload read
back and compared, with the folder's other contents compared before and
after.

**Questions for the owner:** none of this section's is open. Q14 to
Q17, Q19 and Q21 to Q27 were decided (owner, 2026-11-17); Q24 to Q27
(an upload failing before SendObject's data, a readback that differs, a
deletion's bounds, a character outside the BMP in a name) came with the
host cases (12.1).

## 8. The program: "MTP Explorer" (`MTPEXP.EXE`; names are owner drafts)

A Win32 GUI program, ANSI, built with MSVC 6.0.

- **Imports:** only functions in the export tables of 98 SE's and ME's
  `kernel32`, `user32`, `gdi32`, `comctl32`, `comdlg32`, `shell32`,
  `advapi32` and `ole32` (the drag of 8.3, and 8.6's second way).
  - The gate checks its import table against those tables, as `usbprn`'s
    does.
  - The tables of `gdi32`, `comctl32`, `comdlg32`, `shell32` and `ole32`
    are read from the guests' system files in M1, for names and ordinals
    only.
- **Common controls:** features of comctl32 4.70 and later, which 98 SE
  and ME have: tree view, list view including the virtual list
  (`LVS_OWNERDATA`), toolbar with the standard bitmaps, status bar and
  progress bar.
- **Threads:** one window thread, one I/O thread per device (the only
  thread that sends transactions, 7.3), one event thread per device
  that blocks in `WAIT_EVENT` on a bound handle and posts jobs, and one
  cancel thread per device (7.9); per process, the power thread, which
  answers Windows' power broadcasts and nothing else, the scan thread,
  which reads `GET_INFO` on info handles, and the name thread, which
  reads a new device's strings once (7.9, "Standby: the program refuses
  during a transfer", "The registry"). The I/O model is the one M0
  selects (5.6).
- The program fills a gap Windows leaves and exists to use the driver
  here (`AGENTS.md`, "Sources", user-mode code).

### 8.1 Window

```
+----------------------------------------------------------------------+
| MTP Explorer (wdm-free)                                        _ [] X|
| File  Edit  View  Help                                               |
| [Up] [Copy to PC] [Copy from PC] [New Folder] [Delete] [Props] [View]|
+---------------------+------------------------------------------------+
| - Pixel 8           | Name             Size      Type       Modified |
|   - Internal shared | DCIM                       Folder     ...      |
|     + Alarms        | Download                   Folder     ...      |
|     + DCIM          | IMG_0001.jpg     3,204 KB  JPEG Image ...      |
|     + Download      |                                                |
| - Canon EOS         |                                                |
|   + SD              |                                                |
+---------------------+------------------------------------------------+
| 3 objects | Internal shared storage: 41.2 GB free of 128 GB          |
+----------------------------------------------------------------------+
```

- **Tree, left.** Each device, named by its model (GetDeviceInfo, or the
  USB product string). Under it, its storages, by description or label.
  Under those, the folders, read when expanded.
- **List, right.** The selected folder in the Details view: Name, Size,
  Type and Modified, sortable by clicking a column. Large Icons, Small
  Icons and List are on the View menu. Icons and type names come from
  the PC's own file associations, by extension (`SHGetFileInfo` with
  `SHGFI_USEFILEATTRIBUTES`).
- **A splitter** between the two. The window's size and place, the
  splitter and the view are kept in `HKCU\Software\usb-wdm-free\usbmtp`.
- **Status bar:** the number of objects, and the storage's free space and
  capacity.
- **Keys:**
  - Enter opens; Backspace goes up; F5 refreshes;
  - Del deletes; F2 renames; Ctrl+A selects all;
  - Ctrl+V pastes files copied in Explorer into the folder shown (8.3).
- **Right-click menus** on rows and tree nodes.

### 8.2 Operations

| Operation | How |
|---|---|
| Browse | tree and list as above; a folder is read once and kept until refreshed, an event changes it, or the session is lost (7.3) |
| Copy to PC | select, Copy to PC: a folder picker (`SHBrowseForFolder`), then the copy (7.6, 7.8). Folders copy with their contents. A name clash asks Replace, Replace all, Skip or Cancel. Or drag the selection to Explorer or the desktop (8.3) |
| Copy from PC | Copy from PC: a file dialog (several files), or a drop or paste from Explorer (8.3), into the folder shown. Greyed when the storage is read-only or the device lists no SendObjectInfo |
| Open | double-click a file: it is copied to a temporary folder and opened with its program. Changes are not sent back, and the program says so the first time (owner, 2026-11-16: included) |
| New folder | File, New Folder: "New Folder" (or "New Folder (2)", ...), then rename in place if the device can rename |
| Delete | with a confirmation naming the count; for folders with contents it says so |
| Rename | F2 or File, Rename, only when the device can set ObjectFileName (7.4); otherwise greyed |
| Properties | name (exact, 7.6), type, size in bytes, dates, format code, object handle; for a storage, its type, file system, access and space; for a device, maker, model, version, serial, protocol (PTP or MTP), VID/PID and bus speed |
| Refresh | F5: the folder again; on a device node, its storages again |
| Progress and cancel | a progress window per copy: file name, "file 3 of 12", bytes, rate, time left, Cancel (7.5). The main window stays usable: other devices can be browsed during a copy |
| Several devices | every device the program has claimed is a top node, with its own I/O and event threads, so a copy on one does not block another |
| Plug and unplug | `WM_DEVICECHANGE` (any) and a 2 s poll make the program's scan thread read `GET_INFO` on `\\.\USBMTP0` to `15`. A new n or generation is claimed as a new device; a gone one is dropped (5.2). An unplugged device's node goes; a copy in progress ends with "The device was disconnected.", by the safe path of 7.8. A device whose calls fail with the device gone is dropped at once, without waiting for Windows' removal (5.6, 7.5) |
| Never End Task | the program's own Cancel and time limits (7.5) end every stuck transfer, so the user never needs End Task to get out of one. If a user ends the program with End Task anyway and then unplugs the phone, the device may stay in Device Manager until the machine is next touched (M0, 5.6): harmless, and the readme may say so |
| Phone locked, or not in file-transfer mode | a device with no storage shows one grey node, "No storage. Unlock the phone and choose File transfer on it.", read again on StoreAdded or Refresh |
| A device another program has claimed | the claim fails; the node says "In use by another program" |

### 8.3 Drag and drop, copy and paste

- **From Explorer into the program** (copy to device): included. The list
  takes `WM_DROPFILES` (`DragAcceptFiles`, `DragQueryFile` in `shell32`),
  and Ctrl+V takes the clipboard's `CF_HDROP`. About 150 lines.
- **From the program out to Explorer or the desktop** (copy to PC): in
  the first version (owner, 2026-11-16). Two mechanisms were weighed:

  | | Virtual files: `CFSTR_FILEDESCRIPTOR` and `CFSTR_FILECONTENTS` over an `IStream` | Real files staged in a temporary folder, offered as `CF_HDROP` |
  |---|---|---|
  | Who copies from the device | Explorer, pulling our stream across processes, during its drop | the program, in its I/O thread, with 7.5 and 7.8 |
  | Progress and Cancel | Explorer's, if any; a cancel is a failed stream read, which Explorer reports as its own error | the program's own progress window and Cancel |
  | A device error mid-copy | reaches the target file Explorer is writing, possibly after Explorer has replaced an existing file | stays in the temporary folder; the target is not touched |
  | 98 SE and ME support | whether their shell accepts virtual files, and folders among them, is unverified | `CF_HDROP` is what every shell version and most programs accept |
  | Cost | the same COM objects plus a stream object; no extra disk | free space for the files on the temporary folder's disk; a second copy when the drop is on another disk |

  **Taken: staged real files.** It is the one that gives a cancellable copy
  with the program's progress, keeps a device fault away from the user's
  folder, and relies on nothing unverified in the shell. The disk cost is
  said in the readme.

  **The rule:** `CF_HDROP` names complete, usable files from the moment
  it is offered. A target may fetch and keep the list when the mouse
  enters it and never ask again, may read the files' sizes then, or may
  open them at once. So no empty placeholder is ever offered.

  **The baseline: stage, then drag.**

  1. **Drag start** (`LVN_BEGINDRAG` or `TVN_BEGINDRAG`):
     1. The program checks that the `%TEMP%` disk has room for the whole
        selection. If not, nothing is staged, and a message points to
        Copy to PC.
     2. It copies the selection from the device into a new staging folder
        under `%TEMP%\MTPEXP\`, by 7.6's names and 7.8's temporary-file
        steps, with its progress window and Cancel. While this runs the
        drag has not begun: no OLE call is in progress, and the window
        works normally.
     3. Cancel or a device error ends it there: the staging folder is
        removed and no drag happens.
  2. **The drag** (`DoDragDrop`, `ole32`), once every file is complete. If
     the left button is still down, the drag starts at once with a data
     object offering `CF_HDROP` of the staged files, copy and move
     allowed.
     - A small selection stages in well under a second, so the drag
       feels immediate.
     - If the button was released while staging ran, no drag starts.
       The status bar says "Ready to drag: drag the files again", and the
       staged set is kept. A drag of the same objects starts at once from
       it, when each staged file still exists with its size and the
       object's handle, size and date are unchanged.
  3. **The target** (Explorer, the desktop, another program) copies or
     moves complete local files by its own rules: its own replace prompt,
     its own error handling, its own progress. Section 7.8's guarantee
     covers only the staging copy, not what the target then does. On the
     `%TEMP%` disk the shell's default is a move, which is a rename, so no
     second copy is made. The device is never changed by a drag.

  **When staged files go.** A target may keep the paths and use the files
  later, and a move empties the folder. So they are not deleted when
  `DoDragDrop` returns. A staging folder is deleted:

  - when the program exits, for the sets it made, file by file; a file in
    use stays;
  - at the program's next start, if it is under `%TEMP%\MTPEXP\` and
    more than a day old, and no other copy of the program is running;
  - while the program runs, when the staged sets together pass 25% of the
    `%TEMP%` disk's free space: the oldest set first, and only a set more
    than 10 minutes old.

  **Drop-time filling: an optimisation, not the baseline.** Filling the
  files only when the button is released, inside
  `IDropSource::QueryContinueDrag`, before it returns DROP, would save the
  wait for large selections. It is adopted only if M2 shows on 98 SE and
  ME that it works, with its own design:

  - OLE's drag loop is suspended while that method runs, and holds the
    mouse capture.
  - The program must run its own message loop there: `PeekMessage` /
    `MsgWaitForMultipleObjects`, dispatching to the progress window only.
  - Esc must be read with `GetAsyncKeyState`, since OLE delivers it no
    more.
  - Re-entrancy is closed: no second drag, no command, no window close
    while it runs.
  - The method returns DROP only after the files are complete, and
    CANCEL otherwise.
  - The effect last reported to `GiveFeedback` is feedback, not a
    commitment by the target, so the method must still handle a drop the
    target then refuses.
  - The `CF_HDROP` offered before the release must then name files that
    already exist: this is exactly the rule above. So the optimisation
    can only apply to a target known not to read the files before the
    drop, and M2 must show such a target exists before any of this is
    built.
  - Filling inside `GetData` at drop time is not used: a target may have
    fetched the list earlier and never call it again.

  - About 600 to 800 lines of COM in C (the data object with its format
    enumerator, the drop source, staging, the staged-set cache and
    cleanup).
  - Ctrl+C in the program, for pasting in Explorer, would stage the same
    way, then put the data object on the clipboard. It is left out of the
    first version.

### 8.4 Command-line mode (for the legs, and for scripts)

`MTPEXP /c <command> [args] [/log <file>]` runs one command without a
window, writes its result to the log, and returns 0 on success (read with
`START /W` from a batch file on 98 SE and ME).

- **Commands:**
  - `list`, `ls <path>`, `info <path>`;
  - `get <path> <local>`, `put <local> <path>`;
  - `mkdir <path>`, `rm <path>`, `ren <path> <name>`;
  - `sum <path>`: the CRC-32 of an object, read from the device;
  - `cancel-after <bytes>`, a test option that cancels a `get` or `put`
    at a given byte count.
- **Paths:** `<n>:<storage>\<folder>\...\<name>`. `<n>` is the device
  number, and `<storage>` a description or `#<storage id>`; `\uXXXX`
  stands for a character outside the code page.
- About 400 to 600 lines over the same library.

### 8.5 Left out, and the cost of each

| Left out | Why | Cost |
|---|---|---|
| A shell namespace view ("the phone under My Computer") | not asked for | 6,000 to 10,000 lines of COM ([mtp-estimate.md](mtp-estimate.md) 4, C) |
| A drive letter | not asked for; MTP has no byte-addressed writes | 15,000 to 25,000 lines, two kernels (route D) |
| WIA on ME | not asked for; camera-shaped | 4 to 6 weeks (route E) |
| Thumbnails view | GetThumb is optional on devices, and each thumbnail is a transaction | about 800 lines and a JPEG decoder (`uvcdec`'s could serve) |
| Search | not asked for | about 400 lines, after a listing of the whole device |
| Cut, paste and move within the device (MoveObject) | optional on devices | about 200 lines |
| Starting the program when a device is plugged in | not wanted (owner, 2026-11-16); it would need a resident process | about 200 lines |
| Resuming an interrupted copy (GetPartialObject64) | Android only | about 300 lines |

### 8.6 Install and start

- The INF copies `MTPEXP.EXE` with the driver to the Windows folder
  (`%10%`).
- **A Start menu entry, "MTP Explorer".** Three ways, each read
  separately in M1:
  1. the INF's `UpdateInis` to `setup.ini`'s `[progman.groups]`, the 9x
     INF way to make a Start menu entry. Whether a device install runs it
     is not known;
  2. the program, the first time it is run, offers to add the entry
     itself (`IShellLink`);
  3. neither: the readme says to run `MTPEXP.EXE` from the Windows folder
     or the package folder.

  The first that works on both systems is used. The legs check the
  entry's existence and that it starts the program. Owner, 2026-11-16:
  the INF copies the program and adds the entry; that a device install
  does so is to be shown by the legs, not assumed.
- Not started at plug-in (owner, 2026-11-16).

## 9. Package, layout and version

- **Package** `usbmtp`, class folder `src/image/` (a new class: still
  image, cameras and phones). `status.md`, "Layout", gains `image`.

  | Path | What |
  |---|---|
  | `src/image/usbmtp/sys/` | `USBMTP.SYS`: `usbmtp.c` (DriverEntry, PnP, power, dispatch, handles, transfers, timeouts, abort, class requests, identification of 3.3: one file of about 1,700 lines), `slot.c` (the slot table of 5.4, also built into `slotmodel.c`), `cfgwalk.c` (the configuration descriptor walk of 3.3, also built into `cfgtest.c`), `usbmtp.h` (the controls of 5.3, shared with user mode), `usbmtp.rc` |
  | `src/image/usbmtp/lib/` | `mtplib`: `ptpframe.c` (7.2), `ptpsess.c` (7.3 to 7.5), `ptpdata.c` (datasets and property lists), `mtpnames.c` (7.6), `mtpdates.c` (7.7), `mtplib.h` |
  | `src/image/usbmtp/app/` | `MTPEXP.EXE`: `mtpexp.c` (windows), `copy.c` (jobs and progress, 7.8), `cli.c` (8.4), `mtpexp.rc`, the icon |
  | `src/image/usbmtp/probe/` | `MTPPROBE.EXE`, the M0 probe (5.6), shipped for its `/pnp` report (owner, 2026-11-17; its shipped form, Q21); `MTPPRB.SYS`, never shipped |
  | `src/image/usbmtp/` | `USBMTP.INF`, `build.cmd`, `build.ps1`, `gate.ps1`, `inftest.ps1`, `hosttest.c`, `slotmodel.c` and `slotmodel.ps1` (5.4), `cfgtest.c` and `cfgtest.ps1` (3.3), `hosttest.ps1`, `testdata/` (fixtures, 11.3), `imports-*.txt`, `exports98/`, `mtpemu.py` (11.2) |

- **Released**, `free/image/usbmtp/w98se-me/`: `USBMTP.SYS`, `USBMTP.INF`,
  `MTPEXP.EXE`, `README.TXT`; and, with the device list (3.2.1), the
  table's copy, the LGPL 2.1 text, a notice naming libmtp's authors, and
  `MTPPROBE.EXE` (named `MTPIDS.TXT`, `LGPL21.TXT`, `NOTICE.TXT` as
  `free/tools/camcheck/` has one: owner, 2026-11-17, Q21). Every name is
  8.3.
- **Version** 0.1.0.0 (INF `DriverVer`, and the driver's and the program's
  version resources).
- **Class in Device Manager:** a class of the package's own, "Portable
  Devices" (a draft the owner let stand, 2026-11-16).
  - It is installed by the INF's `[ClassInstall]`, as `LIBUSB0.INF`
    installs its own class.
  - It has a GUID of its own, generated once for the package and recorded
    in the INF and in this section at M1. Windows' WPD GUID is not used.
- **`STAGE.TXT`:** `STAGE | W98SE WME |
  free/image/usbmtp/w98se-me/USBMTP.INF` (owner, 2026-11-16), since
  neither system matches `06/01/01` itself.
- **Systems:** 98 SE and ME only (owner, 2026-11-16). No other system is
  built for or given an INF section.

## 10. Sizes (estimates)

| Piece | Lines |
|---|---|
| `USBMTP.SYS` | 2,000 to 2,500 |
| `mtplib` | 4,500 to 4,700 (7.9) |
| `MTPEXP.EXE` (GUI) | 3,000 to 4,000 |
| drag out to Explorer (8.3) | 600 to 800 |
| command-line mode | 400 to 600 |
| `MTPPROBE.EXE` | 300 to 500 |
| `mtpemu.py` | 1,500 to 2,000 |
| QEMU `usb-mtp` patch for Windows | 100 to 200 |
| host test, fixtures, gates, INF test | 1,500 to 2,000 |
| `slotmodel.c` (the slot table's model, 5.4) | 400 to 700 |

## 11. Testing

### 11.1 QEMU's `usb-mtp` on Windows

- `usb-mtp` is an MTP responder over a host folder:
  - GetDeviceInfo, sessions, storage, handles and ObjectInfo;
  - GetObject, GetPartialObject, DeleteObject, SendObjectInfo and
    SendObject;
  - three property operations and Cancel Request (`hw/usb/dev-mtp.c`).
  - Its descriptors are `06/01/01`, at Full and High Speed (64 and 512
    bytes).
- **Why it is absent on a Windows host:** QEMU builds it only off Windows
  (`hw/usb/meson.build`: `if host_os != 'windows'`).
- **What blocks the Windows build** is not the file monitor. QEMU's
  `util/filemonitor-stub.c` is built where inotify is absent, and
  `usb-mtp` already lives with it: it logs that monitoring failed and
  sends no host-side change events. The blockers are POSIX calls:
  - `sys/statvfs.h`;
  - `open(O_DIRECTORY | O_NOFOLLOW)` with `fdopendir`, and `lstat`;
  - files opened without `O_BINARY`;
  - `mbstowcs`/`wcstombs` with a 16-bit `wchar_t`, which garbles names
    outside the C locale.
- **The patch**, `docs/testing/qemu-mtp-win32.patch`, against the QEMU
  11.1.0 release (`qemu-11.1.0.tar.xz`, as the repository's other QEMU
  patches):
  - lift the Windows gate;
  - under `_WIN32`, use GLib's UTF-8 file functions (`g_dir_open`,
    `g_stat`, `g_open` with `O_BINARY`) and `GetDiskFreeSpaceExW`;
  - convert names with `g_utf8_to_utf16` and back, so a host file named
    in any script reaches the guest with its exact UTF-16 name.
  - `dev-mtp.c` keeps its upstream notice (Red Hat, Inc.; GPL v2 or
    later). The patch's changes are GPL-2.0-or-later. A `NOTICE.md` row
    is added with the patch, as for `qemu-usbnet-hs.patch`.
- **The build:** in MSYS2 MINGW64 with the usbredir and GLib packages, so
  one binary carries both `usb-mtp` and `usb-redir`, configured as the
  earlier QEMU kits ([2026-10-23-qemu-xhci-cancel.md](../testing/2026-10-23-qemu-xhci-cancel.md),
  "Build") with `x86_64-softmmu`. Where the tree and the build sit is
  recorded in the run's testing record, not here.
- `usb-mtp` matters most as an implementation that is not ours: it
  checks the library against someone else's reading of the
  specification.

### 11.2 `mtpemu.py`, a simulator over usb-redir

As `uacemu.py` and the other simulators: this repository's own code,
GPL-2.0-or-later, and its strings say SIMULATED. It covers what `usb-mtp`
cannot:

| Profile | What |
|---|---|
| `android` | `06/01/01`, interface string "MTP", OS descriptors, composite with an ADB interface; two storages; GetObjectPropList, SetObjectPropValue (rename), the Android extension list; events; zero-length packets after whole-packet phases |
| `androidff` | the same with the MTP interface `FF/FF/00` |
| `camera` | PTP only: no property operations, a `DCIM` tree of associations, GetObjectHandles refusing a parent filter, no zero-length packets, read-only storage |
| `locked` | no storage until a command on the simulator's socket "unlocks" it; then StoreAdded |
| `huge` | synthetic objects of 3 GiB, 4 GiB - 2, 4 GiB and 5 GiB whose bytes are a formula (no host disk used) |
| `names` | Unicode names (Latin, Greek, CJK, a surrogate pair), names with `\ / : * ?`, `CON`, `con.txt`, `.`, `..`, a name of dots, a 254-character name, two names that show the same in the code page, names differing only in case |
| `many` | 5,000 files in one folder |
| `faults` | per command on the socket: a stall in mid data phase; an early short packet; a response that never comes; DeviceBusy (2019h) for n queries; a stale response container; a response in place of the optional zero-length packet; halted endpoints reported by Get Device Status; a Get Device Status with a wrong length, a wrong code or an endpoint not its own; a device-initiated cancel (bulk halts, TransactionCancelled, the CancelTransaction event); a DeviceReset event; a Cancel Request ignored (so the reset path runs); an event flood of 100 |
| `split` | as `android`, sending every data phase in the split form of 7.2 and expecting it back |

### 11.3 An oracle that is not the library's own

The simulator and the library are written from the same reading, so the
simulator alone would test the library against its own assumptions.
Three checks are independent of that reading:

- **Fixtures by hand.** `testdata/` holds byte sequences written directly
  from the specifications' tables, each with the specification section it
  comes from. They cover every container type, datasets with the largest
  and smallest strings and arrays, and the 7.2 boundary cases. They also
  hold malformed input:
  - lengths 0 to 11;
  - a length past the data;
  - a string count past the payload;
  - an array count times the element size overflowing 32 bits;
  - a wrong transaction id or type;
  - property list elements of unknown and wrong datatypes.

  `hosttest.c` runs the library's parsers over them, and every malformed
  case must give a protocol error, never a crash or a read past the
  buffer.
- **QEMU's `usb-mtp`** (11.1), a separate implementation.
- **Real devices:** the bench phones and camera. Their descriptors and a
  log of the transactions of a listing and a copy (taken with the
  program's own trace build) are kept as fixtures too.

### 11.4 Host tests

`hosttest.c` builds `mtplib` with MSVC 6.0 and runs it on the build host,
against the fixtures and against a scripted fake transport. It covers:

- containers split across packets, and every framing rule of 7.2, in
  both the combined and the split form and in both directions:
  - early short packet;
  - the 12-byte header packet;
  - optional zero-length packet, or a response in its place;
  - the boundary points FFFFFFF2h and FFFFFFF3h of payload, and
    FFFFFFFEh and FFFFFFFFh of ObjectCompressedSize;
- event containers and their transaction id rules (7.4);
- the session and id rules of 7.3;
- the cancel procedure (7.5), with a fake clock:
  - every phase;
  - completion winning the race against the abort;
  - each malformed device status;
  - the device-initiated case;
  - both deadlines;
- the name mapping, rule by rule, and clashes after mapping (7.6);
- dates (7.7);
- the temporary-file and replace sequence of 7.8, against a fake file
  layer that fails at each step in turn. For each step: the original's
  contents are under its name or the reported backup path; a copy
  reported done has its data under the target name; a failed copy either
  keeps its data under a reported temporary path or is reported failed
  with the temporary file deleted, exactly as 7.8 says;
- sizes 2^31 - 1, 2^31, 2^32 - 14, 2^32 - 13, 2^32 - 2, 2^32 - 1, 2^32 and
  5 GiB through every size path.

The driver's slot table (5.4) is checked on the build host by
`slotmodel.c`, which drives the driver's own `slot.c` through every
interleaving of its events up to a bounded depth and checks 5.4's
invariants. The real stacks' side of it (how they complete, cancel and
retire) is read in M0 and exercised by the race legs (11.5).

This is the repository's existing place for such tests, as `usbprn`'s
`hosttest.c`. Beside it, `hostredir.c` runs the library, built as
shipped, against `mtpemu.py` on the build host over usb-redir, with no
guest (7.9, "What the build host can test, and what only a guest
can").

### 11.5 Legs

- **Rig:** each leg runs in a headless QEMU guest on a throwaway overlay
  of the bases of [toolchain.md](../toolchain.md). Every QEMU's pid and
  command line goes in the kit's `pids.txt`.
- **98 SE** (`w98se.qcow2`, with NUSB 3.3):
  - under NUSB's EHCI (`usb-ehci`, High Speed);
  - under xHCI98 3.5.0.0, the latest release (owner, 2026-11-16),
    installed by its own Setup (`INSTALL.BAT /console /yes`) as a user
    would, its SHA-256 and the version the guest shows recorded with
    each run (`qemu-xhci`).

  These are the two legs `AGENTS.md` asks for (the owner restated it on
  2026-11-16): every M0 reading and every leg runs on both, and the
  records show the two side by side with the differences called out.
  Nothing is called ready on 98 SE until both have passed.
- **ME** (`wme.qcow2`):
  - under xHCI98;
  - under ME's own UHCI (Full Speed), as an extra stack, not a
    commitment (deferred).
  - **No USB 2.0 leg on ME (owner, 2026-11-16).** ME has no USB 2.0
    driver of its own, NUSB does not install on ME
    ([open-items.md](../testing/open-items.md), 47.19), and no USB 2.0
    stack for ME is available. The owner decided that ME may be called
    ready and released on its xHCI98 half alone; ME's USB 2.0 half is
    recorded as not tested, and the user documents say plainly that on
    ME the package was tested under xHCI98 only. This replaces Q12.
- **98 SE's own UHCI:** not a commitment. The design promises nothing on
  98 SE's or ME's UHCI stacks; a UHCI leg may be added later as an extra,
  and M0 did not read the composite there.
- **Disks:** a second FAT32 disk for the large-file legs, and a small one
  for the disk-full leg.

| Step | Devices | 98 SE NUSB EHCI | 98 SE xHCI98 | ME xHCI98 | ME USB 2.0 | ME UHCI (extra, not a commitment) |
|---|---|---|---|---|---|---|
| M0: ids, composite node and its configuration, Have Disk pick, WIA on ME | `usb-mtp`, `android`, `androidff` | yes | yes | yes | not tested (owner) | deferred |
| M0: the NTKERN contract of 5.6 | `usb-mtp` with `MTPPROBE` | yes | yes | yes | not tested (owner) | deferred |
| install from the wizard; Device Manager name; Start menu entry (8.6) | `usb-mtp`, `camera` | yes | yes | yes | not tested (owner) | yes |
| a SuperSpeed device: START, GET_INFO's speed 3, a session (the simulator's SuperSpeed profile under xHCI98; the same device at high speed under the USB 2.0 stack) | `android` at SuperSpeed | high speed | yes | yes | not tested (owner) | - |
| list, get, put (1 KB to 100 MB, CRC-32 compared with the host's) | `usb-mtp`, `android`, `camera` | yes | yes | yes | not tested (owner) | yes |
| new folder, rename, delete, folder delete | `android` (rename), `usb-mtp` | yes | yes | yes | not tested (owner) | yes |
| cancel mid-get and mid-put at 20 byte offsets each, the next copy verified; cancel just as a transfer completes (the simulator delays its response by 0 to 5 ms); measured cancel times | `usb-mtp`, `android`, `faults` | yes | yes | yes | not tested (owner) | yes |
| unplug mid-copy; replug; 20 unplug-replug cycles under 1 s each with the program open (n reuse, 5.2) | `android` | yes | yes | yes | not tested (owner) | yes |
| standby during a copy: refused by the program (`BROADCAST_QUERY_DENY`; no power IRP reaches the driver), the copy finishing and verified, a COM port opened after it on 98 SE; and each row of 7.9's races table ("Standby: the program refuses during a transfer"), the event order recorded | `android`, `single` | yes | yes | yes (the program's refusal on ME read here first) | not tested (owner) | - |
| standby while another client transfers and the program idles: the program agrees; with the production build the driver stops the transfer and accepts; with the `standbytest` build the driver refuses and the program shows its refusal message (13), with the serial sentence on 98 SE and without it on ME | `single`, `MTPTEST xferout` | yes | yes | yes | not tested (owner) | - |
| standby with the explorer open and no copy, then resume: a new session, the tree refreshed, events arriving again | `android` | yes | yes | yes | not tested (owner) | - |
| two devices at once | `usb-mtp` and `camera` | yes | yes | yes | not tested (owner) | - |
| sizes 2^31 - 1, 2^31, 2^31 + 1, 4 GiB - 14, 4 GiB - 13 and 4 GiB - 2 both ways; 4 GiB and 5 GiB objects refused for FAT32 with their sizes right; disk full mid-copy (the original kept) | `usb-mtp` (host files), `huge` | yes | yes | yes | not tested (owner) | 100 MB only (Full Speed) |
| names, clashes, an existing file kept when a replace fails | `usb-mtp` (host files), `names`, `faults` | yes | yes | yes | not tested (owner) | yes |
| locked phone; events; event overflow and full refresh | `locked`, `android`, `faults` | yes | yes | yes | not tested (owner) | yes |
| 5,000-file folder | `many` | yes | yes | yes | not tested (owner) | - |
| every `faults` case | `faults` | yes | yes | yes | not tested (owner) | yes |
| GUI: window, menus, progress, Cancel, drop from Explorer (screenshots) | `android` | yes | yes | yes | not tested (owner) | - |
| drag out to an Explorer folder and to the desktop, on the `%TEMP%` disk and on another disk; Cancel during staging; the button released during staging, then the second drag from the staged set; not enough room in `%TEMP%`; Explorer's replace prompt at the target; the staged sets' removal at exit, at the next start and past the 25% mark | `android`, `usb-mtp` | yes | yes | yes | not tested (owner) | - |
| split-form data phases both ways, with the boundary sizes | `split` | yes | yes | yes | not tested (owner) | yes |
| replace on the PC failing at each step (a read-only target, a target opened by another program, a full disk), the reported paths checked | `usb-mtp` | yes | yes | yes | not tested (owner) | - |

- Most legs are driven with the command-line mode (8.4), from a batch file
  on the guest.
- The GUI legs are driven by keystrokes and read from screenshots
  (`src/generic/usbprobe/monitor.py`).
- A remote file system of unknown size is not in the rig: not run, and
  the readme does not promise it.

### 11.6 What emulation cannot show

These are for the owner's bench, and are said as such in the status rows:

- real host controllers and their timing, under NUSB, xHCI98 and ME's
  stacks;
- a phone's own behaviour:
  - the "allow access" prompt;
  - locking while connected;
  - its MTP stack's quirks and firmware;
  - which interface shape it uses (2);
- real cancel timing, and devices that keep partial objects;
- real throughput.

The bench devices: an Android phone in File transfer and in PTP mode; one
with an `FF` MTP interface if at hand; a camera; an iPhone. Each runs on
98 SE under xHCI98 and NUSB, and on ME under xHCI98.

## 12. Milestones

- **M0, readings:**
  - the QEMU patch and build;
  - `mtpemu.py`'s descriptor and `faults` profiles;
  - the probe driver and `MTPPROBE.EXE` (5.6);
  - on stock 98 SE and ME: the ids, the composite node's configuration
    descriptor and the selection it takes (3.3), WIA on ME, Have Disk;
  - the NTKERN contract (5.6);
  - the specifications' values and procedures checked (7.1; done
    2026-11-16), and this document corrected where they differ;
  - the ME EHCI means (11.5).

  No package code before M0's results are recorded and reviewed. The
  probe driver and `MTPPROBE.EXE` are M0 tools, not package code. They
  live in `src/image/usbmtp/probe/`; the probe driver is never shipped,
  and `MTPPROBE.EXE` is shipped for its `/pnp` report from the device
  list onward (owner, 2026-11-17; 3.2.1).

  **M0's record** (a testing record of its own) holds:
  - every system's and stack's version (98 SE and ME builds, NUSB 3.3,
    xHCI98 3.5.0.0, the UHCI driver of each system), the QEMU builds'
    versions and hashes, the simulator's commit, and dated traces;
  - per stack: the composite ids, the configuration descriptor an
    interface node gets, and the selection forms it accepts and refuses
    (3.1, 3.3);
  - Have Disk with a model whose ids do not match, and WIA on ME (3.2);
  - every item of 5.6, with the order of cancel, cleanup and close with
    calls outstanding, process termination included;
  - for bulk IN, bulk OUT, interrupt IN, control and pipe reset URBs on
    both systems: what `IoCancelIrp` does, what the abort fallback does,
    and how long each takes to retire (5.6, item 5). These decide which
    bounds 5.4 may promise;
  - the specifications' versions and hashes (7.1), and each correction
    this document took from them, by section;
  - each reading on both of 98 SE's stacks side by side, and on ME under
    xHCI98 (ME's USB 2.0 half not tested: owner, 2026-11-16).

  **Deferred past M0** (they may accompany M1; nothing below is promised
  until measured): the byte counts of short and zero-length reads (5.6,
  item 2); the `split`, `locked`, `single` and `faults` profiles beyond
  the faults used in M0; real phones and cameras; a pipe reset's cancel
  (5.4's no-bound rule for `RESET_PIPE` covers it); the UHCI stacks;
  ME's Have Disk and ME's WIA beside our driver (3.2).

  **What M0 can do without the QEMU build:**
  - The probe driver and `MTPPROBE.EXE`, built with the repository's own
    toolchains, and their NTKERN readings (5.6, items 1 to 5). They run
    with a simulator on the host's stock QEMU 11.1, which has `usb-redir`.
  - `mtpemu.py`'s descriptor, composite and `faults` profiles, against
    that QEMU.
  - The binding readings on stock 98 SE and ME: ids, the composite node's
    configuration descriptor and selection, Have Disk, WIA on ME.
  - Reading the 98 SE and ME install media's file lists for the composite
    parent drivers (3.1) and the new export tables (8).

  **What waits on the build:** every `usb-mtp` reading.

  **M1 starts** only after M0's record and the slot table (5.4) with its
  model have both been reviewed.
- **M1, the driver and the command-line mode:**
  - `USBMTP.SYS`, `mtplib`, the fixtures and host test, `MTPEXP /c`, the
    INF;
  - the list, get and put legs on every stack;
  - the Start menu readings (8.6).

  **M1's library stage** (7.9, with the device list of 3.2.1) is
  accepted when each of these has passed and is recorded, the builds'
  hashes with it, in a testing record of its own:

  - On the build host:
    1. `hosttest.ps1`: every case of 7.9's host test, the races table of
       7.9's "Standby" at every step, interface 1.1's cases, the
       allocation balance, the fuzzing's counts; run by `build.ps1`.
    2. The library against `mtpemu.py` through `hostredir.c`: the
       `single`, `android`, `androidff`, `camera`, `split` and `locked`
       profiles at high and full speed, and `android` at SuperSpeed:
       list, get and put with CRC-32 compared, every `--ctl` fault, the
       flood, `MtpClose` during a transfer.
    3. The import gate of `MTPTEST` (on the library) and `MTPEXP /c`
       against 98 SE's and ME's export tables.
    4. The INF gate and every mutation of `inftest.ps1` on the generated
       INF and table.
  - In guests, on 98 SE under NUSB's EHCI, 98 SE under xHCI98 and ME
    under xHCI98 (ME's USB 2.0 half not tested: owner), with the
    simulator, and with `usb-mtp` where its build exists:
    5. Install from the wizard with the leg INF (`geninf.ps1 -Leg`): the
       class line and a listed interface line and whole-device line
       binding, the device's name in Device Manager.
    6. `MTPTEST` on the library, then `MTPEXP /c`: list, get and put of
       1 KB to 100 MB with CRC-32 compared; a cancel mid-get and mid-put,
       the next copy verified; the `faults` cases; an unplug mid-copy.
    7. Standby: with the program idle (slept; a new session and events
       after the wake); during a copy (refused by the program, the copy
       finishing, a COM port opened after it on 98 SE); another client
       transferring while the program idles (the production build: the
       driver stops it and accepts; the `standbytest` build: the driver
       refuses and the program shows its message); the event order
       recorded in each (11.5).
    8. The device list: L1 on both of 98 SE's stacks; L2 on 98 SE and on
       ME (one stack each); the readme's steps with the simulator's
       composite, the text corrected by what they show.
  - Among the host cases of 1 above, each a named case of its own:
    registration while a query is answered or a hold stands, a failed
    open's undo with a query at each point, a detach during a snapshot;
    program exit with a device closing, with one GIVENUP, and with the
    power thread still answering; the callback table's refusals of
    re-entry (an operation from a sink, `MtpQueue` from a trace hook) and
    the cancel latch between files, against job completion and against
    the next dequeue; `MtpQueue` accepted and rejected with exactly one
    `run` or `drop`; the deadline observer for recovery, open, close and
    the command-line mode with the window absent; the first-sight
    warning's scope (a new generation, a risen count, a device leaving
    the scans and coming back, a new run) and a stale draining reading;
    the hold delayed before the driver's gate closes, in QUERY-HELD
    (more than 10 s after the agreement) and in SUSPENDING (more than
    30 s after the suspend broadcast, also a suspend with no hold
    standing), under Q23's (A) and (B); a resume with old-session jobs
    queued; `GET_INFO` failing and answering short;
    every mutating operation's uncertainty and the code precedence.
  - And these interleavings, each driven by the step hook at every
    point named, under both of Q23's rules where the hold is involved:
    - **locks and references:** the power thread's snapshot against a
      registration, a detachment and a release that brings a count to
      zero, with the lock order checked (a section held while the
      registry lock is taken fails the test) and the free shown to
      happen outside both locks;
    - **hold release:** each end of the hold (failed query, refusal,
      resume, the button, the lapse) against a job waiting at the head
      of a held queue, an open waiting on "no hold", a name read about
      to start, and a new agreement arriving as the routine runs;
    - **close and the hold:** closing a held device (no CloseSession,
      only `EVENTS` stop and `ABORT`); `/powerwait` expiring after an
      agreement, then the close, with nothing sent to the device;
    - **an unsolicited suspend with a job running:** the races table's
      row in each phase of the job; under (A) no other job dequeued, the
      running job stopped by the driver's query or set and classified
      by the reading after it, a mutating one uncertain, the session
      lost at the resume and nothing replayed, and no claim that the
      device was quiet;
    - **name reads:** a read stuck for 60 s with queries refused
      throughout, then returning and the next query agreed; exit with
      the read still out, its context and the registry kept;
    - **close and cleanup:** `MtpClose` while a job's call is stuck (the
      deadline running from the request, GIVENUP without the close
      starting); the stuck call returning while `EVENTS` stop or
      `ABORT` is not yet quiet (retained, Busy, retried each second,
      then step 7); CLOSED posted while a snapshot or retained
      reference is still out (the free later, with only the process
      hooks); exit at the budget with a device CLOSING, one OPENING,
      one held open and a name read out;
    - **cancel:** a cancel in RESPONSE (the response read whole, the
      next operation refused); a cancel when every byte had moved (the
      settlement finds RESPONSE); a cancel between two transactions of
      one operation; job N's cancel work still out when job N+1 is
      queued (not dequeued until it is published; no `ABORT` of it
      afterwards);
    - **an Android edit cancelled:** EndEditObject sent once when every
      admission condition holds; not sent when recovery reset the
      device, when the session changed, when a power reason was seen,
      when the device is HELD by an unasked suspend, and when GIVENUP;
      sent during a close; its own lost response uncertain and not
      resent;
    - **callbacks:** an operation from `run` and from the event
      callback accepted; from a sink, a source, a progress callback,
      `drop`, the notice callback and a hook refused;
    - **classification:** `GET_INFO` of 41, 42 and 43 bytes at open; a
      device-query refusal early in a long job followed by an
      unrelated 995 (`MTPE_STOPPED` with `MTPF_POWER_SEEN`, not
      `MTPE_POWER`); a recovery step's 995 with and without a power
      reason after it.
  - The allocation balance distinguishes the bounded, intentional
    retention of a GIVENUP device (its buffers and `MTP_DEV`, kept until
    its stuck call has returned **and** its quiet checks have succeeded,
    and released then) from a leak: the host test makes the stuck call
    return, makes `EVENTS` stop and `ABORT` report quiet (and, in a case
    of its own, keeps them not quiet: still retained, no balance
    required), releases every retained and snapshot reference, and then
    requires the balance.
  - In the guests, every row of 7.9's races table whose leg column names
    a leg (the delayed query answer with `pbwatch delay` and the
    delayed suspend with `pbwatch suspend-delay`, two devices, a
    failed query after the agreement, another client transferring, the
    driver's refusal, the window blocked, idle), each on the three
    configurations; a modal dialog of the program shows only the GUI's
    independence and replaces none of them.
  - The INF gate's template, section, directive, encoding and licence
    checks with every mutation of `inftest.ps1`, and the generated INF's
    licence block read against the table.
  - Not gates of this stage: the owner's bench (the Fairphone 3 read
    only, L3); writes on a real phone (Q14); the GUI (M2).
  - 98 SE is accepted only with both of its stacks passed; ME on its
    xHCI98 half (owner, 2026-11-16).
- **M2, the explorer, reading:** window, tree, list, Copy to PC with 7.8,
  progress and cancel, Open, plug and unplug, several devices; drag out
  to Explorer, with the reading of where its fill runs (8.3).
- **M3, the explorer, writing:** Copy from PC, drop from Explorer, new
  folder, delete, rename.
- **M4, the hard cases:**
  - the size points, names, locked phone, standby, rapid replug, every
    fault, 5,000 files;
  - the status and README rows, and the legs record.
  - Release preparation is the owner's call.

### 12.1 M1's library stage: the host cases and their trace

Step 1 of 7.9's order of work is written: the host cases and their
expected results, with no library code, in
`src/image/usbmtp/testdata/` (the format, the execution contract X1 to
X11 that makes every interleaving exact, the invariants I1 to I10, the
universal assertions U1 to U4 and the default device in its
`README.md`; the cases in `cases/<area>.cas`, each one's observable
result in `cases/<area>.expect`; 33 fixtures written by hand as
commented hex, each with its decoding in a `.expect` of its own).
Nothing there is built or run until the library exists. Where a case and
this document differ, this document holds and the case is corrected.

| Area | Cases | Of which wait |
|---|---|---|
| `job` (jobs, callbacks, re-entry) | 30 | none |
| `cancel` (the latch, the handshake, 7.5, the edit's cleanup) | 54 | none |
| `life` (references, close, GIVENUP, removal, exit) | 26 | none |
| `sess` (the open, sessions, ids) | 21 | none |
| `power` (the hold, the registry, the program's refusal, the scan) | 50 | none |
| `if11` (interface 1.1) | 25 | none |
| `ops` (operation contracts, uploads, deletion, Android) | 33 | none |
| `frame` (containers, data phases, sizes) | 17 | none |
| `event` | 11 | none |
| `data` (parsers, fixtures, caps, budget, fuzzing, allocation) | 13 | 1 (DAT-012: the bench capture Q16 allows, committed only after the owner has seen its fields) |
| `names`, `dates`, `copy` (7.6 to 7.8) | 9, 3, 3 | none |
| `redir` (the library against `mtpemu.py`) | 5 | none |
| all | 300 | |

No case depends on Q14 or Q17 (OPS-028 tests the refusal Q17 keeps).
Every question CQ1 to CQ40 below is answered in this document; CQ10,
CQ11, CQ37 and CQ40 by the owner's decisions on Q24 to Q27, and Q15
holds DAT-005. The cases follow Q18 (c) with the program's refusal,
Q19's four answers, Q21's (no host case touches the device list), Q22's
separate power-change message and Q23 (A); (B) is run as a test
configuration wherever section 12 asks for both rules.

Two rules of the README run across every case and are not repeated in
the tables: **X11**, every case (sweeps and lifetime, event, power and
failure cases included) run again failing its n-th allocation for every
n, each run stopped by X11's one stopping rule, its unreached
directives retired, and judged by I1 to I10, U1 to U4 and a balanced
inventory rather than by the case's own `.expect` (DAT-011 names the
outcomes for one scripted session); and **U1 to U4**, hold admission,
session-tagged admission, the cancel work's identity and no automatic
replay (an uncertain mutation never sent again; a folder's deletion's
own second DeleteObject after a settled PartialDeletion allowed),
checked in every run, not only in the cases named for them. A case
named in the tables below establishes its line when run under the
README's contract (X1 to X11); where a case covers a line only in
part, the row or the list of uncovered statements says so.

**Section 12, "M1's library stage", line by line:**

| Line | Cases |
|---|---|
| 1. `hosttest.ps1`: every case of 7.9's host test | every area; the named cases below |
| 1. the races table at every step | the races table below; each row's "every step" is a `sweep` where the row names one |
| 1. interface 1.1's cases | IF-001 to IF-025 |
| 1. the allocation balance | I9 in every case, block by block; X11 (the n-th allocation of every case); DAT-010, DAT-011; retention told from a leak: LIF-007, LIF-008, CAN-017, CAN-029, JOB-009 |
| 1. the fuzzing's counts | DAT-009 |
| 2. the library against `mtpemu.py` through `hostredir.c` | RDR-001 to RDR-005 |
| 3. the import gate | none: a build gate, not a case (below) |
| 4. the INF gate and `inftest.ps1` | none: step 6 of the order of work (below) |
| 5. to 8. the guest legs | none: guests (below) |
| registration while a query is answered or a hold stands | PWR-009, PWR-010, PWR-043, PWR-050, SES-017 |
| a failed open's undo with a query at each point | SES-020 (each failed step by each point of its undo), SES-003, SES-002 |
| a detach during a snapshot | LIF-013; the snapshot against a release to zero: LIF-026 |
| program exit with a device closing, one GIVENUP, the power thread answering | LIF-019, LIF-020, LIF-021 |
| the callback table's refusals of re-entry (an operation from a sink, `MtpQueue` from a trace hook) | JOB-015, JOB-020; JOB-016 to JOB-019, JOB-021 |
| the cancel latch between files, against job completion and the next dequeue | CAN-001, CAN-012, CAN-013 |
| `MtpQueue` accepted and rejected with exactly one `run` or `drop` | JOB-001 to JOB-011, JOB-029; I4 in every case, teardown included |
| the deadline observer for recovery, open, close and the command-line mode with no window | CAN-052, CAN-018, SES-015, LIF-006, LIF-024 |
| the first-sight warning's scope; a stale draining reading | PWR-040; PWR-015, PWR-016 |
| the hold delayed in QUERY-HELD and in SUSPENDING, and a suspend with no hold, under (A) and (B) | PWR-021, PWR-022, PWR-023 |
| a resume with old-session jobs queued | PWR-026, PWR-044 |
| `GET_INFO` failing and answering short | IF-001 to IF-005, IF-025 |
| every mutating operation's uncertainty | OPS-014, OPS-015, CAN-006, CAN-041, PWR-024, PWR-028; U4 in every case |
| the code precedence | IF-023, IF-022, JOB-029, CAN-019 |
| interleavings, locks and references | LIF-013, LIF-014, LIF-025, LIF-026, PWR-009, SES-003, SES-017, SES-020; I1 to I3 and I5 in every case |
| interleavings, hold release (each end against each waiter, a new agreement during the routine) | PWR-032 (each end against each waiter in place before it); a waiter arriving during the routine: PWR-026 (the dequeue), PWR-050 (a registration); PWR-017, PWR-018, PWR-019, PWR-046, PWR-048 |
| interleavings, close and the hold; `/powerwait` | PWR-006; PWR-033 |
| interleavings, an unsolicited suspend with a job running | PWR-024 (the gate closing at each point of the suspend sequence, a job in each phase), PWR-025, CAN-046 |
| interleavings, name reads (stuck 60 s; exit with the read out) | PWR-013, PWR-014, PWR-012 |
| interleavings, close and cleanup | LIF-006, LIF-007, LIF-008, LIF-012, LIF-013, LIF-021 |
| interleavings, cancel (RESPONSE; every byte moved; between transactions; job N's cancel work) | CAN-007, CAN-008, CAN-002, CAN-013; U3 in every case |
| interleavings, an Android edit cancelled | CAN-042 to CAN-049, OPS-031 |
| interleavings, callbacks | JOB-013 to JOB-021 |
| interleavings, classification | IF-002, IF-014, IF-022 |
| the allocation balance telling retention from a leak | LIF-007, LIF-008, CAN-017, CAN-029 |
| the guests' races rows; the INF gate's checks; 98 SE's two stacks | none: guests and step 6 (below) |

**7.9's races table, row by row:**

| Row | Cases |
|---|---|
| a query as a job starts | PWR-001 (every point from the test's `MtpQueue` to the first submission) |
| a query as a job ends | PWR-002 (every point from the last response to the next dequeue) |
| a query during recovery | PWR-003 (every point of 7.5 steps 2 to 5), PWR-047 (every point of step 6, its parts failing and its deadline) |
| a query during open or close; a close during a hold | PWR-004, PWR-005, PWR-006 |
| a query with a call stuck after GIVENUP | PWR-007, LIF-007 |
| several devices, one busy | PWR-008 |
| a registration during an answer or a hold; a failed open; a close and a GIVENUP device during an answer | PWR-009, PWR-010, PWR-011, SES-020, SES-003, LIF-013, LIF-026 |
| a name read when a query comes; a name read stuck | PWR-012, PWR-013, PWR-014 |
| a draining reading stale, removed, of an old generation | PWR-015, PWR-016 |
| an agreement, then `PBT_APMQUERYSUSPENDFAILED` | PWR-017, PWR-037 |
| an agreement, then nothing | PWR-018 (A: the times), PWR-048 (A: the button at every point of the hold), PWR-019 (B), PWR-020 (the wrap) |
| QUERY-HELD delayed | PWR-021 |
| SUSPENDING delayed | PWR-022, PWR-023 |
| a job already running at an unsolicited suspend | PWR-024, PWR-025, CAN-046 |
| a resume with old-session jobs queued | PWR-026, PWR-036 |
| the interval from the agreement to the driver's query | PWR-027 |
| a suspend with a job running and no agreement | PWR-028, IF-007, IF-013 |
| a driver refusal | PWR-029, PWR-041 |
| the window thread blocked | PWR-030 |
| idle | PWR-031, PWR-045 |
| the system's idle timer during a copy | none: the table gives no host test (below) |

**7.9's host test and 11.4, the named cases:** cancel before submission
CAN-003; during the final zero-length packet CAN-009; `MtpClose` during
a transfer LIF-002 and during recovery LIF-003; `MtpCancel` and
`MtpClose` together LIF-004; a failed bind and every failed step of
`MtpOpen` SES-002, SES-006, SES-020; a revoked claim LIF-017; a
replacement LIF-018; the event flood EVT-005, RDR-003; CancelTransaction
while a transfer blocks CAN-038; late completions after given up
LIF-006, LIF-023, JOB-009; the response read at 8, 16, 32 and 64 bytes
FRM-003; a source that underflows OPS-012; the three-handle rule I7,
CAN-051; an operation off the I/O thread JOB-012; `drop` once for every
job not run JOB-008 to JOB-010; the fake transport failing a call made
with a section held I1; `WM_POWERBROADCAST` with a device busy, idle,
held, draining and the window blocked PWR-001, PWR-031, PWR-018,
PWR-038, PWR-030; Android's writing operations OPS-029 to OPS-032; the
program's own refusal PWR-039; framing FRM-001 to FRM-017; events
EVT-001 to EVT-011; ids and sessions SES-009 to SES-014, SES-018,
SES-019; 7.5 with every status row and both deadlines CAN-020 to
CAN-032, its reset's failures CAN-053, CAN-054; names NAM-001 to
NAM-009; dates DTE-001 to DTE-003; 7.8 CPY-001 to CPY-003; the sizes
FRM-013, FRM-014, CPY-003; parsing, malformed datasets, fuzzing and
allocation DAT-001 to DAT-013, OPS-021 to OPS-025.

**The answered questions and the cases that hold each answer:** CQ1
OPS-028; CQ2 SES-002, SES-007, SES-008, IF-001, IF-002; CQ3 PWR-034;
CQ4 JOB-030; CQ5 EVT-007; CQ6 SES-004, SES-013, SES-019; CQ7 IF-019;
CQ8 PWR-013, PWR-032; CQ9 CAN-043 to CAN-049, OPS-014; CQ12 every
`reason=`; CQ13 FRM-004; CQ14 LIF-017; CQ15 JOB-008, JOB-011, JOB-029,
LIF-002 to LIF-005, CAN-048; CQ16 PWR-040 (the program's own file
compiled in); CQ17 IF-025; CQ18 CAN-042, OPS-031; CQ19 CAN-018,
CAN-052, JOB-009, LIF-007, LIF-024; CQ20 CAN-001, CAN-005, CAN-007,
CAN-012; CQ21 CAN-006, CAN-010; CQ22 every `bytes=`; CQ23 CAN-025 to
CAN-027; CQ24 every `EVSTART`; CQ25 CAN-035 to CAN-038, OPS-011,
OPS-012; CQ26 CAN-040, IF-023; CQ28 SES-014; CQ29 IF-017; CQ30 OPS-006,
OPS-032; CQ31 OPS-001, OPS-006; CQ32 EVT-001, EVT-011; CQ33 OPS-002,
OPS-009; CQ35 OPS-016, OPS-020, FRM-004; CQ36 OPS-018; CQ38 OPS-022,
SES-021; CQ39 DAT-003. Session-specific state cleared at a new session
or cache generation: SES-021, OPS-033; no internal transaction while
held: PWR-049 and U1. The owner's decisions: Q15 DAT-005; Q24
OPS-010, OPS-033, CAN-006, PWR-028; Q25 OPS-013; Q26 OPS-019; Q27
NAM-001, NAM-007.

**What no case covers, and why:**

- Section 12's items 3 to 8, the guest rows of the races table and the
  INF gate's checks: build gates, guest legs and step 6 of the order of
  work (the device list), not host cases of this step.
- The races row "the system's idle timer during a copy": its host-test
  column is empty; a leg only.
- Q19 (a)'s operation-level answer: designed but not chosen (the owner
  took the whole job); no case.
- SetObjectPropValue's uncertainty (rename): M3, outside the first cut.
- The texts of section 13: drafts; the cases name the message only
  (`power.cas`'s ids).
- The real phone's datasets as fixtures (DAT-012): Q16 allows them, but
  they are captured on the bench and committed only after the owner has
  seen their readable fields; until then the case waits for the capture.
- The suspend broadcast's own arrival at every point of a running job's
  phase: PWR-024 places it at one point per phase and sweeps the gate's
  closing against the suspend sequence, which is the interleaving the
  races row asks for; the broadcast at every point of the job is the
  query's case (PWR-001, PWR-002), not swept again for the suspend.
- Behaviour across a crash or power loss (7.8) and how 98 SE's and ME's
  tick count behaves in a sleep (7.9): nothing is promised or depends on
  it.
- Whether `simdev.c` and `hostredir.c` behave as the driver does: the
  guest legs (15).

**The questions of the cases** (each with its two readings as first
asked, then where the design now answers it; CQ10, CQ11, CQ37 and CQ40
were the owner's, as Q24 to Q27 of section 14, and are decided):

- **CQ1.** Android's 95C2h to 95C5h on a device that is not a simulator
  and without the test flag: (a) refused with `MTPE_ARG`, or (b) with
  another code; and "its strings say SIMULATED" means (a) the USB string
  descriptors, or (b) GetDeviceInfo's strings. (Also waits for Q17.)
  **Answered** (7.9, "Android's writing operations", "Disabled on real devices"): (a) `MTPE_ARG` with `MTPR_ARG_UNQUALIFIED`; the strings are GetDeviceInfo's Model string. Q17 stays the owner's.
- **CQ2.** `MtpOpen`'s code for a failed step other than a busy or
  claimed `CLAIM`, a `GET_INFO` of 40 to 43 bytes and the deadline (a
  failed reading at open, a generation mismatch, a major version not 1,
  a handle or bind failure, the open's transactions failing): (a)
  `MTPE_DEVICE` for every one, or (b) a code per step (`MTPE_GONE` for a
  handle that does not open, ...).
  **Answered** (7.9, "Open is all or nothing", the table of each failed step's code): (b), a code per step.
- **CQ3.** An open inserted HELD: (a) one deadline of 40,000 ms from the
  call covers the wait and the transactions, or (b) the wait has its own
  40,000 ms and the transactions a fresh deadline after it.
  **Answered** (7.9, "Open is all or nothing" and the waiters table): (a), kept as it stood.
- **CQ4.** An operation called in the same `run` after the session was
  lost: (a) `MTPE_SESSION` at once, nothing sent, or (b) sent in the new
  session once there is one. (No case yet; JOB-010 and CAN-019 avoid it.)
  **Answered** (7.3, "An operation after the loss"; 7.9, "The cancel latch"): (a) `MTPE_SESSION` at once, nothing sent, even after a new session.
- **CQ5.** The transaction a DeviceReset event cancels: (a)
  `MTPE_CANCELLED` with `MTPF_SESSION_LOST`, or (b) `MTPE_SESSION`.
  **Answered** (7.9, "Events", routed to the engine; "One code, and flags"): (a).
- **CQ6.** "Retried 10 times" (`CLAIM`) and "retried 3 times"
  (OpenSession's DeviceBusy): (a) 1 + n tries, or (b) n tries in all.
  **Answered** (7.3; 7.9, "Open is all or nothing"): (a), 11 `CLAIM`s and 4 OpenSessions in all.
- **CQ7.** A call failing 21 on a driver without the power fields while
  `GET_INFO`'s Flags (offset 37, within 44 bytes) shows removal pending:
  (a) `MTPE_STOPPED` ("always" for such a driver), or (b) `MTPE_GONE`.
  **Answered** (7.9, "Classifying a stopped call"): (a), kept as it stood; a later 5 or 1167 still finds the device gone.
- **CQ8.** `MtpNames` while a query is in progress or a hold stands, or
  for a generation whose earlier read is stuck: (a) it returns at once
  with `MTPE_HELD` (and the program retries at the next scan), or (b) with
  another code, or waits.
  **Answered** (7.9, "The registry", strings, "What `MtpNames` returns"): (a) `MTPE_HELD` at once; names already cached return `MTPE_OK` with no driver call.
- **CQ9.** Where the result says that an edit was left open: (a) a flag
  of its own in `MTP_RESULT`, or (b) only `MTPF_UNCERTAIN` and the trace.
  **Answered** (7.9, "One code, and flags"; "Edit lifetime"): (a), the flag `MTPF_EDIT_OPEN`.
- **CQ10.** An upload whose SendObjectInfo succeeded: (a) a failure
  between the pair, or after SendObject's command was written but before
  its data, abandons the reservation with no DeleteObject (the command
  may have reached the device: uncertain), or (b) such a failure deletes
  the reserved handle once ("before SendObject's data phase began").
  **Decided** (Q24, owner, 2026-11-17; 7.9, "Uploads and deletion"): by state; the reserved handle deleted once in states (i) and (iii), abandoned in state (ii).
- **CQ11.** A readback after an OK SendObject that differs in size, name
  or parent: (a) `MTPE_OK` with a flag or field saying so, or (b) another
  code (`MTPE_PROTOCOL`).
  **Decided** (Q25, owner, 2026-11-17; 7.9, "Readback"): (b), `MTPE_PROTOCOL` with `MTPR_READBACK_*`.
- **CQ12.** `MTPE_PROTOCOL`'s reason codes: (a) left to the code, the
  cases checking only that one is set, or (b) listed in this document
  before the library is written, the cases then naming each.
  **Answered** (7.9, "One code, and flags", the reason table): (b); every case names its reason.
- **CQ13.** A response read ended by the time-out because a full-speed
  device omitted the zero-length packet: (a) the bytes that arrived are
  the response and the call succeeds, or (b) `MTPE_TIMEOUT`.
  **Answered** (7.9, "Framing", the response read): (b) `MTPE_TIMEOUT`, then 7.5 step 1's stuck device (CQ35).
- **CQ14.** A revoked claim (5, same generation, no removal flag): (a)
  `MTPE_INUSE`, or (b) another code (`MTPE_GONE`, `MTPE_STOPPED`).
  **Answered** (7.9, "5, and the replacement check"): (a) `MTPE_INUSE` with `MTPF_SESSION_LOST`; only a new `MtpOpen` brings it back.
- **CQ15.** A job stopped by the close's latched cancel, or cancelled
  while queued and then dropped by the close's step 1: (a)
  `MTPE_CLOSING`, by the order of "One code, and flags", or (b)
  `MTPE_CANCELLED`.
  **Answered** (7.9, "Close" step 1; "One code, and flags"): (a) `MTPE_CLOSING`.
- **CQ16.** The first-sight warning's set and the exit sequence are the
  program's: (a) the host test builds that program code with the library
  (the cases drive it), or (b) the library carries them (`mtppower.c`)
  and the program only shows messages.
  **Answered** (7.9, "Program exit", the program's own source file): (a), kept as it stood; the host test compiles the program's file.
- **CQ17.** `MtpEnum` on a driver answering 40 to 43 bytes: (a) listed,
  with the open then refusing it, or (b) left out. (No case yet.)
  **Answered** (7.9, "Devices", `MtpEnum`): (a), listed with "cannot be opened"; the open refuses it.
- **CQ18.** The edit's cleanup EndEditObject: (a) sent by the library
  within the interrupted call, before it returns, or (b) the job's own
  `MtpAndroidEndEdit`, let through by name.
  **Answered** (7.9, "Edit lifetime", made exact): (a), the library's, inside the interrupted call; a later `MtpAndroidEndEdit` returns `MTPE_ARG` (`MTPR_ARG_EDIT`), nothing sent.
- **CQ19.** Automatic recovery whose I/O thread is stuck in a class
  request: the observer gives up at (a) 20 s (the cancel's 10 s and the
  reset's 10 s), or (b) 10 s.
  **Answered** (7.5, "One deadline", the outer bound; 7.9, "The deadline observer"): (a), 20 s from the procedure's start at the latest, step 6's own 10 s when entered.
- **CQ20.** A cancel in IDLE or RESPONSE: (a) no `ABORT` (7.5 runs only
  in COMMAND and the data phases), or (b) the cancel thread is signalled
  anyway and aborts quiet pipes (the handshake's step 3 ends "Then the
  cancel thread is signalled").
  **Answered** (7.5 step 1; 7.9, the handshake's step 3): (a), no `ABORT`; the cancel thread is not signalled.
- **CQ21.** The readback GetObjectInfo after an OK SendObject when the
  job's cancel flag is set: (a) part of the admitted operation, sent, or
  (b) a new transaction, refused.
  **Answered** (7.9, "Readback"): (b), refused; the upload returns `MTPE_OK` with `MTPF_UNVERIFIED`.
- **CQ22.** `MTP_RESULT`'s bytes moved: (a) the object's payload, or (b)
  the container's bytes, header included.
  **Answered** (7.9, "The bytes moved"): (a), object payload bytes; 0 for operations that move no object.
- **CQ23.** An endpoint parameter of 01h with bulk IN 81h and bulk OUT
  02h (or OUT 01h): (a) a bare number (bulk IN; or both when shared), or
  (b) a full OUT address (not this interface's: step 6; or the OUT pipe).
  **Answered** (7.5 step 4, the matching steps): (a), a bare number; bits 8 to 31 checked; a pipe named twice after resolution is repeated.
- **CQ24.** After 7.5's Device Reset: (a) `EVENTS` start at step 6.7
  before the new session, or (b) after OpenSession as 7.3 and 7.9 say,
  or both.
  **Answered** (7.3, "`EVENTS` start comes once"; 7.5 step 6.7 withdrawn): (b), once, after the new session's OpenSession.
- **CQ25.** The code of an operation ended by a bulk failure of no named
  Win32 code (recovered by 7.5), or by a source that underflows or fails:
  (a) `MTPE_STOPPED`, or (b) `MTPE_PROTOCOL` or `MTPE_DEVICE`.
  **Answered** (7.9, "The interface": `MTPE_SOURCE`, and "A bulk failure of no named Win32 code"): neither reading as written: a bulk failure is `MTPE_DEVICE` with the status code read for it (or 0), and a source's failure the new code `MTPE_SOURCE`.
- **CQ26.** A cancel in RESPONSE whose response is then lost: (a)
  `MTPE_TIMEOUT` (the cancel acted on nothing), or (b) `MTPE_CANCELLED`
  (the order).
  **Answered** (7.5 step 1; "One code, and flags"): (b) `MTPE_CANCELLED` with `MTPF_SESSION_LOST`; the reset runs.
- **CQ27.** Number not used.
- **CQ28.** OpenSession answered a code other than SessionAlreadyOpen or
  DeviceBusy: (a) Device Reset, then the replug message with no further
  OpenSession, or (b) one more OpenSession after the reset.
  **Answered** (7.3): (a), no further OpenSession; `MTPE_DEVICE` with the refused code.
- **CQ29.** "Neither, and the next call fails 5": (a) the stopped call's
  result waits for that call (`MTPE_GONE`), or (b) the stopped call
  returns `MTPE_STOPPED` and the device is found gone afterwards.
  **Answered** (7.9, "Classifying a stopped call"): (b) `MTPE_STOPPED` from the evidence at hand; the later 5 is classified when met.
- **CQ30.** An operation the device does not list (GetPartialObject64),
  or one outside the edit's rules (a second BeginEditObject, another
  handle, another operation inside the edit): (a) `MTPE_ARG`, nothing
  sent, or (b) another code.
  **Answered** (7.9, "Edit lifetime", made exact; the reason table): (a) `MTPE_ARG`, `MTPR_ARG_UNLISTED` or `MTPR_ARG_EDIT`, nothing sent.
- **CQ31.** `MtpGetPartial` below 4 GiB on a device that lists 95C1h: (a)
  GetPartialObject, or (b) GetPartialObject64.
  **Answered** (7.9, the transfers, `MtpGetPartial`): (b), 95C1h whenever listed, low offsets included.
- **CQ32.** `MtpDeviceInfo` inside a session: (a) the DeviceInfo kept from
  the open, or (b) GetDeviceInfo again with the session's next id. (No
  case yet.)
  **Answered** (7.4, DeviceInfoChanged): (a) the kept DeviceInfo, replaced by the refresh job's GetDeviceInfo after a DeviceInfoChanged event.
- **CQ33.** An OK response that fails its checks (too few parameters, or
  SendObjectInfo's not matching what was sent) for a mutating operation:
  (a) settled, no `MTPF_UNCERTAIN` (a complete OK was read), or (b)
  uncertain.
  **Answered** (7.9, "Ownership"): (a), settled; `MTPE_PROTOCOL` (`MTPR_RPARAMS` or `MTPR_RVALUE`), no ownership.
- **CQ34.** Number not used.
- **CQ35.** A response lost to a time-out with no cancel: (a) no
  recovery, the next transaction takes the next id in the same session,
  or (b) 7.5 runs (a stuck device: step 6, the session lost).
  **Answered** (7.5 step 1, "A response that does not come"): (b), step 6; old-session jobs dropped; nothing replayed.
- **CQ36.** Inside a folder's PartialDeletion, a child folder: (a) first
  tried as one DeleteObject, or (b) descended into at once.
  **Answered** (7.4, DeleteObject; 7.9, "Deleting a folder"): (a).
- **CQ37.** A deletion meeting its bounds (depth 64, 100,000 objects) or
  a refused handle (0, FFFFFFFFh, seen twice, a loop, not under the
  folder): (a) the whole deletion stops, with a code to be named, or (b)
  that handle is skipped and the rest go on.
  **Decided** (Q26, owner, 2026-11-17; 7.9, "Deleting a folder"): (a), the whole deletion stops, `MTPR_DELETE_*`.
- **CQ38.** A listing that fell back for a protocol error or a cap,
  rather than a refusal: (a) remembered for the session like a refusal,
  or (b) GetObjectPropList tried again for the next folder.
  **Answered** (7.4, "Fallback"): (b), only a refusal is remembered for the session.
- **CQ39.** A count that fails both the fit and the cap: (a)
  `MTPE_PROTOCOL`, or (b) `MTPE_TOOLARGE`.
  **Answered** (7.9, "Before a count is used"): (a) `MTPE_PROTOCOL` (`MTPR_COUNT`).
- **CQ40.** A character outside the BMP (a surrogate pair) in a name
  copied to the PC or shown: (a) one character, one "_" or "?", or (b)
  each unit apart.
  **Decided** (Q27, owner, 2026-11-17; 7.6): (a), one character.

## 13. User-visible strings (owner drafts)

INF:

- Device: "MTP or PTP device (wdm-free)"; listed devices (3.2.1): one
  name for all, `%Phone%`, "MTP device (wdm-free)" (owner, 2026-11-17,
  Q21; a draft; the
  earlier draft "<maker> <model> (MTP, wdm-free)" would add some 55 KB
  to the INF). (No by-hand model: 3.2.)
- Class: "Portable Devices". Provider: "usb-wdm-free". Start menu entry:
  "MTP Explorer".

Program:

- Title: "MTP Explorer (wdm-free)"; with a folder: "<folder> - MTP
  Explorer (wdm-free)".
- Menus:
  - File: Copy to PC..., Copy from PC..., Open, New Folder, Delete,
    Rename, Properties, Close;
  - Edit: Select All;
  - View: Large Icons, Small Icons, List, Details, Refresh;
  - Help: About MTP Explorer.
- Toolbar tips: Up, Copy to PC, Copy from PC, New Folder, Delete,
  Properties, Views.
- Columns: Name, Size, Type, Modified.
- Status: "%d objects", "%s: %s free of %s".
- Tree, when there is no device: "No phone or camera is connected.
  Connect one and choose File transfer (MTP) or PTP on it."
- Tree, device states: "No storage. Unlock the phone and choose File
  transfer on it."; "In use by another program"; "The device stopped
  responding. Disconnect it and connect it again."; "This folder is too
  large to list."
- Progress: "Copying %s", "File %d of %d", "%s of %s (%s/s)", "About %s
  left", Cancel, "Cancelling...".
- Clash: "A file named \"%s\" already exists in this folder." Replace,
  Replace All, Skip, Cancel.
- Delete: "Delete \"%s\" from the device?" or "Delete these %d items from
  the device?", "This folder and everything in it will be deleted."
- Open: "The file is opened from a copy. Changes you make are not sent
  back to the device." with "Do not show this again".
- Start menu offer (8.6, way 2): "Add MTP Explorer to the Start menu?"
- Drag out (8.3): "Preparing %d files for Explorer", "%s of %s", Cancel;
  "Ready to drag: drag the files again"; "There is not enough free space
  on %s to prepare these files (%s needed). Use Copy to PC instead."
- Replace on the PC (7.8): "Your original file is now %s; the copied
  file is %s."; "The copied file was kept as %s."; "The backup of the
  original file was kept as %s."
- Errors:
  - "The device was disconnected."
  - "\"%s\" is %s, too large for this disk (at most %s per file)."
  - "There is not enough free space on %s."
  - "The device refused the operation (%04Xh)."
  - "The device sent data this program cannot read (%s)."
  - "This storage is read-only."
  - "Copying was cancelled."
  - A job found stopped while the driver's power gate was closed
    (`MTPE_POWER`, 7.9; Q22; revised twice: a refused standby or another
    component's power change is not Windows going on standby, and the
    library knows only that the gate was closed when the call stopped):
    "%s was interrupted while the phone's driver was handling a power
    change." (%s the operation: "Copying", "Deleting", "Listing the
    folder"); with `MTPF_STANDBY`, "...handling a standby." instead.
    With `MTPF_UNCERTAIN`: "\"%s\" may be incomplete or already changed
    on the phone; check it before you try again."
  - A job stopped with no known cause (`MTPE_STOPPED`, 7.9): "%s was
    interrupted.", with the same uncertainty sentence when it applies.
  - An upload whose readback differs (`MTPR_READBACK_*`, 7.9; Q25;
    draft): "\"%s\" was sent, but the phone reports it differently;
    check it on the phone."
  - A deletion stopped at its bounds or a refused handle
    (`MTPR_DELETE_*`, 7.9; Q26; draft): "%d of %d items were deleted.
    The device sent a folder listing this program cannot use (%s)."
  - A file on the PC that could not be read while it was sent
    (`MTPE_SOURCE`, 7.9; draft): "\"%s\" could not be read on this PC, so
    it was not copied completely.", with the same uncertainty sentence.
  - An upload whose check was not made (`MTPF_UNVERIFIED`, 7.9; draft):
    "\"%s\" was copied, but this program could not check it on the
    phone."
  - Under Q19 (a)'s operation-level answer, after either: "%d of %d
    files were copied before the interruption."
  - "\"%s\" was not copied: its name cannot be used on this disk."
  - "This device does not answer as a phone or camera (PTP/MTP)."
  - A query refused by the driver (5.4, "What user mode can read";
    drafts of 2026-11-17, revised the same day, for the owner; each says
    only what the driver knows): when the refusal count differs from the
    program's baseline: "USBMTP.SYS refused a power change because
    requests to the phone did not finish stopping." (revised: the count
    covers device queries as well as standby, 7.9). It says nothing of
    any transfer: whether a job was stopped, and whether its outcome is
    uncertain, is that job's own message (above), from its own result.
    While the device is draining (gate bit 8): "Requests to the phone
    have not finished stopping. The phone cannot be used until they
    do." Neither says that Windows stayed up, that a transfer was
    stopped, or that disconnecting the phone will help (REMOVE waits for
    those same requests, 5.4).
  - On 98 SE only, after the first of those two (Q18, decided; Q19;
    draft of 2026-11-17 for the owner): "On Windows 98 SE, restart
    Windows before you use a serial port (a modem or another serial
    device): until then, a program that opens one may stop responding.
    Save your work first." On ME the sentence is left out (Q19).
  - On 98 SE only, a count already nonzero at first sight (7.9; Q19
    (d); draft): "Since this phone was connected, USBMTP.SYS has refused
    at least one power change. If Windows said that it could not go on
    standby, restart Windows before you use a serial port (a modem or
    another serial device). Save your work first."
- The program's own refusal of a standby (7.9; Q19 (b), owner,
  2026-11-17):
  status bar, "Standby was refused because MTP Explorer was working
  with the phone." (revised: the work may be a listing, a recovery, an
  open or a close, not only a copy). No dialog of the program's own:
  Windows shows its box.
- A command given while the program held the device for a power change
  (7.9, the hold; `MTPE_HELD`, or dropped with the old session): "Not
  done because of a power change. Try again."
  (revised: the hold may end with no standby at all).
- The hold past 10 s, under Q23's (A) (7.9): status line, "Waiting for
  Windows to finish a power change.", with a button "Use the device
  now". `MTPEXP /c`, held past `/powerwait`: "Not done: Windows did not
  finish a power change."
- Summary: "%d files copied. These names were changed for Windows:" and
  a list.
- About: "MTP Explorer %s (wdm-free). Free software, GPL-2.0-or-later.
  https://github.com/yeokm1/usb-wdm-free"

Readme (`README.TXT`), first lines: "MTP Explorer and USBMTP.SYS: copy
files to and from phones and cameras (MTP and PTP) on Windows 98 SE and
ME. Point the Add New Hardware Wizard at this folder when the phone or
camera is first connected; on a phone, choose File transfer (MTP) or PTP
first." Later lines: the Kodak cameras ME keeps (3.2), unsplit composites
(3.3), the `~MTP*.TMP` and `~MTP*.OLD` files (7.8), "If your phone is
not listed" (3.2.1, drafted there), and on 98 SE the restart before a
serial port after the driver refused a standby (Q19).

## 14. Owner decisions and open questions

Owner, 2026-11-16, on the questions of the first pass:

- **Q1. Transport:** our own driver `USBMTP.SYS`, not `usbscan.sys`
  (section 4).
- **Q2. Phones whose MTP interface is vendor class `FF`:** model lines for
  listed VID/PIDs (3.2). The by-hand model is dropped: M0's Have Disk
  reading showed a pick does not bind on 98 SE.
- **Q3. Device Manager class** and **Q4. names and texts:** not decided
  one by one. The drafts stand: a class "Portable Devices" of the
  package's own, with its own GUID, not Windows' WPD GUID (9); package
  `usbmtp`, `USBMTP.SYS`, "MTP Explorer" `MTPEXP.EXE`, `src/image/`; and
  the strings of section 13.
- **Q5. Install:** the INF copies the program and adds the Start menu
  entry (8.6), shown by the legs.
- **Q6. Start at plug-in:** no.
- **Q7. Drag out to Explorer:** in the first version, beside the drop
  in. The mechanism is the design's: staged real files (8.3).
- **Q8. A file too large for the PC's disk:** refused, with a message
  (7.8).
- **Q9. Systems:** 98 SE and ME only (owner, 2026-11-16).
- **Q10. Open:** included (8.2).
- **Q11. `STAGE.TXT`:** `USBMTP.INF` is staged on 98 SE and ME (9).
- **Downloads:** approved (7.1).
- **The QEMU build** of section 11.1 goes ahead.

### Open questions

- **Q12. ME's EHCI leg:** answered (owner, 2026-11-16): ME may be called
  ready and released on its xHCI98 half alone; its USB 2.0 half is
  recorded as not tested, and the user documents say so (11.5).
- **Q13. Microsoft OS descriptors:** the driver does not read them (3.3).
  Microsoft's downloadable specification comes under a licence agreement
  whose grant for software drivers names only Windows Vista and 7. Should
  the owner want them read, for example so that a class `FF` phone could
  be told from other vendor devices without a model line, the licence
  question is his to settle first. Recommended: leave them unread.
- **xHCI98:** does its hub report a Microsoft OS descriptor compatible id
  (`USB\MS_COMP_MTP`) on 98 SE and ME? A question only; nothing here
  depends on it.
- **Q14. A write leg on the bench phone** (7.9): may a write leg run on
  the bench phone, with exactly these operations: SendObjectInfo,
  SendObject, and DeleteObject of the leg's own objects; in a folder the
  leg creates, on backed-up, disposable data; each upload read back, and
  the folder's other contents compared before and after; and only after
  the interrupted and uncertain outcomes have run on `simdev.c` (7.9,
  "Before any write leg on a real phone")? Android's partial writes are
  not in it (Q17). Until then every real-phone run only reads.
  **Decided (owner, 2026-11-17): yes, with exactly these limits.** No
  run touches the phone for writing until that leg is started as such.
- **Q15. Strings without their terminating NUL** (7.9): accepted, with a
  trace line (recommended), or refused? Either answer sits inside 7.9's
  bounded decoding: the counted string is consumed whole, a terminator is
  added, and malformed UTF-16 decodes the same way every time.
  **Decided (owner, 2026-11-17): accepted, with a trace line.**
- **Q16. Real datasets as fixtures** (7.9): may the bench phone's
  GetDeviceInfo and StorageInfo be tracked under `testdata/`, its serial
  number replaced, the rest checked for personal data, and the capture
  recorded with the fixture?
  **Decided (owner, 2026-11-17): yes, with the serial number replaced
  and the rest checked for personal data; the fixture's readable
  fields are shown to the owner before it is committed.**
- **Q17. Android's operations** (7.9), two questions:
  - GetPartialObject64 (95C1h, read only): in the library only for now
    (recommended), or used by the program (a resumed copy)?
  - SendPartialObject, TruncateObject, BeginEditObject and EndEditObject
    (95C2h to 95C5h, which change an object in place): kept disabled on
    real devices (recommended) until a leg qualifies them on a device,
    with the forced split header, a write at a nonzero offset and the
    untouched bytes before and after it compared?
  **Decided (owner, 2026-11-17): both as recommended** (GetPartialObject64
  in the library only for now; 95C2h to 95C5h kept disabled on real
  devices until a leg qualifies them).
- **Q18. Refusing a standby on 98 SE** (5.4, "A system query below
  working"): the driver refuses a system query only when what the gate
  stopped has not come back within `QUIET_QUERY_MS` (a stack keeping a
  cancelled request; under the emulator never, with these bounds). On 98
  SE, in the guests tested, no program could open a COM port after such
  a refusal for as long as the opens were observed (20 to 220 s; 14 of
  14 guests, both stacks), and a restart restored access in the two
  guests where one was tried (5.4; the load record, "2026-11-17,
  follow-up").
  The options, with what the user meets in each:
  - (a) keep the refusal on 98 SE as it is: the message box, no standby;
    Windows stays running, so open work can be saved (the transfer has
    already been interrupted: 5.4). Then, as in the tested guests, no
    program can open a serial port until a restart, and a program that
    depends on one (a modem, a serial device) may itself become
    unusable; ports already open kept working, and a printer port
    opened in the legs;
  - (b) never refuse on 98 SE (accept at the bound): the set follows,
    waits `QUIET_SET_MS` and goes down with the stuck request
    quarantined; 98 SE removes the device at the sleep and REMOVE waits
    for that request without a bound, so the hang moves into the standby:
    the machine may not finish going to sleep or come back, and the user
    must switch it off, losing unsaved work in every program. That is the
    failure the standby rework was made to remove (one standby in five
    during a transfer under NUSB's EHCI with the earlier driver);
  - (c) the rule now (refuse only when a request is stuck beyond the
    bound) and document the side effect: the code of (a), with the user
    documents and the program's message (13) saying that on 98 SE a
    restart is needed after this message before a serial port is used;
  - (d) refuse on ME only (ME's ports kept working after a refusal, one
    guest): 98 SE takes (b)'s risk, and the driver must tell 98 SE from
    ME at run time by a test still to be read on both.
  With any of them, outside the driver: the program could refuse a
  standby itself while a transfer runs (`BROADCAST_QUERY_DENY`), which on
  98 SE brings the same message box and left the ports working (four
  guests); it covers only the time the program runs, and it changes
  what a standby during a transfer does now (interrupted, then
  recovered: 5.4, 7.9). Recommended: (c), no driver change, the program's own
  refusal during a transfer weighed at the library stage.

  **Decided (owner, 2026-11-17): (c), with the program's own refusal as
  the normal path.** The program refuses a standby itself while a
  transfer runs; the driver's refusal stays as it is, as the backstop
  for when the program is not running or its refusal is not enough; and
  the user documents and the program's message say that on 98 SE, after
  the driver had to refuse, a restart is needed before a serial port is
  used. No driver change. The program's refusal is designed at the
  library and program stage.
- **Q19. What the user meets with the program's refusal** (7.9,
  "Standby: the program refuses during a transfer"; strings in 13).
  **Decided (owner, 2026-11-17): every recommendation below** ((a) the
  whole job, (b) a status line, (c) the 98 SE restart sentence, left out
  on ME, (d) the conservative first-sight warning on 98 SE); the strings
  stay drafts. The options as they were put:
  - (a) Standby is refused for a whole job (every file of a copy, a
    whole staging or deletion: recommended) or only while a library
    operation is in flight (a standby between two files is then agreed;
    the job stops before its next operation, keeps what was completed,
    and reports how far it got; its own admission boundary and
    interrupted-job rules are in 7.9, "operation-level protection").
  - (b) When the program refuses: nothing of its own beyond Windows' box,
    or a status bar line (recommended; drafted in 13), or a dialog (not
    recommended: Windows already shows one).
  - (c) After an identifiable refusal by the driver: the 98 SE message
    with the serial sentence as drafted in 13; on ME the same message
    without it (recommended, by the one ME guest whose ports kept
    working), or with it on both.
  - (d) A refused-query count already nonzero when the program first
    sees a device on 98 SE: a conservative warning, worded so that it
    does not claim the refusal happened while the program was away (13),
    shown once per device object and count within one run of the
    program, and again in a later run while the count stays nonzero
    (recommended), or not at all. It does not cover a refusal before the
    device was plugged in again (7.9, "A count already nonzero at first
    sight").
  - The readme's line (draft): "On Windows 98 SE: if Windows says it
    cannot go on standby and MTP Explorer says that the driver refused
    it, save your work and restart Windows before you use a modem or
    another serial device."
- **Q20. The device list** (3.2.1): decided (owner, 2026-11-16 and
  2026-11-17). A large list converted from libmtp's device list ships in
  `USBMTP.INF`; a device not in it is added by hand by the readme's
  instruction, with no function in the program. One INF, its
  libmtp-derived lines converted to the GPL by LGPL-2.1 section 3 (the
  second, LGPL INF and its reading fall away); the table's licence line
  `LGPL-2.1-or-later`; a copy of the table in the release folder beside
  the INF; rule R3 (the risky composites left out, about 1,363 rows);
  the readme instruction as drafted, corrected after its leg; the probe
  tool shipped, so that reports can carry its `/pnp` output.
- **Q21. What the user meets with the device list** (3.2.1; 9; 13).
  **Decided (owner, 2026-11-17): every recommendation below** ((a) one
  Device Manager name, "MTP device (wdm-free)"; (b) the probe shipped as
  a `/pnp`-only build; (c) `MTPIDS.TXT`, `LGPL21.TXT`, `NOTICE.TXT`; (d)
  the probe's report in the readme's steps); the strings and the readme
  text stay drafts. The options as they were put:
  - (a) The name listed devices show in Device Manager: one name for
    all, "MTP device (wdm-free)" (recommended; the INF stays within the
    provisional cap), or per-model names (about 55 KB more, past it).
  - (b) The shipped probe: the same `MTPPROBE.EXE` with `/drv` left in
    (it fails at once without `MTPPRB.SYS`), or built with `/pnp` only
    and its report written beside it instead of to `A:\`
    (recommended: a user's machine has no reason to reach the M0 tests,
    and `A:\` is a floppy drive).
  - (c) The release folder's added files: `MTPIDS.TXT` (the table's
    copy), `LGPL21.TXT`, `NOTICE.TXT` naming libmtp's authors and the
    section 3 statement (recommended names), and `MTPPROBE.EXE`.
  - (d) The readme's steps 2 and 3: `regedit` as drafted, or the probe's
    `/pnp` report, which shows every interface's class under every
    stack, so a `FF/FF/00` interface can be picked out as a candidate,
    where the drafted "`DeviceDesc` says MTP" holds only under xHCI98
    (recommended: the probe, the text then read by its leg). The class
    does not make an interface MTP: the readme says the line is tried,
    and the driver's checks and the program show whether it works.
- **Q22. An operation stopped by a power change** (7.9, "Interface
  1.1", "One code, and flags"; 13). **Decided (owner, 2026-11-17): the
  separate power-change interruption message** (the recommendation; the
  string stays a draft). As it was put: the library tells it apart from a
  cancel (`MTPE_POWER`, with `MTPF_STANDBY` when the gate showed a
  standby and `MTPF_UNCERTAIN` when a mutating operation may have
  acted; the cause is inferred from the gate being closed when the call
  stopped, not proven): the program says so with the drafted message,
  "%s was interrupted while the phone's driver was handling a power
  change.", with "a standby" and the uncertainty sentence only when they
  are known (recommended), or shows a generic interruption, "%s was
  interrupted." (the `MTPE_STOPPED` message), with the same uncertainty
  sentence. "Copying was cancelled." is not offered: nobody cancelled.
- **Q23. When the program's standby hold ends without a notice from
  Windows** (7.9, "The hold ends"). **Decided (owner, 2026-11-17): (A),
  held until an explicit recovery action**; the strings stay drafts. As
  it was put: after the program has agreed to a
  standby, Windows normally sends a failure or a resume notice (it did
  in every standby read); if neither comes, the program must either
  keep the device held or reopen it on a time-out:
  - (A) keep it held until an explicit recovery action: after 10 s a
    status line says the program is waiting for Windows to finish a power
    change, with a button to use the devices now (it ends the hold for
    every device of the program); commands wait; an open waits up to its
    deadline and then fails with nothing sent; the command-line mode
    gives up after `/powerwait` and closes without sending anything.
    During the hold the program starts no new transaction, open or name
    read and sends no CloseSession (7.9, "What a hold keeps from the
    device"). After a successful agreement nothing of the program's is
    out, so for this program's own traffic the serial-port risk of
    Q18(c) is taken only after the user's act. A hold entered by a
    suspend Windows did not first ask about stops new work only: a job
    already running then goes on until the driver's query or set stops
    it, under Q18(c)'s backstop. It does not cover
    another client, the time after the program has exited (past its
    graceful budget the process ends and the driver's refusal is the
    backstop), or a request already stuck in the driver (during which
    the program refuses rather than agrees). Cost: devices that wait for
    a click in a case never seen;
  - (B) reopen it on a time-out (10 s after the agreement, 30 s after
    `PBT_APMSUSPEND`) as a heuristic: no click, but a job started after
    the time-out may meet the driver's query if Windows was only slow,
    and then Q18(c) applies, with its serial-port risk on 98 SE, without
    the user having chosen it.

  Recommended, and decided: (A), since the program's refusal exists to
  keep a transfer out of the driver's query, and (B) gives that up
  silently in exactly the slow case. (B) stays described in 7.9 as the
  alternative that was weighed, not as a shipping behaviour.
- **Q24. An upload that fails after SendObjectInfo's OK and before
  SendObject's data** (7.9, "Uploads and deletion"; 12.1, CQ10). After an
  OK SendObjectInfo the device holds a reserved object (some devices list
  it at once, empty). Then the upload fails in one of three states (an
  OK whose parameters fail their checks establishes no reservation the
  library owns, CQ33, and is not in this question):
  - **(i) SendObject's command was never submitted**: a cancel between
    the pair (the latch refuses SendObject), a source that fails before
    any byte, the device given up, held or its session lost before
    SendObject's admission;
  - **(ii) SendObject's command was submitted, its data not yet**: a
    cancel or failure in COMMAND, or the command's write ending without
    its count published. The command may have reached the device;
  - **(iii) SendObject refused by a complete response before any data**
    (a device that answers at once, store full): settled, nothing of
    the object written, the reservation still there.

  The two answers, each designed far enough to implement:
  - (a) **abandon**: the library sends nothing more for the pair. The
    result is the failure's code with `MTPF_UNCERTAIN` (in states (i)
    and (iii) also, since the reservation exists on the device) and the
    reserved
    handle in the result for the program to show. The user sees the
    uncertainty sentence of 13 ("\"%s\" may be incomplete or already
    changed on the phone; check it before you try again."), and on a
    device that lists reservations an empty file of that name after the
    next refresh, which they delete themselves if they wish;
  - (b) **delete the reserved handle once**: DeleteObject with that
    handle, as the pair's cleanup transaction, admitted like the Android
    edit's cleanup (the same session and cache generation, 7.5 at step 5
    with no Device Reset, not HELD, not GIVENUP, CLOSING allowed; the
    one further exception to the cancel flag's refusal, let through by
    name). An OK leaves no trace on the device and the result is the
    failure's code without `MTPF_UNCERTAIN`; the user sees only that the
    copy was cancelled or failed. A refused or lost DeleteObject, or one
    not admitted, falls back to (a)'s result and message, and is never
    sent again.

  Recommended: (a) for state (ii), since the command may have reached the
  device and a delete could then race a write it started; for states (i)
  and (iii), (b), which keeps the earlier design's "only a failure before
  SendObject's data phase began lets the library delete the reserved
  handle" for the states in which nothing of SendObject can be under way
  on the device. Cases waiting on this: OPS-010.
  **Decided (owner, 2026-11-17): as recommended, by state** ((b) in
  states (i) and (iii), (a) in state (ii)).
- **Q25. A readback that differs** (7.9, "Readback"; 12.1, CQ11). After
  an OK SendObject the library reads the new object's ObjectInfo, and the
  size, name or parent differ from what was sent. The upload itself was
  settled by the OK; in either answer nothing is sent again or deleted:
  - (a) **success with a mark**: `MTPE_OK`, the handle, the payload
    bytes, and a flag `MTPF_MISMATCH` with the fields that differ in the
    result. The program counts the file as copied and lists it in the
    copy's summary: "\"%s\" was copied, but the phone reports a different
    %s." (size, name or folder; a draft);
  - (b) **failed verification**: `MTPE_PROTOCOL` with a reason naming
    the field (`MTPR_READBACK_SIZE`, `_NAME`, `_PARENT`), no
    `MTPF_UNCERTAIN`, the handle still returned. The program reports the
    file as not copied correctly: "\"%s\" was sent, but the phone reports
    it differently; check it on the phone." (a draft), and leaves the
    object in place.

  Recommended: (b), since a size or parent that differs is the device
  disagreeing with what was sent, and a copy should not be counted as
  good on that evidence. Cases waiting on this: OPS-013 (in part).
  **Decided (owner, 2026-11-17): (b), failed verification.**
- **Q26. A deletion that meets its bounds or a refused handle** (7.9,
  "Deleting a folder"; 12.1, CQ37): depth 64, 100,000 objects, or a
  handle of 0 or FFFFFFFFh, one seen twice, a loop or one not under the
  folder chosen:
  - (a) **stop the whole deletion**: nothing further is deleted, since
    the traversal's data is suspect; `MTPE_PROTOCOL` with
    `MTPR_DELETE_BOUND` or `MTPR_DELETE_HANDLE` (two reasons added to
    7.9's table under this answer), and the count of objects
    already deleted in the result. The user sees "%d of %d items were
    deleted. The device sent a folder listing this program cannot use
    (%s)." (a draft); the folder chosen and what is left stay on the
    device;
  - (b) **skip that handle**: the handle (and anything under it) is left
    out and the rest is deleted; the folder chosen is then not empty, its
    last DeleteObject is refused, and the deletion is reported as
    refused, with the skipped handles traced. The user sees "Some items
    could not be deleted." (a draft).

  Recommended: (a): deleting more on the strength of a listing already
  shown to be inconsistent is the riskier act. Cases waiting on this:
  OPS-019 (in part).
  **Decided (owner, 2026-11-17): (a), stop the whole deletion.**
- **Q27. A character outside the BMP in a name** (7.6; 12.1, CQ40): a
  surrogate pair such as U+1F600 in a device name, on 98 SE and ME, whose
  code pages have no such character:
  - (a) **one character**: the pair is one Unicode character for 7.6's
    rules: one `?` when shown, one `_` when copied to the PC; the pair is
    kept as such in the library's copy of the name, and the Properties
    dialog shows `U+1F600`. A name "a<U+1F600>.jpg" shows as "a?.jpg" and is
    copied as "a_.jpg";
  - (b) **two units**: each UTF-16 unit is mapped apart, as a plain
    unit-by-unit conversion does: "a??.jpg" shown, "a__.jpg" copied, and
    Properties shows `U+D83D U+DE00`.

  An unpaired surrogate is U+FFFD in both (7.9, "Strings"). Recommended:
  (a), one character per Unicode scalar, as the user would count them.
  Either visibly changes the names of such files. Cases waiting on this:
  NAM-001 (row g), NAM-007 (row c).
  **Decided (owner, 2026-11-17): (a), one character.**

## 15. Unverified, and what would settle it

| Claim | Settled by |
|---|---|
| Current Android presents MTP as `06/01/01`; older phones as `FF/FF/00` | the bench phones' descriptors; Android's sources |
| Stock 98 SE's generic parent and ME's composite parent give `&MI_nn` nodes as documented (3.1); which configuration descriptor an interface node gets, and which selection its parent takes (3.3) | M0, every stack; the install media's file lists |
| How each stack retires a cancelled bulk, interrupt, control and pipe reset URB, and whether an aborted bulk IN's bytes are exact (5.6, item 5); so which bounds 5.4 may promise | M0 |
| The 4-byte size of Get Device Status parameters (7.5; the still image specification leaves it open) | the bench devices; libgphoto2 and libmtp do not parse them |
| The order of cancel, cleanup and close with calls outstanding, including process termination (5.1, 5.6) | M0 |
| Devices that split the header do so as MTP 1.1 Appendix H requires (every data phase, both directions) | the bench devices; the `split` profile follows the specification |
| A target exists that reads no file before the drop, which drop-time filling would need (8.3) | M2, only if that optimisation is pursued |
| ME's WIA leaves a `06/01/01` device bound to our driver alone | not read; nothing relies on it (3.2) |
| 98 SE's and ME's Have Disk offer a model whose ids do not match | 98 SE lists it but the pick does not bind (M0); ME not read; no by-hand model (3.2) |
| `IoGetDeviceProperty(DevicePropertyHardwareID)` returns the `&MI_nn` id on every stack | M0 |
| An `ABORT` on a second handle completes while a read blocks on the first, on 98 SE's and ME's NTKERN; the Win32 errors and byte counts; cleanup on exit and on End Task (5.6) | M0 |
| Android answers GetObjectPropList with depth 1; sends StoreAdded when unlocked; keeps the date sent in SendObjectInfo | the bench |
| A device keeps a partial object after a cancelled SendObject | the bench; the simulator does both |
| 98 SE's and ME's FAT32 code takes files up to 2^32 - 1 bytes | M4 (the 4 GiB - 2 ceiling is used until then) |
| 98 SE and ME USBD accept a 64 KiB `MaximumTransferSize` | M1 ([printing.md](printing.md) 17 had the same reading open) |
| 98 SE and ME send power IRPs to a `DO_POWER_PAGABLE` device at `PASSIVE_LEVEL`, so the late hand-off never runs (5.4, "Power") | no `power-late` line in any standby leg of [2026-11-16-mtp-m1-load.md](../testing/2026-11-16-mtp-m1-load.md) (98 SE under both stacks, ME under xHCI98) |
| Without a D3 from the driver, the 98 SE stacks remove the device as the system goes to sleep, and the guest comes back from the standby (5.4, "Power") | the standby legs of [2026-11-16-mtp-m1-load.md](../testing/2026-11-16-mtp-m1-load.md) with the driver of 2026-11-17: removal on 98 SE under xHCI98 and NUSB's EHCI; on ME under xHCI98 the device is kept (another component's D3 and D0); every guest came back except one standby under NUSB's EHCI during a transfer (next row) |
| The stack returns the driver's cancels during a standby, so the wait for quiet ends (5.4, the third dependency) | read failing once in five standbys during a transfer under NUSB's EHCI; the held request not identified; not settled. Since the standby rework (5.4) a failure costs a refused standby or a quarantined request, not a held system IRP. With the rework in the driver (`0525a8e`), 11 standbys under NUSB's EHCI on fresh guests (10 during a transfer, a bulk IN held): every cancel at the query came back, every query was accepted, every system slept, 0 hangs (the load record); emulated, so a stack that keeps a request is still not exercised |
| What 98 SE and ME do when a driver refuses a system query (a message, silence, a retry), and whether they send S0, a set or nothing after it or after another device's refusal (5.4, "Standby") | read in the rework's guest legs with the test-only zero-bound build (the load record): a message box on each (5.4, "A system query below working"), no set, S0 or retry after our refusal; after another device's refusal the accepted device had already had its set (ME then S0 and D0; 98 SE removed and found it again). The lapse after `QUERY_HOLD_MS` was not reached and stays a local recovery heuristic for a system that sends nothing. Both send programs `PBT_APMSUSPEND` before our query and `PBT_APMRESUMESUSPEND` after our refusal; after it, in the guests tested, 98 SE opened no COM port for as long as the opens were observed (20 to 220 s; 14 of 14 guests, both stacks) and a restart restored access (two guests), after a program's refusal it does (the follow-up of the load record); ME's ports kept working (one guest). Why 98 SE's ports stop opening is not read: an incomplete rollback of the serial path's power preparation after a late refusal is a hypothesis (5.4), with the serial driver's notifications and state not observed; a refusal by another WDM driver was not available to compare |
| Whether another component sends a device query before its D3 on ME, and what it does after a refusal (5.4, "Another component's device query") | no device query seen in any standby leg, the rework's ME legs included (the trace logs every power IRP); not settled for other components |
| A submission committed before the gate closed returns from its `IoCallDriver` promptly, so a set's wait for it is short (5.4, "The submission") | not timed; in the rework's legs no `power-down-calling` line (a wait of a second or more) came, and every set found quiet; a dispatch that never returns is not bounded by the driver |
| An accepted query is followed by its set well within `QUERY_HOLD_MS` on 98 SE and ME, and the stacks return the gate's cancels within `QUIET_QUERY_MS` where they return them at all (5.4, the bounds) | under the emulator (the load record, the rework's legs): every cancel came back within 41 ms where the trace times the query's wait, and the set came after every accepted query in a slept standby, no 250 ms reading of the watch catching the gate between them (the trace has no timestamps); no real stack read; the bounds are choices |
| After another component's D3 during a standby, REMOVE's pass returns (5.4, "Power") | ME under xHCI98, once (the unplug after the wake); the removal during the sleep, before the D0, not read |
| A pending pipe reset is retired by `IoCancelIrp` | not run in M0; until read, a reset no stack retires holds the waits for quiet (5.4) |
| A program's `BROADCAST_QUERY_DENY` on ME ends the standby before any driver query, as on 98 SE (7.9, "Standby: the program refuses during a transfer") | the program's standby legs on ME (11.5); read on 98 SE only (four guests, the load record) |
| The program's own hidden window receives the broadcasts, also during a modal dialog of its window thread; the system's idle standby sends the same query; how long 98 SE and ME wait for a program's answer (7.9) | the program's standby legs; nothing relies on the wait |
| A device install runs `UpdateInis`; the program can add the entry itself (8.6) | M1 |
| A whole-device line on a composite takes the parent under 98 SE's USB 2.0 stack and matches nothing under xHCI98 (3.2.1) | L1 |
| 98 SE's and ME's setup read an INF of R3's size (about 55 KB) whole, and find a line anywhere in it (3.2.1) | L2; the 60 KiB cap is provisional until then |
| libmtp's rows name devices whose interface shapes R3's rule guesses right (3.2.1) | not read; per device, by users' reports with the probe's output and the bench (L3) |
| The readme's steps for an unlisted phone (the `Enum\USB` keys, their `DeviceDesc`, the undo) (3.2.1) | their leg with the simulator's composite; a real phone on the bench |
| `simdev.c` and `hostredir.c` behave as the driver does at its interface (7.9) | the guest legs of 12, "M1's library stage"; a difference found there corrects the imitation |
| ME's USB 2.0 half | not tested; ME is released on xHCI98 alone (owner, 2026-11-16) |
| The sizes of section 10 | estimates |

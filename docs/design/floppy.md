# USB floppy drives (UFI): an INF over the in-box storage driver

Status: design draft, 2026-10-20. No code yet.

## 1. Purpose and devices served

USB floppy drives are UFI devices: mass storage class `08`, subclass `04`
("UFI: Specifies how to interface Floppy Disk Drives to USB", USB-IF Mass
Storage Specification Overview 1.4, table 1). Almost all of them use the
CBI transport, protocol `00` (with command completion interrupt) or `01`
(without) (same document, table 2). The CBI specification limits CBI to
this one device type: "This specification is approved for use only with
full-speed floppy disk drives. CBI shall not be used in high-speed capable
devices, or in devices other than floppy disk drives" (USB MSC CBI
Transport 1.1, section 1.1).

The drives sold today are OEM units from TEAC, Y-E Data, NEC, Mitsumi,
Samsung, Citizen and others, sold under many brands. Every Microsoft system
from ME to 8 has a storage driver, `usbstor.sys`, that can run a CBI
floppy. None of them binds that driver to a floppy by class. Each binds
only a short list of drives by VID/PID (section 2). A drive not on the list
gets no driver. The user sees "Unknown device" or the New Hardware wizard,
and the system has nothing to offer.

This package is an INF and nothing else (Owner, 2026-10-20: INF-only
packages that bind Microsoft's in-box drivers are allowed when they fill a
gap Windows leaves; `AGENTS.md`, "Sources"). It matches
`USB\Class_08&SubClass_04&Prot_00` and `Prot_01` (and later `Prot_50`,
section 6.1) and binds `usbstor.sys` with the same settings Microsoft's
own INF uses for the floppies it lists. It ships no binary, Microsoft's or
ours.

Devices served: any single-function UFI floppy drive, or a UFI function
split off a composite device, on Windows 98 SE (once a USB storage driver
set for 98 SE is installed, section 5), ME, 2000, XP (x86 and x64), Vista,
7 and 8 (x86 and x64).
LS-120/LS-240 SuperDisk and Zip drives are not UFI. They use subclass `02`
or `05`/`06` and are out of scope.

## 2. What each system has

How this was read: the INF text from each system's install medium in
`D:\isos`, extracted with 7-Zip. No binary was opened beyond reading its
version resource and hashing it.

- **ME:** `WIN9X\PRECOPY1.CAB` (`usbstor.inf`, `usbntmap.inf`,
  `ntmap.inf`) and `BASE2.CAB` (the binaries' names and sizes), from
  `Windows Me OEM Full.iso`.
- **2000 SP4:** `I386\USBSTOR.IN_`, `FLPYDISK.IN_` and `TXTSETUP.SIF`, from
  `win2ksp4.ISO`.
- **XP SP3:** the same files, from `Win XP Pro SP3 OEM original.iso`.
- **XP x64 SP2:** `AMD64\USBSTOR.IN_` and `FLPYDISK.IN_`, from
  `Win XP SP2 VL x64.iso`.
- **Vista SP2 x86:** `Windows\System32\DriverStore\FileRepository\usbstor.inf_72a6a3e5\usbstor.inf`
  in `sources\install.wim` of `en_windows_vista_sp2_x86_dvd_342266.iso`.
- **7 SP1 x64:** `Windows\inf\usbstor.inf` in the install image of
  `en_windows_7_professional_with_sp1_vl_build_x64_dvd_u_677791.iso`.
  A 7 SP1 x86 `usbstor.inf` extracted earlier from 7 x86 install media
  has the same class and floppy lines.
- **8 x86:** `Windows\inf\usbstor.inf` and `flpydisk.inf` in the install
  image of `en_windows_8_pro_vl_x86_dvd_917830.iso`.
- **98 SE:** every `WIN98\*.CAB` of `Win98SE.iso` was listed. None holds
  `usbstor.sys`, `usbntmap.sys` or `usbmphlp.pdr`.

Not read: Vista x64, 7 x86 from its ISO, and 8 x64. Section 11 lists them.

### 2.1 How `usbstor.inf` binds a drive, on every system

Every `usbstor.inf` read has three install sections, one per transport.
Each sets one value, `DriverFlags`, in the driver key:

| Section | `DriverFlags` | Transport, as the section name says |
|---|---|---|
| `USBSTOR_BULK` | `1` | Bulk-Only |
| `USBSTOR_CBI` | `2` | CBI with interrupt |
| `USBSTOR_CB` | `3` | CBI without interrupt (CB) |

The value is written as `HKR,,DriverFlags,0x00010001,<n>`. The meaning of
each value is taken from the section names. No driver kit document
describes `DriverFlags` (the 2000 DDK help and the 98 DDK help were
searched). It follows that the INF, not the interface descriptor, tells
`usbstor.sys` which transport to use. That is inferred from the INFs, not
verified.

XP adds `USBSTOR_CBI_NR`, which is CBI plus `NonRemovable=1` with the lower
filter deleted, for three Sony devices.

**On 9x (ME, and 98 SE with NUSB's storage files).** ME's `usbstor.inf` (`DriverVer`
06/08/2000, 5.00.2042.1) has these sections:

- `[USBSTOR.AddReg]` writes `DevLoader=*NTKERN` and
  `NTMPDriver=USBSTOR.SYS` (the WDM driver loaded through NTKERN).
- `[USBSTOR_xxx.AddReg]` writes `DriverFlags`.
- `[USBSTOR_xxx.HW]` writes `upperfilters="USBNTMAP.SYS"` to the hardware
  key.
- `CopyFiles` copies `USBSTOR.SYS` and `USBNTMAP.SYS` to
  `system32\drivers`, with `LayoutFile=layout.inf, layout1.inf, layout2.inf`.

The child device that `usbstor.sys` reports for a floppy is matched in
`usbntmap.inf` (ME): `USBSTOR\GenSfloppy` maps to `USBMPHLP.Dev`, which
sets `DevLoader=*IOS` and `PortDriver=USBMPHLP.PDR`. The drive then reaches
IOS through `usbmphlp.pdr` and gets a drive letter from the 9x block
stack. ME's `ntmap.inf` carries the same `GenSfloppy` line for the 1394
path.

**On 2000 to 8.** The `.NT` sections copy `USBSTOR.SYS`, write
`DriverFlags`, and add the service `USBSTOR` (`ServiceType 1`,
`StartType 3`, `ServiceBinary %12%\USBSTOR.SYS`). The service is added in
`USBSTOR_xxx.NT.Services`. On 7 SP1 x64 the service section also has
`AddReg=DeviceSpecific`. No NT section adds a filter for CBI.

The floppy child binds through `flpydisk.inf` to `sfloppy.sys`
("High-Capacity Floppy Disk Drive"). Its `sfloppy_install.NT.HW` section
writes `SuperFloppy=1`. The match differs by version:

- 2000 SP4 matches `GenSFloppy`.
- XP SP3 and 8 also match `USBSTOR\GenSFloppy`, under the name
  `usbfloppy_devdesc`.
- XP x64 SP2 matches `GenSFloppy` only.

`sfloppy.sys` is on the 2000 and XP CDs (`I386\SFLOPPY.SY_`, and in
`SP4.CAB` / `DRIVER.CAB`).

Microsoft documents that the child's identifiers derive from SCSI INQUIRY
data, but lists both `GenDisk` and `GenSFloppy` for direct-access type 0.
The additional rule choosing between those two is not documented there.
T3 records which child a particular drive exposes; it verifies that the
system's floppy binding works, without establishing the driver's internal
decision rule. See [Identifiers Generated by USBSTOR.SYS](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/identifiers-generated-by-usbstor-sys).
ReactOS's GPL-2.0-or-later `usbstor` (`drivers/usb/usbstor/`)
does not help: its `fdo.c` refuses UFI and every non-Bulk-Only interface
("USB Floppy devices are not supported").

### 2.2 The listed floppies, per system

The table counts the model lines whose description string names a floppy,
and all model lines installed with the CBI section (`USBSTOR_CBI`, and on XP
and later also `USBSTOR_CBI_NR`; CB lines are not counted).

| System | `usbstor.inf` | Floppy lines (VID:PID) | CBI lines | Class matches |
|---|---|---|---|---|
| 98 SE | none on the CD | - | - | - |
| ME | 06/08/2000, 5.00.2042.1 | 4: `03EE:6901` Mitsumi, `0409:0040` NEC, `057B:0000` Y-E Data, `0644:0000` TEAC | 6 | `08/02/50`, `08/05/50`, `08/06/50` |
| 2000 SP4 | 11/14/1999, 5.00.2183.1 | 3: `03EE:6901`, `057B:0000`, `0644:0000` | 4 | the same three |
| XP SP3 x86 | 07/01/2001, 5.1.2600.0 | 8: the ME four, plus `03F0:2001` HP, `0424:0FDC` SMSC, `054C:002C` Sony, `057B:0001` Y-E Data | 19 (16 `USBSTOR_CBI`, 3 `USBSTOR_CBI_NR`) | the same three |
| XP x64 SP2 | 10/01/2002, 5.2.3790.1830 | 7: as XP, without SMSC `0424:0FDC` | 19 | the same three |
| Vista SP2 x86 | 06/21/2006, 6.0.6002.18005 | 8: as XP SP3 | (not counted) | the three, plus `08/08/50`, `08/08/52` |
| 7 SP1 x64 | 06/21/2006, 6.1.7601.17514 | 8: as XP SP3 | (not counted) | as Vista |
| 8 x86 | 06/21/2006, 6.2.9200.16384 | 8: as XP SP3 | 20 | as Vista |

No `usbstor.inf` read, from ME to 8, matches
`USB\Class_08&SubClass_04` in any form. They do not match it with `Prot_00`,
with `Prot_01`, with `Prot_50`, or alone.

The ME, 2000 SP4 and XP SP3 counts were checked again against the INFs on
those CDs (`WIN9X\PRECOPY1.CAB` and `I386\USBSTOR.IN_`): 6, 4 and 19
model lines. [candidates.md](candidates.md) carries the same figures.

### 2.3 Text-mode Setup

`TXTSETUP.SIF` lists the IDs that text-mode Setup loads `usbstor` for. That
covers F6 driver disks read from a USB floppy:

- **2000 SP4:** `03EE:6901`, `04E6:0001`, `057B:0000`, `0644:0000` and the
  three Bulk-Only class IDs.
- **XP SP3:** thirteen VID/PIDs, including the same three floppies
  (`03EE:6901`, `057B:0000`, `0644:0000`), and the same class IDs.

An INF cannot change this. An unlisted drive still cannot serve as the F6
floppy. Only a remastered `I386` could, and that is out of scope. Owner,
2026-10-20: a note that only these three USB floppies work for F6 goes into
xHCI98's own F6 documentation, as a task of that project.

### 2.4 The gap, per system

| System | Gap | What the INF must contain |
|---|---|---|
| 98 SE | No in-box storage at all. With NUSB 3.3's storage files, floppies are matched by VID/PID only (section 5) | The 9x INF of section 4.1. It needs a USB storage driver set for 98 SE already installed |
| ME | Yes. 4 drives listed | The 9x INF of section 4.1 |
| 2000 SP4 | Yes, the widest: only 3 drives listed | The NT INF of section 4.2, undecorated models |
| XP SP3 x86 | Yes. 8 listed | NT INF, `NTx86` models |
| XP x64 SP2 | Yes. 7 listed | NT INF, `NTamd64` models |
| Vista, 7, 8 (x86, x64) | Yes. 8 listed | NT INF, `NTx86` and `NTamd64` models. On x64, a signed catalog (signed as xHCI98 and Stor98 are; Owner, 2026-10-20) |

## 3. What a class match needs

### 3.1 The IDs

On a single-function device, the USB hub driver gives the device node the
compatible IDs `USB\Class_08&SubClass_04&Prot_pp`, then
`USB\Class_08&SubClass_04`, then `USB\Class_08`, built from the interface
descriptor. ME's and XP's own INFs match the first form
(`USB\Class_08&SubClass_06&Prot_50`), which shows the form is generated on
both 9x and NT. On a composite device, the split-off function (`&MI_nn`)
carries the same compatible IDs.

The INF matches one ID per transport. Each is installed with the
`DriverFlags` that goes with its protocol:

| ID | Section | `DriverFlags` |
|---|---|---|
| `USB\Class_08&SubClass_04&Prot_00` | CBI | `2` |
| `USB\Class_08&SubClass_04&Prot_01` | CB | `3` |
| `USB\Class_08&SubClass_04&Prot_50` | Bulk-Only | `1`. Held until such a drive is tested (6.1) |

The INF does not match `USB\Class_08&SubClass_04` without a protocol,
because a protocol it does not know cannot be given a `DriverFlags`.

### 3.2 Rank: listed drives keep Microsoft's line

The 2000 DDK ("1.3 How Does Setup Select a Driver For a Device?") gives
the ranks:

- `0-FFF` when a device hardware ID matches an INF hardware ID;
- `2000-2FFF` when a device compatible ID matches an INF hardware ID.

A drive listed in `usbstor.inf` by VID/PID is therefore a hardware-ID
match for Microsoft's line. Our class line can only make a compatible-ID
match for that drive, so it never displaces Microsoft's choice. That
includes Microsoft's per-drive transport choice: Microsoft binds TEAC
`0644:0000` as CBI.

The same page says that a compatible-ID-only match makes Setup show the
Found New Hardware wizard in a client-side install. That is expected: the
user is pointing it at this INF anyway.

### 3.3 Interface-level IDs only

Owner, 2026-10-20: INFs here match interface-level IDs only and never bind
a whole device generically. The class IDs here come from the interface
descriptor. On a single-interface floppy, the device node is the function,
and Microsoft's own `usbstor.inf` binds Bulk-Only devices on the same node
by the same kind of ID. On composite devices, the INF matches only what the
stack has split off. Owner, 2026-10-20: the INF matches USB floppy drives
by class, with the CBI and CB lines; the Bulk-Only line is held back
(section 10).

## 4. The INFs

The INFs are written from scratch. The value names and section names they
name are facts read from the in-box INFs (section 2). No Microsoft text is
copied.

### 4.1 9x: 98 SE (with a USB storage driver set) and ME

`USBFLOP.INF`, with `Signature="$CHICAGO$"` and Class `USB` (ME's class
GUID `{36FC9E60-C465-11CF-8056-444553540000}`). It contains:

- **Models:** only the two release IDs of 3.1, with install sections
  `UFI_CBI` and `UFI_CB`. `UFI_BOT` and its `Prot_50` model line are
  reserved for M2 after the required hardware test.
- **Install sections:**
  - `AddReg`: `DevLoader=*NTKERN`, `NTMPDriver=USBSTOR.SYS`, and that
    section's `DriverFlags`.
  - `.HW` section `AddReg`: `HKR,,upperfilters,0,"USBNTMAP.SYS"`.
- **No `CopyFiles` in the first cut.** The INF installs no file.

Why there is no `CopyFiles`:

- On 98 SE, 98 SE's `layout.inf` does not know `USBSTOR.SYS`. A copy
  section would make Setup ask for a disk that holds it. The storage driver
  set the user installed first already puts the files in place: NUSB's
  `_NUSB.INF`, for one, copies `usbstor.sys` and `usbntmap.sys` to
  `SYSTEM32\DRIVERS`, and also copies `usbmphlp.pdr` and `DISKTSD.VXD`.
- On ME, the files are in `BASE2.CAB` and listed in `layout.inf`
  (`usbstor.sys=2,,21040`, `usbntmap.sys=2,,7136`,
  `usbmphlp.pdr=2,,4861`). Whether a fresh ME install has them on disk
  before any USB storage device was ever attached is unverified. ME's
  `copy2.inf` lists them, but in a `[Backup.Copy]` section, which settles
  nothing.

If test T1 shows they are missing on a fresh ME, two changes follow. The
ME folder gets its own INF with
`CopyFiles=` / `LayoutFile=layout.inf, layout1.inf, layout2.inf`, exactly
as ME's own `usbstor.inf` does. The 98 SE INF stays copy-free.

The child (`USBSTOR\GenSfloppy`) is left to the system's `usbntmap.inf`.
That is ME's own on ME, and the storage driver set's on 98 SE. NUSB 3.3's
`USBNTMAP.INF`
carries the line, with the comment "add USB Floppy Disk support".

Whether 9x Setup honours `Include=` and `Needs=` is unverified. The 98
DDK's help (`HELP_WIN98DDK.CHM`) has no INF reference. So the 9x INF
writes its own values and does not include `usbstor.inf`.

### 4.2 NT: 2000 to 8, x86 and x64

`USBFLOP.INF` (a separate file in its own folder, section 7), with
`Signature="$Windows NT$"`, Class `USB`, and `CatalogFile=USBFLOP.CAT`.
It contains:

- **`[Manufacturer]`:** `%Mfg%=Models,NTx86,NTamd64`. The undecorated
  `[Models]` section is kept for 2000.
- **Models:** the same two release IDs as 4.1, in each models section.
  There is no `Prot_50` model line in the first release.
- **Install sections:** they include Microsoft's own, so that each system
  gets its own service definition (7's `AddReg=DeviceSpecific`, for one).
  For CBI:

```
[UFI_CBI.NT]
Include=usbstor.inf
Needs=USBSTOR_CBI.NT

[UFI_CBI.NT.Services]
Include=usbstor.inf
Needs=USBSTOR_CBI.NT.Services
```

`UFI_CB` does the same with `USBSTOR_CB.NT`. A later `UFI_BOT` would use
`USBSTOR_BULK.NT`. These sections exist under these names in every NT
`usbstor.inf` read, from 2000 SP4 to 8.

Sources:

- `Include` and `Needs` in `DDInstall`, `DDInstall.Services` and
  `DDInstall.HW` are documented in the 2000 DDK ("INF DDInstall Section",
  "INF DDInstall.Services Section", "INF DDInstall.HW Section").
- XP's own `usbstor.inf` uses `Needs=USBSTOR_CB.NT` and
  `Needs=USBSTOR_CB.NT.Services` for its Casio `QV2KUX` line.

Three points are unverified, and M0 settles them on real systems:

1. **Where the copy comes from.** The included `USBSTOR_xxx.NT` section
   has `CopyFiles=USBSTOR.CopyList`. Through `Needs`, does Setup resolve
   the source with `usbstor.inf`'s own layout: `driver.cab` on 2000/XP,
   the driver store on Vista and later? If not, the fallback is the 9x
   pattern for NT:
   - own `.NT` sections that write `DriverFlags`;
   - a service definition written from documented INF directives and the
     service facts of 2.1, without copying Microsoft's INF text;
   - `CopyFiles` with `LayoutFile=layout.inf` on 2000/XP, and no
     `CopyFiles` on Vista and later, where the in-box package's driver
     store copy is the source (to be confirmed).
2. **Whether 2000 ignores the `NTx86`/`NTamd64` decorations** in
   `[Manufacturer]` and uses the undecorated `[Models]`. The decoration
   syntax is XP-era. If 2000 does not ignore them, 2000 gets its own INF.
3. **Package signature acceptance on x64.** The `.sys` it
   loads is Microsoft-signed, but that does not sign this INF. A project
   test signature on the catalog and Windows' trust of that signature
   are separate facts. Microsoft's documented test-package installation
   route includes certificate-store preparation; test-signing mode alone
   does not establish package trust. See [Installing Test-Signed Driver
   Packages](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/installing-test-signed-driver-packages).
   M0 records on 7 x64 staging, device installation and driver loading
   separately, for unsigned and project-test-signed packages. Vista x64
   and 8 x64 are not tested (section 8), and their acceptance stays
   unverified. M0 records the trust stores, boot policy, prompts and
   error codes. No step of this package imports a certificate or changes
   policy on the user's behalf. A release needs a demonstrated manual
   installation route under the documented policy; a rejected staging
   operation is a failure, not a successful install.

### 4.3 INF and removal contract

Both source INFs have explicit `Provider` and `DriverVer` fields and
package-specific description strings, so the selected line is identifiable
in the wizard and installation log. The NT file also has the `ClassGuid`
for `USB` from 4.1. In the first release, model entries name only the
protocol-specific interface class IDs in 3.1; no class-only,
subclass-only, `USB\COMPOSITE` or whole composite-device match is
emitted. Later per-drive `UFI_CB` lines (6.1, M2) are function hardware
IDs. A later hardware quirk must identify
the floppy function and must not capture its composite parent.

`Include`/`Needs` is the first NT design because it asks the installed
system to process its own storage sections. The documented
[DDInstall](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-ddinstall-section)
and [DDInstall.Services](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-ddinstall-services-section)
directives describe this mechanism. Their presence does not prove that a
particular old system resolves the copy source or section decorations;
those are M0 checks. Each DDInstall section and its `.Services` partner
must reference the same transport. Any `.HW` work is included only if the
target system's in-box section requires it.

The package consists of `USBFLOP.INF` and, on NT, its catalog; it is
installed by hand (`README.md`, "Installing"). It never owns `USBSTOR`,
its binaries, the floppy child's driver, or the 9x mapping components,
even if processing `Needs` creates the in-box service on that
installation. Removing it (uninstalling its devices in Device Manager, and
the published INF where the system allows) must not need, and the INF
must not contain, `DelService=USBSTOR`, a deletion of in-box files or a
change to the system's `usbstor.inf`. An unrelated USB
storage device must continue working after T10.

The release check confirms that each models section contains the two
approved class lines and only the per-drive lines 6.1 records, every
referenced local section exists,
CBI/CB transport pairs agree and `Prot_50` is absent until M2. It validates the
final INF before generating the catalog. Any change to signed INF bytes
requires a new catalog; see [Catalog Files and Digital Signatures](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/catalog-files).

## 5. Windows 98 SE

98 SE has no USB storage of its own (section 2). A user adds it with a
third-party USB storage driver set for 98 SE, installed before this
package. The set read for this design is NUSB 3.3's storage files; this
repository neither ships nor requires any particular one
([candidates.md](candidates.md), item 11).

What NUSB 3.3's files are:

- **The same driver as ME's.** NUSB 3.3's `USBSTOR.SYS` and
  `USBNTMAP.SYS` are byte-identical to ME's (SHA-256 `b92a6a15...` and
  `d1595cdb...`, against `BASE2.CAB`). The version resource reads
  4.90.3000.1. So the transport behaviour on 98 SE and on ME is one
  binary's, and one 9x INF serves both.
- **Two files that differ from ME's.** `USBMPHLP.PDR` and `DISKTSD.VXD`
  are the same size as ME's but hash differently. `_NUSB.INF` annotates
  `DISKTSD.VXD` as "242975, 246387, WinMe (fix dencorso)" and
  `usbmphlp.pdr` as "WinMe (fix, Taiwan comrad)". `USBSTOR.INF`'s history
  says "add USB FDD (for DISKTSD.VXD 4.90.3001)". What the fixes change is
  unverified. The numbers look like Microsoft KB articles that were not
  read.
- **Floppies by VID/PID only.** NUSB 3.3's `USBSTOR.INF` (header 01.08)
  lists 9 floppies: XP SP3's eight plus EKE `049F:0054`. Its `[Generic]`
  section matches subclasses `02`, `05` and `06` with protocols `50`, `00`
  and `01`, and never subclass `04`. An unlisted UFI drive on 98 SE under
  NUSB 3.3 therefore has the same gap as on ME.
- **NUSB 3.6 closes half the gap.** Its `usbstor.inf` (header 01.11a)
  adds `USB\Class_08&SubClass_04&Prot_00` as `USBSTOR_CBI` ("01.09 define
  new USB storage class") and Citizen `08BD:1100`. It does not add
  `Prot_01` or `Prot_50`.

So this INF helps on 98 SE when a storage driver set built on ME's
`usbstor.sys` and `usbntmap.sys`, such as NUSB 3.3's, is installed. It
binds that `usbstor.sys` to an unlisted UFI drive, exactly as on ME.
Without such a set it does nothing.

Owner, 2026-10-20: the 9x INF ships in this package's `free/` folder for
98 SE and ME, and the package's readme states that 98 SE needs USB storage
drivers already installed. The readme names no other repository.

## 6. Risks

### 6.1 CBI, CB and Bulk-Only floppies

- **CBI that should be CB.** Linux forces some drives that present CBI
  onto CB (`drivers/usb/storage/unusual_devs.h`, Linux v6.12):
  - TEAC `0644:0000` bcdDevice `0100`: `USB_SC_UFI, USB_PR_CB`;
  - Y-E Data `057B:0000` before firmware 3.00: `USB_PR_CB` ("We keep this
    entry to force the transport; firmware 3.00 and later is ok").

  Microsoft binds both as CBI (`DriverFlags 2`). So whether such a drive
  needs CB with Microsoft's driver is unknown. Any unlisted drive with the
  same fault would fail under the class line.

  The INF's only remedy is a function hardware-ID line installed with
  `UFI_CB`: include `&MI_nn` on a composite; use a bare VID/PID only for a
  verified single-function floppy. A
  hardware-ID match outranks the class line (3.2), so the fix is per drive
  and leaves other drives alone. `DriverFlags` and XP's `NonRemovable` are
  the only knobs the in-box INFs show. Any other per-device setting
  `usbstor.sys` may read is undocumented, and this design does not look
  for it.
- **The interrupt data of a UFI drive.** It is ASC/ASCQ, not the generic
  CBI status (CBI 1.1, section 3.4.3.1.3, table 3.8). Linux handles this in
  `transport.c` (`usb_stor_CB_transport`, "UFI gives us ASC and ASCQ").
  `usbstor.sys` runs the listed UFI drives as CBI, so it must handle it
  too. That is an inference, settled by T4.
- **Bulk-Only UFI** (`08/04/50`). Linux accepts it
  (`USUAL_DEV(USB_SC_UFI, USB_PR_BULK)` in `unusual_devs.h`), and
  `protocol.c` pads every UFI command to 12 bytes (`usb_stor_ufi_command`).
  Microsoft lists no Bulk-Only UFI drive. Two things are unverified:
  - whether `usbstor.sys` sends 12-byte UFI commands over Bulk-Only;
  - whether any such drive exists in the bench's reach.

  Without this INF such a drive has no driver at all, so the line can only
  improve on that. Its risk is a drive that mounts but misbehaves. Owner,
  2026-10-20: the `Prot_50` line is held until such a drive has been
  tested.

### 6.2 Media formats

The UFI specification's formattable capacities (UFI 1.0, section 4.10.2,
tables 35 and 37) are:

- 720 KB: 1440 blocks of 512 bytes;
- 1.44 MB: 2880 blocks of 512 bytes;
- 1.25 MB: 1232 blocks of 1024 bytes, the Japanese 3-mode format.

With no medium, the drive reports one maximum descriptor for 1.44 MB.

Unverified per system, and part of T5:

- whether 2000 to 8's `sfloppy.sys`, and 9x's `usbmphlp.pdr` with IOS,
  read and format 720 KB media;
- whether they handle 1024-byte sectors (1.25 MB) at all.

Reading 1.44 MB and 720 KB is the minimum to claim. Formatting is claimed
only where T5 passes. 1.2 MB 5.25-inch formats do not exist on UFI drives
and are not claimed.

### 6.3 Boot time, BIOS emulation and drive letters

Many BIOSes emulate a USB floppy as drive A: through INT 13h until an OS
driver takes the controller over. xHCI98 and the in-box host drivers take
ownership during boot.

On 98 SE and ME, a real-mode-visible A: with no protected-mode owner can
end up in MS-DOS compatibility mode. Two outcomes are possible once
`usbstor.sys` claims the same drive:

- **two drive letters for one drive;**
- **a compatibility-mode A: that stops working after the handoff.**

Both are unverified. T6 reads them with the BIOS's USB legacy or floppy
emulation on and off. The user documentation will give the remedy found,
probably "turn USB floppy emulation off in the BIOS" or "attach the drive
after Windows starts".

On 2000, XP and 7 x64, T6 records the letter assigned with and without
an internal floppy and with a remembered assignment already present. The
INF does not promise A: or change drive letters. Whether Disk Management
can reassign a floppy's letter on 2000 is unverified.

Booting from a USB floppy is the BIOS's business, and this package has no
part in it. Text-mode Setup's list is fixed (2.3).

### 6.4 Drives that need a quirk

The floppy entries in Linux's `unusual_devs.h` (v6.12):

| VID:PID | Drive | Linux entry | Effect on this design |
|---|---|---|---|
| `0409:0040` | NEC UF000x | `US_FL_SINGLE_LUN` | Listed by Microsoft (ME, XP+); unlisted on 2000, so the class line serves it there |
| `054C:002C` | Sony USB floppy | `US_FL_SINGLE_LUN` | Listed XP+; class line on ME and 2000 |
| `055D:2020` | Samsung SFD-321U, firmware `0C` | `US_FL_SINGLE_LUN` ("floppy reports multiple luns") | Listed nowhere. Phantom extra drives if `usbstor.sys` scans more than one LUN on it (unverified) |
| `057B:0000` | Y-E Data, firmware below 3.00 | `USB_PR_CB`, `US_FL_SINGLE_LUN` | Listed by Microsoft as CBI everywhere |
| `0644:0000` | TEAC, bcdDevice `0100` | `USB_SC_UFI, USB_PR_CB` | Listed by Microsoft as CBI everywhere |
| `08BD:1100` | Citizen X1DE-USB | `US_FL_SINGLE_LUN` | Listed nowhere by Microsoft. NUSB 3.6 only |

`US_FL_SINGLE_LUN` keeps Linux from probing LUNs above 0. How
`usbstor.sys` counts LUNs on a CBI device is undocumented. Get Max LUN
belongs to Bulk-Only, and CBI has no such request. The Samsung and
Citizen drives are the ones to try if they can be found (T7).

No INF line can express a single-LUN limit. If a drive shows phantom
units, the outcome is a documented limitation, not a fix.

### 6.5 Under each host stack

A floppy is a full-speed device. It uses three kinds of pipe: control
(the ADSC request carries every command), bulk, and, for CBI, interrupt.

The paths:

- **xHCI98:** the drive sits at a root or hub port directly.
- **Microsoft's EHCI stacks and NUSB's EHCI stack** (98 SE; fully
  supported, Owner, 2026-10-20): it runs
  either through a companion UHCI/OHCI controller, or, on chipsets with a
  rate-matching hub, through the EHCI's split transactions.
- **Microsoft's UHCI/OHCI stacks:** directly.

Each path is a separate test leg (T8). Only the split-transaction path,
with the drive behind a High Speed hub's transaction translator, is an
EHCI leg; a drive handed to a companion controller is a UHCI/OHCI leg.
xHCI98's 98 SE/ME silent install does not cover `08/04/00`. Its test
`test/test_silent.c` expects "CBI floppy" not to match. So under xHCI98
the user meets the wizard and points it at this package, as under every
other stack.

### 6.6 Nothing of ours runs

If `usbstor.sys` mishandles a drive, this project cannot fix it short of
writing a storage driver. MSC98 was considered and dropped
([candidates.md](candidates.md), item 11). The INF can only choose the
transport per drive.

## 7. Package, files and folders

- **Package:** `src/storage/usbflop/`. The name is 8.3, and no file of
  that name exists on the 98 SE, ME, 2000 or XP media read.
- **Files:**
  - `W9X\USBFLOP.INF`: the 9x INF;
  - `NT\USBFLOP.INF`: the NT INF;
  - the build script, which makes and signs `USBFLOP.CAT` from the NT
    INF, signed as xHCI98 and Stor98 are (Owner, 2026-10-20);
  - `README.TXT`, the package's readme, with the 98 SE prerequisite
    (section 5).
- **Release folders,** in the one scheme of `README.md`, "Layout":
  - `free/storage/usbflop/w98se-me/`: `USBFLOP.INF` (9x) and `README.TXT`;
  - `free/storage/usbflop/w2k-8/`: `USBFLOP.INF` (NT) and `USBFLOP.CAT`.
    One folder serves every NT target, x86 and x64, as `serial.md`'s
    `w2k-8` does.

  If T1 needs a copying INF for ME, `w98se-me` splits into `w98se` and
  `wme`. If 2000 rejects the decorated `[Manufacturer]`, `w2k` splits off
  and the rest becomes `wxp-8`.
- **Build toolchain:** none beyond the signing tools.
  `chkinf` from WDK 7.1 (`tools\`) may be run on the NT INF as a lint.
  It is not a gate.
- **NOTICE.md rows:** none. Nothing is adapted. Linux's `unusual_devs.h`,
  `transport.c` and `protocol.c` (GPL-2.0+) and ReactOS's `usbstor`
  (GPL-2.0-or-later) were read as references only.

## 8. Test plan

All tests use real drives (Owner, 2026-10-20: one or two mainstream devices
per class). The bench needs:

- **one unlisted UFI CBI drive.** It is the point of the package. Which
  current drive is unlisted is not known until its IDs are read (section
  10).
- **one listed drive**, for example TEAC `0644:0000`, as the control.

A listed drive can also exercise the class path. Use Update Driver / Have
Disk and pick this INF, because a user-chosen driver bypasses ranking.
That is useful when no unlisted drive is at hand. It is not a substitute
for a real unlisted drive, since automatic ranking is then not tested.

Tested systems (Owner, 2026-10-20; `README.md`, "Tested by the author"):
98 SE, ME, 2000 SP4, XP SP3 x86 and 7 x64. "Tested" in the table below
means these five. XP x64, Vista x86 and x64, 7 x86 and 8 x86 and x64 get
the same `w2k-8` folder, built from the same source, and stay "Built, not
tested". A reading only one of them could settle does not block a
release; the release notes list what stays unverified there (section 11).

Each tested system with an EHCI stack (98 SE under NUSB, 2000, XP, 7 x64)
gets an EHCI leg. A USB floppy is Full Speed, so that leg puts the drive
behind a High Speed hub's transaction translator on a real EHCI
controller; a drive on a QEMU EHCI port lands on the companion UHCI and
is not an EHCI leg ([build.md](build.md), section 8).

| Test | What | Systems |
|---|---|---|
| T0 | Read the drive's descriptors (USBView): VID/PID, `bInterfaceSubClass`, `bInterfaceProtocol`, endpoints, bcdDevice | Any |
| T1 | Fresh install, no storage device ever attached: are `USBSTOR.SYS`, `USBNTMAP.SYS` and `USBMPHLP.PDR` on disk? (decides 4.1's `CopyFiles`) | ME. 98 SE with and without NUSB 3.3's storage files |
| T2 | Unlisted drive: the wizard finds `USBFLOP.INF` when pointed at the folder. Device Manager shows the USB node on `usbstor.sys` with the right `DriverFlags`. A listed drive still gets Microsoft's line | Tested |
| T3 | The child's Hardware Ids (`GenSFloppy` or `GenDisk`) and its driver: `sfloppy.sys` on NT, `usbmphlp.pdr` on 9x. A drive letter appears | Tested |
| T4 | 1.44 MB media: directory, read and write of a large file, compare. Write-protect tab honoured. Media change (swap disks) noticed | Tested |
| T5 | 720 KB media: read, write, compare. Format at 1.44 MB and 720 KB from Explorer (2000+) and `FORMAT A:`-style from the 9x shell. 1.25 MB media if a 3-mode drive is at hand | Tested |
| T6 | Boot with the drive attached, BIOS USB floppy emulation on and off: drive letters, compatibility mode on 9x, access after handoff. Hot-plug and surprise removal mid-copy | Tested |
| T7 | Quirk drives if found: Samsung SFD-321U, Citizen X1DE-USB (phantom units?). A drive that needs CB: a function hardware-ID `UFI_CB` line fixes it without capturing a composite parent (6.1) | Any |
| T8 | Host stacks: xHCI98. Microsoft's UHCI/OHCI, and its EHCI with the drive behind a High Speed hub (the EHCI leg). NUSB's EHCI stack on 98 SE (fully supported), likewise behind a High Speed hub | Each tested system, with the stacks it allows; an EHCI leg on 98 SE, 2000, XP and 7 x64 |
| T9 | x64 install with the signed catalog, and once unsigned to record the behaviour | 7 x64 |
| T10 | Uninstall / reinstall. The in-box `usbstor.inf` and the `USBSTOR` service are untouched after removing our device entries | 2000, XP, 7 x64 |

## 9. Milestones

- **M0:** T0, T1, and the three unverified points of 4.2. ME decides the
  copy-free 9x path; 2000 SP4 decides undecorated model selection and
  source resolution; XP SP3 checks the x86 NT path; 7 x64 checks the
  `NTamd64` models and decides package trust and staging. These checks
  settle the INF shape before M1. XP x64, Vista x64 and 8 x64 are not
  tested (section 8); their `NTamd64` selection and package trust stay
  unverified there.
- **M1:** both INFs, the `Prot_00` and `Prot_01` lines, signed catalog,
  the readme. T2 to T6 and T8 on every tested system (section 8), then
  release `usbflop-1.0`. The systems not tested ship in the same folders
  as "Built, not tested", and the release notes list what stays
  unverified there (section 11).
- **M2:** per-drive `UFI_CB` lines, as real drives demand (T7), and the
  `Prot_50` line once a Bulk-Only UFI drive has been tested.

### M0 evidence and exit conditions

Record one result per tested OS and host stack, including OS build and
architecture, drive VID/PID and revision, interface/endpoint descriptors,
storage prerequisite and its version, INF version, chosen models/install
sections, parent and child IDs, `DriverFlags`, copy-source prompts,
installation log or registry observations, assigned drive letter and the
test date. A test on a listed drive forced through Have Disk is labelled
as such; it does not pass the unlisted-drive ranking check.

M0 passes only when a real UFI drive reaches a working floppy child on
each checked path, the `Prot_00` and `Prot_01` lines select distinct
transport settings, and install/remove leaves shared Windows components
intact. A usable CBI drive does not establish CB operation: if no CB drive
is available, that data-path check remains pending and release support is
not claimed from the INF setting alone. Missing prerequisites, rejected
signatures and unresolved copy sources are recorded as failures with a
specific corrective experiment. No README cell becomes Tested from M0
alone unless the recorded real-device run meets README's definition.

## 10. Owner decisions and open questions

Owner, 2026-10-20, on the questions this draft raised:

- **Class match.** The INF matches USB floppy drives by class, with the
  CBI (`Prot_00`) and CB (`Prot_01`) lines of 3.1, which are
  interface-level IDs (3.3). The Bulk-Only line is held back (below).
- **98 SE.** The 9x INF ships in `free/storage/usbflop/w98se-me/`; its
  readme states that 98 SE needs USB storage drivers already installed,
  naming no other repository (section 5). The package depends on no other
  repository.
- **`Prot_50`.** The Bulk-Only UFI line is held until such a drive has
  been tested (6.1).
- **XP F6.** A note that only Mitsumi `03EE:6901`, Y-E Data `057B:0000` and
  TEAC `0644:0000` floppies work in text-mode Setup (2.3) goes into
  xHCI98's own F6 documentation, as that project's task.
- **Test hardware**: one or two mainstream devices per class (section 8).
- **Back-ported EHCI stacks**: NUSB's EHCI stack on 98 SE is fully
  supported and in the test matrix (T8).
- **Signing**: as xHCI98 and Stor98 (section 7).

### Open questions

- **Bench drive.** Which USB floppy drive(s) are on the bench? Section 8
  needs one whose VID/PID none of the lists in 2.2 names, plus ideally a
  listed TEAC or Y-E Data unit as the control.

## 11. Not verified

- What `DriverFlags` 1, 2 and 3 mean inside `usbstor.sys`. Taken from the
  section names.
- How `usbstor.sys` decides to report `GenSFloppy` (T3), and how it counts
  LUNs on CBI drives (T7).
- Whether `usbstor.sys` sends UFI's 12-byte commands over Bulk-Only
  (6.1).
- Whether a fresh ME install has the storage files on disk (T1).
- `Include`/`Needs` source resolution for an in-box INF's `CopyFiles` on
  2000 to 8. 2000's handling of decorated `[Manufacturer]` entries. 9x
  support for `Include`/`Needs`.
- Install of an unsigned INF-only package on x64: read on 7 x64 (T9).
  Vista x64 and 8 x64 are not tested, and stay unverified.
- 720 KB and 1.25 MB support and formatting per system (T5). BIOS
  emulation interplay (T6).
- What NUSB's changed `DISKTSD.VXD` and `USBMPHLP.PDR` fix.
- Vista x64, 7 x86 from its ISO, and 8 x64 `usbstor.inf` were not read
  from the media. Vista SP2 x86, 7 SP1 x64 and 8 x86 were, and agree.
- Everything T2 to T10 reads, on the systems not tested (section 8): XP
  x64 (where `flpydisk.inf` matches `GenSFloppy` only, 2.1), Vista x86 and
  x64, 7 x86, and 8 x86 and x64. The release notes list these.

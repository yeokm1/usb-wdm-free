# USB serial ports (CDC-ACM, and what the vendors already cover)

Status: design draft, 2026-10-20. Implementation status, 2026-10-22:
`USBCDC.INF` (2000 to 8) is written and simulation-tested
([work log](../testing/2026-10-21-usbcdc-vm.md)). The 98 SE / ME driver
(`USBCOM.SYS`, `USBCOMV.VXD`, `USBCOM.INF`, `USBCOMP.INF`) is written and
built ([first build](../testing/2026-10-22-usbcom98.md)), and has run in 98 SE
guests with the simulated CDC-ACM device under xHCI98 and under NUSB's EHCI
([guest legs](../testing/2026-10-22-usbcom98-legs.md)). Section 15 lists
where the implementation departs from this draft; 15.1 what the guest legs
settled.

## 1. Scope, and the owner's decision

USB serial devices fall into two groups:

- **Vendor bridge chips:** FTDI, Silicon Labs CP210x, WCH CH340/CH341 and
  Prolific PL2303. Each vendor shipped its own Windows 98 driver at some
  point (section 2).
- **CDC-ACM devices:** Arduino Uno R3 (ATmega16U2), Leonardo/Micro
  (ATmega32U4), Raspberry Pi Pico (RP2040), ESP32-S2/S3 native USB, STM32
  "Virtual COM Port" firmware, WCH CH9102/CH343, USB modems. No vendor
  driver serves them on 98 SE or ME, and the in-box one serves only a few
  named modems (section 2.5).

Owner, 2026-10-20: chips that have a working vendor driver for 98 SE/ME
go into the non-free pack, `usb-wdm-nonfree`, a separate project, under
`non-free/serial/<chip>/<systems>/` (section 3). This project does not
write drivers for them, and nothing here depends on that pack.

What this design covers is the real gap:

1. **CDC-ACM on Windows 98 SE and ME.** This is the package designed here
   (sections 4 to 13).
2. **CDC-ACM on 2000, XP, Vista, 7 and 8.** Every one of them ships
   Microsoft's `usbser.sys`. We ship an INF that binds it, and write no
   driver. Owner, 2026-10-20: the 2000 front end (M4) is built only if
   2000's `usbser.sys` fails testing as a Ports-class COM port there
   (section 5.3).
3. **Vendor-chip silicon that no vendor 98 driver accepts.** This covers
   FTDI FT-X/FT232H/FT4232H, PL2303G-series, CP2105/CP2108, and whatever the
   section 2 checks add. These are later, optional modules on the same core.
   Owner, 2026-10-20: later, and on demand only.

The gap is not negligible. Arduino-class and RP2040/ESP32 boards are the
USB serial devices people actually plug in today, and on 98 SE/ME none of
them has a COM-port driver from anyone.

## 2. What already exists, per chip and per system

"Verified" means read on this host on 2026-10-20 from the source named.
Where a vendor page could only be fetched from the Internet Archive, the
archived URL is given.

### 2.1 FTDI

| System | Driver | Chips (from its INF) | Status |
|---|---|---|---|
| 98 / ME | VCP **1.09.06**, `R10906.zip`, 2004-11-25 ("Does not support FT2232 or FT4232 devices") | PIDs `6001`, `8372`, `6007`, `6008`, `6009`, `0232` (`FTDIBUS.INF`) | Verified |
| 98 / ME | VCP **1.0.3**, `FTC100103.zip`, 2004-03-12 ("Only supports FT2232D devices") | `6010&MI_00`, `6010&MI_01` (`FTCUSB.INF`) | Verified |
| 2000 | CDM **2.06.00** WHQL, 2009-10-22 | `6001`; `6010&MI_00/01`; `6011&MI_00..03` (`ftdibus.inf`) | Verified |

**Sources.** FTDI's VCP page as archived, with the version table rows and
comments quoted above:
`https://web.archive.org/web/2015/http://www.ftdichip.com/Drivers/VCP.htm`.
The live page `ftdichip.com/drivers/vcp-drivers/` returned 403 to this host.
A search snippet says it keeps a "No Longer Supported" section for older
systems; that section's contents are unverified.

The three ZIPs were fetched from the archived links into the scratchpad, as
data, and their INFs read. Only the INFs and file lists were read, no
binaries:

| File | Size (bytes) | SHA-256 |
|---|---|---|
| `R10906.zip` | 323,481 | `fa41bcfb2cfe2635ee59a7b33be591fa6b361360a6c95517ebf3e4c78fa57d30` |
| `FTC100103.zip` | 403,298 | `2dfb71ce39ec305708d577e846d05fb34eea80e1d0248d533fbab9736e9fd898` |
| `CDM 2.06.00 WHQL Certified.zip` | - | `a6920cac51bf1cf2df6649c789e9fcd604269ab1e73fa74d5f631d4493f847eb` |

**Which silicon the 98/ME driver covers.** FT8U232AM and FT232BM are PID
`6001`. FT232R is also `6001`, but it was released after 1.09.06; whether
1.09.06 drives it correctly is **unverified**. The release notes
(`906 Release Info.DOC`) do not mention it. A test with a real FT232R on
98 SE would settle it.

**Not covered on 98/ME by any FTDI release found:**

- FT-X series, PID `6015` (FT230X, FT231X, FT234X and others)
- FT232H, PID `6014`
- FT4232H, PID `6011`
- FT2232H, PID `6010`: whether the 2004 FT2232D driver accepts the H part is
  unverified.

The PIDs are from Linux `drivers/usb/serial/ftdi_sio_ids.h` lines 22-27.
On 2000, CDM 2.06.00 also lacks `6014` and `6015`; whether a later CDM
installs on 2000 is unverified.

**What FTDI's 98 package is made of**, read from its INFs:

- `FTDIBUS.INF` (Class `USB`): `DevLoader=*ntkern`, `NTMPDriver=ftserial.sys`,
  and an upper filter `ftsenum.sys` that creates a child `FTDIBUS\COMPORT&...`.
- `FTDIPORT.INF` (Class `Ports`) installs that child with:
  - `DevLoader=*vcomm`
  - `PortDriver=ftcomms.vxd` (FTDI's own VCOMM port driver)
  - `Enumerator=ftsenum.vxd`
  - `Contention=*vcd`
  - `PortSubClass=01`

This is the published shape of a working USB-to-COM driver on 98: a WDM
driver below, plus a VCOMM port-driver VxD above. Section 5.1 adopts the
same shape. Silicon Labs' 98 package had the same shape: its v4.20 file list
has `slabcomm.vxd` next to WDM `.sys` files (section 2.3).

### 2.2 WCH CH340 / CH341

- **Driver:** CH341SER **3.7**, 2022-03-02. WCH's description reads
  "支持32/64位Windows 11/10/8.1/8/7/VISTA/XP，SERVER 2022/.../2003，2000/ME/98"
  (32/64-bit Windows 11/10/8.1/8/7/Vista/XP, Server 2022 to 2003, and
  2000/ME/98). Its chip list is CH340G/T/C/N/K/E/B and CH341A/F/T/B/C/U.
  **Verified** from WCH's own download page as archived:
  `https://web.archive.org/web/2022id_/http://www.wch.cn/downloads/CH341SER_EXE.html`.
- **Older releases:** version 3.4 (2016-09-28) gave the same 98/ME support
  on the same page in 2016. A third-party copy of the 3.3 INF header (2011)
  reads "WDM&VXD for Windows 98/Me/2000/XP"; that copy is unverified.
- **Current release:** whether WCH's current page (`wch-ic.com` /
  `wch.cn/downloads/CH341SER_EXE.html`, rendered by script, not readable on
  this host) and its current ZIP still carry the 98 VxD is **unverified**.
  Reading the INF inside the current `CH341SER.ZIP` would settle it.
- **Licence:** WCH describes the package as "用于随产品发行到最终用户"
  (for distribution to end users with products). No EULA was found;
  redistribution terms are **unverified**.
- **CH9102 and CH343 are not CH341-protocol chips.** Linux binds neither in
  `ch341.c`, whose ID table (lines 84-89) lists only `1a86:5523/7522/7523`
  and three others. Mainline `cdc-acm.c` has no entry for them either, so
  Linux serves them through its generic CDC-ACM class match. That suggests
  they present standard CDC-ACM interfaces, and **this package covers
  them**. It is an inference from Linux. Reading a CH9102's descriptors
  would settle it.

### 2.3 Silicon Labs CP210x

- **Historical 98 support:** the CP210x Windows Driver **v4.20** (2004)
  release notes list 9x components: `slabcm95.sys`, `slabwh95.sys`,
  `slabcomm.vxd`, `slabvcd.vxd`, `unin98.exe`, `slabvxd.inf`. That is
  evidence of a 98 build, but the notes never name the operating systems.
  The copy read was a third-party mirror
  (`https://cdn.sparkfun.com/images/tutorials/USB-Drivers/WIN/Readme.txt`).
- **Official download: not located.** Silicon Labs' VCP page links a
  "Legacy OS Software" section whose contents were not readable. The
  official release notes URL returned 403. So a **current official download
  of a 98 driver is unverified**, and so is the licence.
- **Chips:**
  - v4.20 predates CP2105 (PID `ea70`) and CP2108 (`ea71`), so those are
    gaps on 98.
  - Whether it accepts CP2102N or CP2104, which share PID `ea60` with
    CP2101/2, is unverified.
- **If no official 98 package can be found,** CP210x on 98 joins the gap,
  through a module written from the facts of Linux `cp210x.c` (GPL-2.0
  only, so no code is taken; section 10), built on demand only (Owner,
  2026-10-20).

### 2.4 Prolific PL2303

- **98/ME driver:** **2.0.0.19**, included in Prolific installers from
  2009-2010, according to third-party mirrors: the "GPS" installer v1.0.1.3
  (March 2009) and the PL2303 setup v1.3.0 (July 2010), "For Prolific USB
  VID_067B&PID_2303 Only". Prolific's own later notes say "Windows 98 and
  Windows ME driver support is discontinued and is no longer provided."
  **Unverified** against a Prolific page: Prolific's download site needs a
  login.
- **Chips:**
  - The PL2303G series (GC/GB/GT/GL/GE/GS, PIDs `23a3`, `23b3`, `23c3`,
    `23d3`, `23e3`, `23f3`; Linux `pl2303.h` lines 12-17) is certainly not
    covered on 98.
  - Whether 2.0.0.19 drives PL2303HXD or PL2303TA (PID `2303`, newer
    silicon) is **unverified**.
  - Whether it accepts clone chips is **unverified**. The clone rejection is
    reported to be a feature of later 3.x drivers. A test with an HXD, a TA
    and a clone on 98 SE would settle both.
- **Licence:** unverified.

### 2.5 Generic CDC-ACM

**98 SE.** It ships `usbser.sys` and `ccport.sys`; both are in xHCI98's
`tools\win98se-extracted\wdm\`, dated 1999-04-23. It also ships
`wdmmdmld.vxd`, which is named in the INFs; the file itself was not looked
at. They are bound only by `usbcdc.inf`, read from the 98 SE CD:

- `Class=USB`.
- Three modems only, by VID/PID: `1183:4008`, `04C1:0097`, `04C9:F101`.
- `DevLoader=*ntkern`, `NTMPDriver="usbser.sys,ccport.sys"`, and a `ChildID`
  (for example `USB\USR0021`).
- That child is installed by `mdm3cusb.inf` as `Class=Modem`, with:
  - `PortSubClass=02`
  - `FriendlyDriver=Unimodem.vxd`
  - `PortDriver=wdmmdmld.vxd`
  - an empty `Contention`

Nothing matches `USB\Class_02`.

**ME.** Its `usbcdc.inf` (ME CD, `WIN9X` cabs) has the same three files and
the same `NTMPDriver`, a longer VID/PID list (3Com, Lucent, MultiTech,
Metricom, Lasat, Elsa, Compaq), and `ChildID` values of the form
`WDM_MODEM\...`. It uses its own class, "WDM MODEM", whose class installer is
`modem.cpl,WdmEnum_ClassInstall`. ME's `mdm3cusb.inf` is again Class Modem,
with `PortDriver=wdmmdmld.vxd`.

**What this means on 98 SE and ME.** The in-box path produces a *modem*
(PortSubClass 02). The 98 DDK says modems with no resources get no
`PortName` (98 DDK `HELP_OTHER.CHM`, "VCOMM Device Initialization",
`comm_0sqf.htm`: "Internal and external modems devnodes do not have
resources, so are not assigned PortNames"). Such a device is reachable by
its friendly name through Unimodem/TAPI, not as `COMn`. No generic or vendor
CDC-ACM COM-port driver for 98 SE/ME was found.

**2000, XP and later.** `usbser.sys` ships:

- 2000 SP4: `usbser.sys` 1999-10-15, in `I386\DRIVER.CAB`.
- XP SP3: `mdmcpq.inf` copies it.

On both, it is bound only as a `LowerFilters` entry under modem INFs
(`mdmcpq.inf`: `AddService=usbser,...,LowerFilter_Service_Inst`,
`[FakeModemCopyFileSection] usbser.sys,,,0x20`). Nothing matches
`USB\Class_02`, so a CDC-ACM device needs an INF on every target.

### 2.6 Summary

| Device family | 98 SE / ME | 2000 | XP to 8 |
|---|---|---|---|
| FTDI FT232BM/AM, FT2232D | Vendor (non-free pack) | Vendor | Vendor (Windows Update) |
| FTDI FT232R | Vendor, unverified | Vendor | Vendor |
| FTDI FT-X, FT232H, FT4232H | **Gap** | **Gap** (unverified) | Vendor |
| CH340/CH341 | Vendor (non-free pack) | Vendor | Vendor |
| CP2101/2 | Vendor if the v4.x package is found, else **gap** | Vendor | Vendor |
| CP2105/CP2108 | **Gap** | Unverified | Vendor |
| PL2303 (old) | **Gap**: 2.0.0.19 exists only as unofficial copies, so the non-free pack drops it (section 3) | Vendor | Vendor |
| PL2303 HXD/TA/G | **Gap** (G certain, HXD/TA unverified) | Unverified | Vendor |
| **CDC-ACM** (Arduino, Pico, ESP32-S2/S3, STM32, CH9102/CH343, modems) | **Gap: this package** | INF for `usbser.sys` | INF for `usbser.sys` |

## 3. Non-free pack entries (owner decision)

These entries belong to `usb-wdm-nonfree`, a separate project; the free
package of section 4 neither ships nor needs any of them. They are listed
here only so the gap analysis is complete.

Owner, 2026-10-20: the vendor 98 serial drivers are handled as NUSB is in
that pack: they go in the release download only, never into git history;
each one's provenance (official download, version, hash) is recorded; and
any with no official source is dropped. So the PL2303 2.0.0.19 driver, for
which no current official source was found, is dropped, and the CP210x
entry exists only if an official copy is found.

The paths mirror `non-free/ethernet/ax88772/` and the systems folder names
of the pack's `README.md` (`w98se-me`, `w2k-xp`). The files go in
unmodified, with their original names, which are 8.3 except where the vendor
named otherwise (noted). Nothing is downloaded into either repository by
this design.

| Path | Package | Official download | Redistribution terms |
|---|---|---|---|
| `non-free/serial/ftdi/w98se-me/` | FTDI VCP 1.09.06 (`R10906.zip`) | ftdichip.com VCP page, "No Longer Supported" section (live page unread; archived link `ftdichip.com/Drivers/VCP/Win98/R10906.zip`) | See below |
| `non-free/serial/ft2232d/w98se-me/` | FTDI VCP 1.0.3 (`FTC100103.zip`) | As above (`.../VCP/Win98/FTC100103.zip`) | As FTDI |
| `non-free/serial/ftdi/w2k/` | FTDI CDM 2.06.00 | As above (`.../CDM/Win2k/CDM 2.06.00 WHQL Certified.zip`) | As FTDI |
| `non-free/serial/ch341/w98se-me/`, `.../ch341/w2k-xp/` | WCH CH341SER (3.7 or the newest that still lists 98/ME) | `wch.cn/downloads/CH341SER_ZIP.html`, `wch-ic.com/downloads/CH341SER_ZIP.html` | "For distribution to end users with products" (WCH page); EULA unverified |
| `non-free/serial/cp210x/w98se-me/` | CP210x v4.x, if an official copy is found | silabs.com "Legacy OS Software" (contents unverified) | Unverified |
| (none) | Prolific 98/ME driver 2.0.0.19 | No current official source found | Dropped (owner) |

**FTDI's terms.** Neither `R10906.zip` nor the CDM 2.06.00 ZIP contains a
licence file (their file lists, read). FTDI's site terms apply. The archived
VCP page (2015) says "FTDI drivers may be used only in conjunction with
products based on FTDI parts. FTDI drivers may be distributed in any form as
long as license information is not modified." The full Licence Terms of the
same period (`.../Drivers/FTDriverLicenceTerms.htm`, archived 2015-2017),
clause 1.3, are narrower: "If you are a seller or distributor of a Device
then You may distribute the Software with the Device". They also say the
software may be used only with "Genuine FTDI Components". The two texts
disagree on standalone redistribution. Owner, 2026-10-20: the pack treats
them as it treats NUSB: release download only, provenance recorded.

**Notes for whoever fills the pack:**

- `R10906.zip` holds `906 Release Info.DOC`, which is not 8.3. No INF names
  it. Owner, 2026-10-20: it keeps its long name when the FTDI package is
  added, an exception to 8.3 recorded in the pack.
- `FTDIBUS.INF` lists `ftlang.dll` in `[SourceDisksFiles]`, but the ZIP does
  not carry it. Whether the install warns is unverified.
- The CDM 2.06.00 ZIP has `i386\` and `amd64\` subfolders and a long
  top-level folder name. Its files keep their names, and the pack folder
  holds the `i386` set and the INFs, as FTDI laid them out.
- The pack's `README.md` lists 98 SE, ME, 2000 and XP. XP and later get
  FTDI, WCH, Silicon Labs and Prolific drivers from the vendors or Windows
  Update, so the pack need not carry them.

## 4. The package

**One package with a shared serial core and per-protocol modules, not one
package per chip.** The reasons:

- On 98 SE/ME, most of the work does not depend on the chip:
  - the VCOMM port-driver VxD
  - the child Ports devnode and its `PortName`
  - the receive and transmit queues, and the event and notification
    machinery
  - XON/XOFF
  - surprise removal and power
- On 2000+ the NT front end, should it be needed (M4), is chip-independent
  too.
- A chip module only supplies these:
  - its USB match
  - line coding (baud divisor, data/parity/stop)
  - control lines
  - modem and line status decoding
  - framing of received data: FTDI and PL2303 prefix status bytes, CDC
    does not
- Linux splits the work the same way: `drivers/usb/serial/usb-serial.c` and
  `generic.c` are the core, `ftdi_sio.c`, `cp210x.c`, `ch341.c` and
  `pl2303.c` are modules, and `cdc-acm.c` stands alone only because it sits
  directly on the tty layer.

Separate packages would duplicate the VxD and the core for each chip, and
each would need its own `PortName` handling, which is the hardest piece to
get right on 98 (section 5.1.4).

**Name:** `src/serial/usbcom/`

| File | What |
|---|---|
| `USBCOM.SYS` | WDM driver: USB side, protocol modules (CDC-ACM first), child PDO per port, the direct-call port interface (98) and, from M4 only, the NT serial front end |
| `USBCOMV.VXD` | VCOMM port driver (98 SE / ME only) |
| `USBCOM.INF` | Binds `USBCOM.SYS` to the USB devnode (98 SE / ME; Class `USB`, as FTDI's `FTDIBUS.INF`) |
| `USBCOMP.INF` | Installs the child port devnode as Class `Ports` with `DevLoader=*vcomm`, `PortDriver=USBCOMV.VXD` (98 SE / ME) |
| `USBCDC.INF` | 2000 to 8, x86 and x64: binds Microsoft's in-box `usbser.sys` as a Ports-class driver. No binary of ours |

The names avoid `usbser`, which is Microsoft's file on every target.

Release folders, following `README.md`:

- `free/serial/usbcom/w98se-me/`: `USBCOM.SYS`, `USBCOMV.VXD`, `USBCOM.INF`,
  `USBCOMP.INF`.
- `free/serial/usbcom/w2k-8/`: `USBCDC.INF`. It is one folder because an
  INF with `NTx86` and `NTamd64` sections serves every NT target, and it
  holds no binary, so it takes no architecture suffix (`README.md`,
  "Layout").
- Only if M4 is built: `free/serial/usbcom/w2k/` with `USBCOM.SYS` and its
  NT INF, and the INF-only folder becomes `free/serial/usbcom/wxp-8/`.

## 5. Per-OS plumbing

### 5.1 Windows 98 SE and ME: how a WDM driver becomes `COMn`

Applications on 98 open `COMn` through VCOMM:

- Win32: `CreateFile("COM3")` goes to VCOMM.
- Win16: `COMM.DRV` calls VCOMM.

VCOMM is the device loader for Ports and Modem devnodes, and it calls a
*port driver* VxD named by the devnode's `PortDriver` value. This comes from
the 98 DDK `HELP_OTHER.CHM`:

- "VCOMM Device Initialization" (`comm_0sqf.htm`)
- "VCOMM Open - Matching Names to Drivers" (`comm_0sqg.htm`)
- "Installation and the Registry" (`comm_0sqe.htm`)
- "Port Enumeration" (`comm_0t4o.htm`)

A WDM driver is loaded by NTKERN, not VCOMM, so it does not become a COM
port by itself. Something must bridge the two. Three routes were weighed.

#### 5.1.1 Route A: the in-box bridge (`ccport.sys` + `wdmmdmld.vxd`)

Microsoft's own path (section 2.5) is:

- `usbser.sys` under `ccport.sys` as the `NTMPDriver` pair on the USB
  devnode;
- a child created from the `ChildID` value;
- that child loaded by VCOMM with `PortDriver=wdmmdmld.vxd`.

There are two variants:

- **A1:** an INF of ours binds the in-box `usbser.sys,ccport.sys` to CDC-ACM
  devices, plus a child INF.
- **A2:** our WDM driver replaces `usbser.sys` in `NTMPDriver`, under
  `ccport.sys`.

Against it:

- **Nothing about it is documented.** The 98 DDK documentation says nothing
  about `ccport.sys`, `wdmmdmld.vxd`, `NTMPDriver` lists of two drivers, or
  `ChildID`. A full-text search of `HELP_WIN98DDK.CHM` and `HELP_OTHER.CHM`
  found no hits. So for A2:
  - what `ccport.sys` sends down to the driver below it is **unverified**;
  - it could be learned only by watching it, since disassembly is excluded.
- **It is built for modems.** Every in-box child is Class Modem,
  PortSubClass 02. Modems get no `PortName` (section 2.5), so they are opened
  by friendly name through Unimodem, not as `COMn`. Arduino IDE, terminal
  programs and most serial software ask for `COMn`.
  - Whether a Ports-class child (PortSubClass `01`, `PortDriver=wdmmdmld.vxd`,
    `Contention=*vcd`) is given a `PortName` and works is **unverified**.
  - A black-box test on 98 SE settles it: M0 step 3, an INF-only experiment
    that uses only the in-box files, which the user's own CD supplies.
- **A1 inherits a 1998 `usbser.sys`.** We cannot fix it. Its handling of
  composites, zero-length packets and surprise removal is unknown.

Route A is worth one afternoon (M0) because, if a Ports-class child works,
it would give 98 SE/ME users something with no code at all. It is not the
plan.

#### 5.1.2 Route B: our own VCOMM port driver over our WDM driver (chosen)

This is the shape FTDI and Silicon Labs shipped (section 2.1, 2.3). It is
built only from interfaces the 98 DDK documents:

- **VCOMM port-driver interface.** From `incs\INC_WIN98_VCOMM.H` lines
  35-43 and `HELP_OTHER.CHM` ("Communications Port Drivers",
  `comm_0t4m.htm`...`comm_12uo.htm`):
  - `_VCOMM_Register_Port_Driver` and `_VCOMM_Add_Port`
  - `DriverControl` with `DC_InitializePort`
  - `PortOpen` and `_PORTFUNCTIONS`: `PortSetCommState`, `PortSetup`,
    `PortRead`, `PortWrite`, `PortPurge`, `PortEscapeFunction`,
    `PortSetEventMask`, `PortEnableNotification`, `PortSetReadCallback`,
    `PortSetWriteCallback`, `PortGetModemStatus`, `PortGetProperties`,
    `PortClose` and the rest.
- **VxD to WDM.** NTKERN's services `_NtKernCreateFile`, `_NtKernClose`,
  `_NtKernReadFile`, `_NtKernWriteFile`, `_NtKernDeviceIoControl`
  (`incs\INC_WIN98_NTKERN.H` lines 40-44; C prototypes in
  `INC_WIN98_VXDWRAPS.H` lines 347-364), with the library
  `LIB_I386_FREE_VXDWRAPS.LIB` (98 DDK `LIBS_DDK`).
- **Registry values.** `REGSTR_VAL_NTMPDRIVER`, `REGSTR_VAL_PORTDRIVER`,
  `REGSTR_VAL_PORTNAME` and `REGSTR_VAL_PORTSUBCLASS`
  (`INC_WIN98_REGSTR.H` lines 385, 963, 1066, 1071).

The pieces:

1. **`USBCOM.SYS` on the USB devnode** (`USBCOM.INF`: `DevLoader=*ntkern`,
   `NTMPDriver=USBCOM.SYS`). It is the function driver for the device. For
   each serial function it finds, it also acts as a bus driver and creates
   one child PDO, with device id `USBCOM\COMPORT` and instance id
   `<VID>_<PID>_<n>`. FTDI does the same with a separate upper filter
   (`ftsenum.sys` creating `FTDIBUS\COMPORT&...`, section 2.1). We fold it
   into the one driver.
2. **The child devnode is installed by `USBCOMP.INF`** as Class `Ports`
   with:
   - `DevLoader=*vcomm`
   - `PortDriver=USBCOMV.VXD`
   - `Contention=*vcd`, as FTDI does. The DDK notes VCD is the COM-port
     contention handler; it virtualises nothing for a port without I/O
     resources. Whether an empty `Contention` would be better, as the in-box
     `mdm3cusb.inf` has it, is checked in M1.
   - `PortSubClass=01`
   - `ConfigDialog=serialui.dll` and `EnumPropPages=serialui.dll,EnumPropPages`,
     the in-box COM-port property pages (`comm_0sqe.htm`)
   - a default `DCB` value

   NTKERN starts no WDM driver on that PDO. The child INF names no
   `NTMPDriver`, so the PDO is "raw". VCOMM, the devnode's loader, handles
   it.
3. **`USBCOMV.VXD`, loaded by VCOMM when the port is opened by name.** It
   registers with `_VCOMM_Register_Port_Driver` on
   `Sys_Dynamic_Device_Init` ("Initialization", `comm_0t4p.htm`). In
   `DC_InitializePort` it calls `_VCOMM_Add_Port` for its port. On
   `PortOpen`, which runs in the calling thread's context, it:
   1. opens the WDM port device with `_NtKernCreateFile`;
   2. sends one private `_NtKernDeviceIoControl`,
      `IOCTL_USBCOM_GET_INTERFACE`, which returns a table of entry points
      and a context pointer;
   3. closes the handle.

   From then on the VxD calls `USBCOM.SYS` directly. Both are ring 0 in the
   same flat address space. Direct calls are needed because VCOMM requires
   `PortRead`, `PortWrite`, `PortEscapeFunction` and others to be "callable
   at interrupt time" (`comm_12rd.htm`, `comm_12sx.htm`, `comm_12t0.htm`).
   NTKERN file I/O cannot be issued there, but a non-blocking call into a
   table can.

The interface table is one versioned structure. These calls are non-blocking
and safe at interrupt time:

- `Attach(context, callbacks)` / `Detach`
- `KickTx()`: the transmit queue has new data
- `RxResume()`: there is room in the receive queue again
- `SetLine(baud, bits, parity, stop)`
- `SetControlLines(dtr, rts)`
- `SetBreak(on)`
- `Purge(flags)`
- `GetModemStatus()`

The configuration calls queue a work item that sends the control transfer.
`USBCOM.SYS` calls back into the VxD from its URB completion routines with
`RxData(buf, len)`, `TxDone(n)`, `ModemStatusChanged(msr)`,
`LineError(bits)` and `Gone()`.

#### 5.1.3 Route C: a VCOMM port driver over the in-box `usbser.sys`

`USBCOMV.VXD` could open the 98 SE/ME `usbser.sys` device with
`_NtKernCreateFile`, if that driver answers the NT serial IOCTLs there.
That would save writing `USBCOM.SYS` for 98. Rejected as the plan, for
these reasons:

- whether 98's `usbser.sys` creates an openable device object, and which
  IOCTLs it answers, is **unverified**, and only black-box tests could
  learn it;
- it is the same unfixable 1998 binary as A1;
- every data-path call would be an IRP through NTKERN, which cannot be
  issued at interrupt time.

M0 step 4 records whether it would work, for comparison only.

#### 5.1.4 `PortName`: the open problem on 98

VCOMM assigns `PortName` only to devnodes that hold I/O resources. It maps
`03F8` to COM1 and so on, and gives others COM5 and up ("VCOMM Device
Initialization", `comm_0sqf.htm`). It reads `PortName` from the hardware key
when it loads the devnode, before any port driver runs.

A USB child holds no resources, so who writes `PortName` is our problem.
FTDI's 1.09.00 release note says "Update device map with port name", which
suggests that their driver writes it. How they do it is not known, and must
not be learned from their binaries.

The design is as follows:

- **`USBCOM.SYS` chooses the name** before it reports the child PDO, when it
  handles `IRP_MN_QUERY_DEVICE_RELATIONS` (BusRelations). It picks the
  lowest `COMn` with n ≥ 3 that no `PortName` value under `HKLM\Enum` uses.
  It remembers the choice per device instance (VID/PID/serial number, or the
  location), in its own key under the driver key, so the same board keeps
  the same `COMn`.
- **It writes the name** into the child's hardware key, as `PortName` and as
  a `FriendlyName` ending "(COMn)". The key is opened with
  `IoOpenDeviceRegistryKey(PLUGPLAY_REGKEY_DEVICE)` once the PDO exists;
  NTKERN maps that to the devnode's `Enum` key. That mapping is
  **unverified** on 98 and is read in M1.
- **`USBCOMV.VXD` checks the name.** It is also named as the child's
  `Enumerator`, so VCOMM loads it at device-load time (`comm_0sqf.htm`). If
  the name is missing it writes it with `_RegSetValueEx`, and it calls
  `_VCOMM_Add_Port` with the same name.
- **It also writes `HKLM\Hardware\DeviceMap\SerialComm`**, which some
  applications enumerate.

Which of these steps VCOMM actually needs is settled empirically in M1. The
first M1 exit criterion is that `COMn` appears in Device Manager and opens.
If none of them work, the fallback is a static `USBCOMV.VXD`, loaded at boot
through `HKLM\System\CurrentControlSet\Services\VxD\USBCOM` (`StaticVxD`).
It would register its ports with VCOMM itself, without a `PortDriver`
lookup.

#### 5.1.5 What 98/ME users get, and do not

- Win32 and Win16 applications that use the comm API work: HyperTerminal,
  Arduino IDE 1.0.x (if it runs on 98, which is unverified), terminal
  programs, PPP through Dial-Up Networking (DUN) with a "Standard modem"
  on the port.
- MS-DOS programs that drive a UART at `3F8h` directly do not. There is no
  UART to trap.

ME has the same VCOMM, NTKERN and `serialui.dll` model. ME has no real-mode
DOS, so the DOS point applies to DOS boxes only. Whether ME's VCOMM differs
in `PortName` handling is unverified; ME is not a tested system for this
package (section 11), so it stays unverified there.

### 5.2 Composite devices on 98 SE, ME and 2000 under Microsoft's stacks

Many CDC-ACM boards are composite:

- **Leonardo:** CDC plus HID.
- **Pico** (pico-sdk stdio): CDC plus a vendor reset interface, device
  class `EF/02/01` with an IAD (Interface Association Descriptor).
- **ESP32-S2/S3** (TinyUSB, or the S3's USB-Serial-JTAG): CDC plus vendor
  interfaces.

Composites are handled differently per system:

- On 98 SE and 2000, `usb.inf` installs `USB\COMPOSITE` on `usbhub` itself
  (98 SE `usb.inf` line 91 and `[Composite.Dev]`; 2000 SP4 `usb.inf`
  `[Composite.Dev.NT.Services] AddService = usbhub`).
- XP SP3 installs it on `usbccgp` (`[Composite.Dev.NT.Services]
  AddService = usbccgp`).

Whether the 98 SE / 2000 hub's composite parent honours IADs is
**unverified**. Microsoft's "Support for Interface Collections" documents
IAD grouping, but not for these systems; reading it settles which systems.
If the hub splits per interface, the CDC communication interface and its
data interface land in different devnodes. A driver on the first cannot
open the second's endpoints.

Owner, 2026-10-20: as an exception to this repository's rule that INFs
match interface-level ids only, on Microsoft's 98 SE and 2000 stacks
`USBCOM.INF` (and `USBCDC.INF` on 2000) binds the **listed** composite
boards, and only those, by **whole-device hardware id**,
`USB\VID_vvvv&PID_pppp`. A hardware id outranks the `USB\COMPOSITE`
compatible id. The driver then selects the configuration itself and finds
the CDC pair (section 6.1). The other functions are lost while our driver
owns the device (Leonardo's HID, for example), and the release notes say so
for each listed board. Owner, 2026-10-20: the exception covers ME's in-box
stack only if testing shows that ME's stack does not split composites;
if it splits them, ME is served by the `&MI_nn` ids as under xHCI98. ME is
not a tested system for this package (section 11), so which case holds
stays unverified there, and the release notes say so.

**Under xHCI98** the bus splits composites itself, honouring IADs, and gives
each function PDO the hardware ids `USB\VID_vvvv&PID_pppp&MI_nn` and
IAD-derived compatible ids (xHCI98
`docs/contributing/design/13-superspeed-hcd.md` 10.7 and 10.8). The
whole-device id is then not on any PDO a function driver sees. `USBCOM.INF`
therefore lists both forms, `USB\VID_..&PID_..` and `USB\VID_..&PID_..&MI_nn`,
for each composite board. `USBCOM.SYS` handles both cases:

- bound to a whole device, it selects the configuration;
- bound to a function PDO, it selects only its own interfaces, from the
  filtered configuration descriptor.

### 5.3 Windows 2000, XP, Vista, 7, 8: INF only

`USBCDC.INF` (Class `Ports`, Ports class GUID) binds the in-box `usbser.sys`
as the function driver, with the Ports class co-installer `msports.dll`. On
NT, `msports.dll` assigns the `PortName` from ComDB, and the driver creates
the `COMx` link and the `SERIALCOMM` entry. It copies `usbser.sys` the way
Microsoft's own modem INFs do: `Include=mdmcpq.inf` and a copy section with
flag `0x20`, read in 2000 SP4 and XP SP3 `mdmcpq.inf`. For Vista, 7 and 8
the same `Include` is the documented pre-Windows-10 practice. Their
`mdmcpq.inf` was not read on this host (unverified).

**Matches:**

- the known boards' VID/PID (whole device on 2000; `&MI_nn` too on XP+);
- `USB\Class_02&SubClass_02` for functions that `usbccgp` splits on XP SP3
  and later.

**Not** bare `USB\Class_02`: a device of class 02 can equally be CDC
Ethernet.

The INF is written from scratch. Linux's
`Documentation/usb/linux-cdc-acm.inf` says it is based on a Microsoft and
Microchip template "likely to be covered by the MLPL", so it is not used as
a source.

**Unverified, settled in M0:**

- **2000:** whether its `usbser.sys` serves a Ports-class devnode (all its
  in-box uses are as a modem lower filter), and whether it copes with a
  whole composite device;
- **XP SP3:** whether `usbccgp` groups a Pico's IAD into one function;
- **7 x64:** whether an INF-only package with no catalog installs. The
  `.sys` is Microsoft-signed. Vista x64 and 8 x64 are not tested (section
  11), and stay unverified.

If 2000 fails, M4 adds an NT front end to `USBCOM.SYS` for 2000. It
implements the `ntddser.h` IOCTL set with `serial.sys` semantics (section 7)
and is installed with `msports.dll` as a Ports-class driver.

### 5.4 The CDC-ACM class match on 98 SE, ME and 2000 (owner, 2026-10-26)

Owner, 2026-10-26 (xHCI98 roadmap 44.44): a class driver matches its class
where Windows has none of its own (build.md section 7.1). 98 SE, ME and
2000 have no CDC-ACM COM-port driver (section 2.5), so `USBCOM.INF` (98 SE,
ME) and `USBCDC.INF`'s 2000 section match `USB\Class_02&SubClass_02`, the
CDC-ACM function, as `USBCDC.INF` already did on XP and later. Never bare
`USB\Class_02`, which CDC Ethernet shares (section 5.3). The listed boards
keep their lines; a hardware id still outranks the class line.

What the line reaches: a CDC-ACM function that a bus presents on its own,
as xHCI98 does for an IAD function (section 5.2), which gets the function
class's compatible ids. A device of class 02 at the device level (the Uno
R3's shape) reports `USB\Class_02&SubClass_00` and is not reached, as
before. On a stack that splits a composite device per interface, the
CDC communication interface alone would match; the driver could not reach
the data interface there and would refuse its start (Code 10; not run),
where no driver was found before. The Q5 exception (whole-device lines for the
listed boards) is unchanged.

## 6. USB side

### 6.1 Finding the CDC-ACM function

The descriptor rules come from CDC 1.2 and its PSTN subclass, and from Linux
`drivers/usb/class/cdc-acm.c` (GPL-2.0+, v6.12), `acm_probe` lines
1185-1460.

1. **The communication interface.** It is class `02`, subclass `02` (ACM),
   any protocol. Its class-specific descriptors are:
   - Header
   - Call Management (`bDataInterface`)
   - ACM, whose `bmCapabilities` bits are:
     - D1: `SET_LINE_CODING`, `SET_CONTROL_LINE_STATE`, `GET_LINE_CODING`
       and the `SERIAL_STATE` notification;
     - D2: `SEND_BREAK`.
   - Union (`bControlInterface`, `bSubordinateInterface0`)
2. **The data interface.** It is class `0A`, with one bulk IN and one bulk
   OUT endpoint, possibly at a non-zero alternate setting. The
   communication interface has one interrupt IN endpoint, the notification
   endpoint.
3. **Union fallbacks** (Linux lines 1189-1340), adopted:
   - no Union descriptor: use Call Management's `bDataInterface`, then "the
     next interface";
   - Union naming the interfaces the wrong way round: swap them;
   - data endpoints in the wrong direction order: swap them;
   - control and data collapsed into one interface: `combined_interfaces`.
4. **Quirks.** The device table carries a quirk subset, from `cdc-acm.h`
   lines 107-115:
   - `NO_UNION_NORMAL`
   - `SINGLE_RX_URB`
   - `NO_CAP_LINE`
   - `IGNORE_DEVICE`
   - `QUIRK_CONTROL_LINE_STATE`
   - `CLEAR_HALT_CONDITIONS`
   - `SEND_ZERO_PACKET`

   Only entries for devices plausible on these systems are carried, each
   with its Linux line reference.
5. **Several CDC-ACM functions in one device** become several ports, one
   child PDO each. Examples are TinyUSB dual-CDC and some debug probes.

### 6.2 Transfers

- **Bulk IN.** Eight URBs of 4 KiB stay outstanding. Linux keeps `ACM_NR`
  16 of `readsize` each (`cdc-acm.h` line 38); fewer suffice at Full Speed.
  When the receive queue nears full (98: the VCOMM queue; NT: our buffer),
  completed URBs are not resubmitted. The device then sees NAKs, which is
  lossless flow control without CTS (section 7.3).
- **Bulk OUT.** One URB in flight, from the transmit queue (the draft had up
  to four; with four, QEMU's EHCI model under NUSB replayed an OUT the device
  had already taken: [usbcom-nusb-echo](../testing/2026-10-23-usbcom-nusb-echo.md)). A
  zero-length packet follows a transfer that is an exact multiple of
  `wMaxPacketSize` when the device needs one (`SEND_ZERO_PACKET`, Linux line
  1448). xHCI98's handling of zero-length packets and short packets is per
  its USBD interface. This driver does not depend on `USBD_SHORT_TRANSFER_OK`
  semantics beyond what all three Microsoft stacks provide.
- **Interrupt IN notification.** One URB, resubmitted on completion.
  `SERIAL_STATE` (PSTN 1.2, 6.5.4) gives these bits:

  | Bit | Signal | Mapped to |
  |---|---|---|
  | D0 | `bRxCarrier` | DCD (RLSD) |
  | D1 | `bTxCarrier` | DSR |
  | D2 | `bBreak` | break |
  | D3 | `bRingSignal` | RI |
  | D4 | `bFraming` | framing error |
  | D5 | `bParity` | parity error |
  | D6 | `bOverRun` | overrun |

  Linux decodes the same bits at lines 304-354.
- **Control.**
  - `SET_LINE_CODING` (7 bytes: `dwDTERate`, `bCharFormat`, `bParityType`,
    `bDataBits`)
  - `SET_CONTROL_LINE_STATE` (`wValue` D0 = DTR, D1 = RTS)
  - `SEND_BREAK` (`wValue` in ms; `0xFFFF` = until a `SEND_BREAK` with 0,
    PSTN 1.2 6.3.13), sent only when `bmCapabilities` D2 is set
- **No isochronous transfers.** Full Speed is enough. High-Speed CDC
  devices simply get larger bulk packets.

### 6.3 Chip modules (later, on demand only)

Each is a table entry plus about 300 lines on the core. The Linux sources,
with their licences; a GPL-2.0 (only) source gives facts only (section
10):

- **FTDI:** `ftdi_sio.c` (GPL-2.0+), which may be adapted.
  - Divisor functions: `ftdi_232am_baud_base_to_divisor` (line 1127),
    `ftdi_232bm_baud_base_to_divisor` (1152, a 3 MHz base with eighths
    encoded as sub-divisor codes) and `ftdi_2232h_baud_base_to_divisor`
    (1173, a 12 MHz base for H parts).
  - Each 64-byte packet in carries a 2-byte modem and line status prefix.
- **CP210x:** `cp210x.c` (GPL-2.0; facts only). The rate is set as a 32-bit value, with
  per-part rounding: `cp210x_get_an205_rate` (line 998) and
  `cp210x_get_actual_rate` (1010).
- **CH341:** `ch341.c` (GPL-2.0; facts only). `ch341_get_divisor` (line 175) produces a
  prescaler and divisor; `ch341_set_baudrate_lcr` (239) writes them.
- **PL2303:** `pl2303.c` (GPL-2.0; facts only). The rate is encoded either directly or
  as a divisor: `pl2303_encode_baud_rate_direct` (602) and
  `pl2303_encode_baud_rate_divisor` (610). The HXN/G series uses different
  registers (`PL2303_HXN_*`, lines 167-172).

## 7. Serial behaviour

### 7.1 Timeouts

- **On 98/ME, VCOMM owns `COMMTIMEOUTS`.** `_VCOMM_GetSetCommTimeouts`
  (`comm_0sz4.htm`) is a VCOMM service, and `_PORTFUNCTIONS` has no timeout
  entry (`comm_12pg.htm`). The port driver's job is to keep the receive
  queue current, and to fire the read callback (`PortSetReadCallback`,
  `comm_12t5.htm`) and event notifications promptly. To that end, received
  bytes reach the VCOMM queue from the URB completion, with no batching
  delay.
- **On NT (M4 only), the driver implements `serial.sys` semantics exactly**,
  using `SERIAL_TIMEOUTS` from `ntddser.h` (2000 DDK `tools\ntddk\inc\ntddser.h`,
  WDK 7.1 `inc\api\ntddser.h`):
  - **Read return now:** `ReadIntervalTimeout == MAXULONG` with both total
    fields 0 returns at once with whatever is buffered.
  - **Read, wait for the first byte:** `MAXULONG` / `MAXULONG` /
    0 < constant < `MAXULONG` returns at once if any byte is buffered.
    Otherwise it waits up to the constant for the first byte and returns as
    soon as one arrives.
  - **Read, the general case:** the total timeout is
    `multiplier × length + constant`, where 0 for both means none. The
    interval timer restarts at each byte, and starts only after the first.
    With all three fields zero, a read waits until the full length arrives.
  - **Write:** the total timeout is `multiplier × length + constant`, and 0
    means none. A write that times out completes `STATUS_TIMEOUT` with the
    count actually sent.

  ReactOS `drivers/serial/serial/rw.c` (GPL-2.0 per ReactOS's top-level
  `COPYING`; read 2026-10-20) implements the same cases (lines 190-220), with
  a worker thread per read. We do it with timers and cancel-safe queues
  instead. Its `devctrl.c` has 22 `FIXME`/`UNIMPLEMENTED` markers, so it is a
  cross-check, not a source.

### 7.2 Line state

**Outputs.** `DTR` and `RTS` come from:

- the DCB (`fDtrControl`, `fRtsControl`);
- 98: `PortEscapeFunction`'s `SETDTR`, `CLRDTR`, `SETRTS` and `CLRRTS`
  (`comm_12t0.htm`);
- NT: `IOCTL_SERIAL_SET_DTR`, `_CLR_DTR`, `_SET_RTS`, `_CLR_RTS`.

Each change sends one `SET_CONTROL_LINE_STATE` carrying both bits, in call
order, never coalesced. esptool's reset sequence for ESP32 boards depends on
the exact order of DTR/RTS transitions. Unverified for the S2/S3 native-USB
case; M2 tests it.

**Inputs.**

- DSR, DCD and RI come from `SERIAL_STATE`.
- CDC has no CTS. CTS reads as always asserted, as Linux reports it:
  `acm_tty_tiocmget` ORs in `TIOCM_CTS` unconditionally, `cdc-acm.c` line
  938.
- 98: the modem status shadow byte uses 8250 MSR layout, deltas included
  ("Notifications", `comm_12np.htm`). `EV_CTS`, `EV_DSR`, `EV_RLSD`,
  `EV_RING`, `EV_BREAK` and `EV_ERR` are raised from changes.
- NT: `IOCTL_SERIAL_GET_MODEMSTATUS`, `WAIT_ON_MASK`.

### 7.3 Flow control

- **RTS/CTS.** Hardware handshaking does not exist on a CDC link. If the
  DCB asks for it, the request is accepted and ignored, and CTS stays
  asserted. Inbound flow is controlled by withholding IN URBs (section 6.2),
  which never loses data.
- **XON/XOFF.** If the DCB enables `fOutX`, received XOFF and XON stop and
  restart the transmit side. With `fInX`, XOFF is sent when the receive
  queue passes `XoffLim` and XON when it falls below `XonLim`
  (`PortRead`'s note on the XON limit, `comm_12rd.htm`). `TransmitCommChar`
  goes ahead of the queue.

### 7.4 Break

- 98: `PortEscapeFunction`'s `SETBREAK` and `CLRBREAK`.
- NT: `IOCTL_SERIAL_SET_BREAK_ON` and `_OFF`.

Either sends `SEND_BREAK` with `0xFFFF` or `0`. A received break
(`SERIAL_STATE` D2) raises `EV_BREAK` and `CE_BREAK`.

### 7.5 Baud rate

CDC carries any 32-bit `dwDTERate`, with no divisor to compute.

- On 98, a Win16 DCB may carry `CBR_*` index values (`0xFF10`...). Whether
  VCOMM converts them to a rate before `PortSetCommState` is unverified.
  The VxD maps them defensively to their rates.
- `bCharFormat` is 0/1/2 for 1, 1.5 and 2 stop bits. `bParityType` is
  0-4 for none, odd, even, mark and space. `bDataBits` is 5, 6, 7, 8 or 16.

### 7.6 Arduino and RP2040 resets

- **Uno R3** (16U2 firmware): asserting DTR on open pulses the 328P's reset
  through a capacitor. So `PortOpen` / `IRP_MJ_CREATE` sends DTR per the
  DCB default, `DTR_CONTROL_ENABLE`, and `PortClose` drops DTR and RTS.
  `serial.vxd` and `serial.sys` both drop DTR on close for real UARTs. The
  sketch-upload tools rely on this.
- **Leonardo/Micro and Pico ("1200-baud touch"):** the host opens the port,
  sets 1200 baud, and closes it. The device sees `SET_LINE_CODING`(1200)
  followed by DTR dropping, and reboots into its bootloader, which
  re-enumerates with another PID (Leonardo bootloader `2341:0036`, Pico
  BOOTSEL as a mass-storage device). The driver therefore:
  - sends `SET_LINE_CODING` synchronously, before `SetCommState` returns,
    so the order is kept;
  - drops DTR on close, even with a transmit queue pending, after a short
    drain bound to `WriteTotalTimeout`.

  The bootloader PIDs are in `USBCOM.INF`, so the bootloader gets a port
  too. On 98 the first appearance of each new PID runs the hardware wizard
  once.

### 7.7 Throughput target

Full Speed bulk tops out at about 1.2 MB/s (19 × 64-byte packets per frame).
The targets are:

- sustained 1 Mbit/s each way with no loss on 98 SE through a Pico echo
  sketch;
- 921600 baud on an Uno R3, where the 16U2's UART is the limit.

On 98 the cost is the VxD's byte copy into VCOMM's queue and VCOMM's
per-event notification. This is measured in M3.

### 7.8 Multiple ports

Each CDC function on each device gets a child PDO and a `COMn`. The VxD
keeps one `_PORTINFORMATION` per open port. There are no globals per port.

### 7.9 Surprise removal while open

**On 98.** `USBCOM.SYS` sees `IRP_MN_REMOVE_DEVICE`. 98 has no
`IRP_MN_SURPRISE_REMOVAL`; that is a 2000-and-later minor code (2000 DDK
`wdm.h`). Whether NTKERN sends `IRP_MN_STOP_DEVICE` first on a yank is
unverified, and M3 logs it. The driver:

1. marks each port dead;
2. cancels its URBs;
3. calls the VxD's `Gone()` callback. The VxD:
   - drops DSR and DCD in the shadow;
   - raises `EV_RLSD`, `EV_DSR` and `EV_ERR`;
   - makes later `PortRead`s return what is left;
   - fails `PortWrite`s.
4. **does not free** the port context until the VxD calls `Detach` from
   `PortClose`, which may come much later. A reference count shared by both
   sides keeps the context alive.

This ordering is the main crash risk in the whole design, and M3 tests it
with a port held open by HyperTerminal while the board is pulled. VCOMM's
side of a removed devnode with an open port is per `comm_0sqf.htm` ("VCOMM
unloads the port driver, if necessary"). Whether it does so while a client
still holds the handle is unverified, and M3 settles it.

**On NT (M4):** standard `SURPRISE_REMOVAL` handling. Pending IRPs complete
with `STATUS_DELETE_PENDING`, the `COMx` link goes, and the device object
stays until the last handle closes. In-box `usbser.sys`'s behaviour here is
whatever it is; M0 records it.

### 7.10 Power

- No selective suspend: the device stays in D0 while the driver is loaded.
- On system suspend, outstanding URBs are cancelled and the line state is
  saved. On resume, the driver sends `SET_LINE_CODING` and
  `SET_CONTROL_LINE_STATE` again and resubmits the IN and notification URBs.
  If the device re-enumerated across the suspend, it is treated as removal
  plus arrival.
- On 98 an open port survives standby (M3 tests APM and ACPI standby).
- The device's remote wakeup is not armed.

## 8. INF and install

**98 SE / ME** (`free/serial/usbcom/w98se-me/`). The user points the Add New
Hardware Wizard at the folder. Windows installs `USBCOM.INF` on the USB
devnode, then `USBCOMP.INF` on the child that `USBCOM.SYS` reports, with no
second prompt if both INFs are in the same folder. FTDI's package relies on
the same behaviour.

`USBCOM.INF` matches:

- a list of known boards by VID/PID (Q6), plus their `&MI_nn` forms for xHCI98;
- a generic model for unlisted boards, offered for manual "Have Disk"
  selection only. Owner, 2026-10-22:
  - The generic CDC-ACM entry uses an id that no device reports, `USBCOM\MANUAL_CDC_ACM`, in `USBCOM.INF` and in every model section of `USBCDC.INF`.
  - It never matches automatically.
  - It replaces the draft's `USB\Class_02&SubClass_00&Prot_00` with `ExcludeFromSelect`: that keyword only hides a model from the user's list; it does not stop automatic matching.

The driver refuses to start (Code 10) on a device without a CDC-ACM
function, such as a CDC-ECM adapter. Because the refusal is in the driver,
that model is safe to offer.

**2000 to 8** (`free/serial/usbcom/w2k-8/USBCDC.INF`): Update Driver, then
point it at the folder. The INF is unsigned; whether this blocks install
on x64 is read on 7 x64 in M0, and stays unverified on Vista x64 and 8
x64.

## 9. Build toolchain

- **`USBCOM.SYS` (x86, 98 SE to 2000).** The Windows 2000 DDK
  (`tools\ntddk`) with MSVC 6.0 (`tools\MSVC600`), as `uac2.md` section 8
  and xHCI98 do, under the same 9x import rule. The import table is limited
  to what 98 SE's NTKERN and `usbd.sys` export, and a script checks it.
- **`USBCOMV.VXD`.** The 98 DDK (`tools\win98ddk-extracted`):
  - `INC_WIN98_VMM.INC`, `INC_WIN98_VCOMM.H`, `INC_WIN98_NTKERN.H` and
    `INC_WIN98_VXDWRAPS.H`;
  - `LIB_I386_FREE_VXDWRAPS.LIB`;
  - the DDK's `BIN_LINK.EXE` with `/VXD`;
  - MSVC 6.0 `cl`;
  - `ml.exe` from the 2000 DDK (`tools\ntddk\bin\ml.exe`; whether it builds
    VxD segments, or a MASM 6.11d is needed, is unverified and checked in
    M1).

  The VxD is in C with a small assembly stub for the DDB and control
  dispatch, written from the 98 DDK's VxD documentation and the `VMM.INC`
  macros. The DDK's own `SERIAL.VXD` sample source
  (`COM_DDK\SRC_COMM_SERIAL_*`) is not read (Owner, 2026-10-20;
  `AGENTS.md`, "Sources"): VCOMM's expectations come from
  `HELP_OTHER.CHM` and the headers only.
- **x64.** None: on x64 targets only `USBCDC.INF` ships.
- All toolchains are installed into this repository's git-ignored `tools/`,
  and the package's build script names the paths.

## 10. What is adapted, and the NOTICE.md rows

Owner, 2026-10-25: the repository is GPL-2.0-or-later. A source under
GPL-2.0 only is not adapted; the modules below that would have followed
one are written from the facts it shows (request codes, register values,
divisor formulas, timeout cases) and from the chip vendors' published
documents where they exist, with no code, names or comments taken.

| File (planned) | Adapted from | Licence |
|---|---|---|
| `src/serial/usbcom/acmdesc.c` (CDC descriptor walk, union fallbacks, quirk table): **not adapted after all**. The walk in `acmcore.c` (2026-10-22) is original, written from CDC 1.2 and the behaviour list in 6.1, so it needs no NOTICE row. Its quirk table carries no entries yet | Linux `drivers/usb/class/cdc-acm.c` and `cdc-acm.h`, v6.12 | GPL-2.0+ |
| `src/serial/usbcom/m_ftdi.c` (on demand) | Linux `drivers/usb/serial/ftdi_sio.c`, `ftdi_sio.h`, `ftdi_sio_ids.h` | GPL-2.0+ |
| `src/serial/usbcom/m_cp210x.c` (on demand) | Not adapted: facts only from Linux `drivers/usb/serial/cp210x.c` (GPL-2.0) and Silicon Labs' published interface notes | - |
| `src/serial/usbcom/m_ch341.c` (on demand) | Not adapted: facts only from Linux `drivers/usb/serial/ch341.c` (GPL-2.0) | - |
| `src/serial/usbcom/m_pl2303.c` (on demand) | Not adapted: facts only from Linux `drivers/usb/serial/pl2303.c`, `pl2303.h` (GPL-2.0) | - |
| `src/serial/usbcom/nt_ser.c` (if M4; timeout case table only) | Not adapted: the timeout cases are written from the driver kit documentation of `SERIAL_TIMEOUTS`; ReactOS `drivers/serial/serial/rw.c` ("See COPYING in the top level directory") is a cross-check only | - |

Everything else is original:

- the VCOMM VxD, written from the 98 DDK documentation;
- the direct-call interface;
- the `PortName` logic;
- the INFs. The `linux-cdc-acm.inf` template is excluded, as section 5.3
  says.

## 11. Test plan

Tested systems (Owner, 2026-10-20; `README.md`, "Tested by the author"):
98 SE, 2000 SP4, XP SP3 x86 and 7 x64. ME, XP x64, Vista x86 and x64, 7
x86 and 8 x86 and x64 get release folders built from the same source and
stay "Built, not tested". A reading only one of them could settle does not
block a milestone or the release; the release notes list what stays
unverified there (section 14).

**QEMU** (11.1 on this host) has no CDC-ACM device model. Its USB devices
are `usb-serial` (an FTDI FT232BM, `hw/usb/dev-serial.c`), `usb-braille`
(also FTDI-based), `usb-net`, `usb-audio`, storage, HID and `usb-host` /
`usb-redir` passthrough (`qemu-system-i386 -device help`). So:

- **CDC-ACM** is tested by passing a real board into the 98 SE, 2000, XP
  and 7 x64 guests with `usb-host`. This covers the guest's own UHCI/OHCI
  stack and xHCI98 (`nec-usb-xhci`). The boards are Full Speed: on a QEMU
  EHCI port they land on the companion UHCI, which is not an EHCI leg
  ([build.md](build.md), section 8).
- **EHCI legs** run on real hardware, with the board behind a High Speed
  hub's transaction translator: under NUSB's EHCI stack on 98 SE, and
  under Microsoft's EHCI stack on 2000, XP and 7 x64. Each stays pending
  until it is run.
- QEMU's `usb-serial` (FTDI) concerns the non-free pack's FTDI entry, not
  this package, and is not part of this plan.
- **Real hardware** (Owner, 2026-10-20: one or two mainstream devices per
  class):
  - Arduino Uno R3: non-composite, DTR reset.
  - Raspberry Pi Pico: IAD composite, 1200-baud touch, throughput.

  Other boards (Leonardo's HID composite, ESP32-S2/S3's DTR/RTS sequence,
  CH9102) are tested only if at hand; otherwise their claims stay
  **unverified** until a user reports. Each runs on 98 SE under the in-box
  stack, under NUSB's EHCI stack (fully supported, Owner, 2026-10-20) and
  under xHCI98, and on real 98 SE hardware once; and on 2000, XP and 7 x64
  under Microsoft's stacks, with the EHCI legs above. The results go into
  `README.md`'s "Tested by the author" table.
- **Applications:** HyperTerminal; a Win32 test tool built with MSVC 6 that
  exercises:
  - `SetCommState`, `SetCommTimeouts` in all four cases of 7.1,
    `WaitCommEvent` for every `EV_*`, `PurgeComm`,
    `EscapeCommFunction`, `TransmitCommChar` and break;
  - a 1 Mbit/s echo with a checksum;
  - the Arduino reset;
  - pull-while-open.

  Also a Win16 terminal, to confirm the `COMM.DRV` path, and Dial-Up
  Networking with a "Standard modem" on the port.

## 12. Milestones (smallest useful first)

- **M0: INF experiments, no driver code.**
  1. `USBCDC.INF` on 2000 SP4, XP SP3 and 7 SP1 x64, with Uno R3, Pico
     and Leonardo. Record the Ports-class binding, composite behaviour,
     x64 install, and surprise removal. XP x64, Vista, 7 x86 and 8 are
     not tested (section 11).
  2. Ship `USBCDC.INF` for whatever passes. That alone serves 2000 to 8, or
     XP to 8 if 2000 fails; the systems not tested are "Built, not
     tested".
  3. On 98 SE: Route A1 with a Ports-class child INF over the in-box
     `usbser.sys`/`ccport.sys`/`wdmmdmld.vxd`. If an Uno R3 gets a working
     `COMn`, ship that INF early as an interim 98 binding, with its unknowns
     stated (Owner, 2026-10-20).
  4. On 98 SE: whether the in-box `usbser.sys` device opens with
     `_NtKernCreateFile`, for Route C (record only).
- **M1: 98 SE, Uno R3 only.**
  - `USBCOM.SYS` (CDC-ACM, non-composite, one port), `USBCOMV.VXD` and both
    INFs.
  - Exit: `COMn` in Device Manager; HyperTerminal round-trip; the Uno R3
    resets on open; the 7.1 timeout cases pass through VCOMM.
  - Settles `PortName` (5.1.4), `Contention`, and the `ml.exe` question.
- **M2: composites.**
  - The Pico (and other listed boards at hand) by whole-device id under
    Microsoft's 98 SE stack, by `&MI_nn` under xHCI98 and NUSB's stack;
    multiple CDC functions; bootloader PIDs.
  - ME is not tested (section 11). The `w98se-me` folder ships for it as
    "Built, not tested"; whether ME's stack splits composites (section
    5.2) and how its VCOMM handles `PortName` (5.1.5) stay unverified
    there, and the release notes say so.
- **M3: robustness.**
  - Surprise removal while open, standby/resume, two boards at once, the
    throughput target, Win16 and Dial-Up Networking.
  - First release, `usbcom-1.0`.
- **M4 (only if M0 shows 2000's `usbser.sys` unusable; Owner,
  2026-10-20):** the NT front end
  of `USBCOM.SYS` for 2000, with full `ntddser.h` IOCTLs and `serial.sys`
  timeout semantics, installed with `msports.dll`.
- **M5 (later, on demand only; Owner, 2026-10-20):** chip modules for the
  gaps in section 2.6, ordered by what users ask for. FT-X and PL2303G are
  the likely first.

## 13. Owner decisions and open questions

Owner, 2026-10-20, on the questions this draft raised:

- **Q1. Redistribution in the non-free pack.** The vendor 98 serial drivers
  are handled as NUSB is there: release download only, never in git
  history, provenance recorded; any with no official source is dropped
  (PL2303 2.0.0.19). This package depends on none of them.
- **Q2. Chip modules for vendor gaps** (FT-X/FT232H, PL2303G,
  CP2105/CP2108): later, on demand only.
- **Q3. 2000 fallback.** M4 is built only if 2000's `usbser.sys` fails
  testing.
- **Q4. Interim 98 INF.** If Route A1 works in M0, an INF-only 98 binding
  ships early.
- **Q5. Composite trade-off.** An exception to the interface-level-only
  rule: listed composite boards are bound as a whole device by VID/PID on
  Microsoft's 98 SE and 2000 stacks only, documented as losing their other
  functions there (section 5.2). ME's stack takes the same exception only
  if testing shows it does not split composites; ME is not tested
  (section 11), so this stays unverified there.
- **Non-8.3 readme in the FTDI package** (a matter for the non-free pack):
  `906 Release Info.DOC` keeps its long name, an exception to 8.3 recorded
  in the pack.
- **Test hardware**: one or two mainstream devices per class (section 11).
- **Back-ported EHCI stacks**: NUSB's EHCI stack on 98 SE is fully
  supported and in the test matrix.

Owner, 2026-10-22:

- **Q6. The board list.** Accepted.
  - The list has these boards besides the Uno R3 and the Pico:
    - Arduino Leonardo and Micro with their bootloader PIDs;
    - STM32 VCP `0483:5740`;
    - CH9102 `1A86:55D4`;
    - Espressif `303A`, Adafruit `239A` and SparkFun `1B4F` boards.
  - The exact PIDs come from documented sources, each recorded. They are in `src/serial/usbcom/boardids.txt`, with its sources, selection rules and file hashes. `geninf.ps1` generates the INF model sections from it, and the gates check the INFs against it.
  - Single-function CDC devices are listed as whole devices everywhere: the Caterina bootloaders, the STM32 VCP and the CH9102. That they are single-function is an inference, to be read in M0/M2.
  - Arduino-sketch PIDs (composite) are listed as whole devices on 98 SE/ME and 2000 only (the Q5 exception), plus `&MI_00` in `USBCOM.INF`. On XP and later the interface-class match serves them.
  - The Pico keeps `&MI_00` only (Owner, 2026-10-23). A pico-sdk/TinyUSB build with two CDC ports therefore gets one COM port under xHCI98, which splits the composite and leaves the second function's `&MI_02` child without a driver, and both under NUSB's EHCI, where the whole-device line binds and `USBCOM.SYS` serves both functions ([legs](../testing/2026-10-23-receiver-dualacm-sim.md), `acmemu.py --profile dual`). Stock single-CDC firmware is unaffected. An `&MI_02` line was tried and withdrawn: on stock firmware `&MI_02` is the reset interface, where `USBCOM.SYS` would fail its start (Code 10).
  - UF2 bootloader and CircuitPython PIDs are not listed.
- **Generic model.** Manual "Have Disk" selection only, under an id no device reports (section 8).

### Open questions

None at present.

## 14. Unverified, and what would settle it

| Claim | Settled by |
|---|---|
| FT232R works with FTDI 1.09.06 on 98 | Real FT232R on 98 SE |
| FT2232H works with FTDI 1.0.3 | Real FT2232H on 98 SE |
| FTDI's live "No Longer Supported" section still hosts these packages | Reading the live page in a browser |
| Current CH341SER ZIP still carries 98/ME support | Its INF |
| An official CP210x 98 package exists, and which PIDs it lists | Silicon Labs "Legacy OS Software" listing, and its INF |
| PL2303 2.0.0.19: HXD/TA/clone support; licence | Real devices on 98 SE; its installer's EULA |
| CH9102/CH343 present standard CDC-ACM | `lsusb -v` of each |
| Ports-class child over `wdmmdmld.vxd` gets a `COMn` | M0 step 3 |
| 98 SE / 2000 hub composite parent ignores IADs | Microsoft "Support for Interface Collections"; M0 |
| `IoOpenDeviceRegistryKey(PLUGPLAY_REGKEY_DEVICE)` on 98 maps to the `Enum` hardware key | Settled on 98 SE: yes (15.1) |
| What VCOMM needs for a resource-less `PortName` | Settled on 98 SE: the SYS's `PortName` write alone (15.1) |
| `_NtKernCreateFile` path form accepted for a WDM device object | Settled on 98 SE: `\Device\UsbComCOMn` opens (15.1) |
| Win16 `CBR_*` values reach `PortSetCommState` | M1 |
| VCOMM's behaviour on devnode removal with a port open | Settled on 98 SE with the simulated device: the handle stays and fails until closed (15.1); real boards M3 |
| 2000's `usbser.sys` as a Ports-class driver; XP SP3 IAD grouping; unsigned INF on 7 x64 | M0 |
| Unsigned INF on Vista x64 and 8 x64; `USBCDC.INF` on XP x64, Vista, 7 x86 and 8 | Not tested (section 11); in the release notes |
| ME: its VCOMM's `PortName` handling; the whole 98 package on ME | Settled in a QEMU guest under xHCI98 with the simulated Uno: the 98 SE behaviour, COM3 by the SYS's write ([guest legs](../testing/2026-10-22-usbcom98-legs.md)) |
| ME: whether its stack splits composites | Under xHCI98 in QEMU: xHCI98 splits the Pico model and `&MI_00` binds, as on 98 SE ([guest legs](../testing/2026-10-22-usbcom98-legs.md)); ME has no NUSB, so no other USB 2.0 stack is tested |
| Vista/7/8 `mdmcpq.inf` carries `usbser.sys` the same way | Their `mdmcpq.inf` |
| 2000 DDK `ml.exe` builds VxDs | Not needed: the VxD is C with inline assembly, linked by MSVC 6.0 `LINK /VXD` (section 15) |
| PSTN 1.2 section numbers cited (6.3.13 `SEND_BREAK`, 6.5.4 `SERIAL_STATE`); the PSTN text was not read on this host, the bit layouts match Linux `cdc-acm.c` | Reading USB-IF `PSTN120.pdf` |
| esptool DTR/RTS sequence on S2/S3 native USB | M2 |

## 15. Implementation notes, 2026-10-22

What the first build of the 98 SE / ME driver settled, and where it departs from the draft:

- **VxD without an assembler.**
  - The DDB is a C structure.
  - The control procedure and the VMM event thunks are `__declspec(naked)` functions with inline assembly.
  - Services are called through the 98 DDK `vmm.h` `VxDCall` macro.
  - MSVC 6.0's `LINK /VXD` links the image from `usbcomv.def`, which follows "Building a Virtual Device".
- **When the VxD calls the SYS (5.1.2).**
  - The VxD calls the table only from `PortOpen`, `PortClose` and its own global-event callback, never at interrupt time.
  - The interrupt-time port functions change only the VxD's own state and schedule the event.
  - The SYS turns every table call into work for its DPC; all USB I/O starts there, in URB completion, or in the pipe-reset work item.
- **Removal (7.9).** This replaces the shared reference count.
  - After the SYS calls `Gone`, the VxD never calls the table again, not even `Detach`.
  - The SYS frees the port once its I/O has drained. The SYS image may unload after that, so the VxD must not hold a pointer into it.
- **Line coding order (7.6).**
  - `SET_LINE_CODING` and `SET_CONTROL_LINE_STATE` go through one FIFO of control operations, one at a time and in call order.
  - `PortSetCommState` must be callable at interrupt time, so it cannot wait for the transfer. The order is kept, but the call returns before the device has the new rate.
- **PortName (5.1.4).**
  - `USBCOM.SYS` chooses the number. It writes `PortName` and `FriendlyName` through `IoOpenDeviceRegistryKey(PLUGPLAY_REGKEY_DEVICE)` on every PnP IRP of the child until the write succeeds.
  - If the Enum tree cannot be read, it starts at COM5.
  - The VxD-as-`Enumerator` fallback is not implemented. It is built only if M1 shows the SYS write comes too late for VCOMM.
  - The child's instance id is `<VID>_<PID>_<driver-key tag>_<n>`, so two identical boards get different ids.
- **SERIALCOMM.** The SerialComm value is written as volatile. It cannot be removed when the board goes, because the 98 `WDM.H` declares no `ZwDeleteValueKey`.
- **The generic model in 8.** `ExcludeFromSelect` hides a model from the user's list; it does not keep it out of automatic matching, so the draft's wording did not describe a working mechanism. The owner settled it on 2026-10-22 (section 13).
- **`DriverControl` arguments.** The help (`comm_12pc`) and the `vcomm.h` comment give two different orders for the arguments after `ReferenceData`. The VxD takes as the port name whichever candidate argument is a pointer (an I/O base and an IRQ fit 16 bits). M1 records which order VCOMM really uses.
- **Power (7.10).** The FDO maps S-IRPs to D-IRPs with `PoRequestPowerIrp`. D3 cancels the data transfers; D0 resends the last line coding and control lines and restarts.

### 15.1 What the 98 SE guest legs settled, 2026-10-22

Read in 98 SE guests (QEMU, xHCI98 3.1.0.0 and NUSB 3.3's EHCI) with this
package's simulated CDC-ACM device (`acmemu.py`), the trace flavour's 0xE9
lines and `COMTEST.EXE` ([guest legs](../testing/2026-10-22-usbcom98-legs.md)).

- **The VxD is `USBCOMV.VXD`, not `USBCOM.VXD`** (module and DDB `USBCOMV`).
  NTKERN lists a loaded WDM driver with VXDLDR under its module name, so with
  `USBCOM.SYS` loaded, VCOMM's load of a VxD also named `USBCOM` found the
  SYS's entry and sent it `Sys_Dynamic_Device_Reinit`: fatal exception 0E at
  `0028:53F000FF` on every open. A port driver's module name must differ from
  its WDM driver's.
- **The VxD stays loaded once loaded.** VCOMM loads the port driver at an
  open and unloads it when its last port closes (`comm_0sqg`), but it keeps
  the `PortName` pointer given to `_VCOMM_Add_Port` and compares it at the
  next open. A reload at another address left it dangling (fatal exception 0E
  in VCOMM). The control procedure refuses `Sys_Dynamic_Device_Exit`
  (`kernel_9ipk` allows it), so every pointer VCOMM holds stays valid.
- **`DriverControl` order.** VCOMM calls `DC_Initialize` with
  `(hDevNode, RefData, AllocBase, AllocIrq, PortName)`: the `vcomm.h`
  comment's order, not `comm_12pc`'s. A load other than by a VCOMM open
  (a Win32 `CreateFile` of the file) gets `hDevNode` 0 and no port; it adds
  none.
- **`PortName` (5.1.4): the SYS's write is enough.**
  `IoOpenDeviceRegistryKey(PLUGPLAY_REGKEY_DEVICE)` on the child PDO opens its
  `Enum` hardware key on 98 SE, the write lands during the child's first
  `QUERY_ID`, before VCOMM loads the devnode, and VCOMM opens the port by that
  name. The VxD-as-`Enumerator` fallback and a static VxD are not needed. The
  Enum scan reads `HKLM\Enum` through `ZwOpenKey`/`ZwEnumerateKey`.
- **The child gets no `IRP_MN_START_DEVICE` on 98 SE.** Its loader is VCOMM,
  not NTKERN; Device Manager still shows it working. When its driver is
  installed it gets `IRP_MN_REMOVE_DEVICE` and no start after. The ports
  therefore run from the FDO's start, and only the FDO's stop or removal
  tells an open port it is gone.
- **Removal (7.9).** A pull sends the FDO `IRP_MN_REMOVE_DEVICE` (no
  `STOP` before it; 98 has no `SURPRISE_REMOVAL`); the open port gets `Gone`,
  its handle fails until closed, and VCOMM does not close it for the
  program. The replugged board gets the same `COMn` and opens.
- **`Contention=*vcd` works** for a port without resources; it stays, as
  FTDI has it.
- **VCOMM's Win32 `WriteFile` calls `PortWrite` once** and then only waits for
  the transmit queue to drain; what is not taken is reported as a failed
  write (Microsoft's `serial.vxd` took a 16384-byte write whole). The VxD's
  own transmit queue is therefore at least 64 KiB. A larger single write is
  still cut at 64 KiB: a known limit. `CE_TXFULL` on a partial accept changes
  nothing in VCOMM.
- **`PDVersion` 0x0100** is accepted by VCOMM; no other value was tried.
- **The child installs with a second wizard.** 98 SE runs the Found New
  Hardware wizard again for `USBCOM\COMPORT`; it finds `USBCOMP.INF` in the
  same folder by itself, but the user clicks through it (section 8 expected
  no second prompt).


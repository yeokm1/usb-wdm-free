# USB CCID smart card readers (ccid98): reading and design

Status: reading and design of 2026-11-06, reviewed and revised. No code.
**Dropped (owner, 2026-11-06): Microsoft's `usbccid.sys` covers 2000 and
XP** (it can still be downloaded from the Microsoft Update Catalog,
section 2), so no free reader driver is built for them. 98 SE and ME:
option A of section 7.7, no replacement for the Base Components (owner,
2026-11-06); the reader design of section 4 is kept for the day 9x is
taken up with Microsoft's Base Components.

## 1. Purpose and devices served

USB smart card readers that follow the USB Chip/Smart Card Interface
Devices (CCID) class, 1.1 (interface class `0B`): nearly every USB reader
made since about 2003 (SCM/Identiv SCR3310, HID Global OMNIKEY 3021/3121,
Gemalto IDBridge/GemPC Twin, ACS ACR38/ACR39, Cherry keyboards with a
reader, Yubikeys and other tokens in their CCID mode). A reader is useful
only with a PC/SC application above it: a certificate store, a login, a
signing tool, a card utility.

## 2. What each system has (reading)

Install media (file names and INF text only):

| | 98 SE | ME | 2000 SP4 | XP SP3 |
|---|---|---|---|---|
| PC/SC resource manager (`scardsvr`), `winscard.dll` | no | no | yes | yes (`SCARDSVR.EX_`, `WINSCARD.DL_` in `I386`) |
| Reader driver library `smclib.sys` | no | no | yes | yes (`SMCLIB.SY_`) |
| A CCID class driver | no | no | no | no: `SMARTCRD.INF` lists a few vendor readers by id (Gemplus GemPC430 `08E6:0430`, the AMEX reader `08E6:0435`, SCM `0066:1001` with `stcusb.sys`, serial and PC Card readers), no `USB\Class_0B` match |

- **98 SE and ME**: Microsoft shipped the PC/SC stack for 95, 98, ME and
  NT4 separately, as the "Smart Card Base Components" (`scbase.exe`,
  version 4.71.1015, a Platform SDK redistributable of the period). It
  is not on the CDs, Microsoft no longer offers it from its own site as
  far as found, and it is Microsoft's, so this repository cannot ship it.
  The Windows 98 Smart Card DDK (`HELP_SCDDK.CHM`) documents reader
  drivers for that stack as either WDM drivers on `smclib.sys` or VxDs on
  `smclib.vxd`: "The smart card resource manager can communicate with
  WDM and VxD drivers" (`scovr_3uwj.htm`); a WDM reader is reached through
  `\DosDevices\SCReader<n>` (`scovr_4uyb.htm`), a VxD one through
  `HKLM\System\CurrentControlSet\VxD\Scmlib\Devices\SCReader<n>`
  (`scovr_06k3.htm`). Which of the two libraries the Base Components put
  on a 98 SE system is not read here.
- **2000 and XP**: the resource manager and `smclib.sys` are in the box;
  a reader driver is a WDM driver in the `SmartCardReader` class
  (`{50DD5230-BA8A-11D1-BF5D-0000F805F530}`), its service in the reader
  load-order group (the 98 Smart Card DDK page `scovr_3e7m.htm` spells it
  "Smart Card Reader"; Microsoft's later documentation spells it
  `SmartCardReader`; XP's own `smartcrd.inf` sets no group, so the value
  is read from a working XP reader before an INF names it), that answers the
  `IOCTL_SMARTCARD_*` requests (`scovr_5neb.htm`), usually by handing
  them to `smclib.sys`'s `SmartcardDeviceControl`, which calls the
  driver's `RDF_*` callbacks (`scovr_44yt.htm`, `scovr_581f.htm`).
  Microsoft later offered a CCID class driver (`usbccid.sys`) through
  Windows Update; it is not on the XP SP3 CD. The Microsoft Update
  Catalog still offers it (read 2026-11-06, section 2's table).

Makers' drivers (owner policy of 2026-11-06; makers' sites, not
downloaded):

| Maker | Old-Windows drivers | Read |
|---|---|---|
| SCM Microsystems / Identiv (SCR3310 and family) | 4.x for 98 and ME (third-party listings name `SCRx31_4.24.00.01`), 8.x installers for 2000 to 11 | Identiv's support site (`support.identiv.com`) refused the connection from this host; not read |
| HID Global (OMNIKEY 3x21) | its driver list has 2000/XP entries (workbench tools), current CCID drivers for 7 and later; no 98/ME entry found | terms not read |
| Gemalto / Thales (GemPC, IDBridge) | not searched | - |
| ACS (ACR38 and family) | ACS's product documentation names CCID drivers for 98, ME, 2000 and XP | packages and terms not read |
| Microsoft | "USB Smart Card reader" (catalog update `8a9785ae-b679-4da9-a627-1c84424d556d`, 2005-05-17, for 2000, XP and Server 2003): `usbccid.sys` 5.2.3790.2444, a kernel driver matching `USB\Class_0B&SubClass_00`; the cab `10373903_41f8f3383ea94733495ae519f7cf458edb85de77.cab`, 25,886 bytes, SHA-256 `3cf8d69b185f909a6012c34cbd7b3c84d3fd6cb676785fc270a235c011517f0d`, from `catalog.s.download.windowsupdate.com/msdownload/update/v3-19990518/cabpool/`. Also "Microsoft Usbccid Smartcard Reader (WUDF)" 6.0.6001.18176 for XP (update `1ae5fdcc-7718-4991-a1bd-bc1c47d2f624`, 2,808,885 bytes, SHA-256 `7a3af6572cf6ab953039f97e4960d87c7467178b2958c13261689dfc932a243d`, a user-mode driver with Microsoft's framework co-installers) | the cabs hold no licence text; the catalog's FAQ says "Software updates you receive through the Microsoft Update Catalog are governed by any license terms that come with them. If no license terms come with them, they may be governed by the original license agreement for the software to which they apply." usb-wdm-nonfree carries both, unpacked (owner, 2026-11-06) |

The survey is incomplete (no maker's terms are recorded, Gemalto not
searched): finishing it, with exact packages, the systems each covers and
its terms, is the first step if the owner takes this up (Q2).

So on 2000 and XP many readers have a maker's driver, and Microsoft's
own `usbccid.sys` covers the class there (the free reader was dropped for
them), and on 98 SE and ME the
missing piece is first of all Microsoft's resource manager, not a reader
driver.

QEMU's `usb-ccid` (`hw/usb/dev-smartcard-reader.c`, read for its
behaviour): a CCID 1.1 reader `08E6:4433` with one slot, T=0 only,
`dwFeatures` `000104FE` (automatic parameters, voltage, clock, baud rate,
PPS, IFSD; TPDU-level exchange), bulk OUT/IN and an interrupt IN for slot
changes; its card is `ccid-card-emulated` (an NSS certificate store on the
host) or `ccid-card-passthru`. The usb4-emu fork build used for this
repository's legs has `usb-ccid` but not `ccid-card-emulated`; this host's
QEMU 11.1.0 has both, but it hung at 98 SE's boot logo on the q35 base.

## 3. The CCID protocol the driver needs (USB CCID 1.1)

- Descriptors: the CCID class descriptor (21h, 54 bytes: slots,
  protocols, clocks, data rates, IFSD, `dwFeatures` with the exchange
  level, `dwMaxCCIDMessageLength`), bulk OUT, bulk IN, optional
  interrupt IN.
- Messages (10-byte header, sequence number, slot): PC_to_RDR_IccPowerOn
  (62h), IccPowerOff (63h), GetSlotStatus (65h), XfrBlock (6Fh),
  GetParameters (6Ch), SetParameters (61h), ResetParameters (6Dh), Escape
  (6Bh), Abort (72h); replies RDR_to_PC_DataBlock (80h), SlotStatus (81h),
  Parameters (82h), Escape (83h); the time-extension status; the
  interrupt NotifySlotChange (50h) and HardwareError (51h); the class
  request ABORT (01h).
- The exchange level decides the host's work (CCID 1.1 section 3.2):
  a TPDU-level reader (QEMU's) handles T=0's procedure bytes itself and
  takes a T=0 TPDU (header and data) per XfrBlock; the host builds the
  TPDUs and, for T=1, runs the block protocol (I/R/S blocks, chaining,
  IFSD). Status words such as 61xx and 6Cxx are returned to the caller as
  Windows' `SCardTransmit` contract for T=0 has it (the application sends
  GET RESPONSE), not handled by the driver. A short-APDU-level reader
  takes a whole short APDU; an extended-APDU-level reader can need CCID
  message chaining (`wLevelParameter`, `bChainParameter`); v1 would serve
  TPDU and short-APDU readers and refuse extended APDUs it cannot chain
  with a status the caller sees. Character-level readers are not served.
- From the ATR (ISO/IEC 7816-3): the protocols offered, TA1 (Fi/Di), TC1,
  TA2 (specific mode), T=1 IFSC and BWI/CWI, for SetParameters and the
  `SCARD_*` attributes.

Facts may be taken from pcsc-lite's CCID driver (`LudovicRousseau/CCID`,
LGPL-2.1-or-later, a licence AGENTS.md allows to adapt) and Linux has no
CCID kernel driver.

## 4. The decision on the OS-facing model

**Decision (proposed): one WDM reader driver, `ccid98.sys`, in the
`SmartCardReader` class, built on Microsoft's reader library `smclib.sys`
as the DDK documents a WDM reader (`SmartcardInitialize`,
`SmartcardDeviceControl`, the `RDF_*` callbacks): the driver does the CCID
transport, the power, the protocol selection and the transmit callbacks;
the library does the IOCTL checking and the ATR parsing.**

This section is an outline, not a design ready for code (the driver was
dropped before that stage). A design pass before any code settles at
least:

- **Transmit per exchange level**: for a TPDU-level reader the driver
  drives `smclib.sys`'s T=1 loop (`SmartcardT1Request` and
  `SmartcardT1Reply`) and sends each block; for a short-APDU-level reader
  it sends the APDU with no block framing; in both, the `SCARD_IO_REQUEST`
  header is kept and the shared request and reply buffer is respected.
- **Card tracking**: how the CCID slot-change notification and the slot
  status update `ReaderCapabilities.CurrentState`, drop a removed card's
  state, and claim and complete the pending tracking request exactly once
  against cancel, cleanup and removal.
- **Reset and power**: cold reset (power off, then PowerOn), warm reset
  (PowerOn on an active slot) and power-down mapped separately; the ATR
  copied to the reply and to `CardCapabilities.ATR`; card state dropped and
  rebuilt across suspend and resume.
- **Installation on 2000 and XP**: besides the `SmartCardReader` class and
  the service, the resource manager's configuration Microsoft documents
  for reader installation before Windows 7 (the Calais reader entries and
  the `SCardSvr` start type).

- On 2000 and XP `smclib.sys` is in the box; the driver serves Windows'
  own resource manager through `\DosDevices\SCReader<n>`.
- On 98 SE and ME the same WDM driver serves the Base Components' resource
  manager, which the 98 Smart Card DDK says talks to WDM readers by the
  same device name, if `smclib.sys` is present there. Whether the Base
  Components install it, or which package of Microsoft's does, is not
  read (section 7.1).

Options considered: (a) the decision above; (b) a library-free driver
that implements the `IOCTL_SMARTCARD_*` contract itself (ATR parsing,
`SCARD_*` attributes, T=1), which needs no `smclib.sys` on 9x but departs
from the documented model and must then reproduce the library's checks
and synchronisation (about twice the code, and a compatibility question
of its own with the resource managers); (c) a VxD reader on `smclib.vxd`
for 9x plus (a) for NT (two drivers). (a) is proposed; (b) is the
fallback only if 98 SE must be served and no Microsoft package that the
user can obtain brings `smclib.sys`.

## 5. Size, testing and risks (estimate)

Written before the owner dropped the 2000 and XP reader (section 6); the
2000 and XP parts below are kept as the estimate that decision was taken
on, not as planned work. A 9x reader, if taken up, would reuse the 98 SE
and ME parts.

| Part | Size (estimate) |
|---|---|
| CCID transport (descriptors, bulk message exchange with sequence and time extension, slot-change interrupt, abort, stale sequence numbers) | 700-900 lines |
| Driver lifetime: PnP start/stop/surprise removal, power, create/close/cleanup, the outstanding bulk and interrupt requests, cancel of a pending tracking or transmit request racing removal | 600-800 lines |
| The `smclib.sys` callbacks (power, set protocol, transmit for T=0 TPDUs and T=1 through the library, short APDUs, card tracking) | 600-900 lines |
| Option (b) only: the IOCTL contract, ATR parser and T=1 of its own | 1,500-2,200 lines more |
| INFs (98 SE/ME with the Base Components' registry; 2000/XP `SmartCardReader` class) | 2 files |
| `ccidemu.py`, a usb-redir CCID reader with a simple card (T=0 and T=1 cards, APDU- and TPDU-level reader shapes, insert/remove) | 600-900 lines |
| `SCTEST.EXE` (winscard: list readers, wait for a card, connect, transmit SELECT and READ BINARY, status; then the failure cases below) | 400-600 lines |

Test cases beyond the successful exchange, each with its expected
outcome: card removal during a transmit and during a pending
is-present/is-absent request; reader unplug with a handle open; close and
reopen; suspend and resume with a card in; a malformed or oversized reply
length from the reader; a stale sequence number; a T=1 R-block retry and
a resynchronisation; an extended APDU on a short-APDU reader (refused).

Legs (2000 and XP, superseded by the drop): Windows' EHCI and xHCI98 with
the simulator and QEMU's `usb-ccid` (with `ccid-card-emulated` from a QEMU
build that has it). When 98 SE and ME are taken up (section 7): 98 SE
under NUSB's EHCI and xHCI98, ME under xHCI98 and Windows' own UHCI
(NUSB does not install on ME; an EHCI leg on ME needs a board or card
maker's USB 2.0 stack for ME, of which some shipped one, deferred until
one is at hand), each with the PC/SC layer the owner chooses.

Risks: the resource manager's undocumented expectations beyond the DDK's
IOCTL pages (state transitions, cancel of the tracking request,
`SCARD_ATTR_*` values it requires); T=1 on real cards; readers whose
`dwFeatures` ask for host-side PPS; the 9x Base Components' behaviour with
a WDM reader (documented, never read here).

## 6. Owner decisions and questions

Owner, 2026-11-06, in order: first, build the reader driver for 2000
and XP and estimate a Base Components replacement for 98 SE and ME
(section 7); then, before any code: check whether Microsoft's
`usbccid.sys` can still be downloaded for 2000 and XP, and drop the free
reader if it can; 98 SE and ME: option A of section 7.7. The catalog
offers it for both (section 2), so: **dropped (owner, 2026-11-06):
Microsoft's `usbccid.sys` covers 2000 and XP.** usb-wdm-nonfree carries
both catalog cabinets, unpacked, in `non-free/smartcard/usbccid/` (owner,
2026-11-06; the terms are in section 2). The XP user-mode build there
matches ICCD tokens only (`USB\Class_0B&SubClass_00&Prot_01` and
`&Prot_02`, read in its INF); the kernel driver matches every CCID
reader.

## 7. 98 SE and ME: a free replacement for the Smart Card Base Components (estimate)

Asked by the owner on 2026-11-06: could this repository build its own
replacement for Microsoft's Smart Card Base Components, for 98 SE and ME
users who do not have them? An estimate and a feasibility answer; nothing
is built.

### 7.1 What the Base Components are

From the DDK documentation and the public descriptions of the package
(the package itself was not obtained: no Microsoft URL for it was
found, and the Internet Archive holds no copy of Microsoft's own download,
section 7.6):

- **`winscard.dll`**, the PC/SC API applications call: about 60
  functions (`SCardEstablishContext`, `SCardListReaders`,
  `SCardGetStatusChange`, `SCardConnect`, `SCardBeginTransaction`,
  `SCardTransmit`, `SCardControl`, `SCardGetAttrib`, `SCardStatus`,
  `SCardDisconnect`, the A and W forms, and the database functions
  `SCardIntroduce*`, `SCardListCards`, `SCardLocateCards`).
- **The resource manager** (`scardsvr.exe`; on 2000 and XP the
  "Smart Card" service): it owns the readers, tracks card insertion and
  removal, gives each card to one connection at a time or shares it, and
  carries transactions. On 9x it runs as a background process.
- **The reader driver library**: `smclib.vxd` for VxD readers and
  `smclib.sys` for WDM readers; a reader maker's installation guide of
  the period asks for `smclib.vxd` 4.00.951 in `WINDOWS\SYSTEM` and
  `smclib.sys` 5.00 in `SYSTEM32\DRIVERS` after the 9x PC/SC components are
  installed (read in a third-party copy of that manual; to be checked
  against the package).
- **The contract readers' drivers expect** (98 Smart Card DDK): a WDM
  reader under `\DosDevices\SCReader<n>`, a VxD reader as
  `SCReader<n>=driver.vxd` under `HKLM\System\CurrentControlSet\VxD\
  Scmlib\Devices`, the `IOCTL_SMARTCARD_*` requests, the "Smart Card
  Reader" group; the card database under
  `HKLM\Software\Microsoft\Cryptography\Calais` (readers, known cards by
  ATR and mask, each card's cryptographic provider).
- Helpers: `scarddlg.dll` (the card selection dialog) and the smart card
  service providers (`scardssp.dll` and the others, COM objects that few
  programs use).

### 7.2 What a free replacement must provide

The programs people would run on 98 SE and ME with a reader: PC/SC
tools (card and SIM editors, diagnostic programs), home banking clients
of the period that speak PC/SC directly, and cryptographic programs that
go through a card maker's CSP or PKCS#11 module, which in turn calls
`winscard.dll`. All of them need the core API with Microsoft's behaviour
(the state flags of `SCardGetStatusChange`, sharing modes and
transactions, `SCARD_ATTR_*` values, error codes); few need the database
functions beyond `SCardListReaders` and `SCardListCards`, the dialog or
the service providers. CryptoAPI programs also need the card's CSP, which
the Base Components never contained (the card maker's); PKCS#11 programs
need the card's PKCS#11 module instead.

### 7.3 Open implementations and what may be used

- **pcsc-lite** (the resource manager and `winscard` API of Linux and the
  BSDs): its core is under a BSD-style licence, which AGENTS.md lists as
  one that may be adapted here with its notices kept; a few of its files
  are under other licences and would be checked one by one.
- **The CCID driver of pcsc-lite** (`libccid`): LGPL-2.1-or-later, also
  adaptable here.
- **Wine's `winscard`** (LGPL-2.1-or-later): a thin layer over pcsc-lite
  on its hosts, with little of the resource manager in it.
- **ReactOS's `winscard`**: largely stubs.
So the PC/SC logic could be adapted from pcsc-lite; the Windows side
(the DLL's exports and Microsoft's exact behaviour) has to be written
from Microsoft's published API documentation.

### 7.4 What 9x makes hard

- **No services**: a resource manager would be a process started from
  `RunServices` that also calls `RegisterServiceProcess` (without it the
  process ends at log-off), with no service control.
- **No named-pipe servers on 9x**: pcsc-lite's clients reach its daemon
  over Unix-domain sockets, which need replacing. Ways: (i) a serverless
  design, each `winscard.dll` instance talking to the reader driver
  directly and the instances arbitrating through a shared memory section,
  named mutexes and events; (ii) a user-mode broker reached over loopback
  TCP (98 SE and ME have Winsock) or over request queues in shared memory;
  (iii) the broker in ring 0 (the reader driver, or a small VxD) keeping
  the per-card state. (i) has no process whose death strands the others
  and is taken for the estimate; (ii) is closer to pcsc-lite's own shape
  and would cost about the same.
- **ANSI only**: the W functions convert on 9x.
- **The reader library**: a free CCID reader on 9x without Microsoft's
  package would need the
  library-free form (section 4, option b) or a free `smclib.sys` that
  other makers' WDM reader drivers could use too (larger).
- **Coexistence**: a free `winscard.dll` in `WINDOWS\SYSTEM` collides
  with Microsoft's if a user later installs it; the INF must refuse to
  replace a Microsoft copy, and vice versa is not under our control.

### 7.5 Size (estimate)

| Part | Files | Functions | Lines | Worker-days |
|---|---|---|---|---|
| `winscard.dll`: the core API (A and W), the database read functions, error and attribute mapping | 4-6 | 45-60 | 2,500-3,500 | 8-12 |
| Arbitration: shared section, per-reader state, sharing modes, transactions, card tracking threads, crash recovery of a dead owner | 3-4 | 30-40 | 1,500-2,500 | 8-12 |
| Reader access on 9x: the CCID driver's library-free build (section 4, option b) | 2-3 | 30-40 | 1,500-2,200 | 8-10 |
| Install and registry contract (INF, `RunServices`, the Calais keys, coexistence checks) | 2 | - | 300-500 | 2-3 |
| A free `smclib.sys` for other makers' WDM readers (optional) | 3-4 | 15 exported, about 40 internal | 2,000-3,000 | 10-15 |
| Tests: an API test program, the simulator's card cases, two or three real 9x programs | 2-3 | - | 800-1,200 | 5-10 |

Without the optional `smclib.sys`: about 31-47 worker-days (the sum of
the rows); with it, 41-62. Not in these figures: the database write
functions (`SCardIntroduce*`, `SCardForget*`), the card selection dialog
(`scarddlg.dll`), the service provider COM objects and VxD readers on
`smclib.vxd`; each would be estimated on its own if wanted.

### 7.6 Microsoft's terms

The Base Components were offered as a free download and were bundled on
reader makers' CDs of the period; their licence text was not read: no
copy of Microsoft's own download is in the Internet Archive (searched
2026-11-06 under `download.microsoft.com` and `microsoft.com`), so the
package was not obtained (owner, 2026-11-06: only from such a copy).

### 7.7 Options and recommendation

- **A. No replacement**: 98 SE and ME users bring Microsoft's Base
  Components; a free CCID reader for them (section 4, option a, on
  `smclib.sys` from Microsoft's package) is built only if 9x smart card
  readers are taken up later.
- **B. A minimal free PC/SC layer** for the PC/SC tools and banking
  clients (section 7.5 without the optional `smclib.sys`: the core API,
  serverless arbitration, the library-free reader, install and tests):
  about 31-47 worker-days.
- **C. B with a free `smclib.sys`** for other makers' WDM readers: about
  41-62 worker-days. A full replacement (section 7.5's items left out
  too) would be more.

Recommended: A now. B only if the owner names 9x programs that people
run and that need it, once A's legs show the driver side works; C not.

**Owner, 2026-11-06: A.** B and C stay as estimates.

- Q3 (the Base Components' terms) is open only if a copy of
  Microsoft's own download turns up.

# USB Ethernet drivers

Status: design draft, 2026-10-20. Implementation, 2026-10-22: `cdcnet`
0.1.0.0 exists in `src/net/cdcnet/` (ECM, NCM, RNDIS; NDIS 5.0 x86 and 5.1
x64 builds; `CDCNET.INF`, `CDC98.INF`, `CDCRNDIS.INF`), with the core kept
inside the package for now rather than in `src/net/common/` (section 3.2).
Its runs are in `docs/testing/2026-10-22-cdcnet.md`. `rtl815x` has no code;
`ax88179` is separate work. 2026-11-06: `ax88772`, the ASIX AX88772B and
AX88772C on 98 SE and ME (section 6.8); the CoreChips SR9800 in it and
`sr9700` for the SR9700, 98 SE to 7 (section 6.9).

Free NDIS miniport drivers for the USB Ethernet adapters and USB network links
people can buy or use today, which Windows 98 SE has no driver for, and which
ME and 2000 (and in places XP to 7) also lack. The targets are Windows 98 SE
through Windows 7 for `cdcnet`, and Windows 98 SE, ME and 2000 for
`rtl815x` and `ax88179` (Owner, 2026-10-20; section 1).

Where this document says "Linux" it means `drivers/net/usb/` at Linux master
`f47d223627f80cb7a1eb14c585fe6cb35de5e984` (2026-10-20, after tag `v7.3`).
"The 2000 DDK network guide" is `d_net.chm` from the Windows 2000 DDK
documentation; its pages are cited by their
section number and file name inside the CHM. "The 98 DDK guide" is
`HELP_WIN98DDK.CHM` from the locally installed Windows 98 DDK. Toolchain
installation follows [build.md](build.md); paths in another checkout are
never build inputs (AGENTS.md, "Standalone").

Which Windows versions each USB Ethernet chip already has a driver for (in
the box, from its maker, or here), and the gaps: [ethernet-coverage.md](ethernet-coverage.md) (2026-10-27).

## 1. Summary of the decisions

1. **Three packages, sharing one NDIS core compiled into each.**
   `src/net/cdcnet/` (CDC-ECM, CDC-NCM and RNDIS, the standard protocols),
   `src/net/rtl815x/` (Realtek RTL8152/8153 family in its vendor mode) and
   `src/net/ax88179/` (ASIX AX88179/AX88178A). A shared library,
   `src/net/common/`, holds the NDIS upper edge, the USB lower edge, the
   receive and send pipelines and the OID handling; each package links it
   statically and adds only its framing and its chip setup. Owner,
   2026-10-20: these three packages on a shared core in `src/net/common/`,
   the `src/<class>/common/` pattern `README.md` allows. Section 3.
2. **The smallest useful driver is `cdcnet`, and it already covers the
   RTL8153.** The RTL8153 offers a standard CDC-ECM configuration beside its
   vendor one (Linux `cdc_ether.c` lists `0BDA:8153` with interface class
   `02/06/00` and leaves it to `r8152.c` only because the vendor mode is
   preferred). A `cdcnet` that selects the ECM configuration drives most
   modern USB-C gigabit dongles at once, before any Realtek register is
   touched. `rtl815x` follows for speed (receive aggregation), not for
   coverage. Owner, 2026-10-20: `cdcnet` takes the RTL8153 until `rtl815x`
   ships, then only `rtl815x` does (section 6.5).
3. **NDIS 5 only, Windows 98 SE through Windows 7.** Owner, 2026-10-20:
   NDIS 5 and nothing newer: one x86 NDIS 5.0 binary per package with a
   WDM lower edge, deserialized, built with the Windows 2000 DDK; for
   `cdcnet` it serves 98 SE, ME, 2000, XP, Vista and 7 x86 (on Vista and 7
   through NDIS 6's translation layer), and `cdcnet` alone also has an x64
   NDIS 5.1 build for XP x64, Vista x64 and 7 x64. `rtl815x` and `ax88179`
   stop at 2000 (item 8). There is no NDIS 6 code and Windows 8 is not a
   target (Microsoft says Windows 8 removed NDIS 5.x). Section 4.
4. **v1 offloads nothing**: 1500-byte MTU, no jumbo frames, no checksum or
   segmentation offload, no VLAN, no wake-on-LAN. Link state, multicast
   filtering, surprise removal and a system sleep/resume cycle are in v1.
5. **Not planned** (Owner, 2026-10-20): a free AX88772 driver (ASIX's own
   vendor driver stays in the separate non-free pack, which this
   repository does not depend on); the DM9601/SR9700/CH9200 clones; and
   Android tethering under Microsoft's 98 SE, ME and 2000 USB stacks.
   Tethering is served under xHCI98 and on XP and later. Sections 2.4 and
   2.6. Owner, 2026-11-06 (xHCI98 roadmap 47.26 and 47.27): a free driver
   for the AX88772B and AX88772C on 98 SE and ME, which ASIX's drivers
   leave out (section 6.8), and for the CoreChips SR9700 and SR9800;
   the AX88772 and AX88772A stay with ASIX's driver.
6. **iPhone tethering is not RNDIS.** It is Apple's own vendor class
   (`FF/FD/01`, Linux `ipheth.c`) and needs the phone paired and trusted
   first. Out of scope; section 2.4.
7. **RNDIS on XP to 7 is an INF over Windows' own driver.** Owner,
   2026-10-20, after the survey of existing drivers (`existing.md` section
   4): XP, Vista and 7 have an RNDIS driver in the box that only lacks a
   device INF, so `cdcnet` carries `CDCRNDIS.INF`, which binds Windows' own
   RNDIS driver by the RNDIS class ids. `cdcnet`'s own RNDIS code targets
   98 SE, ME and 2000 only. Whether Windows' own driver serves Android
   tethering through that INF is verified in M2 (XP) and M5 (7 x64); on
   XP x64, Vista and 7 x86 it stays unverified. Section 7.5.
8. **`rtl815x` and `ax88179` target 98 SE, ME and 2000 only.** Owner,
   2026-10-20, after reading the vendors' own drivers (`existing.md`
   sections 4.1 and 4.2): Realtek and ASIX both ship drivers for XP to 7.
   - Realtek: XP 5.23, Vista 6.27, 7 7.61.20. Their INFs cover `0BDA:8152`,
     `0BDA:8050` and `0BDA:8153` at every chip revision; the 7 driver adds
     `8053`, `8155`, `8156` and `8056`.
   - ASIX: AX88179/AX88178A, XP and Vista 1.4.2.0 (x86) and 1.6.2.0 (x64),
     7 1.12.11.0 (x86) and 1.14.11.0 (x64).
   - Their INFs do not list the AX88179A or AX88179B, so on XP to 7 those
     chips go through `cdcnet`'s NCM (section 2.2), as they do on every
     target.

   XP to 7 are therefore not targets of `rtl815x` and `ax88179`.

   Owner, 2026-10-27: `rtl815x` also serves XP to 7, for the chips
   Realtek's XP to 7 drivers do not list: the RTL8153D, 8156 and 8156B on
   XP and Vista, the RTL8157 and 8159 on XP, Vista and 7, x86 and x64. Its
   RTL8152 and RTL8153 ids stay off XP to 7 (section 7.6).

## 2. Purpose and devices served: the survey

What decides each candidate: can it be bought today, is there a free and
mature reference (Linux) for it, how much protocol it
takes, and whether some Windows target already has a driver.

### 2.1 What the targets already have

| System | USB Ethernet in the box | Source |
|---|---|---|
| 98 SE | None. Its only USB-CDC INF is `usbcdc.inf`, a modem INF binding three vendor ids to `usbser.sys` | 98 SE's in-box `usbcdc.inf`; xHCI98 `docs\contributing\build-and-test.md` line 1476: "neither target ships an RNDIS host driver" (98 SE, 2000) |
| 98, 98 SE, ME, 2000 | RNDIS only through Microsoft's separately distributed "Remote NDIS USB Driver Kit" (`usb8023w.sys`/`rndismpw.sys` for 98, `...m.sys` for ME, `...k.sys` for 2000), which a device vendor had to ship | Linux `Documentation/usb/linux.inf` at tag `v2.6.20` (sections `RNDIS_CopyFiles_98`, `_ME`, `_NT`) |
| XP | RNDIS: `usb8023.sys`/`rndismp.sys` are in the box, but a device still needs a vendor INF that names them | same `linux.inf`: "Microsoft ... bundled them into XP"; its XP section has "no copyfiles - the files are already in place". XP's own `netrndis.inf` was not read (no XP INF tree on this host) |
| 7 | RNDIS by INF `rndiscmp.inf`, matching only `USB\Class_EF&SubClass_04&Prot_01` and the Microsoft OS descriptor id `USB\MS_COMP_RNDIS&MS_SUBCOMP_5162001`, not Android's `E0/01/03` | `xhci98\tools\win7-x86-extracted\inf\rndiscmp.inf` |
| Vista | `wceisvista.inf` (Windows Mobile RNDIS) | present in `xhci98\tools\vista-x86-extracted\inf\`; its match list was not read |
| any 98 SE - 8 | No CDC-ECM or CDC-NCM class driver | xHCI98 design record 13 section 10.8, item 3: "no stock INF on any target binds a CDC class id", read over the INF set that record inventories (its section 10.6 table), not every INF of every system. Windows 8's INF set was not read; unverified for 8 |
| any | No Realtek RTL815x or ASIX AX88179 driver | xHCI98 `docs\contributing\test-equipment.md` row 10: "ASIX parts have Win98 and Win2000 drivers, and the RTL8153 in most modern USB-C dongles does not". The vendors' own XP to 7 drivers exist and are non-free (section 1 item 8; `existing.md` sections 4.1 and 4.2); none exists for 98 SE, ME or 2000 |

### 2.2 Candidates

| Family | Bought today | Linux reference (SPDX) | Protocol size | Verdict |
|---|---|---|---|---|
| **CDC-ECM** (`02/06/00` + data `0A/00/00`) | Built into many adapters as a second configuration (RTL8153, the ASIX AQC111U/AQC112U 5G/2.5G parts, Lenovo and other dock NICs listed in `cdc_ether.c`), Linux/Raspberry Pi USB gadgets, some phones and modems, QEMU `usb-net` | `cdc_ether.c` (GPL-2.0-or-later), on `usbnet.c` (GPL-2.0-or-later) | Smallest: one Ethernet frame per bulk transfer, five class requests, two notifications | **v1** in `cdcnet` |
| **CDC-NCM** (`02/0D/00`) | 2.5G dongles and docks, newer phones in some modes, Linux gadgets (`f_ncm`) | `cdc_ncm.c` ("Dual BSD/GPL": GPL-2.0 or BSD-2-Clause; header lines 15-17) | Medium: NTB16 transfer blocks with frame aggregation both ways, eight class requests | **v1.x** in `cdcnet` |
| **RNDIS** (`02/02/FF`, `E0/01/03`, `EF/01/01`, `EF/04/01`; `rndis_host.c` lines 654-690) | Android USB tethering, Linux/Raspberry Pi gadgets in RNDIS mode, QEMU `usb-net`'s first configuration, older cable modems | `rndis_host.c` (GPL-2.0-or-later); Microsoft's published [MS-RNDIS] protocol specification | Medium: an encapsulated control channel carrying NDIS OIDs, 44-byte packet headers, multiple packets per transfer | **v1** in `cdcnet` on 98 SE, ME and 2000 (QEMU's `usb-net` is RNDIS first, ECM second); on XP to 7 an INF over Windows' own driver (section 7.5) |
| **Realtek RTL8152/8153** (vendor mode) | By far the commonest chip in USB-A and USB-C gigabit and 100M dongles and dock NICs; `r8152.c`'s id table alone carries Lenovo (12), Realtek (8), Microsoft (4), TP-Link, TRENDnet, Dell, Samsung, ASUS, Linksys, NVIDIA and D-Link entries. Market share figures were not measured | `r8152.c` (GPL-2.0-only, "Copyright (c) 2014 Realtek Semiconductor Corp."), about 10,500 lines | Large: OCP register access per chip revision (17 revisions, `enum rtl_version`), PHY bring-up, 24-byte receive descriptors, 8-byte transmit descriptors, 16-48 KiB aggregated transfers. Optional PHY firmware patches (`rtl_nic/rtl8153*.fw`) | **v2** as `rtl815x`, 98 SE, ME and 2000 (Realtek's own drivers serve XP to 7); RTL8153 in ECM mode is covered by v1 |
| RTL8156/8157/8159 (2.5G/5G/10G) | Growing (2.5G dongles) | same `r8152.c` (`r8156_init`) | As above | `rtl815x` later, on 98 SE, ME and 2000; until then by `cdcnet` if the part offers ECM/NCM (unverified per part) |
| **ASIX AX88179 / AX88178A** | Common second-tier USB 3 gigabit chip (D-Link DUB-1312, Sitecom, Toshiba, Belkin, Lenovo OneLink and others in `ax88179_178a.c`) | `ax88179_178a.c` (GPL-2.0-or-later, "Copyright (C) 2011-2013 ASIX") | Medium: register setup, receive aggregation with a trailer header (section 6.6), 8-byte transmit header | **v2** as `ax88179`, 98 SE, ME and 2000 (ASIX's own drivers serve XP to 7). The AX88179A/B present CDC-NCM by default and go through `cdcnet` on every target (`existing.md` section 4.2) |
| ASIX AX88772/A/B/C | Still sold in cheap USB 2.0 100M dongles | `asix_devices.c` (GPL-2.0-or-later) | Small: 4-byte length header | Not planned (owner); section 2.6 |
| Microchip/SMSC LAN95xx, LAN78xx | Mostly soldered onto boards (Raspberry Pi models use them as the board's own NIC); retail dongles are rare (unverified) | `smsc95xx.c` (GPL-2.0-or-later), `lan78xx.c` (GPL-2.0+) | Medium | Not planned |
| Davicom DM9601 and clones (CoreChips SR9700, QinHeng CH9200) | Cheapest USB 1.1/2.0 10/100 dongles | `dm9601.c` (GPLv2 text, no SPDX line) | Small | Not planned (owner) |
| Apple iPhone tethering | Every iPhone | `ipheth.c` (BSD-3-Clause or GPL-2.0) | Vendor class `FF/FD/01`, legacy framing or a non-compliant NCM variant (`ipheth.c` lines 221-228); the phone must be paired and trusted first (by a user-space pairing service on Linux) | Out of scope |

### 2.3 Why these picks

- **Coverage per line of code.** The CDC classes are public specifications,
  needing no vendor knowledge; one driver serves every device that offers
  them. Because the RTL8153 and the ASIX AQC111U/112U parts expose an ECM
  configuration (`cdc_ether.c` entries for `0BDA:8153` and `0B95:2790`/`2791`),
  `cdcnet` alone reaches most dongles in shops today, if more slowly than
  their vendor modes.
- **QEMU can test it.** QEMU's `usb-net` is RNDIS (configuration value 2,
  listed first) and CDC-ECM (configuration value 1), `0525:A4A2`
  (`hw/usb/dev-network.c` lines 46-68, 267-290). Nothing else in this list
  can be exercised without hardware.
- **Vendor modes are for speed.** ECM moves one frame per USB transfer; at
  gigabit rates that is about 80,000 transfers a second each way, which a
  Windows 98-era system will not sustain. The RTL8153 and AX88179 vendor
  modes pack many frames into one 16-48 KiB transfer.
- **No public datasheets.** Realtek and ASIX publish no programming manuals
  for these chips; `r8152.c` (written by Realtek) and `ax88179_178a.c`
  (written by ASIX) are the only free references; `r8152.c` is GPL-2.0
  only and `ax88179_178a.c` GPL-2.0-or-later (section 9).

### 2.4 Tethering

Android's USB tethering is RNDIS (`E0/01/03` in an IAD, `rndis_host.c`
line 664). Whether current Android releases also offer NCM for tethering was
not checked (unverified). iPhones use `ipheth`, not RNDIS (above): the
brief's "Android/iPhone-style tethering" holds only for Android.

Owner, 2026-10-20: tethering is served under xHCI98 (which splits the IAD
composite on every system) and on XP and later (where `usbccgp` does).
Under Microsoft's 98 SE, ME and 2000 stacks, which predate IADs, Android
tethering is not planned, and no phone is listed for whole-device binding.

### 2.5 What `cdcnet` also fixes on XP to 7

Windows 7's in-box RNDIS INF does not match Android's `E0/01/03` (section
2.1), and no target has CDC-ECM or NCM. So `cdcnet` is useful on the NT
targets too, not only on 98 SE and ME: its driver for ECM and NCM, and for
RNDIS on XP to 7 an INF, `CDCRNDIS.INF`, that names the class ids for
Windows' own RNDIS driver (section 7.5).

### 2.6 AX88772: not planned

The non-free sibling repository, a separate project this one does not
depend on, holds ASIX's own drivers for the AX88772 and AX88772A
(`0B95:7720`, `0B95:772A`; its 98 SE/ME INF, `DriverVer` 3.0.3.12, lists no
other id). Owner, 2026-10-20: no free AX88772 driver is planned; the
vendor driver in the non-free pack is the only one. The AX88772B/C, sold
today, therefore has no driver on 98 SE/ME from this repository; the
release notes say so. The cheap DM9601/SR9700/CH9200 clones are not planned
either.

Owner, 2026-11-06: the AX88772B and AX88772C get a free driver on 98 SE
and ME, `ax88772` (section 6.8); the AX88772 and AX88772A stay with
ASIX's driver. The CoreChips SR9700 and SR9800 are xHCI98 roadmap 47.27.

## 3. Package structure

### 3.1 Options weighed

- **One monolithic `usbnet.sys` with every chip in it.** Each fix to one
  chip re-releases the others. On Windows 98 a second kernel module loaded
  as a DLL is possible (98 SE loads WDM export drivers such as `usbd.sys`),
  but plug-in loading across 98 SE to 7 adds a mechanism no other driver in
  this repository needs.
- **One independent package per chip family with no shared code.** The NDIS
  upper edge, OID handling, packet pools, URB plumbing, power and removal
  logic would be written three or four times, and the hardest bugs (removal
  races, the 98 SE specifics) fixed three or four times.
- **Chosen: per-family packages statically linking one shared core.** Each
  package has its own `.sys`, INF, version and release tag, as AGENTS.md
  ("Source and binaries") expects, and a user installs only what their
  adapter needs. The core is a source library, not a released package.

### 3.2 Layout

| Path | What | Driver file (8.3) |
|---|---|---|
| `src/net/common/` | Shared library: the NDIS 5 upper edge (5.0 for x86, 5.1 for x64), USB lower edge, receive/send pipelines, OIDs, statistics, MAC address policy, power and removal. No INF, never released on its own | - |
| `src/net/cdcnet/` | CDC-ECM, CDC-NCM, RNDIS (98 SE, ME, 2000); the ECM configuration of known vendor adapters; the XP to 7 RNDIS INF | `CDCNET.SYS`, `CDCNET.INF`, `CDCRNDIS.INF` |
| `src/net/rtl815x/` | Realtek RTL8152/8153 (later 8156) vendor mode | `RTL815X.SYS`, `RTL815X.INF` |
| `src/net/ax88179/` | ASIX AX88179/AX88178A | `AX179F.SYS`, `AX179F.INF` |
| `src/net/ax88772/` | ASIX AX88772B/AX88772C on 98 SE and ME (section 6.8); CoreChips SR9800 on 98 SE to 7 (section 6.9.1) | `AX772F.SYS`, `AX772F98.INF`, `AX772F.INF` |
| `src/net/sr9700/` | CoreChips SR9700, 98 SE to 7 (section 6.9.2) | `SR9700F.SYS`, `SR970098.INF`, `SR9700F.INF` |

The `F` suffix keeps the free driver's file name from colliding with the
vendor's own (ASIX's AX88179 file name was not checked). The core sits in
`src/net/common/`, as `README.md`'s layout allows for code shared within a
class (Owner, 2026-10-20). Owner, 2026-10-20: the driver file names in the
table are accepted.

### 3.3 The internal interface between core and package

Each package supplies a table the core calls:

- `Probe(config descriptors)`: which configuration and interfaces to select.
- `Start(device)`: chip bring-up after SELECT_CONFIGURATION; reads the MAC.
- `RxParse(buffer, length, emit)`: split one completed bulk-IN transfer into
  frames and call `emit(frame, length)` for each.
- `TxBuild(frames[], count, buffer, max)`: pack queued frames into one
  bulk-OUT transfer, with headers, padding and zero-length rules.
- `Notify(interrupt data)`: link state and speed from the interrupt endpoint.
- `SetFilter(packet filter, multicast list)`, `Stop()`, `Suspend()`,
  `Resume()`.

The core owns everything else, so a new chip is the table and its register
code.

The table is an internal source interface, with no binary ABI. `Probe` is a
pure parser: it returns a proposed configuration, control/data interface
numbers, alternate setting and endpoint addresses, without changing the
device. `RxParse` borrows its input only for the call; `emit` copies a
validated frame before returning. `TxBuild` returns the bytes used, number
of frames consumed and a protocol-specific termination action. Neither
parser allocates memory or calls NDIS or USB. A malformed block is rejected
before any frame from that block is emitted. Those rules make the same
parsers usable by the host tests without a kernel shim.

`Start`, filter changes and recovery run on one passive-level control
queue per adapter. Control completion handlers finish bookkeeping and queue
the next action. Data completions may parse, copy and repost according to
section 5.3. Neither path waits for a control request or recursively runs
bring-up. A failure before initialization succeeds unwinds every resource
already acquired; `MiniportHalt` is not the cleanup path for a failed
`MiniportInitialize`.

## 4. Target systems and the OS-facing model

### 4.1 NDIS version per system

| System | NDIS | Evidence |
|---|---|---|
| 98 SE (and 98) | 5.0 | 98 DDK guide, "NDIS Miniport Drivers" (`legal_035f.htm`): "To create and test an NDIS miniport driver, refer to the Windows 2000 DDK documentation. A miniport driver created in this manner is binary compatible and will run on both the Windows 2000 and Windows 98 operating systems. There are a small number of features in the Windows 2000 operating system available to these miniports that are not available to a miniport running on Windows 98." And `po-dev_7t9j.htm`: "Network adapter drivers must conform to the NDIS 5.0 specification". The 98 DDK's own `INC_WIN98_NDIS.H` is the NDIS 3 VxD header (`NDIS_MAJOR_VERSION 0x03`) and is not used |
| ME | 5.0 (unverified from Microsoft) | No ME DDK on this host. Indirect: the RNDIS kit's ME INF section writes `MajorNdisVersion 5`, `MinorNdisVersion 0` (Linux `linux.inf`), and ASIX ships one 98 SE/ME binary that registers as NDIS 5.0 (its INF). Reading the ME DDK's network notes would settle it |
| 2000 | 5.0 | 2000 DDK `ndis.h`; network guide chapter 12 |
| XP, XP x64 | 5.1 | WDK 7.1 `inc\ddk\ndis.h` lines 258-269: a 5.1 miniport needs `NTDDI_VERSION >= NTDDI_WINXP`, 5.0 needs `>= NTDDI_WIN2K`; `NDIS51_MINIPORT_CHARACTERISTICS` (line 8015) adds `PnPEventNotifyHandler` |
| Vista | 6.0 (6.1 with SP1/Server 2008) | same header lines 261-264: 6.0 and 6.1 need `NTDDI_VISTA` |
| 7 | 6.20 | same header line 262: 6.20 needs `NTDDI_WIN7` |
| 8 | 6.30 | Microsoft, "NDIS 6.30 Backward Compatibility" (learn.microsoft.com, windows-hardware/drivers/network/ndis-6-30-backward-compatibility): "The TCP/IP protocol driver that ships with Windows 8 has been updated to NDIS 6.30". No WDK 8 header on this host |

**Do NDIS 5 miniports load on Vista and 7?**

- Yes. Microsoft, "NDIS 6.0 Backward Compatibility" (learn.microsoft.com
  previous-versions, archived): "The Windows Vista operating system supports
  NDIS 3.0 - 5.x miniport drivers ... NDIS 6.0 provides a translation layer
  ... the translation might degrade system performance--particularly in the
  send and receive code paths." WDK 7.1's `ndis.h` still builds 5.x
  miniports for any `NTDDI_VERSION` from 2000 up (`NDIS_LEGACY_MINIPORT`,
  line 373).
- Windows 8 is not a target. Microsoft's NDIS 6.30 page, "Features that have
  been removed": "The following features have been removed from Windows 8 and
  later: NDIS 5.x and earlier". Owner, 2026-10-20: no NDIS 6 code and no
  Windows 8; every package stops at 7, which NDIS 5 reaches.

### 4.2 Builds and binaries

Owner, 2026-10-20: NDIS 5 only, two builds.

| Build | NDIS | Toolchain | Systems | Why |
|---|---|---|---|---|
| x86 NDIS 5 | 5.0, `NDIS50_MINIPORT`, `NDIS_WDM=1`, `USE_KLOCKS=1` | Windows 2000 DDK with MSVC 6.0 | `cdcnet`: 98 SE, ME, 2000, XP, Vista and 7 x86. `rtl815x`: the same, XP to 7 for its newer chips only (section 7.6). `ax88179`: 98 SE, ME and 2000 | The only NDIS version 98 SE has; Vista and 7 load it through the translation layer. The 2000 DDK network guide section 12.6 names the two compile flags |
| x64 NDIS 5.1 | 5.1 | WDK 7.1, `x64 WNET` environment | `cdcnet` and `rtl815x` (from 2026-10-27): XP x64, Vista x64, 7 x64 | XP x64 is NDIS 5.1; Vista and 7 x64 load it through the translation layer |

The translation layer's cost on Vista and 7 is accepted; throughput there is
measured on 7 x64 (section 6.7), not assumed.

Kernel-mode code signing is required on Vista and 7 x64, which `cdcnet`
and (from 2026-10-27) `rtl815x` target. Owner, 2026-10-20:
signing follows the same approach as xHCI98 and Stor98, with the tooling
copied into this repository, not referenced.

### 4.3 The NDIS-WDM miniport model (all systems)

From the 2000 DDK network guide, chapter 12 ("Miniport Driver with a WDM
Lower Interface", `212nonds_1l2f.htm`): a miniport "that controls either
Universal Serial Bus (USB) or IEEE 1394 ... buses requires a miniport driver
that exposes a standard NDIS miniport interface at its upper edge and uses
the class interface for the particular bus at its lower edge."

- **DriverEntry**: `NdisMInitializeWrapper`, `NdisMRegisterMiniport` with
  `InitializeHandler`, `HaltHandler`, `ResetHandler`, `ReturnPacketHandler`,
  `SendPacketsHandler`, `QueryInformationHandler`, `SetInformationHandler`.
  No ISR, interrupt, `TransferData`, `AllocateComplete` or `CheckForHang`
  handler, and no shutdown handler (section 12.2, `212nonds_9147.htm`).
  `NdisMRegisterUnloadHandler` (2000 DDK `ndis.h` line 10368).
- **MiniportInitialize**: `NdisMSetAttributesEx` with
  `NDIS_ATTRIBUTE_DESERIALIZE` (0x20, `ndis.h` line 10739): "A miniport
  driver with a WDM lower edge must be a deserialized miniport driver"
  (section 12.3, `212nonds_263r.htm`). Then `NdisMGetDeviceProperty` for the
  PDO, FDO and next-lower device object (`103ndisx_314i.htm`); every URB is
  an `IRP_MJ_INTERNAL_DEVICE_CONTROL` with `IOCTL_INTERNAL_USB_SUBMIT_URB`
  sent to the next-lower device object (section 12.4, `212nonds_692f.htm`).
  On 98 the raw and translated resource lists come back NULL, which this
  driver never asks for. Then read the configuration descriptors, let the
  package choose, `SELECT_CONFIGURATION`, `SELECT_INTERFACE` where the data
  interface has an alternate setting, chip `Start`, read the MAC, start the
  receive and interrupt pipelines.
- **Deserialized rules** (section 4.5.1, `204trnsf_6e7b.htm`): every packet
  passed to `MiniportSendPackets` completes asynchronously through
  `NdisMSendComplete`; the miniport queues internally and may not return
  `NDIS_STATUS_RESOURCES`.
- **Source split**: the guide advises separate source files for NDIS-calling
  and WDM/USB-calling code (section 12.5, `212nonds_082v.htm`);
  `src/net/common/` keeps `ndis5.c` apart from `usbio.c`.

### 4.4 Windows 98 SE and ME specifics

- **How the miniport loads.** The 9x INF sets `DevLoader=*ndis` and
  `DeviceVxDs=<file>.sys`: `NDIS.VXD` is the device loader and loads the WDM
  `.sys` itself (the RNDIS kit's `RNDIS_AddReg_98` in `linux.inf`; ASIX's
  98 SE/ME INF does the same). The 2000 DDK network guide documents
  `DevLoader` and `DeviceVxD` as "Windows 95/98 Ndi Values and Keys Not Used
  in Windows 2000" (`501install_0sfb.htm`). DDK sample INFs such as
  `NET557ex.INF` are not read (Owner, 2026-10-20; `AGENTS.md`, "Sources").
- **WDM services under NTKERN.** The miniport's USB half calls only WDM
  kernel routines (`IoAllocateIrp`, `IoCallDriver`, `KeInitializeEvent` and
  the like), as the guide requires (section 12.5). 98 SE lacks some of them:
  `IoAllocateWorkItem`/`IoQueueWorkItem` must not be assumed available.
  Passive-level work
  therefore goes through `NdisScheduleWorkItem` (2000 DDK `ndis.h` line
  6675), subject to the local import gate and scheduling/lifetime
  experiment. Each link checks imports against the target's recorded
  export set, following [build.md](build.md).
- **Protocol binding.** The RNDIS kit's 98 section lists the upper range
  `ndis3,ndis4,ndis5`. `cdcnet` follows it (a list that includes `ndis3`);
  which upper ranges 98 SE's protocols actually bind is read from the DDK
  network INF documentation and on a guest (**unverified**).
- **Single binary for 98 SE and ME.** Microsoft's RNDIS kit shipped separate
  98, ME and 2000 builds (`linux.inf`), but the 98 DDK's statement of binary
  compatibility (section 4.1) and ASIX's single 98 SE/ME driver point to one
  binary across 98 and ME at least. Why Microsoft
  split its builds is unknown; a risk to watch (section 12).
- **Receive indication.** `NdisMIndicateReceivePacket` is the NDIS 5 path;
  on 98, `NDIS.VXD` presents it to NDIS 3 protocols such as the 98 TCP/IP
  stack. How it does so (copying through lookahead or not) was not read and
  bears on 98 SE throughput; measured in M2.
- **Upcalls on 98 SE and ME** (section 6.4.2; record
  `docs/testing/2026-11-07-98se-tcp.md`). In the shared core
  (`netcore.c`) and in `cdcnet.c`, x86 NDIS 5.0 builds only, no receive
  indication and no
  send completion is made from a USB completion routine when
  `IoIsWdmVersionAvailable(1, 0x10)` fails (98 SE and ME): `Receive` and
  `TxDone` queue the packet (a ring of receive packets, a list of
  completed sends through `MiniportReserved`) and set an NDIS miniport
  timer with no delay; its routine, `Upcalls`, completes the sends and
  indicates the receives, in order, outside the lock. Every deferred
  receive goes with `NDIS_STATUS_SUCCESS`. `MiniportHalt` stops the timer,
  waits for a routine already due, completes the queued sends and puts
  the unindicated receives back in the pool; a sleep request does the
  same before it completes (NDIS timers are cancelled before a sleep
  state), and a D0 request turns delivery back on before the device
  restarts, so no completion of the restart reaches NDIS directly.
  While asleep a send that fails at once completes at once. Made
  straight from the USB
  completions, the same calls stalled the 98 TCP/IP within seconds of
  sustained TCP, and on the real RTL8157 faulted it (exceptions 0E in
  `MSTCP`), with the core's own state intact; from the timer they ran
  clean (simulated chips; the real RTL8157 was not retested). Why the completion context is unsafe there was not read (no
  Microsoft binary is read); the record holds the evidence. NT keeps the
  direct calls, and the NDIS 5.1 (x64) build has none of this code.
  Status indications and OID and reset completions, made from the
  passive worker, are not deferred.

### 4.5 OIDs

General and 802.3 objects from the 2000 DDK network guide, chapters 2.0
("General Objects", `22genoid_1m2a.htm`) and 3.0 ("Ethernet Objects",
`23ethoid_5i7m.htm`); the exact mandatory set is read from those pages when
coding. v1 answers at least: `OID_GEN_SUPPORTED_LIST`,
`_HARDWARE_STATUS`, `_MEDIA_SUPPORTED`/`_IN_USE` (802.3),
`_MAXIMUM_FRAME_SIZE` (1500), `_MAXIMUM_TOTAL_SIZE` (1514),
`_MAXIMUM_LOOKAHEAD`/`_CURRENT_LOOKAHEAD`, `_LINK_SPEED`,
`_TRANSMIT_BUFFER_SPACE`/`_RECEIVE_BUFFER_SPACE`, `_TRANSMIT_BLOCK_SIZE`/
`_RECEIVE_BLOCK_SIZE`, `_VENDOR_ID`/`_VENDOR_DESCRIPTION`/
`_VENDOR_DRIVER_VERSION`, `_DRIVER_VERSION`, `_MAC_OPTIONS`,
`_MEDIA_CONNECT_STATUS`, `_MAXIMUM_SEND_PACKETS`,
`_CURRENT_PACKET_FILTER`, the transmit/receive OK/error/no-buffer counters,
`OID_802_3_PERMANENT_ADDRESS`/`_CURRENT_ADDRESS`/`_MULTICAST_LIST`/
`_MAXIMUM_LIST_SIZE` and the collision/alignment counters (reported zero),
and `OID_PNP_CAPABILITIES`/`_QUERY_POWER`/`_SET_POWER` (section 6.1,
`206pm_8dbb.htm`: answering `OID_PNP_CAPABILITIES` is what makes NDIS treat
the miniport as power-management aware). The core answers OS-facing
capabilities from its implemented contract, rather than forwarding the
device's supported-OID list or offload claims. Only device-specific values
and operations, such as RNDIS link state and filter programming, go to the
device. Every OID checks its input/output length and fills the NDIS 5
bytes-read/written/needed outputs. USB work requested by an OID is queued;
the pending query or set completes exactly once through the corresponding
NDIS 5 completion routine, including on stop or removal.

### 4.6 Link state, filters, frame size, offload

- **Media connect/disconnect.** Initial state from the device (ECM/NCM
  `NETWORK_CONNECTION` notification, RNDIS `OID_GEN_MEDIA_CONNECT_STATUS`,
  chip PHY register), then `NdisMIndicateStatus` with
  `NDIS_STATUS_MEDIA_CONNECT`/`_DISCONNECT` (2000 DDK `ndis.h` lines 5554,
  11012) and `NdisMIndicateStatusComplete` on each change. Link speed from
  ECM/NCM `CONNECTION_SPEED_CHANGE` or the chip. Whether 98 SE's TCP/IP acts
  on media sense (renewing DHCP on reconnect) was not read; 2000 and later
  do (unverified here).
- **Packet filter**: directed, multicast, all-multicast, broadcast,
  promiscuous, mapped to ECM/NCM `SET_ETHERNET_PACKET_FILTER`, RNDIS
  `OID_GEN_CURRENT_PACKET_FILTER`, or chip receive-control registers.
- **Multicast**: `OID_802_3_MAXIMUM_LIST_SIZE` reports 32. ECM/NCM: if the
  functional descriptor's `wNumberMCFilters` is non-zero, send
  `SET_ETHERNET_MULTICAST_FILTERS`; otherwise, or past the device's limit,
  fall back to all-multicast. RNDIS: `OID_802_3_MULTICAST_LIST` passed down.
  r8152 and AX88179: the chips' 64-bit hash tables (from the Linux drivers'
  `set_rx_mode` routines).
- **MTU**: 1500 only. No jumbo frames, no VLAN tags, in v1 or planned.
- **Offload**: section 4.9 (2000 and later, the RTL8157 and RTL8159 only).

### 4.7 Power and removal

- **System sleep**: `OID_PNP_SET_POWER` to D3 cancels and drains every URB,
  calls the package's `Suspend`; D0 re-runs `Start` (the Realtek chips need
  their bring-up repeated after power loss, `r8152.c` resume paths) and
  restarts the pipelines. No wake-on-LAN (`OID_PNP_ENABLE_WAKE_UP` not
  offered).
- **Selective suspend** of an idle adapter: not planned (NDIS 5 has no idle
  framework).
- **Surprise removal**: on NDIS 5.0 (98 SE, ME, 2000) there is no PnP event
  callback; the driver sees URBs fail (`USBD_STATUS_DEVICE_GONE`, or an
  `NTSTATUS` such as `STATUS_DEVICE_NOT_CONNECTED`), stops reposting, marks
  the adapter gone so sends complete with failure, and `MiniportHalt` then
  cancels what is left with no wait that depends on the device. On XP's 5.1
  build `PnPEventNotifyHandler` receives `NdisDevicePnPEventSurpriseRemoved`
  and `NDIS_ATTRIBUTE_SURPRISE_REMOVE_OK` (0x80, WDK 7.1 `ndis.h` lines 7673,
  9721; absent from the 2000 DDK header) suppresses the unsafe-removal
  warning; the x86 NDIS 5.0 binary on XP, Vista and 7 relies on the
  URB-failure path above. Every pending URB, queued/running task and
  outstanding receive owner is tracked so halt cannot return while any
  can still access the adapter (section 4.7.1). Draining canceled requests
  does not mean waiting for a response from the removed device.
  Owner, 2026-11-07 (a real RTL8157 under xHCI98 on XP): a host
  controller driver may hold a departed device's transfers instead of
  failing them (xHCI98 parks them on the device until the pipe is aborted
  or the device removed), while TCP/IP does not unbind - so `MiniportHalt`
  never comes - before every send it made has completed. Unplugged under
  traffic, the adapter stayed in Device Manager with 40 sends held in the
  transmit slots, a replug never loaded and shutdown hung. The core now
  reports a departure, whichever way it is found (a failed transfer, the
  2 s link poll's control read, an abort answering device-gone), before
  it waits for any transfer: the gate closes, the pump is let finish,
  every frame a transmit slot holds is completed failed at once (its data
  was copied into the slot, which stays busy until its IRP ends),
  `Link(-1)` flushes the queue, and the pipes are aborted (`ReleaseGone`,
  `AbortGone`, the check at the top of each `Quiesce` pass and between its
  aborts; `Quiesce` repeats the aborts while transfers remain). A
  synchronous abort that never completes is still waited for: no lower
  driver seen does that.
- **Reset** (`MiniportReset`): reset the pipes (`RESET_PIPE` after
  `ABORT_PIPE`) and re-run the package's `Start` without reconfiguring.
  USB recovery runs asynchronously at passive level; the callback returns
  pending and finishes through `NdisMResetComplete`. Restore the cached
  filter, multicast list and current address before reopening traffic.
  The precise NDIS 5 `AddressingReset` and completion-IRQL contract is
  checked against the target DDK documentation in M0.

#### 4.7.1 Lifetime and ownership contract

The implementation has its own adapter states: `Starting`, `Running`,
`Quiescing`, `Suspended`, `Recovering`, `Gone`, `Halting`. These are driver
states, not NDIS 6 pause/restart states. One state lock protects the state,
submission gate, queues and generation number. No NDIS indication,
completion, USB submission or wait runs while that lock is held.

Closing the submission gate precedes cancellation. Every submitted IRP
and every queued/running control task holds an adapter reference acquired
before it becomes visible to another execution path. Completion may occur
inside `IoCallDriver`; bookkeeping must already be valid. Cancellation
does not return ownership: the completion routine does. Cancellation and
completion synchronize access to the IRP so neither can use an IRP the
other has already freed. Old-generation completions may release resources
and complete sends, but cannot repost traffic after reset or resume.
Admission to USB submission acquires a counted submitter slot under the
state lock. Quiesce closes admission, waits for already admitted submitters
to return from `IoCallDriver`, then cancels the now-stable outstanding set.
No new submitter can enter after gate close. This closes the race between
checking `Running` and actually sending an IRP without holding the state
lock across a potentially inline completion.

A completion routine never calls `IoCallDriver` for a repost while it is
itself running inside a submitter's `IoCallDriver`. Each repost goes
through one per-pipe submission pump: a completion marks the URB ready
and, if the pump is already active on this or another path, returns; the
active pump loops over ready URBs, rechecking the gate and generation
before each submit. Consecutive inline completions therefore iterate
instead of nesting, and stack depth stays bounded. The fake completion
backend's immediate mode runs a long run of inline successes and checks
the nesting depth.

The core owns a send packet from acceptance until one
`NdisMSendComplete`, including queued sends that never reached USB. Its
queue has a fixed packet and byte limit; excess accepted packets complete
with failure, without an unbounded allocation. Receive frames are copied
into a separate bounded NDIS packet/buffer pool. A successful deserialized
receive indication transfers ownership until `MiniportReturnPacket`,
which may run reentrantly; reserve the reference before indication. Pool
exhaustion drops the frame and increments the no-buffer counter. An
optional `NDIS_STATUS_RESOURCES` indication is reclaimed after the call
using its saved pre-indication status, as the
[NDIS 5 receive documentation](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553533(v=vs.85))
requires.

Sleep drains USB requests before the power OID completes. Surprise
removal closes the gate and performs no new device requests. Halt stops
timers, drains control tasks and USB completions, completes queued sends,
and waits for all outstanding receive ownership before freeing pools and
the context. A timeout records a fault; it never authorizes freeing memory
still owned by an asynchronous operation. M0 must establish the target
stack's abort/cancel completion behavior before this path is implemented.

### 4.8 MAC address

Order of preference:

1. `NetworkAddress` from the adapter's registry key
   (`NdisReadNetworkAddress`), a locally administered address the user set
   on the Advanced tab, applied where the device can change it (RTL815x and
   AX88179 node-id registers; RNDIS and ECM devices generally cannot, so the
   value is refused there with an event-log entry on NT).
2. The device's own: ECM/NCM `iMACAddress` string descriptor (12 hex
   digits; Linux `usbnet_get_ethernet_addr`), RNDIS
   permanent and current address OIDs (permanent identity and the address
   actually used for traffic), the chips'
   EEPROM-loaded node-id register (Linux `r8152.c`, `ax88179_178a.c`).
3. If that is missing, all zeros, multicast or all ones: a random locally
   administered address generated once and kept in the adapter's driver key,
   so it survives reboots, but only if the device can be programmed to use
   it. An ECM/RNDIS device with no valid current address and no supported
   way to change it fails initialization; reporting a different address
   only to NDIS would leave directed receive filtering inconsistent.

Docks' "MAC address pass-through" (r8152's ACPI-driven Lenovo/Dell feature)
is not in scope.

### 4.9 Transmit by bytes and task offload (2026-11-07)

Record: `docs/testing/2026-11-07-rtl8157-performance.md`.

- **Transmit transfers are filled by bytes.** `TxTake` takes queued sends
  in order while each fits the transfer buffer: at the next multiple of
  the chip's alignment, behind its header, padded to 60 bytes. The chip's
  `TxBatch` caps the count (`NET_TX_BATCH`, 64, at most). The first send
  always goes; `SendPackets` admits none longer than a buffer holds. Before,
  the count was the buffer divided by a full frame, so a 16 KiB buffer
  carried at most 10 frames even when they were 60-byte ACKs. Linux
  `r8152_tx_agg_fill` fills by bytes too. A send's length is kept in the
  second slot of `MiniportReserved` while it is queued.
- **Task offload, from the 2000 DDK network guide chapter 9**
  (`209offl_54dj.htm` 9.1 to `209offl_0353.htm` 9.7, `105nstru_3f5e.htm`,
  `105nstru_7c36.htm`). NT only (2000, XP, XP x64 and later): on 98 SE and
  ME the core reads no per-packet information and answers
  `OID_TCP_TASK_OFFLOAD` with `NDIS_STATUS_NOT_SUPPORTED`, as it does for a
  chip that reports no offload (`NET_PARAMS.Offload` 0: every chip but the
  RTL8157 and RTL8159 today).
  - The query is answered for IPv4 over Ethernet II
    (`IEEE_802_3_Encapsulation`, header 14) with the tasks the chip has:
    checksums (TCP, UDP and IPv4 header on transmit; all three checked on
    receive, IP and TCP options allowed) and the large send.
    `MaxOffLoadSize` is the largest large send (one transmit buffer less
    its header) less 134 bytes of Ethernet, IP and TCP headers with
    options; `MinSegmentCount` 2.
  - A set enables what it lists and only what was offered; a header with
    no task turns all off. Every set is taken, not only the first: TCP/IP
    turns offload off and on again around IP security (9.8).
  - Transmit: a send's large-send value (an MSS) or its checksum request
    is checked against its own IPv4 and TCP headers. A request the adapter
    cannot do (not enabled, not IPv4, the wrong protocol) fails that send;
    nothing is sent wrong. A large send's value becomes its TCP payload
    bytes once it is in a transfer, and 0 if that transfer fails (9.7).
  - A frame with an offload is not padded to 60 bytes: the chip pads it,
    as Linux leaves it to. Padded, a TCP ACK with a checksum request left
    a real RTL8157 with a bad checksum (the peer dropped the ACKs and
    receives stalled at 0.44 Mb/s on XP).
  - Receive: the chip's verdicts (`RxChecksum`), masked by what TCP/IP
    enabled, set the `...Succeeded` bits of the packet's checksum
    information. A failure is never reported: TCP/IP checks such a frame
    itself, as Linux leaves it to its stack (`CHECKSUM_NONE`).
- **The chip interface** (`netchip.h`) gains `NET_PARAMS.Offload` and
  `LsoMax` and two optional routines after `RxReset`: `TxOffload` (marks a
  header `TxHeader` wrote) and `RxChecksum` (reads the receive header
  before a frame `RxParse` emitted). The other packages leave them out.
- **RTL8157 and RTL8159** (`rtlchip.c`): 32 KiB transmit buffers, as
  Realtek's Windows driver sends 32848-byte transfers to the RTL8157, and
  the transmit checksums. The descriptor bits are Linux `r8152.c`'s
  (`tx_desc`, `rx_desc_v2`), and a real RTL8157 under Realtek's driver
  10.68.20 used the same ones (usbmon, Windows 10 x64).
- **The large send is not offered** (2026-11-07). With it offered (up to
  32752 bytes), 2000 SP4 and XP SP3 enabled it but never handed the
  driver one, also with `MaxOffLoadSize` 64 KiB; so it never ran on the
  chip, and it is left out until a system that uses it is tested. The
  core's large-send path, `RtlTxOffload`'s large-send bits and the model's
  segmentation stay, covered by the host and simulator tests. Realtek's
  driver leaves a large send's TCP header offset 0, where Linux gives it;
  the core gives it, as Linux. The first format (RTL8152 to RTL8156B) keeps 16 KiB and no
  offload: its bits are Linux's too, but were not checked on hardware here.
- **The receive checksums are not offered** (2026-11-07). Offered, 2000
  SP4 enabled them and received slower in interleaved runs (median 153
  against 161 Mb/s, mean 151 against 176): TCP/IP kept the checked
  packets, the 64-packet receive pool ran down to 9 free and 27% of the
  frames went up as copies. XP showed no gain. `RtlRxChecksum` and the
  core's reporting stay, covered by the host and simulator tests.
- **The model** (`rtlemu.py`) segments a large send and computes the
  checksums asked for on the RTL8157/8159, and writes the receive checksum
  bits as the real RTL8157 did; what was not seen is marked inferred there.

### 4.10 The Advanced tab: the makers' keywords (2026-11-09)

Owner, 2026-11-09: each Ethernet driver's settings on its adapter's
Advanced tab are matched, chip by chip, to what the chip maker's own INF
offers, with the maker's defaults; for `sr9700` and `cdcnet`, which no
maker's INF covers, only what the chip or the class supports. Before
this, every package's INF offered `NetworkAddress` alone (`cdcnet` also
its own `Protocol`), and `rtl815x` kept EEE on as Linux sets it, where
Realtek's driver leaves it off (section 6.4.2, "Kept as Linux sets
them").

#### 4.10.1 The makers' INFs, as read

Read 2026-11-09 from `usb-wdm-nonfree` `d2f7a26` (the files as released;
an INF is text, read for its `Ndi\params` keys, values, defaults and the
install section each id takes). "Default" is the `default` value of the
key; where an INF also writes the value itself (`HKR,,<name>`) it is the
same. Keywords the drivers here cannot honour are in 4.10.3.

**Realtek** (`rtl815x`). XP 5.23 (`rtuxp.INF`), Vista 6.27
(`rtux86lh.INF`, `rtux64lh.INF`) and 7 7.61.20 (`rtump62x86sta.INF`,
`rtump62x64sta.INF`) list the RTL8152/8152B, RTL8153/8153B and, in the
7 INF only, the RTL8153C, RTL8153D, 8155, 8156 and 8156B; each chip revision
(`REV_xxxx`) takes a section that pulls in one `.reg` section per
keyword; a bare id (no revision) takes a fallback section
(`RTL8152BXx86`, `RTL8153Xx86`, `RTL8156Xx86`, `RTL8155Xx86` in the 7 INF)
that offers only the address, speed, flow control, VLAN and checksum
keywords. The table is the revision sections'. The RTL8157 and RTL8159 are only in Realtek's Windows 10 INF
10.68.20, which was read on 2026-11-07 for its ids, chip sections and
these four defaults (`docs/testing/2026-11-07-realtek-diagnostic.md`,
section 6.4.2): `*EEE`, `EnableGreenEthernet`, `AdvancedEEE`,
`EnableExtraPowerSaving` all 0. That INF is not on this host now; its
other keywords for these chips were not recorded.

| Keyword (7 INF; XP and Vista name) | Values | XP 5.23 default | Vista 6.27 default | 7 7.61.20 default | Chips offered on (7 INF) |
|---|---|---|---|---|---|
| `NetworkAddress` | edit, 12 hex digits, optional | empty | empty | empty | all |
| `*SpeedDuplex` (XP `SpeedDuplex`) | 0 auto, 1 10 half, 2 10 full, 3 100 half, 4 100 full, 6 1000 full, 2500, 5000 | 0 | 0 | 0 | all; the list stops at the chip's speed (8152: to 4; 8153 family: to 6; 8155/8156/8156B: to 2500). Fibre and "rm10mbe" sections drop some |
| `*FlowControl` (XP `FlowControl`) | 7: 0 off, 3 Rx and Tx; XP: 0 off, 1 on | 1 (on) | 3 | 3 | all |
| `*EEE` (XP and Vista `EEE`) | 0, 1 | 1 on most 8152B, 8153, 8153B sections, 0 on the "basicfunc" ones | the same | **0 on every revision section** | every chip |
| `EEEMaxSupportSpeed` | 10, 100, 1000, 2500 (Mb/s) | - | - | 1000 | 8155, 8156, 8156B |
| `EnableGreenEthernet` | 0, 1 | 1 (8153B; 0 on its "basicfunc" section) | the same | **0** | 8153B/C, 8153D, 8155, 8156, 8156B (not the 8152B or 8153) |
| `AdvancedEEE` | 0, 1 | 0 (8153B) | 0 (8153B) | **0** | 8153B/C, 8153D, 8155, 8156, 8156B (not the 8152B or 8153) |
| `EnableExtraPowerSaving` (XP and Vista `EnableU2P3`), "Idle Power Saving" | 0, 1 | 1 (8153B; 0 on its "basicfunc" section) | the same | **0** | every chip |
| `*IPChecksumOffloadIPv4`, `*TCPChecksumOffloadIPv4`, `*UDPChecksumOffloadIPv4` (XP `CksOffload`) | 0 off, 1 Tx, 2 Rx, 3 Rx and Tx | 3 | 3 | 3 | all |

The XP and Vista INFs offer the four power keywords only on some
sections (EEE on the RTL8152B and later, the other three on the
RTL8153B), with EEE, green Ethernet and idle power saving on by default;
the 7 INF offers EEE and idle power saving on every chip's revision
sections and green Ethernet and Advanced EEE from the RTL8153B on, all
off by default, as the Windows 10 INF has them on the RTL8157 and
RTL8159.

**ASIX, AX88179 and AX88178A** (`ax88179`). XP and Vista 1.4.2.0 / x64
1.6.2.0 (`Ax88179_178a.inf`), 7 1.12.11.0 / x64 1.14.11.0
(`AX88179_178a.inf`); no ASIX driver for 98 SE, ME or 2000.

| Keyword | Values | XP/Vista default | 7 default |
|---|---|---|---|
| `NetworkAddress` | edit, optional | `000000000000` | the same |
| `*SpeedDuplex` | 0 auto, 1, 2, 3, 4 (10/100 half/full), 6 1000 full | 0 | 0 (three Allied Telesis media-converter sections offer one value each: 6, 4 or 0) |
| `*FlowControl` | 0 off, 1 Tx, 2 Rx, 3 Rx and Tx | 3 | 3 (0 in the Allied Telesis sections) |
| `*EEE` | 0, 1 | not offered | **1** (0 in three Allied Telesis sections, 1 in `ATUMC2000T`) |

**ASIX, AX88772B and AX88772C** (`ax88772`, 98 SE and ME). 2000/XP
5.4.3.0, XP x64 5.6.3.0, Vista 5.8.2.0 (`Ax88772b.inf`): `ConnectionType`
0 auto, 1 10 half, 2 10 full, 3 100 half, 4 100 full, default 0;
`FlowControl` 0 off, 1 Tx, 2 Rx, 3 on, default 3; `NetworkAddress`.
Vista's INF (5.8.2.0) has the starred `*SpeedDuplex` and `*FlowControl`
with these values instead. 7 5.12.12.0
(`Ax88772b.inf`): `*SpeedDuplex` 0 to 4, default 0; `*FlowControl` 0 to
3, default 3; `NetworkAddress`; no EEE (the chip has a 10/100 PHY). ASIX's
98 SE/ME driver 3.0.3.12 (`Ax88772.inf`, AX88772 and AX88772A only) has
`ConnectionType` with other values (0 auto, 2, 3, 8, 9) and the same
`FlowControl`.

#### 4.10.2 What each driver offers

One keyword set per package, on every system it serves: the newest
maker INF's names and values for its chips (Realtek's 7 INF, ASIX's 7
INFs), so a value means the same under 98 SE as under 7, and one parser
in the core reads it. The XP and Vista names (`SpeedDuplex`, `EEE`,
`EnableU2P3`, `ConnectionType`) are not read. The defaults are the newest
maker INF's: so `rtl815x`'s EEE, green Ethernet, Advanced EEE and idle
power saving are off by default, as Realtek's 7 and 10 drivers leave
them. A keyword is offered only where the driver performs the function
(4.10.3 lists the rest).

| Package, chips | Keywords (default) |
|---|---|
| `rtl815x`, RTL8152 family | `NetworkAddress`; `*SpeedDuplex` 0-4 (0); `*FlowControl` 0, 3 (3); `*EEE` (0) |
| `rtl815x`, 1 Gb/s (RTL8153, 8153B, 8153C, 8153D, and every id whose chip is not known from it) | as the RTL8152's, `*SpeedDuplex` adds 6; and `EnableGreenEthernet` (0), `AdvancedEEE` (0), `EnableExtraPowerSaving` (0) |
| `rtl815x`, 2.5 Gb/s (RTL8155 id, 8156, 8156B) | as 1 Gb/s, `*SpeedDuplex` adds 2500; and `EEEMaxSupportSpeed` 10, 100, 1000, 2500 (1000) |
| `rtl815x`, RTL8157 / RTL8159 | as the RTL8152's, `*SpeedDuplex` 0-4, 6, 2500, 5000 (and 10000 on the RTL8159, by the pattern of Realtek's values: no INF read here lists it); `EnableGreenEthernet` (0), `AdvancedEEE` (0); on 2000 to 7 also the three transmit checksum keywords, 0 off, 1 Tx (1) |
| `ax88179`, AX88179, AX88178A | `NetworkAddress`; `*SpeedDuplex` 0-4, 6 (0); `*FlowControl` 0-3 (3); `*EEE` (1, ASIX's 7 default; owner question 12.3) |
| `ax88179`, the Allied Telesis media converters `07C9:000E`, `000F`, `0010` | as ASIX's sections for them: `*SpeedDuplex` one value each (6, 4, 0); `*FlowControl` 0-3 (0); `*EEE` 0 only |
| `ax88772`, AX88772B, AX88772C | `NetworkAddress`; `*SpeedDuplex` 0-4 (0); `*FlowControl` 0-3 (3) |
| `ax88772`, SR9800 (no maker INF) | `NetworkAddress`; `*SpeedDuplex` 0-4 (0): its PHY is a clause 22 MII PHY (Linux `sr9800.c` drives it through the generic MII calls) |
| `sr9700` (no maker INF) | `NetworkAddress` only: the chip runs 10 Mb/s half duplex only and has no MII (section 6.9.2) |
| `cdcnet` (no maker INF) | `NetworkAddress`, applied on an NCM function whose `bmNetworkCapabilities` has D1 (CDC NCM 1.0, `SetNetAddress`), refused on any other (ECM and RNDIS define no way to set it); and its own `Protocol` |

Which keyword set an adapter gets is chosen in the INF by its id. For
`rtl815x` an id names a speed class, not a chip: the RTL8152 ids
(`0BDA:8152`, `8050`, `8052`, and the rebrands Realtek's 7 INF puts in
its RTL8152B sections), the 2.5 Gb/s ids (`0BDA:8155`, `8156`, `8056`
and the rebrands in Realtek's RTL8156 and 8156B sections), the RTL8157
and RTL8159 ids, and 1 Gb/s for every other id. Within a class the chip
is read at start (`RtlChipId`); a keyword the chip lacks is ignored and
traced (green Ethernet and Advanced EEE on an RTL8153 of a 1 Gb/s id,
idle power saving on an RTL8153 5C00 or 5C10, an 8153B or an 8153C).
Idle power saving is not offered on the RTL8152, RTL8157 and RTL8159,
which Realtek's INFs offer it on with 0 as the default: the driver never
turns the USB 2 PHY's P3 state on there (Linux has no such step for the
RTL8152; on the RTL8157 and RTL8159 the setup keeps it off), so 0 is
what they get.

#### 4.10.3 Not offered

Wake-on-LAN of every kind (`*WakeOnMagicPacket`, `*WakeOnPattern`,
`WakeOnLinkChange`, `S5WakeOnLan`, `WolShutdownLinkSpeed`, `MaskTimer`,
`WakeUp`): no wake-up source (section 4.7). VLAN and priority
(`*PriorityVLANTag`, `VlanID`, `VLAN_ID`): no tags (4.6). Jumbo frames
(`*JumboPacket`): MTU 1500 (4.6). Large send (`*LsoV2IPv4`,
`*LsoV1IPv4`), IPv6 and receive checksums, ARP and NS offload: not
offered by the driver (4.9). Realtek's `GigaLite` (the 8156 core's setup
turns its "lite" modes off, as Linux does; turning them on is not
described), `EnableExtraTransmissionParm`, `BatteryModeLinkSpeed`,
`EnableAdaptiveLinkCap`, `EnableCloneMAC` and `DashEnable`: no public
description of what they do. ASIX's `AutoDetach`, `AutoDetachTimer` and
`UsbPowerSave`: `ax88179` keeps Linux's auto-detach (the EEPROM's
setting, which is ASIX's 7 default, 2; its XP and Vista default is 0)
and no U1/U2 setting.

#### 4.10.4 What each keyword does

What a Realtek or ASIX keyword does in their drivers is not published;
each is mapped to the chip function of the same name that Linux drives
(facts only from `r8152.c`, GPL-2.0-only; `ax88179_178a.c` and
`asix_devices.c` are GPL-2.0-or-later):

- **`*SpeedDuplex`** (every chip with an MII PHY): 0 autonegotiates
  everything the chip and the USB link carry (as before). 1 to 4 turn
  autonegotiation off and force 10 or 100 Mb/s, half or full duplex in
  the PHY's control register (Linux `rtl8152_set_speed`,
  `mii_ethtool_set_link_ksettings`). 6, 2500, 5000 and 10000 keep
  autonegotiation and advertise that one speed only (full duplex): a
  gigabit or faster link needs autonegotiation, and Linux forces one
  through the advertisement in the same way. A speed the chip or the USB
  link cannot carry (2500 at USB High Speed, where the multi-gigabit
  speeds are never advertised, section 6.4.1; 1000 on an RTL8152) is
  taken as 0, and traced. The link the chip then reports is the link
  (section 4.6).
- **`*FlowControl`**: `rtl815x` (Realtek offers on or off): 0 clears the
  two pause bits of the advertisement, any other value sets them (as now;
  Linux `r8152b_enable_fc`). `ax88179` and `ax88772` (ASIX offers Tx and
  Rx apart): 3, the default, keeps what each driver does today (Linux's:
  on the AX88179 the PHY's own advertisement and both medium pause
  enables, here on a full-duplex autonegotiated link, where Linux sets
  them on every link; on the AX88772B/C both pause bits advertised and
  the enables resolved with the partner's advertisement, IEEE 802.3
  Table 28B-3). 0 to 2: the advertisement as Linux's phylib sets it for
  the pair (Rx only: PAUSE and ASM_DIR; Tx only: ASM_DIR; none: neither),
  and the medium register's transmit and receive pause enables only for
  the directions allowed, after that resolution, at link-up. The SR9800
  keeps Linux's (no pause advertised, both enables); a value there is
  reported, not applied. A speed
  forced with autonegotiation off (`*SpeedDuplex` 1 to 4) has no
  autonegotiation, so no pause; 6 and above keep autonegotiation, and
  pause resolves as for 0.
- **`*EEE`** and **`EEEMaxSupportSpeed`** on the RTL8156 revision 7020:
  ignored, traced. Its setup never turns EEE on (Linux enables EEE on the
  RTL8156 only from revision 7030), and no on-sequence for it is known,
  so it stays off whatever the keyword says.
- **`*EEE`**: `rtl815x`: the chip's EEE on (the steps it runs now, Linux

  `rtl_eee_enable(true)`) or off (Linux `rtl_eee_enable(false)`: the
  MAC's EEE enables cleared, the PHY's EEE configuration off, the EEE
  advertisement 0; on the RTL8152 its own three PHY registers as Linux's
  `r8152_eee_en(false)` writes them). `ax88179`: on, Linux's
  `ax88179_enable_eee` and the EEE advertisement of 100 and 1000 Mb/s; off,
  as now (`ax88179_disable_eee`, advertisement 0).
- **`EEEMaxSupportSpeed`** (RTL8156 core, with EEE on): the EEE
  advertisement stops at that speed: 2500 advertises 100, 1000 and 2.5G;
  1000 (the default) 100 and 1000; 100 only 100; 10 none (EEE at 10 Mb/s
  is the PHY's own 10BASE-Te enable, which `*EEE` sets).
- **`EnableGreenEthernet`** (RTL8153B, 8153C and the 8156 core): Linux's
  `GREEN_ETHERNET`. Off: the 8153B/C keep the full PHY bias
  (`r8153b_green_en(false)`'s values) and the 8156 core clears
  `GREEN_ETH_EN`; on: as Linux after the first setup (the lower bias,
  `GREEN_ETH_EN` set), which is what the driver does now.
- **`AdvancedEEE`** (RTL8153B and later): Linux's EEE+ (`PLA_EEEP_CR`,
  `rtl_set_eee_plus`). Off: never on. On: as now, at 10 Mb/s only, never
  on the RTL8159.
- **`EnableExtraPowerSaving`**, "Idle Power Saving": the USB 2 PHY's P3
  state (`UR_U2P3` bit 0; Linux `r8153_u2p3en`), which Realtek's XP and
  Vista INFs name `EnableU2P3` under the same description. The driver
  turns it on today on the RTL8153 5C20 and 5C30 (PHY setup and wake)
  and the 8156 core (PHY setup and wake), as Linux does. Off: those
  steps are skipped, the ones that clear it stay, so it stays off. On: as
  now. On the other chips it is never on (above).
- **Transmit checksums** (RTL8157 and RTL8159, 2000 to 7): 0 takes the
  task out of what the core offers TCP/IP (section 4.9); 1, 3 keep it; 2
  (receive only) is taken as 0, the receive checks not being offered.
- **`NetworkAddress`**: as section 4.8; on `cdcnet` (NCM with D1 only)
  `SetNetAddress` is sent with the data interface at alternate 0, before
  it is selected. A refusal at start leaves the function's own address
  for the adapter's life; an address taken at start is set again at each
  resume and reset, and a refusal then fails that bring-up, so the
  function and NDIS never carry traffic on different addresses. A resume
  whose bring-up fails (an address refused, or any of its requests
  failing while the function wakes) leaves the data path closed and the
  link down; the worker then retries the bring-up on its own, after 1,
  2, 4 and 8 quiet seconds and then every 8, six times in all, and on
  success restores the cached packet filter, reports the link and
  reopens the data path. After the last retry a reset, another sleep or
  a replug still tries it.


Every keyword is read once, at `MiniportInitialize`, before the chip's
`Start`; a change takes effect when the adapter restarts (disabled and
enabled, or the system restarted), as the Advanced tab's change does on
these systems. A resume re-applies the values read.

#### 4.10.5 The core and the chip interface

`netchip.h` gains a settings table in `NET_CHIP`: `Settings` (an array of
`NET_SETTING`: the keyword's ASCII name, its default, and its allowed
values) with `SettingCount`. The values reach the chip through
`NET_PROBE`, which the lower edge keeps for the adapter's life and hands
to `Start` and `Resume`: `NET_PROBE.Settings` points at one value per
table entry, in the table's order, set before `Start` and never changed
after. The chips' `Start` clears its own state and reads the values
from the probe there, so nothing a start clears holds them. A value a
chip cannot apply (above) is named in `NET_PARAMS.Ignored`, a bit per
table entry, which the lower edge traces after `Start`. The core
(`netcore.c`, all four copies) reads each keyword in `Initialize` with
`NdisReadConfiguration` as a string and parses it as decimal digits
itself: the Advanced tab writes strings on every system, and
`NdisParameterInteger`'s base on 98 SE was not read; a `REG_DWORD`
(type integer) is taken as is. A value that is missing, malformed or not
in the allowed list becomes the default, traced. The transmit checksum
keywords are the core's own: on 2000 and later it masks
`NET_PARAMS.Offload` with them; 98 SE and ME have no offload. `cdcnet`
(its own code, no shared core) reads `NetworkAddress` with
`NdisReadNetworkAddress` as the shared core does.

#### 4.10.6 The INFs

The NT INFs (2000 to 7) and the 98 SE/ME INFs carry the same keys, one
`AddReg` section per keyword set: `Ndi\params\<keyword>` with
`ParamDesc`, `type` (`enum`, `edit` for `NetworkAddress`), `default` and
the `enum` values, and the value itself in the driver key at its default
(`HKR,,<keyword>,0x00000002,"<default>"`, `FLG_ADDREG_NOCLOBBER`, so a
reinstall keeps a changed value). `NetworkAddress` stays optional with
no stored value. The descriptions are this repository's own wording.
`RTL81598.INF`, `RTL815X.INF` and `RTLXP7.INF` gain one install section
per speed class; `AX772F98.INF` and `AX772F.INF` one for the SR9800;
`AX179F98.INF` and `AX179F.INF` one for each Allied Telesis id.


#### 4.10.7 Legs

Host tests: the core's parse (missing, malformed, out of list, integer
type), each chip's register writes for each value against its model.
Guest legs, on the simulators (`rtlemu.py`, `axemu.py`, `ax7emu.py`,
`ncmemu.py`), on 98 SE (the 9x INF form) under NUSB's EHCI and under
xHCI98, on XP (the NT form), and on 7 x64 for `rtl815x`:

- After install the driver key holds each keyword at its default
  (read back with the test aid `NETSET.EXE`).
- The Advanced tab itself, once per INF form (98 SE's `netdi.dll` with
  the starred names, and an NT system): every keyword is listed with its
  default; one value is changed through the dialog, the dialog reopened
  to show it, and the value read back (its registry type and text).
- Every other value is written with `NETSET.EXE` as the tab writes it,
  the adapter restarted, and the simulator's log shows the registers the
  value sets (EEE enables and advertisement, the PHY control register and
  advertisement, the pause bits, green and bias, EEE+, U2P3, the node id),
  and the link and a ping pass.

## 5. USB side: binding and configuration

### 5.1 What the driver binds to, per host stack

| Device shape | xHCI98 | Microsoft stacks (98 SE/ME USB 1.1 + NUSB, 2000-8) |
|---|---|---|
| Vendor dongle, several configurations (RTL8153; `r8152.c` line 10456: "The vendor mode is not always config #1") | Never split: one device PDO with the whole-device ids (design record 13 section 10.8); the function driver configures it | The INF matches the hardware id `USB\VID_vvvv&PID_pppp`, which outranks any composite compatible id, so the whole device comes to this driver |
| Device class `02` (QEMU `usb-net`: `bDeviceClass` `USB_CLASS_COMM`, two configurations) | Whole device (device class of its own) | Whole device |
| Single configuration, IAD-grouped function (Android RNDIS + ADB/MTP, Linux composite gadgets) | Split by IAD; the function PDO's compatible ids come from the IAD, e.g. `USB\Class_E0&SubClass_01&Prot_03` (section 10.8 rule 1) | XP SP2+, Vista, 7: `usbccgp` groups by IAD (Microsoft's documentation; not read here). 98 SE, ME, 2000: the composite parent predates IADs and is expected to split per interface, leaving the control and data interfaces in different devnodes, which no CDC driver can use (unverified; a guest reading would settle it). Not served there (owner) |
| Single configuration, two CDC interfaces, no IAD | Split per interface (no CDC rule; section 10.8 item 3); unusable as above | Same, on every Microsoft stack |

Consequences for `cdcnet`:

- The INF lists class/compatible ids (`USB\Class_02&SubClass_06`,
  `USB\Class_02&SubClass_0D`, the four RNDIS triples) for function PDOs, and
  hardware ids for known dedicated whole devices (`USB\VID_0BDA&PID_8153`,
  others as read).
  A whole-device VID/PID line is allowed only for a verified dedicated
  network device. A shared gadget VID/PID is not evidence that every
  firmware using it is single-function, and a whole-device match takes
  the device from the composite parent before `Probe` can look: failing
  initialization afterwards does not give its other functions back.
  An INF line cannot test the descriptors, and Linux lets any gadget
  combine any VID/PID with any functions, so `0525:A4A2` (QEMU's
  `usb-net`, Linux's Ethernet gadget in RNDIS mode) and `0525:A4A1`
  (CDC-only gadgets) were kept to the test-only INFs (`CDCTEST.INF`,
  `CDCT98.INF`) in `src/net/cdcnet/`; for `0525:A4A2` the owner's
  decision below supersedes this. Composite matches stay at the
  function/interface level. Record the descriptor set behind every
  whole-device allow-list entry in M0.
- Owner's decision, 2026-10-24 (the 0.1.0 re-cut for xHCI98 3.2.0.0),
  superseding "a shared gadget id never goes in a released INF" for this
  one id: the released INFs (`CDC98.INF`, and every models section of
  `CDCNET.INF`) list `USB\VID_0525&PID_A4A2`, the Linux Ethernet
  gadget's default id (`g_ether` with RNDIS) and QEMU's `usb-net`. That
  device is class `02` at the device level with two configurations
  (RNDIS value 2 listed first, ECM value 1; section 2.3), so no stack
  splits it and no interface-level line can match it. 98 SE under
  xHCI98 3.2.0.0 reads its ids as hardware
  ids `USB\VID_0525&PID_A4A2&REV_0000` and `USB\VID_0525&PID_A4A2`, and
  compatible ids `USB\CLASS_02&SUBCLASS_00&PROT_00`,
  `USB\CLASS_02&SUBCLASS_00` and `USB\CLASS_02`. With the interface-only
  INFs, 98 SE found no driver for it (xHCI98's matrix leg N6). The line
  uses the same install sections the test INFs used in every `cdcnet`
  leg, and the driver handles the whole device as in those legs: it picks
  ECM, or RNDIS with the preset, and selects only its own interfaces. The
  risk above stays for firmware that reuses the id on a composite device:
  `cdcnet` gets the whole device and the other functions get no driver.
  The release notes keep this composite-gadget risk. `0525:A4A1` and
  other gadget ids stay test-only. No device-level class line
  (`USB\Class_02`) is listed: it would also take CDC ACM modems.
- On a whole device the driver selects one configuration and only its own
  interfaces. On a function PDO it selects a configuration naming only the
  interfaces the function owns, which is how xHCI98 routes per-function
  SELECT_CONFIGURATION (design record 13 section 10.9) and how `usbccgp`
  behaves.
- Where a stack splits a CDC function apart (the last two rows), the only
  route would be a whole-device hardware-id line. Owner, 2026-10-20: INFs
  match interface-level ids and never bind a multi-function device
  generically; Android tethering under Microsoft's 98 SE, ME and 2000 stacks
  is not planned, so no phone is listed that way. Such devices are served
  under xHCI98 and on XP and later; any other unsplit device gets a VID/PID
  line only on request.

### 5.2 Configuration choice

The USB core reads every configuration descriptor (`GET_DESCRIPTOR`,
`CONFIGURATION`, index 0 to `bNumConfigurations`-1), validates descriptor
boundaries and passes the resulting buffers to the pure `Probe` parser.
`Probe` picks, in order of
the package's preference: `rtl815x` the configuration whose first interface
is vendor class `FF` (the same test as `r8152.c`'s
`rtl8152_cfgselector_choose_configuration`); `cdcnet` NCM over ECM over
RNDIS (NCM aggregates; RNDIS is the least robust and most parsing). Whether
98 SE's Microsoft USBD lets a function driver select a configuration other
than the first was not read (unverified); xHCI98 leaves an unsplit device
Addressed for its driver to configure (section 10.8).

### 5.3 Pipes and transfers common to all

- One bulk IN, one bulk OUT, one interrupt IN (status). The interrupt pipe
  always has one URB outstanding.
- Receive: N bulk-IN URBs outstanding (N = 4 for ECM/RNDIS at Full/High
  Speed, 8 with aggregation; tunable in the registry), each a buffer from a
  non-paged pool, `USBD_SHORT_TRANSFER_OK` set, reposted from the completion
  routine through the submission pump of section 4.7.1, never by direct
  recursion. Frames are copied out of the buffer into NDIS packets from a
  preallocated pool and indicated in batches; the buffer reposts at once.
  Copying keeps buffer ownership simple and suits aggregation, where one
  buffer holds many frames; zero-copy indication is a later option.
- Send: frames queue in the core; the package packs as many as fit into one
  bulk-OUT transfer (one frame for ECM, many for NCM/RNDIS/vendor modes);
  at most M logical transfers outstanding (4 by default for aggregated
  protocols). ECM and the initial RNDIS path use one logical OUT transfer
  at a time until the termination tests below pass.
- **Transfer size.** On 2000 and 98 a pipe's `MaximumTransferSize` defaults
  to `USBD_DEFAULT_MAXIMUM_TRANSFER_SIZE`, which is `PAGE_SIZE` (2000 DDK
  `inc\usbdi.h` line 324); the driver sets it to its largest buffer in
  SELECT_CONFIGURATION. Whether 98 SE's own USBD caps it was not read.
- **Short-packet termination.** A bulk-OUT transfer whose length is a
  multiple of `wMaxPacketSize` must be ended short. ECM ends it with a
  zero-length packet. RNDIS ends it with one appended zero byte, which is
  outside the last message's `MessageLength` and its `DataLength`, as
  Microsoft's [RNDIS USB short-packet rule](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/usb-short-packets)
  requires; a ZLP is not its substitute. Linux pads by one byte where the
  device tolerates it and sends a ZLP otherwise (`usbnet.c` lines
  1449-1462); for ECM `cdcnet` sends a separate zero-length bulk transfer, which
  xHCI98 supports (`src\xhci_xfer.c` lines 851-859); Microsoft's UHCI/OHCI/
  EHCI stacks' handling of a zero-length bulk OUT was not read. The AX88179
  instead sets padding bits in its header (`ax88179_178a.c` lines
  1485-1486); NCM leaves a short NTB or pads per its spec.
  An ECM payload and its required ZLP are one logical send: no following
  payload may overtake or separate them. In v1 the next ECM payload is
  submitted only after the previous payload and any ZLP complete. The
  RNDIS trailer byte is part of its own transfer, so it needs no second
  submission.
  A completed ZLP is not an Ethernet packet. The protocol returns its
  termination action explicitly; the common USB code must not add a byte
  to an Ethernet frame by a generic padding rule.

## 6. Per-protocol USB detail

### 6.1 CDC-ECM

Specification: USB-IF CDC 1.2 and its "Ethernet Control Model" subclass
document (ECM 1.2), to be fetched into `tools/` or a `refs` folder.

- Interfaces: Communications `02/06/00` with an interrupt IN, Header, Union
  and Ethernet Networking functional descriptors; Data `0A/00/00` whose
  alternate 0 has no endpoints and alternate 1 has the bulk pair. Selecting
  alternate 1 starts traffic; alternate 0 stops it (used for reset and
  suspend).
- Ethernet Networking descriptor: `iMACAddress`, `wMaxSegmentSize`,
  `wNumberMCFilters` (bit 15: imperfect filtering), `bNumberPowerFilters`.
- Requests: `SET_ETHERNET_MULTICAST_FILTERS` (0x40),
  `SET_ETHERNET_PACKET_FILTER` (0x43) (QEMU `dev-network.c` lines 88-91 for
  the codes); statistics requests are optional and not used.
- Notifications: `NETWORK_CONNECTION` (0x00) and `CONNECTION_SPEED_CHANGE`
  (0x2A).
- The interrupt-IN transfer is one `wMaxPacketSize` (at most 64), not a
  fixed 64 bytes. A USB transfer ends only on a short packet or when its
  length is met, so a larger transfer lets a full-size packet run on into
  the next notification: on 98 SE under xHCI98 a 16-byte
  `CONNECTION_SPEED_CHANGE` on a 16-byte endpoint and the following
  `NETWORK_CONNECTION` 0 arrived in one 24-byte completion, and an 8-byte
  notification on an 8-byte endpoint (RNDIS `RESPONSE_AVAILABLE`, ECM/NCM
  `NETWORK_CONNECTION`) would wait for a second packet that may never come
  (`2026-10-23-cdcnet-ncm-sim.md`). With one packet per transfer every
  packet completes at once. A notification longer than a packet (16 bytes
  on an 8-byte endpoint) then arrives as two full packets:
  `CdcNotifyFeed` holds the head of a full packet and joins it to the
  next. Each completion is walked notification by notification (8 bytes
  plus `wLength`), in order, so coalescing on any stack is still handled;
  a truncated tail that cannot continue is dropped and counted. Linux
  `usbnet` also posts one `wMaxPacketSize` for its status URB.
- Data: one raw Ethernet frame (no FCS) per bulk transfer.

### 6.2 CDC-NCM

Specification: USB-IF "Network Control Model" 1.0 (in the CDC 1.2 set).

- Interfaces as ECM but subclass `0D`, plus the NCM functional descriptor.
- Setup: `GET_NTB_PARAMETERS` (NTB sizes, divisors, alignment, maximum
  datagrams per NTB), `SET_NTB_INPUT_SIZE`, optional `SET_NTB_FORMAT`
  (16-bit only in v1), CRC off through `SET_CRC_MODE` only when advertised,
  `SET_MAX_DATAGRAM_SIZE` if supported; then the discovered active data
  alternate setting. Require NTB16 support; do not probe unsupported
  optional requests as if they were mandatory.
- Receive: each transfer is one NTB16: a 12-byte NTH16 ("NCMH") pointing to
  one or more NDP16 ("NCM0") tables of datagram index/length pairs.
  `RxParse` walks them with every offset and length bounds-checked.
- Send: pack frames into an NTB16 up to `dwNtbOutMaxSize` and the device's
  datagram limit, with `wNdpOutAlignment`, `wNdpOutDivisor` and
  `wNdpOutPayloadRemainder` honoured; a short
  NTB ends the transfer, otherwise a ZLP per the spec's rules (Linux
  `cdc_ncm.c` lines 1434 and the `FLAG_SEND_ZLP` variants at 1928-1945 show
  device-specific differences).
- Linux `cdc_ncm.c` is dual-licensed (GPL-2.0 or BSD-2-Clause); it is
  consulted for behaviour (facts) only, and `cdcnet`'s NCM code is written
  from the CDC NCM specification.

The parameter names are device-relative: `In` constrains device-to-host
NTBs, `Out` constrains host-to-device NTBs. Cap both directions to a local
allocation limit and the NTB16 block-length range; never allocate directly
from a descriptor value. NDP-table alignment and datagram payload
divisor/remainder are distinct constraints. The field names and capability
bits are cross-checked against the
[Linux CDC definitions](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/include/uapi/linux/usb/cdc.h)
and setup behavior against
[`cdc_ncm.c`](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/net/usb/cdc_ncm.c).
The USB-IF NCM specification remains the source for the wire contract.

Before emission, validate NTH signature/header/block length, every NDP
signature/length/link, and the terminating zero datagram entry. Bound the
number of tables and datagrams; reject cycles, truncated tables, overlapping
metadata/payload ranges and frames outside the Ethernet size limit.
Offset/length checks use subtraction after establishing the offset is in
range, so integer wrap cannot turn a malformed block into a valid one.

### 6.3 RNDIS

Specification: Microsoft's published [MS-RNDIS] protocol document (Open
Specifications), to be fetched.

- Control channel: `SEND_ENCAPSULATED_COMMAND` / `GET_ENCAPSULATED_RESPONSE`
  on endpoint 0, with `RESPONSE_AVAILABLE` on the interrupt endpoint.
- Sequence: `INITIALIZE` (the host offers its maximum transfer size; the
  device answers its own and the packets per transfer it accepts,
  `rndis_host.c` lines 348-358), query `OID_GEN_PHYSICAL_MEDIUM` (to refuse
  wireless RNDIS, which is out of scope) and
  `OID_802_3_PERMANENT_ADDRESS`, set `OID_GEN_CURRENT_PACKET_FILTER`
  (lines 391-429). Send idle `KEEPALIVE` probes and answer device probes;
  `HALT` on an orderly stop with the device present; `RESET` on
  `MiniportReset`.
- Data: each frame behind a 44-byte `PACKET_MSG` header; several messages
  per transfer up to the negotiated limits.
- The device controls every offset and length in responses and packets;
  `cdcnet` validates them all before use. RNDIS parsing bugs are a known
  source of host-side faults, so this code gets the most review and fuzzing
  (section 10).

One control exchange is active at a time, with a request ID, expected
response type, bounded response size and deadline. Unsolicited status and
device keepalive messages cannot be mistaken for its completion. An idle
timer queues a host keepalive only while running; it follows the USB
mapping's timeout, confirmed in M0. Host initiation is required as well as
answering device probes by the
[keepalive contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/remote-ndis-keepalive-msg).

For data, validate each complete message before emitting any frame in the
transfer. `DataOffset` is relative to the field at byte 8, not to the start
of the message. Advance by validated `MessageLength`, including padding;
respect negotiated packet alignment and transfer limits. After the last
message a transfer may carry trailing zero bytes outside any
`MessageLength` (the device's one-byte short-packet terminator, or
padding up to the transfer end). A remaining tail that is entirely zero
is accepted as that padding, whatever its length, within the validated
transfer bound; a tail with any non-zero byte that is not a valid
message makes the transfer malformed. Tests cover padding shorter than,
equal to and longer than the 44-byte header. Bounds-check
optional metadata even when v1 ignores its meaning. These offsets and
padding rules come from Microsoft's
[packet message definition](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/remote-ndis-packet-msg).

### 6.4 RTL8152/8153 vendor mode (`rtl815x`)

From `r8152.c`: chip revision read from the PLA version register
(`rtl8152_get_version`, lines 10015-10090) and mapped to `RTL_VER_01`-`17`;
v2 supports `RTL_VER_01/02/07` (8152, 8152B; `r8152b_init`), `03`-`06` (8153;
`r8153_init`), `08/09` (8153B; `r8153b_init`) and `14` (8153C), with 8156 and
later after. Register access is vendor control requests on the OCP space;
bring-up is the per-revision `init`, `hw_phy_cfg`, `up` and `down` routines.

Owner, 2026-10-25: the chip core (`rtlchip.c`, `rtlchip.h`), first adapted
from `r8152.c` function by function, was rewritten, ahead of the move of
the repository to GPL-2.0-or-later. The rewrite keeps the register facts (chip ids
`4C00`, `4C10`, `4800`, `5C00`-`5C30`, `6000`, `6010`, `6400`; register
addresses, bit values, the order of reads and writes, and which values
read back decide a path) and states them as tables of register steps run
by one interpreter, under this project's own names (`MR_`, `UR_`, `PR_`,
`SR_`); no code, function names, comments or structure of `r8152.c` are
kept. The facts were taken from the adapted core and its control-request
traces. Check (`docs/testing/2026-10-25-rtl815x-rewrite.md`): a trace
harness drove the old and new cores through every chip id at Full, High
and SuperSpeed and 24 register-model variants (stuck and absent bits,
efuse and PHY values, failed reads); the logged requests, sleeps and
results are byte-identical. With a failure injected at each request the
request count, status and register state match; the diagnostic
`Timeouts` count differs only after an I/O failure. The core reports the chip id (`NET_PARAMS.Version`, e.g.
`5c10`) where it reported Linux's `RTL_VER_` number before.

- Receive: aggregated, each frame behind a 24-byte `rx_desc` (six dwords,
  lines 821-840), buffers of 16-48 KiB by revision (`rx_buf_sz`, lines
  9754-9909); the driver walks descriptors with bounds checks.
- Send: each frame behind an 8-byte `tx_desc` with first/last-segment bits
  (lines 842-850), several frames per transfer.
- **Firmware patches**: Linux requests `rtl_nic/rtl8153a-2.fw` and others
  (lines 9925-9943) but continues without them: `rtl8152_request_firmware`
  logs "unable to load firmware patch" and bring-up proceeds to
  `hw_phy_cfg` (lines 5659-5689, 7069-7080). The blobs are under Realtek's
  `linux-firmware` licence, not GPL; v2 runs without them, and any effect on
  link stability is a test item.
- USB 3 link power management: Linux sets `disable_hub_initiated_lpm`
  (line 10453); whether xHCI98 enables U1/U2 for such a device, and what the
  driver must do about it, is a question for xHCI98's maintainer (owner).

#### 6.4.0 RTL8156, RTL8156B and RTL8153D (the 8156 core)

Owner, 2026-10-27 (xHCI98 3.3.0.0, task 44.70 part B): the core serves the
RTL8156 (chip ids `7020` and `7030`, Linux's `RTL_VER_10` and `11`), the
RTL8156B (`7400`, `7410`; `RTL_VER_12`, `13`) and the RTL8153D (`7420`,
`RTL_VER_15`, a 1 Gb/s chip on the RTL8156B's core) [linux] lines
10055-10075. The id `7010` (Linux's `RTL_TEST_01`) stays refused. No such
adapter is on the bench; the runs are against `rtltest.c`'s model and the
simulator (`docs/testing/2026-10-27-rtl815x-newchips.md`). [linux] means
`r8152.c` at v7.3, the same file as `f47d2236`; only its register facts
are used (GPL-2.0-only), restated as this core's own step tables.

- **Tables**: one base setup (`SqSetupMG`) for the 8156 core and its
  successors, with lines guarded per chip ([linux] lines 8366-8508); PHY
  setup per family, `SqPhy56A` for the RTL8156 ([linux] lines 7502-7714)
  and `SqPhy56B` for the RTL8156B and RTL8153D ([linux] lines 7716-7973),
  each with per-chip tuning tables (`SqPhy7020`, `SqPhy7030`,
  `SqPhy7400Pre`, `SqPhy7400`, `SqPhy7410`, `SqPhy7420`); wake
  (`SqWakeMG`, [linux] lines 6824-6886), link up (`SqRxOnMG`, lines
  6408-6460, 3597-3618), link down (`SqRxOffMG`, lines 6381-6387,
  6462-6468), sleep (`SqSleepMG`, lines 6888-6935) and detach
  (`SqDetachMG`, lines 9681-9691). The receiver and transmitter reset
  differs per chip: the RTL8156 turns its receiver and transmitter off
  around a buffer-unit reset with the ECM bit set, the RTL8156B and
  RTL8153D reset the MAC [linux] lines 3421-3456.
- **New step kinds**: the second PHY SRAM window (`0xb87c`/`0xb87e`), the
  indirect window (section 6.4.1) and the PHY patch window as steps. A
  patch window the PHY does not grant within 5 s is withdrawn, the PHY
  setup ends there, and the autonegotiation after it restarts without a
  PHY reset [linux] lines 4577-4610, 7598-7599, 7943-7944, 6610-6613.
- **Firmware**: Linux names `rtl_nic/rtl8156a-2.fw` (`7030`) and
  `rtl8156b-2.fw` (`7410`, `7420`) [linux] lines 9969-9976; none is loaded
  here, and Linux's PHY setup goes on without one [linux] lines 5579,
  7509-7520. The `7400`'s PHY setup has its own patch-window block of PHY
  writes, which needs no file [linux] lines 7873-7893.
- **Autonegotiation**: 2.5G is advertised (`0xa5d4` bit 7) by the
  `7020` to `7410` at USB SuperSpeed only; at High Speed and below the
  chips advertise 1000 at most, as Linux does [linux] lines 10250-10270.
  The RTL8153D advertises no multi-gigabit speed.
- **EEE**: off on the `7020` (Linux enables it only from the `7030`);
  100 and 1000 advertised, and 2.5G in EEE advertisement 2 (`0xa6d4`
  bit 0) on the others, the RTL8153D included [linux] lines 9806-9835,
  5773-5843.
- **An RTL8156 with reversed pairs**: when `0xd842` bit 5 and the PHY's
  `0xd068` bit 1 say so, the PHY's per-pair values are exchanged, pair A
  with D and B with C [linux] lines 7636-7691.
- **Link up**: early receive timeout 640 ns, the extra aggregation timer
  15 us, early receive size in 8-byte units from the buffer; at 2.5G the
  idle slowdown off; the RTL8156's EEE wake time by speed; at USB High
  Speed the L1 timing by flow control [linux] lines 6408-6460, 3637-3714.
- **Buffers**: Linux uses 48 KiB bulk IN on the 8156 core [linux] lines
  9823, 9848; this core uses 32 KiB (`RX_BUFFER_MAX`), with the early
  receive size set from it. Receive and transmit records are the first
  format (24-byte and 8-byte headers).
- **Link speed**: `0xe908` bit 10 gives 2500 Mb/s; NDIS gets 25,000,000
  in 100 b/s units.
- **USB ids**: the 98 SE/ME and 2000 INFs add Realtek's `0BDA:8155` and
  `0BDA:8156` [linux] lines 10402-10403 to the ids they had; the core
  takes the chip from its id register, so any listed id serves any of
  these chips. Linux's table does not say which id an RTL8153D uses.
- **Whose driver serves what** (2026-11-06, xHCI98 task 47.25): Realtek's
  own drivers cover the RTL8156 and RTL8156B on 7 only (its 7 7.61.20 INF,
  in `usb-wdm-nonfree`). Its XP 5.23 and Vista 6.27 INFs do not list
  `0BDA:8156`, and Realtek offers nothing for 98 SE, ME or 2000 (its
  download list, read 2026-11-06). `rtl815x` therefore serves both
  chips on 98 SE, ME, 2000, XP, XP x64 and Vista, and leaves 7 to
  Realtek (section 7.6). The legs ran under xHCI98 3.4.1.0 on 98 SE,
  ME, 2000, XP and XP x64, at 2.5 Gb/s at SuperSpeed and at 1 Gb/s at
  High Speed, and under NUSB's EHCI on 98 SE. Every leg used the simulator
  ([testing/2026-11-06-rtl8156.md](../testing/2026-11-06-rtl8156.md)).
  `0BDA:8056` is not in Linux's table. Realtek's 7 INF lists it in its
  RTL8156B sections. Owner, 2026-11-06: `RTL81598.INF`, `RTL815X.INF` and
  `RTLXP7.INF`'s XP, XP x64 and Vista sections list it too, taking the id
  only from that INF. No device on it has been seen. The chip core reads
  the chip from `MR_TX_CONFIG` and never from the USB product id, so the
  id needs no chip path of its own. The simulator presents it as
  `--chip 8156b-8056` (chip id `7410`, the model's inference), and an XP
  leg passed. 7 stays Realtek's.  Owner, 2026-11-06: `STAGE.TXT` stages
  `rtl815x` on 98 SE, ME, 2000, XP and XP x64 on these simulator legs;
  Vista and 7 stay copy-only.

#### 6.4.1 RTL8157 (5G) and RTL8159 (10G)

Owner, 2026-10-27 (xHCI98 3.3.0.0, task 44.70): the core serves the
RTL8157 (chip id `1030`, Linux's `RTL_VER_16`) and RTL8159 (`2020`,
`RTL_VER_17`) [linux] lines 10077-10080. No such adapter is on the bench;
every run is against `rtltest.c`'s model or the simulator. The facts were
read from `r8152.c` at `f47d2236` (the v7.3 file, the same lines) and
restated as this core's own step tables: the 8156 core's shared tables
(`SqSetupMG`, `SqWakeMG`, `SqRxOnMG`, `SqRxOffMG`, `SqSleepMG`,
`SqDetachMG`, with `K_57` lines), and the two chips' own PHY setup
(`SqPhy57`, `SqPhy1030`, `SqPhy2020`) and descriptor format (`DESC_V2`).
A first, separate set of tables for the two chips, read from Linux
independently, gave the same requests as the shared tables
(`docs/testing/2026-10-27-rtl8157-8159.md`).

- **Firmware**: Linux names `rtl_nic/rtl8157-1.fw` and `rtl8159-1.fw`
  [linux] lines 9983-9986, but applying a patch returns at once when none
  was loaded [linux] line 5579, and the PHY setup goes on [linux] lines
  7069-7080, 7975-8352. Both chips are brought up without a patch, as the
  older chips are. One tuning comment ("with uc2 patch", [linux] line
  8226) suggests that some RTL8159 values assume the patch: a real-chip
  check item.
- **Bring-up** ([linux] lines 8366-8512): bits in USB `0xcffe`, `0xd3ca`,
  `0xd26b` and `0xcfee`; U1/U2 off; the wait for the PHY parameters
  loading from flash (`0xd3ae` bit 1, `0xd284` bits 2 and 5, [linux] lines
  5884-5898); autoload; on the RTL8159 after a power cut a 20 ms wait for
  `0xd284` bit 6, else the chip is not used [linux] lines 8354-8364,
  8413-8417; a PHY in external init let go of; U2P3 off through the
  indirect window; timers; power cut and UPS off (the PHY set up again if
  the cut flag stays set, [linux] lines 4276-4321); no wake source; U1/U2
  on at SuperSpeed; MAC clock slowdown; link flags; receive aggregation
  on, with `0xd406` bit 10 (and bit 11 on the RTL8159) cleared;
  zero-length receive off in the buffer unit; the transmit signature
  checked (`0xd4ae` bit 1 cleared); clause 22 forced mode; tally reset.
- **Indirect window** (new on these chips): address `0xd5d6`, data
  `0xd5d8` (dword), command `0xd5dc` (bit 0 busy, bit 1 write, bit 2 the
  USB IP target, else the buffer unit). An access waits for idle (2 ms
  polls, 20 ms) and is given up if the window stays busy [linux] lines
  1688-1735.
- **PHY setup** ([linux] lines 7975-8352): ALDPS and EEE off, the PHY's
  power-saving bits, each chip's PHY tuning values (99 steps on the
  RTL8157, 126 on the RTL8159, many through the PHY SRAM windows at
  `0xa436` and `0xb87c`), then, while the PHY grants a patch request, EEE
  clock division; green Ethernet, EEE (advertising 100 and 1000, 2.5G and
  5G, and 10G on the RTL8159, [linux] lines 9872-9874, 9894-9896,
  5773-5843), ALDPS and pause. A request not granted ends the PHY setup
  there (Linux also leaves out the PHY reset of the autonegotiation after
  it, [linux] lines 8323-8324, 8351, 6610-6611; so does this core, for
  every chip of the 8156 core, section 6.4.0).
- **Autonegotiation**: 10/100/1000 as before, and 2.5G and 5G (and 10G on
  the RTL8159) in MMD 7 register 32 (`0xa5d4`), at USB SuperSpeed only;
  below SuperSpeed those bits are cleared [linux] lines 6580-6603,
  10262-10280. The simulator's author reports that an RTL8159 without its
  PHY patch links at 5 Gb/s at most (Linux commit `a51e171e`, not read
  here); v7.3 still advertises 10G, and so does this core. The link speed
  NDIS gets is the one the chip reports.
- **Up and down** ([linux] lines 6824-6935): the MAC is not reset; its
  receiver and transmitter are turned off instead [linux] lines 3435-3438.
  VLAN removal (`0xc012` bits 6-7) off while up and on in OOB; jumbo
  transmit size, flow-control thresholds at 8 and 16 KiB, transmit FIFO
  thresholds.
- **Link up** ([linux] lines 6408-6430, 3597-3618, 3633-3714): no limit on
  frames per transfer (`0xd4ee` bits 0-8 cleared); early receive timeout
  640 ns and the extra aggregation timer 15 us; early receive size in
  16-byte units, (buffer - 1522 - 16 - 16) / 16; EEE+ never on the RTL8159
  [linux] line 3476.
- **Records** ([linux] lines 864-896, 9718-9732, 3037-3131, 2630-2770):
  receive headers of 16 bytes, the length with its FCS in bits 17-31 of
  the first dword, records 16-byte aligned; transmit headers of 16 bytes,
  16-byte aligned: the first and last bits with the length, zero, the
  length shifted left 4, and the signature `0xa8000000`.
- **Buffers**: Linux uses bulk IN 32 KiB on the RTL8157 and 48 KiB on the
  RTL8159 [linux] lines 9887, 9909; this core uses 32 KiB for both, the
  lower edge's largest (`RX_BUFFER_MAX`), with the early receive size set
  from it, so no transfer is longer than the buffer. Transmit 16 KiB as for
  the others [linux] line 1275 (32 KiB was tried, section 6.4.2).
- **Link speed**: `0xe908` bits 10, 12 and 14 give 2500, 5000 and 10000
  Mb/s [linux] lines 746-748; NDIS gets them in 100 b/s units
  (`OID_GEN_LINK_SPEED`: 100,000,000 at 10 Gb/s, which fits a `ULONG`).
- **Stop** adds `0xcf04` bits 0-2 and 7 cleared to the power-cut-off
  steps [linux] lines 9681-9691.
- **USB ids**: Realtek's `0BDA:8157` and `0BDA:815A` [linux] lines
  10404-10405. Linux's id table lists rebranded ids by vendor, not by
  chip, so none of them is known to be an RTL8157 or RTL8159; the core
  takes the chip from its id register behind whatever USB id the INF
  lists.

#### 6.4.2 The RTL8157 on real hardware (2026-11-07)

A real RTL8157 (chip `1030`, USB `0BDA:8157` rev 3000) was passed through
to QEMU guests and cabled to a 10GbE port; record
`docs/testing/2026-11-07-rtl8157-hardware.md`. Owner's decisions of
2026-11-06/07 after it:

- **Removal under traffic** (section 4.7): fixed in the shared core.
- **98 SE send status**: 98 SE's NDIS reads the `Status` member of a
  packet a deserialized miniport accepts and hands a packet left at
  another value back to its protocol while the miniport still holds it.
  The send queue (linked through `MiniportReserved`) was then corrupted:
  sends stalled and the 98 TCP/IP took fatal exceptions 0E. Every packet
  is now set to `NDIS_STATUS_PENDING` in `MiniportSendPackets`, which the
  2000 DDK's page on that handler allows for a deserialized driver.
- **The core's lock**: on 98 SE `NdisAcquireSpinLock` did not keep the
  lower edge's USB completions out of the core's queues; the core takes
  the kernel spin lock inside its `NDIS_SPIN_LOCK` directly (on NT what
  `NdisAcquireSpinLock` does).
- **Receive**: frames of one bulk-IN transfer are indicated in arrays of
  up to 16 (`NET_RX_GATHER` on the completion's stack); a pool-low frame
  goes alone with `NDIS_STATUS_RESOURCES` after what is gathered.
- **Bulk-IN depth**: 8 transfers outstanding on every chip (4 before;
  Linux 10, Realtek's Windows 10 driver 5 were seen).
- **Transmit queue**: a valid send is never failed because the core's
  queue is long. The queue is the packets themselves, linked through
  `MiniportReserved`, so it needs no allocation and has no limit, as the
  NDIS 5 rules for a deserialized miniport require. The pump is kicked
  every 8 packets of a long array. Before, a 64-frame limit failed sends
  under load: 3191 in an 11-minute run on XP, each a loss for TCP.
- **Transmit size**: 32 KiB, at most 20 frames, on the RTL8157 and
  RTL8159 (Realtek's size: its Windows 10 driver sent bulk-OUT transfers
  of up to 32848 bytes) was measured with no outbound gain and is held
  for the owner, off this line of commits. Every chip keeps 16 KiB.
- **PHY power-down** at halt, disable and suspend: MII control bit 11 set
  after the sleep steps; every setup table clears it. Realtek's Windows 10
  driver does this on disable (a capture of it on the RTL8157); Linux
  leaves the PHY up, so an rtl815x adapter that was disabled kept a
  5 Gb/s link before.
- **Kept as Linux sets them**: EEE (100M to 5G advertised), ALDPS, EEE and
  10M clock division, MAC clock slow-down. Section 4.10 (owner,
  2026-11-09) puts EEE, green Ethernet, EEE+ and the USB 2 PHY's P3 state
  under the Advanced tab, off by default as in Realtek's 7 and 10 INFs. Realtek's Windows 10 driver
  leaves all of these off by default (`*EEE` 0, `EnableGreenEthernet` 0,
  `AdvancedEEE` 0, `EnableExtraPowerSaving` 0 in its INF; the captured
  register writes agree). Both leave the chip's U1/U2 off while the link
  is up, as Linux does for the 8157, and its UPS and power-cut features
  off.
- **Kept**: the 2 s link poll, the one detector of an unplug while the
  host controller driver holds the transfers.
- **Not done**: checksum offload (later); a larger receive pool for the
  fast chips on NT (an experiment, adopted only if it clearly helps).

Found after it on 98 SE: sustained TCP in either direction stalled after
10-100 MB, once with a corrupted 17512-byte region of the stream, and the
98 TCP/IP (`MSTCP`) faulted with exceptions 0E; the receive pool stayed
whole when it happened. XP never showed it (1.4 GB both ways). Cause and
fix: section 4.4, "Upcalls on 98 SE and ME" (record
`docs/testing/2026-11-07-98se-tcp.md`).

### 6.5 Interaction with the RTL8153's ECM mode

Both `cdcnet` (ECM configuration) and `rtl815x` (vendor configuration) can
drive `0BDA:8153`. Owner, 2026-10-20: `CDCNET.INF` lists the Realtek ids
until `rtl815x` ships; from then `RTL815X.INF` lists them and `CDCNET.INF`
drops them, so one INF matches. `rtl815x` targets 98 SE, ME and 2000 only
(section 1 item 8), so the move is made in those systems' models. Owner,
2026-10-20: `CDCNET.INF`'s XP to 7 models keep the Realtek ids, as no
`rtl815x` ships there, so `cdcnet`'s ECM mode is a free fallback for the
RTL8153 on XP to 7. With no installer, nothing of this package moves a
device already on Realtek's driver; only a user's own Update Driver can.
Where both INFs are staged, Windows' ranking (signature from Vista on,
then the match, date and version) picks one for a new device, and M2
records which. The release notes tell users with Realtek's driver to
keep it.

Owner, 2026-10-24 (usb-wdm-free 0.1.0, shipped in xHCI98 3.2.0.0's free
set): `CDC98.INF` and `CDCNET.INF` no longer list `0BDA:8153` on any
system, and `rtl815x` ships copy-only. The xHCI98 installer stages the free
and non-free sets together and requires one INF per hardware id per
system; with the id in `CDCNET.INF` it clashed with Realtek's own XP and 7
x64 INFs in `usb-wdm-nonfree`, and on 98 SE, ME and 2000 with
`RTL81598.INF`/`RTL815X.INF`. An RTL8153 is served by Realtek's driver from
XP on, and on 98 SE, ME and 2000 by `rtl815x` installed by hand. `pkg.ps1`
now refuses any whole-device id in a `cdcnet` release INF except the
Linux Ethernet gadget's `0525:A4A2` (section 5.1, 2026-10-24).

### 6.6 AX88179/AX88178A (`ax88179`)

From `ax88179_178a.c`:

- Receive: aggregated with the header at the end: a 4-byte trailer giving
  the packet count and the offset of a per-packet metadata array; each
  packet is 8-byte aligned and preceded by 2 bytes of IP-alignment padding
  (comment at lines 1365-1400). Bulk-in aggregation size is set from a table
  by link speed and USB speed (`AX88179_BULKIN_SIZE`, line 186; transfer
  size `1024 * (tmp[3] + 2)`, line 1569).
- Receive mode, FCS (owner, 2026-10-22): `AxStart` writes `AX_RX_CTL`
  with the bits Linux's `ax88179_reset` writes (`DROPCRCERR`, `IPE`,
  `START`, `AP`, `AMALL`, `AB`) and reads it back; the driver logs both
  ("rx config: wrote 3aa, read back 3aa"). Neither Linux nor ASIX's
  public datasheet names a bit that forwards the FCS (the datasheet only
  says forwarding is optional), so the mode is "no FCS": what the chip
  delivered with this configuration on 98 SE, and what Linux assumes (it
  never strips one). The 2026-10-21 host-side run saw the chip forward
  an FCS, so the receive path keeps a safety net: a record whose last
  four bytes are its own CRC32 loses them and is counted; the driver
  logs the first one ("frame arrived with an FCS") and the count at
  halt (`docs/testing/2026-10-22-ax88179-9x.md`).
- Send: 8-byte header (length, and MSS/padding flags) per frame (lines
  1478-1486).
- Bring-up and PHY: the driver's `bind`, `reset` and `link_reset` paths.

### 6.7 Throughput targets

Targets, not measurements; each milestone records what it reached.

| Case | Bus limit | Target |
|---|---|---|
| QEMU `usb-net` (Full Speed only: `.full` descriptors only, 64-byte bulk packets, `dev-network.c` lines 179-183, 302) | 12 Mbit/s | Functional only |
| ECM or RNDIS at High Speed | about 40 MB/s practical bulk | 10 MB/s on 98 SE, more on XP+ |
| RTL8153/AX88179 vendor mode at High Speed | same | 25 MB/s on 2000 |
| RTL8153/AX88179 vendor mode at SuperSpeed under xHCI98 | USB bandwidth exceeds the gigabit link; controller and host overhead must be measured | 60 MB/s on 2000, gigabit wire rate (about 117 MB/s) as a stretch |

At SuperSpeed the per-transfer cost in the host controller driver and the
OS's TCP/IP stack set the limit, which is why aggregation matters and plain
ECM will fall well short of gigabit on any target.

### 6.8 AX88772B and AX88772C (`ax88772`)

Owner, 2026-11-06 (xHCI98 roadmap 47.26): a free driver for the ASIX
AX88772B and AX88772C on 98 SE and ME, which ASIX's drivers leave out.
This reverses section 2.6 for these two chips only. The AX88772 and
AX88772A stay with ASIX's driver in the non-free pack on every system.

**Whose driver covers what** (read 2026-11-06; ethernet-coverage.md has
the full survey):

| System | AX88772B, AX88772C |
|---|---|
| 98 SE, ME | none from ASIX: its 98/ME driver (3.0.3.12, non-free `ax88772/w98se-me/`) lists only `0B95:7720` and `0B95:772A` (its `Ax88772.inf` read 2026-11-06). **`ax88772` (this package)** |
| 2000, XP | ASIX 5.4.3.0 (non-free `ax88772b/w2k-xp/`), `0B95:772B` at any revision |
| XP x64, Vista | ASIX 5.6.3.0, 5.8.2.0, 5.10.2.0 (non-free), `0B95:772B` |
| 7 | ASIX 5.12.12.0 / 5.14.12.0 (non-free), `0B95:772B` revs `0001` and `0002`, `0B95:7E2B`, `2001:1A02` |

So the package targets 98 SE and ME only and has no NT INF; on 2000 and
later ASIX's driver serves the chip. `0B95:7E2B` on 2000 to Vista and
`17EF:7203` on 2000 to 7, which no ASIX INF lists, stay uncovered there
(a lower-priority gap the owner may later give to this package).

**The package.** A new package, `src/net/ax88772/`, `AX772F.SYS`, on
the shared core (netcore.c, netusb.c, netchip.c copied in, as the other
two vendor packages do). Not `ax88179`: the two chips share nothing
below the core. The AX88179 addresses a register file through one
request (`ACCESS_MAC`) and frames receive data with a trailer; the
AX88772 family has one vendor request per register and a 4-byte header
per frame. A combined driver would only share the core, which each
package compiles in anyway, and would put the released, bench-run
`AX179F.SYS` at risk for a chip nobody here has. The `F` suffix keeps
the name off ASIX's own `AX88772B.SYS` (section 3.2). The CoreChips
SR9800, which Linux drives with the same command set and framing
(`sr9800.c`, "Based on asix_common.c"), is added to this package by
roadmap 47.27 (section 6.9).

**Sources.** Linux `drivers/net/usb/asix_devices.c`, `asix_common.c`
and `asix.h` at v7.3 (SPDX GPL-2.0-or-later, so their sequences may be
adapted with their notices kept; section 9), and
`drivers/net/phy/ax88796b.c` (GPL-2.0+) for the embedded PHY. Cited
below by symbol. Facts no Linux file gives are marked *inferred*.

- **Ids** (`products[]`): `0B95:772B` (ASIX AX88772B; the AX88772C uses
  the same id at revision `0002`, as ASIX's 7 INF lists it),
  `0B95:7E2B` (ASUS, `ax88772b_info`), `17EF:7203` (Lenovo U2L100P,
  `ax88772b_info`), and `2001:1A02` (D-Link DUB-E100 C1, which Linux
  serves with `ax88772_info` and ASIX's 7 INF with its AX88772B/C
  driver). The AX88772 and AX88772A ids (`7720`, `772A` and the OEM ids
  on `ax88772_info`) are not listed: ASIX's 98/ME driver has them, and
  the free and non-free packs never list one id on one system. HG20F9
  (`066B:20F9`, `hg20f9_info`, "Appears to be compatible with Asix
  88772B") is left out: no chip is named.
- **Interface** (`usbnet_get_endpoints`): one vendor interface with a
  bulk IN, a bulk OUT and an interrupt IN. *Inferred*: interface 0,
  class `FF`, endpoints 0x81 (interrupt, 8 bytes), 0x82 and 0x03 (bulk);
  the probe takes the first such interface (`NcFindVendorInterface`),
  whatever its endpoint numbers.
- **Requests** (`asix.h`, `asix_read_cmd`, `asix_write_cmd`): vendor,
  device recipient, bmRequestType `C0`/`40`. `SET_SW_MII` 06,
  `READ_MII_REG` 07 (wValue PHY, wIndex register, 2 bytes),
  `WRITE_MII_REG` 08, `STATMNGSTS_REG` 09 (1 byte), `SET_HW_MII` 0A,
  `READ_EEPROM` 0B (wValue word, 2 bytes), `READ_RX_CTL` 0F,
  `WRITE_RX_CTL` 10 (wValue the mode, no data), `WRITE_IPG0` 12,
  `READ_NODE_ID` 13, `WRITE_NODE_ID` 14 (6 bytes),
  `WRITE_MULTI_FILTER` 16 (8 bytes), `READ_PHY_ID` 19 (2 bytes; byte 1
  the embedded PHY's address), `READ_MEDIUM_STATUS` 1A,
  `WRITE_MEDIUM_MODE` 1B (wValue), `WRITE_GPIOS` 1F (wValue),
  `SW_RESET` 20 (wValue), `SW_PHY_SELECT` 22 (wValue), `QCTCTRL` 2A.
- **MII access** (`__asix_mdio_read`, `asix_check_host_enable`): up to
  30 tries of `SET_SW_MII`, 1 ms, `STATMNGSTS_REG`, until its bit 0
  (`AX_HOST_EN`) is set; then the register read or write, then
  `SET_HW_MII`. When the 30 tries run out the access fails and so does
  the stage (Linux still writes on a timeout).
- **Chip code** (`ax88772_bind`): `STATMNGSTS_REG` & 0x70: 0x00
  AX88772, 0x10 AX88772A, 0x20 AX88772B (the AX88772C reads 0x20 too:
  Linux has no other code, and its PHY id, `003b1881` against the
  AX88772A/B's `003b1861` in `ax88796b.c`, tells them apart). Only 0x20
  is served; any other code is refused at Start, with the code in the
  log. An AX88772 resets differently (`ax88772_hw_reset`), and an
  AX88772A's PHY needs Linux's link workarounds
  (`asix_ax88772a_read_status`, `asix_ax88772a_link_change_notify`); no
  listed id is expected to carry either.
- **MAC** (`ax88772_bind`, `FLAG_EEPROM_MAC`): on the three
  `ax88772b_info` ids, EEPROM words 4, 5 and 6 (`READ_EEPROM`, two bytes
  each, the MAC in order); on `2001:1A02` (`ax88772_info`),
  `READ_NODE_ID`. An address that is zero or multicast is reported as
  read; the core then applies section 4.8 (a stored locally
  administered address, written to `NODE_ID` through SetMac), where
  Linux takes a random one.
- **PHY** (`asix_read_phy_addr`, `ax88772_bind`): the address is byte 1
  of `READ_PHY_ID`; it is embedded when it is 0x10 (`AX_EMBD_PHY_ADDR`).
  Only the embedded PHY is served: with an external one Linux also parks
  the embedded PHY with its PLL kept on (`ax88772_init_phy`), which this
  core does not do, so Start refuses it.
- **Bring-up** (`ax88772a_hw_reset`, then `ax88772_reset`, in Linux's
  order): `WRITE_GPIOS` `AX_GPIO_RSE` (0x80) and 5 ms;
  `SW_PHY_SELECT` embedded | `AX_PHYSEL_SSEN` (0x10), 10 ms;
  `SW_RESET` `IPPD|IPRL` (0x60), 10 ms; `SW_RESET` `IPRL` (0x20),
  160 ms; `SW_RESET` 0; `SW_RESET` `IPRL`, 200 ms; on code 0x20
  `QCTCTRL` wValue 0x8000 wIndex 0x8001 (the bulk-IN burst: SR9800's
  table names it the 2 KiB entry, `SR9800_BULKIN_SIZE`); `WRITE_IPG0`
  wValue 0x001D wIndex 0x0012 (Linux writes `AX88772_IPG0_DEFAULT |
  AX88772_IPG1_DEFAULT` and `AX88772_IPG2_DEFAULT`; the value is kept
  as written); `WRITE_NODE_ID` the MAC; `WRITE_RX_CTL`
  `AX_DEFAULT_RX_CTL` (0x0088: `SO` receive on, `AB` broadcast, MFB
  2048); `WRITE_MEDIUM_MODE` `AX88772_MEDIUM_DEFAULT` (0x0306);
  `WRITE_RX_CTL` again; `READ_RX_CTL` and `READ_MEDIUM_STATUS` for the
  log.
- **PHY start**: first the PHY id (registers 2 and 3), polled up to 100
  times 1 ms apart while it reads 0 or FFFF (`asix_get_phyid`); a PHY
  that never answers fails Start. Then the soft reset (`asix_soft_reset`
  in `ax88796b.c`: "Asix PHY won't reset unless reset bit toggles"; IEEE
  802.3 clause 22): BMCR 0, then BMCR reset (0x8000), polled until clear
  (500 ms at most; a PHY that never clears fails Start); ADVERTISE
  0x05E1 (10/100 half and full, 802.3) with symmetric and asymmetric
  pause (0x0C00, as `ax88772_phylink_setup` offers `MAC_SYM_PAUSE |
  MAC_ASYM_PAUSE`); BMCR autonegotiation enable and restart (0x1200).
- **Link** (`ax88772_mac_link_up`, `ax88772_mac_link_down`; the
  resolution is clause 22's and 28B's): BMSR read twice (the link bit
  latches low); up when bit 2 is set and autonegotiation is complete
  (bit 5). Speed and duplex from ADVERTISE & LPA, highest first
  (100 full, 100 half, 10 full, 10 half), or BMCR's speed and duplex
  bits when no common ability is read (as `asix_ax88772a_read_status`
  falls back). Pause by IEEE 802.3 Table 28B-3 with both pause bits
  advertised: in full duplex, a partner advertising PAUSE gives transmit
  and receive pause, one advertising ASM_DIR only receive pause. Medium
  on link up: `AC|RE`, plus `FD`, `PS` at 100 Mb/s, `TFC` with transmit
  pause and `RFC` with receive pause (`ax88772_mac_link_up`). On a link
  that goes down the medium is written 0, and with the AX88772A/B PHY
  (id `003b1861`) the PHY is reset and autonegotiation started again, as
  Linux does for that PHY (`asix_ax88772a_link_change_notify`: some
  partners otherwise leave a stale LPA); the AX88772C's PHY (`003b1881`)
  has no such rule. Linux polls the PHY (phylink); the core's Poll reads
  BMSR each tick. The chip records the link as up only once Link has
  applied all of it, so a Link that fails part way is seen by the next
  Poll and run again. The same holds for the link-down work (the medium
  and the PHY reset) and for a filter write that failed: Poll reports a
  change until they are done, and the core applies the filter after
  Link while the link is up. A Link that has not finished stays pending
  in the chip state, whatever the PHY reports, so it is always run again.
  An address write that failed is kept and written again by the next
  Link, and an interrupted link-down step (the PHY reset leaves BMCR 0
  part way) is finished by the next Link before the link is read, even
  if the carrier is back by then. BMSR's link bit latches low, so a drop that is already
  over by the next read still shows once: when Poll or Link sees it
  after a link that was up, the link-down work (and the AX88772A/B PHY's
  reset) is done before the link is applied again.
- **Status** (interrupt IN): Linux's AX88772 path uses no status
  report. *Inferred*: the AX88772 family sends the 8-byte report of
  `asix_status` (`struct ax88172_int_data`, byte 2 bit 0 the link), so
  Status returns that bit, and a change only wakes the worker, whose
  Link reads the PHY. A report that never comes costs nothing: Poll
  finds the change.
- **Receive filter** (`asix_set_multicast`): `AX_DEFAULT_RX_CTL`, plus
  `PRO` (promiscuous), or `AMALL` (all multicast, or more than 64
  addresses), or `AM` with the 64-bit hash (`ether_crc` >> 26) written
  by `WRITE_MULTI_FILTER`.
- **Stop**: medium 0 (as link down), `WRITE_RX_CTL` 0, and the PHY
  powered down (BMCR bit 11, `genphy_suspend`): the state Linux leaves
  it in before `phylink_start` (`ax88772_init_phy` calls
  `phy_suspend`).
  **Suspend**: medium `RE` cleared (`ax88772_suspend`). **Resume**: the
  whole bring-up again, up to three times, as `ax88772_resume` tries
  the reset three times (the core does not retry a failed resume).
- **Transmit** (`asix_tx_fixup`): one frame per bulk-OUT transfer, after
  a 4-byte header: the length in bits 0-15 and its complement in bits
  16-31. When header and frame end on a packet boundary
  (`(len + 4) % wMaxPacketSize == 0`), the 4 bytes `00 00 FF FF`
  (0xffff0000) follow instead of a zero-length packet.
- **Receive** (`asix_rx_fixup_internal`, `struct asix_rx_fixup_info`):
  a stream of records, each a 4-byte header (length in bits 0-10, its
  complement in bits 16-26) and the frame, padded to an even length.
  One bulk-IN transfer holds several records, and **a record may span
  two transfers**: Linux carries the partial frame, and a header split
  2 + 2 bytes, into the next transfer. Its checks, kept here: the two
  length fields agree; the length is at most 1518 (1500 + 14 + 4); the
  transfer is consumed exactly, each record's pad included, so a
  transfer that ends on an odd byte is malformed. When a frame is being
  carried and the new transfer holds the rest, its pad and a whole
  header after it (`((remaining + 1) & ~1) + 4` bytes; Linux tests
  `remaining + 4`, which can read past the end), that header is checked
  first; if it does not agree, the carried frame is dropped and parsing
  starts at the transfer's first byte (Linux's "synchronisation was
  lost"). A half header carried from the last transfer is completed
  before a trailing two bytes are taken as a new half header (Linux
  tests the other way round and would lose it). Any other bad header
  makes the transfer malformed. A record of 1 to 13 bytes, or of 1515 to
  1518 (a VLAN tag), is consumed and counted as dropped, across
  transfers too, keeping the stream in step: the core's frames are 14
  to 1514 bytes (section 4.6). A record of length 0 consumes its header
  only. The FCS is
  never forwarded: Linux strips none and `AX_DEFAULT_RX_CTL` asks for
  none.
- **Receive buffers.** The burst `QCTCTRL` sets is 2 KiB, and Linux
  reads 2048-byte transfers (`ax88772_bind`, `rx_urb_size`). This core
  does the same: a transfer the size of the burst ends with the burst,
  so it never waits for data that is not coming. A larger transfer
  would complete only on a short packet, and a burst that ends on a
  packet boundary with no zero-length packet (not documented either
  way) would hold its frames back until more traffic came.
- **Carrying a frame across transfers in the core.** netchip.h's rule
  that RxParse validates a whole transfer before emitting still holds:
  RxParse first walks the transfer on a copy of the stream's position
  (the counters, not the frame bytes), and only if the whole transfer
  is well formed walks it again and emits. A malformed transfer clears
  the live position, so a carried frame is dropped with it, and emits
  nothing. Six changes to the shared core. Items 1 and 3 change nothing
  for a package that does not use them; items 4 to 6 fix the core for
  every package, and on 2026-11-06 (owner) every copy of the core,
  `ax88179`'s, `rtl815x`'s and `sr9700`'s too, took them:
  1. The carry state (the partial frame, up to 1518 bytes, its length
     and the half header) lives in the chip state, which this package
     enlarges: netchip.h's `NET_CHIP_STATE` becomes overridable
     (`#ifndef`, default 256), and this package's build sets 2048.
  2. Transfers must be parsed one at a time and in order. The core
     parses each bulk-IN transfer in its completion routine, and with
     several outstanding, two completions could run at once on a
     multiprocessor system. The chip asks for one bulk-IN transfer at a
     time (`RxCount` 1): its completion parses it before the pump
     submits the next one, so parsing is serial and in stream order on
     any processor count. The cost is a gap between transfers, which
     the chip's own receive buffer absorbs; throughput is a bench
     reading (section 6.7). Parsing several outstanding transfers in
     order would need an ordered queue in the core, not only a lock.
  3. The stream must be restarted where data was lost. A bulk-IN
     transfer whose data never reaches RxParse (it failed, was
     cancelled by a quiesce, or completed with the data path closed)
     breaks it, and so does one the core drops for a length beyond its
     buffer, so the chip table gains an optional `RxReset`, which
     netusb.c's completion calls for such a transfer while the slot is
     still busy (so never beside RxParse). It drops the carried frame;
     the next transfer is read from its first byte as a header. A
     transfer that then starts inside a record fails the header checks
     and is dropped whole. Under steady traffic every later transfer
     can start inside a record too, so the chip restarts the stream:
     after 4 malformed transfers in a row its Poll reports a change,
     the core quiesces the data path and runs Link (the core runs Link
     only with the data path closed), and Link turns receive off and
     on (`WRITE_RX_CTL` 0, then the mode) and reads the stream from its
     next byte. *Inferred*: that turning receive off empties the chip's
     receive buffer, so the next transfer starts on a record; no Linux
     file says so (Linux restarts nothing and waits for a transfer that
     starts on a record).
     The other packages' tables leave `RxReset` NULL.
  4. A bring-up must not run beside the data path, since it resets the
     chip state the stream lives in. netusb.c's resume runs the
     bring-up only on a D0 that follows a D3 (a D0 while in D0 is
     ignored), after draining the data path; it tries the chip's Resume
     three times (the AX88772B's and SR9700's retry inside too); and when
     every try fails it leaves the data path closed and reports the
     device gone, as recovery does when it gives up, rather than
     reopening traffic on a half-reset chip. Only the chip's bring-up failing counts as that: a failure to
     write the address or the filter after it is logged, and a filter
     the chip keeps pending is applied again later.
  5. The pump stops after a failed IRP allocation instead of picking
     the slot it just freed again at once (a loop at DISPATCH_LEVEL
     while allocations keep failing); the worker's next tick and the
     next completion pump again.
  6. On 98 SE and ME the worker thread cannot be joined (a wait on a
     thread object faults there; section 4.7.1), so halt waits for its
     exit event only, after which the image may be unloaded. So no
     instruction of the image runs after that event is set
     (`ExitByKernel`, x86 only): the worker enters `KeSetEvent` with a
     stack frame whose return address is `PsTerminateSystemThread`,
     followed by that call's own return address and status, so the
     stdcall `KeSetEvent` returns straight into
     `PsTerminateSystemThread`. Both addresses are read from the import
     table before the signal. Because `KeSetEvent` may still touch the
     event after halt has woken, the event is a nonpaged block of its
     own that is never freed: one 16-byte `KEVENT` and its pool header
     per worker started on those systems (each start, not each resume),
     kept until restart, about 24 KB per 1000 plugs. Nothing on 98 SE
     or ME proves that `KeSetEvent` has returned, so nothing can free
     it soundly. Assumed, not documented for NTKERN: that the two imports
     resolve to its resident code, that `KeSetEvent` takes three
     stack words, and that `PsTerminateSystemThread` neither returns nor
     uses its caller's frame. On NT halt waits for the event and the
     thread object, with the plain calls. A residual stays where the
     thread object could not be referenced on NT, as in xHCI98's own
     threads.
- **Not done**: checksum offload (none in v1), wake-on-LAN (the monitor
  mode is left as reset leaves it), EEPROM writes, LED settings,
  jumbo frames.

**INF.** `AX772F98.INF`, one `$CHICAGO$` INF for 98 SE and ME (section
7.1), listing the four ids above (and the SR9800's, section 6.9.1).
`AX772F.INF`, the NT INF, lists only the SR9800: on 2000 and later
ASIX's driver serves the AX88772B and AX88772C. Folder
`free/net/ax88772/w98se-me/` and the folders of section 6.9.1.

**Tests.** Host: `chiptest.c` drives the chip core against a model of
the request set (bring-up order, MII host-enable, link resolution,
filter, the transmit pad, and the receive stream: records split at every
byte offset across two and three transfers, split headers, lost
synchronisation, bad and oversized lengths, VLAN-size skips, odd
lengths); `coretest.c` runs the unchanged core with the larger chip
state. The simulator `axemu772.py` (usb-redir, as `axemu.py`) models the
request set and the stream with bursts of the `QCTCTRL` size that split
records across transfers. The guest legs, 98 SE and ME, run against it
under xHCI98 and, on 98 SE, NUSB's EHCI. Because the simulator and the
driver are written from the same Linux facts, a misreading shared by
both passes; a real AX88772B and AX88772C on the bench stay owed.

### 6.9 CoreChips SR9800 and SR9700

Owner, 2026-11-06 (xHCI98 roadmap 47.27): the CoreChips SR9700 and
SR9800, which no driver serves on any of this project's systems.

**Whose driver covers what** (searched 2026-11-06; ethernet-coverage.md
section 3.3): none on any system. CoreChips' site
(`http://www.corechip-sz.com/`, reachable on 2026-11-06, unlike the
2026-10-27 survey) lists the SR9700 among its products with no driver,
does not list the SR9800, and offers downloads, behind a login, for its
SR9900 family and hubs only. No OEM was found that publishes a driver
for either chip on its own site; copies of drivers said to be for them
are on third-party driver sites only, which this project does not take
drivers from. No Windows has an in-box driver for `0FE6:9700`, `9702`
or `9800` (section 2 of ethernet-coverage.md). So the free drivers aim
at every system of this project: 98 SE to 7, x86 and x64.

**Sources.** Linux `drivers/net/usb/sr9800.c`, `sr9800.h`, `sr9700.c`
and `sr9700.h` at v7.3. `sr9800.c`, `sr9800.h` and `sr9700.c` carry no
SPDX line and say "version 2" of the GPL; `sr9700.h` is
GPL-2.0-only. Facts only are taken from them (AGENTS.md, "Sources"):
ids, request codes, register addresses and values, sequences and frame
formats, restated here and cited by symbol; no code, names, comments or
structure. Facts no Linux file gives are marked *inferred*.

#### 6.9.1 SR9800 (in `ax88772`)

Linux drives the SR9800 with the AX88772's command set and framing
(`sr9800.c` says it is based on `asix_common.c`), so it joins the
`ax88772` package, `AX772F.SYS`, as a second chip kind chosen at Start
by the device's id. Its differences from the AX88772B (6.8):

- **Id**: `0FE6:9800` (`products[]`).
- **MAC**: `READ_NODE_ID` (0x13, 6 bytes) at bring-up (`sr9800_bind`);
  written back with `WRITE_NODE_ID` (0x14) at the end of the reset.
- **MII** (`sr_mdio_read`, `sr_mdio_write`): `SET_SW_MII` (06), the read
  (07) or write (08), `SET_HW_MII` (0A); no host-enable poll.
- **LEDs** (`sr9800_bind`): request 0x70 (`SR_CMD_LED_MUX`), wValue
  0x4080, wIndex 0x4001 (link/activity on LED 0 and 1).
- **PHY address** (`sr_get_phy_addr`): byte 1 of request 0x19;
  embedded when 0x10.
- **Bring-up**, Linux's `bind` then `reset` order: `SW_PHY_SELECT`
  (0x22) wValue embedded; PHY power-up (`sr9800_phy_powerup`):
  `SW_RESET` (0x20) 0x60 (IPPD|IPRL), 20 ms; 0x20 (IPRL), 600 ms; 0, 20
  ms; 0x20. `WRITE_RX_CTL` (0x10) 0. The PHY id (registers 2, 3). The
  default mode (`sr9800_set_default_mode`, below). Bulk-IN size
  (`SR_CMD_BULKIN_SIZE` 0x2A, `SR9800_BULKIN_SIZE`): at High Speed
  wValue 0x8100 wIndex 0x8147 (4 KiB), at Full Speed 0x8000 / 0x8001
  (2 KiB). Then `sr9800_reset`: `WRITE_GPIOS` (0x1F) 0xB0
  (RSE|GPO_2|GPO2EN), 5 ms; the PHY address again; `SW_PHY_SELECT`;
  `SW_RESET` 0x48 (IPPD|PRL), 150 ms; 0, 150 ms; 0x20 with the
  embedded PHY (0x04, PRTE, with an external one), 150 ms;
  `WRITE_RX_CTL` 0; `SW_RESET` 0x08 (PRL), 150 ms; 0x28 (IPRL|PRL),
  150 ms; the default mode again; `WRITE_NODE_ID`.
- **PHY id** (`sr_get_phyid`): register 2 read up to 100 times, 1 ms
  apart, while it reads 0 or FFFF; then register 3. Read after the PHY
  power-up, for the log.
- **Default mode** (`sr9800_set_default_mode`): BMCR reset (0x8000);
  ADVERTISE 0x01E1 (10/100 half and full, 802.3; no pause bits);
  autonegotiation restart: Linux's `mii_nway_restart` reads BMCR and sets
  the restart bit if autonegotiation is on; this core writes BMCR 0x1200
  (enable and restart), its own choice, since the reset leaves
  autonegotiation on;
  `WRITE_MEDIUM_MODE` (0x1B) 0x0336 (`SR9800_MEDIUM_DEFAULT`: FD, RFC,
  TFC, PS, AC, RE); `WRITE_IPG012` (0x12) wValue 0x001D wIndex 0x0012;
  `WRITE_RX_CTL` 0x0188 (`SR_DEFAULT_RX_CTL`: SO, AB, RH1M); RX_CTL and
  the medium read back for the log.
- **Link** (`sr9800_link_reset`): the medium is the default with PS
  cleared below 100 Mb/s and FD cleared at half duplex; RFC and TFC
  stay set, as Linux leaves them. The speed and duplex come from the
  PHY as in 6.8. The status report is Linux's (`sr_status`,
  `struct sr9800_int_data`): 8 bytes, byte 2 bit 0 the link.
- **Filter** (`sr_set_multicast`): `SR_DEFAULT_RX_CTL` plus PRO, AMALL or
  AM with the 64-bit hash (request 0x16), as 6.8.
- **Receive** (`sr_rx_fixup`): the AX88772's records (4-byte header,
  length and complement in 11 bits, padded to even). Linux reads
  transfers of the burst size (4 KiB, 2 KiB at Full Speed) and refuses a
  record that does not end inside its transfer. The stream parser of 6.8
  is used unchanged, with the transfer the burst size: it takes what
  Linux takes, and also a record that spans, which Linux would drop
  (this core's choice: the stream parser is shared). One transfer
  outstanding, as 6.8.
- **Transmit** (`sr_tx_fixup`): the 4-byte header as 6.8. Linux
  appends the pad word 0xffff0000 when `(len + 4) % (maxpacket - 1)` is
  0 (len the frame), and the driver sets `FLAG_MULTI_PACKET`
  (`DRIVER_FLAG`), so usbnet adds neither a pad byte nor a zero-length
  packet otherwise: a transfer may end on a packet boundary
  unterminated. This core does exactly what Linux does, the rule that
  is known to work with the chip; whether `% maxpacket` was meant is
  left to the bench.
- **Stop**: medium 0, `WRITE_RX_CTL` 0, BMCR power-down (as 6.8;
  Linux's SR9800 driver has no stop: this core's choice). **Suspend,
  Resume** as 6.8, also this core's choice (Linux's SR9800 driver has
  no suspend or resume of its own).
- **Systems**: 98 SE to 7, x86 and x64. The 98 SE/ME INF
  (`AX772F98.INF`) gains `0FE6:9800`; a new NT INF, `AX772F.INF`, lists
  only `0FE6:9800` (2000 to 7: ASIX's drivers serve the AX88772B/C
  there), with the x86 NDIS 5.0 binary for 2000 to 7 x86 and an x64
  NDIS 5.1 build (`build.cmd ndis51`, as `rtl815x`'s) for XP x64 to 7
  x64. Folders `free/net/ax88772/w98se-me/`, `w2k/` (2000), `wxp/`,
  `wv-7-x86/`, `wxp-x64/`, `wv-7-x64/`, as section 7.4 names them for
  `rtl815x`.

#### 6.9.2 SR9700 (`sr9700`)

The SR9700 is a different chip (`sr9700.c`: based on `dm9601.c`): a
register file reached by three requests, no MII, 10 Mb/s half duplex
only, and its own framing. A new package, `src/net/sr9700/`,
`SR9700F.SYS`, on the shared core (unchanged: no frame spans
transfers, so neither the larger chip state nor `RxReset` is needed).

- **Ids** (`products[]`): `0FE6:9700`; `0FE6:9702` interface 1 (the
  same chip with a CD-ROM on interface 0). On 2000 and later Windows'
  composite parent accepts the function's SELECT_CONFIGURATION, so the
  NT INF matches `USB\VID_0FE6&PID_9702&MI_01` and the probe takes the
  vendor interface, whichever its number. On 98 SE and ME whether the
  composite parent splits it is not shown, so `SR970098.INF` lists only
  `0FE6:9700`. *Not run*: no composite model of the `9702` is in the
  simulator.
- **Requests** (`sr9700.h`): vendor, device recipient. Read registers
  0x00 (wValue 0, wIndex the first register, length the count); write
  registers 0x01 (wValue 0, wIndex the first register, the data);
  write one register 0x03 (wValue the byte, wIndex the register, no
  data).
- **Registers** (`sr9700.h`): NCR 0x00 (bit 0 reset), NSR 0x01 (bit 6
  link), RCR 0x05 (bit 0 receive on, 1 promiscuous, 3 all multicast, 4
  discard CRC errors, 5 discard long frames), PAR 0x10-0x15 (the MAC),
  PRR 0x1F (bit 0 PHY reset).
- **Bring-up** (`sr9700_bind`): NCR = 1 (reset), 20 us (here 1 ms, the
  smallest sleep); the MAC from PAR (6 bytes; loaded from the EEPROM at
  power-on, or a default the chip holds); PRR = 1, 20 ms; PRR = 0, 2
  ms; RCR = 0x31 (receive on, discard CRC errors and long frames).
- **Link**: NSR bit 6; always 10 Mb/s half duplex
  (`sr9700_get_link_ksettings`). The status report (`sr9700_status`):
  8 bytes, byte 0 is NSR, bit 6 the link.
- **Filter** (`sr9700_set_multicast`): RCR 0x31, plus 0x02
  (promiscuous), or 0x08 (all multicast) when any multicast is wanted:
  the chip has no multicast filter.
- **MAC change** (`sr9700_set_mac_address`): PAR written with request
  0x01.
- **Receive** (`sr9700_rx_fixup`): one transfer holds several packets,
  each 3 bytes (a status byte that must be 0x40, `RSR_MF`, then the
  length, little-endian, the FCS included), the frame and its 4-byte
  FCS, with no padding; Linux reads 3 KiB transfers (`rx_urb_size`
  3072), and Linux refuses a packet that spans two (its parser's
  contract; whether the chip can split one is not shown).
  The FCS is dropped (the chip checks it: RCR bit 4). Here the parser
  checks the whole transfer first (netchip.h), and a status byte other
  than 0x40, a length below 4 or a packet past the end makes the
  transfer malformed. Four transfers outstanding.
- **Transmit** (`sr9700_tx_fixup`, usbnet's padding): one frame per
  transfer after a 2-byte header holding its length. When header and
  frame end on a packet boundary the header says one byte more and a
  zero byte follows (usbnet adds the byte for a driver without
  `FLAG_SEND_ZLP` or `FLAG_MULTI_PACKET`; the header is raised to
  match).
- **Stop**: RCR 0 (receive off). **Suspend**: RCR 0. **Resume**: the
  bring-up again. All three are this core's choice: Linux's SR9700
  driver has none of them.
- **Systems**: 98 SE to 7, x86 and x64, as 6.9.1: `SR970098.INF`
  (98 SE, ME) and `SR9700F.INF` (2000 to 7 x86 and x64), the same
  folder names under `free/net/sr9700/`. (`SR970098.INF`: 8.3.)

**Tests.** Host: each chip core against a model of its requests and
framing. Simulators: `ax7emu.py --chip sr9800` and a new `sremu.py`
for the SR9700, written from the same Linux facts (so, again, a shared
misreading passes, and the real chips stay owed to the bench). Guest
legs against them under xHCI98 and, on 98 SE, NUSB's EHCI.

## 7. INF and install per system

### 7.1 Windows 98 SE and ME (section of one `$Chicago$` INF)

From the 2000 DDK network guide, "Version Section for Network INF Files"
(`501install_3g87.htm`): `Class=Net`,
`ClassGUID={4D36E972-E325-11CE-BFC1-08002BE10318}`, `Signature="$Chicago$"`
with `Compatible=1` for an INF used on both 9x and 2000. The 9x model
section (the undecorated one) carries, modelled on the network guide's INF
pages and the RNDIS kit's sections as `linux.inf` records them, written
fresh:

- `AddReg`: `DevLoader=*ndis`, `DeviceVxDs=cdcnet.sys`,
  `EnumPropPages=netdi.dll,EnumPropPages`, `NDIS\LogDriverName`,
  `NDIS\MajorNdisVersion=5`, `NDIS\MinorNdisVersion=0`,
  `Ndi\DeviceID`, `Ndi\Interfaces\DefUpper`/`UpperRange`
  (`ndis3,ndis4,ndis5`), `DefLower`/`LowerRange` (`ethernet`),
  `Ndi\Install\ndis3`(`4`,`5`) naming the install section,
  `Ndi\params\NetworkAddress` (an optional edit field).
- The install section copies the `.sys` to DIRID 11 (`SYSTEM`) through
  `DestinationDirs`, as the RNDIS kit's 98 section does.
- `MaxInstance` and `ExcludeFromSelect` as the RNDIS kit uses them.
- ME: an `.ME`-decorated section if anything differs (`MillenniumPreferred`
  in the RNDIS kit's `[Version]`); none is expected.

### 7.2 Windows 2000, XP, XP x64

`.NT` (and `.NTamd64`) sections: `Characteristics=0x84` (physical, has UI),
`BusType=15` (PNPBus), `AddReg` with `Ndi\Service`,
`Ndi\Interfaces\UpperRange=ndis5`, `LowerRange=ethernet`; `.Services` with
`AddService`, `ServiceType=1`, `StartType=3`, `LoadOrderGroup=NDIS`,
`ServiceBinary=%12%\cdcnet.sys`; an event log entry. Same structure as the
network guide's NT INF pages and the RNDIS kit's NT sections.

### 7.3 Windows Vista and 7

The same NDIS 5 binaries, through `.NTx86` / `.NTamd64` sections of the NT
INF; whether Vista and 7 want `*IfType=6`, `*MediaType=0` and
`*PhysicalMediaType=14` (as Windows 7's `rndiscmp.inf` sets them) for an
NDIS 5 miniport is read from the WDK 7.1 documentation and tested on 7 x64
in M5; on Vista and 7 x86 they stay unverified. A test signature is
embedded in the x64 driver, following
[build.md](build.md) section 7, step 4. The INF-only RNDIS binding has a separate installation-policy
experiment in section 7.5; an embedded signature on `CDCNET.SYS` does not
sign `CDCRNDIS.INF`.

### 7.4 Folders under `free/`

Folder names follow the one scheme of `README.md`, "Layout" (Owner,
2026-10-20), per package:

| Folder | Contents |
|---|---|
| `free/net/cdcnet/w98se-me/` | x86 NDIS 5 `CDCNET.SYS`, `CDCNET.INF` |
| `free/net/cdcnet/w2k-xp/` | the same x86 NDIS 5 binary, the NT INF (RNDIS ids in its 2000 models only); `CDCRNDIS.INF` (XP models only) |
| `free/net/cdcnet/wv-7-x86/` | the same x86 NDIS 5 binary, the NT INF with the Vista/7 values; `CDCRNDIS.INF` |
| `free/net/cdcnet/wxp-x64/` | x64 NDIS 5.1 binary with its embedded test signature, the NT INF; `CDCRNDIS.INF` |
| `free/net/cdcnet/wv-7-x64/` | the same signed x64 NDIS 5.1 binary; the NT INF with the Vista/7 values; `CDCRNDIS.INF` |

`ax88179` targets 98 SE, ME and 2000 only (section 1 item 8), so it has
two folders, with no `CDCRNDIS.INF`. `rtl815x` has those two and, from
2026-10-27, four for XP to 7 (section 7.6):

| Folder | Contents |
|---|---|
| `free/net/rtl815x/w98se-me/`, `free/net/ax88179/w98se-me/` | x86 NDIS 5 `RTL815X.SYS` or `AX179F.SYS`, its INF |
| `free/net/rtl815x/w2k/`, `free/net/ax88179/w2k/` | the same x86 NDIS 5 binary, the NT INF |
| `free/net/rtl815x/wxp/`, `free/net/rtl815x/wv-7-x86/` | the same x86 NDIS 5 `RTL815X.SYS`, `RTLXP7.INF` |
| `free/net/rtl815x/wxp-x64/`, `free/net/rtl815x/wv-7-x64/` | x64 NDIS 5.1 `RTL815X.SYS` with its embedded test signature, `RTLXP7.INF` |
| `free/net/ax88772/w98se-me/`, `free/net/sr9700/w98se-me/` | x86 NDIS 5 `AX772F.SYS` or `SR9700F.SYS`, `AX772F98.INF` or `SR970098.INF` |
| `free/net/ax88772/w2k/`, `wxp/`, `wv-7-x86/`; the same under `free/net/sr9700/` | the same x86 binary, `AX772F.INF` or `SR9700F.INF` (section 6.9) |
| `free/net/ax88772/wxp-x64/`, `wv-7-x64/`; the same under `free/net/sr9700/` | x64 NDIS 5.1 build (`build.cmd ndis51`), the NT INF |

All names 8.3. `cdcnet`'s x64 build is one binary from XP x64 to 7 x64; it
sits in two folders because 8.3 has no room for one name covering XP x64
to 7 x64, and because the Vista/7 folder has its own INF policy.

### 7.5 RNDIS on XP to 7: `CDCRNDIS.INF`

Owner, 2026-10-20: on XP, Vista and 7, RNDIS devices are served by
Windows' own RNDIS driver (`rndismp.sys` with `usb8023.sys` on XP, section
2.1), bound by an INF of this package; `cdcnet`'s RNDIS code is not used
there, and the x64 build leaves it out.

- **What it matches**: the four RNDIS class triples of section 2.2
  (`02/02/FF`, `E0/01/03`, `EF/01/01`, `EF/04/01`) as compatible ids, for
  function PDOs. Whole devices that also offer ECM (QEMU's `usb-net`, Linux
  gadgets, `0525:A4A2`) are bound by `CDCNET.INF`'s gadget line (section
  5.1; `CDCTEST.INF` in the earlier legs), and `cdcnet` then selects their
  ECM configuration on XP to 7.
- **What it installs**: nothing of its own. It names the in-box driver's
  service and files and takes its install sections from the in-box RNDIS
  INF (`netrndis.inf` on XP, `rndiscmp.inf` on 7, Vista's as read) by
  `Include` and `Needs`. Which sections those INFs define is read from the
  files in M2 and M5; DDK sample INFs are not read.
- **Models**: decorated for XP and later only (`NTx86.5.1`, `NTamd64`), so
  2000 finds no model in it and `CDCNET.INF`'s 2000 models, which carry the
  RNDIS ids, bind `cdcnet` there. Whether 2000 ignores the decorated
  models as Microsoft Learn's "INF Manufacturer Section" describes is
  unverified; M1 on 2000 settles it.
- **The test**: Android tethering (an `E0/01/03` function in an IAD
  composite) on XP in M2 and on 7 x64 in M5. If Windows' own driver
  fails it on a system, `cdcnet`'s RNDIS code is built for that system
  instead, and this section says so. XP x64, Vista and 7 x86 are not
  tested; their `CDCRNDIS.INF` ships "Built, not tested".
- An INF-only binding owns no binary; how 7 x64 treats an uncatalogued
  INF over signed in-box files is read in M5, as for `usbcom`'s INF
  (`serial.md`). Vista x64 stays unverified.

### 7.6 `rtl815x` on XP to 7: `RTLXP7.INF`

Owner, 2026-10-27 (xHCI98 roadmap 44.70): `rtl815x` serves XP to 7 for
the chips Realtek's own XP to 7 drivers in `usb-wdm-nonfree` do not list,
and never for those they do. One INF, `RTLXP7.INF`, has one models
section per system, so each Windows sees exactly its set. The ids are
Linux `r8152.c`'s for these chips, with one exception: `0BDA:8056`, from
Realtek's 7 INF (owner, 2026-11-06; below). Linux names no rebrand's
chip. Owner, 2026-11-07: ids that Realtek's own INFs list (its 7 INF
7.61.20 in `usb-wdm-nonfree`, and its Windows 10 INF 10.68.20, read for
its ids and chip sections only) are added where their chip is one this
section serves and Realtek's XP to 7 INFs do not list them on that
system; the id only is taken, and none has been seen here.

| Models section | Systems | Ids |
|---|---|---|
| `[Models]` | 2000, which ignores decorations (`RTL815X.INF` serves it) | none |
| `NTx86.5.1`, `NTamd64.5.2` | XP, XP x64 | `0BDA:8053` (RTL8153D), `0BDA:8156` (RTL8156, 8156B), `0BDA:8056` (RTL8156B, from 2026-11-06), `0BDA:8157` (RTL8157), `0BDA:815A` (RTL8159); from 2026-11-07 `0BDA:8057`, `0BDA:A157` (RTL8157), `0BDA:805A`, `0BDA:A15A`, ASUS `0B05:1D91` (RTL8159), and the rebrands Realtek's 7 INF puts in its RTL8156 or 8156B sections: D-Link `2001:B302`, Elecom `056E:4017`, I-O DATA `065F:3200`, Telaum `0DF4:0025`, Satechi `310E:0062`, Gopod `2D01:0032`, Getac `2BAF:0012`, ASUS `0B05:18D1` |
| `NTx86.6.0`, `NTamd64.6.0` | Vista x86 and x64 | the same |
| `NTx86.6.1`, `NTamd64.6.1` | 7 x86 and x64 | `0BDA:8157`, `0BDA:815A`; from 2026-11-07 `0BDA:8057`, `0BDA:A157`, `0BDA:805A`, `0BDA:A15A`, `0B05:1D91` (Realtek's 7 INF lists the RTL8156 and 8156B rebrands itself) |
| `NTx86.6.2`, `NTamd64.6.2` | 8 and later | none: no NDIS 5 there (section 4.1) |

Read against `usb-wdm-nonfree`'s Realtek INFs (2026-10-27): its XP 5.23
and Vista 6.27 INFs list `0BDA:8050`, `8152` and `8153` (bare and by
revision) and rebrands, and no `8053`, `8156`, `8157` or `815A`. Its 7
7.61.20 INFs add `8053`, `8155`, `8156` and `8056`, and no `8157` or
`815A`. So no id here is matched by both packs on one system. The
RTL8152 and RTL8153 ids stay off XP to 7. The ids of 2026-11-07 were read
against the same INFs: none of them is in the XP or Vista INF, and the 7
INF lists the eight RTL8156 and 8156B rebrands (so 7 does not get them
here) and none of the RTL8157 and RTL8159 ids.

`RTL81598.INF` and `RTL815X.INF` (98 SE, ME, 2000) list every chip, so
from 2026-11-07 they also take the RTL8152B's `0BDA:8052`, TP-Link
`2357:0603` and `0604`, Lenovo `17EF:3052`, `3057`, `3065`, `30FA`,
`720A`, `8153`, `A3C1`, `309B`, `309C` and `309D`, and Belkin, LG,
Elecom, ASUS, I-O DATA, Telaum, Satechi, Gopod, Club 3D, IOI, Luxshare
and Getac rebrands (the INFs' comments list them). Not taken: D-Link
`2001:7E34` and `A710` (Realtek names them as RNDIS devices), Killer
`1A56:3100` (Realtek's own line is commented out), the RTL8156C, the
Dash parts `0BDA:8D57` and `8D5A`, Realtek's test chip and the RTL8150,
a different design. The chip core reads the chip from its id register
(`RtlChipId`) and checks no vendor or product id, so an id costs an INF
line only; an id carrying a chip it does not know is refused at start.

`0BDA:8053` is the RTL8153D's id here. Linux added it to its table in the
commit that added the RTL8153C and RTL8153D, and the simulator models the
RTL8153D on it. Linux does not say which of the two chips uses it. An
RTL8153C on `0BDA:8053` would also bind on XP and Vista. The chip core
serves it, and Realtek's XP and Vista INFs do not list that id.

The x86 folders take the NDIS 5.0 binary, the x64 folders the NDIS 5.1
build of section 4.2 (`build.cmd ndis51`, as `cdcnet`'s), with the
release's embedded test signature (section 7.3). In `STAGE.TXT` the XP
and XP x64 lines are STAGE on simulator legs (owner, 2026-11-06); the Vista
and 7 lines stay copy-only. The legs (XP to 7, x86 and x64, simulated chips
at High Speed) are in
[testing/2026-10-27-rtl815x-xp7.md](../testing/2026-10-27-rtl815x-xp7.md),
and those under xHCI98 on XP and XP x64 (2.5 Gb/s) in
[testing/2026-11-06-rtl8156.md](../testing/2026-11-06-rtl8156.md).

## 8. Build toolchain

Per AGENTS.md, everything lives in this repository's git-ignored `tools/`,
installed per host and documented by the build. The shared build/import
contract is [build.md](build.md); no build or test resolves a path into
another repository:

- Windows 2000 DDK reconstructed into `tools\ntddk` and MSVC 6.0 in
  `tools\MSVC600`, for the x86 NDIS 5 build. `ndis.lib` is in
  `ntddk\libfre\i386`.
- WDK 7.1 in `tools\WinDDK71` for the x64 NDIS 5.1 build (`WNET` amd64).
- Each package's `build.cmd` builds one explicit profile per invocation;
  the release runner invokes every required profile and runs its import
  gate (98 SE's set is the strictest). `NDIS.VXD` and `NTKERN.VXD` are
  VxDs with no PE export table, so 98 SE's and ME's sets come from the 98
  DDK documentation and headers and from guest load tests, as
  [build.md](build.md) section 5.1 says. ME's load test is not a release
  blocker (section 11.1).
- Host-side unit tests for the pure parsers (NTB16, RNDIS messages, rx_desc
  walks, AX88179 trailers), compiled by this repository's test scripts and
  fed with synthetic, captured and fuzzed buffers. Keep raw captures and
  expected outcomes with a note naming the device, descriptor/transfer
  source and date. No parser test needs a live driver or another checkout.

## 9. Adapted code and NOTICE.md rows

Specifications first: the CDC/ECM/NCM structures and request codes are
written from the USB-IF documents, the RNDIS messages from [MS-RNDIS]. Linux
code under GPL-2.0-or-later is adapted where it is the only reference
(chip register sequences) and consulted for behaviour; a Linux file under
GPL-2.0 only gives facts only (owner, 2026-10-25: the repository is
GPL-2.0-or-later). Expected `NOTICE.md` rows, each file keeping the
Linux copyright lines and SPDX tag:

| File (planned) | Adapted from | Licence |
|---|---|---|
| `src/net/cdcnet/rndis.c` | Linux `drivers/net/usb/rndis_host.c` (Copyright (C) 2005 David Brownell): init/query sequence, quirks | GPL-2.0-or-later |
| `src/net/cdcnet/ecm.c` | Linux `drivers/net/usb/cdc_ether.c` (Brownell; Ole Andre Vadla Ravnas): descriptor parsing quirks, device table | GPL-2.0-or-later |
| `src/net/cdcnet/ncm.c` | Not adapted: NTB sizing from the CDC NCM specification; Linux `drivers/net/usb/cdc_ncm.c` ("Dual BSD/GPL") consulted for quirks as facts | - |
| `src/net/common/*.c` (where it follows usbnet's pipeline logic) | Linux `drivers/net/usb/usbnet.c` (Brownell; David Hollis) | GPL-2.0-or-later |
| `src/net/rtl815x/*.c` | Not adapted: written from the chips' register facts (section 6.4; owner, 2026-10-25) | - |
| `src/net/ax88179/*.c` | Linux `drivers/net/usb/ax88179_178a.c` (Copyright (C) 2011-2013 ASIX) | GPL-2.0-or-later |
| `src/net/ax88772/ax7chip.c`, `ax7chip.h`, `ax7emu.py` | Linux `drivers/net/usb/asix_common.c`, `asix_devices.c`, `asix.h` at v7.3 (Copyright (C) 2003-2006 David Hollis, (C) 2005 Phil Chang, (C) 2006 James Painter, (c) 2002-2003 TiVo Inc.); the PHY reset rule from `drivers/net/phy/ax88796b.c` (GPL-2.0+) as a fact; the SR9800's values from `sr9800.c`/`sr9800.h` as facts only | GPL-2.0-or-later |
| `src/net/sr9700/*` | Not adapted: written from the facts of Linux `sr9700.c` and `sr9700.h` (section 6.9.2) | - |

The licence column is the source's. `rtl815x` was
rewritten from the register facts (section 6.4) and no longer follows
`r8152.c`'s code. No Realtek firmware blob is redistributed. Not used: Microsoft's
RNDIS kit binaries, ASIX's or Realtek's Windows drivers, and the vendor INFs
in the non-free repository beyond the facts quoted in section 2.6.

## 10. Test plan

Tested systems (Owner, 2026-10-20; `README.md`, "Tested by the author"):
98 SE, 2000 SP4, XP SP3 x86 and 7 x64, each where the package targets it.
For `cdcnet` that is all four; for `rtl815x` and `ax88179`, 98 SE and
2000. ME, XP x64, Vista x86/x64 and 7 x86 get release folders built from
the same source and stay "Built, not tested"; a reading only they could
settle does not block a release, and the release notes list it (section
11.1). Each tested system has an EHCI leg: NUSB's EHCI stack on 98 SE,
Microsoft's on 2000, XP and 7 x64.

### 10.1 QEMU

- `-device usb-net,netdev=n0,mac=...` on QEMU's UHCI (and the EHCI's
  companions) for Microsoft's and NUSB's stacks and on `qemu-xhci` for
  xHCI98. Full Speed only: `usb-net` has no High Speed descriptors, so it
  runs on a companion and never exercises EHCI. The EHCI leg is the
  hardware of section 10.2 (a High Speed adapter, or a Full Speed one
  behind a High Speed hub). Exercise: RNDIS (configuration
  value 2) and ECM (value 1, by a registry switch forcing the choice),
  link up, DHCP from QEMU's user network, ping, a TCP transfer to the host
  through `hostfwd`, multicast join, unplug and replug (`device_del`/
  `device_add` from the monitor), system standby and resume where the guest
  supports it, adapter disable/enable, driver update.
- Guests: 98 SE (in-box UHCI, NUSB's EHCI stack and xHCI98), 2000 SP4
  (Driver Verifier on), XP SP3 and 7 x64. NUSB's EHCI stack on 98 SE is
  fully supported (Owner, 2026-10-20) and in every 98 SE leg. The ME, XP
  x64, Vista and 7 x86 guests are not test legs; they serve readings and
  bug reports only.
- QEMU has no NCM, Realtek or ASIX device model; those need hardware or
  `usb-host` passthrough.

### 10.2 Hardware

Owner, 2026-10-20: one or two mainstream devices per class, and no
Raspberry Pi gadget rig.

- An RTL8153 USB-C or USB-A dongle (ECM mode for `cdcnet` on every
  tested system, vendor mode for `rtl815x` on 98 SE and 2000).
- An AX88179 dongle, for `ax88179` on 98 SE and 2000.
- An Android phone with USB tethering (RNDIS in an IAD composite): on XP
  and 7 x64 through `CDCRNDIS.INF` and Windows' own driver, and under
  xHCI98 on 98 SE and 2000 through `cdcnet`'s RNDIS code.
- CDC-NCM is tested on hardware only if one of these offers it; otherwise
  it rests on QEMU-free host tests of the NTB parser and on users' reports.
- Each on the owner's real machines (an xHCI machine with xHCI98; an older
  machine with UHCI/EHCI for Microsoft's and NUSB's stacks), at a root port
  and behind a hub, at High Speed and SuperSpeed. The EHCI leg of each
  tested system is a High Speed dongle on the EHCI machine; QEMU's Full
  Speed `usb-net` does not count as one (section 10.1).
- Throughput with a period-appropriate tool on 98 SE (a file copy over SMB
  timed, or an iperf 1.7 Win32 build) and iperf on the NT targets
  (2000 only, for `rtl815x` and `ax88179`).

The results go into `README.md`'s "Tested by the author" table.

### 10.3 Robustness

- Host-side fuzzing of `RxParse` for NCM, RNDIS, rx_desc and AX88179 trailer
  formats, and of RNDIS control responses: no read or write outside the
  buffer for any input.
- Unplug during a transfer under load, 50 cycles per tested system;
  unplug during initialization; resume with the adapter removed while
  asleep.
- Exact bulk-packet multiples and adjacent sends: for ECM on a 64-byte
  bulk pipe, use 64-, 128- and 1514-byte Ethernet frames. For RNDIS vary
  the complete wire message, including its 44-byte header and padding:
  an 84-byte Ethernet frame without extra padding gives a 128-byte RNDIS
  message. Repeat immediately below, at and above 512/1024-byte wire
  boundaries on hardware. Verify the termination action: a ZLP for ECM,
  with no following payload ahead of it; one trailing zero byte for
  RNDIS, outside `MessageLength`. On receive, accept an RNDIS transfer
  that ends in the one-byte terminator or in zero padding.
- A fake USB completion backend completes immediately, later, during
  cancel and after a generation change. A fake NDIS receiver returns
  packets immediately or holds them across quiesce. Every accepted send
  completes once; all reference counts return to zero before teardown.
- NCM malformed cases include cycles, zero divisors, missing terminators,
  offset wrap, wrong signatures, metadata overlaps and unsupported CRC.
  RNDIS cases include truncated headers, invalid byte-8-relative offsets,
  oversized message lengths and a status notification between request and
  response. Every case has an explicit accept/drop/fail result.

## 11. Milestones

| # | Content | Done when |
|---|---|---|
| M0 | Build/import feasibility, device descriptors, USB and NDIS lifetime experiments (table below) | Every blocking row has recorded evidence or a revised design; no release or hardware support claim is inferred from a host parser test |
| M1 | `src/net/common/` NDIS 5 shim and `cdcnet` RNDIS + ECM, x86 NDIS 5 build | QEMU `usb-net` passes DHCP, ping and a transfer on 98 SE (in-box UHCI, NUSB's stack through its companion, xHCI98) and on 2000 (Microsoft's UHCI, xHCI98); unplug/replug clean. The EHCI leg is M2's High Speed hardware |
| M2 | Same on XP (ME built, not tested); `CDCRNDIS.INF` on XP with Android tethering (verifying Windows' own driver); Android tethering under xHCI98 on 98 SE and 2000; the RTL8153 in ECM mode | Throughput recorded; RTL8153 dongle working on 98 SE, 2000 and XP, each with an EHCI leg (NUSB's on 98 SE) |
| M3 | CDC-NCM in `cdcnet`; first release `cdcnet-1.0.0` (`w98se-me`, `w2k-xp` with `CDCRNDIS.INF`; ME "Built, not tested") | NCM host tests, and an NCM-capable device if one is on the bench |
| M4 | `rtl815x` for RTL8152/8153/8153B vendor mode, on 98 SE, ME and 2000 (`w98se-me`, `w2k`; ME built, not tested) | On 98 SE and 2000: throughput at SuperSpeed beats M2's ECM figure on the same system; an EHCI leg works; sleep/resume works |
| M5 | Vista and 7 x86/x64 for `cdcnet`: the x86 NDIS 5.0 and x64 NDIS 5.1 binaries with the Vista/7 INF values, x64 signing; XP x64; `CDCRNDIS.INF` with Android tethering on 7 x64. `rtl815x` and `ax88179` have no XP to 7 leg (section 1 item 8) | `cdcnet` on 7 x64, with an EHCI leg; the XP x64, Vista x86/x64 and 7 x86 folders built, not tested |
| M6 | `ax88179`, on 98 SE, ME and 2000 (`w98se-me`, `w2k`) | As M4 |
| M7 | RTL8156 in `rtl815x`, on 98 SE, ME and 2000 | As M4 |

### 11.1 M0 evidence and exit conditions

Each record names the system/service pack, architecture, host stack,
device or emulator version, date, exact input and observed result. Failed
experiments remain in the record alongside the change they require.

| Question | Experiment or reading | Exit condition |
|---|---|---|
| Can the common x86 NDIS 5/WDM ABI load? | Local 2000 DDK headers/docs; compile and import-check a minimal miniport, then load on clean 98 SE and 2000 guests | All NDIS/kernel imports supported; initialize/halt repeatedly without resource leaks. XP and 7 x64 are later milestone legs; ME, XP x64, Vista and 7 x86 are built, not tested |
| Does the lower stack support the selected configuration and transfer sizes? | QEMU ECM configuration 1 and RNDIS configuration 2 on the initial 98 SE/2000 stacks; record SELECT_CONFIGURATION result and returned pipe limits | Both protocols have a working configuration and bounded buffer plan, or the affected stack's limitation is documented before M1 |
| How do stop and cancel return ownership? | Delay completions, abort with receive/interrupt URBs pending, race gate close with an admitted submitter, unplug during control work and initialization | No new submitter admitted after gate close; already admitted submissions drain before cancellation; each IRP/task releases its reference once; no context freed with outstanding owners |
| Is a separate OUT ZLP supported and ordered? | Exact-packet-size sends followed by a distinguishable frame on each initial host stack | Correct frame boundary and byte contents, with a viable termination policy; QEMU success alone does not settle real-controller behavior |
| Do the model sections bind only the intended function? | Enumerate hardware/compatible IDs and selected model on 2000 and XP; inspect dedicated-network and composite descriptor fixtures | On NT, `CDCNET.INF`'s RNDIS lines select `cdcnet` only on 2000, and XP selects `CDCRNDIS.INF`; no unrelated composite interface captured |
| Which NDIS 5 callbacks may pend or wait? | Target DDK reference pages for query/set, reset, power, halt and packet returns | Written callback/IRQL/completion table in source documentation; one implementation path for each asynchronous owner |
| Which RNDIS idle deadline applies? | Published USB transport mapping and QEMU behavior with idle traffic | Timer/request-ID behavior specified; no hardcoded deadline inferred from a different bus mapping |

Real RTL8153 configuration descriptors and Android IAD grouping are
hardware gates in M2, not prerequisites fabricated from QEMU. NCM setup
and framing in M3 remain experimental until an NCM device is exercised;
the release notes name that limit if only parser tests have run. Only a
recorded author run may change a README cell to `Tested`.

Left unverified on the systems that are not tested (section 10), listed
in the release notes of each folder that carries them, and blocking no
release:

- ME: its NDIS version (section 4.1), one binary across 98 SE and ME
  (section 4.4), and every run of `cdcnet`, `rtl815x` and `ax88179`.
- XP x64: the x64 binary on native NDIS 5.1, without the translation
  layer, and `CDCRNDIS.INF`.
- Vista x86/x64 and 7 x86: the Vista/7 INF values (section 7.3), Vista's
  in-box RNDIS INF sections and `CDCRNDIS.INF` with tethering (section
  7.5), Vista x64's policy for an uncatalogued INF, and the x86 binary
  through the translation layer.

## 12. Owner decisions, risks and open questions

### 12.1 Owner decisions

Owner, 2026-10-20, on the questions this draft raised:

- **Core location**: `src/net/common/`, the `src/<class>/common/` pattern
  `README.md` now lists.
- **Package split**: three packages, `cdcnet`, `rtl815x`, `ax88179`, on the
  shared core.
- **`0BDA:8153`**: `cdcnet` until `rtl815x` ships, then only `rtl815x`
  (on 98 SE, ME and 2000, its systems; section 6.5). Owner, 2026-10-20:
  on XP to 7, `cdcnet` keeps the RTL8153 ids as a free fallback, and
  never displaces Realtek's driver where that is already installed
  (section 6.5).
- **Vista and 7**: NDIS 5 only; the x86 NDIS 5.0 binary and the x64 NDIS
  5.1 binary serve them (`cdcnet` only, after the reading below). No NDIS
  6 code.
- **Windows 8**: not a target.
- **Phones under 98 SE/ME/2000's Microsoft stacks**: not planned; Android
  tethering only under xHCI98 and on XP and later.
- **AX88772 and the cheap clones**: neither planned; ASIX's own AX88772
  driver lives only in the separate non-free pack.
- **Hardware**: one or two mainstream devices per class, no Pi gadget rig
  (section 10.2).
- **Folder split**: the one scheme of `README.md`, "Layout" (section 7.4).
- **Driver file names**: `CDCNET.SYS`, `RTL815X.SYS`, `AX179F.SYS`, as
  section 3.2 gives them.

After the survey of existing drivers (`existing.md` section 4):

- **RNDIS on XP to 7**: an INF, `CDCRNDIS.INF`, binding Windows' own RNDIS
  driver by class id, with Android tethering verified (on XP and 7 x64,
  the tested systems; section 10); `cdcnet`'s own
  RNDIS code targets 98 SE, ME and 2000 only (sections 1 item 7 and 7.5).
- **`rtl815x` and `ax88179` on XP to 7**: Realtek's and ASIX's own XP to
  7 drivers were read first (INFs, chip revisions).

After that reading:

- **`rtl815x` and `ax88179`**: 98 SE, ME and 2000 only. Realtek's XP 5.23,
  Vista 6.27 and 7 7.61.20 cover `0BDA:8152`, `8050` and `8153` at every
  revision (the 7 driver adds `8053`, `8155`, `8156`, `8056`); ASIX's
  AX88179 drivers (XP/Vista 1.4.2.0 and 1.6.2.0, 7 1.12.11.0 and
  1.14.11.0) cover the AX88179 and AX88178A but not the AX88179A/B, which
  `cdcnet`'s NCM serves on every target (section 1 item 8).

Owner, 2026-10-27 (xHCI98 roadmap 44.70): `rtl815x` adds the RTL8153D,
8156, 8156B, 8157 and 8159. On XP to 7 it serves only the chips
`usb-wdm-nonfree`'s Realtek drivers leave out: the RTL8153D, 8156 and
8156B on XP and Vista, the RTL8157 and 8159 on XP, Vista and 7, through
`RTLXP7.INF` (section 7.6), with an x64 NDIS 5.1 build.

Also: NUSB's EHCI stack on 98 SE is fully supported; signing follows
xHCI98's and Stor98's approach.

After the first `ax88179` and `rtl815x` builds
([testing/2026-10-22-ax88179-rtl815x.md](../testing/2026-10-22-ax88179-rtl815x.md)):

- Owner, 2026-10-22: `AX179F.INF` and `AX179F98.INF` match the ASIX
  reference id at revision 0100 only (`USB\VID_0B95&PID_1790&REV_0100`),
  because the AX88179A/B share `0B95:1790` and present CDC-NCM, which
  `cdcnet` serves; a revision-less whole-device match would take them
  from it.
- Owner, 2026-10-22: `RTL815X.INF` and `RTL81598.INF` keep the Realtek
  OEM ids of Linux `r8152.c` that may carry an RTL8156 (2.5G) part; the
  driver refuses an unknown chip version cleanly at start, before any
  register write. Only `0BDA:8155/8156/8157/815A` are left out.
- Owner, 2026-10-22: the generated fallback address of section 4.8 is
  kept in the driver key with `NdisWriteConfiguration` (as
  `FallbackAddress`); that import, with `NdisReadNetworkAddress` and
  `NdisGetCurrentSystemTime`, is verified on 98 SE in the first guest
  load leg.

### 12.2 Technical risks and unverified points

- 98 SE `NDIS.VXD` behaviour for NDIS 5 WDM miniports beyond loading: how
  receive packets reach NDIS 3 protocols, which NDIS 5 functions it lacks
  (the 98 DDK says "a small number of features ... not available", without a
  list). Mitigation: M1 on 98 SE first; an import gate against 98 SE's
  `NDIS.VXD` and NTKERN exports.
- Why Microsoft built its RNDIS kit separately for 98, ME and 2000.
- Whether 98 SE's Microsoft USBD lets a function driver select a
  non-default configuration, and its transfer-size and zero-length-packet
  handling.
- How 98 SE/ME/2000's composite parent treats IAD devices.
- RTL8153 configuration layout per dongle (how many configurations, where
  ECM sits): read from real devices with xHCI98's `XHCITR` tree tool (its
  design record 25) or USBView.
- Whether the RTL8156 and AX88179A parts offer ECM/NCM configurations.
- RNDIS parsing is device-controlled input in kernel mode: every length
  checked, fuzzed (section 10.3).
- Realtek bring-up without firmware patches: possible link quirks on some
  revisions.
- 98 SE TCP/IP throughput ceiling, unknown until measured.
- `0BDA:8053` on XP and Vista (section 7.6): which chips use it, the
  RTL8153D only or the RTL8153C too, is not stated by Linux. A real
  adapter settles it.
- ME's NDIS version, and Android's current tethering protocols, unverified
  (sections 4.1, 2.4). ME is not tested, so the first stays unverified
  unless the ME DDK is read (section 11.1).

### 12.3 Open questions to the owner

- **`ax88179`'s EEE default** (section 4.10.2). ASIX's 7 INF offers
  `*EEE` on by default for the AX88179 and AX88178A (off only in the
  three Allied Telesis sections, on in `ATUMC2000T`); its XP and Vista INF does not offer it; Linux

  turns it off at every reset and `ax88179` does the same today. Options:
  (a) on, ASIX's 7 default, by the rule of 2026-11-09 (the maker's
  default); (b) off, as Linux and as `ax88179` has run on the bench.
  Owner, 2026-11-09: (a), as implemented; off if a real AX88179
  misbehaves with it. Read the same day
  ([testing/2026-11-09-ax88179-eee-real.md](../testing/2026-11-09-ax88179-eee-real.md)):
  the owner's real AX88179, passed through to 98 SE under xHCI98, linked
  at 1000 full against an EEE-capable 10GbE port with EEE on, carried 500
  pings of 1472 bytes and sustained TCP (host to guest) without loss or
  error; with EEE off the link and the TCP rate were the same. A guest
  that stopped answering after one hot replug with EEE on is open, its
  cause not known. Not read: whether EEE was negotiated (the
  driver reads no EEE status; low-power idle is not visible from the
  guest), the guest-to-host direction, and a switch in between.



## 13. Sources

- Windows 2000 DDK: `d_net.chm` network guide (chapters 2.x, 4.5, 6.1, 12;
  `NdisMGetDeviceProperty`; network INF sections 1.2.x), `inc\ddk\ndis.h`,
  `inc\usbdi.h`. DDK sample sources are not read.
- Windows 98 DDK: `HELP_WIN98DDK.CHM` (`legal_035f.htm`, `po-dev_7t9j.htm`),
  `INC_WIN98_NDIS.H`.
- WDK 7.1: `inc\ddk\ndis.h`.
- Microsoft Learn: "NDIS 6.0 Backward Compatibility" (archived),
  "NDIS 6.30 Backward Compatibility".
- Microsoft Learn: [RNDIS USB short packets](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/usb-short-packets),
  [NDIS miniports with a WDM lower edge](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/ndis-miniport-drivers-with-a-wdm-lower-edge),
  [NDIS 5 receive packet ownership](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553533(v=vs.85)),
  [RNDIS packet messages](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/remote-ndis-packet-msg),
  [RNDIS keepalive messages](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/remote-ndis-keepalive-msg).
- Linux at `f47d2236`: [`include/uapi/linux/usb/cdc.h`](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/include/uapi/linux/usb/cdc.h)
  and [`drivers/net/usb/cdc_ncm.c`](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/net/usb/cdc_ncm.c),
  checked for NCM parameter names, capability bits and setup behavior.
- Windows INFs: 98 SE `usbcdc.inf`; 7 `rndiscmp.inf`.
- Linux at `f47d2236`: `drivers/net/usb/{usbnet,cdc_ether,cdc_ncm,rndis_host,r8152,ax88179_178a,asix_devices,smsc95xx,lan78xx,dm9601,ipheth}.c`;
- Linux at tag v7.3 (section 6.8): `drivers/net/usb/asix_devices.c`,
  `asix_common.c`, `asix.h` (GPL-2.0-or-later), `sr9800.c` and
  `sr9800.h` (facts only), `drivers/net/phy/ax88796b.c` (GPL-2.0+);
  `Documentation/usb/linux.inf` at `v2.6.20`.
- QEMU `hw/usb/dev-network.c` (master, 2026-10-20); QEMU 11.1.0 on this host.
- xHCI98: `AGENTS.md` ("Architecture Overview", "DDK"),
  `docs\contributing\design\13-superspeed-hcd.md` (sections 6.3, 10.8, 10.9),
  `docs\contributing\design\19-x64-test-signing.md`,
  `docs\contributing\build-and-test.md` (line 1476),
  `docs\contributing\test-equipment.md` (row 10),
  `docs\usb-xhci-info\win98-wdm.md`, `docs\using\release-notes.md`,
  `src\xhci_xfer.c`.
- Specifications to fetch before coding: USB-IF CDC 1.2 with ECM 1.2 and
  NCM 1.0; Microsoft [MS-RNDIS].

# USB Ethernet drivers

Status: design draft, 2026-10-20. Implementation, 2026-10-22: `cdcnet`
0.1.0.0 exists in `src/net/cdcnet/` (ECM, NCM, RNDIS; NDIS 5.0 x86 and 5.1
x64 builds; `CDCNET.INF`, `CDC98.INF`, `CDCRNDIS.INF`), with the core kept
inside the package for now rather than in `src/net/common/` (section 3.2).
Its runs are in `docs/testing/2026-10-22-cdcnet.md`. `rtl815x` has no code;
`ax88179` is separate work.

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
   2.6.
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

## 2. Purpose and devices served: the survey

What decides each candidate: can it be bought today, is there a free and
mature reference (Linux, GPL-2.0-compatible) for it, how much protocol it
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
  (written by ASIX) are the only free references, and both are GPL-2.0
  compatible.

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
| x86 NDIS 5 | 5.0, `NDIS50_MINIPORT`, `NDIS_WDM=1`, `USE_KLOCKS=1` | Windows 2000 DDK with MSVC 6.0 | `cdcnet`: 98 SE, ME, 2000, XP, Vista and 7 x86. `rtl815x`, `ax88179`: 98 SE, ME and 2000 | The only NDIS version 98 SE has; Vista and 7 load it through the translation layer. The 2000 DDK network guide section 12.6 names the two compile flags |
| x64 NDIS 5.1 | 5.1 | WDK 7.1, `x64 WNET` environment | `cdcnet` only: XP x64, Vista x64, 7 x64 | XP x64 is NDIS 5.1; Vista and 7 x64 load it through the translation layer |

The translation layer's cost on Vista and 7 is accepted; throughput there is
measured on 7 x64 (section 6.7), not assumed.

Kernel-mode code signing is required on Vista and 7 x64, which only
`cdcnet` targets. Owner, 2026-10-20:
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
- **Offload**: none in v1. NDIS 5 task offload (`OID_TCP_TASK_OFFLOAD`) is a
  2000-and-later feature whose use by 98 SE's stack was not checked; the
  r8152 and AX88179 checksum offloads are a later, optional step on 2000
  only.

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
- Linux `cdc_ncm.c` is dual-licensed; this project takes it under GPL-2.0.

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

`rtl815x` and `ax88179` target 98 SE, ME and 2000 only (section 1 item
8), so each has two folders, with no `CDCRNDIS.INF`:

| Folder | Contents |
|---|---|
| `free/net/rtl815x/w98se-me/`, `free/net/ax88179/w98se-me/` | x86 NDIS 5 `RTL815X.SYS` or `AX179F.SYS`, its INF |
| `free/net/rtl815x/w2k/`, `free/net/ax88179/w2k/` | the same x86 NDIS 5 binary, the NT INF |

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
is adapted where it is the only reference (chip register sequences) and
consulted for behaviour. Expected `NOTICE.md` rows, each file keeping the
Linux copyright lines and SPDX tag:

| File (planned) | Adapted from | Licence |
|---|---|---|
| `src/net/cdcnet/rndis.c` | Linux `drivers/net/usb/rndis_host.c` (Copyright (C) 2005 David Brownell): init/query sequence, quirks | GPL-2.0-or-later |
| `src/net/cdcnet/ecm.c` | Linux `drivers/net/usb/cdc_ether.c` (Brownell; Ole Andre Vadla Ravnas): descriptor parsing quirks, device table | GPL-2.0-or-later |
| `src/net/cdcnet/ncm.c` | Linux `drivers/net/usb/cdc_ncm.c` (Copyright (C) ST-Ericsson 2010-2012): NTB sizing and quirks | GPL-2.0 (taken from "Dual BSD/GPL") |
| `src/net/common/*.c` (where it follows usbnet's pipeline logic) | Linux `drivers/net/usb/usbnet.c` (Brownell; David Hollis) | GPL-2.0-or-later |
| `src/net/rtl815x/*.c` | Not adapted: written from the chips' register facts (section 6.4; owner, 2026-10-25) | - |
| `src/net/ax88179/*.c` | Linux `drivers/net/usb/ax88179_178a.c` (Copyright (C) 2011-2013 ASIX) | GPL-2.0-or-later |

All are compatible with this repository's GPL-2.0. `rtl815x` was
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
- ME's NDIS version, and Android's current tethering protocols, unverified
  (sections 4.1, 2.4). ME is not tested, so the first stays unverified
  unless the ME DDK is read (section 11.1).

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

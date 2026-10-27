# USB Ethernet coverage: chips x Windows

Status: survey, 2026-10-27 (task 44.68 part 1, for xHCI98 3.3.0.0). No
code. Owner, 2026-10-27: "Check if have existing os supported, then put in
none free. Then make for those os not covered by nonefree." Owner, later
the same day: 44.68 writes no new free drivers (beyond `ax88179`, which
exists); the work is this survey and adding makers' drivers to
`usb-wdm-nonfree` where the owner accepts their terms (section 7). This document therefore lists, per chip and
Windows version, what already serves it, what the non-free pack could
carry, and the gaps that stay uncovered. It reverses nothing in
[ethernet.md](ethernet.md) by itself: the "not planned" verdicts of its
section 2.6 stand as far as free drivers go.

## 0. How this was checked

- **Linux** (facts only: chip names, USB ids, which file serves which
  chip): `drivers/net/usb/` at tag `v7.3` (commit
  `c03c1a14507dfaf8b856c2608f9ac216021fc222`, 2026-10-18), sparse clone,
  read only.
- **In-box Windows drivers**: the network-class INFs of each system,
  read from install media on this host and listed for any USB id (V):
  98 SE (CD INF set, 340 INFs), ME (371), 2000 SP4 (`I386\*.IN_`, 654),
  XP SP3 x86 (`I386\*.IN_`, 827), Vista SP2 x86 and x64, 7 SP1 x86 and
  x64, 8 x86 (RTM) and 8.1 x64 with Update (`install.wim`, `Windows\inf`
  and the driver store). XP x64 was not read (no full INF set on this
  host; U). Windows 10 was not read; Windows 11 (build 26300, the host)
  was, as a pointer only.
- **Makers' drivers**: the makers' own sites (ASIX, Realtek), and where a
  download is gone, the Internet Archive's copies of the maker's own file
  URLs (ASIX `FrootAttach/driver/`, Davicom `userfile/`, Realtek
  `rtdrivers/`, Microchip `ww1.microchip.com/downloads/`). Each archive
  was fetched into a scratch folder outside every repository, unpacked
  with 7-Zip without running anything, and only its INFs, readmes,
  history files and licence files were read. No installer or EXE was
  run; no binary was read.
- **Not reachable**: Microchip's software-library pages (HTTP 403 to this
  host), WCH's CH9200 page and CoreChips' site (no official copy found,
  live or archived). Facts that rest on those are marked U.
- V = read in the file or page named; U = unverified, with what would
  settle it. No legal conclusion is drawn anywhere: "licence: none found"
  means no licence text was found in the package or on the download page.

## 1. Summary

| Chip family | 98 SE | ME | 2000 | XP | XP x64 | Vista x86/x64 | 7 x86/x64 | 8 / 8.1 / 10 |
|---|---|---|---|---|---|---|---|---|
| ASIX AX88179, AX88178A | free `ax88179` | free `ax88179` | free `ax88179` (built, no leg) | nonfree | nonfree | nonfree | nonfree | in box 8.1 |
| ASIX AX88179A, AX88772D, AX88279 (CDC-NCM by default) | free `cdcnet` NCM (per ethernet.md; no such device run, U) | same | same | same (U: the ASIX XP INF also matches `0B95:1790` at any revision) | same | same | same | ASIX 8+ |
| ASIX AX88179B | U (id and protocol not read) | U | U | U | U | U | U | ASIX 8+ |
| ASIX AX88279A | gap (U: id) | gap | gap | gap | gap | gap | gap | ASIX 10+ |
| ASIX AX88772, AX88772A | nonfree | nonfree | nonfree | nonfree | step 2 | step 2 | step 2 | in box 8 |
| ASIX AX88772B, AX88772C | **gap** | **gap** | step 2 | step 2 | step 2 | step 2 | step 2 (5.12.12.0, live) | in box 8 |
| ASIX AX88772E | gap (U: id) | gap | gap | gap | gap | gap | gap | ASIX 8+ |
| ASIX AX88178 | step 2 | step 2 | step 2 | step 2 | step 2 | step 2 | step 2 | ASIX 7/8 build |
| ASIX AX88172 | step 2 (U: INF Signature `$Windows NT$`) | step 2 (same U) | step 2 | step 2 | **gap** | **gap** | **gap** | - |
| ASIX AX88172A | **gap** | **gap** | step 2 | step 2 | step 2 | step 2 | step 2 | - |
| MosChip/ASIX MCS7830/7832/7730 | step 2 | step 2 | step 2 | step 2 | step 2 | step 2 | step 2 | - |
| Realtek RTL8152(B), RTL8153 (A), RTL8153B | free `rtl815x` (simulated only) | free `rtl815x` | free `rtl815x` | nonfree | nonfree | nonfree | nonfree | in box 8.1 |
| Realtek RTL8153C | free `rtl815x` (chip id `6400`) | same | same | nonfree INF matches (revision-less `0BDA:8153` line; U whether the driver runs it) | same U | same U | nonfree | in box 8.1 |
| Realtek RTL8153D (`0BDA:8053`) | U (`RTL815X.INF` lists the id; its chip id is not among `rtl815x`'s, which refuses unknown ids) | U | U | gap (no `8053` line) | gap | gap | nonfree | Realtek 8+ |
| Realtek RTL8155 | gap (`rtl815x` leaves `8155` out) | gap | gap | gap | gap | gap | nonfree | Realtek 8+ |
| Realtek RTL8156, RTL8156B | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | nonfree | Realtek 8/10 |
| Realtek RTL8157, RTL8159 | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | Realtek 10+ |
| Realtek RTL8150 | step 2 | step 2 | step 2 | step 2 | step 2 | step 2 | candidate (Vista INF would install, owner question 8) | - |
| Davicom DM9601 | step 2 | step 2 | step 2 | step 2 | **gap** | **gap** | **gap** | - |
| Davicom DM9620/9621/9621A | step 2 | step 2 | step 2 | step 2 | step 2 | step 2 | step 2 | Davicom 8 |
| Davicom DM9622 (`0A46:9622`) | step 2 | step 2 | step 2 | **gap** | **gap** | **gap** | **gap** | - |
| CoreChips SR9700, SR9800 | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | - |
| WCH CH9200 | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | gap (U: WCH 7+ driver reported) | U |
| Microchip LAN9500/9505/9530/9730/9900/EC00 | **gap** | **gap** | **gap** | step 2 (U licence) | step 2 (U) | step 2 (U) | step 2 (U) | in box (11; 10 U) |
| Microchip LAN7500/7505 | **gap** | **gap** | **gap** | gap (U: package not obtained) | gap (U) | gap (U) | gap (U) | in box (11) |
| Microchip LAN7800/7801/7850 | **gap** | **gap** | **gap** | **gap** | **gap** | step 2 (U licence) | step 2 (U) | in box (11) |
| Aquantia/Marvell AQC111U/112U | U: `cdcnet` binds ECM by interface class only if the stack selects the ECM configuration (no descriptor read) | U | U | U | U | U | U (Marvell 7 driver reported) | Marvell |
| ADMtek Pegasus (ADM8511 and OEMs) | gap | gap | gap | in box for 17 of Linux's 70 distinct ids (and 6 Linux lacks); the rest gap | gap (U) | gap | gap | - |
| KLSI KL5KUSB101 (`kaweth`) | gap | gap | gap | in box for 19 of Linux's 35 ids (and 4 Linux lacks); the rest gap | gap (U) | gap | gap | - |
| CATC Netmate, Belkin F5U011/F5U111 | gap | gap | gap | gap | gap | gap | gap | - |
| CDC-ECM, CDC-NCM | free `cdcnet` | free `cdcnet` | free `cdcnet` | free `cdcnet` | free `cdcnet` | free `cdcnet` | free `cdcnet` | NCM in box (11) |
| RNDIS | free `cdcnet` | free `cdcnet` (ME: one in-box id) | free `cdcnet` | `CDCRNDIS.INF` over in-box | same | same | same | in box |
| CDC-EEM | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | - |

"nonfree": in `usb-wdm-nonfree` v1.0.0 today. "step 2": a maker's driver
this survey found and proposes for `usb-wdm-nonfree` (section 7). "free":
`usb-wdm-free`. "in box": Windows' own INF (section 2). "gap": nothing
found; it stays uncovered (section 6).

## 2. What Windows has in the box (V unless marked)

| System | USB Ethernet INFs (network class, USB ids) |
|---|---|
| 98 SE | none |
| ME | `usb8023.inf`: RNDIS, one id `0565:0041` |
| 2000 SP4 | none (only ISDN: `avmisdn.inf`, `didiva.inf`) |
| XP SP3 x86 | `net8511.inf` 2.4.2001.719 (`ADM8511.SYS`, ADMtek Pegasus II and OEMs, 23 ids); `netklsi.inf` 3.43.5.0 (`usb101et.sys`, KLSI KL5KUSB101 and OEMs, 23 ids); `netbcm4u.inf` (Broadcom iLine10 USB HomePNA phoneline, 6 ids: not Ethernet, no Linux driver) |
| XP x64 | not read (U) |
| Vista SP2 | `WceRndis.inf` (`EF/01/01`), `WceISVista.inf` (`E0/01/03`); no vendor NIC |
| 7 SP1 | `rndiscmp.inf` (`EF/04/01`, MS OS descriptor), `WceISVista.inf`; no vendor NIC |
| 8 x86 RTM | `netax88772.inf` (`0B95:7720`, `772A`, `772B`, `7E2B`, `0421:772A`, `0DB0:A877`, `125E:180D`) |
| 8.1 x64 Update | 8's, plus `netax88179_178a.inf` (`0B95:1790`, `178A`, `0DF6:0072`), `rtu64w8.inf` (`0BDA:8152`), `rtu30x64w8.inf` (`0BDA:8153`) |
| 11 (host, pointer only) | the above, plus Microchip `net9500`/`net7500`/`net7800`, Realtek `rtux64w10.inf` 10.38 and `rtucx21x64.inf` 11.4, `usbncm.inf` (CDC-NCM by class) |

## 3. Per chip: Linux driver, makers' drivers, non-free pack

Modern ASIX and Realtek chips are listed one per row (owner, 2026-10-27).
"Oldest Windows" is the oldest system the maker's *current* download page
offers a driver for.

### 3.1 ASIX

| Chip | USB id | Linux | Oldest Windows on ASIX's page today | Older package that lists the chip | `usb-wdm-nonfree` today |
|---|---|---|---|---|---|
| AX88179 | `0B95:1790` (rev `0100`) | `ax88179_178a.c` (vendor interface `FF/FF/00` only) | XP and Vista (1.4.2.0, 1.6.2.0, 2013-05-24), live: <https://www.asix.com.tw/en/support/download/step2/11/2/3> | - | XP to 7 (1.4.2.0 / 1.6.2.0 / 1.12.11.0 / 1.14.11.0), the newest for those systems |
| AX88178A | `0B95:178A` | `ax88179_178a.c` | same | - | same |
| AX88179A | `0B95:1790` rev `0200` | `cdc_ncm.c` by class (its default NCM configuration); `ax88179_178a.c` matches only the vendor interface | Windows 8 (3.18.1.0, 2024-01-08), live product page | none: the XP to 7 INFs list `0B95:1790` without a revision, so Windows would offer them; whether they run the A part is U | XP to 7 INF matches by id (U whether it works); `cdcnet` NCM serves it on every target |
| AX88179B | U (not named in any INF read) | as AX88179A (U) | Windows 8 (3.18.1.0) | none | as AX88179A (U) |
| AX88772D | `0B95:1790` rev `0300` | as AX88179A | Windows 8 (the AX88179A family package) | none | as AX88179A |
| AX88279 | `0B95:1790` revs `0400`, `0500` | as AX88179A | Windows 8 (3.18.1.0) | none | as AX88179A |
| AX88279A | U | U | Windows 10 (4.20.1.0) | none | - |
| AX88772E | U (not in the Win8 INF's model list; U) | U (`cdc_ncm` if it presents NCM: U) | Windows 8 (3.18.1.0) | none | - |
| AX88772C | `0B95:772B` (rev `0002`), `0B95:7E2B` | `asix_devices.c` (`ax88772b_info`) | Windows 7 (5.12.12.0 x86, 5.14.12.0 x64, 2015-05-14), live | AX88772B XP/2000 5.4.3.0, XP x64 5.6.3.0, Vista 5.8.2.0/5.10.2.0 (2010-05-21), archived official | none |
| AX88772B | `0B95:772B` (rev `0001`) | `asix_devices.c` | no page today (EOL) | as AX88772C | none |
| AX88772A, AX88760 | `0B95:772A` | `asix_devices.c` (`ax88772_info`) | no page today | 98 SE/ME 3.0.3.12; 2000/XP 3.4.3.38; XP x64 3.6.3.18; Vista 3.8.3.22 / 3.10.3.14; 7 3.12.3.2 / 3.14.3.2, archived official | 98 SE to XP |
| AX88772 | `0B95:7720` | `asix_devices.c` | no page today | same | 98 SE to XP |
| AX88178 | `0B95:1780` | `asix_devices.c` (`ax88178_info`) | no page today | 98 SE/ME 1.0.3.46; 2000/XP 1.4.3.64; XP x64 1.6.3.64; Vista 1.8.3.14 / 1.10.3.14; 7 1.12.6.0 / 1.14.6.0, archived official | none |
| AX88172A | `0B95:172A` | `ax88172a.c` with `asix_devices.c` | product page live, no Windows driver listed | 2000/XP 1.4.0.6; XP x64 1.6.0.6; Vista 1.8.0.5 / 1.10.0.5; 7 1.12.0.2 / 1.14.0.2, archived official | none |
| AX88172 | `0B95:1720` | `asix_devices.c` (`ax8817x_info`) | no page today | 98/ME 1.0.0.11; 2000 2.3.0.2; XP 2.0.0.5 (2003), archived official | none |
| MCS7830, MCS7832, MCS7730 | `9710:7830`, `7832`, `7730` | `mcs7830.c` | no page today (ASIX's MosChip line) | 98 SE/ME/2000/XP 1.49.0.0 (one 32-bit package with a `$CHICAGO$` INF and `MOSMAC98.SYS`); XP x64 1.49.0.0; Vista 1.0.0.18; 7 1.0.0.17, archived official | none |

Rebranded ASIX ids (Linux `ax88179_178a.c` against the ASIX INFs):
`0DF6:0072` (Sitecom), `17EF:304B` (Lenovo), `0930:0A13` (Toshiba) are in
the XP, Vista and 7 INFs; `04E8:A100` (Samsung), `050D:0128` (Belkin),
`0711:0179` (MCT), `07C9:000E`/`000F`/`0010` (Allied Telesis) only in the
7 INF (which also lists `07C9:0011`, not in Linux). Linux's `04B4:3610`
(Cypress GX3) and `2001:4A00` (D-Link DUB-1312) are in no ASIX INF read.
`asix_devices.c` lists about 30 OEM ids (Linksys, D-Link, Apple
`05AC:1402`, Lenovo `17EF:7203`, ...); the ASIX AX88772/A INFs list only
`0B95:7720`, `772A`, plus `0DB0:A877` (MSI) and `0421:772A` / `125E:180D`
on Vista; the AX88772B/C 7 INF adds `2001:1A02` (D-Link DUB-E100 C1).

### 3.2 Realtek

| Chip (r8152 version id) | USB id | Linux | Oldest Windows on Realtek's page today | `usb-wdm-nonfree` today (Realtek's own INFs, active lines only) |
|---|---|---|---|---|
| RTL8152 / RTL8152B (`4C00`, `4C10`, `4800`) | `0BDA:8152`, `0BDA:8050` | `r8152.c` | XP 5.23 (live list, 2021/07/27) | XP, XP x64, Vista, 7: any revision |
| RTL8153 (A) (`5C00`-`5C30`) | `0BDA:8153` revs `30xx` | `r8152.c`; `r8153_ecm.c` for its ECM configuration | XP 5.23 | XP to 7 |
| RTL8153B (`6000`, `6010`) | `0BDA:8153` revs `31xx` | `r8152.c` | XP 5.23 | XP to 7 (XP/Vista INF sections `RTL8153B*`) |
| RTL8153C (`6400`) | `0BDA:8153` revs `32xx` (U: from the 7 INF's revision list, not named) | `r8152.c` | 7 (U) | explicit `32xx` lines in the 7 INF only; the XP and Vista INFs also match `0BDA:8153` without a revision, so Windows would offer them (U whether those drivers run the chip) |
| RTL8153D | `0BDA:8053` (revs `33xx`, the 7 INF's `RTL8153D*` sections); the 7 INF also lists `0BDA:8153` at revs `33xx` (chip not named, U) | `r8152.c` matches `0BDA:8053` | 7 | 7 only |
| RTL8156 (`7020`, `7030`) | `0BDA:8156` revs `30xx` | `r8152.c` | 7 7.61.20 (2021/12/28) | 7 only |
| RTL8156B(G) (`7410`, `7420`) | `0BDA:8156` revs `31xx`, `0BDA:8056` | `r8152.c` | 7 7.61.20 | 7 only |
| RTL8155 | `0BDA:8155` | `r8152.c` | 7 | 7 only |
| RTL8157 (5G) | `0BDA:8157` (`8057`, `A157`) | `r8152.c` (`RTL_VER_16`, `r8157_*` paths) | Windows 10 10.68.20 (2026/08/31); the 8/8.1 package 8.68.20 not read (U) | none: the 7 INF has these lines **commented out** |
| RTL8159 (10G) | `0BDA:815A` (`805A`, `A15A`) | `r8152.c` (`RTL_VER_17`) | Windows 10 10.68.20 | none (commented out in the 7 INF) |
| RTL8150 | `0BDA:8150` | `rtl8150.c` | 98 SE/ME, 2000, XP (x86, x64) and Vista (x86, x64) in one package, 5.126 (2008/04/28 and 2010/11/03 on the archived page) | none |

Realtek's live list (`https://www.realtek.com/Download/List?cate_id=585`,
read 2026-10-27 through its `ListAllDownloadItem` JSON) offers XP 5.23,
Vista 6.27, 7 7.61.20, 8/8.1 8.68.20, 10 10.68.20 and 11 115X.23.20: the
non-free pack's XP, Vista and 7 packages are still the newest for those
systems, so a newer package would widen nothing.

Rebranded Realtek ids (Linux `r8152.c` against Realtek's INFs; the
vendor-id lines in Realtek's INFs carry a fixed revision each, so a
dongle reporting another `bcdDevice` does not match):

| Linux id | Maker | XP 5.23 / Vista 6.27 | 7 7.61.20 |
|---|---|---|---|
| `045E:07AB`, `07C6`, `0927`, `0C5E` | Microsoft (Surface adapters) | no | no |
| `0955:09FF` | NVIDIA | no | no |
| `04E8:A101` | Samsung | yes (rev `3000`) | yes |
| `13B1:0041` | Linksys | yes | yes |
| `17EF:304F`, `3054`, `3062`, `3069`, `3082`, `7205`, `720C`, `7214`, `A359` | Lenovo | yes | yes |
| `17EF:3098`, `721E`, `A387` | Lenovo | no | yes |
| `2357:0601`, `0602` | TP-Link | yes | yes |
| `2001:B301` | D-Link | no | yes |
| `413C:B097` | Dell | no | yes |
| `0B05:1976` | ASUS | no | yes |
| `20F4:E02B`, `E02C` | TRENDnet | no | yes |

Anker, UGREEN and Cable Matters ship under Realtek's own `0BDA` ids in
Linux's table (none has its own row in `r8152.c`); their dongles match by
`0BDA:8152`/`8153`/`8156` (U: not checked per product). `usb-wdm-free`'s
`RTL815X.INF` lists the same 30 Linux ids (less `0BDA:8155/8156/8157/815A`).

### 3.3 Other families

| Chip | USB ids (Linux) | Linux | Makers' drivers found | `usb-wdm-nonfree` today |
|---|---|---|---|---|
| Davicom DM9601 | `0A46:9601` and 12 OEM/clone ids | `dm9601.c` | 98/ME 2.0.100706.02 (2010, also 9620/9621/9622); DM9621A package 5.0.1.0 (2012-11-21, "Win98/2K", INF with `$CHICAGO$` and NT sections, 6 ids); ME 1.60, 2000 2.10, XP 1.90 "Logo" drivers (2002); none for XP x64, Vista or 7 (the V1.0.4.0 INFs for those omit `9601`) | none |
| Davicom DM9620/9621/9621A/9622 | `0A46:9620`, `9621`, `9622`, `0269`, `1269` | `dm9601.c` | the above, plus "DM96xx Driver V1.0.4.0" (2013, one installer): 2000 3.0.2.0416, XP and XP x64 5.0.2.0709, Vista 5.0.2.0406, 7 and 8 5.0.3.0703 | none |
| CoreChips SR9700 | `0FE6:9700`, `9702` | `sr9700.c` | none found (corechip-sz.com not archived) | none |
| CoreChips SR9800 | `0FE6:9800` | `sr9800.c` | none found | none |
| WCH CH9200 | `1A86:E092` | `ch9200.c` | none read; a 7+ driver (`ch9100nic.inf` 1.0.0.0, 2013) is reported by third-party listings only (U) | none |
| Microchip LAN9500/9505/9530/9730/9900-9909/EC00 | `0424:9500` and 17 more | `smsc95xx.c` | `lan9500-wdf-v18.12.18.0.zip`: XP x86/x64 2.6.5.0 (NDIS 5.1, KMDF 1.9), Vista 2.11.0.0, 7 2.17.0.0 | none |
| Microchip LAN7500/7505 | `0424:7500`, `7505` | `smsc75xx.c` | the package `lan7500-wdf-v18.12.18.0.zip` is named in Microchip's readme (XP to 10, per a search summary: U); no copy obtained | none |
| Microchip LAN7800/7801/7850 | `0424:7800`, `7801`, `7850`, `07C9:0012` | `lan78xx.c` | `lan7800-wdf-v18.12.14.0.zip`: Vista 3.5.2.0, 7 3.8.4.0 (KMDF 1.9); no XP | none |
| Aquantia/Marvell AQC111U, AQC112U (ASIX-branded too) | `2ECA:C101`, `0B95:2790`, `2791`, `20F4:E05A`, `1C04:0015` | `aqc111.c` (vendor) and `cdc_ether.c` (ECM) | Marvell "AQ_USBdongle" 1.8.0.0 for 7, reported by third-party listings (U) | none |
| ADMtek Pegasus/Pegasus II | 74 entries (70 distinct ids) in `pegasus.h` | `pegasus.c` | XP in box (`net8511.inf`, 23 ids, 17 of them in Linux's table); makers' 98/ME/2000 drivers not searched (ADMtek, later Infineon; U) | none |
| KLSI KL5KUSB101 | 35 ids | `kaweth.c` | XP in box (`netklsi.inf`, 23 ids, 19 of them in Linux's table); not searched otherwise (U) | none |
| CATC Netmate / Belkin F5U011/F5U111 / smartNIC | 3 ids | `catc.c` | not searched (U) | none |
| Intellon INT51x1 powerline | `09E1:5121` | `int51x1.c` | not searched (U) | none |

## 4. Linux sources (facts)

At `v7.3`; line counts of the file as it stands; SPDX as tagged, or the
header's own words where there is no SPDX line. `usb-wdm-free` adapts only
GPL-2.0-or-later (or compatible) code with notices kept and a `NOTICE.md`
row; from GPL-2.0-only files it takes facts only (AGENTS.md, "Sources").

| File | SPDX / header | Lines | Chips |
|---|---|---|---|
| `asix_devices.c` (+ `asix_common.c` 756, `asix.h` 252, `ax88172a.c` 358) | GPL-2.0-or-later | 1634 | AX88172, AX88772/A/B/C, AX88178, AX88172A |
| `ax88179_178a.c` | GPL-2.0-or-later | 1954 | AX88179, AX88178A (vendor interface only) |
| `r8152.c` | GPL-2.0-only | 10520 | RTL8152/B, 8153 A-D, 8155, 8156/B, 8157, 8159 |
| `r8153_ecm.c` | GPL-2.0-or-later | 176 | RTL8153 ECM configuration |
| `rtl8150.c` | GPL-2.0-only | 996 | RTL8150 |
| `dm9601.c` | no SPDX; "GNU General Public License version 2" | 632 | DM9601, DM962x and clones |
| `sr9700.c` (+ `sr9700.h` GPL-2.0-only, 162) | no SPDX; "version 2" | 484 | SR9700 |
| `sr9800.c` (+ `sr9800.h`, 201) | no SPDX; "version 2" | 853 | SR9800 |
| `ch9200.c` | no SPDX; "version 2" | 429 | CH9200 |
| `mcs7830.c` | GPL-2.0-or-later | 630 | MCS7830/7832/7730 |
| `smsc95xx.c` (+ `.h` 352) | GPL-2.0-or-later | 2195 | LAN95xx |
| `smsc75xx.c` (+ `.h` 409) | GPL-2.0-or-later | 2339 | LAN7500/7505 |
| `lan78xx.c` (+ `.h` 882) | GPL-2.0+ | 5423 | LAN7800/7801/7850 |
| `aqc111.c` (+ `.h` 232) | GPL-2.0-or-later | 1493 | AQC111U/112U |
| `pegasus.c` (+ `.h` 301) | GPL-2.0-only | 1365 | ADMtek Pegasus |
| `kaweth.c` | GPL-2.0-or-later | 1126 | KLSI KL5KUSB101 |
| `catc.c` | GPL-2.0-or-later | 1008 | CATC Netmate |
| `int51x1.c` | GPL-2.0-or-later | 156 | Intellon powerline |
| `cdc_ether.c` | GPL-2.0-or-later | 1000 | CDC-ECM (served by `cdcnet`) |
| `cdc_ncm.c` | no SPDX; "Dual BSD/GPL" | 2149 | CDC-NCM (served by `cdcnet`) |
| `rndis_host.c` | GPL-2.0-or-later | 711 | RNDIS (served by `cdcnet`) |
| `cdc_eem.c` | GPL-2.0-or-later | 366 | CDC-EEM (not served) |
| `cdc_subset.c` | GPL-2.0-or-later | 357 | link cables, PDAs, Linux gadget subset mode |
| `usbnet.c` | GPL-2.0-or-later | 2294 | the common framework |

Protocol size, from the files and ethernet.md section 2.2: CDC-ECM moves
one frame per transfer with no header; RNDIS puts a 44-byte header on each
packet and runs a control channel; CDC-NCM aggregates frames in transfer
blocks (NTB16). The ASIX AX8817x/AX88772 family, MosChip, Davicom,
CoreChips and WCH parts frame each frame behind a short header (2 to 4
bytes) or none, with a few vendor control requests for registers and a MII
PHY; AX88179 and the Realtek RTL815x family add receive aggregation with
per-packet descriptors and per-revision bring-up (`r8152.c` is ten times
the size of the small drivers); the LAN78xx and LAN95xx need
register setup with checksum offload words. Not used further here: no free
driver is planned beyond `ax88179`.

## 5. Testability

Only the AX88179/178A family can be tested on real hardware: the owner's
AX88179 (`0B95:1790`) has run `ax88179` on 98 SE and ME through USB
passthrough to a VM (owner, 2026-10-27). The owner has no other adapter.
QEMU (upstream) emulates only `usb-net` (RNDIS and ECM); `cdcnet` NCM and
`rtl815x` run against this project's simulated devices (`ncmemu.py`,
`rtlemu.py`). The makers' drivers proposed for the non-free pack are
carried untested, as the pack's README says of every driver it holds.

## 6. Gaps

Cells with no maker's driver found (or proposed in section 7), no in-box driver and no
`usb-wdm-free` driver. Each stays uncovered (owner, 2026-10-27: no new free
drivers in 44.68).

| Chip | Systems left uncovered | Why |
|---|---|---|
| AX88772B, AX88772C | 98 SE, ME | ASIX's 98/ME driver (3.0.3.12, 3.0.1.11) lists only `0B95:7720`/`772A`; no 772B build found. Stays uncovered |
| AX88172 | XP x64, Vista, 7 | no build found. Stays uncovered |
| AX88172A | 98 SE, ME | no build found. Stays uncovered |
| AX88772E, AX88279A | 98 SE to 7 | ASIX lists 8 or 10 and later; id not settled (U); `cdcnet` if they present NCM (U). Stays uncovered |
| RTL8156, RTL8156B | 98 SE, ME, 2000 | `rtl815x` refuses the chip version at start; `cdcnet` only if the dongle offers an ECM/NCM configuration (U). Stays uncovered |
| RTL8156, RTL8156B | XP, XP x64, Vista | Realtek's XP 5.23 and Vista 6.27 INFs do not list `0BDA:8156`/`8056`. Stays uncovered |
| RTL8157, RTL8159 | 98 SE to 7 | Realtek: Windows 10 and later; the 7 INF's lines are commented out. Stays uncovered |
| DM9601 | XP x64, Vista, 7 | Davicom's XP x64/Vista/7 INFs omit `0A46:9601`. Stays uncovered |
| SR9700, SR9800 | 98 SE to 7 | no maker's driver found. Stays uncovered |
| CH9200 | 98 SE to 7 | none read (a 7+ driver reported, U). Stays uncovered |
| LAN9500 family | 98 SE, ME, 2000 | Microchip's oldest is XP. Stays uncovered |
| LAN7500/7505 | 98 SE to 7 | package not obtained (U); Microchip site refused this host. Stays uncovered until read |
| LAN7800 family | 98 SE to XP x64 | Microchip's oldest is Vista. Stays uncovered |
| Pegasus, KLSI | all but XP x86; on XP x86 the 53 Pegasus and 16 KLSI ids Linux lists that `net8511.inf`/`netklsi.inf` do not | XP in box only; makers' drivers not searched. Stays uncovered |
| CATC Netmate, INT51x1 | all | not searched (1999-2003 USB 1.1 parts). Stays uncovered |
| CDC-EEM | all | `cdcnet` has ECM, NCM and RNDIS only. Stays uncovered |
| Rebranded Realtek ids `045E:07AB/07C6/0927/0C5E`, `0955:09FF` | XP to 7 | no Realtek XP-7 INF lists them; on 98 SE to 2000 `rtl815x` lists them. Stays uncovered |
| Rebranded ASIX ids `04B4:3610`, `2001:4A00` | XP to 7 | no ASIX INF lists them (on 98 SE to 2000 `AX179F98.INF`/`AX179F.INF` list them, with every other Linux AX88179 id). Stays uncovered |
| ASIX AX88179 OEM ids `04E8:A100`, `050D:0128`, `0711:0179`, `07C9:000E`/`000F`/`0010` | XP, XP x64, Vista | only ASIX's 7 INF lists them. Stays uncovered |
| ASIX AX88772C `0B95:7E2B` | 2000 to Vista | only the 7 INF (5.12.12.0) lists it. Stays uncovered |
| Realtek OEM ids `17EF:3098`, `721E`, `A387`, `2001:B301`, `413C:B097`, `0B05:1976`, `20F4:E02B`, `E02C` | XP, XP x64, Vista | only Realtek's 7 INF lists them (`rtl815x` lists them on 98 SE to 2000). Stays uncovered |
| RTL8150 OEM ids `0411:0012`, `3980:0003`, `07B8:401A`, `1557:8150`, `0586:401A` | 98 SE to 7 | Realtek's INFs list only `0BDA:8150`. Stays uncovered |
| DM9622 `0A46:9622` | XP to 7 | Davicom's XP to 7 INFs omit it. Stays uncovered |
| LAN7800 OEM id `07C9:0012` | Vista, 7 | Microchip's INFs list only `0424:78xx`. Stays uncovered |
| RTL8153D `0BDA:8053` | 98 SE to Vista | `rtl815x` does not know its chip id (U); Realtek's XP/Vista INFs have no `8053` line. Stays uncovered |
| RTL8155 `0BDA:8155` | 98 SE to Vista | `rtl815x` leaves it out; Realtek's XP/Vista INFs do not list it. Stays uncovered |
| AX88179B, AQC111U/112U | 98 SE to 7 | U: whether `cdcnet` reaches them (NCM/ECM configuration selection, not read from a device). Uncovered until shown |

## 7. Step 2: drivers for `usb-wdm-nonfree`

For the separate worker that adds them (this survey commits nothing
there). Layout follows `non-free/ethernet/<driver>/<system>/`, folders
named as the pack's README does (`w98se-me`, `w2k`, `wxp`, `wxpvis`,
`w7`, a `64` suffix for 64-bit, `w2k-xp` where one build serves both).
Every archive below was read on 2026-10-27; sizes and SHA-256 are of the
archive as fetched. Each zip holds one folder named after itself (except
where said) whose files are copied without it, as for the pack's existing
ASIX rows.

**Licence, for every package in this section unless said otherwise:**
none found. No licence, EULA or readme with terms is in any of the
archives; the INFs carry only the maker's copyright line, quoted per
package. Redistributable: **unclear**, on the same footing as the ASIX and
Realtek drivers the pack already carries ("Licence: none found" in its
NOTICE.md); owner question 1.

### 7.1 Already in the pack, not to duplicate

- `ax88772/w98se-me` (3.0.3.12) and `ax88772/w2k-xp` (3.4.3.38): the
  newest for those systems. The older 3.0.1.11 (98/ME, 2004) was found
  and is not proposed.
- `ax88179/wxpvis`, `wxpvis64`, `w7`, `w7_64`: still the newest ASIX
  offers for XP to 7. ASIX's Windows 8 package 3.18.1.0
  (`ASIX_USB_Ethernet_Win8_64bit_Driver_v3.18.1.0.zip`, live as
  `https://www.asix.com.tw/en/support/download/file/1856`, 92,429 bytes,
  SHA-256 `7f7b147179e9de6883624275c0e31993a6ed9354d9dca861553a6863444f8335`)
  names AX88179, AX88179A (`REV_0200`), AX88772D (`0300`) and AX88279
  (`0400`) under `0B95:1790` with an undecorated `NTamd64` section, so its
  INF would install on 7 x64; whether the driver runs there is U (owner
  question 4). Not proposed.
- `rtl815x/wxp`, `wvista`, `w7`: still the newest Realtek offers.

### 7.2 Proposed packages

Archived URLs are the Internet Archive's copies of the maker's own file
URLs: `https://web.archive.org/web/<timestamp>/<original>`.

**(a) ASIX AX88772/AX88772A, XP x64, Vista and 7** (`ax88772/`)

| Folder | Systems | Version, date | Archive (size, SHA-256) | Archived official URL | INF ids |
|---|---|---|---|---|---|
| `wxp64/` | XP x64 | 3.6.3.18, 2007-09-21 | `AX88772_772A_WinXP2K3_64bit_Driver_v3.6.3.18_WHQL.zip` (26,708, `ce31fc3a8119d9d7df38edd8b40bb828da41498f3f6bc3bd791cee7cfffa06a7`) | `20110802222733/http://www.asix.com.tw/FrootAttach/driver/AX88772_772A_WinXP2K3_64bit_Driver_v3.6.3.18_WHQL.zip` | `0B95:7720`, `772A` |
| `wvista/` | Vista x86 | 3.8.3.22, 2009-10-02 (KMDF 1.7) | `AX88772_772A_760_Vista_32bit_Driver_v3.8.3.22_WHQL.zip` (1,105,293, `b49f0fb05a8ccf5a33d843bbbb38bfeafb530ed861a6c18a4f570cf376b7a364`) | `20111127002545/http://www.asix.com.tw/FrootAttach/driver/AX88772_772A_760_Vista_32bit_Driver_v3.8.3.22_WHQL.zip` | `0B95:7720`, `772A`, `0DB0:A877`, `0421:772A`, `125E:180D` |
| `wvista64/` | Vista x64 | 3.10.3.14, 2008-11-18 | `AX88772_772A_Vista_64bit_Driver_v3.10.3.14_WHQL.zip` (33,026, `c1b167f1b6f9dea416355e938aa1abcf4f4dac00ecc43df700fd47fe29804ba5`) | `20140703183417/http://www.asix.com.tw/FrootAttach/driver/AX88772_772A_Vista_64bit_Driver_v3.10.3.14_WHQL.zip` | `0B95:7720`, `772A`, `0DB0:A877` |
| `w7/` | 7 x86 | 3.12.3.2, 2009-06-10 (KMDF 1.9) | `AX88772_772A_760_Win7_32bit_Driver_v3.12.3.2_WHQL.zip` (1,166,729, `16e4309bfa3bb04ad182de1a600b03ce0b065e94efc5f3e010195e0cf6c64ee0`) | `20111127002136/http://www.asix.com.tw/FrootAttach/driver/AX88772_772A_760_Win7_32bit_Driver_v3.12.3.2_WHQL.zip` | `0B95:7720`, `772A`, `0DB0:A877`, `0421:772A` |
| `w7_64/` | 7 x64 | 3.14.3.2, 2009-06-10 (KMDF 1.9) | `AX88772_772A_760_Win7_64bit_Driver_v3.14.3.2_WHQL.zip` (1,428,346, `b512266a949ec00177dd0972c941f9dc156869935b1069e16055c0105882ba2c`) | `20111214235701/http://www.asix.com.tw/FrootAttach/driver/AX88772_772A_760_Win7_64bit_Driver_v3.14.3.2_WHQL.zip` | as `w7/` |

ASIX's archived AX88760/772A page (PItemID 103, 2010) listed newer XP x64
3.6.3.38 and Vista 3.8.3.23 / 3.10.3.23 builds; no copy of those files was
found. INF copyright: "Copyright 2003-2007/2008/2009, ASIX Electronics
Corporation".

**(b) ASIX AX88772B/AX88772C, 2000 to 7** (`ax88772b/`)

| Folder | Systems | Version, date | Archive (size, SHA-256) | URL | INF ids |
|---|---|---|---|---|---|
| `w2k-xp/` | 2000, XP x86 | 5.4.3.0, 2010-05-21 (KMDF 1.7, `WdfCoInstaller01007.dll`) | `AX88772B_XP2K_32bit_Driver_v5.4.3.0_WHQL.zip` (1,108,220, `3ee77b145333e671614519ba81789c1467b6772c3a6e6e7103c00167b2f6f367`) | archived `20111106121911/http://www.asix.com.tw/FrootAttach/driver/AX88772B_XP2K_32bit_Driver_v5.4.3.0_WHQL.zip` | `0B95:772B` |
| `wxp64/` | XP x64 | 5.6.3.0, 2010-05-21 | `AX88772B_XP2K3_64bit_Driver_v5.6.3.0_WHQL.zip` (1,488,097, `f120b772fce256fb0bfbe91da7396767b17f44954663a986cc319766a5095645`) | archived `20111106122513/...AX88772B_XP2K3_64bit_Driver_v5.6.3.0_WHQL.zip` | `0B95:772B` |
| `wvista/` | Vista x86 | 5.8.2.0, 2010-05-21 | `AX88772B_Vista_32bit_Driver_v5.8.2.0_WHQL.zip` (1,113,184, `e3b2ceb8f1cf689af666bb7099f2b48d318a0baa6c0e176d2febff3319f924b4`) | archived `20111106121035/...AX88772B_Vista_32bit_Driver_v5.8.2.0_WHQL.zip` | `0B95:772B` |
| `wvista64/` | Vista x64 | 5.10.2.0, 2010-05-21 | `AX88772B_Vista_64bit_Driver_v5.10.2.0_WHQL.zip` (1,493,165, `19cdc77be51490383d90625048a96ff7513f76234713b8547ec99dcd944762eb`) | archived `20111106121442/...AX88772B_Vista_64bit_Driver_v5.10.2.0_WHQL.zip` | `0B95:772B` |
| `w7/` | 7 x86 | 5.12.12.0, 2015-05-14 | `AX88772C_772B_Win7_32bit_Driver_v5.12.12.0_WHCK.zip` (51,395, `959f783ebe0d160b5b2693fd99b2c146de41ad0f82a1daeb24c414f75f05b370`) | live `https://www.asix.com.tw/en/support/download/file/241` (AX88772C page) | `0B95:772B` revs `0001`, `0002`; `0B95:7E2B` rev `0001`; `2001:1A02` |
| `w7_64/` | 7 x64 | 5.14.12.0, 2015-05-14 | `AX88772C_772B_Win7_64bit_Driver_v5.14.12.0_WHCK.zip` (59,790, `79aa9496bffb0cb409c2f57539e077b47c142f12888c1358c2e53793f36f2fea`) | live `https://www.asix.com.tw/en/support/download/file/242` | as `w7/` |

Original prefix for the archived rows: `http://www.asix.com.tw/FrootAttach/driver/`.
INF copyright: "Copyright 2010, ASIX Electronics Corporation" (2015 for 7).

**(c) ASIX AX88178, 98 SE to 7** (`ax88178/`)

| Folder | Systems | Version, date | Archive (size, SHA-256) | Archived URL (timestamp/original) | INF ids |
|---|---|---|---|---|---|
| `w98se-me/` | 98 SE, ME | 1.0.3.46, 2005-01-18 (`$CHICAGO$`) | `AX88178_WinME98_v1.0.3.46.zip` (13,974, `ad44af1ac38a60c0769bf63a63592699b04c5dde29192be1b69de1f6a1b20666`); files at the zip's root | `20060507062610/http://www.asix.com.tw:80/FrootAttach/driver/AX88178_WinME98_v1.0.3.46.zip` | `0B95:1780` |
| `w2k-xp/` | 2000, XP x86 | 1.4.3.64, 2010-11-24 (NDIS-WDM, no KMDF) | `AX88178_XP2K_32bit_Driver_v1.4.3.64_WHQL.zip` (28,087, `f957ae8c8e3f72c973fe5b76fa7a710b54d1f91458cc38276c735f69b7dc790c`) | `20171108160114/http://asix.com.tw/FrootAttach/driver/AX88178_XP2K_32bit_Driver_v1.4.3.64_WHQL.zip` | `0B95:1780` |
| `wxp64/` | XP x64 | 1.6.3.64, 2010-11-24 | `AX88178_XP2K3_64bit_Driver_v1.6.3.64_WHQL.zip` (35,037, `e4b72e43bca21894a97051469c0441f24539250db78d362c95e9bb84ac7298f6`) | `20171108200702/http://asix.com.tw/FrootAttach/driver/AX88178_XP2K3_64bit_Driver_v1.6.3.64_WHQL.zip` | `0B95:1780` |
| `wvista/` | Vista x86 | 1.8.3.14, 2010-11-24 (KMDF 1.7) | `AX88178_Vista_32bit_Driver_v1.8.3.14_WHQL.zip` (1,100,080, `796de3d5e29cd2c9e6e485e9b717dada8597eb62fea7f4cc8d5775afa394290e`) | `20171108213820/http://asix.com.tw/FrootAttach/driver/AX88178_Vista_32bit_Driver_v1.8.3.14_WHQL.zip` | `0B95:1780` |
| `wvista64/` | Vista x64 | 1.10.3.14, 2010-11-24 | `AX88178_Vista_64bit_Driver_v1.10.3.14_WHQL.zip` (1,477,896, `891b94d5a4bdbf8fdeb29cfe88b750977ca284a80450611d978a87b109e9e95b`) | `20171108183405/http://asix.com.tw/FrootAttach/driver/AX88178_Vista_64bit_Driver_v1.10.3.14_WHQL.zip` | `0B95:1780` |
| `w7/` | 7 x86 (and 8) | 1.12.6.0, 2013-07-12 (KMDF 1.9) | `AX88178_Win7_Win8.x_32bit_Driver_v1.12.6.0_WHCK.zip` (1,454,326, `6a3dde95aa1d2f5bccd63ca69167e00cd0b523689b40049a7259681333f5a6f2`) | `20171108154804/http://asix.com.tw/FrootAttach/driver/AX88178_Win7_Win8.x_32bit_Driver_v1.12.6.0_WHCK.zip` | `0B95:1780` |
| `w7_64/` | 7 x64 (and 8) | 1.14.6.0, 2013-07-12 | `AX88178_Win7_Win8.x_64bit_Driver_v1.14.6.0_WHCK.zip` (1,714,600, `6220bb36412d6cef7b772e74ef907b3a90f166fdb3a77da159a21f0012ba1583`) | `20171108231631/http://asix.com.tw/FrootAttach/driver/AX88178_Win7_Win8.x_64bit_Driver_v1.14.6.0_WHCK.zip` | `0B95:1780` |

INF copyright: "Copyright 2004-2005 / 2004-2010 / 2003-2010 / 2003-2013,
ASIX Electronics Corporation".

**(d) ASIX AX88172, 98 SE to XP** (`ax88172/`). Each zip holds a version
folder (`v1.0.0.11/`, `v2.3.0.2_WHQL/`, `v2.0.0.5_WHQL/`).

| Folder | Systems | Version, date | Archive (size, SHA-256) | Archived URL | INF ids |
|---|---|---|---|---|---|
| `w98se-me/` | 98 SE, ME | 1.0.0.11, 2003-07-04 | `AX88172_WinME98_v1.0.0.11.zip` (8,811, `943c46dcb0a1cec68563222962a65d49f5b072df2ae806c2edf85069d22b329b`) | `20060523201836/http://www.asix.com.tw:80/FrootAttach/driver/AX88172_WinME98_v1.0.0.11.zip` | `0B95:1720` |
| `w2k/` | 2000 | 2.3.0.2, 2003-07-02 | `AX88172_Win2K_v2.3.0.2_WHQL.zip` (13,045, `0df00da45898719691167faa08ccdeaeffb0b68496c2967ddd13c9c2d779af86`) | `20060523201824/http://www.asix.com.tw:80/FrootAttach/driver/AX88172_Win2K_v2.3.0.2_WHQL.zip` | `0B95:1720` |
| `wxp/` | XP x86 | 2.0.0.5, 2003-05-22 | `AX88172_WinXP_v2.0.0.5_WHQL.zip` (12,951, `eb03c248f8ea7dbc4933cd6efaa3c2426f4fabfacca76e584dde6acb473458f5`) | `20060523201904/http://www.asix.com.tw:80/FrootAttach/driver/AX88172_WinXP_v2.0.0.5_WHQL.zip` | `0B95:1720` |

The 98/ME INF's `Signature` reads `"$Windows NT$"` although its sections
are 9x-style (U whether 98 SE accepts it; owner question 5). INF copyright:
"Copyright 2001-2003, ASIX Electronics Corporation".

**(e) ASIX AX88172A, 2000 to 7** (`ax88172a/`)

| Folder | Systems | Version, date | Archive (size, SHA-256) | Archived URL | INF ids |
|---|---|---|---|---|---|
| `w2k-xp/` | 2000, XP x86 | 1.4.0.6, 2010-06-02 | `AX88172A_XP2K_32bit_Driver_v1.4.0.6_WHQL.zip` (33,119, `180a608be8ea7a1c3946a1fcc4eaf5ae5d50d5a1ba15f7da8e5c42b9afec13e5`) | `20171109030053/http://asix.com.tw/FrootAttach/driver/AX88172A_XP2K_32bit_Driver_v1.4.0.6_WHQL.zip` | `0B95:172A` |
| `wxp64/` | XP x64 | 1.6.0.6 | `AX88172A_XP2K3_64bit_Driver_v1.6.0.6_WHQL.zip` (41,548, `bf9164547e6ef05c2afd311fb8c26dfbf864ff01f03d433d25dbdc755370e817`) | `20171109040410/...AX88172A_XP2K3_64bit_Driver_v1.6.0.6_WHQL.zip` | `0B95:172A` |
| `wvista/` | Vista x86 | 1.8.0.5 (KMDF 1.7) | `AX88172A_Vista_32bit_Driver_v1.8.0.5_WHQL.zip` (1,106,187, `a729958ecaea1edd7634b5662305b64ffddc86bb05b898e06974520394e809c9`) | `20171109005925/...AX88172A_Vista_32bit_Driver_v1.8.0.5_WHQL.zip` | `0B95:172A` |
| `wvista64/` | Vista x64 | 1.10.0.5 | `AX88172A_Vista_64bit_Driver_v1.10.0.5_WHQL.zip` (1,484,548, `898bde6066be1d53334cc4fbf97a0ba945a9260622a3e4652d71d744d71c4d40`) | `20171109052754/...AX88172A_Vista_64bit_Driver_v1.10.0.5_WHQL.zip` | `0B95:172A` |
| `w7/` | 7 x86 | 1.12.0.2 (KMDF 1.9) | `AX88172A_Win7_32bit_Driver_v1.12.0.2_WHQL.zip` (1,458,056, `ca1c4a8d804c4c4071f46b58e7a297d0a76e6c2416ce62e6707fc387fcba34c9`) | `20171109070729/...AX88172A_Win7_32bit_Driver_v1.12.0.2_WHQL.zip` | `0B95:172A` |
| `w7_64/` | 7 x64 | 1.14.0.2 | `AX88172A_Win7_64bit_Driver_v1.14.0.2_WHQL.zip` (1,718,450, `c17f856a8768ac0f73568b3694213c8c3477cf5825e74f9e91d151a8a31af98a`) | `20171109015339/...AX88172A_Win7_64bit_Driver_v1.14.0.2_WHQL.zip` | `0B95:172A` |

All `DriverVer` 2010-06-02; original prefix `http://asix.com.tw/FrootAttach/driver/`.
INF copyright: "Copyright 2007-2010, ASIX Electronics Corporation".

**(f) MosChip (ASIX) MCS7830/7832/7730, 98 SE to 7** (`mcs7830/`)

| Folder | Systems | Version, date | Archive (size, SHA-256) | Archived URL | INF ids |
|---|---|---|---|---|---|
| `w98se-xp/` | 98 SE, ME, 2000, XP x86 (one `$CHICAGO$` INF, `MOSMAC98.SYS` for 9x, `MOSUMAC.SYS` for NT) | 1.49.0.0, 2012-05-25 | `MCS783x_Windows_XP_32bit_Driver_v1.49.0.0_WHQL.zip` (47,610, `f8632cac2c506c34dfb7299c88c6a66f0545d2c8ec957c0b9e63a8787f7e1d8f`) | `20171031193523/http://asix.com.tw/FrootAttach/driver/MCS783x_Windows_XP_32bit_Driver_v1.49.0.0_WHQL.zip` | `9710:7730`, `7830`, `7832` |
| `wxp64/` | XP x64 | 1.49.0.0 | `MCS783x_Windows_XP_64bit_Driver_v1.49.0.0_WHQL.zip` (34,040, `dfcbd8258abb0f526ecae49399c15b3b18b80de20054d711103b861a491ea226`) | `20171031185302/...MCS783x_Windows_XP_64bit_Driver_v1.49.0.0_WHQL.zip` | same |
| `wvista/` | Vista x86 | 1.0.0.18, 2011-08-09 (KMDF 1.9) | `MCS783x_Vista_32bit_driver_v1.0.0.18_WHQL_20110902.zip` (1,451,221, `5747144fc5f2886dcf2710b79bd5f61adf7ea12fdf807ea799281501c7e7f333`) | `20171101010524/...MCS783x_Vista_32bit_driver_v1.0.0.18_WHQL_20110902.zip` | same |
| `wvista64/` | Vista x64 | 1.0.0.18 | `MCS783x_Vista_64bit_driver_v1.0.0.18_WHQL_20110902.zip` (1,711,105, `4e5355b3f5ef3aac513aed5d19f506c8bd3861f6f723acf49ab86eaa6fcf19df`) | `20171101021713/...MCS783x_Vista_64bit_driver_v1.0.0.18_WHQL_20110902.zip` | same |
| `w7/` | 7 x86 | 1.0.0.17, 2010-06-01 | `MCS783x_Win7_32bit_driver_v1.0.0.17_WHQL_20110902.zip` (32,275, `0f31509b36580d37bcbd435a67c29220a08f65b87a8eb291b7aee91b14483710`) | `20171031191755/...MCS783x_Win7_32bit_driver_v1.0.0.17_WHQL_20110902.zip` | same |
| `w7_64/` | 7 x64 | 1.0.0.17 | `MCS783x_Win7_64bit_driver_v1.0.0.17_WHQL_20110902.zip` (36,370, `cd179d486b2d1c5bf8201db4a15bb5ef1f1e795c7bb8d3b57f8786ab88a4cebd`) | `20171031164337/...MCS783x_Win7_64bit_driver_v1.0.0.17_WHQL_20110902.zip` | same |

Original prefix `http://asix.com.tw/FrootAttach/driver/`. ASIX's archived
MCS7830 page (PItemID 109, 2014) lists "Windows XP/2000 32-bit Driver
v1.49.0.0" and "Windows XP/2003 64-bit Driver v1.49.0.0" (2012-05-25),
Vista 1.0.0.18 (2011-08-09). INF copyright: "(c) Copyright 2002 - 2012"
(no holder named); the INF header reads "ASIX MosUMac ndis-wdm driver".

**(g) Realtek RTL8150, 98 SE to Vista** (`rtl8150/w98se-vista/`, the whole
package as released, as the pack keeps Realtek's other packages)

- Archive: `0001-8150_INST_5_126.zip`, 4,215,806 bytes, SHA-256
  `b8d07d77df1593484c7ebe2d45de6a75a0b937f083769ef4cef674ea4ffaa4ff`;
  one folder `8150_INST_5_126/` holding `WIN98/`, `WINME/`, `WIN2000/`,
  `WINXP/` (x86 and `rtl8150x64.sys`), `WINVISTA/32/`, `WINVISTA/64/`,
  InstallShield setup files, `LICENSE/LICENSE.RTF`, `RELEASE_NOTE/RELEASE.TXT`.
- URL: Realtek's own file server, archived:
  <https://web.archive.org/web/20240219063703/https://www.realtek.com/rtdrivers/cn/nic/0001-8150_INST_5_126.zip>;
  page <https://web.archive.org/web/20210126192000/https://www.realtek.com/en/component/zoo/category/rtl8150l-m-software>
  ("Windows Driver (Support Win98SE/WinME/Win2K/WinXP/Vista) 1.26 2008/04/28").
- Versions: 98/ME `DriverVer` 5.1191.0319.2004; 2000, XP, Vista
  5.126.0411.2008. Ids: `0BDA:8150` only (Linux lists five OEM ids,
  `0411:0012`, `3980:0003`, `07B8:401A`, `1557:8150`, `0586:401A`, which
  no INF matches). The Vista INFs (undecorated `NTx86`, `NTamd64`) would
  also install on 7 (U).
- Licence: `LICENSE.RTF` is InstallShield's placeholder ("Please edit this
  file to add license message"), as in the Realtek packages the pack
  already carries. INF copyright: "Copyright 1996-2008, Realtek
  Semiconductor Corporation".

**(h) Davicom DM9601/DM962x** (`dm96xx/`)

| Folder | Systems | Version, date | Archive (size, SHA-256) | Archived URL | INF ids |
|---|---|---|---|---|---|
| `w98se-2k/` | 98 SE, ME, 2000 (one INF with 9x and NT sections) | 5.0.1.0, 2012-11-21 | `DM9621A_Win982KV5.0.1.0.rar` (20,219, `3b10033f48da265123c49b7cbb7b55d4057d70078947b3a3a7294ca69969f31e`) | `20130528124624/http://www.davicom.com.tw/userfile/24247/DM9621A_Win982KV5.0.1.0.rar` | `0A46:9601`, `9620`, `9621`, `9622`, `0269`, `1269` |
| `wxp/` | XP x86 (DM9601 too) | 1.90, 2002-03-12 ("Logo" driver) | `DM9601_XP_Logo_Driver_587532_1.zip` (19,303, `2b81dfe45d027baa6007277951ecb2de2338acf6a49eae53c268c575c460efc4`); files at the root | `20130528120714/http://www.davicom.com.tw/userfile/24247/DM9601_XP_Logo_Driver_587532_1.zip` | `0A46:9601` |
| `wxp-7/` | XP x86/x64, Vista, 7, 8 (DM962x only) | XP 5.0.2.0709, Vista 5.0.2.0406, 7/8 5.0.3.0703 (2012) | `DM96xx_Driver_V1.0.4.0.zip` (16,475,943, `dcc2552409ef13e92225e49818c01595c6d5897df92a40a9ee8e2ea01854d497`), holding one installer `DM96xx_Driver V1.0.4.0.exe` with per-system folders (`Win2k`, `Winxp`, `XP_x64`, `Vista_x86/x64`, `Win7_x86/x64`, `Win8_x86/x64`, `Win98`) | `20130528120023/http://www.davicom.com.tw/userfile/24247/DM96xx_Driver_V1.0.4.0.zip` | `0A46:9620`, `9621`, `0267`, `0268`, `0269`, `1267`, `1268`, `1269` (2000 folder: `9601`, `9620`, `9621`, `9622`) |

Also found and not proposed: `DM96xxDriver_Win98.rar` (2010, 98/ME,
2.0.100706.02, ids `9601`/`9620`/`9621`/`9622`; superseded on 98/ME by
5.0.1.0), the ME and 2000 "Logo" drivers (2002). The V1.0.4.0 installer
can be unpacked with 7-Zip without running it (as was done here for its
INFs); whether to carry the installer as released, its unpacked folders,
or both is owner question 6. INF copyright: "Copyright 2001, DAVICOM
Semiconductor, Inc." (others carry none).

**(i) Microchip LAN9500 family and LAN7800 family** (`lan9500/`,
`lan7800/`): licence **unclear** beyond "none found in the zip":
Microchip's download pages refused this host, so whether they show terms
is U (owner question 2).

| Folder | Systems | Version | Archive (size, SHA-256) | Archived URL | INF ids |
|---|---|---|---|---|---|
| `lan9500/` (whole package: `ndis51/`, `ndis60/`, `ndis620/`, `ndis630/`, `ndis650/`) | XP x86/x64 2.6.5.0 (2014-07-30), Vista 2.11.0.0 (2016-08-01), 7 2.17.0.0 (2017-04-04); 8 and 10 folders too | 18.12.18.0 | `lan9500-wdf-v18.12.18.0.zip` (13,380,608, `a2728876845a4476a51004119d193faa21cf86206d5ad35ccefc5af684404ee5`) | `20200816184737/http://ww1.microchip.com/downloads/en//softwarelibrary/obj-lan95xx-windows/lan9500-wdf-v18.12.18.0.zip` | `0424:9500`, `9505`, `9530`, `9535`, `9730`, `9735`, `9900`-`9906`, `9E00`, `9E01`, `9E08`, `9E09`, `EC00` |
| `lan7800/` (whole package) | Vista 3.5.2.0 (2015-12-14), 7 3.8.4.0 (2018-08-10); 8 and 10 folders too | 18.12.14.0 | `lan7800-wdf-v18.12.14.0.zip` (10,380,378, `01b50282c8a129cdee586b9c59f0999e9a217b4fe8059a97b4bd1f0a0b59234e`) | `20200816184807/http://ww1.microchip.com/downloads/en//softwarelibrary/obj-lan78xx-windows/lan7800-wdf-v18.12.14.0.zip` | `0424:7800`, `7801`, `780A`, `7850` |

INF copyright: "Copyright (c) SMSC. All rights reserved." (XP) and
"Copyright (c) Microchip Technology Inc. All rights reserved." (Vista on).

### 7.3 NOTICE.md rows, in the pack's style

```
| `ax88772/wxp64/`, `wvista/`, `wvista64/`, `w7/`, `w7_64/`: `ax88772.sys`, `Ax88772.inf`, the catalog, `history.txt` where shipped | ASIX Electronics Corporation | 3.6.3.18 (XP x64), 3.8.3.22 (Vista), 3.10.3.14 (Vista x64), 3.12.3.2 (7), 3.14.3.2 (7 x64) |
| `ax88772/wvista/WdfCoInstaller01007.dll` | Microsoft Corporation, the KMDF 1.7 co-installer, as ASIX ships it with that driver | 1.7.6001.0 (U: the version resource of this copy not read; the pack's other copy is 1.7.6001.0) |
| `ax88772/w7/WdfCoInstaller01009.dll`, `ax88772/w7_64/WdfCoInstaller01009.dll` | Microsoft Corporation, the KMDF 1.9 co-installer, as ASIX ships it with that driver | 1.9.7600.16385 (U, as above) |
| All files in `ax88772b/w2k-xp/`, `wxp64/`, `wvista/`, `wvista64/`, `w7/`, `w7_64/` | ASIX Electronics Corporation (`WdfCoInstaller01007.dll`: Microsoft Corporation, as ASIX ships it) | 5.4.3.0, 5.6.3.0, 5.8.2.0, 5.10.2.0, 5.12.12.0, 5.14.12.0 |
| All files in `ax88178/w98se-me/`, `w2k-xp/`, `wxp64/`, `wvista/`, `wvista64/`, `w7/`, `w7_64/` | ASIX Electronics Corporation (KMDF co-installers: Microsoft Corporation, as ASIX ships them) | 1.0.3.46, 1.4.3.64, 1.6.3.64, 1.8.3.14, 1.10.3.14, 1.12.6.0, 1.14.6.0 |
| All files in `ax88172/w98se-me/`, `w2k/`, `wxp/` | ASIX Electronics Corporation | 1.0.0.11, 2.3.0.2, 2.0.0.5 |
| All files in `ax88172a/w2k-xp/`, `wxp64/`, `wvista/`, `wvista64/`, `w7/`, `w7_64/` | ASIX Electronics Corporation (KMDF co-installers: Microsoft Corporation, as ASIX ships them) | 1.4.0.6, 1.6.0.6, 1.8.0.5, 1.10.0.5, 1.12.0.2, 1.14.0.2 |
| All files in `mcs7830/w98se-xp/`, `wxp64/`, `wvista/`, `wvista64/`, `w7/`, `w7_64/` | ASIX Electronics Corporation (MosChip Semiconductor's line; `WdfCoInstaller01009.dll`: Microsoft Corporation) | 1.49.0.0, 1.49.0.0, 1.0.0.18, 1.0.0.18, 1.0.0.17, 1.0.0.17 |
| All files in `rtl8150/w98se-vista/` | Realtek Semiconductor Corp.; its setup engine is Flexera's InstallShield, as Realtek ships it | 5.126 (`NET8150.INF` `DriverVer` 04/11/2008,5.126.0411.2008; 98/ME 5.1191.0319.2004) |
| All files in `dm96xx/w98se-2k/`, `wxp/`, `wxp-7/` | DAVICOM Semiconductor, Inc. (`WdfCoInstaller01009.dll`, `DPInst*.exe`: Microsoft Corporation, as Davicom ships them) | 5.0.1.0; 1.90; V1.0.4.0 (5.0.2.0709 / 5.0.2.0406 / 5.0.3.0703) |
| All files in `lan9500/`, `lan7800/` | Microchip Technology Inc. (SMSC for the NDIS 5.1 build; `WdfCoInstaller01009.dll`: Microsoft Corporation) | 18.12.18.0; 18.12.14.0 |
```

The pack counts files per folder ("All 4 files in ..."); the worker
replaces "All files" with the counts when copying. Each needs its provenance block (vendor, package, original and archived
URL, obtained date, archive name/size/SHA-256, "Licence: none found" with
the INF's copyright line, modification: none), copied from the tables
above.

### 7.4 STAGE.TXT lines, in its format

One INF per hardware id per system. Davicom's and Microchip's lines are
proposals pending owner questions 2 and 6.

```
# ---- Ethernet: ASIX AX88772 / AX88772A, XP x64 to 7
STAGE | WXP64            | non-free/ethernet/ax88772/wxp64/Ax88772.inf    | 3.6.3.18
STAGE | WVISTA           | non-free/ethernet/ax88772/wvista/Ax88772.inf   | 3.8.3.22, KMDF 1.7; its undecorated [Models] would also match on 7: there w7 wins
STAGE | WVISTA64         | non-free/ethernet/ax88772/wvista64/Ax88772.inf | 3.10.3.14; on 7 x64 w7_64 wins
STAGE | W7               | non-free/ethernet/ax88772/w7/Ax88772.inf       | 3.12.3.2, KMDF 1.9
STAGE | W7X64            | non-free/ethernet/ax88772/w7_64/Ax88772.inf    | 3.14.3.2
# ---- Ethernet: ASIX AX88772B / AX88772C
STAGE | W2K WXP          | non-free/ethernet/ax88772b/w2k-xp/Ax88772b.inf   | 5.4.3.0, KMDF 1.7
STAGE | WXP64            | non-free/ethernet/ax88772b/wxp64/Ax88772b.inf    | 5.6.3.0
STAGE | WVISTA           | non-free/ethernet/ax88772b/wvista/Ax88772b.inf   | 5.8.2.0; its .6.1 sections would match on 7: there w7 wins
STAGE | WVISTA64         | non-free/ethernet/ax88772b/wvista64/Ax88772b.inf | 5.10.2.0
STAGE | W7               | non-free/ethernet/ax88772b/w7/Ax88772b.inf       | 5.12.12.0 (2015), also 7E2B and D-Link 2001:1A02
STAGE | W7X64            | non-free/ethernet/ax88772b/w7_64/Ax88772b.inf    | 5.14.12.0
# ---- Ethernet: ASIX AX88178
STAGE | W98SE WME        | non-free/ethernet/ax88178/w98se-me/Ax88178.inf | 1.0.3.46
STAGE | W2K WXP          | non-free/ethernet/ax88178/w2k-xp/Ax88178.inf   | 1.4.3.64; its .6.0/.6.1 sections would match on Vista and 7: there wvista and w7 win
STAGE | WXP64            | non-free/ethernet/ax88178/wxp64/Ax88178.inf    | 1.6.3.64
STAGE | WVISTA           | non-free/ethernet/ax88178/wvista/Ax88178.inf   | 1.8.3.14, KMDF 1.7
STAGE | WVISTA64         | non-free/ethernet/ax88178/wvista64/Ax88178.inf | 1.10.3.14
STAGE | W7               | non-free/ethernet/ax88178/w7/Ax88178.inf       | 1.12.6.0, KMDF 1.9
STAGE | W7X64            | non-free/ethernet/ax88178/w7_64/Ax88178.inf    | 1.14.6.0
# ---- Ethernet: ASIX AX88172
STAGE | W98SE WME        | non-free/ethernet/ax88172/w98se-me/AX88172.INF | 1.0.0.11; Signature reads $Windows NT$ (owner to confirm 98 SE accepts it)
STAGE | W2K              | non-free/ethernet/ax88172/w2k/ax88172.inf      | 2.3.0.2
STAGE | WXP              | non-free/ethernet/ax88172/wxp/AX88172.INF      | 2.0.0.5
# ---- Ethernet: ASIX AX88172A
STAGE | W2K WXP          | non-free/ethernet/ax88172a/w2k-xp/AX88172A.inf   | 1.4.0.6
STAGE | WXP64            | non-free/ethernet/ax88172a/wxp64/AX88172A.inf    | 1.6.0.6
STAGE | WVISTA           | non-free/ethernet/ax88172a/wvista/AX88172A.inf   | 1.8.0.5
STAGE | WVISTA64         | non-free/ethernet/ax88172a/wvista64/AX88172A.inf | 1.10.0.5
STAGE | W7               | non-free/ethernet/ax88172a/w7/AX88172A.inf       | 1.12.0.2
STAGE | W7X64            | non-free/ethernet/ax88172a/w7_64/AX88172A.inf    | 1.14.0.2
# ---- Ethernet: MosChip/ASIX MCS7830 / MCS7832 / MCS7730
STAGE | W98SE WME W2K WXP | non-free/ethernet/mcs7830/w98se-xp/netmosu.inf | 1.49.0.0; $CHICAGO$ INF, 9x and NT sections
STAGE | WXP64            | non-free/ethernet/mcs7830/wxp64/USBMAC64.inf    | 1.49.0.0
STAGE | WVISTA           | non-free/ethernet/mcs7830/wvista/netmosu.inf    | 1.0.0.18, KMDF 1.9
STAGE | WVISTA64         | non-free/ethernet/mcs7830/wvista64/USBMAC64.inf | 1.0.0.18
STAGE | W7               | non-free/ethernet/mcs7830/w7/netmosu.inf        | 1.0.0.17
STAGE | W7X64            | non-free/ethernet/mcs7830/w7_64/USBMAC64.inf    | 1.0.0.17
# ---- Ethernet: Realtek RTL8150
STAGE | W98SE            | non-free/ethernet/rtl8150/w98se-vista/WIN98/NET8150.INF        | 5.1191 (2004)
STAGE | WME              | non-free/ethernet/rtl8150/w98se-vista/WINME/NET8150.INF        | 5.1191 (2004)
STAGE | W2K              | non-free/ethernet/rtl8150/w98se-vista/WIN2000/NET8150.INF      | 5.126 (2008)
STAGE | WXP WXP64        | non-free/ethernet/rtl8150/w98se-vista/WINXP/NET8150.INF        | 5.126; x86 and x64 files in one folder
STAGE | WVISTA           | non-free/ethernet/rtl8150/w98se-vista/WINVISTA/32/NET8150.INF  | 5.126
STAGE | WVISTA64         | non-free/ethernet/rtl8150/w98se-vista/WINVISTA/64/NET8150.INF  | 5.126; Realtek lists no 7 driver, and this INF's undecorated NTamd64 would install on 7 x64 (owner to confirm adding W7X64, and W7 on the 32/ line)
# ---- Ethernet: Davicom DM9601 / DM962x (owner to confirm the layout)
STAGE | W98SE WME W2K    | non-free/ethernet/dm96xx/w98se-2k/DM9USB.inf | 5.0.1.0
STAGE | WXP              | non-free/ethernet/dm96xx/wxp/Dm9usb.inf      | 1.90 (2002), DM9601 only; the DM962x ids are in wxp-7 (no clash)
STAGE | WXP              | non-free/ethernet/dm96xx/wxp-7/Winxp/DM9USB.inf     | 5.0.2.0709, DM962x
STAGE | WXP64            | non-free/ethernet/dm96xx/wxp-7/XP_x64/DM9USB.inf    | 5.0.2.0709
STAGE | WVISTA           | non-free/ethernet/dm96xx/wxp-7/Vista_x86/DM9USB.inf | 5.0.2.0406
STAGE | WVISTA64         | non-free/ethernet/dm96xx/wxp-7/Vista_x64/DM9USB.inf | 5.0.2.0406
STAGE | W7               | non-free/ethernet/dm96xx/wxp-7/Win7_x86/DM9USB.inf  | 5.0.3.0703
STAGE | W7X64            | non-free/ethernet/dm96xx/wxp-7/Win7_x64/DM9USB.inf  | 5.0.3.0703
# ---- Ethernet: Microchip LAN9500 / LAN7800 (owner to confirm the licence)
STAGE | WXP              | non-free/ethernet/lan9500/ndis51/x86/net9500-x86-n51f.inf  | 2.6.5.0
STAGE | WXP64            | non-free/ethernet/lan9500/ndis51/x64/net9500-x64-n51f.inf  | 2.6.5.0
STAGE | WVISTA           | non-free/ethernet/lan9500/ndis60/x86/net9500-x86-n60f.inf  | 2.11.0.0
STAGE | WVISTA64         | non-free/ethernet/lan9500/ndis60/x64/net9500-x64-n60f.inf  | 2.11.0.0
STAGE | W7               | non-free/ethernet/lan9500/ndis620/x86/net9500-x86-n620f.inf | 2.17.0.0
STAGE | W7X64            | non-free/ethernet/lan9500/ndis620/x64/net9500-x64-n620f.inf | 2.17.0.0
STAGE | WVISTA           | non-free/ethernet/lan7800/ndis60/x86/net7800-x86-n60f.inf  | 3.5.2.0
STAGE | WVISTA64         | non-free/ethernet/lan7800/ndis60/x64/net7800-x64-n60f.inf  | 3.5.2.0
STAGE | W7               | non-free/ethernet/lan7800/ndis620/x86/net7800-x86-n620f.inf | 3.8.4.0
STAGE | W7X64            | non-free/ethernet/lan7800/ndis620/x64/net7800-x64-n620f.inf | 3.8.4.0
```

The packages' other INFs (8,
10, the ARM builds, Davicom's `Win2k`/`Win98`/`Win8_*` folders) are
`COPY` lines. The worker must check the "one INF per id per system" rule
by reading each INF's `[Models]` for each system, as the pack's existing
lines do, and against `usb-wdm-free`'s `STAGE.TXT` `EXCLUDES` rules.

## 8. `ax88179` in `usb-wdm-free` (not changed here)

On branch `relicense` (0.1.3): released since 0.1.1 (owner, 2026-10-25),
now at driver version 0.1.2.0 in `free/net/ax88179/w98se-me/`
(`AX179F98.INF`, `AX179F.SYS`, staged on 98 SE and ME) and
`free/net/ax88179/w2k/` (`AX179F.INF`, copy-only). Runs: the owner's real
AX88179 through VM passthrough, 98 SE under xHCI98 and under NUSB's EHCI,
ME under xHCI98 (install, DHCP, ping, removal under traffic PASS;
`docs/status.md`). Left before it is called ready on all its systems: a
2000 leg (built, never run), an ME leg under NUSB's EHCI, and the run on a
real PC rather than a VM (AGENTS.md, "Standalone": both host stacks, 98 SE
minimum). Nothing is needed for 3.3.0.0 beyond what 0.1.3 already ships.

## 9. Out of scope (with reason)

| Linux file | Why out |
|---|---|
| `qmi_wwan.c`, `cdc_mbim.c`, `huawei_cdc_ncm.c`, `sierra_net.c`, `hso.c`, `kalmia.c`, `lg-vl600.c`, `cx82310_eth.c`, `cdc-phonet.c` | mobile-broadband modems, ADSL routers, phone links |
| `ipheth.c` | iPhone tethering (Apple vendor class; pairing) |
| `net1080.c`, `gl620a.c`, `plusb.c`, most of `cdc_subset.c` | PC-to-PC link cables (`cdc_subset.c` also carries 2000-era PDAs and boot loaders; its Linux gadget id `0525:A4A2` is `cdcnet`'s) |
| `zaurus.c` | Sharp Zaurus and similar PDAs |
| `int51x1.c` | powerline adapter, listed in the tables as "other"; not searched |

## 10. Owner questions

1. The ASIX, MosChip, Davicom and Realtek (RTL8150) packages carry no
   licence text, as the ASIX and Realtek drivers already in the pack. May
   they be added on the same basis?
2. Microchip's packages: no licence file inside; Microchip's download
   pages refused this host, so any click-through terms are unread. Add,
   or read the page first (or mark "Not shipped")?
3. Which adapters does the owner have besides the AX88179? (None,
   2026-10-27: every row here is untested, as the pack says of all its
   drivers.)
4. ASIX's Windows 8 package (3.18.1.0) names the AX88179A, AX88772D and
   AX88279 and its INF has no version decoration: try it on 7 x64, or
   leave those chips to `cdcnet`'s NCM?
5. AX88172 98/ME (1.0.0.11): its INF's `Signature` is `$Windows NT$`.
   Carry it as released and mark the 98 SE leg unknown?
6. Davicom V1.0.4.0 is one installer: carry the `.exe` as released (with
   `COPY` lines only), its 7-Zip-unpacked folders (as the pack did with
   Realtek's 7 self-extractor), or both?
7. Older builds found but newer ones listed and not archived (AX88772 XP
   x64 3.6.3.38, Vista 3.8.3.23 / 3.10.3.23): accept the older archived
   builds?
8. RTL8150 on 7: Realtek lists no 7 driver, but its Vista INFs (undecorated
   `NTx86`/`NTamd64`) would install there. Stage them on 7 too?

## 11. Sources

- ASIX: live product pages
  `https://www.asix.com.tw/en/product/USBEthernet/<line>/<chip>` (AX88772C,
  AX88772E, AX88172A, AX88179A, AX88179B, AX88279, AX88279A) and
  `https://www.asix.com.tw/en/support/download/step2/11/2/3` (AX88179),
  read 2026-10-27; the Internet Archive's index of
  `asix.com.tw/FrootAttach/driver/` and of
  `asix.com.tw/download.php?sub=driverdetail&PItemID=` (85, 86, 84, 96,
  97, 103, 105, 108, 109).
- Realtek: `https://www.realtek.com/Download/ListAllDownloadItem?cate_id=585`
  (live JSON); RTL8150 page and package as cited in 7.2 (g).
- Davicom: the Internet Archive's index of `davicom.com.tw/userfile/24247/`.
- Microchip: the Internet Archive's copies under
  `ww1.microchip.com/downloads/en//softwarelibrary/`.
- `usb-wdm-nonfree` v1.0.0 (`README.md`, `NOTICE.md`, `STAGE.TXT`, its
  Realtek and ASIX INFs), read only.
- Linux `v7.3` `drivers/net/usb/` (facts only).
- Windows install media on the survey host, as section 0.

# USB Wi-Fi coverage: chips x Windows

Status: survey, 2026-10-27 (task 44.71 part 1, for xHCI98 3.3.0.0). No
code. Like `ethernet-coverage.md` for USB Ethernet (branch
`lan-survey`), it lists, per USB Wi-Fi chip family and Windows version, what
already serves it and what the chip makers' drivers could add to
`usb-wdm-nonfree`. It also lists the gaps that stay uncovered. No free
Wi-Fi driver is planned. A separate worker adds the drivers to
`usb-wdm-nonfree` once the owner has answered section 10. This survey
commits nothing there.

## 0. How this was checked

- **Linux** (facts only: chip names, USB ids, which file serves which
  chip): `drivers/net/wireless/` at tag `v7.3` (commit
  `c03c1a14507dfaf8b856c2608f9ac216021fc222`, 2026-10-18), sparse clone,
  read only. `rndis_wlan.c` is no longer in `v7.3`. Its id table was read
  at tag `v6.6` (`drivers/net/wireless/legacy/rndis_wlan.c`).
- **In-box Windows drivers**: the network-class INFs of each system,
  read from the same install-media extracts as the Ethernet survey, and
  listed for any USB id: 2000 SP4, XP SP3 x86, Vista SP2 x86 and x64, 7
  SP1 x86 and x64, 8 x86 (RTM) and 8.1 x64 with Update. 98 SE and ME:
  the Ethernet survey's reading of their network INFs (no USB id but
  ME's RNDIS `0565:0041`) is reused. XP x64 was not read (U).
- **Makers' drivers**: the chip makers' own file servers first (Realtek
  `rtdrivers/cn/wlan/`, Ralink `ralinktech.com.tw/data/drivers/`,
  MediaTek's download CDN, ZyDAS `web_driver/`), as archived by the
  Internet Archive, then dongle brands' official pages (TP-Link live,
  Linksys archived) where the chip maker offers nothing. Each archive was
  fetched into a scratch folder outside every repository and unpacked
  with 7-Zip. InstallShield cabinets were opened with unshield 1.6.2. Two
  InstallShield setup programs whose payload 7-Zip cannot open were split
  by a short Python script (run with `-I`) that only reads the file table
  at the end of the program. No installer or EXE was run. Only INFs,
  readmes, release notes, `.ini` text and licence files were read. No
  driver or program binary was read.
- **Not reachable or not fetched**: Realtek's live WLAN pages (they
  redirect), Realtek's `agree-to-download` file links (the file is sent
  by e-mail), MediaTek's current product pages (read as archived only),
  ZyDAS's ZD1211B packages (only 404 pages archived), Linksys's
  WUSB54GS driver (only a redirect archived), and Atheros's own site
  (no USB Wi-Fi download found). Facts that rest on those are marked U.
- V = read in the file or page named. U = unverified, with what would
  settle it. No legal conclusion is drawn anywhere. "Licence: none found"
  means no licence text was found in the package or on its download
  page. Where a licence text was found, it is quoted and nothing more.

## 1. Summary

Rows are chip families. "Linux" is the Linux file that drives them.

| Chip family (Linux) | 98 SE | ME | 2000 | XP | XP x64 | Vista x86/x64 | 7 x86/x64 | 8 / 8.1 |
|---|---|---|---|---|---|---|---|---|
| Realtek RTL8187L (`rtl8187`) | step 2 (a) | step 2 (a) | step 2 (a) | step 2 (a) | step 2 (a) | step 2 (a) | step 2 (a, 7 build) | - |
| Realtek RTL8187B (`rtl8187`) | **gap** | **gap** | **gap** | step 2 (b) | step 2 (b) | step 2 (b) | in box, and step 2 (b, newer) | in box |
| Realtek RTL8188CU, RTL8192CU (`rtl8192cu`, `rtl8xxxu`) | **gap** | **gap** | **gap** | step 2 (c) | step 2 (c) | step 2 (c) | step 2 (c) | in box |
| Realtek RTL8192DU (`rtl8192du`) | **gap** | **gap** | **gap** | step 2 (c) | step 2 (c) | step 2 (c) | step 2 (c) | in box 8.1 |
| Realtek RTL8723AU (`rtl8xxxu`) | **gap** | **gap** | **gap** | step 2 (c) | step 2 (c) | step 2 (c) | step 2 (c) | in box 8.1 |
| Realtek RTL8188SU, RTL8191SU, RTL8192SU (none in `v7.3`) | **gap** | **gap** | step 2 (f) | step 2 (f) | step 2 (f) | step 2 (f) | step 2 (f) | in box |
| Realtek RTL8188EU (`rtl8xxxu`) | **gap** | **gap** | **gap** | step 2 (d, e) | step 2 (d, e) | step 2 (d, e) | step 2 (d, e) | in box 8.1 (3 ids) |
| Realtek RTL8188FU, RTL8192EU, RTL8723BU, RTL8710BU (`rtl8xxxu`) | **gap** | **gap** | **gap** | step 2 (d) | step 2 (d) | step 2 (d) | step 2 (d) | Realtek 8+ |
| Realtek RTL8192FU (`rtl8xxxu`) | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | U |
| Realtek RTL8723DU (`rtw88`) | **gap** | **gap** | **gap** | step 2 (d) | step 2 (d) | step 2 (d) | step 2 (d) | Realtek 8+ |
| Realtek RTL8811AU, RTL8821AU, RTL8812AU, RTL8814AU (`rtw88`) | **gap** | **gap** | **gap** | step 2 (d, e) | step 2 (d, e) | step 2 (d, e) | step 2 (d, e) | in box 8.1 (8812AU, some 8811AU) |
| Realtek RTL8811CU, RTL8821CU (`rtw88`) | **gap** | **gap** | **gap** | step 2 (d, e) | step 2 (d, e) | step 2 (d, e) | step 2 (d, e) | Realtek 8+ |
| Realtek RTL8812BU, RTL8822BU (`rtw88`) | **gap** | **gap** | **gap** | step 2 (d, e) | step 2 (d, e) | step 2 (d, e) | step 2 (d, e) | Realtek 8+ |
| Realtek RTL8822CU (`rtw88`) | **gap** | **gap** | **gap** | 1 Linksys id in (e) | same | same | same | U |
| Realtek RTL8851BU, RTL8852AU/8832AU, RTL8852BU/8832BU, RTL8852CU/8832CU, RTL8922AU (`rtw89`) | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | brands: 10 and 11 |
| Ralink RT2500USB / RT2570 (`rt2500usb`) | step 2 (g) | step 2 (g) | step 2 (g) | step 2 (g) | step 2 (g) | step 2 (g) | U (Vista INF, undecorated) | - |
| Ralink RT2501USB / RT73 family (`rt73usb`) | step 2 (g) | step 2 (g) | step 2 (g) | step 2 (g) | step 2 (g) | step 2 (g) | in box (57 of 75 ids) | in box |
| Ralink/MediaTek RT2870, RT3070, RT3072, RT3370, RT3572, RT5370, RT5372, RT5572 (`rt2800usb`) | **gap** | **gap** | candidate (h, U on 2000) | candidate (h) | candidate (h) | candidate (h) | in box (110 ids), candidate (h) | in box |
| MediaTek MT7601U (`mt7601u`) | **gap** | **gap** | candidate (h, U) | candidate (h) | candidate (h) | candidate (h) | candidate (h) | in box 8.1 (2 ids) |
| MediaTek MT7610U (`mt76x0u`), MT7612U (`mt76x2u`) | **gap** | **gap** | candidate (h, U) | candidate (h) | candidate (h) | candidate (h) | candidate (h) | in box 8.1 (MT7610U, 11 ids) |
| MediaTek MT7921AU, MT7925U, MT7663U (`mt76`) | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | U |
| Atheros AR9271, AR7010 (`ath9k_htc`) | **gap** | **gap** | **gap** | step 2 (i) | step 2 (i) | step 2 (i) | step 2 (i) | step 2 (i) |
| Atheros AR9170 (`carl9170`) | **gap** | **gap** | step 2 (j) | step 2 (j) | step 2 (j) | step 2 (j) | step 2 (j) | - |
| ZyDAS ZD1211 (`zd1211rw`) | step 2 (k) | step 2 (k) | step 2 (k) | step 2 (k) | step 2 (k) | **gap** | **gap** | - |
| ZyDAS ZD1211B (`zd1211rw`) | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | - |
| Broadcom BCM4320, RNDIS (`rndis_wlan`, gone in `v7.3`) | U | U | U | U | U | U | U | - |
| Broadcom BCM43235 (Linksys AE2500 only, `brcmfmac`) | **gap** | **gap** | **gap** | step 2 (l) | step 2 (l) | step 2 (l) | step 2 (l) | U |
| Broadcom BCM43143, BCM43236, BCM43242, BCM43569, Cypress 4373 (`brcmfmac`) | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | **gap** | U |
| Intel | no USB Wi-Fi part in Linux | | | | | | | |

"step 2": a maker's package this survey found and proposes for
`usb-wdm-nonfree` (section 8). The letter names the package. "candidate":
found, but its licence text limits distribution (owner question 2).
"in box": Windows' own INF (section 2). "gap": nothing found. It stays
uncovered (section 7). `usb-wdm-nonfree` carries no Wi-Fi driver today,
and `usb-wdm-free` has none planned.

## 2. What Windows has in the box (V unless marked)

| System | USB Wi-Fi INFs (network class, USB ids) |
|---|---|
| 98 SE, ME | none |
| 2000 SP4 | none |
| XP SP3 x86 | none |
| XP x64 | not read (U) |
| Vista SP2 x86, x64 | none |
| 7 SP1 x86, x64 | `net8187bv32.inf` / `net8187bv64.inf` 6.1159.0323.2009 (Realtek RTL8187B: `0BDA:8187`, `8189`, `8197`, `8198`, `8199`, all `REV_0200`); `netr73.inf` / `netr7364.inf` 4.00.00.0015 (Ralink RT73 family, 57 ids); `netr28u.inf` / `netr28ux.inf` 3.00.00.0061 (Ralink RT2870 family, 110 ids) |
| 8 x86 RTM | 7's three (RT73 at 4.00.10.0000, 62 ids; RT2870 at 4.0.0.201, 226 ids), plus `net8192su32.inf` (RTL8188SU/8191SU/8192SU, 88 ids) and `netrtwlanu.inf` 1015.4.711.2012 (RTL8188CU/8192CU, 62 ids) |
| 8.1 x64 Update | 8's, with `netr28ux.inf` 5.01.02.0000 (285 ids, adding MT7601U and MT7610U ids), `netrtwlanu.inf` 1025.1.0423.2013 (139 ids, adding RTL8188EU, RTL8192EU, RTL8723AU, RTL8812AU and RTL8811AU ids), and `mwlu97w8x64.inf` (Marvell, Surface only) |

So on 98 SE to Vista no USB Wi-Fi adapter has an in-box driver. On 7,
three families do (RTL8187B, RT73, RT2870).

## 3. Per family: Linux, makers' drivers, systems

"Linux ids" is the number of distinct ids in the Linux table. "INF ids"
is how many of them the maker's INF lists (section 4 has the files).

### 3.1 Realtek

| Chip | Linux | Linux ids | Maker's package found (systems, version) | INF ids |
|---|---|---|---|---|
| RTL8187L | `rtl818x/rtl8187/dev.c` | 13 | (a) Realtek `8187_1313.0613`: 98 SE, ME (5.1313.0613.2008), 2000 and XP (same), XP x64 (5.1312.0416.2008), Vista x86/x64 (6.1313.0613.2008). (a) Realtek `RTL8187L_Win7`: 7 x86/x64 (6.1316.1209.2009) | 8 (98 SE to Vista), 9 (7) |
| RTL8187B | same file | 8 | (b) Realtek `RTL8187B_Auto_Install_Program`: XP (5.1162.0610.2009), XP x64 (5.1160.0415.2009), Vista (6.1162.0610.2009), 7 (62.1181.1105.2009) | 1 (XP, Vista), 3 (7) |
| RTL8188CU, RTL8192CU | `rtlwifi/rtl8192cu/sw.c`, `rtl8xxxu` | 82 | (c) Realtek `RTL819xCU_AutoInstallPackage`, 1021.3.0912.2012: XP, XP x64, Vista, 7, 8 | 65 (XP), 67 (7) |
| RTL8192DU | `rtlwifi/rtl8192du/sw.c` | 19 | (c) | 16 |
| RTL8723AU | `rtl8xxxu` | 3 | (c) | 2 |
| RTL8188SU, RTL8191SU, RTL8192SU | none in `v7.3` | - | (f) Realtek `RTL8192SU_AutoInstallPackage`: 2000, XP, XP x64 (1084.53.0809.2011), Vista (1084.53), 7 (1086.48.0809.2011) | 76 to 78 ids, Realtek `0BDA:8171`-`8175`, `8192`, `8712`, `C047` and OEMs |
| RTL8188EU | `rtl8xxxu` | 15 | (d) Realtek 1030.25.0701.2017 (`DriverVer` 2017-12-25): XP, XP x64, Vista, 7, 8, 8.1, 10. (e) Realtek 1030.17.1101.2016 (`DriverVer` 2017-04-06): the same systems | 2 (d), 10 (e) |
| RTL8188FU | `rtl8xxxu` | 1 | (d) | 1 |
| RTL8192EU | `rtl8xxxu` | 11 | (d), (e) | 2 (d), 6 (e) |
| RTL8723BU | `rtl8xxxu` | 2 | (d) | 1 |
| RTL8710BU | `rtl8xxxu` | 2 | (d) | 2 |
| RTL8192FU | `rtl8xxxu` | 6 | none found | 0 |
| RTL8723DU | `rtw88/rtw8723du.c` | 2 | (d) | 1 |
| RTL8812AU | `rtw88/rtw8812au.c` | 34 | (d), (e) | 5 (d), 31 (e) |
| RTL8811AU, RTL8821AU | `rtw88/rtw8821au.c` | 26 | (d), (e) | 4 (d), 17 (e) |
| RTL8814AU | `rtw88/rtw8814au.c` | 14 | (d), (e) | 4 (d), 11 (e) |
| RTL8811CU, RTL8821CU | `rtw88/rtw8821cu.c` | 15 | (d), (e) | 4 (d), 2 (e) |
| RTL8812BU, RTL8822BU | `rtw88/rtw8822bu.c` | 36 | (d), (e) | 7 (d), 10 (e) |
| RTL8822CU | `rtw88/rtw8822cu.c` | 7 | (e) lists one of them, Linksys `13B1:0043` | 1 |
| RTL8851BU, RTL8852AU/8832AU, RTL8852BU/8832BU, RTL8852CU/8832CU, RTL8922AU | `rtw89/rtw88*u.c` | 6, 14, 15, 12, 15 | none for 98 SE to 7. TP-Link's Archer TX20U and TX20U Nano pages offer Windows 10 and 11 only (V; the chip is not named on those pages, U) | - |

Realtek's own current pages for RTL8192EU, RTL8811AU, RTL8811CU,
RTL8812AU, RTL8812BU and RTL8814AU (archived 2023) list one package,
"32bit/64bit Windows7, Windows8.1, Windows10 WLAN USB driver (Install
Package) 1030.25.0701.2017, 2018/09/07, 50 MB". The page names its chips
as "RTL8812AU/RTL8811AU/RTL8192EU/RTL8812BU/RTL8811CU/RTL8814AU" (V).
The package itself also holds `WinXP`, `WinX64` and `VistaX86/X64`
folders, and its release note reads "support WinXP ~ Win10 OS" (V,
`Release notes/Drv_Release.txt`). So (d) is offered by Realtek for 7
and later, and carries XP and Vista builds as well (owner question 6).

(e) holds more brand ids than (d) (108 ids against 39) for the same chips
and the same systems. The two overlap on most Realtek ids (owner
question 5).

Brand pages read for the same chips (TP-Link, live, V):

- TL-WN725N v1: "WinXP/Vista/7".
- TL-WN725N v2: "WinXP/ WinVista/Win7/Win8/Win8.1/Win10/Win11".
- TL-WN823N v1: "Vista/XP/7/8/8.1". TL-WN823N v2: "WinXP/Win7/Win8/Win8.1/Win10/Win11".
- TL-WN722N v2: "WinXP/Win7/Win8/Win8.1/Win10".
- Archer T4U v1: "Win11/10/8.1/8/7/XP". Archer T2U Nano v1: "WinXP 32/64bits".
- Archer T3U v1: "win10, win11" only.

No brand page read offers 98 SE, ME or 2000 for any Realtek 802.11n or
later chip.
The chip inside each model is not named on those pages (U). They were
not downloaded, as Realtek's own (c) and (d) serve the same systems.

### 3.2 Ralink and MediaTek

| Chip | Linux | Linux ids | Maker's package found (systems, version) | INF ids |
|---|---|---|---|---|
| RT2500USB (RT2570) | `ralink/rt2x00/rt2500usb.c` | 31 | (g) Ralink 2007 package `IS_AP_STA_7x_..._062807_0.1.0.23.exe`: 98 SE and ME (`RT2500USB_Driver_9x`), 2000 and XP (`_XP2k`), XP x64, all 2.01.01.0000 (2006-11-08); Vista x86/x64 3.00.02.0000 (2006-12-27). Also the 2005 package `IS_STA_2500USB_..._1.0.3.0.exe` (98 SE, ME, 2000, XP, 2.00.03.0000) and `RT2500USBV2.0.3.0x64.zip` (XP x64), both older | 21 |
| RT2501USB, RT2571W, RT2573, RT2671 (RT73) | `ralink/rt2x00/rt73usb.c` | 75 | (g): 98 SE and ME (`RT73_Driver_9x`), 2000 and XP, XP x64, all 1.02.01.0000 (2007-05-14); Vista x86/x64 3.00.06.0000 (2007-06-26). 7: in box | 29 (98 SE to XP x64), 31 (Vista) |
| RT2770, RT2870 | `ralink/rt2x00/rt2800usb.c` | 334 (whole RT2800 USB family) | (g) also holds an early RT2870 build: 2000 and XP, XP x64 1.00.03.0000 (2007-05-09), Vista 1.00.04.0000, `148F:2770`, `2870` only | 16 to 17 |
| RT2870, RT3070, RT3071, RT3072, RT3370, RT3572, RT3573, RT5370, RT5372, RT5572 | same file | 334 | (h) MediaTek `IS_Setup_ICS_011916_1.5.39.173` (one setup program): 2000 and XP (`RT2870_Driver_XP2k`), XP x64, Vista x86/x64, all `RT2870.inf` 5.01.25.0 (2015-10-28); 7, 8, 8.1, 10 `netr28u(x).inf` 5.01.25.0 | 272 |
| MT7601U | `mediatek/mt7601u/usb.c` | 17 | (h) | 13 |
| MT7610U | `mediatek/mt76/mt76x0/usb.c` | 25 | (h) | 19 |
| MT7612U | `mediatek/mt76/mt76x2/usb.c` | 16 | (h) (it carries `FW_7662.bin`) | 3 |
| MT7921AU, MT7925U, MT7663U | `mediatek/mt76/mt7921`, `mt7925`, `mt7615/usb.c` | 6, 4, 2 | none found for 98 SE to 7 (U: MediaTek's pages for these were not read) | - |

MediaTek's archived pages for RT2070, RT2770, RT2870, RT3572 and RT5372
(2021, V) say: "Please contact your device manufacturer website for
product driver downloads. The driver downloads offered below for the
RT2870 are designated for business partner use. By downloading MediaTek
product drivers you acknowledge they are provided without warranty and
MediaTek does not provide direct support to end-users." The MT7610U and
MT7612U pages link (h)'s package and a Mac package. MediaTek's other
archived files found: `RT2770_2870_RT307x.zip` (holding the older
`IS_Setup_ICS_102615_1.5.39.161.exe` and an OmniPeek capture driver),
`RTUSB_D2870-4.2.9.2_UI-4.0.9.6_2013_11_29.zip` and
`MT7612_7610U_D5.0.1.25_SDK1.0.2.18_UI5.0.0.27_20151209.zip` (both Mac
`.dmg` only). Not proposed.

### 3.3 Atheros, ZyDAS, Broadcom and others

| Chip | Linux | Linux ids | Maker's package found (systems, version) | INF ids |
|---|---|---|---|---|
| AR9271, AR7010 | `ath/ath9k/hif_usb.c` | 27 | (i) TP-Link `TL-WN721N_V1_140915` (TL-WN721N v1; the TL-WN722N v1 package `131113` has the same XP to 7 builds): XP and XP x64 `netathuw(x).inf` 7.7.0.102 (2011-07-28), Vista 8.0.0.84 (2011-04-20), 7 2.0.0.62 (2011-04-20), 8 10.0.0.25, 8.1 10.0.0.28. No 2000 build | 4 (XP to 7: `0CF3:9271`, `1006`, `7010`, `7015`), 16 (8, 8.1) |
| AR9170 | `ath/carl9170/usb.c` | 28 | (j) TP-Link `TL-WN821N_WN822N_100629` (TL-WN821N v2, TL-WN822N v1): 2000 (`Windows 2000`, `arusb_2k.sys`), XP and XP x64 `arusb_xp.inf` 3.0.0.165 (2010-02-25), Vista 3.1.0.131 (2010-03-01), 7 3.5.0.12 (2009-11-26) | 10 |
| ZD1211 | `zydas/zd1211rw/zd_usb.c` | 26 | (k) ZyDAS `ZD1211USB_Install_4_13_0_0_ALL`: 98 SE, ME, 2000, XP, XP x64, 4.13.0.0 (2006-02-27). One setup program | 12, and 3 ids Linux lacks |
| ZD1211B | same file | 30 | ZyDAS listed `ZD1211BWinPkgS_6_17_0_0_ALL.zip` and `6_3_1_0` (2006-2007); no archived copy (404 only) | 0 |
| BCM4320 (RNDIS) | `rndis_wlan.c` (at `v6.6`; gone by `v7.3`) | 11 (Linksys WUSB54GS/GSC, Belkin F5D7051, U.S. Robotics USR5420/5421, Buffalo WLI-U2-KG125S, ASUS WL169gE, others) | not fetched. Linksys's `WUSB54GS_20050428.exe` is archived as a redirect only (U) | - |
| BCM43235 (Linksys AE2500) | `broadcom/brcm80211/brcmfmac/usb.c` | 1 (`13B1:0039`) | (l) Linksys AE2500: XP and XP x64 `bcmwlhigh5.inf` 5.100.68.46 (2011-03-28), Vista `bcmwlhigh6.inf` 5.100.243.6 (2011-03-30), 7 `bcmwlhigh6.inf` 5.100.68.46 | `13B1:0039`, `003A` |
| BCM43143, BCM43236, BCM43242, BCM43569, Cypress 4373 | same file | 7 more | not searched (U) | - |

Not searched (U), each a small or old family: Atheros AR5523
(`ar5523.c`, D-Link DWL-G132, Netgear WG111T), Intersil ISL3887
(`p54usb.c`, 65 ids), Atmel AT76C50x (`at76c50x-usb.c`, 54 ids), Marvell
`libertas` and `mwifiex` USB parts, Qualcomm `ath6kl` and `ath10k` USB,
Redpine `rsi_91x_usb.c`, Morse Micro `mm81x`, pureLiFi `plfxlc`. Intel
has no USB Wi-Fi part in Linux `v7.3`.

## 4. Linux sources (facts)

At `v7.3`, line counts of the file as it stands, SPDX as tagged or the
header's own words. No free Wi-Fi driver is planned, so nothing here is
adapted. The table only records where the ids came from.

| File | SPDX / header | Lines | Ids |
|---|---|---|---|
| `realtek/rtl818x/rtl8187/dev.c` | GPL-2.0-only | 1697 | 21 |
| `realtek/rtl8xxxu/core.c` | GPL-2.0-only | 8295 | 122 |
| `realtek/rtlwifi/rtl8192cu/sw.c` | GPL-2.0 | 385 | 72 |
| `realtek/rtlwifi/rtl8192du/sw.c` | GPL-2.0 | 398 | 19 |
| `realtek/rtw88/rtw88??u.c` (8723du, 8812au, 8814au, 8821au, 8821cu, 8822bu, 8822cu) | GPL-2.0 OR BSD-3-Clause | 36 to 104 each | 2 to 36 each |
| `realtek/rtw89/rtw88??u.c`, `rtw8922au.c` | GPL-2.0 OR BSD-3-Clause | 72 to 146 each | 6 to 15 each |
| `ralink/rt2x00/rt2500usb.c` | GPL-2.0-or-later | 1984 | 31 |
| `ralink/rt2x00/rt73usb.c` | GPL-2.0-or-later | 2543 | 75 |
| `ralink/rt2x00/rt2800usb.c` | GPL-2.0-or-later | 1276 | 334 |
| `mediatek/mt7601u/usb.c` | GPL-2.0-only | 379 | 17 |
| `mediatek/mt76/mt76x0/usb.c` | GPL-2.0-only | 356 | 25 |
| `mediatek/mt76/mt76x2/usb.c`, `mt7921/usb.c`, `mt7925/usb.c`, `mt7615/usb.c` | BSD-3-Clause-Clear | 154 to 372 | 16, 6, 4, 2 |
| `ath/ath9k/hif_usb.c` | no SPDX; ISC-style "Permission to use" | 1517 | 27 |
| `ath/carl9170/usb.c` | no SPDX; "GNU General Public License" | 1231 | 28 |
| `zydas/zd1211rw/zd_usb.c` | GPL-2.0-or-later | 2040 | 56 |
| `broadcom/brcm80211/brcmfmac/usb.c` | ISC | 1607 | 9 |
| `legacy/rndis_wlan.c` (`v6.6`) | GPL-2.0-or-later | 3760 | 11 |

## 5. Wi-Fi security and configuration on old systems

On 98 SE, ME and 2000, Windows has no Wi-Fi configuration of its own.
Each driver's own utility sets the network name and key, and WPA or WPA2
works only if that utility (and its supplicant) supports it. On XP,
Windows' Wireless Zero Configuration can do it. Its WPA2 support needs
XP SP2 with Microsoft's WPA2 update, or SP3 (U: Microsoft's documents
were not read here). From Vista on, Windows does it itself.

| Package | Utility for 98 SE, ME, 2000 | WPA / WPA2 in the package's own text (V) |
|---|---|---|
| (a) RTL8187L | Realtek's `RtWLan` utility, with an EAP packet protocol driver | `Release_87.txt`: "2005.10.07 1.180 1. Implement AES and WPA2", "2005.11.04 1.187 1. Solve WPA-none AES issue in win98se with eappkt9x.vxd", "2006.05.12 1.229 1. Revise for WPA2" |
| (f) RTL8188SU/8191SU/8192SU | Realtek's utility (2000) | `DATA/8192SU.ini` and `Utility_Release.txt` name WPA2 |
| (c) RTL8188CU/8192CU | none needed below XP (no 2000 build) | `DATA/8192CU.ini` names WPA2 |
| (d), (e) Realtek 1030 | none (driver-only package; XP uses Windows, with Realtek's `Eappkt` protocol "for WinXP") | not stated |
| (g) Ralink 2007 | Ralink's `RaUI` (`RaUI_9x`, `RaUI_XP2k`), with Meetinghouse Data Communications' AEGIS client (`MeetingHouse/AegisI2.exe`, `AegisI5.exe`) as Ralink ships it | WPA2 not found in the text files read (U: the help files were not read) |
| (h) MediaTek | `RaUI` (2000 and XP) | `RaUI_Languages/Lang_USA.ini` names WPA2 |
| (k) ZyDAS ZD1211 | ZyDAS's `ZDWlan` utility, with its own 802.1X DLL and OpenSSL's `libeay32.dll`/`ssleay32.dll` | `ZDWlan.INI`: `WPAMode=1`, `WPA2Mode=1`. `ZDMLa.INI` lists "WPA", "WPA PSK", "WPA2", "WPA2 PSK" |
| (j) AR9170 | TP-Link's `Setup.exe` and `QSS.exe` (2000) | not stated (U) |
| (i) AR9271, (b) RTL8187B, (l) AE2500 | no 98 SE to 2000 build | not stated |

So the systems where WPA2 is documented below XP are: 98 SE and ME with
the RTL8187L and the ZD1211, and 2000 with those two and the RTL8192SU
family. The Ralink RT73 and RT2500USB serve 98 SE to 2000 too, but
WPA2 there is not shown in what was read.

## 6. Testability

The owner has no USB Wi-Fi adapter (no Wi-Fi hardware was named for
44.71). QEMU emulates no USB Wi-Fi device. The makers' drivers proposed
below are carried untested, as the pack's README says of every driver it
holds.

## 7. Gaps

Cells with no maker's driver found (or proposed in section 8), no in-box
driver and no `usb-wdm-free` driver. Each stays uncovered (no free
Wi-Fi driver is planned).

| Chip | Systems left uncovered | Why |
|---|---|---|
| Every Realtek 802.11n and later part (RTL8188CU and newer) | 98 SE, ME | No Realtek or brand build for 9x found. Stays uncovered |
| RTL8188CU, RTL8192CU, RTL8192DU, RTL8723AU, RTL8188EU and every later Realtek USB part | 2000 | Realtek's packages start at XP. Brand pages read start at XP. Stays uncovered |
| RTL8187B | 98 SE, ME, 2000 | Realtek's package starts at XP. Stays uncovered |
| RTL8188SU, RTL8191SU, RTL8192SU | 98 SE, ME | Package starts at 2000 (its setup notes name a 98 tool, but no 9x INF is in it). Stays uncovered |
| RTL8192FU | 98 SE to 7 | In no Realtek INF read. Stays uncovered |
| RTL8822CU | 98 SE to 7 | Only Linksys `13B1:0043` is in (e). Stays uncovered |
| RTL8851BU, RTL8852AU/8832AU, RTL8852BU/8832BU, RTL8852CU/8832CU, RTL8922AU | 98 SE to 7 | Makers and brands offer 10 and 11 only. Stays uncovered |
| Realtek ids the INFs leave out | XP to 7 | For example 17 of 82 RTL8188CU/8192CU ids, 5 of 15 RTL8188EU ids, 26 of 36 RTL8812BU/8822BU ids (section 3.1, best of (c), (d), (e)). Stays uncovered |
| RT2870 to RT5572, MT7601U, MT7610U, MT7612U | 98 SE, ME | No 9x build after 2007. (g)'s 9x folders hold RT73, RT2500USB and PCI parts only. Stays uncovered |
| The same | 2000 to 7 | Only (h), whose licence text limits distribution (owner question 2). Uncovered until the owner decides. On 7 the in-box `netr28u` serves 110 ids |
| RT2500USB | 7 | No 7 build. Vista's undecorated INF would install (U, owner question 8). Stays uncovered until then |
| MT7921AU, MT7925U, MT7663U | 98 SE to 7 | Nothing found. Stays uncovered |
| AR9271, AR7010 | 98 SE, ME, 2000 | TP-Link's package starts at XP. Stays uncovered |
| AR9271/AR7010 ids beyond the 4 Atheros ones | XP to 7 | The XP to 7 INFs list `0CF3` ids only (23 Linux ids left out). Stays uncovered |
| AR9170 | 98 SE, ME | TP-Link's package starts at 2000. Stays uncovered |
| ZD1211 | Vista, 7 | ZyDAS's last package found is 2006, XP x64 at most. Stays uncovered |
| ZD1211B | 98 SE to 7 | ZyDAS's packages are not archived. Brand packages were not searched (U). Stays uncovered |
| BCM4320 (RNDIS) | 98 SE to 7 | Not fetched. Whether `usb-wdm-free`'s `cdcnet` RNDIS binds them is U (it has no Wi-Fi settings). Stays uncovered |
| BCM43143, BCM43236, BCM43242, BCM43569, Cypress 4373 | 98 SE to 7 | Not searched. Stays uncovered |
| AR5523, ISL3887, AT76C50x and the others in 3.3 | all | Not searched. Stays uncovered |

## 8. Step 2: packages for `usb-wdm-nonfree`

For the separate worker that adds them. Layout:
`non-free/wifi/<family>/<systems>/`, with folder names as the pack's
README gives them. Every archive below was read on 2026-10-27. Sizes and
SHA-256 are of the archive as fetched. Archived URLs are the Internet
Archive's copies of the maker's own file URLs, written as
`https://web.archive.org/web/<timestamp>/<original>`.

**Licence, for (a) to (f), (i), (j), (k) and (l):** none found. No
licence, EULA or readme with terms is in those archives, and none is on
the download pages read. The INFs carry only the maker's copyright line,
quoted per package. This is the same footing as the Ethernet drivers the
pack carries ("Licence: none found" in its `NOTICE.md`). Owner
question 1.

**Licence for (g) and (h):** quoted in their rows. Owner questions 2
and 3.

### 8.1 The packages

**(a) Realtek RTL8187L, 98 SE to 7** (`rtl8187/`)

| Folder | Systems | Version, date | Archive (size, SHA-256) | Archived URL | INF ids |
|---|---|---|---|---|---|
| `w98se-vista/` (the contents of the zip's `87-USB/`, the whole "UI 1.00.0023" package as released) | 98 SE, ME, 2000, XP, XP x64, Vista x86/x64 | 5.1313.0613.2008 (XP x64 5.1312.0416.2008, Vista 6.1313.0613.2008), `DriverVer` 2008-06-13 | `0001-8187_1313.0613_UI_1.00.0023.zip` (10,459,616, `761c9f420c0f2636b429ddbc1905f45f38727cdfbca6b84f5cb39139c74d7898`) | `20240219095753/https://www.realtek.com/rtdrivers/cn/wlan/0001-8187_1313.0613_UI_1.00.0023.zip` (also `20151231070225`, on `12244.wpc.azureedge.net/8012244/drivers/rtdrivers/cn/wlan/`) | `0BDA:8187`, `0769:11F2`, `0789:010C`, `0DF6:000D`, `114B:0150`, `1371:9401`, `13D1:ABE6`, `18E8:6232`, all `REV_0100` |
| `w7/` (the zip's one folder, whole) | 7 x86/x64 | 6.1316.1209.2009 | `0001-RTL8187L_Win7_6.1316.1209.2009.L.zip` (18,383,978, `063c2a3193b454b8a43e80a55122aa758aeba287a99b0a33d62d5812b161a9f3`) | `20240219102145/https://www.realtek.com/rtdrivers/cn/wlan/0001-RTL8187L_Win7_6.1316.1209.2009.L.zip` | the above, plus `145F:0161`, `1B75:8187`, `7392:7612`, all `REV_0100` |

Linked from Realtek's old RTL8187L download list
(`realtek.com.tw/downloads/RedirectFTPSite.aspx?...DownID=437&PFid=1`,
archived 2015). INF copyright: "Copyright (C) 2008 Realtek Semiconductor
Corp." (2009 for 7). Both packages hold Realtek's InstallShield setup
and its utility. The 98 SE and ME INFs (`RTL8187/Win98/`, `WinME/`) and
the others copy their own files (`CopyFiles` lines active).

**(b) Realtek RTL8187B, XP to 7** (`rtl8187b/wxp-7/`, the zip's one
folder, whole)

- Archive: `0001-RTL8187B_Auto_Install_Program.zip`, 17,212,818 bytes,
  SHA-256 `aa503063bfad2e6987161388b0736bcf0f3e00ac6fc7b72a8451bf0fdd55b82a`,
  one folder
  `RTL8187B_Drv_XP_5.1162.0610.2009_Win7_62.1181.1105.2009_UI_1.00.0145.ALL.L/`.
- URL: `https://web.archive.org/web/20240219101632/https://www.realtek.com/rtdrivers/cn/wlan/0001-RTL8187B_Auto_Install_Program.zip`
  (Realtek list entry `DownID=880`).
- Versions: XP 5.1162.0610.2009, XP x64 5.1160.0415.2009, Vista
  6.1162.0610.2009, 7 62.1181.1105.2009 (newer than 7's in-box
  6.1159.0323.2009, same five ids).
- Ids: XP to Vista `0BDA:8187` and `8189`, `REV_0200`. 7 adds `8197`,
  `8198`, `8199`. INF copyright: "Copyright (C) 2008 Realtek
  Semiconductor Corp." (2009 for 7).

**(c) Realtek RTL8188CU, RTL8192CU, RTL8192DU, RTL8723AU, XP to 7**
(`rtl8192cu/wxp-7/`, the zip's one folder, whole)

- Archive: `0001-RTL819xCU_AutoInstallPackage.zip`, 35,282,265 bytes,
  SHA-256 `ad686d0e08c390f074e989950b70b4b94c03a5114c79f3cf265a912b20f4f1ef`,
  one folder `(BV5)RTLWlanU_1021.3.0912.2012_UI_1.00.0199.SupportALL.L/`.
- URL: `https://web.archive.org/web/20240219104028/https://www.realtek.com/rtdrivers/cn/wlan/0001-RTL819xCU_AutoInstallPackage.zip`.
- Version: 1021.3.0912.2012 (`DriverVer` 2012-09-12) for XP, XP x64,
  Vista, 7 and 8 (`RTWLANU_Driver/<system>/netrtwlanu.inf`).
- Ids: 93 (XP) and 95 (7). Realtek's own: `0BDA:018A`, `0193`, `0723`,
  `0724`, `1724`, `317F`, `8111`, `8170`, `8176`-`817C`, `817E`, `817F`,
  `818A`, `8192`-`8194`, `819A`, `8725`, `872B`, `A179`, `A724`, `E194`
  (7 adds `1E1E`, `8186`), and brand ids (Sitecom, Edimax, Abocom, Amigo,
  Hercules and others). It also lists `0BDA:8179` (RTL8188EU), which
  (d) lists too (section 8.3).
- Also in it: `RTLBt.inf` (XP, the RTL8723AU's Bluetooth half, 1.0.0.0)
  and an intermediate driver `IM/WINXP/*/TWOPORT/XPTWOPORT*.INF`.
  INF copyright: "Copyright (C) 2012 Realtek Semiconductor Corp."

**(d) Realtek 1030.25, USB 802.11n and 802.11ac parts, XP to 7**
(`rtwlanu/wxp-7/`, the zip's one folder, whole)

- Archive: `0002-Realtek_WindowsDriver_1030.25.0701.2017.zip`,
  52,899,174 bytes, SHA-256
  `dac755a0cecbdbe2818313f2049dfb140c98667ca08fe71b0478759c85d46c3a`,
  one folder
  `RTLWlanU_WindowsDriver_1030.25.0701.2017_DUA_Drv_3.00.0019.L/`.
- URL: `https://web.archive.org/web/20240218131614/https://www.realtek.com/rtdrivers/cn/wlan/0002-Realtek_WindowsDriver_1030.25.0701.2017.zip`.
  Realtek's pages (section 3.1) link it through `agree-to-download`.
  The archived generic `agree-to-download` page (2020) shows only an
  audio-driver note and an e-mail field. No terms for this file.
- Version: 1030.25.0701.2017, `DriverVer` 2017-12-25, for XP and XP x64
  (`netrtwlanu_XP.inf`), Vista (`netrtwlanu_Vista.inf`), 7, 8, 8.1 and 10
  (`netrtwlanu.inf`).
- Ids (39): Realtek `0BDA:0170`, `0179`, `0811`, `0823`, `2003`, `2005`,
  `2006`, `8179`, `818B`, `818C`, `8812`, `8813`, `881A`-`881C`, `A179`,
  `A811`, `A812`, `B703`, `B711`, `B720`, `B812`, `B81A`, `B82C`, `C811`,
  `C820`, `C82B`, `D723`, `D72B`, `F179`, and a few brand ids. Chips
  (by Linux's tables): RTL8188EU, RTL8188FU, RTL8192EU, RTL8723BU,
  RTL8710BU, RTL8723DU, RTL8812AU, RTL8811AU/8821AU, RTL8814AU,
  RTL8811CU/8821CU, RTL8812BU/8822BU.
- Driver-only package: `Setup.exe` (InstallShield), `Install.bat`, the
  drivers, `EAP/` and `IHV/` folders. No configuration utility. INF
  copyright: "Copyright (C) 2012 Realtek Semiconductor Corp."

**(e) Realtek 1030.17, the same chips with more brand ids, XP to 7**
(`rtwlanu17/wxp-7/`, the zip's one folder, whole; owner question 5)

- Archive: `0001-Realtek_USB_Windows_Driver_1030.17.1101.2016.zip`,
  25,710,952 bytes, SHA-256
  `a3ce0028313a8ca9058969170cc66966cc0488766a7ea7c4f66bf536885af45a`,
  one folder `Realtek_USB_Windows_WHQL_Driver_1030.17.1101.2016/`.
- URL: `https://web.archive.org/web/20240218044047/https://www.realtek.com/rtdrivers/cn/wlan/0001-Realtek_USB_Windows_Driver_1030.17.1101.2016.zip`
  (Realtek list entry `DownID=1120&PFid=64`, archived 2019).
- Version: 1030.17.1101.2016, `DriverVer` 2017-04-06. XP, XP x64,
  Vista, 7, 8, 8.1, 10. Drivers only, no setup program.
- Ids: 108. Realtek's own `0BDA:0179`, `0811`, `8179`, `818B`, `8812`,
  `8813`, `881A`-`881C`, `A179`, `A811`, `A812`, `B812`, `C811`, `C82B`,
  and about 90 brand ids (ASUS, Belkin, D-Link, Edimax, Linksys,
  Netgear, TP-Link, TRENDnet and others). 86 of its 108 ids are not in
  (d), and 17 of (d)'s 39 are not in it. INF copyright: as (d).

**(f) Realtek RTL8188SU, RTL8191SU, RTL8192SU, 2000 to 7**
(`rtl8192su/w2k-7/`, the zip's one folder, whole)

- Archive: `0001-RTL8192SU_AutoInstallPackage.zip`, 29,820,529 bytes,
  SHA-256 `535ffcf7822c827d2bdaa1358ba57b7a1b7cc6245839054407b334fcebdad331`,
  one folder
  `RTL8188_8191_8192_SU_WindowsDriver_1086.48.0809.2011.F0049_12.P0406_UI_1.00.0187.L/`.
- URL: `https://web.archive.org/web/20240219103357/https://www.realtek.com/rtdrivers/cn/wlan/0001-RTL8192SU_AutoInstallPackage.zip`
  (Realtek list entry `DownID=858`).
- Versions: 2000, XP, XP x64 and Vista 1084.53.0809.2011. 7
  1086.48.0809.2011 (`88_91_92_SU_Driver/<system>/net8192su.inf`). The
  2000 INF has undecorated models sections.
- Ids: 76 to 78. Realtek `0BDA:8171`-`8175`, `8192`, `8712`, `C047`, and
  brand ids (Corega, D-Link, EnGenius, Belkin, Edimax, Sitecom, ASUS,
  TRENDnet and others). INF copyright: "Copyright (C) 2008 Realtek
  Semiconductor Corp."

**(g) Ralink RT73, RT2500USB (and early RT2870), 98 SE to Vista**
(`ralink/w98se-vista/`, owner question 3)

- Archive: one setup program,
  `IS_AP_STA_7x_D-1.2.1.0_VA-3.0.6.0_2500_D-2.1.1.0_VA-3.0.2.0_RU-2.0.2.0_VA-1.0.12.0_AU-1.1.9.0_062807_0.1.0.23.exe`,
  31,858,076 bytes, SHA-256
  `dd69079bbf806907a0ed2c3372d2d4c38b37d0412f3116030e74151b1ac7abbb`.
- URL: `https://web.archive.org/web/20070813201119/http://www.ralinktech.com.tw/data/drivers/IS_AP_STA_7x_D-1.2.1.0_VA-3.0.6.0_2500_D-2.1.1.0_VA-3.0.2.0_RU-2.0.2.0_VA-1.0.12.0_AU-1.1.9.0_062807_0.1.0.23.exe`.
- Inside (read from its InstallShield cabinet, not run): driver folders
  for RT73, RT2500USB, RT2870 (early), and the PCI parts RT2500, RT61,
  RT2860, each as `_9x`, `_XP2k`, `_XPX64`, `_VistaX86`, `_VistaX64`
  (RT2870 has no `_9x`). `RaUI_9x`, `RaUI_XP2k`, `RaUI_Vista`, `SoftAP`,
  `MeetingHouse`.
- Versions: RT73 1.02.01.0000 (2007-05-14, 9x to XP x64), 3.00.06.0000
  (Vista). RT2500USB 2.01.01.0000 (2006-11-08), 3.00.02.0000 (Vista).
- Ids: RT73 30 (Ralink `148F:2573`, `2671`, `9021` and brands), 32 on
  Vista. RT2500USB 23 (`148F:2570`, `9020` and brands), 24 on Vista.
- Licence text (`_Support_Language_Independent_OS_Independent_Files/License.txt`,
  "RALINK Wireless Utility for Windows 98/ME/2000/XP/Vista"):
  "Reproduction and Distribution. You may reproduce and distribute an
  unlimited number of copies of the SOFTWARE PRODUCT; provided that each
  copy shall be a true and complete copy, including all copyright and
  trademark notices, and shall be accompanied by a copy of this EULA.
  Copies of the SOFTWARE PRODUCT may be distributed as a standalone
  product or included with your own product." It also says "You may not
  reverse engineer, decompile, or disassemble the SOFTWARE PRODUCT". The
  2005 RT2500USB package carries the same text for "Windows
  98/ME/2000/XP".

**(h) MediaTek RT2870 to MT7612U, 2000 to 7** (`mtk-rt2870/w2k-7/`,
owner question 2)

- Archive: `IS_Setup_ICS_011916_1.5.39.173.zip`, 42,835,761 bytes,
  SHA-256 `1849add500b2e0a1650ad64bfdbb79946cc1c4af08344664143dc6cb72a38ea1`,
  holding one setup program `IS_Setup_ICS_011916_1.5.39.173.exe`
  (2016-01-20).
- URL: `https://web.archive.org/web/20180131104148/https://d86o2zu8ugzlg.cloudfront.net/mediatek-craft/drivers/IS_Setup_ICS_011916_1.5.39.173.zip`,
  linked from MediaTek's RT2070 and MT7612U pages (archived 2021).
- Inside: `RT2870_Driver_XP2k/RT2870.inf` (`$Chicago$`, undecorated
  models), `_XPX64`, `_VistaX86`, `_VistaX64` (`RT2870.inf`),
  `_Win7X86/X64`, `_Win8*`, `_Win8.1*`, `_Win10*` (`netr28u(x).inf`), all
  5.01.25.0 (2015-10-28), with firmware for RT2870, RT3573, MT7601U,
  MT7610U, MT7662. Also `RaUI`, `SoftAP`, an NDIS protocol driver and
  `devcon.exe`.
- Ids: 317 in every INF. Ralink and MediaTek's own: `148F:2070`, `2770`,
  `2870`, `3070`-`3072`, `3370`, `3572`, `3573`, `5370`, `5372`, `5572`,
  `6370`, `6570`, `7601`, `760A`-`760D`, `7610`, `761A`, `761B`, `8070`,
  `F101`, `F301`, `0E8D:7600`, `760A`, `760B`, `7610`, `7612`, `761A`,
  `7650`, `7662`, and many brand ids.
- Licence text
  (`_Support_Language_Independent_OS_Independent_Files/License.txt`,
  "Copyright (C) [2014] Mediatek Inc."): "(2) Reproduction and
  Distribution: You may reproduce and distribute an unlimited number of
  copies of the Software; provided that each copy shall be a true and
  complete copy, including all copyright and trademark notices, and shall
  be accompanied by a copy of this Agreement. Copies of the Software may
  be distributed only with your own product." It also says "You shall
  not reverse engineer, decompile, or disassemble the Software." The
  download page says the downloads "are designated for business partner
  use" (section 3.2).

**(i) Atheros AR9271, AR7010, XP to 7** (`ar9271/wxp-7/`, the contents of
the zip's inner folder `TL-WN721N_V1_140915/`)

- Archive: `TL-WN721N_V1_140915.zip`, 7,368,965 bytes, SHA-256
  `5217cb84427bee80754e9ab6718acde854bd1eb0507580dd19d00803f27f345d`.
- URL (TP-Link, live 2026-10-27):
  `https://static.tp-link.com/resources/software/TL-WN721N_V1_140915.zip`,
  from `https://www.tp-link.com/us/support/download/tl-wn721n/v1/`
  ("Published Date: 2014-09-15", "Vista/XP/7/8/8.1").
- Versions: XP and XP x64 7.7.0.102 (2011-07-28), Vista 8.0.0.84, 7
  2.0.0.62 (2011-04-20), 8 10.0.0.25, 8.1 10.0.0.28. Drivers and a
  release note only. TP-Link's utility is a separate download
  (`TL-WN721N_V1_Utility_140915.zip`, not fetched).
- Ids: XP to 7 `0CF3:9271`, `1006`, `7010`, `7015`. 8 and 8.1 add
  Netgear, D-Link, AzureWave, Lite-On and TP-Link ids (22). INF copyright:
  "Copyright 2009 Atheros Communications Inc., all rights reserved."
  (8.1: "Copyright (c) 2011 Qualcomm Atheros, Inc.").
- TP-Link's TL-WN722N v1 package (`TL-WN722N_V1_131113.zip`, 13,206,671
  bytes, SHA-256
  `564e872f4a384d03429711356ce27f19189df8ff3f2d35c3ee522ef9fdb6eac8`)
  holds the same XP to 8 builds with a `Setup.exe`. Not proposed.

**(j) Atheros AR9170, 2000 to 7** (`ar9170/w2k-7/`, the contents of the
zip's `TL-WN821N_WN822N/`)

- Archive: `TL-WN821N_WN822N_100629.zip`, 30,190,672 bytes, SHA-256
  `3d4d5d7d49f61e5b4e17e0ea4d5d415770599d7b238f31eb019c1a7b5aaa7d44`.
- URL (TP-Link, live 2026-10-27):
  `https://static.tp-link.com/resources/software/TL-WN821N_WN822N_100629.zip`,
  from `https://www.tp-link.com/us/support/download/tl-wn822n/v1/`
  ("TL-WN822N_100629", "Published Date: 2010-06-29", "Windows
  2000/XP/Vista/7", "WHQL-Certified").
- Versions: 2000, XP and XP x64 3.0.0.165 (2010-02-25), Vista
  3.1.0.131 (2010-03-01), 7 3.5.0.12 (2009-11-26). With TP-Link's
  `Setup.exe`, `QSS.exe` and two user guides.
- Ids (10): `0CF3:1002`, `9170`, Netgear `0846:9001`, `9010`, `9040`,
  D-Link DWA-160 `07D1:3C10`, `3A09`, Arcadyan `083A:F522`, `1435:0804`.
  INF copyright: "Copyright (c) 2009, TP-LINK TECHNOLOGIES CO., LTD.All
  Rights Reserved."

**(k) ZyDAS ZD1211, 98 SE to XP x64** (`zd1211/w98se-xp/`, owner
question 4)

- Archive: `ZD1211USB_Install_4_13_0_0_ALL.zip`, 3,667,982 bytes,
  SHA-256 `7bce05894139eae0d03108021522f237398adc504d12d4f74df9256bf2df88fd`,
  holding one setup program `ZD1211USB_Install_4_13_0_0_ALL.exe`
  (2006-04-12).
- URL: `https://web.archive.org/web/20060626171824/http://www.zydas.com.tw:80/web_driver/ZD1211_USB/Windows/ZD1211USB_Install_4_13_0_0_ALL.zip`.
- Inside: `ZD1211U.inf` 4.13.0.0 (2006-02-27; models undecorated, `NT.5.1`
  and `ntamd64`), `ZD1211U.sys` for 98 SE and ME, `ZD11U2K.sys`,
  `ZD11UXP.sys`, `ZD11U64.sys`, a catalog, the `ZDWlan` utility and its
  protocol drivers. The INF's `CopyFiles` file lines are commented out
  (`;ZD1211U.sys,ZD11U98.sys,,2`): the setup program places the driver,
  and the INF alone copies nothing.
- Ids (15): `0ACE:1211`, `0586:3401`, `3402`, `0B3B:1630`, `5630`,
  `6630`, `129B:1666`, `0CDE:0009`, `0011`, `079B:004A`, `6891:A727`,
  `1435:0711`, `0DF6:9071`, `07B8:6001`, `126F:A006`. INF copyright:
  "Copyright (c) 2000-2003 ZyDAS Technology Corporation."
- The utility ships OpenSSL's `libeay32.dll`, `ssleay32.dll` and
  `openssl.exe`. No OpenSSL licence text is in the package.

**(l) Broadcom BCM43235, Linksys AE2500 only, XP to 7** (`ae2500/`)

| Folder | Systems | Version, date | Archive (size, SHA-256) | Archived URL |
|---|---|---|---|---|
| `wxp/` | XP, XP x64 | 5.100.68.46, 2011-03-28 | `AE2500_xpdriver_5.100.68.46_WHQL,0.zip` (3,117,121, `ee9bdf47246e37bc2d1150e0504bd18dcedfbf0a2b8c67c561a25fdcff592250`) | `20210922091040/https://downloads.linksys.com/downloads/driver/AE2500_xpdriver_5.100.68.46_WHQL,0.zip` |
| `wvista/` | Vista x86/x64 | 5.100.243.6, 2011-03-30 | `AE2500_vistadriver_5.100.243.6_WHQL,0.zip` (10,695,279, `217b3287fdc27fe2e652b9538329edc1f0db42b516749382eb109bfdfc0d2463`) | `20210922091037/https://downloads.linksys.com/downloads/driver/AE2500_vistadriver_5.100.243.6_WHQL,0.zip` |
| `w7/` | 7 x86/x64 | 5.100.68.46, 2011-03-28 | `AE2500Win7_WHQL.zip` (10,580,152, `fbcba9646b1eb2c9f0fa0135c12d5e3748c7a7152f3c32fed5228880de7ab3e2`) | `20150809163702/http://downloads.linksys.com/downloads/driver/AE2500Win7_WHQL.zip` |

Ids: `13B1:0039`, `003A` only (manufacturer "Cisco"). One dongle model.
INF copyright: "Copyright 1998-2011, Broadcom Corporation." Owner
question 9.

### 8.2 NOTICE.md rows, in the pack's style

A new `### non-free/wifi/` table:

```
| All files in `rtl8187/w98se-vista/` | Realtek Semiconductor Corp.; its setup engine is Flexera's InstallShield, as Realtek ships it | 5.1313.0613.2008 (`Win98\Netrtuw.inf` `DriverVer` 06/13/2008; XP x64 5.1312.0416.2008; Vista 6.1313.0613.2008) |
| All files in `rtl8187/w7/` | as `rtl8187/w98se-vista/` | 6.1316.1209.2009 |
| All files in `rtl8187b/wxp-7/` | as `rtl8187/w98se-vista/` | XP 5.1162.0610.2009, XP x64 5.1160.0415.2009, Vista 6.1162.0610.2009, 7 62.1181.1105.2009 |
| All files in `rtl8192cu/wxp-7/` | as `rtl8187/w98se-vista/` | 1021.3.0912.2012 |
| All files in `rtwlanu/wxp-7/` | as `rtl8187/w98se-vista/` | 1030.25.0701.2017 (`DriverVer` 12/25/2017) |
| All files in `rtwlanu17/wxp-7/` | Realtek Semiconductor Corp. | 1030.17.1101.2016 (`DriverVer` 04/06/2017) |
| All files in `rtl8192su/w2k-7/` | as `rtl8187/w98se-vista/` | 1084.53.0809.2011 (2000 to Vista), 1086.48.0809.2011 (7) |
| `ralink/w98se-vista/IS_AP_STA_7x_..._0.1.0.23.exe` | Ralink Technology Corp. (now MediaTek Inc.); InstallShield setup; inside it Meetinghouse Data Communications' AEGIS client, as Ralink ships it | 0.1.0.23 (RT73 1.02.01.0000 / 3.00.06.0000, RT2500USB 2.01.01.0000 / 3.00.02.0000) |
| `mtk-rt2870/w2k-7/IS_Setup_ICS_011916_1.5.39.173.exe` (if the owner accepts its terms) | MediaTek Inc. (Ralink's line); InstallShield setup | 1.5.39.173 (`RT2870.inf` 5.01.25.0) |
| All files in `ar9271/wxp-7/` | Qualcomm Atheros, Inc. (Atheros Communications), as TP-LINK Technologies Co., Ltd. ships it for the TL-WN721N | 7.7.0.102 (XP), 8.0.0.84 (Vista), 2.0.0.62 (7), 10.0.0.25 (8), 10.0.0.28 (8.1) |
| All files in `ar9170/w2k-7/` | Atheros Communications, Inc., as TP-LINK Technologies Co., Ltd. ships it for the TL-WN821N and TL-WN822N (the INFs' copyright: TP-LINK) | 3.0.0.165 (2000, XP), 3.1.0.131 (Vista), 3.5.0.12 (7) |
| `zd1211/w98se-xp/ZD1211USB_Install_4_13_0_0_ALL.exe` | ZyDAS Technology Corporation; InstallShield setup; OpenSSL's DLLs inside, as ZyDAS ships them | 4.13.0.0 |
| All files in `ae2500/wxp/`, `wvista/`, `w7/` | Broadcom Corporation, as Cisco-Linksys ships it for the AE2500 | 5.100.68.46, 5.100.243.6, 5.100.68.46 |
```

The worker replaces "All files" with the counts, as the pack does, and
writes each package's provenance block (vendor, package, original and
archived URL, obtained date, archive name, size and SHA-256, the licence
line, modification: none) from 8.1. For (g) and (h) the licence line
quotes the text above and the package's `License.txt` stays with it.

### 8.3 STAGE.TXT lines, in its format

One INF per hardware id per system. The lines for (g), (h) and (k) wait
for owner questions 2 to 4. The worker must still check the "one INF
per id per system" rule by reading each `[Models]` for each system, as
the pack's existing lines do.

```
# ---- Wi-Fi: Realtek RTL8187L (0BDA:8187 REV_0100 and 7 brand ids)
STAGE | W98SE            | non-free/wifi/rtl8187/w98se-vista/RTL8187/Win98/Netrtuw.inf    | 5.1313 (2008)
STAGE | WME              | non-free/wifi/rtl8187/w98se-vista/RTL8187/WinME/Netrtuw.inf    | 5.1313 (2008)
STAGE | W2K WXP          | non-free/wifi/rtl8187/w98se-vista/RTL8187/WinXP2K/Netrtuw.inf  | 5.1313. undecorated [Realtek] for 2000, NTx86 for XP
STAGE | WXP64            | non-free/wifi/rtl8187/w98se-vista/RTL8187/WinX64/Netrtuw_x64.inf | 5.1312
STAGE | WVISTA           | non-free/wifi/rtl8187/w98se-vista/RTL8187/VistaX86/Netrtuw.inf | 6.1313
STAGE | WVISTA64         | non-free/wifi/rtl8187/w98se-vista/RTL8187/VistaX64/Netrtuw.inf | 6.1313
STAGE | W7               | non-free/wifi/rtl8187/w7/RTL8187/Win7X86/Netrtuw.inf           | 6.1316 (2009)
STAGE | W7X64            | non-free/wifi/rtl8187/w7/RTL8187/Win7X64/Netrtuw.inf           | 6.1316
# ---- Wi-Fi: Realtek RTL8187B (0BDA:8187/8189 REV_0200, on 7 also 8197, 8198, 8199)
STAGE | WXP              | non-free/wifi/rtl8187b/wxp-7/RTL8187B/WinXP/net8187b.inf    | 5.1162 (2009)
STAGE | WXP64            | non-free/wifi/rtl8187b/wxp-7/RTL8187B/WinX64/net8187b.inf   | 5.1160
STAGE | WVISTA           | non-free/wifi/rtl8187b/wxp-7/RTL8187B/VistaX86/net8187b.inf | 6.1162
STAGE | WVISTA64         | non-free/wifi/rtl8187b/wxp-7/RTL8187B/VistaX64/net8187b.inf | 6.1162
STAGE | W7               | non-free/wifi/rtl8187b/wxp-7/RTL8187B/Win7X86/net8187b.inf  | 62.1181, newer than 7's in-box 6.1159
STAGE | W7X64            | non-free/wifi/rtl8187b/wxp-7/RTL8187B/Win7X64/net8187b.inf  | 62.1181
# ---- Wi-Fi: Realtek RTL8188CU / RTL8192CU / RTL8192DU / RTL8723AU, 1021.3
STAGE | WXP              | non-free/wifi/rtl8192cu/wxp-7/RTWLANU_Driver/WinXP/netrtwlanu.inf    | 1021.3 (2012). 0BDA:8179 also in rtwlanu: worker to resolve
STAGE | WXP64            | non-free/wifi/rtl8192cu/wxp-7/RTWLANU_Driver/WinX64/netrtwlanu.inf   | 1021.3
STAGE | WVISTA           | non-free/wifi/rtl8192cu/wxp-7/RTWLANU_Driver/VistaX86/netrtwlanu.inf | 1021.3
STAGE | WVISTA64         | non-free/wifi/rtl8192cu/wxp-7/RTWLANU_Driver/VistaX64/netrtwlanu.inf | 1021.3
STAGE | W7               | non-free/wifi/rtl8192cu/wxp-7/RTWLANU_Driver/Win7X86/netrtwlanu.inf  | 1021.3
STAGE | W7X64            | non-free/wifi/rtl8192cu/wxp-7/RTWLANU_Driver/Win7X64/netrtwlanu.inf  | 1021.3
COPY  | WXP WXP64        | non-free/wifi/rtl8192cu/wxp-7/RTWLANU_Driver/WinXP/RTLBt.inf         | RTL8723AU Bluetooth half. Windows' own Bluetooth stack
COPY  | WXP64            | non-free/wifi/rtl8192cu/wxp-7/RTWLANU_Driver/WinX64/RTLBt.inf        | as above
# ---- Wi-Fi: Realtek 1030.25 (RTL8188EU/FU, 8192EU, 8723BU/DU, 8710BU, 8812AU, 8811AU/8821AU, 8814AU, 8811CU/8821CU, 8812BU/8822BU)
STAGE | WXP              | non-free/wifi/rtwlanu/wxp-7/RTWLANU_Driver/WinXP/netrtwlanu_XP.inf       | 1030.25 (DriverVer 2017-12-25). Realtek's page lists 7 and later (owner question 6)
STAGE | WXP64            | non-free/wifi/rtwlanu/wxp-7/RTWLANU_Driver/WinX64/netrtwlanu_XP.inf      | 1030.25
STAGE | WVISTA           | non-free/wifi/rtwlanu/wxp-7/RTWLANU_Driver/VistaX86/netrtwlanu_Vista.inf | 1030.25
STAGE | WVISTA64         | non-free/wifi/rtwlanu/wxp-7/RTWLANU_Driver/VistaX64/netrtwlanu_Vista.inf | 1030.25
STAGE | W7               | non-free/wifi/rtwlanu/wxp-7/RTWLANU_Driver/Win7X86/netrtwlanu.inf        | 1030.25. the Win8 folders' INFs are decorated 6.1 too: COPY
STAGE | W7X64            | non-free/wifi/rtwlanu/wxp-7/RTWLANU_Driver/Win7X64/netrtwlanu.inf        | 1030.25
# ---- Wi-Fi: Realtek 1030.17 (owner question 5: COPY, as it overlaps rtwlanu on Realtek's own ids)
COPY  | WXP              | non-free/wifi/rtwlanu17/wxp-7/WinXP/netrtwlanu_XP.inf       | brand ids rtwlanu lacks. point Windows at it by hand
COPY  | WXP64            | non-free/wifi/rtwlanu17/wxp-7/WinX64/netrtwlanu_XP.inf      |
COPY  | WVISTA           | non-free/wifi/rtwlanu17/wxp-7/VistaX86/netrtwlanu_Vista.inf |
COPY  | WVISTA64         | non-free/wifi/rtwlanu17/wxp-7/VistaX64/netrtwlanu_Vista.inf |
COPY  | W7               | non-free/wifi/rtwlanu17/wxp-7/Win7X86/netrtwlanu.inf        |
COPY  | W7X64            | non-free/wifi/rtwlanu17/wxp-7/Win7X64/netrtwlanu.inf        |
# ---- Wi-Fi: Realtek RTL8188SU / RTL8191SU / RTL8192SU
STAGE | W2K              | non-free/wifi/rtl8192su/w2k-7/88_91_92_SU_Driver/Win2K/net8192su.inf    | 1084.53 (2011). 0BDA:8192 also in rtl8192cu: worker to check revisions
STAGE | WXP              | non-free/wifi/rtl8192su/w2k-7/88_91_92_SU_Driver/WinXP/net8192su.inf    | 1084.53
STAGE | WXP64            | non-free/wifi/rtl8192su/w2k-7/88_91_92_SU_Driver/WinX64/net8192su.inf   | 1084.53
STAGE | WVISTA           | non-free/wifi/rtl8192su/w2k-7/88_91_92_SU_Driver/VistaX86/net8192su.inf | 1084.53
STAGE | WVISTA64         | non-free/wifi/rtl8192su/w2k-7/88_91_92_SU_Driver/VistaX64/net8192su.inf | 1084.53
STAGE | W7               | non-free/wifi/rtl8192su/w2k-7/88_91_92_SU_Driver/Win7X86/net8192su.inf  | 1086.48
STAGE | W7X64            | non-free/wifi/rtl8192su/w2k-7/88_91_92_SU_Driver/Win7X64/net8192su.inf  | 1086.48
# ---- Wi-Fi: Atheros AR9271 / AR7010 (TP-Link TL-WN721N v1 package)
STAGE | WXP              | non-free/wifi/ar9271/wxp-7/Windows XP 32bit/netathuw.inf     | 7.7.0.102 (2011)
STAGE | WXP64            | non-free/wifi/ar9271/wxp-7/Windows XP 64bit/netathuwx.inf    | 7.7.0.102
STAGE | WVISTA           | non-free/wifi/ar9271/wxp-7/Windows Vista 32bit/netathur.inf  | 8.0.0.84
STAGE | WVISTA64         | non-free/wifi/ar9271/wxp-7/Windows Vista 64bit/netathurx.inf | 8.0.0.84
STAGE | W7               | non-free/wifi/ar9271/wxp-7/Windows 7 32bit/netathur.inf      | 2.0.0.62
STAGE | W7X64            | non-free/wifi/ar9271/wxp-7/Windows 7 64bit/netathurx.inf     | 2.0.0.62
# ---- Wi-Fi: Atheros AR9170 (TP-Link TL-WN821N v2 / TL-WN822N v1 package)
STAGE | W2K              | non-free/wifi/ar9170/w2k-7/Driver Files/Windows 2000/arusb_xp.inf     | 3.0.0.165, arusb_2k.sys. undecorated [Atheros]
STAGE | WXP              | non-free/wifi/ar9170/w2k-7/Driver Files/Windows XP 32bit/arusb_xp.inf | 3.0.0.165
STAGE | WXP64            | non-free/wifi/ar9170/w2k-7/Driver Files/Windows XP 64bit/arusb_xp.inf | 3.0.0.165
STAGE | WVISTA           | non-free/wifi/ar9170/w2k-7/Driver Files/Windows Vista 32bit/arusb_lh.inf  | 3.1.0.131
STAGE | WVISTA64         | non-free/wifi/ar9170/w2k-7/Driver Files/Windows Vista 64bit/arusb_lhx.inf | 3.1.0.131
STAGE | W7               | non-free/wifi/ar9170/w2k-7/Driver Files/Windows 7 32bit/arusb_win7.inf    | 3.5.0.12
STAGE | W7X64            | non-free/wifi/ar9170/w2k-7/Driver Files/Windows 7 64bit/arusb_win7x.inf   | 3.5.0.12
# ---- Wi-Fi: Broadcom BCM43235, Linksys AE2500 (13B1:0039, 003A, owner question 9)
STAGE | WXP WXP64        | non-free/wifi/ae2500/wxp/xp/bcmwlhigh5.inf      | 5.100.68.46
STAGE | WVISTA WVISTA64  | non-free/wifi/ae2500/wvista/vista/bcmwlhigh6.inf | 5.100.243.6. its 6.1 sections would match on 7: there w7 wins
STAGE | W7 W7X64         | non-free/wifi/ae2500/w7/win7/bcmwlhigh6.inf      | 5.100.68.46
```

The other INFs in these packages (8, 8.1 and 10 builds, Realtek's
`IM/.../XPTWOPORT*.INF`, `Autorun.inf`) are `COPY` lines. Packages (g),
(h) and (k) carried as setup programs have no INF on disk and so no
line. Their setup programs are copied, never run, as STAGE.TXT's header
says. If the owner chooses unpacked folders for (g) instead, its lines
would be: RT73 `RT73_Driver_9x/rt73.inf` (W98SE WME),
`RT73_Driver_XP2k/rt73.inf` (W2K WXP), `RT73_Driver_XPX64/rt73.inf`
(WXP64), `RT73_Driver_VistaX86/netr73.inf` (WVISTA),
`RT73_Driver_VistaX64/netr7364.inf` (WVISTA64), and the same five for
RT2500USB (`rt25u98.inf`, `rt2500usb.inf`, `netr70.inf`,
`netr7064.inf`). The early RT2870 folders would be `COPY`, as (h) is
newer if carried.

## 9. Out of scope (with reason)

| Item | Why out |
|---|---|
| PCI, PCIe, CardBus and SDIO Wi-Fi (Realtek `RTL8185`, `RTL8192SE`, Ralink RT61, RT2860, `rtl8723bs`, and the PCI folders inside (g)) | not USB |
| Mobile-broadband modems and phone tethering | Ethernet survey's section 9 |
| Bluetooth halves of combo chips (RTL8723AU/BU/DU, MT7612U, MT7921AU) | Bluetooth is candidates.md section 9, not proposed |
| Monitor-mode capture drivers (Ralink and Realtek "OmniPeek" packages) | not a network adapter driver |

## 10. Owner questions

1. Realtek (a) to (f), TP-Link's Atheros (i) and (j), ZyDAS (k) and
   Linksys (l): no licence text found, as for the Ethernet drivers the
   pack carries. Add them on the same basis?
2. MediaTek (h): its EULA allows copies "only with your own product".
   Its page says the downloads are "designated for business partner use".
   List it under "Not shipped" with its link, as the pack did for Silicon
   Labs? Or carry it?
3. Ralink (g): one setup program. Its EULA allows copies "as a standalone
   product", each "a true and complete copy" with the EULA. Carry the
   `.exe` as released (no `STAGE` line), its unpacked folders, or both?
   Would unpacked folders still be "a true and complete copy"?
4. ZyDAS (k): one setup program. Its INF copies no files on its own, so
   an unpacked folder would not install from the INF alone. Carry the
   setup program as released, with no `STAGE` line? It also holds
   OpenSSL's DLLs with no OpenSSL licence text.
5. Realtek (e) 1030.17 lists 86 ids that (d) 1030.25 lacks (most of them
   brand ids),
   and overlaps it on Realtek's own ids. Carry both, with (e) as `COPY`
   only? Or only (d)?
6. Realtek (d): Realtek's page lists it for 7 and later, but it carries
   XP and Vista builds and its note says "support WinXP ~ Win10 OS".
   Stage it on XP and Vista too?
7. MediaTek (h), if carried: its `RT2870_Driver_XP2k` INF is undecorated.
   Stage it on 2000 as well as XP, marked unknown?
8. Ralink RT2500USB on 7: no 7 build. Vista's INF (`netr70.inf`,
   undecorated) would install there. Stage it on 7 too, as owner
   question 8 of the Ethernet survey asked for the RTL8150?
9. Linksys AE2500 (l) serves one dongle (two ids). Worth carrying?
10. Realtek's packages (a), (b), (c), (f) are whole "Auto Installation
    Program" folders with InstallShield, a utility, EAP add-ons and 30+
    languages (17 to 52 MB). Carry them whole, as the pack does with
    `rtl815x`? Or only the driver folders?
11. TP-Link's AR9271 utility (`TL-WN721N_V1_Utility_140915.zip`) is a
    separate download. Leave it out, as XP to 7 can configure Wi-Fi
    themselves?
12. Which USB Wi-Fi adapters does the owner have, if any? Every row here
    is untested.

## 11. Sources

- Realtek: `https://www.realtek.com/rtdrivers/cn/wlan/` and
  `12244.wpc.azureedge.net/8012244/drivers/rtdrivers/cn/wlan/` (Internet
  Archive indexes), the archived `realtek.com.tw` download lists
  (`RedirectFTPSite.aspx` entries), and the archived
  `realtek.com/en/component/zoo/category/<chip>-software` pages
  (rtl8192eu, rtl8811au, rtl8811cu, rtl8812bu, rtl8814au, 2023).
- Ralink: the Internet Archive's index of `ralinktech.com.tw/data/drivers/`
  and `ralinktech.com/drivers/Windows/`.
- MediaTek: the archived `mediatek.com/products/broadbandWifi/<chip>`
  pages (mt7601u, mt7610u, mt7612u, rt2070, rt2770, rt2870, rt3070,
  rt3572, rt5372, 2021) and the index of
  `d86o2zu8ugzlg.cloudfront.net/mediatek-craft/drivers/`.
- ZyDAS: the Internet Archive's index of `zydas.com.tw/web_driver/`.
- TP-Link: live `https://www.tp-link.com/us/support/download/<model>/<version>/`
  pages, read 2026-10-27: tl-wn722n v1 and v2, tl-wn721n v1, tl-wn822n
  v1, tl-wn821n v2, tl-wn7200nd v1, tl-wn725n v1 and v2, tl-wn727n v4,
  tl-wn321g v4, tl-wn322g v3, tl-wn823n v1 and v2, archer-t2u v1 and v3,
  archer-t2u-nano v1, archer-t2u-plus v1, archer-t3u v1, archer-t4u v1,
  archer-tx20u v1, archer-tx20u-nano v1.
- Linksys: the Internet Archive's index of
  `downloads.linksys.com/downloads/`.
- `usb-wdm-nonfree` (`README.md`, `NOTICE.md`, `STAGE.TXT`, branch
  `lan-add`), read only.
- Linux `v7.3` `drivers/net/wireless/` and `v6.6`
  `drivers/net/wireless/legacy/rndis_wlan.c` (facts only).
- Windows install media on the survey host, as section 0.

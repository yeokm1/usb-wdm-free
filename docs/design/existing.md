# Existing drivers for the planned packages

Status: survey, 2026-10-20, with the owner's decisions on its verdicts
(section 12). No code.

Owner, 2026-10-20: "For free drivers, we should see which ones already
exist and not implement them again, e.g. serial." This document checks
each planned package against drivers that already exist: vendor drivers,
community packs, free projects and Microsoft redistributables. An existing
driver that works goes into `usb-wdm-nonfree`, with full provenance
(official source, version, archive hash, licence text), or into this
repository if its licence allows, instead of being rewritten.
`usb-wdm-nonfree` is a separate project: no driver of this repository
needs anything from it, and a package narrowed here serves its own scope
on its own.

Each package gets one of three verdicts:

- **(A) Covered.** Drop our package and carry the existing driver.
- **(B) Partly covered.** Narrow our package to the gap, which is stated.
- **(C) Not covered.** Keep the package.

## How this was checked

- Web research on 2026-10-20: vendor pages and their Internet Archive
  snapshots, Microsoft Learn and archived KB articles, MSFN, VOGONS,
  WinRAID, OSR, GitHub, SourceForge and archive.org items.
- Some downloads were fetched into the session scratchpad, outside both
  repositories. Only their INF text, readmes, file lists, version resources
  and PE import tables were read. Nothing was run.
- Some sites refused automated fetches: VOGONS and MSFN (403),
  PCGamingWiki, and Yamaha's and Realtek's download gates. Facts from those
  sites rest on search-engine summaries and are marked unverified.

Every fact is marked as one of these:

- **V (verified):** read on the page or in the file named.
- **U (unverified):** with what would settle it.

## Verdicts

| Package | Verdict | Existing driver(s) | Gap our package keeps |
|---|---|---|---|
| `uvc98` | **C** | Only Logitech's own `lvuvc.sys` (QuickCam 10.5.1) on 2000, for six Logitech PIDs; Vimicro 2000 UVC (U). Both non-free and licence-barred | Generic UVC on 98 SE, ME and 2000: all of it |
| `uac2` | **C** | Thesycon TUSBAudio vendor builds (XP-8), C-Media OEM builds (XP-8): each locked to one vendor's VID/PID, not redistributable | Generic UAC 2 on every target |
| `usbmidi` | **C** (already scoped to the gap) | ME and XP-8: in-box class MIDI. Vendor drivers (Yamaha, Roland/Edirol, M-Audio) per VID/PID, all non-redistributable | 98 SE and 2000: generic MIDIStreaming. ME and XP-8: vendor-class devices. Both as designed. Owner, 2026-10-20: M-Audio MidiSport devices dropped (section 3.4) |
| `cdcnet` ECM, NCM | **C** | None below Windows 10/11 (UsbNcm), except commercial OEM-only drivers | All targets |
| `cdcnet` RNDIS | **B** | Microsoft's Remote NDIS USB Driver Kit for 98, ME and 2000 (EULA bars third-party and GPL-adjacent distribution). In-box `rndismp` on XP, Vista and 7, bound only by a device INF | The RNDIS driver on 98 SE, ME and 2000. Owner, 2026-10-20: on XP to 7, an INF of `cdcnet`'s over the in-box driver instead of our RNDIS code (section 4.5) |
| `rtl815x` | **B** | Realtek: XP 5.23, Vista 6.27, 7 7.61.20, all non-free. Their INFs cover `0BDA:8152`, `8050` and `8153` at every revision; the 7 driver adds `8053`, `8155`, `8156` and `8056`. Licence and redistribution terms U. Download is captcha-gated | Owner, 2026-10-20: 98 SE, ME and 2000 only; XP to 7 are not targets (section 4.5) |
| `ax88179` | **B** | ASIX: XP/Vista 1.4.2.0 (x86) and 1.6.2.0 (x64), 2013; 7 1.12.11.0 (x86) and 1.14.11.0 (x64), 2018. AX88179 and AX88178A; their INFs do not cover the AX88179A/B. Non-free; licence U | Owner, 2026-10-20: 98 SE, ME and 2000 only, for AX88179/AX88178A; XP to 7 are not targets (section 4.5). The current AX88179A/B default to CDC-NCM, so `cdcnet` serves them on every target |
| `usbcom` | **C** (already scoped) | None on 98 SE and ME. Microsoft's in-box stack is modem-only and VID/PID-bound; MCCI's driver is OEM-only. On 2000 to 8, `usbser.sys` by INF, as designed | CDC-ACM as a COM port on 98 SE and ME |
| `xpad98` | **B** | Masahiko Morii's `x360c` (360 wired: 98 SE, ME, 2000 to 7; 25 pad IDs in its final 2010 builds) and `x360wc` (360 receiver: 2000 to 7): free to download, no licence. XBCD 0.2.8 (GPL-2.0-or-later, 360 wired, XP and later) | Owner, 2026-10-20: Xbox One/Series pads, 98 SE to Vista. The 360 receiver on 98 SE and ME (on 2000, `x360wc` serves it). 360-protocol pads outside `x360c`'s 25 IDs (8BitDo, current clones). The XP to 8 opt-in mode is dropped; XBCD (free) covers XP and later (section 6.4) |
| `usbprn` | **C** | Microsoft's Windows 98 `usbprint.sys` 4.10.2223 and `usbmon.dll`, with a Microsoft 9x `usbprint.inf` binding `USB\Class_07`. Shipped only inside vendor packages (HP, Zebra, WCH). No official Microsoft source; the 98 redistribution allowance has lapsed | 98 SE USB printing: all of it, in free form |
| `usbflop` | **C** (98 SE note) | NUSB 3.6 binds `Class_08&SubClass_04&Prot_00` on 98 SE (`floppy.md` already records this). heha's 2007 class INF for 2000 (no licence, forces CBI on every protocol). Vendor floppy drivers, all VID/PID-locked | All systems as designed |
| `libusb0` | **A** (already) | libusb-win32 0.1.12.2, GPL-2.0-or-later (`libusb.md`) | None: `libusb.md` already adopts the existing driver |

None of the existing drivers found for any package is free software that
could be adopted here, apart from XBCD and libusb-win32. Only these
qualify for the non-free pack under the provenance rule (an official
source and a licence text):

- the ASIX AX88179 drivers for XP to 7;
- possibly Realtek's XP to 7 drivers, once their terms are read;
- Morii's `x360c`, only if its author grants terms.

Owner, 2026-10-20: Morii's `x360c` and `x360wc` go in the non-free pack
as NUSB is carried there (from Morii's own site, provenance recorded), in
their final 2010 builds, and the Realtek and ASIX drivers are read before
either is added (section 12). That reading is done: `rtl815x` and
`ax88179` narrow to 98 SE, ME and 2000 (section 4.5).

## 1. `uvc98`: UVC webcams on 98 SE, ME and 2000

### 1.1 Microsoft `usbvideo.sys`

- Ships with XP SP2 and later only (KB899271 for XP SP2;
  `en.wikipedia.org/wiki/USB_video_device_class`). V.
- **On 2000 it does not load.** The INF installs, but the driver fails with
  Code 31 because `WmiTraceMessage` and `WmiQueryTraceInformation` are
  missing from 2000's `ntoskrnl`. V:
  - OSR NTDEV, June 2007,
    `https://community.osr.com/t/can-usbvideo-sys-work-on-win2000-me/28826`;
  - MSFN "Windows 2000 on Haswell?" pages 5 to 7, October 2014,
    `https://msfn.org/board/topic/170950-windows-2000-on-haswell/page/5/`.
- **BlackWingCat's Extended Core does not fix it.** He wrote that "Windows
  2000 has not been supported UVC" (MSFN 2014; V, from fetched summaries).
- On ME its INF has only an NT section. V, from the OSR thread.
- Microsoft (Doron Holan, OSR thread): OS components may not be
  redistributed to earlier systems. Non-free.

### 1.2 Logitech QuickCam 10.5.1 (`lvuvc.sys`)

- **What it is:** Logitech's own AVStream UVC driver. It imports only
  `ntoskrnl`, `hal`, `ks.sys` (`KsInitializeDriver`) and `USBD.SYS`, and no
  WMI.
- **Version:** `lvPRO3v.inf`, `DriverVer` 02/03/2007, 10.5.1.2023. V.
- **Systems:**
  - 2000 SP4 and XP. Its readme says "High speed 2.0 cameras will only
    function in full speed mode in Windows 2000". V.
  - Not 98 or ME: `Setup.ini` sets the minimum OS to 5.0.2195. V.
- **Devices:** the `[Logitech.NT]` section binds only
  `USB\VID_046D&PID_08C1/08C2/08C3/08C5/09C0/08C9/08CA/08CB/08CC/08CE/09C1&Mi_00`.
  These are Logitech cameras of 2006-07. V.
- **Status:** 10.5.1 is "the last supported QuickCam release for Windows
  2000". V:
  `https://support.logi.com/hc/en-us/articles/360023373333-QuickCam-11-Release-Notes-and-Version-History`.
- **Source:** no official download found. The copy read is a third-party
  archive.org item, `https://archive.org/details/logitech-quickcam-version-10.5.1`
  (uploaded 2021-11-04).
- **Licence:** `license.txt` grants use "on one computer only with the
  Logitech product you have purchased" and forbids distribution and
  derivative works. V. Non-free, and it **cannot go in the non-free pack**.
- Whether it drives other UVC cameras with an edited INF is U. Editing the
  INF would breach the licence anyway.

### 1.3 Other vendors

- **Vimicro.** Aggregator listings name a "Vimicro_UVC_WIN2K_WINXPSP1_Setup"
  and `vmcam332.inf` 3015.2000.3001.15 (2007), with 2000 in the OS list.
  All U: no INF was obtained (downloads sit behind redirect spam). Probably
  bound to VID 0AC8.
- **Sonix `snp2uvc.sys`.** The INF read is decorated for XP and Vista only
  and binds one PID (`04F2:B028`). V:
  `https://contents.driverguide.com/content.php?id=1215684&path=snp2uvc.inf`.
- **Pre-UVC chip drivers for 98 and ME** (Vimicro ZC03xx, Sonix SN9C1xx,
  Z-Star): many exist, all vendor-mode drivers tied to their PIDs. None is
  labelled UVC (U for each package).
- **Logitech on 98 and ME:** the last version is QuickCam 8.4.8, which has
  no UVC cameras (Logitech support pages). V.
- **Microsoft LifeCam:** XP SP2 or Vista. Search-level evidence, U.

### 1.4 Community work

- MSFN topic 177015 ("Windows 9X/ME - USB 2.0 Webcams"), 2023-12-20:
  MERCURY127 reports a homemade "generic usb a/v driver, using wdmex and sys
  files from XP" capturing from a cheap endoscope on 98 SE. V that the post
  says so.
  - The attachment was password-protected and has been removed.
  - Nobody else reproduced it.
  - It redistributes XP binaries.
  - Not usable.
- No free UVC driver for any Windows before XP was found.

### 1.5 Verdict: C

- **98 SE and ME:** nothing.
- **2000:** non-free, vendor-locked, licence-barred drivers for a few
  2006-07 cameras.
- **Keep `uvc98` as designed.**

## 2. `uac2`: USB Audio 2.0, 98 SE to 8

- **Thesycon TUSBAudio** (`thesycon.de/eng/usb_audiodriver.shtml`, V from
  archive.org snapshots):
  - Systems:
    - 2013 to 2015 (2.x): XP, Vista, 7, 8 and 8.1.
    - 2016 (3.29): 7 to 10 only.
    - Never 2000, 98 or ME.
  - The engine is class-generic, but "Customization is a mandatory step"
    (FAQ): every shipped build is bound to one licensee's VID/PID.
  - Commercial; not redistributable by us.
- **Vendor builds on old TUSBAudio:**
  - Benchmark DAC2: `tusbaudio.sys` 1.61.0 (2012). Its `DAC2.inf` binds only
    `USB\VID_1781&PID_0E4F`. Its page says XP, Vista, 7, 8 and 10. V,
    `https://benchmarkmedia.com/pages/dac-drivers`, package inspected.
  - Cambridge Audio v1.67 (7). V.
  - Behringer UMC driver 2.29 "for Windows XP + Vista". U, aggregators only.
  - Each one serves only its vendor's devices.
- **XMOS:** Thesycon-based. Its evaluation driver is "Locked to XMOS VID &
  PIDs" and runs on 7 to 10 (XMOS XM-003139-PC, 2016). V.
- **C-Media CM6631A/CM6632A ("USB Audio Class 1.0/2.0 UAA Driver"):** a
  PortCls driver.
  - The ASUS Xonar U7 XP package (5.12.11.19, 2013) binds only
    `USB\VID_1043&PID_857C&MI_00` and `PID_85C1`. V, package inspected
    (`dlcdnets.asus.com/pub/ASUS/Audio_Card/Xonar_U7/`).
  - Other OEM builds are VID-locked too. U, aggregators.
- **Class-bound hack:** Realtek USB Audio 6.4.0.393 with a modified INF
  binding `USB\Class_01&SubClass_0x&Prot_20`, from the WinRAID thread
  `https://winraid.level1techs.com/t/request-help-usb-audio-class-2-support-pre-win10-1703/96769`.
  V, INF read.
  - x64 only.
  - The catalog signature is broken.
  - Users report static or silence on 7 SP1 and 8.1.
  - Proprietary.
- **Open source:**
  - Microsoft `low-latency-audio` (MIT; ACX, Windows 11 era; "no public
    release of the driver, yet"). V. Usable only as reference.
  - Audio-Widget ASIO_UAC2 (user-mode ASIO over libusbK; licence U). Not a
    WDM sound device.
- **Backports of `usbaudio2.sys`:** fail with Code 39 or leave the system
  unbootable (WinRAID thread). V.
- **98 SE, ME and 2000:** no UAC 2 driver of any kind was found.

**Verdict: C.** No generic or redistributable driver exists on any target.
Keep `uac2`.

## 3. `usbmidi`: USB MIDI 1.0

### 3.1 In the box

Microsoft Learn, "USB Audio Class System Driver (Usbaudio.sys)", V:

- "Support for MIDI devices was added in Windows Me."
- 98 SE and 2000 have no USB MIDI.

This matches `usbmidi.md`. No 2000 hotfix adding MIDI was found (U for
absence).

### 3.2 Community packs on 98 SE

- **98SE2ME (MDGx):** "option 5" installs ME's `USBAUDIO.SYS` 4.90.3001
  (Q280127), `WDMA_USB.INF` (renamed `*ME.INF`), `PORTCLS.SYS` and
  `KMIXER.SYS` into 98 SE, from the user's own English ME CD. V,
  `http://web.archive.org/web/20250814013230id_/https://www.mdgx.com/9s2m/READ1ST.TXT`.
  - No report of USB MIDI working through it was found.
  - Whether the renamed INF outranks 98 SE's own is U.
  - It needs an ME CD, so it is not a driver anyone can ship.
- **U98SESP 3.63:** ships 98 SE's own QFE `USBAUDIO.SYS` 4.10.2224 and no
  INF change. No MIDI. V, inspected.
- **Community threads:** VOGONS t=46741 (2016) and MSFN 135789 (2009)
  ended unresolved. V, archive copies.

### 3.3 Vendor drivers

| Driver | Systems | Devices | Source | Licence |
|---|---|---|---|---|
| Yamaha USB-MIDI Driver V1.2.2 (`um122me9.zip`, 2004) | 98, 98 SE, ME | About 70 INFs, all `USB\VID_0499&PID_xxxx`. No class match | `https://europe.yamaha.com/en/support/updates/umd-v122-win-kbd.html` (login). V | "may not reproduce ... or distribute". Non-free |
| Yamaha V2.3.3 (bundled with V3.1.3) | 2000 SP4 | Presumed VID 0499 only (U) | `https://europe.yamaha.com/en/support/updates/umd-v313-win32-kbd.html`. V that it exists | As above |
| Roland/Edirol UM-1 2.00 (2005) | 98/ME, and 2000/XP (separate packages) | `USB\VID_0582&PID_0009` only, and only in "advanced driver" mode. V | roland.com support | "distribution ... prohibited". Non-free |
| Roland UM-2, UM-3, UM-880, PCR | 98/ME and/or 2000/XP | One product each (listing V, INFs not read) | roland.com | Non-free |
| M-Audio MIDISPORT/Keystation, driver 4.1.21 | 98, ME, 2000, XP | Per-product INFs (`USBKS1X1.INF` and others). IDs U | `https://vogonsdrivers.com/getfile.php?fileid=1552`; CD at `https://archive.org/details/midisport-series` | Not read. Non-free |
| Korg | Legacy driver is for serial TO-HOST only. No USB-MIDI driver for 98 or 2000 found | - | korg.com. V | - |

The same Microsoft MIDI issue (V) explains why ME to 8 users need vendor
drivers for some devices:
`https://github.com/microsoft/MIDI/issues/1234`. Many Yamaha devices
declare class FF with no standard MIDIStreaming interface.

Not searched (the search budget ran out): E-mu Xmidi, CME, Novation,
Behringer, Steinberg Midex 8 (a 2001 review says 98 SE/ME/2000; U).

### 3.4 Verdict: C

- **98 SE and 2000:** nothing serves class-compliant MIDI devices (cheap
  cables, CH345, Keystations, Roland units in class mode). Named vendor
  devices have non-free, non-redistributable drivers.
- **ME and XP to 8:** the gap is the vendor-class devices, as `usbmidi.md`
  already scopes it. The drivers that exist for them forbid distribution,
  so none can go in the non-free pack.
- **Keep `usbmidi` as designed.**

Owner, 2026-10-20: M-Audio MidiSport devices are dropped from `usbmidi`'s
scope. M-Audio's own driver supports 98, ME, 2000 and XP and is officially
downloadable (archived), but its licence forbids redistribution, and a
free driver would need the same restricted firmware. The non-free pack
records the download links only. `usbmidi` covers class-compliant devices
and the vendor-class families of `usbmidi.md` section 7.

## 4. `cdcnet`, `rtl815x`, `ax88179`: USB Ethernet

### 4.1 Realtek RTL8152/RTL8153 family

- **Current page** (`https://www.realtek.com/Download/List?cate_id=585`, V):
  - Windows 10 10.68.20, 8/8.1 8.68.20.
  - **7 / 2008 R2 7.61.20 (2021-12-28)**.
  - **Vista 6.27**.
  - **XP "for FE/GBE" 5.23**.
  - Archive snapshots (2019, 2021) date XP 5.23 and Vista 6.27 to
    2018-03-06. V.
  - No entry for 2000, ME or 98.
- **2000:** TechSpot labels a mirrored 5.13 "Windows XP and 2000". That is
  third-party, and Realtek says XP only. U.
- **Hardware ids** (owner's reading, 2026-10-20; the download needs terms
  and a captcha):
  - XP 5.23, Vista 6.27 and 7 7.61.20 all cover `0BDA:8152`, `0BDA:8050`
    and `0BDA:8153` at every chip revision.
  - The 7 driver adds `0BDA:8053`, `8155`, `8156` and `8056`.
- **Licence text:** not read. U.
- **Community:** no RTL815x driver for 98, ME or 2000 was found. U for
  absence.
- **Non-free.**

### 4.2 ASIX AX88179 / AX88178A

ASIX page: `https://www.asix.com.tw/en/support/download/step2/11/2/3`. V.

| Package | Date | SHA-256 (fetched as data) |
|---|---|---|
| `AX88179_178A_WinXP_Vista_32bit_Driver_v1.4.2.0_WHQL.zip` | 2013-05-24 | `86272f515eefb147b60fc032e09ad261d43f2d19215e74c90b41aeef6d49e567` |
| `AX88179_178A_WinXP_Vista_64bit_Driver_v1.6.2.0_WHQL.zip` | 2013-05-24 | `1b4553504b898ba645a643426887cfa11c0b255ecfa309db6979cffc8ed8964d` |
| `AX88179_178A_Win7_64bit_Driver_v1.14.11.0_WHCK_20180119.zip` | 2018-01-19 | `8458bf955d7be2df6df7abfb137d2848b30ae6a05ffae4c7b6ab5124162063a5` |

The 7 32-bit package (1.12.11.0) was listed but not fetched.

- **XP/Vista INF** (V):
  - NDIS 5, KMDF; ships `WdfCoInstaller01009.dll`.
  - Binds `0B95:1790` (AX88179), `0B95:178A` (AX88178A) and three OEM IDs
    (Lenovo, Sitecom, Toshiba).
- **Windows 7 INF** (V): the same IDs plus Samsung, Belkin, MCT and Allied
  Telesis IDs.
- **2000: not covered.**
  - KMDF 1.9 runs on XP and later only (Microsoft Learn KMDF version
    history). V.
  - OSR, 2019-02-09: the XP/Vista driver fails with Error 31 on 2000 SP4.
    V.
  - Plugable, 2017: ASIX "doesn't offer drivers for Windows 2000". V.
- **98 SE and ME:** nothing.
- **Current production parts** (AX88179A, AX88179B, AX88772D/E, AX88279):
  - ASIX lists Windows 8 and later only. V.
  - These chips present **CDC-NCM by default**. V: ASIX's AX88179A product
    page; Linux patch v11 on LWN, `https://lwn.net/Articles/1094282/`.
  - So `cdcnet`'s NCM support serves them on every target.
  - The 2013 and 2018 drivers' INFs do not cover the A and B revisions
    (owner's reading, 2026-10-20), so on XP to 7 too they go through
    `cdcnet`'s NCM.
- **Licence:** the ZIPs contain no licence file. Terms U. Non-free.
- **For the record (AX88772):** ASIX's archived pages list "Windows ME/98SE
  v3.0.3.12, 2005-09-09". The non-free pack already carries that driver
  (`non-free/ethernet/ax88772/w98se-me/`) and ASIX's 2000/XP build
  (`non-free/ethernet/ax88772/w2k-xp/`).

### 4.3 RNDIS

- **Microsoft "Remote NDIS USB Driver Kit"** (V, archived pages and
  packages):
  - `rndisusb1-1.exe` (2002-08-16) holds `usb8023w`/`rndismpw` (98, 98 SE),
    `usb8023m`/`rndismpm` (ME) and `usb8023k`/`rndismpk` (2000), all
    5.2.3667.0, with a template INF and no hardware ids.
    - Official URL, now 404:
      `http://download.microsoft.com/download/whistler/RND/1.0a/WXP/EN-US/rndisusb.exe`.
    - The 2002 build (archived 20020808) has SHA-256
      `e6c50cb8cb5a633d4792bb1e48039d68ec9507b5b4f62b87b48e2ff8a18d900e`.
    - 1.1 (archived 20040219) has SHA-256
      `eee88bff7d755a6c76c2e3524caa5d613adb5f0df93b9bd777f7e7f09819661b`.
  - The 2005 kit (`RNDIS-USB-Kit_05.exe`, SHA-256
    `8fad4f7dc6d7bc63f2a27ed64ca27f99817cf7127f807fe7af36cd89e82fd7c2`)
    is for 2000 only.
  - **EULA** (`IHV_RNDIS_USB_EULA.doc`, V). Redistribution is allowed only:
    - "in conjunction with and as a part of your hardware products";
    - not for profit;
    - unmodified;
    - and not with copyleft ("Identified") software.

    **So it cannot go in the non-free pack** and cannot be shipped next to
    GPL drivers.
- **XP:** `rndismpx.sys`/`usb8023x.sys` are in the box. `netrndis.inf` is a
  template with no hardware ids, so a device INF is needed: Linux's
  `linux.inf`, or community Android "tether" INFs (V that they exist).
  KB959765 is a hotfix (5.1.2600.3483), not an INF. V.
- **Vista and 7:** `rndismp` is in the box. Windows 7's `rndiscmp.inf`
  does not match Android's `E0/01/03` (`ethernet.md`). An INF that names
  the class ids would bind it; whether it then works is U (test).
- **Gap for `cdcnet` RNDIS:**
  - On 98 SE, ME and 2000 no shippable driver exists, so ours stays.
  - On XP to 7, Microsoft's in-box RNDIS driver could serve Android
    tethering and RNDIS gadgets through an INF of ours, the same pattern as
    `usbcom` on 2000 to 8. Our RNDIS code would then build only for 98 SE
    to 2000.
  - Owner's choice: one RNDIS implementation everywhere (as `ethernet.md`
    decides), or INF-only on XP to 7.

### 4.4 CDC-ECM and CDC-NCM

- **Microsoft:**
  - No CDC-ECM driver in any Windows version (Microsoft Learn, "USB device
    class drivers included in Windows"). V.
  - `UsbNcm.sys` is listed for Windows 11 / Server 2022. V. Its MIT source,
    `https://github.com/microsoft/NCM-Driver-for-Windows`, is NetAdapterCx,
    for Windows 10/11 only.
- **Commercial:**
  - Belcarra USBLAN: Windows 10/11, with an OEM kit for 7 to 8.1; the
    evaluation stops after 30 minutes. V.
  - Thesycon CDC ECM/NCM: Windows 10/11. U.
  - MCCI: OEM licence only. V.
  - None is free or end-user redistributable.
- No free ECM or NCM miniport for 98 to 7 was found. U for absence.

### 4.5 Verdicts

- **`cdcnet` ECM and NCM: C**, on all targets.
- **`cdcnet` RNDIS: B.** The gap is 98 SE, ME and 2000. XP to 7 could be
  served by an INF over Microsoft's in-box driver (section 4.3).
  - Owner, 2026-10-20: on XP to 7, RNDIS is an INF of `cdcnet`'s that
    binds Windows' own RNDIS driver by class id, with Android tethering
    verified on each system. `cdcnet`'s own RNDIS code targets 98 SE, ME
    and 2000 only (`ethernet.md` section 7.5).
- **`rtl815x`: B.**
  - Gap: 98 SE, ME and 2000.
  - XP to 7: Realtek's own drivers (XP 5.23, Vista 6.27, 7 7.61.20) cover
    `0BDA:8152`, `8050` and `8153` at every revision (section 4.1).
    `cdcnet` ECM also covers the RTL8153 there.
- **`ax88179`: B.**
  - Gap: 98 SE, ME and 2000, for AX88179 and AX88178A.
  - XP to 7: ASIX's own drivers cover the AX88179 and AX88178A.
  - AX88179A/B and later, which ASIX's XP to 7 INFs do not list, are
    served through `cdcnet` NCM on every target.
- Owner, 2026-10-20, on `rtl815x` and `ax88179`: Realtek's and ASIX's XP
  to 7 drivers were read first (INFs, chip revisions).
- Owner, 2026-10-20, after that reading: both packages target 98 SE, ME
  and 2000 only; XP to 7 are not targets. Both come after `cdcnet`.

## 5. `usbcom`: CDC-ACM serial

`serial.md` section 2 already covers the vendor bridge chips (FTDI,
CH340/CH341, CP210x, PL2303). This section adds only the CDC-ACM search.

- **Microsoft's 98 SE stack** (`usbser.sys`, `ccport.sys`,
  `wdmmdmld.vxd`):
  - It makes a VCOMM port, for modems only. V: KB 308349, archived
    `https://web.archive.org/web/20031030161106/http://support.microsoft.com:80/default.aspx?scid=kb;en-us;308349`.
    That KB gives `usbser.sys` 4.10.0.2224 as a support-only hotfix for a
    shutdown hang.
  - `usbcdc.inf` binds three modems by VID/PID (as `serial.md` 2.5 found).
  - No working INF that turns it into a Ports-class COM port was found.
    One 2004 attempt failed: "NTKERN couldn't load it",
    `https://community.osr.com/t/emergency-call-questions-on-windows-98-inf-for-a-usb-cdc-modem/19943`.
  - Non-free.
- **Community packs:**
  - 98SE2ME copies ME's `USBSER.SYS` and `USBCDC.INF`; V, file list.
    ME's INF is modem-only too (`serial.md` 2.5).
  - U98SESP3's USB add-on drops in XP's `USBSER.SYS` 5.1.2600.6442 with no
    INF referencing it, so it is a file swap only. V.
- **Commercial:**
  - MCCI WMC/CDC drivers list "Win98, 98SP1, 98SE, 98ME, Win 2000, XP" (V,
    datasheet 971534e, 2005,
    `https://mcci.com/wp-content/uploads/2018/01/971534e.pdf`), but are
    OEM-licensed and rebranded per device. No public download. Non-free and
    not obtainable.
  - Thesycon CDC/ACM: 2000 to Vista in 2007, later 7 and up. V. Commercial.
- **Silicon vendors:**
  - Microchip AN956 (2004): INFs for 2000/XP only. V.
  - Atmel 4322A (2004): INF for 2000/XP. V.
  - ST STSW-STM32102: its data brief lists "98SE", but its `stmcdc.inf`
    1.3.1 has NT sections only. V.
  - WCH CH343SER (CH9102/CH343): "Windows 11/10/.../XP/2000", no 98/ME. V,
    archived wch-ic.com page and INF 1.7. Also vendor-mode, VID 1A86 only.
  - None has a 9x path.
- **Open source:** none for any Windows.
  - ReactOS has no `usbser`. V.
  - libwdi's CDC INF is for 7 to 11. V.
  - Linux's `linux-cdc-acm.inf` is NT-only. V.
- **2000:** `usbser.sys` with a vendor INF works as a COM port per
  Microchip AN956, Atmel 4322A and Linux's `linux-cdc-acm.inf` (V, vendor
  documentation). This confirms `serial.md`'s INF-only plan. A user report
  specific to 2000 was not found.

**Verdict: C.** Keep `usbcom` as `serial.md` scopes it.

Owner, 2026-10-20, on the vendor bridge-chip drivers of `serial.md`
section 2 for the non-free pack: FTDI VCP 1.0.3, CDM 2.06.00 and WCH
CH341SER are carried, with their long file names as shipped. Prolific's
PL2303 (restrictive EULA) and Silicon Labs' CP210x (its EULA allows
distribution only inside the licensee's product) are not carried; the
pack gives their download links only.

## 6. `xpad98`: Xbox pads

### 6.1 Masahiko Morii, `x360c` (wired) and `x360wc` (wireless receiver)

- **Pages:**
  - Original, now 404: `http://www.katch.ne.jp/~morii/driver/x360c/index.html`.
    Archive snapshots run from 2009-04-01 to 2011-09-03 (V, CDX).
  - Live mirror:
    - `http://lavendy.net/special/driver/x360c/index.html`
    - `http://lavendy.net/special/driver/x360wc/index.html`

    Both are marked "development ended". V. Whether lavendy.net is the
    author's own site is U.
- **Builds** (V):
  - `x360c`: 98 SE/ME, 2000, XP x86/x64, Vista and 7.
  - `x360wc`: the same list without 98/ME.
- **The 98/ME build**, `x360c.w98.x86.en.zip` (files dated 2010-05-04,
  71,016 bytes), was read as data. V.
  - SHA-256
    `32c9ea96d43b5c94287402b258a7a977b8f60e68a9d24587d7ed13b122591657`
    (the copy fetched for this survey; the same hash as the
    oemdrivers.com copy).
  - It is a HID minidriver (`DevLoader=*ntkern`), with DirectInput
    property-sheet and force-feedback DLLs.
  - Morii's final builds match **25 distinct VID/PIDs** (Microsoft
    `045E:028E`, HORI, Mad Catz, Logitech, Saitek; the full list is in
    `gamepad.md` section 5.1). On 98 SE/ME they are split across
    `x360c.inf`, `x360c_hori.inf` and `x360c_mdcz.inf`. No 8BitDo, no
    current clones, no class match. (Owner, 2026-10-20, from the non-free
    collection's check of the final builds; an earlier count here said 28.)
  - The readme lists these known bugs: the pad is lost after standby; most
    force-feedback effect types are unsupported; no XInput.
  - Users report duplicate HID devices and install failures. V: MSFN topic
    119841 page 3, and
    `https://retrosystemsrevival.blogspot.com/2018/05/xbox-360-controller-driver-for-windows.html`.
- **The 2000 builds** need SP4 (V).
  - `x360wc` matches the receiver `045E:0719` in all builds, plus
    `045E:028F` (Play & Charge Kit) from XP on.
  - BlackWingCat (2010) reports the 2000 build "doesn't fully work" without
    his `fcwin` tool. V:
    `http://blog.livedoor.jp/blackwingcat/archives/1104543.html`.
- **Source:** published for 98/ME (dated 2006, older than the 2010 98
  binary), 2000 (2009), XP to 7 (2010) and wireless (2009). V.
- **Licence: none stated anywhere.** The pages only say "use at your own
  risk". So it is all rights reserved: not free, and not packageable under
  the provenance rule without the author's permission.
- **Redistributed anyway** by:
  - U98SESP3's USB add-on (V, its INF names "Masahiko Morii");
  - MDGx's `X360C.EXE` (V);
  - a 2024 archive.org repack (V).

### 6.2 XBCD

- **XBCD 1.07 (Redcl0ud, 2005):**
  - Original Xbox (XID) pads only, on 98, 2000 and XP.
  - V: `https://web.archive.org/web/20180730215135/http://redcl0ud.com/xbcd.html`.
- **XBCD 0.2.x "360" line:**
  - `https://sourceforge.net/projects/xbcd/`, SVN r45, last commit
    2016-02-04; trunk is 0.2.8 (2012-09-19). V.
  - **GPL-2.0-or-later** (headers of `XBCD_driver.c` and `Readme.txt`). V.
  - Detects 360 pads by interface `FF/5D/01`, so clones are served. V.
  - Its INF (DriverVer 1.1.0, 2008) is `$Windows NT$` with NTx86/NTAMD64
    models only. The receiver `045E:0719` is commented out as unsupported.
    V.
  - The code keeps Win9x paths (`isWin9x`, a RemoveLock shim). V.
  - 0.2.7 is reported for XP and later (U, search snippet). Works on 7/10
    32-bit (V, `https://www.s-config.com/xbcd-original-xbox-controllers-win10/`).
  - Not reported working on 98 SE: an MSFN user in 2015 found it did not
    see a 360 pad.

### 6.3 Others

- **Microsoft `xusb21` 2.1.0.1349:** BlackWingCat installed it on 2000 with
  an edited INF in 2020, and rumble worked in XInput mode. V:
  `https://win2k.org/blog/2020/05/21/1997719/`. Microsoft EULA; non-free,
  not redistributable.
- **Xbox One/Series:**
  - Microsoft's first PC driver (2014-06-05) is for 7/8 (U for the exact
    OS range). Non-free.
  - No XP or older driver exists. A 2026 GitHub "XP x64" repository is an
    empty placeholder. V.
- **References only:** `roblambell/XboxOneController` (user-mode XInput
  DLL, Apache-2.0), Linux `xone` and `xbox_gip`, Mac 360Controller
  (GPL-2.0).

### 6.4 Verdict: B

- **Xbox 360 wired, XP to 8 x86 (the planned opt-in mode):** covered by
  XBCD, which is free (GPL-2.0-or-later). Drop that mode from `xpad98`,
  and adopt XBCD 0.2.8 as a free package here if the owner wants it
  shipped. Alternatively, keep the mode and take XBCD as a source reference.
- **Xbox 360 wired on 98 SE, ME and 2000:** Morii's `x360c` works, with
  known bugs, but has no licence and a fixed 25-ID list.
  - If Morii grants redistribution terms, it goes in the non-free pack and
    `xpad98`'s 360 wired front end on these systems shrinks to the devices
    it lacks: IDs not on its list, such as 8BitDo and current clones.
    Otherwise the gap stays whole.
  - A second route: build from XBCD's GPL code, which already has 9x paths,
    with a 9x/2000 INF.
- **360 Wireless Receiver:**
  - 98 SE and ME: not covered.
  - 2000: `x360wc` (no licence, `045E:0719` only) and `xusb21` (non-free).
    Gap stays.
- **Xbox One/Series:** not covered on any target. Keep.

Owner, 2026-10-20:

- Morii's `x360c` and `x360wc` go in the non-free pack, as NUSB is
  carried there: from Morii's own site, with provenance recorded.
  `x360c` stays the 360-pad driver there, in Morii's final 2010 builds
  (the 25 pad IDs of section 6.1), and `x360wc` (2000 to 7) with it.
- `xpad98` keeps:
  - Xbox One and Series pads, 98 SE to Vista;
  - the 360 wireless receiver on 98 SE and ME only; on 2000, `x360wc`
    serves it;
  - 360-protocol pads outside `x360c`'s 25 IDs (8BitDo, current
    clones).
- The XP to 8 opt-in mode is dropped: XBCD covers XP and later.

`gamepad.md` section 1 carries this scope.

## 7. `usbprn`: USB printing on 98 SE

- **Microsoft's 98 build** (V, version resources and INF text read):
  - `usbprint.sys` 4.10.2223.1 and `usbmon.dll` 4.10.2223.
  - A Microsoft `usbprint.inf` (`$CHICAGO$`, `DriverVer=12/17/1999`)
    binding `USB\Class_07` ("USB Printing Support") that installs
    `usbprint.sys` but does not register `usbmon`.
  - INF text: `https://contents.driverguide.com/content.php?id=239148&path=USBPRINT.INF`
    (a Zebra/Eltron package).
  - These files reached users only inside vendor packages:
    - HP LaserJet 3200 for 98 (DriverGuide listing; V, listing only).
    - Zebra/Eltron.
    - WCH `CH341PRT.ZIP` v1.2 (2007-09-17). Archived
      `https://web.archive.org/web/20160405035724/http://winchiphead.com/download/CH341/CH341PRT.ZIP`;
      its WIN98 INF binds only WCH's `1A86:7584`, `1A86:5584` and
      `4348:5584`, and registers `usbmon`.
  - **No Microsoft download, update or KB for it was found.** U for
    absence.
  - Jan Axelson (2006 archive of `janaxelson.com/usbwin.htm`): "Until
    Microsoft ends support for Windows 98, the printer driver is allowed to
    be distributed for use on Windows 98 systems." V. Support ended in 2006.
  - A Lexmark note reportedly says Microsoft ended those bundling licences
    (U, the page is 404).
- **U98SESP3:**
  - Carries `USBPRINT.SYS` 4.10.2223.1 and `USBMON.DLL` 4.10.2223, but with
    replace-only copy flags (0x420) in `SPUPDATE.INF`. It updates them if
    present and does not add USB printing. V.
  - The 3.19 changelog calls it a "Win98 Hotfix" (V,
    `https://www.htasoft.com/u98sesp/index.htm`). The hotfix number is U.
- **98SE2ME:** copies ME's `USBPRINT.SYS` from the user's ME CD, with no
  `USBMON.DLL` and no printer INF in its file list. V. Whether printing
  then works is U.
- **NUSB 3.6:** no printer support; it deletes `usbprint.sys` from
  `SYSTEM`. V.
- **Printer and cable vendors:**
  - Epson ("EPSON USB Printer Devices", EPUSB port) and Canon ("USB Class
    Driver" for 98/ME) have their own 98 port drivers. Class binding and
    terms U.
  - WCH's CH341 paths are VID-locked (above).
  - No 98 driver was found for Prolific PL-2305, Belkin F5U002 or MosChip
    MCS7705. U.

**Verdict: C.**

- A class-generic 98 SE printer driver exists only as Microsoft's
  binaries, with no official source and a lapsed redistribution allowance.
  So it fails the non-free pack's provenance rule, as the unofficial
  PL2303 driver did (`serial.md` section 3).
- Keep `usbprn`.
- If an official Microsoft or vendor download with terms that allow
  redistribution turns up, the owner may prefer to carry that in the
  non-free pack. That would make the verdict A for printers. Our port
  monitor would still be needed with the bare Microsoft INF, which does not
  register `usbmon`.

## 8. `usbflop`: UFI floppies by class

- **Microsoft's `usbstor.inf`** binds floppies only by VID/PID on every
  system:
  - `floppy.md` section 2, read from the install media;
  - Windows 11's INF on this host;
  - Microsoft Learn, "Identifiers generated by USBSTOR.SYS".

  V. KB 327242 (an XP SP2 hotfix) sets `DriverFlags` per device and adds no
  class binding. V,
  `https://mskb.pkisolutions.com/kb/327242`.
- **NUSB 3.6** (`usbstor.inf` 01.11a) binds `Class_08&SubClass_04&Prot_00`
  generically. `Prot_01` and `Prot_50` are not bound. V. This matches
  `floppy.md` section 5. The non-free pack carries NUSB 3.3's INF, which
  has VID/PIDs only.
- **heha (Henrik Haftmann), "Allgemeiner USB-Floppy-Treiber" (April 2007):**
  archived
  `https://web.archive.org/web/20240425132235/https://www-user.tu-chemnitz.de/~heha/hsn/t_und_m.zip/USB-Massenspeicher/USB-Floppy/Win2k/usbstor.inf`.
  V.
  - **For 2000:** a modified `usbstor.inf` with
    `USB\Class_08&SubClass_04` mapped to `USBSTOR_CBI`.
    - It forces CBI (`DriverFlags=2`) for CB drives too. That is a likely
      fault (inference).
    - NT sections only.
  - **For 98:** an edited TEAC INF that rebinds TEAC's own non-free
    `teacufdd.sys`/`teacvsdd.vxd` to `Class_08&SubClass_04`.
  - No licence stated. The site has moved behind a bot challenge
    (`https://heha.fwh.is/`).
- **Vendor floppy drivers** (IBM/Lenovo `usbfdw9x` 2.00 2004, Fujitsu
  USBFDDW98 4.51 2001, TEAC, Y-E Data) are all VID/PID-locked and non-free.
  V for the first two; Fujitsu forbids redistribution.

**Verdict: C.**

- No class INF with a licence exists.
- heha's 2000 INF is prior art but carries a CB/CBI error and no terms.
- On 98 SE, NUSB 3.6 already covers `Prot_00` if the user installs full
  NUSB 3.6. Our 9x INF stays as `floppy.md` designs it, binding by class
  the `usbstor.sys` of whichever USB storage driver set is installed, and
  covering `Prot_01` as well.

## 9. `libusb0`

`libusb.md` already rebuilds the existing free driver, libusb-win32
0.1.12.2 (GPL-2.0-or-later), rather than writing one. That is verdict A in
this document's terms, and nothing here changes it.

## 10. Community packs, by class (summary)

The packs inspected:

- **98SE2ME** (MDGx; last changelog 2006-02-14). Needs the user's ME CD.
  `http://web.archive.org/web/20150602080451/http://www.mdgx.com/9s2m/9S2MLIST.TXT`.
- **U98SESP 2.1a** (Gape, 2005), `https://archive.org/details/sesp21a-en`.
- **U98SESP3** (PROBLEMCHYLD; 2018 build read; 3.66 is from 2021, U),
  `https://archive.org/details/u98sesp3alt`.
- **NUSB 2.0, 3.0, 3.3, 3.5 and 3.6** (Maximus-Decim).
- **For 2000:** BlackWingCat Extended Kernel 3.0e and Extended Core v16a;
  Unofficial SP5 (Gurgelmeyer, 2006).

All of them redistribute Microsoft binaries without Microsoft's
permission. V for each pack's own terms.

| Class | 98 SE | ME | 2000 |
|---|---|---|---|
| Printer | 98SE2ME: ME `USBPRINT.SYS` only. U98SESP3: replace-only update. Neither adds working class 07 support (V; whether 98SE2ME works is U) | No pack | No pack |
| CDC serial | File updates only; in-box modem-only binding unchanged | None | None |
| Audio/MIDI | 98SE2ME: ME `USBAUDIO.SYS` + INF from the user's ME CD; MIDI unproven | None | Extended Core v16a: XP `wdmaud`/`drmk`; its `usbaudio.sys` lines are commented out (V) |
| UVC | None (98SE2ME has only USBCAMD libraries) | None | None; XP `usbvideo.sys` fails (section 1.1) |
| RNDIS/ECM | 98SE2ME copies ME's `USB8023.SYS`/`USB8023.INF`; whether it serves RNDIS devices is U | None | None |
| Xbox pads | U98SESP3 add-on: Morii `x360c` (section 6.1) | None | None (`xusb21` by hand, section 6.3) |
| Mass storage | NUSB, 98SE2ME, U98SESP3 add-on | In box | In box |
| Composite (`usbccgp`) | NUSB 3.5/3.6 and U98SESP3: XP `usbccgp.sys` | None | In box |

KernelEx (GPL-2.0, 4.5.2, 2013) is a user-mode API layer and touches no
drivers. V, `sourceforge.net/projects/kernelex`.

One fact here matters to the class designs:

- **XP's `usbccgp.sys` on 98 SE.** NUSB 3.5/3.6 and U98SESP3 install it
  for `USB\COMPOSITE`. V. This bears on the open composite-grouping
  question in `candidates.md` ("What a class driver from here sits on").
  Under those packs, 98 SE may split composites by IAD as XP SP2+ does
  (U: XP's `usbccgp` build 5585 or 6437 and its IAD handling on 98 SE).

## 11. Unverified points that would change a verdict

| Point | Would change | Reading that settles it |
|---|---|---|
| Realtek XP 5.23, Vista 6.27, 7 7.61.20: hardware ids and licence | Owner, 2026-10-20: the hardware ids are read (section 4.1) and `rtl815x` is narrowed to 98 SE, ME and 2000. The licence bears only on the non-free pack | Read the licence when the drivers are fetched |
| ASIX AX88179 redistribution terms; whether the 2013/2018 drivers accept AX88179A/B | Owner, 2026-10-20: their INFs do not cover AX88179A/B (section 4.2); `ax88179` is narrowed to 98 SE, ME and 2000. The terms bear only on the non-free pack | ASIX's terms page or a written answer |
| Morii's permission to redistribute `x360c`/`x360wc` | Owner, 2026-10-20: settled without it. `xpad98` leaves `x360c`'s 25 IDs and the 2000 receiver to Morii's drivers, which the non-free pack carries, in their final 2010 builds, as it carries NUSB | None needed |
| Whether XBCD 0.2.8's 9x paths work on 98 SE with an INF | Whether `xpad98` 360 wired on 98 SE/ME/2000 becomes an XBCD port (free) rather than new code | Build XBCD with a 9x INF and test |
| An official Microsoft (or vendor) download of the 98 `usbprint.sys` with redistribution terms | `usbprn` could become A for the driver part (non-free), leaving the port monitor | Microsoft download centre archives; KB for the 4.10.2223 hotfix |
| Whether 98SE2ME's ME `USBAUDIO.SYS` + `WDMA_USB.INF` gives working USB MIDI on 98 SE | Not the verdict (it needs an ME CD and cannot be shipped), but the user documentation | A test on 98 SE |
| Whether Windows' in-box `rndismp` serves Android tethering under a class-id INF | Owner, 2026-10-20: `cdcnet` RNDIS on XP to 7 is INF-only; if the test fails on a system, `cdcnet`'s RNDIS code is built for it | A test on XP, Vista and 7 |
| Vimicro's 2000 UVC driver: INF and terms | Nothing likely (vendor-locked) | Obtain the INF |

## 12. Owner decisions

Owner, 2026-10-20, on this survey's verdicts:

- **`usbmidi`**: M-Audio MidiSport devices are dropped. M-Audio's own
  driver covers 98 to XP and is officially downloadable (archived), but
  its licence forbids redistribution, and a free driver would need the
  same restricted firmware. The non-free pack records the download links
  only (section 3.4).
- **`xpad98`**: narrowed. Morii's `x360c` and `x360wc` go in the non-free
  pack, from Morii's own site, with provenance recorded: `x360c` in its
  final 2010 builds (25 pad IDs) stays the 360-pad driver there, and
  `x360wc` serves the 360 receiver on 2000 to 7. `xpad98` keeps Xbox One
  and Series pads (98 SE to Vista), the 360 receiver on 98 SE and ME
  only, and 360-protocol pads outside `x360c`'s 25 IDs. The XP to 8
  opt-in mode is dropped; XBCD covers XP and later (section 6.4).
- **`rtl815x` and `ax88179`**: Realtek's and ASIX's XP to 7 drivers were
  read first (INFs, chip revisions). They cover the chips' current
  revisions (sections 4.1 and 4.2), so both packages target 98 SE, ME and
  2000 only. ASIX's INFs do not list the AX88179A/B; on XP to 7 those go
  through `cdcnet`'s NCM, as on every target (section 4.5).
- **`cdcnet` RNDIS**: on XP to 7, an INF binding Windows' own RNDIS driver
  by class id, with Android tethering verified; `cdcnet`'s own RNDIS code
  targets 98 SE, ME and 2000 only (section 4.5).
- **Vendor serial drivers for the non-free pack**: FTDI VCP 1.0.3, CDM
  2.06.00 and WCH CH341SER are carried with their long names as shipped;
  PL2303 and CP210x are not, link only (section 5).
- **Order**, from these verdicts (`README.md`, "Status"):
  - wave 1: `usbflop`, `usbcom`, `usbmidi`, `cdcnet` and `xpad98` (the
    installer was deferred, Owner, 2026-10-20);
  - wave 2: `uac2`, then `uvc98`;
  - Owner, 2026-10-21: the waves swapped. Wave 1 is now `uac2`, with
    the INF-only `usbflop` and `usbcom`'s `USBCDC.INF`; wave 2 is
    `uvc98` (no UVC camera on the bench yet), `usbcom`'s 98 SE/ME
    driver, `usbmidi`, `cdcnet` and `xpad98` (`README.md`, "Status");
  - later: the installer; `libusb0`; `rtl815x` and `ax88179` after `cdcnet`; `usbprn`
    at low priority.
- `uvc98`, `uac2`, `usbcom`, `usbprn`, `usbflop` and `libusb0` keep their
  verdicts and their designs.

### Open questions

None at present. The unverified points of section 11 that remain are
settled by the readings and tests named there.

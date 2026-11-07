# 98 SE and ME: the KS file prompts of uac2 and usbmidi, 2026-11-07

Installing `uac2` on 98 SE made Setup ask for `ks.sys`, then
`ksclockf.ax`, and neither was on the 98 SE CD's `\WIN98` folder or in
`C:\WINDOWS\OPTIONS\CABS` (owner, 2026-11-07). This record establishes
what a clean 98 SE has, what Setup asks for and why, and tests the INF
change of branch `w47-ksinf` (`docs/design/uac2.md` section 24,
`docs/design/usbmidi.md` section 19). Development legs in QEMU, not
"Tested". No Microsoft binary was read; Windows' own `ks.inf`,
`wdmaudio.inf` and `layout.inf` were read as text to understand them,
and the CD's CAB files were listed by name only.

## Result

- On a clean 98 SE the kernel side of WDM audio is installed by Windows
  Setup, but the DirectShow side of KS and the catalogs are not: the
  README's "Windows already has them" is not true there for
  `ksclockf.ax`, `ksproxy.ax`, `ksdata.ax`, `ksinterf.ax`, `ksvpintf.ax`,
  `ks.cat` and `wdmaudio.cat`.
- Setup looks for those files "on UAC2 experimental build folder", the
  package's own disk, because the INF included `ks.inf` and
  `wdmaudio.inf`. They are on the 98 SE CD only inside CAB files, so the
  CD's `\WIN98` does not answer the prompt.
- Skipping them was harmless in the runs: Windows' own install of
  SysAudio's software device, which `WDMAUDIO.Registration` starts
  through RunOnce, copies the same files from Windows' CABs, with
  Windows' own "Windows 98 Second Edition CD-ROM" prompt when the
  `SourcePath` does not hold them; the CD's `\WIN98` answers that one.
- With the change (no Microsoft INF included on 9x; the RunOnce entry
  written by the package's INF) Setup asks for no KS file. Play and
  record pass on a clean 98 SE, after Windows' own USB audio, over an
  existing 0.1.8 install, after an unplug, after a restart, and on ME.

## Rig

Kit `E:\xhci98-vm\a347\ksinf` (copied from the `mx9x` kit's harness),
ports 51540-51557, headless QEMU, TCG, `-machine pc`, 1 vCPU, 256 MB,
`piix3-usb-uhci` with Windows' own USB 1.1 stack. Guest drives: C: the
system, D: the transfer folder (VVFAT), E: the 98 SE CD image (on ME: E:
the ME CD, F: the 98 SE CD).

- Base `w98c`: overlay of `E:\usb-wdm-free\vm\w98se.qcow2` (98 SE,
  4.10.2222 A), the UHCI controller's wizard answered (its files from the
  CD's `\WIN98`), a Run entry for the harness. Its `SourcePath` is
  `D:\WIN98\`, where this rig has no CD, so Windows' own prompts appear;
  on a PC whose CD is at that letter they do not.
- Base `w98u`: `w98c` plus QEMU's `usb-audio` (a USB Audio 1.0 speaker)
  installed by Windows' own "USB Audio Device" (`wdma_usb.inf`, files
  from the CD's `\WIN98`), then unplugged.
- ME: overlay of the `mx9x` kit's `men` base (stock ME, UHCI, Run entry).
- Device: `uacemu.py --profile fs --fs-feedback 3 --fb-ppm 15000` (a Full
  Speed UAC 2.0 composite device, 48 kHz) on UHCI port 1.
- Package staged in `C:\WDMFREE\UAC2` with `DevicePath` naming it. That
  staging makes Setup ask once for `uac2wdm.sys` (its disk's default path
  is the `SourcePath`), answered with `C:\WDMFREE\UAC2`; it is not one of
  the KS prompts and is not counted below.
- Tests: `UACWAV list`, `UACWAV play 2 16 48000 5`,
  `UACWAV rec 1 16 48000 3` (signal check), `MIDITEST list`;
  `regedit /e` of the MEDIA class, the RunOnce key and the KS CLSIDs;
  `dir` of the KS files.
- Binaries: `uac2wdm.sys` of w47-rel019 (sha256 `8e2b8729...901233c`);
  for the 0.1.8 install, release 0.1.8's `uac2wdm.sys` (`d6773576...b7ee80`)
  and INF (`b6650f93...b576cf1`). INFs: A = w47-rel019's
  (`0c8944fd...c487a`), B = A with `Include=wdmaudio.inf` and
  `Needs=WDMAUDIO.Registration` only (`f57c5ad9...7d406`), C = the change
  (`422cd4f2...bd9bf`, comments differing from the commit), C2 = the
  change as committed (`87236d1a...8daa4`).

## What a clean 98 SE has (base `w98c`, before any audio install)

From `dir` in the guest, and the image's file list:

| Present | Absent |
|---|---|
| `SYSTEM32\DRIVERS`: `ks.sys`, `portcls.sys`, `stream.sys`, `sysaudio.sys`, `kmixer.sys`, `swmidi.sys`, `wdmaud.sys`, `redbook.sys`, `sbemul.sys`, `usbaudio.sys`, `gm.dls` | `mspclock.sys`, `mskssrv.sys` |
| `SYSTEM`: `wdmaud.drv`, `ksuser.dll`, `mmdevldr.vxd`, `dmusic.dll`, `mmci.dll`, `streamci.dll` | `ksproxy.ax`, `ksclockf.ax`, `ksdata.ax`, `ksinterf.ax`, `ksvpintf.ax` |
| `INF`: `ks.inf`, `wdmaudio.inf`, `wdma_usb.inf`, `ksfilter.inf` | catalogs `ks.cat`, `wdmaudio.cat`, `ksfilter.cat` |

No MEDIA class device (no SysAudio, KMixer, synth, clock or service
proxy), and no `HKCR\CLSID` key for the KS proxy or the clock forwarder.

All of them are on the 98 SE CD, inside CAB files only (`layout.inf`
and the CAB listing): `ks.sys` in `BASE5.CAB`, `ksclockf.ax`,
`ksproxy.ax`, `ksdata.ax`, `ksinterf.ax`, `ksvpintf.ax` in
`DRIVER11.CAB`, `ksuser.dll` in `DRIVER12.CAB`, `ks.cat` and
`wdmaudio.cat` in `CATALOG3.CAB`. None is a loose file in `\WIN98`.

## Legs

| Leg | Base | INF | KS prompts from the package's disk | Windows' own prompts | Play | Record |
|---|---|---|---|---|---|---|
| a1 | w98c | A | `ks.cat`, `ksclockf.ax`, `ksdata.ax`, `ksinterf.ax`, `ksproxy.ax`, `ksuser.dll`, `ksvpintf.ax`, `wdmaudio.cat`; `E:\WIN98` (the CD) given for `ks.cat`: "Setup could not find a file on the specified path" | - | - (the run also skipped `uac2wdm.sys`; ended) | - |
| a2 | w98c | A | the same 8, skipped; `ks.cat` once more later, skipped | "Windows 98 Second Edition CD-ROM" twice (one OK'd away, `MSPCLOCK.sys` answered with `E:\WIN98`) | PASS | PASS |
| a2, cold start | | | | | 0 wave devices: list and play timed out, "no UAC2 input" (one run, cause not read) | FAIL |
| b1 | w98c | B | `wdmaudio.cat` (could not be answered) | - | not run | not run |
| c1 | w98c | C | none | `ks.sys` on "Windows 98 Second Edition CD-ROM", answered with `E:\WIN98`; nothing more | PASS | PASS |
| c1, cold start | | | | | PASS; MIDI: "Microsoft GS Wavetable SW Synth" | PASS |
| c1, unplug and plug | | | | | PASS | PASS |
| u1 | w98u | A | the same 8 and `ks.cat` again, all skipped, though every file was already installed | - | PASS | PASS |
| u2 | w98u | C2 | none | none | PASS | FAIL (3 of 8 buffers in 13 s) |
| u2, cold start | | | | | PASS; MIDI: SW Synth | PASS |
| u3 | w98u | C2 | none | none | PASS | PASS, PASS, FAIL (0 of 8) |
| u4 | w98u | A | the same 8, skipped | - | not run: the guest stopped at the install's end (its clock stopped; no answer to the harness on COM1) | - |
| g1 | w98c | 0.1.8 | the same 8, skipped | "Windows 98 Second Edition CD-ROM", answered with `E:\WIN98` | PASS | - |
| g1, upgrade | | C2 | none | none | PASS | PASS |
| m1 | ME `men` | C2 | none | none | PASS | PASS |

Notes:

- In a2 every file skipped from the package's disk was present after the
  install (`ksproxy.ax`, `ksclockf.ax`, ..., `ks.cat`, `wdmaudio.cat`):
  the SysAudio, MSPCLOCK and MSKSSRV installs (Windows' `wdmaudio.inf`
  and `ksfilter.inf`) copied them. The MEDIA class then held the device,
  "Microsoft Streaming Clock Proxy", "Microsoft Streaming Service Proxy",
  "Microsoft Kernel System Renderer", "Microsoft Kernel GS Wavetable
  Synthesizer", "Microsoft Kernel Audio Mixer" and "Microsoft DirectMusic
  SW Synth (WDM)". c1 ended with the same devices, files and CLSIDs.
- The device's loader: with INF A, `*ntkern,mmdevldr.vxd` (written by
  `WDMAUDIO.Registration` over the INF's own `*NTKERN`); with C and C2,
  `*NTKERN,MMDEVLDR.VXD` from the INF.
- The MIDI output (SW Synth) appears after a cold start, not in the
  install's session (c1).
- g1, upgrade: with 0.1.8 installed and playing, the C2 package was
  copied over `C:\WDMFREE\UAC2`. Plugging the device on port 2 kept the
  0.1.8 binding (same instance). After Device Manager > Remove and a
  plug, Windows installed C2 without a prompt (`INF\OTHER` copy replaced:
  10,824 bytes; loader `*NTKERN,MMDEVLDR.VXD`); play and record PASS.
- The record failures in u2 and u3 (first session, after Windows' own
  USB audio) came and went within one session and passed after a cold
  start. u4, the same install with INF A, stopped the guest at the
  install's end. Full Speed UAC 2.0 on 98 SE's own
  USB 1.1 stack is a known weak path (`docs/design/uac2.md`, "Known issue
  (0.1.5)"); the INF change touches only what Setup writes.
- ME already had SysAudio, KMixer, the synth and the KS proxies installed
  (`SourcePath` `C:\WINDOWS\OPTIONS\INSTALL\`); nothing was asked.

## Not run

- No XP leg: the NT install sections are byte for byte unchanged in both
  `uac2wdm.inf` files (the change adds four strings to `[Strings]`), and
  `mkinf.ps1` writes the usbmidi INFs of `w2k`, `wxp`, `wxp-x64`,
  `wv-8-x86` and `wv-8-x64` byte for byte as before.
- No usbmidi guest leg; its 9x section has the same change as `uac2`'s.
- No leg under NUSB's EHCI or xHCI98.

# Toolchains and test guests

What goes into this repository's git-ignored `tools/` and `vm/`, and how
the builds use it ([design/build.md](design/build.md) sections 2, 3 and
8). Nothing here is fetched at build time and no build reads a path
outside this repository. The kits are licensed products; each developer
supplies their own legally held copy and copies it in once.

Recorded 2026-10-20 on the author's build host (Windows 11, x64).

## Kits

| Folder | Kit | Checked versions | Source media read (SHA-256) |
|---|---|---|---|
| `tools/MSVC600/` | Microsoft Visual C++ 6.0, SP6 compiler | `CL.EXE` 12.00.8804, `LINK.EXE` 6.00.8447 | `MSVC600.zip`, 47,050,976 bytes, `a4c46a05039cc8f512043728c8132a830a04ffd68d6f038df100413ae6c21061` |
| `tools/ntddk/` | Windows 2000 DDK | headers and `libfre\i386` libraries as below | `WIN2KDDK.EXE`, 68,905,688 bytes, `e81728f182a278fc749abe32a366ae6726f15ca5f2c9e43912672707a4df2213` |
| `tools/win2kddk-extracted/` | Windows 2000 DDK, the CABs unpacked (documentation, for example `D_NET.CAB`) | - | the same `WIN2KDDK.EXE` |
| `tools/win98ddk-extracted/` | Windows 98 SE DDK, unpacked with flattened file names (`INC_WIN98_SETUPX.H` is `inc\win98\setupx.h`) | - | `win98se_ddk.exe`, 26,377,712 bytes, `08318354cbac0b17d2e976a70b901ceb336947e76e4af8c869efdc6327a6a5dc` |
| `tools/WinDDK71/` | Windows Driver Kit 7.1.0 (7600.16385.1) | amd64 `cl.exe` 15.00.30729.207, `link.exe` 9.00.30729.207; `SignTool`, `Inf2Cat`, `MakeCert`, `CertMgr`, `pvk2pfx` present | `WDK71.ISO`, 649,877,504 bytes, `5edc723b50ea28a070cad361dd0927df402b7a861a036bbcf11d27ebba77657d` |
| `tools/nasm/` | NASM 3.02 (Netwide Assembler, BSD-2-Clause), `nasm.exe` and `LICENSE` only, for libjpeg-turbo's SIMD in `src/video/uvcdec` | `nasm -v`: 3.02, "compiled on Jun 28 2026" | `nasm-3.02-win32.zip`, 661,142 bytes, `dca7d736580aafcf88a07838bb597a4f093fa157e56ce522891e86ab0a37c949`, from `https://www.nasm.us/pub/nasm/releasebuilds/3.02/win32/nasm-3.02-win32.zip` (2026-10-22; the release folder publishes no checksum to compare) |
| `tools/nusb-extracted/`, `tools/nusb33e.exe` | NUSB 3.3 (third-party), for 98 SE test guests only; never shipped or built against | - | `nusb33e.exe`, 774144 bytes, `f1f30a800cf6013eb6f18db775d88e777ddc3924622e6560edd2dcd7b53698d3` |

On the author's host these were copied from the xHCI98 checkout's own
`tools/` and from `D:\isos`. That is only where the copies came from:
the builds use this repository's `tools/` alone.

DDK and WDK sample sources (`ntddk\src`, `WinDDK71\src`) are present
because they ship inside the kits. They are not read and not used
(`AGENTS.md`, "Sources").

## Sentinels

A build checks these before compiling and names the missing kit if one
is absent.

| Profile | Compiler and linker | Key header | Import library |
|---|---|---|---|
| `wdmx86` | `MSVC600\VC98\BIN\CL.EXE`, `LINK.EXE`, `MSVC600\Common\MSDev98\Bin\MSPDB60.DLL` | `ntddk\inc\wdm.h` | `ntddk\libfre\i386\wdm.lib` |
| `wdm64` | `WinDDK71\bin\x86\amd64\cl.exe`, `link.exe` | `WinDDK71\inc\ddk\wdm.h` | `WinDDK71\lib\wnet\amd64\ntoskrnl.lib` |
| `icmx86` (`src/video/uvcdec`) | `MSVC600\VC98\BIN\CL.EXE`, `LINK.EXE`, `MSPDB60.DLL`, `ntddk\bin\rc.exe`, `nasm\nasm.exe` | `MSVC600\VC98\Include\VFW.H` | `MSVC600\VC98\Lib\LIBCMT.LIB` |

Notes from the first builds:

- The 2000 DDK's `wdm.h` includes `excpt.h`, which the DDK does not
  carry. `wdmx86` puts `MSVC600\VC98\Include` after `ntddk\inc\ddk` and
  `ntddk\inc` for it.
- MSVC 6.0's tools need `MSVC600\Common\MSDev98\Bin` on `PATH` for
  `MSPDB60.DLL`.
- WDK 7.1's amd64 build uses `/GS-`: the security cookie would need a
  library this profile does not link.
- `icmx86` assembles libjpeg-turbo's i386 SIMD with NASM (`-fwin32
  -DWIN32`); NASM 3.02's COFF objects link with MSVC 6.0's `LINK.EXE`.
  libjpeg-turbo's CMake build is not used, so no CMake is installed: the
  package's `build.ps1` compiles the files its `CMakeLists.txt` names
  (2026-10-22). The NASM zip was unpacked into its own empty folder on
  E: and only `nasm.exe` and `LICENSE` copied to `tools/nasm/`.

## Building

```
src\<class>\<package>\build.cmd <profile> <free|checked>
```

Output goes to `out\<class>\<package>\<profile>\<flavour>\`: the
binary, the INF with CRLF line endings, `build.log` (every command line
and its output), `record.txt` (commit, tree state, kit, flags, hashes),
`headers.txt`, `imports.txt`, `gate.txt`, and `PASS.TXT`, written last
and only when every gate passed. The first package with a build is
`src/generic/wdmsmoke/`, the disposable smoke driver of build.md
section 6.

## Test guests

QEMU disk images, each a fresh install extracted as a single standalone
image from a clean snapshot of the author's earlier guests. Owner rule
(2026-10-22): VM disks and scratch go on E:. The base images therefore
live in `E:\usb-wdm-free\vm\`, not in the repository's `vm/`; scripts may
fall back to `vm/` on a host without E:. The move was made on 2026-10-22.
The E: copies are SHA-256-identical to the originals. All 42 overlays
were rebased metadata-only, then checked with `qemu-img check` and
`--backing-chain`, and one per base matched its old chain under
`qemu-img compare`. The `vm/` originals were then deleted. Records:
`E:\usb-wdm-free\vm\copy-sha256.txt`, `rebase-log.txt`, `check-log.txt`,
`compare-log.txt` and `deleted-originals.txt`. New overlays must use
`E:/usb-wdm-free/vm/<image>` as their backing file.

| Image | System | Taken from snapshot | Logon |
|---|---|---|---|
| `w98se.qcow2` | 98 SE, NUSB 3.3 installed | `post-nusb` | - |
| `wme.qcow2` | ME | `winme-clean-install` | - |
| `w2k.qcow2` | 2000 SP4 | `win2k-xonly-clean-install` | - |
| `wxp.qcow2` | XP SP3 x86 | `winxp-clean-autologon` | `test`, no password |
| `wxp64.qcow2` | XP x64 SP2 | `winxp64-clean-autologon` | `Administrator` / `test` |
| `wv86.qcow2`, `wv64.qcow2` | Vista SP2 x86, x64 | `vista-clean-autologon`, `vista-x64-clean-autologon` | `test` / `test` |
| `w786.qcow2`, `w764.qcow2` | 7 SP1 x86, x64 | `win7-clean-autologon`, `win7-x64-clean-autologon` | `test` / `test` |

Run each test on a throwaway overlay so the base stays fresh:

```
qemu-img create -f qcow2 -b w98se.qcow2 -F qcow2 run.qcow2
```

Owner, 2026-10-21: put new VM overlays, exchange disks and temporary
files under `E:\usb-wdm-free\`. Set the launched QEMU process's `TEMP`
and `TMP` there too: writable virtual FAT may create a temporary backend.
Use `-display none` and a localhost-only monitor for unattended tests.
`src/generic/usbprobe/monitor.py` sends monitor commands and captures PNGs;
its Dvorak option types into the Windows 2000 guest's text fields.
The 2000, XP and 7 guests' keyboards are US Dvorak and 98 SE is QWERTY
(`monitor.py --layout dvorak`; the X4/M2 helper `E:\usb-wdm-free\wave1\hmp.py --type`
maps Dvorak by default and QWERTY with `HMP_LAYOUT=us`). In the 2000 guest,
stage the usbmidi INF before plugging the device (STOP 0xD3 otherwise). A
FAT floppy media change right after writes once gave STOP 0x23. The 2000
base has no USB stack files: expand `usbport.sys`, `usbd.sys`, `usbhub.sys`,
`usbhub20.sys` and `usbehci.sys` from the guest's own
`Driver Cache\i386\sp4.cab`, and shut down cleanly before adding EHCI.
Eject a virtual FAT floppy before changing its host staging directory,
then remount it. Shut down the guest and quit QEMU after each test; leave
no visible QEMU windows or idle test processes running when finished.

QEMU 11.1.0 settings that booted each tested guest, 2026-10-21
([testing/2026-10-21-wdmsmoke.md](testing/2026-10-21-wdmsmoke.md)):

| Guest | Command and machine options |
|---|---|
| 98 SE | `qemu-system-i386 -machine pc -cpu pentium3 -m 256` (TCG) |
| 2000 SP4 | `qemu-system-i386 -machine pc,acpi=off -cpu pentium3,-apic -m 256 -vga cirrus -rtc base=localtime` (TCG) |
| XP SP3 | `qemu-system-x86_64 -machine pc -accel whpx,kernel-irqchip=off -cpu pentium3 -smp 4 -m 512 -vga std` |
| 7 x64 | `qemu-system-x86_64 -machine pc -accel tcg,thread=multi -cpu qemu64 -smp 4 -m 2048 -vga std` |

This host's `qemu-system-i386` has no WHPX, so WHPX guests use
`qemu-system-x86_64`. The other guests are recorded when first booted.

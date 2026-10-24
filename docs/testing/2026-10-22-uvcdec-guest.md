# uvcdec in 98 SE and 2000 guests: JDTEST, the INF, playback

Date: 2026-10-22. The `uvcdec` codec alone (no camera, no USB device, no
`uvc98` driver): the host test `JDTEST.EXE` run inside the guests, `UVCDEC.INF`
installed and removed, MJPEG AVIs made from the test vectors played. QEMU
guests under TCG; no real-PC claim and no speed claim.

## Build and guests

- Build: `build.cmd icmx86 free` at `8df85e3` (tree clean), libjpeg-turbo
  3.2.0, MSVC 6.0 + NASM 3.02. `UVCDEC.DLL` 454,656 bytes, sha256
  `5827291E...C589`; `JDTEST.EXE` 589,824 bytes, sha256 `e682d339...872a`
  (imports `KERNEL32.dll` only). The host gate passed (336 checks) before the
  guest runs. The INF used for the final checks is the one committed with
  this record.
- 98 SE: overlay over `E:\usb-wdm-free\vm\w98se.qcow2`, `-machine pc -accel tcg
  -smp 1 -m 256 -vga cirrus`, `-cpu pentium3` and `-cpu core2duo`. The files
  on a VVFAT hard disk (D:), results written to a VVFAT floppy (A:).
- 2000 SP4: overlay over `E:\usb-wdm-free\vm\w2k.qcow2`, `-machine pc,acpi=off
  -cpu pentium3,-apic -smp 1 -m 512`, files on VVFAT (E:), results on A:.
- Host: loaded (about 13 other QEMU guests and builds running), so the
  timings below are only compared within one run.

## 1. JDTEST in the guests

`JDTEST.EXE D:\VECTORS 5` (five timing iterations), output redirected to A:.

| Guest | CPU model | SIMD line | Checks | SIMD against C |
|---|---|---|---|---|
| 98 SE | pentium3 | `MMX SSE (0x05)` | 256, 0 failures, PASS | MMX: 154,224,000 bytes, 0 differ |
| 98 SE | core2duo | `MMX SSE SSE2 (0x0D)` | 296, 0 failures, PASS | SSE2 and MMX: 308,448,000 bytes, 0 differ |
| 2000 SP4 | pentium3 | `MMX SSE (0x05)` | 256, 0 failures, PASS | MMX: 154,224,000 bytes, 0 differ |

So **98 SE enables the XMM state** (CR4.OSFXSR): `uvcjSimdSupport`'s
`xorps`/`pxor` probes did not fault, SSE was kept on the Pentium III model
and SSE2 on the Core 2 model, and the SSE2 decode paths ran and matched plain
C byte for byte. libjpeg-turbo's i386 decoder has no SSE-only path, so a
Pentium III decodes with MMX. The decoded vectors' PSNR lines equal the host's
(for example `b640d.jpg fast RGB24 43.22 dB`, `h1080n.jpg fast RGB24 31.08 dB`).

Per-frame 1080p decode times, best of 5, ratio to plain C in the same run.
TCG emulates the guest's SIMD instructions, so these say nothing about real
hardware speed; they are recorded as ratios only.

| Guest, frame | Path | SIMD ms | C ms | SIMD / C |
|---|---|---|---|---|
| 98 SE pentium3, `h1080n.jpg` | MMX RGB24 fast | 149.54 | 125.66 | 1.19 |
| 98 SE pentium3, `h1080en.jpg` | MMX RGB24 fast | 113.83 | 77.87 | 1.46 |
| 98 SE pentium3, ljt 4:2:2 2h1v | MMX RGB24 fast | 136.88 | 119.30 | 1.15 |
| 98 SE pentium3, ljt 4:2:2 2h1v | MMX YUY2 fast | 97.02 | 96.73 | 1.00 |
| 98 SE core2duo, `h1080n.jpg` | SSE2 RGB24 fast | 75.18 | 140.65 | 0.53 |
| 98 SE core2duo, `h1080en.jpg` | SSE2 RGB24 fast | 60.01 | 85.18 | 0.70 |
| 98 SE core2duo, ljt 4:2:2 2h1v | SSE2 RGB24 fast | 78.66 | 159.68 | 0.49 |
| 98 SE core2duo, ljt 4:2:2 2h1v | SSE2 YUY2 fast | 68.07 | 103.51 | 0.66 |
| 98 SE core2duo, ljt 4:2:2 2h1v | MMX RGB24 fast | 152.62 | 159.68 | 0.96 |
| 2000 pentium3, ljt 4:2:2 2h1v | MMX RGB24 fast | 121.05 | 94.36 | 1.28 |

Under TCG, MMX is no faster than C (TCG runs MMX through helper calls) and
SSE2 is about twice as fast as C, close to the host's real-hardware ratio
(section 14.5: SSE2 0.42, MMX 0.47 of C for ljt 4:2:2 RGB24 fast). The MMX
gain on a real Pentium III stays **unverified** (bench).

## 2. UVCDEC.INF and playback, 98 SE

Installed with `rundll.exe setupx.dll,InstallHinfSection DefaultInstall 132
D:\UVCDEC.INF` (the command right-click Install runs), read back by copying
`SYSTEM.INI` to A: and `regedit /e` of the `MediaResources\icm\VIDC.MJPG`
key. 98 SE's own `[drivers32]` had no `VIDC.MJPG` before.

**The INF as it was (`8df85e3`)**: install wrote `VIDC.MJPG=uvcdec.dll` and
the three `MediaResources` values; uninstall removed the line, the values and
`uvcdec.dll` (an empty `VIDC.MJPG` key is left). Its plain `UpdateInis` add
**replaces** an existing `VIDC.MJPG` value (a foreign `VIDC.MJPG=other.dll`
became `uvcdec.dll`), and its uninstall line deletes `VIDC.MJPG` whatever it
names, so it broke the owner's rule (leave an existing `VIDC.MJPG` codec
alone, 2026-10-22). Changed in the commit with this record.

What 98 SE's setupx was seen to do with `UpdateInis` (one-line test INFs on
the core2duo guest, each read back from `SYSTEM.INI`): a plain add replaces
the value of an existing key; flag 3 with `KEY=*` renames the key in place,
keeping its value, and does not remove an entry already under the new name
(two `VIDC.MJPG` lines resulted); flag 3 with `KEY=*` and no new entry deletes
it; flag 1 deletes only when key and value both match. The new `[UVCDEC.Ini]`
(four lines, explained in the INF) is built on exactly these.

**The new INF** (pentium3 guest, cold boot), each row read back:

| Step | `[drivers32]` | `MediaResources\icm\VIDC.MJPG` |
|---|---|---|
| clean | no `VIDC.MJPG` | key empty |
| install | `VIDC.MJPG=uvcdec.dll` | uvcdec's Description, Driver, FriendlyName |
| install again | unchanged (byte-identical `SYSTEM.INI`) | unchanged |
| uninstall | none (`SYSTEM.INI` byte-identical to clean) | values removed |
| test INF writes `VIDC.MJPG=other.dll` and Driver `other.dll` | `VIDC.MJPG=other.dll` | Description `other codec`, Driver `other.dll` |
| install | `VIDC.MJPG=other.dll` (`SYSTEM.INI` byte-identical, no `UVCDEC.KEP` left) | other's values kept; FriendlyName (absent before) added as uvcdec's |
| uninstall | `VIDC.MJPG=other.dll` kept | values removed (other's too) |
| install on clean | `VIDC.MJPG=uvcdec.dll` | uvcdec's |

The install shows no warning when it leaves another codec in place: an INF
cannot show a message only in that case. The warning is the README's
("Installing"), with how to check and switch.

**Playback** (16-colour Cirrus display, the guest's own driver not set up):

- Old Media Player (`MPLAYER.EXE`, MCI AVIVideo, Video for Windows):
  `b640n.avi` (640x480, 20 frames, every frame without DHT) and `h1080n.avi`
  (1920x1080, no DHT) open and show the decoded pattern, colour bars over
  the gradient. With the codec uninstalled the same file gives
  `MMSYSTEM006 There is no driver installed on your system`, so the frames
  were decoded by `uvcdec` (it is the only `VIDC.MJPG`).
- Windows Media Player 6.4 (`MPLAYER2.EXE`, DirectShow): `move.avi`
  (FFmpeg `testsrc`, MJPEG 4:2:2, 640x480, 50 frames) plays, the sweep moving
  between screenshots. Without `uvcdec`: "Cannot play back the video stream:
  no suitable decompressor could be found". `HKCR\CLSID\{301056D0-6DFF-11D2-9EEB-006008039E37}`
  (Microsoft's MJPEG Decompressor) is absent from this 98 SE; the AVI
  Decompressor `{CF49D4E0-1115-11CE-B03A-0020AF0BA770}` is present, so
  DirectShow reached `uvcdec` through it.

## 3. UVCDEC.INF and playback, 2000 SP4

Installed with `rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall
132 E:\UVCDEC.INF`; 2000 asks once about the missing digital signature
("Sound, video and game controllers"; Yes). Read back with `regedit /e` of
`...\Windows NT\CurrentVersion\Drivers32` and `drivers.desc`.

On 2000, `UpdateInis` on `system.ini` `[drivers32]` goes to the `Drivers32`
registry key, but not as on 98 SE: the rename lines left `VIDC.MJPG` with an
**empty** value, and flags 1, 2 and 3 with `VIDC.MJPG=uvcdec.dll` all deleted
a `VIDC.MJPG=other.dll` (test INFs, six cases). So the 2000 section writes
`VIDC.MJPG` with `AddReg` and `FLG_ADDREG_NOCLOBBER`, and its uninstall
leaves `VIDC.MJPG` in place (no INF line can remove it only while it names
`uvcdec.dll`). Final INF, fresh overlay:

| Step | `Drivers32` `VIDC.MJPG` | `drivers.desc` `uvcdec.dll` |
|---|---|---|
| clean | absent | absent |
| install | `uvcdec.dll` | uvcdec's |
| uninstall | `uvcdec.dll` left (the DLL is removed from `system32`) | removed |
| test INF writes `VIDC.MJPG=other.dll` | `other.dll` | absent |
| install | `other.dll` kept | uvcdec's |
| uninstall | `other.dll` kept | removed |

Playback, with the install as above (an earlier overlay, same install
section): `MPLAY32.EXE` shows `b640n.avi` decoded (true colour), and
Windows Media Player plays `move.avi` (frame counter advancing). Without
`uvcdec`, Windows Media Player reports "no suitable decompressor", and
`{301056D0-...}` is absent from this 2000 SP4 image too.

## Verdict

- JDTEST in the guests: PASS on 98 SE (pentium3, core2duo) and 2000
  (pentium3); 98 SE enables SSE/SSE2, every SIMD path equals plain C.
- Codec on 98 SE and 2000: installs, VfW and DirectShow decode MJPEG
  (with and without DHT, 640x480 and 1080p) through it: PASS.
- `UVCDEC.INF`: the old one replaced an existing `VIDC.MJPG`; the new one
  leaves it alone on both systems (the warning is the README's). Open
  points: 2000's uninstall leaves `VIDC.MJPG=uvcdec.dll` behind, and 98 SE's
  uninstall removes the `MediaResources` values even when another codec
  wrote them.

Not covered: ME; `ICInfo`/`ICOpen` from a test program of our own (the
players' use of the codec stands in for them); a VfW capture application;
any camera.

Evidence (not in the repository): `E:\usb-wdm-free\uvcdec-legs\evidence\`
(the three JDTEST outputs and the playback screenshots), the read-backs in
`out*\` there.

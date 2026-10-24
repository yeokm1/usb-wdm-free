# uvcdec: the MJPEG codec for uvc98

`UVCDEC.DLL` is a Video for Windows Installable Compression Manager (ICM)
codec driver that decompresses FOURCC `MJPG` to RGB24, RGB32 and YUY2. It
exists so that the MJPEG formats `uvc98` passes through from a UVC webcam
can be shown and captured on Windows 98 SE, ME and 2000: MJPEG is the only
way a USB 2.0 camera reaches 1080p at 30 fps. Design:
[docs/design/uvc.md](../../../docs/design/uvc.md), section 14.

This software is based in part on the work of the Independent JPEG Group.

The decoder is libjpeg-turbo 3.2.0 with its x86 SIMD routines (MMX, SSE2,
AVX2), chosen at run time from CPUID and from what the system has enabled;
on a processor or system with none of them its plain C routines decode.

## Files

| File | What |
|---|---|
| `uvcdec.c` | `DriverProc`: the ICM driver messages (`DRV_*`, `ICM_GETINFO`, `ICM_DECOMPRESS_QUERY`, `_GET_FORMAT`, `_BEGIN`, `ICM_DECOMPRESS`, `_END`, the `ICM_DECOMPRESSEX` set for whole frames) |
| `uvcjpeg.c`, `uvcjpeg.h` | The frame decoder on libjpeg-turbo, free of Windows headers: RGB24/RGB32 through libjpeg-turbo's colour conversion straight into B,G,R(,X), YUY2 from its raw (downsampled) output for 2h1v and 2h2v frames, 1/2, 1/4, 1/8 IDCT scaling, the SIMD choice (`uvcjSimdSupport`, `uvcjSetSimd`) |
| `jconfig.h`, `jconfigint.h`, `jversion.h` | libjpeg-turbo's configuration headers, filled in by hand as its CMake build would for 32-bit MSVC (no CMake step) |
| `turbo/` | libjpeg-turbo 3.2.0, the files the codec and its test compile, with four MSVC 6.0 patches (below) |
| `jdtest.c` | Host test: decode checks on `vectors/` and on frames it encodes itself, every decode at each SIMD level against plain C, the ICM interface called directly, 1080p decode timing per SIMD level |
| `vectors/` | MJPEG test frames of a synthetic pattern and `mkvec.py`, which makes them |
| `UVCDEC.INF` | Installs and registers the codec |
| `build.cmd`, `build.ps1` | The build (MSVC 6.0, NASM for the SIMD) and its gates, then the host test |
| `imports-dll.txt`, `exports98/` | The imports the DLL may use, and 98 SE's and ME's `KERNEL32.DLL` export tables (names and ordinals only, copied from `src/generic/libusb0/exports98/`, where their provenance is recorded) |

## Building

```
src\video\uvcdec\build.cmd icmx86 <free|checked> [timing iterations]
```

Output in `out\video\uvcdec\icmx86\<flavour>\`: `UVCDEC.DLL`, `UVCDEC.INF`
(CRLF), `build.log`, `headers.txt`, `imports.txt`, `exports.txt`,
`gate.txt`, `test.log`, `record.txt`, `SHA256.TXT` and, last, `PASS.TXT`.
Gates: i386 GUI DLL, subsystem and OS version 4.00, no delay imports, the
only export `DriverProc`, imports from `KERNEL32.DLL` only (static C
runtime), each listed in `imports-dll.txt` and in both export tables. The
host test `JDTEST.EXE` runs under the build and fails it on any failed
check. The kit is `tools\MSVC600` (and `rc.exe` from `tools\ntddk\bin`), with
NASM 3.02 in `tools\nasm` for libjpeg-turbo's SIMD, as
[docs/toolchain.md](../../../docs/toolchain.md) records. There is no CMake
step: `build.ps1` compiles the files libjpeg-turbo's `CMakeLists.txt` names
(`JPEG_SOURCES`, and `simd/CMakeLists.txt`'s i386 `SIMD_SOURCES` through
NASM with `-fwin32 -DWIN32`), each `-8`/`-12`/`-16` wrapper being one file
compiled for that sample precision. No x64 build: the codec serves uvc98,
whose systems are 98 SE, ME and 2000.

## SIMD and the plain C fallback

libjpeg-turbo chooses its routines per decompress object from
`master->simd_support`. `uvcjOpen` sets it from `uvcjSimdSupport()`:
CPUID's sets (`simd/i386/jsimdcpu.asm`, which already requires the
system's XSAVE support before it reports AVX2), less SSE and SSE2 if an SSE
or SSE2 instruction run under `__try` faults. CPUID shows what the
processor has, not whether the system saves the XMM registers on a task
switch; a system that does not (Windows 95, and any system that has not set
CR4.OSFXSR) makes the instruction fault, and the decoder then stays on MMX
or plain C. MMX needs no system support. Which sets 98 SE and ME enable on
an SSE2 processor is **unverified** until a guest runs the codec. On a
Pentium III libjpeg-turbo uses MMX (its SSE routines serve only the float
DCT, which the codec does not select); on a Pentium 4 and later, SSE2.

The libjpeg-turbo environment variables (`JSIMD_FORCENONE` and the like)
are not read: the build defines `NO_GETENV` (MSVC 6.0's C library has no
`getenv_s`), and `uvcjSetSimd` is the codec's own switch; the host test
uses it to decode every vector at each SIMD level and in plain C.

## Installing

Copy `UVCDEC.DLL` and `UVCDEC.INF` to one folder, right-click `UVCDEC.INF`
and choose Install. By hand:

- 98 SE, ME: `rundll.exe setupx.dll,InstallHinfSection DefaultInstall 132 UVCDEC.INF`.
  This copies `uvcdec.dll` to the system folder, adds `VIDC.MJPG=uvcdec.dll`
  to `SYSTEM.INI` `[drivers32]`, and the
  `HKLM\System\CurrentControlSet\Control\MediaResources\icm\VIDC.MJPG` key
  the Multimedia control panel lists codecs from.
- 2000: `rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall 132 UVCDEC.INF`
  (setupapi uses `[DefaultInstall.NT]`; it asks once about the missing
  digital signature). This copies the DLL to `system32` and sets
  `VIDC.MJPG` = `uvcdec.dll` under
  `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32`, with its
  `drivers.desc` description.

**Another MJPEG codec already installed.** If something already holds
`VIDC.MJPG`, the INF leaves it alone and `uvcdec` is not used for MJPEG; the
install says nothing about it (an INF cannot show a message only in that
case), so check after installing:

- 98 SE, ME: open `SYSTEM.INI` (in the Windows folder) and find the
  `VIDC.MJPG=` line in `[drivers32]`. 2000: in `regedit`, look at
  `VIDC.MJPG` under
  `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32`.
- If it names a file other than `uvcdec.dll`, that codec decodes MJPEG.
  To switch to `uvcdec`, note the old value, then change it to
  `uvcdec.dll` (98 SE, ME: edit the line and restart; 2000: edit the
  value). To switch back, put the old value back.

Uninstall with `DefaultUninstall` in place of `DefaultInstall`. On 98 SE and
ME it removes the `VIDC.MJPG` line only while it names `uvcdec.dll`; it also
removes the values of the `MediaResources\icm\VIDC.MJPG` key, whoever wrote
them. On 2000 it removes the DLL and its description but leaves `VIDC.MJPG`
in place, because 2000's setupapi was seen to delete that value whatever it
names (2026-10-22); after uninstalling, delete `VIDC.MJPG` by hand in
`regedit` if it names `uvcdec.dll`.

DirectShow applications reach the codec through DirectShow's AVI
Decompressor, which wraps ICM codecs. Neither the 98 SE guest nor the 2000
SP4 guest of 2026-10-22 has Microsoft's MJPEG Decompressor filter: without
`uvcdec`, Windows Media Player reports "no suitable decompressor" for an
MJPEG AVI (design section 14.4).

Guest runs: 98 SE and 2000 SP4, 2026-10-22, `docs/testing/2026-10-22-uvcdec-guest.md`
(design section 14.7).

## Test vectors

Every frame is the synthetic pattern of `vectors/mkvec.py` (colour bars over
gradients; the 1080p frames add a fixed pseudo-random noise so they are as
large as a webcam's). No camera capture is used. They were made on
2026-10-22 with FFmpeg 8.1 (gyan.dev full build), an encoder independent of
the decoder under test, with standard Huffman tables so that dropping DHT
leaves a valid UVC-style frame:

```
python -I mkvec.py pattern 320 240 0 p320.rgb
python -I mkvec.py pattern 640 480 0 p640.rgb
python -I mkvec.py pattern 1920 1080 1 p1080n.rgb
ffmpeg -f rawvideo -pix_fmt rgb24 -s 320x240 -i p320.rgb -frames:v 1 -c:v mjpeg -huffman default -pix_fmt yuvj422p -color_range pc -q:v 3 b320d.jpg
ffmpeg ... -s 640x480 -i p640.rgb ... -pix_fmt yuvj420p ... -q:v 3 b640d.jpg
ffmpeg ... -s 1920x1080 -i p1080n.rgb ... -pix_fmt yuvj422p ... -q:v 4 h1080d.jpg
ffmpeg ... -s 1920x1080 -i p1080n.rgb ... -pix_fmt yuvj420p ... -q:v 4 h1080e.jpg
python -I mkvec.py strip b320d.jpg b320n.jpg      (and b640d, h1080d -> h1080n, h1080e -> h1080en)
```

| File | Size | Layout | DHT |
|---|---|---|---|
| `b320d.jpg`, `b320n.jpg` | 320x240 | Y 2h2v, chroma 1h2v (FFmpeg's 4:2:2) | yes, no |
| `b640d.jpg`, `b640n.jpg` | 640x480 | 4:2:0 (2h2v) | yes, no |
| `h1080n.jpg` | 1920x1080, 253,789 bytes | FFmpeg's 4:2:2 | no |
| `h1080en.jpg` | 1920x1080, 198,677 bytes | 4:2:0 | no |

The test also encodes the pattern itself with the libjpeg-turbo compressor (test
build only) in the 2h1v layout UVC cameras use, 2h2v, 1h1v, grayscale and
with restart markers, and strips their DHT.

## libjpeg-turbo 3.2.0: provenance and changes

`turbo/` holds 224 files of libjpeg-turbo 3.2.0 (released 2026-06-30), from
the official source tarball
`https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz`,
2,537,858 bytes, SHA-256
`6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e`,
downloaded 2026-10-22. Its detached signature
(`libjpeg-turbo-3.2.0.tar.gz.sig`, SHA-256
`434aa81b514a0c7b94eb1b3e7784afe4d521b1a96a71de4a2d3476be3c595555`)
verified with GnuPG 2.4.9 as a good signature by "The libjpeg-turbo
Project (Signing key for official binaries)", key fingerprint
`0338 C8D8 D9FD A62C F9C4 21BD 7EC2 DBB6 F4DB F434`, taken from
`https://raw.githubusercontent.com/libjpeg-turbo/repo/main/LJT-GPG-KEY`
(SHA-256 `9a34027e528eea605be09ec3bf2a3cc3926f6c853e97b86914eb37f350691ca9`).
`.gitattributes` keeps the files out of line-ending conversion.

Taken, everything else in the release (the TurboJPEG API, the
applications, `cjpeg`/`djpeg`'s image formats and libspng, the CMake
build, Java, documentation, test images, the other SIMD architectures) left
out:

- `LICENSE.md`, `README.ijg` (unaltered, as the IJG licence's condition (1)
  requires) and `README.md`.
- `src/`: every `j*.c` and `j*.h` except `jcstest.c`, `jpegtran.c`,
  `jdatadst-tj.c` and `jdatasrc-tj.c`; the `src/wrapper/j*.c` precision
  wrappers; `jversion.h.in`.
- `simd/`: `jsimd.c`, `jsimd.h`, `jsimdconst.h`, `jsimddct.h`,
  `jsimdint.h`, `nasm/` and `i386/`.

Changed, for MSVC 6.0, each marked in the file's header comment (the
libjpeg-turbo licence summary in `LICENSE.md` asks for that, and the IJG
licence for this statement):

| File | Change | Why |
|---|---|---|
| `src/jinclude.h` | `SNPRINTF` is `_snprintf` for `_MSC_VER < 1400` (the variadic `_snprintf_s` macro for newer MSVC) | MSVC 6.0 has neither variadic macros nor `_snprintf_s` |
| `src/jinclude.h` | defines `JUINT64`: `unsigned __int64` for `_MSC_VER < 1400`, else `unsigned long long` | MSVC 6.0 has no `long long` |
| `src/jdct.h` | `UDCTELEM2` is `JUINT64` (two typedefs) | as above |
| `src/jdapistd.c` | two `(unsigned long long)` casts are `(JUINT64)` | as above |
| `src/jchuff.c` | `simd_bit_buf_type` is `JUINT64` (compressor, host test only) | as above |

`SNPRINTF` only formats libjpeg's error messages, which the codec never
asks for. `SIZE_MAX`, missing from MSVC 6.0, is defined in this folder's
`jconfigint.h`, not in `turbo/`. Nothing else is changed: every other C
file compiles with MSVC 6.0 at `/W3` as released (the only warnings, C4761
in `jcdctmgr.c`, are in the compressor), and NASM 3.02 assembles the SIMD
files as released.

The configuration headers in this folder are libjpeg-turbo's templates
filled in by hand: `jconfig.h` (`src/jconfig.h.in`: libjpeg v6b API,
`JPEG_LIB_VERSION` 62, arithmetic coding, SIMD), `jconfigint.h`
(`src/jconfigint.h.in`: `INLINE` `__inline`, `SIZEOF_SIZE_T` 4, no
`<intrin.h>`, no thread-local storage, `SIMD_ARCHITECTURE` `I386`) and
`jversion.h` (`src/jversion.h.in` with the copyright year). Output is B,G,R
through libjpeg-turbo's `JCS_EXT_BGR` and `JCS_EXT_BGRX` colour spaces, so
no change to the library's RGB order is needed.

libjpeg-turbo's decoder uses the standard Huffman tables of ISO 10918-1
Annex K.3 when a frame has no DHT (`src/jdhuff.c` calls `std_huff_tables`,
`src/jstdhuff.c`, when it sets up the entropy decoder), so no table
insertion code was added.

Licences (`turbo/LICENSE.md`): the libjpeg API library this codec uses is
under the IJG licence (`turbo/README.ijg`); the SIMD code is under the zlib
licence, which `LICENSE.md` says the IJG licence subsumes for the libjpeg
API library; the Modified (3-clause) BSD licence covers the TurboJPEG API,
the applications and the build system, none of which is built here, and is
listed in `LICENSE.md`. All are permissive and GPL-compatible. For binaries,
`LICENSE.md` asks only for the statement that the software "is based in
part on the work of the Independent JPEG Group" when, as here, neither the
TurboJPEG API nor its programs are included; this file, `UVCDEC.INF` and
the DLL's version resource carry it.

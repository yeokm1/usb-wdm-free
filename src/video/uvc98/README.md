# UVC experiment and CamCheck

This package is under development. There is no installable webcam driver
yet. Portable parsing tests and the preview helper build independently of
the future STREAM.SYS minidriver.

## CamCheck

Build with `preview.cmd` using the repository-local MSVC 6 toolchain.
The output is `out/video/uvc98/preview/CAMCHECK.EXE`; run it inside the
Windows system whose camera driver is being checked. It uses the documented
AVICap/Video for Windows interface, including the OS mapper for WDM cameras.
It depends on the system's AVICap/VfW and MSVCRT DLLs. No installation,
camera service or external repository is required by this utility.

1. Select a capture device and press **Preview**.
2. Inspect the live image and the reported format and frame count.
3. Use **Format** or **Source** for the driver's own configuration dialogs.
4. Press **Stop** to release the camera; close the application when finished.

Preview can be retried while connected. **Refresh** enumerates devices
while disconnected. The enumeration API exposes capture indices 0–9.
Frame counters count preview callbacks containing data; they do not certify
pixel contents, frame rate, UVC conformance or successful USB transfers.
The utility displays video without recording it to disk. A camera must
already have a working capture driver visible to VfW.

API references: [capture driver connection](https://learn.microsoft.com/en-us/windows/win32/multimedia/connecting-a-capture-window-to-a-capture-driver),
[capture enumeration](https://learn.microsoft.com/en-us/windows/win32/api/vfw/nf-vfw-capgetdriverdescriptiona),
[AVICap callbacks](https://learn.microsoft.com/en-us/windows/win32/multimedia/avicap-callback-functions),
[legacy platform availability](https://learn.microsoft.com/en-us/previous-versions/ms707166(v=vs.85)).
Only API documentation and published headers were used; no driver-kit
sample sources were read.

## Payload core

`test.cmd` builds and runs guarded host tests of `uvcpay.c` using MSVC 6.
The original core parses optional UVC payload timestamps and assembles
bounded frames into caller-owned storage. It discards initial partial
frames, detects FID/EOF boundaries, drops errored or oversized frames,
validates exact uncompressed sizes when configured, and resynchronizes
conservatively after USB loss. Bulk fragments must first be reassembled
into complete UVC payloads by a future transport layer.

There is no USB I/O, image decoding/conversion, OS pin, device binding,
clock recovery or no-FID camera quirk in this core. The host checks and
specification audit do not establish a functioning Windows camera driver.
See [the audit](../../../docs/testing/2026-10-21-uvc-design-audit.md).

`isotest.cmd` builds the original isochronous-IN walker `uvciso.c` with the
actual payload core. The walker takes one completed transfer's packet
descriptors (offset, actual length, status) and feeds each successful
payload in order. A failed packet, an out-of-bounds or oversized packet, a
failed transfer, or a resubmission gap is reported as loss and never read.
Empty packets are ignored. `UvcIsoPlan` in the same file computes the
section 5.4 transfer shape for a selected isochronous alternate: Full
Speed 16 packets x 3 URBs; High Speed 8 packets x up to 6 URBs. Packets
sit back to back at their capacity, high-bandwidth ones too (USBD's packet
length is the offset distance; the earlier 4096-byte stride was refused by
xHCI98, 2026-10-23). Only bInterval 1 endpoints are planned.
The host test has 2486348 checks (2026-10-23). They cover the walker
against a guard page, an exhaustive planner sweep, and all 11 isochronous
alternates of the physical Brio 100 fixture through the actual descriptor
parser. Neither piece is wired to a USB transport yet.

`negtest.cmd` builds `uvcneg.c`, the PROBE/COMMIT sequence of design
section 5.2. It sends SET_CUR(PROBE) with format/frame/interval and the
fixed-interval hint, then GET_CUR(PROBE). The answer is validated through
`UvcDescSelect`, and SET_CUR(COMMIT) then sends exactly the bytes the
device answered. The class requests go through one caller callback, which
the kernel adapter will map to control transfers. Its host test runs a
scripted camera over the real Brio 100 fixture: 228 good negotiations
across all 38 frames and six payload answers, each choosing the smallest
sufficient alternate. Seven misbehaviours (failed or short requests, an
unknown format, a payload too large, a wrong frame size, an unlisted
interval, a failed commit) never lead to a commit that should not happen.
3228 checks in all with the payload-limit cases (2026-10-22).
The package's test scripts run their test EXE by explicit path (`.\NAME.EXE`),
so they also work in a shell that sets `NoDefaultCurrentDirectoryInExePath`.
All nine passed again after that change (2026-10-22).

## Simulated camera

`porttest.cmd` checks the actual public-structure request adapter and
compiles its real NT mapping/notification hooks; `port64.ps1` adds an amd64
object-compile check. These files remain excluded from M0. See the
[adapter record](../../../docs/testing/2026-10-21-uvc-port.md) for ownership,
class-synchronization and final callback-rundown limits.

`desc.cmd` checks the bounded descriptor/probe codec and tight uncompressed
layouts. YUY2, NV12, I420 and M420 have validated frame byte counts and
row geometry; MJPEG retains variable-size handling. This portable metadata
does not add a decoder, Windows media type or USB streaming support.
See [layout validation](../../../docs/testing/2026-10-21-uvc-layouts.md).

`host.ps1` builds a host-only DLL from the actual descriptor and payload
cores; `python -B simtest.py` checks the synthetic USB model against them.
`uvcemu.py --port <localhost-port> --log <new-jsonl-path>` serves original
moving YUY2 bars through QEMU usbredir as an explicitly simulated UVC 1.1
full-speed bulk camera, local test ID FFFF:0002. It does not access a real
camera. The fixed format is 160x120, 10 fps, with 1022-byte maximum payloads.
Requests may split payloads at 64-byte USB packet boundaries, including
the final 62-byte short packet; unsupported splits are rejected. The
model is a bounded transport test aid, not a complete camera emulator.

`uvcsim.py --port <port> --log <new-jsonl-path>` is the isochronous
camera the 98 SE legs use in place of a passed-through camera: QEMU's
`usb-host` passthrough of the Brio 100 restarts its isochronous stream
under QEMU's EHCI (275 restarts in one capture, `2026-10-22-98se-nusb-legs.md`),
so those legs cannot pass with passthrough. It is a SIMULATED UVC 1.1
camera (`--uvc 1.0` for the 26-byte probe) with its own descriptors (not the Brio's): one
IAD function, video control with a status interrupt endpoint and a
brightness control, video streaming with YUY2 and MJPEG at 160x120,
320x240 and 640x480. The isochronous alternates are 128 to 1023 bytes at Full Speed (`--speed full`). At High
Speed they are 192 to 1024 bytes single-transaction, then 2x1024, 3x800 and
3x1024. At High Speed every size to 320x240 at 30 fps fits a
single-transaction alternate; only YUY2 640x480 at 30 fps needs three
transactions (10 fps and below do not; `--fps` sets the default rate).
It answers probe and commit (a commit may lower the payload size, as
uvc98's Brio bandwidth quirk does), sends one packet per (micro)frame in
real time with 12-byte payload headers (FID toggle, EOF, a PTS per frame,
SCR), moving bars with the frame number in the top rows, and baseline
DC-only JPEG frames for MJPEG (`--no-dht` drops the Huffman tables as many
cameras do). It follows alternate-setting changes and QEMU's iso stream
start and stop. For unplug and replug it uses `--unplug-after` /
`--replug-after`, or QEMU `device_del`, which it sees as end of stream. IDs default to
046D:094C so `UVCM298.INF`'s per-camera lines bind it, as `uacemu.py`
presents the X4's; every string says SIMULATED. `--uvc 1.5` makes it a
UVC 1.5 camera: `bcdUVC` 0x0150, the 48-byte probe and commit, a 13-byte
processing unit and an encoding unit, and two more formats, NV12 with
continuous frame intervals and an H.264 format (`--h264 frame|native|both|none`:
frame-based with the H264 GUID, the 1.5 `VS_FORMAT_H264` family, both, or
none) whose frames are SIMULATED access units, not pictures.
`python -I -B uvcsimtest.py` checks it through the driver's own descriptor
parser, probe/commit negotiation (`uvcneg.c`) and payload assembler (the
`host.ps1` DLL), and decodes its MJPEG with FFmpeg when installed. Its
`--h264 native` test found that a `VS_FORMAT_H264` (or VP8) format made
the whole camera invalid; fixed on branch `uvc15-fix` (576fc46), and the
test passes only against a DLL built from that parser.

## Unwired asynchronous stream policy

`flowtest.cmd` builds the actual `uvcflow.c` dispatch policy with the
`uvcown.c` ownership table. Its host checks cover synchronous completion
reentry, delayed mapping/cancellation, paused reads, reserved teardown
capacity/generations and frame deadlines. It yields an uncompleted worker
exit ticket for UNINITIALIZE; an external joiner must establish worker exit
and callback rundown before final notification. These are explicit adapter
obligations, not facts proved by the portable core.

This policy remains outside the M0 kernel build. The original `uvcrun.c`
worker is unsafe/unapproved and remains unwired; the tested M0 miniport
still rejects RUN. See [the worker design](../../../docs/design/uvcrun.md)
and [policy tests](../../../docs/testing/2026-10-21-uvc-flow.md).

`m1build.cmd` is a separate NT2000+ ROOT\UVCSIM1 synthetic worker prototype,
with default class synchronization enabled. `worktest.cmd` exercises its
actual kernel engine under injected operations and `m1test.cmd` its actual
dispatch routing. M0 is unchanged; M1 has no USB transport or Windows98
streaming claim. See [the M1 preflight](../../../docs/testing/2026-10-21-uvc-worker.md)
for native timer/rundown gates and the exact evidence scope.

## M2: UVC over USB (NT2000+ experiment)

`m2build.cmd` builds `uvcm2.sys`: the M1 worker with `UVWORK_USB` and the
`uvcusb.c` transport. The transport covers descriptors, configuration,
PROBE/COMMIT through `uvcneg.c`, one isochronous IN ring through
`uvciso.c` and the payload core, and a per-camera bandwidth quirk.
`UVCM2.INF` matches one listed camera (the Brio 100, `USB\VID_046D&PID_094C`).
`m2gate.ps1` is the exact import allowlist. `M1CAP.EXE <hardware id>`
reads three frames from any listed device and, for a real camera, saves
them as `A:\FRAMEn.YUY`. `yuy2png.py` converts a saved frame to PNG on the
host. On a Windows 2000 guest with the Brio 100 passed through, M1CAP read
three complete, changing camera frames (2026-10-22, see the
[M2 run](../../../docs/testing/2026-10-22-uvc-m2.md)). This is not a release,
there is no 98 SE path yet. Formats come from the camera's descriptors
(YUY2 frames up to 640x480, each at its default interval); M1CAP takes
an optional `WxH`. 160x120, 176x144 and 320x240 pass on 2000 under EHCI;
sizes that need a high-bandwidth alternate do not (see the run record).

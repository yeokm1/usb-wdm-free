# UAC2 short-packet OUT boundary on a Windows host, 2026-10-21

The runtime owner reported QEMU host errors
`libusb_submit_transfer [iso] ... INVALID_PARAM` while the X4 playback path
was attempting short isochronous OUT packets. This record is a read-only
source/API contract investigation. It does not establish that a replacement
host driver/backend has been installed or that audio has played.

## Pinned permitted source

Official libusb tag `v1.0.29` resolves to commit
`15a7ebb4d426c5ce196684347d2b7cafad862626`.
The unchanged
[`libusb/os/windows_winusb.c`](https://github.com/libusb/libusb/blob/15a7ebb4d426c5ce196684347d2b7cafad862626/libusb/os/windows_winusb.c)
was downloaded from its commit-pinned raw GitHub URL and retained under
ignored `docs/references/libusb-v1.0.29/`. It is 174936 bytes, SHA-256
`fdefa52c67b032546b30d889ad74b0ab38dc62dbe7281dc5d3b366ec2970f251`.
The neighboring ignored manifest records its origin. Original libusb
copyright and `LGPL-2.1-or-later` notice remain intact. No code is copied
or adapted into this repository by this investigation; no NOTICE row is
therefore added for this read-only source.

The runtime owner subsequently called the documented `libusb_get_version`
API of QEMU's bundled DLL and obtained **1.0.30.12037**. This is function-call
version metadata, not code/disassembly. The corresponding official release
tag `v1.0.30` was therefore also acquired, resolving to commit
`87a55632db62c9bdc58cd31d3ccfa673f1bb017f`. Its unchanged
[`windows_winusb.c`](https://github.com/libusb/libusb/blob/87a55632db62c9bdc58cd31d3ccfa673f1bb017f/libusb/os/windows_winusb.c)
is retained under ignored `docs/references/libusb-v1.0.30/`: 200765 bytes,
SHA-256 `1f6d600a0c579d2d7b13a86997e8fbe3ef47f2a5b0f014842e2143a4714f82fc`,
Git blob `b84c4ac9f907556924b3fd8f52e86e4528ef0d43` independently computed
and matched to the official GitHub contents API. Its manifest records the
commit-pinned URL. The restriction below is unchanged in this release,
which matches the live DLL's major/minor/micro API version; the DLL's full
build identity and local patches are still not proven from that metadata.
The earlier v1.0.29 reference is retained as the first inspected version.
The host error is consistent with this restriction; detailed live backend
logging remains a useful corroboration. No Microsoft/vendor driver
binary code, disassembly or DDK/WDK sample source was read.

## Exact rejection and its implication

In `winusbx_submit_iso_transfer()`, v1.0.30 lines 3654-3662
(v1.0.29 lines 3214-3222),
the native WinUSB OUT path rejects the packet list when:

- a nonfinal positive packet is smaller than
  `MaximumBytesPerInterval` and the following packet is nonzero; or
- a nonzero packet occurs after a zero-length packet.

Therefore, with endpoint maximum 112 and requested packet lengths
`[24,24,...]`, the first pair already returns
`LIBUSB_ERROR_INVALID_PARAM`, before buffer registration or an OS USB
submission. Endpoint maximum is capacity; it is not the correct PCM byte
count for every service interval. This rejection explains why increasing
guest queue depth or changing the ring buffer cannot overcome the backend
contract.

The official Microsoft API reference for
[WinUsb_WriteIsochPipeAsap](https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_writeisochpipeasap),
under **Syntax** and **Remarks**, provides one buffer length and no caller
OUT packet-descriptor array. WinUSB packetizes writes at the endpoint's
maximum interval capacity and recommends whole-frame transfer sizes for
continued streaming. Its ContinueStream parameter controls continuation
at USB frame boundaries. The related
[WinUsb_WriteIsochPipe](https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_writeisochpipe)
reference documents the same packetization with an explicit frame number.
These are published API documentation, not samples or binary internals.

The native libusb branch consequently permits full packets followed by at
most one short positive packet and trailing zeroes, rather than arbitrary
short positive packets per interval. One short packet followed by zeroes
can pass this local check, but that changes scheduling/throughput and is
not a demonstrated fix. Repopulating the buffer into front-loaded maximum
packets also changes audio timing and requires independent USB Audio
contract and real-device proof. Padding every 24-byte PCM payload to 112
with zeroes would send extra audio samples; it must not be used as a
compatibility fix.

## Feasible next experiment and evidence limits

The pinned v1.0.30 source's libusbK/libusb0 branch, lines 3537-3577
(v1.0.29 lines 3117-3152), creates a
`KISO_CONTEXT` with individual offsets derived from each requested packet
length and calls `IsoWritePipe`. It does not impose the native WinUSB
maximum-size-before-short restriction. Thus a supported **host-side
libusbK backend/binding** is a direct experiment that can preserve the
guest's correct per-interval PCM lengths. A Linux host/usbfs path is another
route, but it has not been exercised here. The official
[libusb Windows documentation](https://github.com/libusb/libusb/wiki/Windows)
describes supported host bindings and the required libusbK DLL; changing a
binding must be recorded as host state, not a guest driver change.

Finally, the pinned native WinUSB OUT completion path in
`winusbx_copy_transfer_data()` sets requested packet lengths back as actual
lengths and marks packets completed after the overall request. Those
values are backend bookkeeping, not independently measured OUT packet
lengths/statuses from the device. A successful call alone is insufficient
evidence of audible playback, correct clock rate or absence of packet loss.
The separate guest playback verifier, host backend logs and actual audio
result remain necessary. This investigation changed neither USB binding,
QEMU state nor driver source.

# Portable uncompressed UVC layout expansion, 2026-10-21

`uvcdesc.c/.h` now recognizes complete NV12, I420 and M420 format GUIDs,
requires their 12-bit-per-pixel field and exposes their frame descriptors.
YUY2 remains 16-bit packed 4:2:2; MJPEG remains variable-size. This is
original portable descriptor/layout code, with no USB transactions, kernel
RUN, OS media types, image conversion or consumer decoding. No hardware or
VM was used. It does not establish full UVC support on any Windows version.

## Normative and supplemental sources

Primary: official USB-IF [UVC 1.5 archive](https://www.usb.org/sites/default/files/USB_Video_Class_1_5.zip),
`USB_Video_Payload_Uncompressed_1.5.pdf`, revision 1.5, August 9, 2012.
Ignored local `docs/references/uvc15-uncompressed.pdf` SHA256:
`5b06aca4d91132d3f62f11c9d68f441631b6367ed1a3b8f63715bef014a86f93`.
Printed pages below map to PDF pages by adding 6.

- Section 2.2/Table 2-1, printed p.1/PDF7: exact four format GUIDs.
- Section 2.3, p.2/PDF8: dimension constraints from chroma subsampling,
  no padding, and packed versus planar classification.
- Section 3.1.1/Table 3-1, pp.4–5/PDF10–11: GUID and bits-per-pixel fields.
- Section 3.1.2/Table 3-2, pp.5–7/PDF11–13: width/height, intervals and
  deprecated descriptor maximum versus negotiated frame size.
- Sections 3.3.1–2, p.8/PDF14: planar payload alignment and packed
  macropixel alignment. This change does not implement USB payload splitting.

The UVC payload PDF names the layouts but does not spell out each component
byte order. Supplemental published format documentation supplies that detail:
[Microsoft NV12 documentation](https://learn.microsoft.com/en-us/windows/win32/medfound/recommended-8-bit-yuv-formats-for-video-rendering#nv12)
describes Y followed by interleaved U/V;
[Microsoft I420 documentation](https://learn.microsoft.com/en-us/windows/win32/directshow/yuv-video-subtypes)
describes Y, then U, then V;
[Linux M420 format documentation](https://www.kernel.org/doc/html/v6.10/userspace-api/media/v4l/pixfmt-m420.html)
describes two Y rows followed by one interleaved Cb/Cr row. The implementation
uses UVC's tight wire rows, not a Windows display-surface stride or a Linux
padded row layout. These are documentation facts; no reference implementation
or driver-kit sample code was copied or adapted.

The linked Linux M420 example's numeric row labels jump from 8 to 16 for
the next group despite describing equal four-byte rows. That table is not
used as a tight-wire offset oracle. Offsets here follow its stated Y/Y/UV
ordering together with UVC section 2.3's no-padding rule: for width4 the
next group starts at12. Tests assert that every frame byte is covered once.

## Implemented contract

`UvcDescLayout` validates descriptor-sized dimensions and returns total bytes,
bits per pixel, chroma macropixel dimensions and row-sequence geometry.
Unsupported kinds, odd width, odd 4:2:0 height, zero/oversized dimensions
and 32-bit frame-size overflow fail without changing the output.

| Kind | Tight wire row sequences | Frame bytes |
|---|---|---|
| YUY2 | Packed YUYV, row stride `2*width` | `2*width*height` |
| NV12 | Full Y plane; half-height UV plane, both row strides `width` | `width*height*3/2` |
| I420 | Full Y plane; half-width/half-height U then V planes | `width*height*3/2` |
| M420 | Even Y row, odd Y row, UV row; repeat group of `3*width` bytes | `width*height*3/2` |

M420 metadata is three strided **row sequences**, not three contiguous
planes. The macropixel dimensions describe chroma geometry, not a complete
USB packetization rule. `UvcDescRow` recomputes validated geometry, requires
the caller's buffer to cover the full frame, bounds a requested row and
returns offset/length without accessing pixels. It rejects aliased offset
and length outputs, and leaves outputs unchanged on failure.

The descriptor parser matches all 16 GUID bytes; a matching FOURCC alone
does not suffice. A recognized GUID with incorrect bits-per-pixel is now a
malformed descriptor rather than a silently skipped format. Unknown GUIDs
remain skipped. Frame byte sizes are derived from validated geometry;
`UvcDescSelect` requires an exact negotiated byte count for every recognized
uncompressed format, without clamping it. MJPEG retains its separate
variable-size policy and caller allocation budget. Existing `UVC_DESC_FRAME`
and model structure sizes are unchanged; the existing `Kind` field identifies
the added layout. Color matching, interlace/property propagation, transport,
consumer media negotiation and decoding remain outside this subset.

## Validation

`desc.cmd`, MSVC 6 `/W4 /WX`: **7497 checks pass**, including the previous
YUY2/MJPEG tests and codec tests. New synthetic tests cover independent 4x4
row-offset/size tables and byte coverage (every byte exactly once), component
roles, row/buffer bounds, null and aliased outputs, unchanged failure outputs,
even dimensions and odd YUY2 height, complete GUID suffix mismatches,
bits-per-pixel mismatches, per-format negotiated-size mismatch, and every
descriptor-buffer truncation directly against a no-access guard page.
Layout output itself is written immediately before a guard page.

Overflow tests include a valid 65534x43692 4:2:0 frame of **4294967292**
bytes and the rejected next even height. Its final row ends exactly at the
computed boundary. This tests arithmetic only; no large allocation occurs.

`host.ps1` rebuilt the actual C integration DLL; `simtest.py`: **7 tests OK**.
`flowtest.cmd`: **361 checkpoints pass**. The immutable physical Brio100
`CONFIG.BIN` still parses as result0/Valid1, UVC0100, **38 frames/11
alternates**. This is descriptor-fixture regression only, not a new camera
streaming run. The new NV12/I420/M420 configurations are explicitly synthetic.

SHA256 at this check:

| File | SHA256 |
|---|---|
| `uvcdesc.c` | `a188b45d962bba98bac809523fa9c01f01fd6c2b1f40fcc2dd5d80cba19820d0` |
| `uvcdesc.h` | `b6612046ccc600d374be5daf3c49aafb86f792a084b0d17e5fd812a05b27f1a1` |
| `desctest.c` | `0396ad7f4fd4acf04f7239ca3ee5d7ca73dc4225f665aa5e670d43eabcd9785c` |
| `out/video/uvc98/desc/DESCTEST.EXE` | `e46fc11407a98ca8e5ee9f4d771d7d46b3ba3e5da87b446e2c7d385c593ae3c9` |

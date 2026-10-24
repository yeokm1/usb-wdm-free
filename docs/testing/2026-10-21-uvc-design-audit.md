# UVC negotiation and payload design audit, 2026-10-21

This bounded review checks sections 5.2, 5.5 and 5.6 of the draft
[UVC design](../design/uvc.md), and reads the new portable `uvcpay.c` core.
It does not establish a working webcam driver, Windows installation,
camera capture, preview, USB compliance or compatibility with any device.
There is no webcam kernel/USB transport implementation tested here.

## References and method

Official PDF origins, exact hashes, retained notices and archive-member
mapping are recorded in [the acquisition record](2026-10-21-uvc-spec.md).
The PDFs remain ignored local references, not redistributed artifacts.
Page numbers below distinguish printed pages from one-based PDF pages.
Text was extracted with pypdf; the UVC 1.1 payload table and UVC 1.5 probe
table were also rendered with Poppler and visually checked.

Linux comparison uses `drivers/media/usb/uvc/uvc_video.c` at pinned v6.12
commit `adc218676eef25575469234709c2d87185ca223a`, whose original bytes and
license notices remain in ignored `docs/references/linux-v6.12/`.
See [the source provenance record](2026-10-21-linux-refs.md).
No Linux code is copied or adapted by this audit; its implementation is a
comparison, not a replacement for the USB specification.

## Probe and commit

| Point | Exact reference | Finding and design consequence |
|---|---|---|
| UVC 1.1 layout | Core section 4.3.1.1, Table 4-47, printed p.103 onward / PDF p.116 onward | `wLength` is 34. The advertised revision governs the complete layout; accepting a short data response as a complete structure is unsafe. |
| UVC 1.5 layout | Core section 4.3.1.1, Table 4-75, printed p.134 onward / PDF p.149 onward | `wLength` is 48. Merely extending a 34-byte allocation without validating the additional fields is not a verified 1.5 implementation. |
| UVC 1.0 baseline | Linux `uvc_video_ctrl_size()`, lines 241-253 | Linux selects 26 bytes below revision 1.1, 34 below 1.5, and 48 thereafter. No UVC 1.0 core PDF was reviewed; the design now states that independent normative review remains outstanding. |
| Negotiated values | UVC 1.1 section 4.3.1.1.1, Tables 4-48/4-49, printed pp.112-113 / PDF pp.125-126 | Unsupported and negotiable fields start zero; GET_CUR returns adjusted values, and COMMIT uses the successful probe result. Validate returned format/frame indices, interval, sizes and endpoint feasibility rather than assuming the initial request was accepted. GET_LEN exists for both controls, but does not make a short or incompatible layout valid. |
| Maximum payload ownership | UVC 1.1 Table 4-47, printed p.109 / PDF p.122; UVC 1.5 Table 4-75, printed p.139 / PDF p.154 | The device sets `dwMaxPayloadTransferSize`. The 1.5 text explicitly calls for host reconfiguration to avoid overshoot. A host cannot silently replace this value with an endpoint's smaller capacity and assume the stream will fit. |

The draft incorrectly attributed a general clamp to the largest alternate
to Linux's `uvc_fixup_video_ctrl()`. That function contains frame-size
repair, a sign-extension repair and a separately enabled bandwidth quirk;
it does not establish the claimed general clamp. `uvc_probe_video()`
attempts compression negotiation, while `uvc_video_start_transfer()`
selects a sufficient endpoint alternate and fails when none exists.
The draft now requires renegotiation or failure and distinguishes candidate
compatibility repairs from the normal protocol. Endpoint service interval,
link speed and high-bandwidth transaction count still need validation in
the future USB transport, beyond this review's scope.

## Payload boundaries and header bounds

UVC 1.1 core section 2.4.3.3, Table 2-5, printed pp.31-32 / PDF pp.44-45,
requires a payload header and identifies FID, EOF, PTS, SCR, STI and ERR.
Table 2-6, printed pp.32-33 / PDF pp.45-46, specifies four PTS bytes and
six SCR bytes. SCR's SOF portion is 11 bits; its upper five bits are reserved.
Thus the present base header needs at least
`2 + (PTS ? 4 : 0) + (SCR ? 6 : 0)` bytes, within both declared header
length and actual received length. Ignoring timestamp values does not
remove the need to validate their declared storage. Future extended header
groups are not verified by this audit.

For frame-based formats FID toggles at the start of each frame. A toggle
can finish the old buffer, but the very same payload then belongs to the
new frame. Linux `uvc_video_decode_start()` returns `-EAGAIN` for its caller
to repeat that payload after advancing buffers. Dropping it would lose the
new frame's first data. Initial partial frames should be discarded, and FID
state must keep advancing even when no delivery buffer is available.

The core Table 2-5 describes EOF as optional. The uncompressed 1.1 payload
section 2.4, printed pp.2-3 / PDF pp.9-10, and MJPEG 1.1 payload section
2.2, printed pp.2-3 / PDF pp.9-10, describe setting EOF in the final sample.
The receiver should tolerate absent EOF using FID boundaries, rather than
requiring EOF to make progress. EOF completes an accumulated nonempty frame
once; a header-only EOF may close earlier data, but must not publish an
empty frame. Linux `uvc_video_decode_end()` uses that nonempty-buffer rule.

ERR describes a transmission error for the payload. The receiver policy
must retain that error on the affected frame until its boundary and discard
the complete frame, rather than silently stitching good fragments around
the bad one. This is a conservative delivery policy derived from the
payload semantics, not a separate spec-mandated dropped-frame API.
Overflow and failed/malformed payloads need the same bounded treatment.
Unknown lost headers can conceal a new FID: the first observed frame after
such loss may already be partial and must also be discarded before resuming.
STI denotes still capture methods 2/3; the draft's method-2 path must only
route it when that mode is negotiated. No-FID devices need separate quirks.

For bulk, core section 2.4.3.2, printed pp.23-24 / PDF pp.36-37, separates
USB transfers, UVC payloads and video samples. A payload can span USB/URB
chunks. Linux `uvc_video_decode_bulk()` retains the first header until a
short transfer or maximum payload size ends it, then applies its saved EOF.
The draft now makes these boundaries explicit and does not claim that
rounding a USB read buffer changes the negotiated UVC maximum. Fragmented
bulk input is outside the new portable core's contract.

## Read-only review of the portable core

The initial `src/video/uvc98/uvcpay.c` reviewed here bounds the header and
optional fields, advances the same toggling-FID payload into the new frame,
suppresses initial partial data and duplicate EOF delivery, retains ERR
on the active frame, and limits copies to caller capacity. Expected fixed
frame size is optional so compressed frames can have variable size.
The review does not repeat or extend the root agent's host-test claim.

Two concrete issues were reported directly to its owner for correction:

- `UvcPayLost()` originally marked only the currently active frame. If the
  missing payload was the first payload of the next FID, the next observed
  toggle cleared that error and a partial compressed frame could be
  delivered. Require loss resynchronization and a regression that loses a
  frame's first payload. This is a correctness issue even with bounded
  memory accesses.
- The initial SCR extraction exposed all 16 bits as `ScrSof`; that field
  should mask the five reserved bits or explicitly be named/documented as
  raw storage. Timing is not implemented, so this is not a measured clock
  recovery failure.

The owner then added `Unsynced`: loss marks an active frame bad, and the
first surviving valid header closes/drops it and establishes a discarded
FID. Only the subsequent toggle starts a trusted frame. Repeated loss or
malformed headers retain this resynchronization requirement. This closes
the reported partial-compressed-frame path, including loss when no output
frame is active. The owner also masked `ScrSof` with `0x7ff`.

The corrected source and variable-size frame-boundary-loss regression were
read again. Running `src/video/uvc98/test.cmd` independently after those
changes returned **PASS: 94104 guarded UVC payload checks and frame-sequence
assertions**. This verifies the bounded host-side core under its documented
complete-payload, serialized, synchronous-callback contract; it does not
exercise actual USB loss, scheduling, bulk fragments, image decoding or an
OS camera driver. No webcam, audio or VM state was changed by this audit.

## Subsequent portable descriptor and control implementation

The initial implementation described below exposed YUY2/MJPEG. A later
[uncompressed layout expansion](2026-10-21-uvc-layouts.md) adds portable
NV12/I420/M420 descriptor/frame validation and tight row geometry, with
7497 descriptor/codec checks. Its record supersedes the initial two-format
limit below; it adds no USB transport, consumer decoder or kernel RUN.

`src/video/uvc98/uvcctrl.c/.h` now implements an original allocation-free
probe/commit wire codec. It accepts exact advertised-revision lengths for
1.0 (26), 1.1 (34) and 1.5 (48), rejects other revisions and short/long
buffers, performs little-endian scalar accesses without packed casts, and
rejects field values that would truncate on encoding. Decode zeroes output
before failure. The 14 extra bytes in the 1.5 layout are retained verbatim
as opaque encoder fields; there is no claim to implement their policy.
Neither codec function issues SET_CUR/GET_CUR or carries out negotiation.
The 26-byte baseline still has the independent normative-review limitation
recorded above.

`uvcdesc.c/.h` implements the original bounded descriptor model and selection
check with caller-owned storage and an immutable caller-owned configuration:

- It stops at configuration `wTotalLength`, checks descriptor steps,
  interface/alternate uniqueness, actual endpoint counts and alternate-zero
  presence. Its function anchor is an explicit VC interface number. The
  VC header's `baInterfaceNr` collection is restricted by an enclosing IAD
  when present; a VS interface assigned to another IAD or claimed by a
  second VC collection is rejected. It does not select neighboring HID or
  audio functions. Core 1.1 section 3.6, printed p.47 / PDF p.60, and
  section 3.7.2/Table 3-3, printed pp.49-50 / PDF pp.62-63, supply these
  association and VC header facts.
- VC/VS class blocks stay inside both class `wTotalLength` and their
  containing interface. The VS input header and its control-array size are
  bounded; terminal linkage must identify one USB-streaming output terminal.
  See core 1.1 section 3.9.2.1/Table 3-13, printed pp.62-64 / PDF pp.75-77.
  This is not a complete unit/entity graph or control-bitmap validator.
- Only YUY2 (matching GUID and 16 bits/pixel) and MJPEG frame descriptions
  are exposed. Recognized unsupported format families are skipped, not
  converted into invented supported formats. Unknown future layouts may
  be rejected. Format/frame counts, indices, defaults, dimensions, ordered
  discrete intervals and continuous min/max/positive-step ranges are checked.
  YUY2 width must be even and `width * height * 2` must fit 32 bits.
  Uncompressed 1.1 section 3.1, Tables 3-1 through 3-4, printed pp.4-8 /
  PDF pp.11-15, and MJPEG 1.1 section 3.1 define these fields. The deprecated
  descriptor maximum frame-size field is retained separately rather than
  treated as a substitute for the negotiated IN value.
- Full/high-speed IN endpoint alternatives report packet bytes,
  transactions, capacity per service interval and interval in microframes.
  Full-speed isochronous packets are bounded to 1023 bytes; high-speed
  packets to 1024 bytes with at most three transactions. Reserved packet
  bits, invalid high-bandwidth combinations and invalid intervals are
  rejected. USB 2.0 section 9.6.6/Table 9-13, printed pp.269-272 / PDF
  pp.297-300, and Table 9-14, printed p.273 / PDF p.301, define the wire
  fields and high-bandwidth packet minima (513 for two transactions, 683
  for three). Bulk is limited to alternate zero with legal full/high-speed
  packet sizes; no bulk reassembly or USB reads are implemented.
- `UvcDescSelect` validates a device-adjusted format/frame/interval and
  nonzero frame size, requires exact derived YUY2 frame bytes, and selects
  a sufficient smallest isochronous alternate. It never reduces the
  negotiated payload requirement. For bulk, the negotiated payload can span
  packets, so endpoint packet size is not incorrectly used as its limit.
  This check neither reserves bandwidth nor promises an allocation can fit.
  Callers must enforce their own negotiated frame-allocation budget,
  especially for MJPEG. Its deprecated descriptor maximum is not silently
  substituted for the device-adjusted probe value. USB-valid intervals
  through 16 are parsed; a Windows transport with narrower scheduling
  support must explicitly refuse those it cannot carry.
- Storage caps are 8 streams, 128 supported frames and 128 endpoint
  alternatives. Exceeding them returns a limit error. All failed parses
  leave `Valid=0`; callers must consume no partially built model fields.

`src/video/uvc98/desc.cmd` builds the original host tests with VC6 at
`/W4 /WX`. The final recorded result is **PASS: 6059 synthetic
descriptor/codec checks including guarded truncations and mutations**.
Tests include independent known wire offsets, 26/34/48-byte round trips,
buffers ending at a no-access page, every configuration truncation,
all-byte single-bit mutations, incorrect class totals, wrong function and
duplicate collection references, invalid intervals/defaults/indices,
YUY2 arithmetic overflow, high-bandwidth endpoint restrictions, service
interval conversion, fixed storage exhaustion and oversized negotiated
payload rejection without clamping. A synthetic full-speed bulk fixture
also verifies that payload size may exceed one endpoint packet.

All fixtures are explicitly **synthetic**, not captured camera descriptors.
No real camera has been matched, probed, committed or streamed by this
code, and no kernel package is created. A live device/configuration capture
and hardware/OS integration tests remain separate gates. No downloaded
source was adapted; no additional third-party NOTICE row is required for
these original files.

## Independent review of the subsequent usbredir simulation

The original `uvcemu.py`, `simtest.py`, `host.ps1` and `hostabi.c` were
reviewed before the owner's native preview experiment. The explicit model
is local `FFFF:0002`, UVC 1.1, full-speed bulk, YUY2 160x120 at 10 fps.
It generates moving bars, not a physical camera image. The host DLL exposes
the actual original C descriptor and payload cores plus their ABI sizes;
Python tests pass opaque correctly sized state buffers instead of assuming
a separate hand-written structure layout.

Two concrete protocol defects were reported to the model owner:

1. An initial 1024-byte payload with a two-byte header left 1022 YUY2 data
   bytes, splitting a four-byte macropixel. Uncompressed 1.1 section 3.3.2,
   printed p.8 / PDF p.15, requires packed transfers on macropixel
   boundaries. The owner changed the maximum payload to **1022**, leaving
   **1020 data bytes**, and added an alignment assertion.
2. Initial COMMIT handling silently normalized any format/frame 1/1 input
   to the default, even a mismatched interval or frame/payload size. Core
   1.1 Table 4-49, printed p.113 / PDF p.126, requires the successful probe
   result and rejects unsupported committed states. The owner now compares
   the complete submitted control with the negotiated probe and stalls a
   mismatch; a regression covers submitting the unadjusted request.

A further simulation boundary was reported: splitting payloads at arbitrary
tiny or misaligned host read-buffer sizes can manufacture short USB packet
boundaries inside a UVC header/data continuation. The model now explicitly
refuses reads that would split an actual 64-byte USB packet. Aligned reads
and a request that fits the complete remaining short packet are supported;
larger reads can receive a complete payload terminated by its short packet. This is an explicit
model subset, not a universal USB bulk/overflow implementation.

The corrected header is two bytes, EOH set, PTS/SCR/STI/ERR clear. FID
alternates per generated frame and EOF appears on the final payload. The
payload's terminal USB packet is short with the chosen 1022-byte maximum.
Byte continuation across aligned host-buffer chunks is retained; only the
first chunk of a UVC payload has its header. Three generated frames through
the C assembler yield the two expected complete images after its deliberate
initial-partial-frame discard.

The owner extended request-size coverage to 64/128/512/1022/4096/65535,
tiny/misaligned-read refusal and bounded cancellation/reset. The 256-entry
pending queue stores identifiers/lengths, not full requested buffers. Thus
large queued lengths do not directly allocate a matching image buffer.
The subsequent native Windows 7 preview experiment submitted fifteen
64-byte reads followed by a 62-byte read for a 1022-byte payload. The
initial restriction also rejected that legitimate final short packet.
The model now checks the remaining payload at delivery time: an unaligned
request may consume the complete remaining short packet, but cannot split
a larger USB packet. A seventh integration test reconstructs the exact
15x64+62 sequence and checks its header and 1020 YUY2 data bytes. All seven
tests pass. This is an additional modeled request pattern, not universal
USB overflow emulation. The native preview run is recorded separately.

The model/network runner still has ordinary blocking socket I/O; this is
not a stress-tested arbitrary-peer server or a production transport.

An independent run of `simtest.py` against the corrected source and existing
host DLL returned **6 tests, OK**. This checks synthetic descriptor/control
behavior and exact generated bytes through owned C cores. It does not test
a real camera, Windows inbox-driver packet scheduling or native preview,
and does not establish USB conformance. The owner records any guest preview
experiment separately. No simulator/preview source or VM was changed by
this reviewer.

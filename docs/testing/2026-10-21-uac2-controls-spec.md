# UAC2 control/descriptor specification audit, 2026-10-21

Primary reference: USB-IF **USB Device Class Definition for Audio Devices,
Release 2.0, September 2026**, local ignored
`docs/references/audio2-2026.pdf`, SHA-256
`14CED91EAAF64EEDEB11C93BABBCB2459B84A9D1160ED3925DE9FDF08E00BD1D`.
Page references below are the printed page numbers and one-based PDF pages.
The earlier April 2025 file `audio2.pdf`, SHA-256
`0D8D56901EE9257D1B3647F8D7CC4D4CDD204DC0ECF498376524FB8E14163DC3`,
was consulted first; the relevant control tables were rechecked against
2026. The PDF remains internal reference material, not a release payload.
This note contains original analysis, not reproduced specification pages.

## Request encoding and clock controls

| Driver operation | Specification anchor | Assessment |
|---|---|---|
| `EntityRequest`, `ClockRequest` | §5.2.2, Table 5-1, pp.90–91 | Interface class request; direction selects GET/SET, selector occupies high `wValue`, master channel zero occupies low byte, entity occupies high `wIndex`, AC interface occupies low byte. This matches the current URB construction. |
| `CUR=1`, `RANGE=2` | A.14, Table A-14, pp.134–135 | Request constants match. |
| Clock frequency selector 1 and validity selector 2 | A.17.1, Table A-17, pp.135–136 | Selectors match. |
| Four-byte little-endian frequency CUR | §5.2.5.1.1 pp.97–98; §5.2.3.3, Table 5-6 p.93 | GET, conditional SET to 48000, and GET readback use the correct width. Readback avoids assuming a device accepted the exact requested value. |
| RANGE count then full response | §5.2.2 p.91; Table 5-7 pp.93–94 | A short requested prefix is explicitly supported. Requesting two count bytes first, then `2+12*n`, is valid. Triplet offsets MIN=2, MAX=6, RES=10 match. |
| Clock validity GET | §5.2.5.1.2 p.98; Layout 1 p.92 | Read-only one-byte CUR, CN0, is correct. Requiring TRUE before streaming is conservative. No settle/retry guarantee is established by this single observation. |
| Clock capability gate | §4.7.2.1, Table 4-6 pp.49–50 | Low bitmap nibble `0x07` correctly denotes frequency read/write and validity read-only. This is an X4 restriction; valid fixed read-only clocks are not supported by this transport. |

The current `SetClock` RANGE decoder has a validation gap: §5.2.1 p.90
requires ordered, nonoverlapping subranges, and a single-value subrange
must use zero RES. The decoder neither checks ordering/overlap nor rejects
MIN=MAX with nonzero RES. It also interprets nondegenerate zero-RES ranges
as continuous; this audit found no specification basis for that meaning.
Failing closed for that case is recommended rather than assuming support.
This was reported to the runtime owner; it has not been demonstrated as
the X4 playback failure.

The decoder rejects zero MIN and caps subranges at 32. The frequency
control definition permits zero-Hz attribute values, so the former is
stricter than the general contract. The latter is a deliberate bounded
implementation limit. Neither restriction establishes support for all
otherwise valid UAC2 clocks.

## Clock topology and feature controls

`UacClock` reads Input Terminal clock ID at offset 7 and Output Terminal
clock ID at offset 8, matching Tables 4-9/4-10, pp.52–54. Selector sources
start at offset 5 and selectors use one-based input indices, matching
§4.7.2.2/Table 4-7, pp.50–51 and §5.2.5.2.1 p.98. The actual X4 path is
Terminal 3 → one-input Selector 34 → Source 33. Choosing its sole input
does not invent a multi-input selection. Multiple-input selectors require
an external/current selection; the present transport rejects them, and
clock multipliers remain unsupported.

`ReadFeature` correctly tests master-channel mute/volume capability pairs
at bitmap bits 1..0 and 3..2. The capability encodings are described in
§4.7.2.8/Table 4-13, pp.58–59. Master GET_CUR mute uses one byte and selector
1; volume uses two signed little-endian bytes and selector 2, matching
§5.2.5.7.1–2 pp.102–103 and A.17.7/Table A-23 p.137. It performs no mute
or volume SET. Reading only CUR is sufficient for these passive diagnostic
values; this is not a volume-control implementation.

Volume code `0x8000` denotes silence, not ordinary -128 dB gain (§5.2.5.7.2,
p.103). Diagnostics retain its raw signed value. X4 FU1 has master bitmap
`0x0F` and eight channel bitmaps `0x0C`: per-channel volume exists too.
Master mute/volume observations therefore cannot rule out individual
channel attenuation or silence. Read-only channel GETs would be useful
if nonzero USB delivery succeeds but audible output remains absent.

## Descriptor gaps reproduced and corrected

Table 4-5 p.48 defines the AC header subtotal as the header plus AC entity
descriptors. Table 4-13 p.59 defines Feature Unit length as
`6+4*(channels+1)`, hence at least 10 bytes and a whole number of four-byte
control bitmaps. The original parser accepted both of these malformed,
otherwise well-framed fixtures:

- XP configuration with AC subtotal changed to 1.
- FU1 shortened from 42 to 9 bytes, with the removed bytes excised and
  both configuration and AC lengths adjusted consistently.

The temporary audit executable compiled the actual `uacdesc.c`; both
counterexamples returned `UAC_OK`. This demonstrates missing structural
validation, not an out-of-bounds read or a known X4 firmware defect.

With the runtime owner's agreement, `uacdesc.c` now checks AC subtotal
bounds and the exact sum of AC class-specific descriptors, requires
entities after the AC header, and rejects Feature Units shorter than 10
or with incomplete four-byte control arrays. `desctest.c` checks both the
full and XP-filtered captures against subtotal 0, 8, ±1, configuration
size and 65535, plus correctly reframed FU lengths 9, 11 and 13.
`test.cmd` passes 3555 assertions and 1143 byte mutations. Conditional
assertion counts changed because more malformed mutations are rejected.
The portable parser is still not a complete UAC2 descriptor validator.

Parser SHA-256 after this correction:
`C8EC6870BED6B0CE04343AD35A774DA0268A13AE073888CE9E28DCCD5B1DA83F`.
Transport reviewed at SHA-256
`31C7CC0CC0C60C125192D6DE7E877BDA1D8ED5C0417793EEDB921C72DBAA5B32`;
subsequent runtime-owner edits require a fresh check of outstanding gaps.
No main miniport, INF, USB transport or VM was changed by this audit.

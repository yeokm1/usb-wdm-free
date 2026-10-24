# UAC2 portable control core, 2026-10-21

New original `src/audio/uac2/uacctl.c`/`.h` implements a bounded, allocation-free
wire core independent of the X4 driver. It is not integrated into the USB
transport, PortCls properties or Windows mixer UI. No existing SYS, INF,
transport or driver build was changed. No USB requests were sent in this work.
The full [feature ledger](../design/uac2-features.md) remains the completion
target; this is partial host-tested implementation, not full UAC2 support.

The source is derived from the official Audio Device Class 2.0 September2026
edition (`audio2-2026.pdf`, SHA256
`14ced91eaaf64eedeb11c93babbcb2459b84a9d1160ed3925de9fdf08e00bd1d`).
Its [acquisition record](2026-10-21-uac2-spec.md) provides the official source
and internal-reference licence boundary. No specification text is copied into
the implementation, and no DDK sample or binary code was consulted.

## Implemented boundaries

| Component | Behavior | Exact class specification anchor |
|---|---|---|
| `UacCtlAccess` | Extract one selector's two-bit advertised lane; 00 absent, 01 readable, 11 read/write; reject reserved10, selector0/overflow and short bitmaps. Caller must select the appropriate descriptor/channel bitmap. | Descriptor `bmControls` definitions, e.g. §4.7.2.1 Table4-3 pp.49-50; Feature §4.7.2.8 Table4-12 pp.59-60 |
| `UacCtlPolicy` | Explicit partial selector/width/recipient/channel/read-only/RANGE policy. Unknown selectors and variable-layout controls fail. | Sections and selector tables below |
| `UacCtlSetup` | Serialize exactly eight little-endian setup bytes. Reject invalid GET/SET/attribute/access, out-of-byte target fields, forbidden entity/interface/endpoint combinations, reserved endpoint-address bits, endpoint0 and out-of-cluster channels. GET_RANGE supports a two-byte count prefix; SET_RANGE requires independently established write capability and a nonempty triplet-shaped length. | §§5.2.1-3 pp.89-94, Table5-1 pp.90-91; AppendixA.14 pp.134-135 |
| `UacCtlValue` | Validate exact scalar CUR lengths and applicable Boolean, selector pin-count, multiplier nonzero, encoder quality/type and format-bit constraints. Preserve volume's silence sentinel; reject that sentinel for input gain/pad and PE gain where it is not defined. Encoder Param1..8 are signed32 opaque values. | Sections below; common controls §§5.2.4.1-8 pp.94-97 |
| `UacCtlRange`/`RangeGet`/`RangeContains` | Parse complete Layout1/2/3 RANGE responses, widths1/2/4, signed/unsigned extrema. Check count versus exact actual length, ordered nonoverlapping intervals, MIN<=MAX, positive resolution for an interval and RES0 for a single value. Membership uses step alignment. No allocation or unaligned typed loads. | §5.2.1 pp.89-90; §§5.2.3.1-3 pp.92-94, Tables5-3,5-5,5-7 |

All output buffers/structs stay unchanged on failure. The RANGE view refers
to caller-owned immutable storage that must remain alive. Its signed64
intermediate accommodates the entire signed32 span and unsigned32 maximum
without overflowing a signed32 subtraction. A valid view must originate from
`UacCtlRange`; it is not a capability to validate arbitrary pointers.

The setup encoder requires the caller to establish entity existence,
interface ownership, endpoint type, descriptor-specific permission lane and
channel count. A valid numeric address is not proof that the target exists.
Endpoint targets use Entity=Interface=0 and an endpoint address; AS-interface
targets use Entity=Endpoint=0. Clock/feature/effect/encoder targets use an
entity ID plus its actual AC or AS interface. SET_RANGE is not authorized
merely by a read/write CUR lane: §5.2.2 permits programmable RANGE, but its
availability needs separate device-specific evidence.

## Explicit policy coverage

Selectors below are hexadecimal. All support CUR. `R` indicates generic scalar
RANGE shape; actual advertised access still gates every request.

| Family | Implemented selectors and layout | Exact sections/pages and selector tables |
|---|---|---|
| Clock Source | 01 frequency U32/R; 02 validity Boolean, read-only | §§5.2.5.1.1-2 pp.97-98; A.17.1 TableA-17 pp.135-136 |
| Clock Selector | 01 selection U8, one-based up to descriptor input-pin count, master only | §5.2.5.2.1 p.98; A.17.2 TableA-18 p.136 |
| Clock Multiplier | 01 numerator,02 denominator U16 nonzero, read-only, master | §§5.2.5.3.1-2 p.99; A.17.3 TableA-19 p.136 |
| Selector Unit | 01 selection U8/pin count,02 latency U32 read-only, master | §5.2.5.6.1 p.102; §5.2.4.8 p.97; A.17.6 TableA-22 p.137 |
| Feature Unit | 01 mute,07 AGC,09 bass boost,0A loudness,0D phase inverter,11 high-pass Boolean; 02 volume,0B input gain,0C gain pad S16/R; 03 bass,04 mid,05 treble S8/R; 0E underflow,0F overflow Boolean read-only; 10 latency U32 read-only/master | §§5.2.5.7.1-5 pp.102-104, §§5.2.5.7.7,9-16 pp.105-108; common under/overflow/latency §§5.2.4.4-5,8 pp.95,97; A.17.7 TableA-23 pp.137-138 |
| Parametric EQ effect | 01 enable Boolean/master; 02 frequency,03 Q-factor U32/R; 04 gain S16/R; 05 underflow,06 overflow Boolean read-only; 07 latency U32 read-only/master | §§5.2.5.8.1.1-6 pp.108-109; common latency §5.2.4.8 p.97; A.17.8.1 TableA-24 p.138 |
| Encoder | 01 bitrate U32/R; 02 quality U8/R with CUR0..100; 03 VBR Boolean; 04 type U8/CUR1..9; 05 underflow,06 overflow Boolean read-only/per-channel; 07 error S16 read-only; 08..0F Param1..8 S32/R, meaning opaque | §§5.2.6.2.1-8 pp.118-120; common channel status/error §§5.2.4.4-6 pp.95-96; A.17.12 TableA-33 p.142 |
| AS interface | 01 active alternate U8,03 active format U32/exactly one bit, both read-only/master | §§5.2.6.1.1,3 pp.117-118; A.17.11 TableA-32 pp.141-142 |
| AS endpoint | 01 pitch Boolean; 02 overrun,03 underrun Boolean read-only/master | §§5.2.6.4.1-3 p.127; A.17.14 TableA-38 p.144 |
| Mixer Unit | 01 matrix node S16/R; low request byte is zero-based MCN, not master/channel numbering. The [Linux-adapted matrix helper](2026-10-21-uac2-mixer.md) establishes node number and fixed/programmed access | §4.7.2.6 pp.55-57; §5.2.5.5.1 p.101; A.17.5 TableA-21 p.137 |

The generic RANGE parser checks scalar wire structure, not every control's
domain-specific RANGE restrictions. For example a volume MIN cannot be the
silence sentinel, and encoder-quality bounds are limited to0..100. Future
control-specific RANGE validation must apply such rules before presenting a
range to users or generating writes. The current serializer also requires a
full scalar CUR length, although the class's generic GET convention permits
prefix retrieval. Only the RANGE count-prefix workflow is provided here.

Graphic EQ's band bitmap, delay's special representation, valid-alternate
bitmaps, other mixer unit controls, other effects/processors, terminal clusters,
decoder-specific layouts, MEM and status interrupt payloads are not
implemented by this module. Their ledger requirements remain open. A
standardized encoder Param<X> wire representation does not define or reverse
engineer vendor parameter semantics or provide an audio codec.

## Reproducible checks

Run `src/audio/uac2/ctest.cmd` with the repository-named MSVC6 toolchain.
It builds the portable core independently with `/Od` and `/O2`, `/W4 /WX`,
and runs **9,613,940 checks in each build**. Ignored logs are in
`out/audio/uac2/control/`.

Tests cover all access-lane bit patterns, every known policy's read/write
restrictions, endpoint-address legality, unchanged setup on failure,
selector pin counts, Boolean/quality/type values, guarded complete/truncated
responses, touching/overlapping/inverted/zero-count ranges, single-value
resolution and signed32/unsigned32 extrema. An independent integer oracle
checks 100,000 randomized signed/unsigned8 triplets and membership of every
8-bit value for accepted intervals. Input fixtures end immediately before
an inaccessible page; lengths0/1 must not dereference the inaccessible page.

The assertion/fixture harness is compiled `/Od`. An initial MSVC6 `/O2`
harness build produced incorrect fixture bytes; adding an unrelated diagnostic
print changed that outcome. This test-harness optimization discrepancy is not
attributed to USB or the core and has not been fully reduced. Both optimized
and unoptimized core builds pass the same unoptimized guarded harness.
An independent **GCC16.1.0 x64** build with both core and full harness at `-O2`
also passes all9,613,940 checks. So does `-fsanitize=undefined` with
`-fsanitize-undefined-trap-on-error` (trap mode avoids an unavailable host
libubsan runtime). These checks did not diagnose undefined behavior, but they
do not establish a minimal compiler-defect reproduction. The optional
`gtest.ps1 -Compiler <installed-gcc.exe>` reruns these checks with no fetching;
the actual host compiler was MSYS2's GCC16.1.0 under
`C:/Users/yeokm1/scoop/apps/msys2/current/mingw64/bin/gcc.exe`, independent of
the normal repository-local MSVC6 build. No
target-kernel, Windows property, simulated-device or physical-control success
is inferred from these host checks.

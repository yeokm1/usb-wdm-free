# UAC2 feature coverage ledger

Status: 2026-10-21; the Feature Unit volume and mute rows 2026-10-28. The requested target is the full standardized UAC2
feature set, including its applicable errata and companion format/terminal
definitions. This supersedes the narrower feature exclusions in
`uac2.md`; those exclusions describe the initial experiment, not the agreed
completion boundary. No missing standardized family below is silently
removed from the work. This ledger is a requirements inventory and an
evidence boundary, not a conformance or certification statement.

Support means discovering and validating each advertised capability,
issuing only controls whose advertised access permits them, transporting
the applicable format, and exposing usable behavior to the target OS.
Devices need not implement every optional feature; a host must not invent
an optional capability or require an absent one. A descriptor parser
accepting a field is not a working stream or control implementation.

## References and evidence levels

Page numbers below are printed pages, also the one-based PDF pages in
these three editions. `A` means the September 2026 Audio Device Class
Definition 2.0; `F` means Audio Data Formats 2.0, May 31, 2006; `T` means
Terminal Types 2.0, May 31, 2006. The unchanged official PDFs are retained
only as internal references under ignored `docs/references/`. Acquisition,
URLs, archive membership and licence boundaries are recorded in the
[specification acquisition log](../testing/2026-10-21-uac2-spec.md).

| Reference | Official origin | SHA-256 |
| --- | --- | --- |
| A: `audio2-2026.pdf` | [USB-IF, errata/ECNs through September 14, 2026](https://www.usb.org/sites/default/files/Audio2_with_Errata_and_ECN_through_Sep_14_2026.pdf) | 14ced91eaaf64eedeb11c93babbcb2459b84a9d1160ed3925de9fdf08e00bd1d |
| F: `audio-formats2.pdf` | `Audio2.0 final/Frmts20 final.pdf` in [USB-IF original set](https://www.usb.org/sites/default/files/Audio2.0_final.zip) | eff2b5e0b0795e0744b5caa06aad425a3073dc051f31fd6fbb4bc6e6ed142781 |
| T: `audio-terminals2.pdf` | `Audio2.0 final/Termt20 final.pdf` in the same original set | 3a633f67988493f520a9021c65a2000e03307aef69bd0a2ddb7880f733e22458 |

**P** = physical-device evidence for the explicitly named configuration;
**S** = simulated-device/guest evidence; **H** = portable or direct host test;
**C** = source implementation only; **M** = missing. These labels do not
transfer between configurations, controls, formats or operating systems.
The current code baseline is `src/audio/uac2/`: `uacdesc.c`, `uacrate.c`,
`uacusb.c`, `uacstream.h` and `uac2wdm.cpp`. Runtime evidence and binary
identities are in [the VM/physical log](../testing/2026-10-21-uac2-vm.md).

The current physical evidence is only Creative X4 041E:3278, Windows 7
x86 guest, QEMU EHCI, host libusbK, interface 4 alternate 1, stereo signed
16-bit PCM at 48 kHz, OUT01 plus feedback83. Installed SYS SHA-256:
`c7a172067c3d9b540c84d965664ab9bdca90ac435a1374b0759ef85d7e61126e`.
Two controlled AudioSrv restart/immediate-player runs completed 8/8 buffers
and 96000 client samples with checked close. Each recorded **nine packet
errors** and 40/41 rejected feedback values. Nonzero USB data and valid
feedback were recorded. Audible output, fidelity, absence of packet loss,
long-duration operation and unconstrained startup are **not verified**;
the earlier startup/watchdog timeout remains a limitation. This is not
general playback qualification, even for every X4 alternate.

Subsequent isolated QEMU/libusb diagnostics confirmed that folded libusbK
packet errors had been classified as success. The latest strict WinMM PASS
still contains24 failed raw OUT packets and24 QEMU empty-ring events, despite
the guest reporting nine errors. Its host OUT window/QPC is about two seconds,
while GetTickCount reports1500ms. Neither low guest error counts nor a WinMM
PASS alone establishes clean delivery. Current priority is repeated zero-error
physical transport runs before wider feature integration. See the latest
sections of the runtime log for exact binary identities and negative runs.

## Function discovery, streams and timing

| Required family | Specification anchor | Current evidence and remaining work |
| --- | --- | --- |
| Function/interface collection, IAD boundaries, protocol 0x20, multiple audio functions, categories and strings | A §§3.2-3.9 pp.19-20; §§4.2-4.7 pp.45-48; A.1-A.7 pp.131-132 | **H/C partial:** bounded function parser validates IAD/interface boundaries and captured X4 layouts. Runtime is an exact X4 path. **M:** generic multi-function discovery/binding, full category/string preservation, all valid descriptor layouts. |
| Device/configuration/qualifier/other-speed descriptors and alternate setting transitions | A §§4.2-4.6 pp.45-46; §§3.16.2,4.9 pp.39-40,75-84 | **H/C partial:** configuration parsing and exact X4 alt0/alt1 selection. **M:** comprehensive other-speed/qualifier policy, generic alternate negotiation, recovery/rollback and multi-stream transitions. |
| Playback and capture, simultaneous full duplex and independent AS interfaces | A §§3.13.2-3 pp.27-28; §3.16.2 pp.39-41 | **P/S:** one playback path only. Parser can retain Type I IN candidates; miniport rejects capture streams. **M:** capture USB/rings/positions/OS pins, duplex admission and shared-clock/resource arbitration. |
| Mono, stereo, multichannel and named/raw channel clusters | A §§3.12-3.13.1 pp.21,25-27; §4.1 pp.43-44; F §2.3.1 pp.13-16 | **H/C partial:** channel counts retained; runtime admits exactly two. **M:** generic channel masks/order, predefined and raw-data positions, cluster names, per-channel controls and multichannel OS formats. |
| Multiple simultaneous clusters/functions; shared and independent clock domains; inter-channel synchronization | A §§3.10,3.12 pp.21; §3.16.2.1 p.40; §3.16.3 p.41 | **M:** no general concurrent stream/domain manager or inter-stream synchronization test. A single endpoint's progress does not cover this. |
| Full-speed and high-speed USB transport | A §1.1 p.13; §§3.11,4.10 pp.21,85-88; USB2 Chapters5/9 | **P:** high-speed X4 only, speed established by bus interface. **H:** planner covers FS and HS units. **M:** FS streaming, general legal service intervals/packet sizes, HS high-bandwidth transactions and controller/stack combinations. USB2-defined capability must not be inferred from bcdUSB alone. |
| Asynchronous, adaptive and synchronous sources/sinks | A §3.11 pp.21; §§3.16.2.1-2 pp.40-41; §4.10 pp.85-88 | **P/S limited:** asynchronous OUT with explicit feedback. **M:** adaptive/synchronous paths and other direction/sync combinations, including adaptive sources with required feedback behavior. |
| Explicit feedback, interval scaling, startup, unchanged feedback and clock drift | A §3.16.2.2 pp.40-41; §§4.10.2 pp.87-88; USB2 §5.12.4.2 pp.74-75 | **H/P partial:** FS10.14/HS16.16 decoding and fractional planner; actual HS feedback. Valid ZLP keepalive after an initial value is implemented. **M:** FS real transfer, general endpoints/rates/intervals, locking/drift/jitter/loss/recovery and explicit-feedback source direction. Current +/-12.5% acceptance and 200ms watchdog are experimental policies, not class limits. |
| Implicit-feedback data endpoint usage and valid clock relationships | A §4.10.1.1 Table4-33 pp.85-86; USB2 §5.12.4 | **M:** no implicit-feedback pairing/measurement/scheduling. Do not reuse arbitrary capture data as a clock merely because it exists. |
| Packetization, whole audio slots, fractional rates, MaxPacketsOnly and lock delay | F §§2.1-2.3.1 pp.12-16; A §4.10.1.2 pp.86-87 | **H:** rational packet sizing and capacity/phase tests. **P:** only fixed48k path. **M:** general MaxPacketsOnly padding/semantics, advertised lock-delay units and samples/time handling, general transfer delimiters and dynamic rate changes. |
| Stream start/pause/resume/stop, cancellation, unplug, reset, power and restart | A §§3.16,5.1 pp.38-42,89; target Windows PnP/USB contracts | **H/S/P partial:** bounded ownership/cancel-drain, direct stream callbacks, controlled physical close/reopen. **M:** broad active-unplug, surprise removal, suspend/resume, fault injection, starvation/overflow, long runs and every target OS. A checked teardown is required for each transport, not a USB-class compliance shortcut. |

## All standardized format families

Each family below remains requested. Encoded transport and a device's
encoder/decoder controls are distinct from implementing a host codec.
Passing standardized encoded bytes to capable hardware can be supported
without decoding them in this driver. Conversely, advertising encoded
support to Windows requires a real data/KS contract; relabeling bytes as
PCM is not format support. Codec licensing/specification acquisition must
be recorded where needed and cannot be used to hide a missing standard
transport family.

| Format capability | Exact anchor | Current coverage / required completion |
| --- | --- | --- |
| Type I signed PCM: legal subslots/resolutions, rates and channels | F §§2.3.1.1-2.3.1.7.1 pp.13-16; Table2-2 p.15; A.2.1 p.29 | **H/C partial:** parser admits PCM with 1-4-byte subslots and valid significant bits; planner is format-frame based. **P/S:** stereo16/48k only. **M:** generic PCM transport/OS negotiation, 8/24/32-bit containers/resolutions, arbitrary advertised rate ranges, multichannel/capture. |
| Type I PCM8 unsigned, IEEE_FLOAT, ALAW and MULAW | F §§2.3.1.7.2-4 p.16; A.2.1 p.29 | **M:** format recognition/representation, OS exposure and stream tests for every listed encoding. They must not be treated as signed PCM merely because Type I packing is similar. |
| Type I RAW_DATA | F §2.3.1.7.5 p.17; A.2.1 p.29 | **M:** opaque PCM-slot-like transport and a clearly identified noninterpreting host contract. The standardized raw carrier is not a vendor-specific semantic decoder. |
| Type II MPEG, AC-3, WMA, DTS and RAW_DATA | F §§2.3.2.1-8 pp.17-19; Table2-3 p.18; A.2.2 pp.29-30 | **M:** descriptor negotiation, encoded-frame boundaries, delimiter/MaxPacketsOnly rules, bandwidth/timing, equivalent-PCM-slot feedback, OS encoded/raw contract and tests. Includes all four named codecs and opaque raw streams. |
| Type III IEC61937 AC-3; MPEG1 Layer1; MPEG1 Layer2/3 or MPEG2 NOEXT; MPEG2 EXT; AAC ADTS; MPEG2 Layer1 LS; Layer2/3 LS; DTS-I/II/III; ATRAC; ATRAC2/3; TYPE_III_WMA | F §§2.3.3.1-2 pp.19-20; Table2-4 p.20; A.2.3 TableA-4 p.30 | **M:** every defined bit allocation, burst/transport framing and OS format mapping. Similar two-channel16-bit USB layout does not permit PCM mixing, scaling or conversion of encoded bursts. |
| Type IV external connections, including IEC60958_PCM | F §2.3.4 pp.20-21; Table2-5 p.21; A.2.4 pp.30-31 | **M:** endpoint-free AS representation and format/encoder/decoder control for external connections. Include the Type IV table's PCM, PCM8, float, companding, MPEG/AC-3/WMA, IEC61937 variants and IEC60958_PCM. Type IV is not USB sample transport and its bit map is not identical to Type I/II/III. |
| Extended Type I, II and III; optional packet headers and Type I synchronous control channel | F §2.4.1-3 pp.21-25; Tables2-6/7/8 pp.22-25; A.1 p.29 | **M:** all three extended descriptors, packet header/audio/control-channel combinations, bounds and OS-facing transport. A legal control-only extended Type I alternate must not be discarded solely for having no audio samples. |
| Sideband protocols: undefined/opaque and Hi-Res Presentation TimeStamp | F §2.4.4 pp.25-26; A.3 pp.31-32 | **M:** declared header protocol, timestamp layout and relation to SOF/stream time, exact transport and usable API. Unknown protocol payloads must remain opaque, not guessed. |

DSD/DoP and proprietary encodings are **not named formats in these pinned
F Type I-IV tables**. The standardized RAW_DATA/extension mechanism remains
in scope; implementing a particular opaque format's meaning needs its
own authorized specification and explicit status entry. This distinction
does not remove the named standard encoded formats above from the goal.
UAC3 is a different class revision, not an unimplemented UAC2 format.

## Clock and audio entity graph

| Entity/capability | Exact anchor | Current evidence / missing behavior |
| --- | --- | --- |
| Clock Source: internal/external, clock types, SOF synchronization, association, frequency and validity | A §§3.13.11,4.7.2.1 pp.36-37,49-50; §5.2.5.1 pp.97-98; A.17.1 pp.135-136 | **H/C/P partial:** source discovery, 48k RANGE/CUR/conditional SET/readback and validity on X4 clock33. **M:** read-only/fixed clocks, general ranges/resolutions and rates, zero-Hz attribute treatment, clock-loss/revalidation and asynchronous changes. Current exact frequencyRW/validityRO gate excludes other legal clocks. |
| Clock Selector: multi-input clock switching/current selection | A §4.7.2.2 pp.50-51; §5.2.5.2 p.98; A.17.2 p.136 | **H:** graph traversal/loop rejection and supplied one-based selections. **P:** X4 one-input selector34 only. **M:** multi-input GET/SET, rate/range propagation, dependency/active-stream policy and notifications. |
| Clock Multiplier: numerator/denominator and derived rates | A §4.7.2.3 pp.51-52; §5.2.5.3 pp.98-99; A.17.3 p.136 | **M:** intentionally rejected by current resolver; rational propagation, overflow checks and advertised access handling needed. Never assume ratio1. |
| Input/Output Terminals and associations | A §§3.13.2-3 pp.27-28; §§4.7.2.4-5 pp.52-54; §5.2.5.4 pp.99-101 | **H/C partial:** structural fields and terminal clock link; exact X4 route. **M:** full signal graph, associated-terminal relationships, controls and dynamic clusters. |
| Mixer Units: arbitrary programmable input/output matrix and channel clusters | A §3.13.4 p.29; §4.7.2.6 pp.55-57; §5.2.5.5 pp.101-102; A.17.5 p.137 | **H/C partial:** [Linux-adapted portable `uacmix.c`](../testing/2026-10-21-uac2-mixer.md) validates the MSB-first programmable matrix, fixed-node readability, zero-based MCN0..255 and scalar CUR/RANGE request setup with caller-resolved input counts. **M functional:** topology traversal/cluster resolution, USB execution, mixer-specific RANGE limits, cluster/status/latency controls, Windows topology and actual device tests. |
| Selector Units: signal source selection | A §3.13.5 p.29; §4.7.2.7 pp.57-58; §5.2.5.6 p.102; A.17.6 p.137 | **M functional:** graph selection, one-based source controls, latency, topology/OS routing and reconfiguration. This is distinct from a Clock Selector. |
| Feature Units | A §3.13.6 p.30; §4.7.2.8 pp.58-59; §5.2.5.7 pp.102-108; A.17.7 pp.137-138 | **H:** structural minimum/bitmap tests; `uacvol.c` walks from the playback USB streaming terminal to an Output Terminal through Feature, Mixer, Selector, Effect, Processing, Extension and SRC units and picks the unit for master or per-channel volume and mute (`voltest.c`, the recorded X4 and built graphs). **S:** playback volume and mute on 98 SE, XP x86 and XP x64 ([volume record](../testing/2026-10-28-uac2-volume.md)). **M:** capture-side units, every other Feature Unit control below, the units' other effects on the path. |
| Sampling Rate Converter Units across clock domains | A §3.13.7 pp.30-31; §4.7.2.9 p.60 | **M:** source/clock graph, input/output-domain relationships and OS stream/rate policy. This entity is device-side conversion; a host resampler does not substitute for discovering it. |
| Effect Units: parametric EQ section, reverberation, modulation delay, dynamic range compressor | A §3.13.8 pp.31-34; §4.7.2.10 pp.61-64; §5.2.5.8 pp.108-115; A.17.8 pp.138-140 | **M:** descriptor-specific controls, per-channel access/ranges, device processing selection and OS exposure for each standardized effect. |
| Processing Units: up/down-mix, Dolby Prologic, stereo extender | A §3.13.9 pp.34-35; §4.7.2.11 pp.65-71; §5.2.5.9 pp.115-116; A.17.9 pp.140-141 | **M:** mode/cluster mappings, processing enable/parameters, graph effects and OS topology for every standard process type. |
| Extension Units: standardized wrapper/common controls and vendor-defined function | A §3.13.10 pp.35-36; §4.7.2.12 pp.72-73; §5.2.5.10 p.116; A.17.10 p.141 | **M:** common enable/cluster/status/latency behavior and safe opaque/vendor control exposure. Unknown vendor-specific processing semantics are not standardized audio features; they require documented per-vendor support rather than invented interpretation. |
| Encoders/decoders associated with AS alternates | A §3.14 pp.37-38; §§4.9.4-5 pp.77-84; §§5.2.6.2-3 pp.118-126 | **M:** all standard encoder and MPEG/AC-3/WMA/DTS decoder descriptors, capabilities and controls; tied to Type II/III/IV requirements above, not deferred as proprietary protocols. |

## Complete control-family inventory

All rows require advertised access checking, correct channel/entity/interface
or endpoint addressing, CUR/RANGE widths, invalid/unsupported responses,
signed values and update notifications where applicable. Standard latency
controls remain required even when only their selector table names them;
the common definition is A §5.2.4.8 p.97. No OS control may claim to operate
hardware merely because a software property handler returns success.

| Controls to implement | Specification anchor | Current status |
| --- | --- | --- |
| Common enable, mode select, dynamic cluster, underflow, overflow, encoder error, decoder error, latency | A §5.2.4 pp.94-97 | **M general.** Clock feedback/error counters are not substitutes for standardized controls. |
| Terminal copy-protection level, connector insertion/cluster information, overload, cluster, underflow, overflow, latency, phantom power | A §3.15 p.38; §5.2.5.4 pp.99-101; A.17.4 p.136 | **M.** Phantom-power control in the current ECN is included; do not issue it without supported access and an explicit user/OS operation. |
| Mixer matrix, cluster, underflow, overflow, latency; selector source and latency | A §§5.2.5.5-6 pp.101-102; A.17.5-6 p.137 | **M.** |
| Feature master/per-channel mute and volume, including silence sentinel and ranges | A §§5.2.5.7.1-2 pp.102-103; A.17.7 p.137 | **H/S playback (0.1.4.0):** RANGE per controlled channel (multi-subrange, a RES of 0 read as 1, the silence MIN read as its next step), CUR with the silence value kept, SET_CUR only on read/write controls, master or per-channel, the KS VOLUMELEVEL/MUTE nodes with BASICSUPPORT; a software gain where no writable control exists; simulated devices on 98 SE (xHCI98), XP x86 and XP x64 (Windows' EHCI) ([volume record](../testing/2026-10-28-uac2-volume.md)). **P diagnostic GET_CUR only** on the real X4. **M:** capture gain, change notifications (no interrupt endpoint), a real device, Vista and later. |
| Feature bass, mid, treble, graphic EQ bands | A §§5.2.5.7.3-6 pp.103-105 | **M**, including variable band maps/values and advertised ranges. |
| Feature AGC, delay, bass boost, loudness, input gain, input-gain pad, phase inverter | A §§5.2.5.7.7-13 pp.106-108 | **M**, including correct units and channel applicability. |
| Feature underflow, overflow, latency and high-pass filter | A §§5.2.5.7.14-16 p.108; A.17.7 p.138; common latency p.97 | **M.** High-pass ECN must not be omitted by following only the 2006 selector set. |
| Parametric EQ: enable, center frequency, Q factor, gain, underflow, overflow, latency | A §5.2.5.8.1 pp.108-110; A.17.8.1 p.138 | **M.** |
| Reverb: enable, type, level, time, feedback, pre-delay, density, HF rolloff, underflow, overflow, latency | A §5.2.5.8.2 pp.110-112; A.17.8.2 pp.138-139 | **M.** |
| Modulation delay: enable, balance, rate, depth, time, feedback, underflow, overflow, latency | A §5.2.5.8.3 pp.112-113; A.17.8.3 p.139 | **M.** |
| Dynamic range compressor: enable, compression rate, max amplitude, threshold, attack/release time, underflow, overflow, latency | A §5.2.5.8.4 pp.113-115; A.17.8.4 pp.139-140 | **M.** |
| Up/down-mix and Dolby Prologic: enable, mode, cluster, underflow, overflow, latency; stereo extender: enable, width, underflow, overflow, latency | A §5.2.5.9 pp.115-116; A.17.9 pp.140-141 | **M.** |
| Extension common enable, cluster, underflow, overflow, latency | A §5.2.5.10 p.116; A.17.10 p.141 | **M.** Vendor-private controls need separate descriptors/docs and cannot be assumed to share standard selectors. |
| AS active alternate, valid alternates and audio-data-format selection | A §5.2.6.1 pp.117-118; A.17.11 pp.141-142 | **M class controls.** Standard SET_INTERFACE on exact X4 alternates does not implement these optional class control selectors. |
| Encoder bit rate, quality, VBR, type, underflow/overflow, encoder error and parameters1-8 | A §5.2.6.2 pp.118-120; A.17.12 p.142 | **M.** Encoder-specific parameter meanings need their authorized format definitions. |
| MPEG decoder dual channel, second stereo, multilingual, dynamic range, scaling, high/low scaling and errors; AC-3 mode/dynamic range/scaling/high-low scaling and errors | A §§5.2.6.3.1-2 pp.120-126; A.17.13.1-2 p.143 | **M** for all listed controls, including underflow/overflow/decoder error. |
| WMA and DTS decoder underflow, overflow and decoder error | A §§5.2.6.3.3-4 p.126; A.17.13.3-4 p.144 | **M.** |
| Endpoint pitch, data overrun and data underrun | A §5.2.6.4 pp.126-127; A.17.14 p.144 | **M.** Transport packet-error counters are not endpoint class-control support. |

## Requests, events, terminal types and OS exposure

| Capability | Specification/contract anchor | Evidence and missing work |
| --- | --- | --- |
| Standard descriptor/configuration/interface requests and zero-bandwidth behavior | A §5.1 p.89; §3.16.2 p.40 | **C/P limited:** exact X4 configuration/alt selection. **M:** general runtime negotiation and safe recovery for all supported stream families. |
| Generic CUR/RANGE request/parameter-block engine, variable ranges, access bitmaps and short-prefix reads | A §§5.2.1-3 pp.89-94; A.14 pp.134-135 | **H/C partial:** separate `uacctl.c` portable access-lane/setup/scalar RANGE core now covers an explicit subset of clock, selector, feature, PE effect, encoder and interface/endpoint policies, with guarded signed/unsigned1/2/4-byte tests; see [scope and exact policy table](../testing/2026-10-21-uac2-controls.md). **P:** existing X4 clock frequency/validity and diagnostic mute/volume only. **S:** the driver builds its Feature Unit requests with `uacctl.c` (volume and mute CUR/RANGE, `UacUsbClassRequest`). **M:** transport/OS integration of the rest of the core, remaining control-specific RANGE semantics and variable layouts, matrix addressing, all other advertised control policies and general response/recovery handling. |
| Memory requests and memory-change events | A §5.2.7.1 pp.127-128, Table5-19 p.128 | **M:** standardized request wrapper, sizes/ranges/ownership and safe OS exposure; memory contents may be entity/vendor-defined and are not implied to be universally decodable. |
| AC interrupt endpoint and six-byte interrupt messages | A §4.8.2 p.74; §§6.1-2 pp.129-130 | **M:** interrupt transport, originator/interface/endpoint addressing, control/memory change dispatch, cache refresh and client notification. Polling a few CUR values is not an event implementation. |
| Physical buttons and host/device control binding | A §3.16.4 pp.41-42; Chapter6 pp.129-130 | **M:** integrated audio-control changes and OS notifications; separate HID buttons require the relevant HID contract and binding. HID reports are not automatically UAC2 controls. |
| All standard terminal families | T §§2.1-2.7 pp.8-12 | **C partial:** numeric terminal fields exist. **M:** complete semantic/OS mapping for USB (including standardized vendor-specific terminal wrapper), input/microphone arrays, output/speakers, bidirectional/headsets, telephony, external analog/digital/optical/SPDIF/ADAT/TDIF/MADI and embedded-function terminals. Codes and associations must remain intact even where OS UI groups them. |
| Windows stream formats, topology, mixer controls, channel maps, positions and notifications | A topology/format semantics above; target PortCls/KS/WinMM contracts | **P/S:** one WaveCyclic playback endpoint at stereo16/48k and real buffer completion. **S:** a master volume and mute on the speaker line (WDMAud "Master Volume"/"Master Mute"; Volume Control, tray and Sounds and Audio Devices on 98 SE and XP). **M:** capture, all advertised standard formats, other per-channel/matrix/effect/clock controls, event properties (KSEVENT_CONTROL_CHANGE), timing/latency and opaque/encoded APIs that preserve semantics. |
| Target systems/builds | Repository target scheme: 98SE/ME/2000/XP/Vista/7/8; x86 and x64 where applicable | **Build-only:** x86/x64 free/checked gates; **P/S:** recorded Win7 x86 path. Earlier XP enumeration/plumbing evidence is narrower than streaming. **M:** runtime qualification per OS/architecture/USB stack, legacy import/PnP/PortCls behavior and complete APIs for every implemented feature. No cell becomes Tested from compilation alone. |

## Completion evidence required

Every missing row needs original/authorized implementation, malformed and
legal descriptor/control tests, and positive end-to-end evidence of its
actual operation. Simulators can cover optional families for which hardware
is unavailable, but their protocols/fixtures must be independently checked
against these specifications; the same mistaken parser and model agreeing
is insufficient. Record packet/URB errors, data integrity, timing, status
changes, cancellation and client close behavior. Keep synthetic/physical,
OS/architecture, speed/sync, format/channel/rate and exact binary identity
separate. The nine current physical packet errors and setup-path timeout
remain open quality work, not waived prerequisites.

This ledger changes documentation only. It neither implements these missing
families nor claims that an optional device feature exists on the X4.

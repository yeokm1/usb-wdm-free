# UVC 1.0 through 1.5 feature coverage ledger

Snapshot: 2026-10-21. The owner's scope is **all standardized UVC capabilities
through 1.5**, including encoding, extension-unit access, still capture, media
transport and render/output. Earlier draft exclusions are superseded. This is
a coverage/work ledger, not a conformance declaration or a promise that every
camera implements every optional feature. Each device's descriptors and
request capabilities must govern what the driver exposes.

There is currently **no project USB webcam streaming driver**. Portable
parsers, a simulated USB camera, CAMCHECK, and a synthetic STREAM.SYS metadata
experiment cover different parts of the problem. The real Brio native-driver
run does not turn their combination into a project-driver hardware pass.

## Reference keys and remaining acquisition

The original official archives, hashes and selected PDF identities are in
[the reference record](../testing/2026-10-21-uvc-spec.md). Sources below were
read from those downloaded archives, including their additional payload PDFs;
they are not implementation sources from a driver kit. The references remain
ignored/internal-use files. This ledger is original analysis, without
redistributing the specifications.

| Key | Official document / archive | Page convention used below |
|---|---|---|
| C11 | `USB_Video_Class_1.1.pdf`, [official 1.1 archive](https://www.usb.org/sites/default/files/USB_Video_Class_1_1_090711.zip), June 1, 2005 | Printed page p is PDF page p+13 |
| C15 | `UVC 1.5 Class specification.pdf`, [official 1.5 archive](https://www.usb.org/sites/default/files/USB_Video_Class_1_5.zip), August 9, 2012 | Printed p is PDF p+15 |
| U11 / U15 | `USB_Video_Payload_Uncompressed_1.1.pdf` / `_1.5.pdf` in the corresponding archive | U11 PDF=p+7; U15 PDF=p+6 |
| J11 / J15 | MJPEG payload PDF in the corresponding archive | PDF=p+7 |
| F11 / F15 | Frame Based payload PDF in the corresponding archive | PDF=p+7 |
| S11 / S15 | Stream Based payload PDF in the corresponding archive | S11 PDF=p+7; S15 PDF=p+6 |
| D11 / D15 | DV payload PDF in the corresponding archive; 1.5 filename ends `_DV_15.pdf` | PDF=p+7 |
| T11 / T15 | MPEG-2 TS payload PDF in the corresponding archive | PDF=p+7 |
| H10 | `USB_Video_Payload_H 264_1 0.pdf`, revision 1.00 April 26, 2011, in the 1.1 archive, plus `Video_Payload_H264_09Aug2011_errata.pdf` | Main PDF=p+9 |
| H15 | `USB_Video_Payload_H264_1.5.pdf`, in the 1.5 archive | PDF=p+8 |
| V15 | `USB_Video_Payload_VP8_1.5.pdf`, in the 1.5 archive | PDF=p+7 |
| M11 / M15 | `USB_Video_Transport_1.1.pdf` / `_1.5.pdf`: **Media Transport Terminal**, not USB packet transport | PDF=p+7 |

All section/page citations below use **printed pages**, with the conversion
above providing exact PDF anchors. Payload specification revisions are not
interchangeable with the device's core `bcdUVC` revision.

**UVC 1.0 / 1.0a normative gap:** the original 1.0 and 1.0a core/payload
documents are not in the acquired archives. C11 revision history, pp.iv–v,
records them and changes from them, but is not a complete replacement. The
26-byte 1.0 codec, pinned Linux corroboration and physical Brio 1.0 descriptors
provide useful evidence, not a complete 1.0 conformance audit. Acquire the
original official 1.0/1.0a references before closing their ledger. In
particular, review formerly defined descriptor/format values made reserved
in 1.1; do not silently treat the 1.1 reserved list as a complete 1.0 inventory.

A bounded acquisition follow-up checked the current
[USB-IF document library](https://www.usb.org/documents?items_per_page=50&order=title&search=USB+Class&sort=asc)
and probed eight plausible legacy 1.0/1.0a/1.0c ZIP URLs under the official
`sites/default/files` and `developers/devclass_docs` paths. All those URL
probes returned HTTP403; that is inconclusive, not evidence that the files
do not exist. The ignored `docs/references/uvc10-url-probes.json` preserves
the exact attempts. No original 1.0 core was acquired. The library's separate
Audio/Video Device Class1.0 archive is a different class/specification and is
not a substitute for UVC1.0.

## Status vocabulary and evidence boundaries

- **Portable subset:** actual original C code exists, with bounded host tests;
  no Windows USB transport or property implementation follows from that.
- **Model only:** the explicitly simulated FFFF:0002 USB device implements a
  narrow behavior. Native Windows consuming it tests the model/reference path.
- **Native reference:** Windows' in-box driver and CAMCHECK were exercised;
  the project's USB kernel driver was not.
- **Missing:** required implementation/integration/validation is absent.
  A skipped descriptor is not feature support.

Current evidence: [descriptor/payload audit](../testing/2026-10-21-uvc-design-audit.md),
[M0 miniport and unwired worker limits](../testing/2026-10-21-uvc-miniport.md),
[buffer contract](../testing/2026-10-21-uvc-stream-buffer-contract.md), and
[real Brio native reference](../testing/2026-10-21-brio100-native.md).

`uvcctrl.c` encodes/decodes exactly 26/34/48 bytes for revisions
0100/0110/0150. It also has typed access to the 14 extra 1.5 bytes. This is
wire serialization, **not full 1.5 negotiation or encoder support**.
`uvcdesc.c` exposes YUY2/NV12/I420/M420/MJPEG frames and FS/HS endpoint
capacities; [uncompressed layout tests](../testing/2026-10-21-uvc-layouts.md)
cover tight wire rows and exact uncompressed frame sizes.
`uvcpay.c` consumes already complete UVC payloads and assembles bounded
frames. There is no project USB request engine connecting these cores.

## Revisions, topology and descriptors

| Capability | Reference | Current status / remaining gate |
|---|---|---|
| Core 1.0/1.0a behavior | C11 revision history; original references still needed | 0100 wire subset and actual Brio descriptor fixture; full normative review missing |
| Core 1.1 behavior | C11 §§2–4 | Portable subset; class control, transport and OS integration missing |
| Core 1.5 behavior and protocol code 01 | C15 §§2.2, 3.7.1, Appendix A.3, pp.5,46,156 | Codec recognizes 0150; protocol/version capability policy missing |
| Multiple revision configurations and fallback | C15 §3.4.1 p.45 | Missing configuration discovery/selection; parser takes one caller-selected config |
| Device/configuration/IAD/VC collection boundaries | C15 §§3.1–3.7 pp.44–49; C11 §§3.1–3.7 pp.46–51 | Portable bounds, VC anchor, `baInterfaceNr` and containing IAD checks; not complete standard-device validation |
| Input/output headers and per-format control capabilities | C15 §§3.9.2.1–2 pp.61–65 | Input capture subset; render/output headers and full control bitmap semantics missing |
| Input/output terminals, source links, associations and entity graph | C15 §§2.3,3.7.2.1–2 pp.6–9,49–51 | Complete graph validation, duplicate IDs, cycles, routes and terminal capability exposure missing |
| Camera, selector, processing, encoding and extension units | C15 §§3.7.2.3–7 pp.51–59 | Full variable-length descriptor/control bitmap models missing; no property implementation |
| Terminal types: USB streaming/vendor, camera/media input, display/media output, external composite/S-video/component/vendor | C15 Appendix B pp.162–163 | Parsed streaming terminal references are not implementation of these terminal types; media/output routes missing |
| Discrete and continuous frame intervals/defaults | C15 §3.9.2.4 p.66; U15 §3.1.2 pp.5–7 | Portable YUY2/MJPEG validation/selection; live interval changes missing |
| Color primaries, transfer characteristics and matrix coefficients | C15 §3.9.2.6 p.68 | Color matching descriptor and propagation to consumers missing |
| Still descriptors, image patterns and compression patterns | C15 §3.9.2.5 pp.66–68 | Parser retains a stream's still-method value only; actual still descriptors/negotiation missing |
| Full/high/SuperSpeed endpoint descriptors and companion limits | C15 §§3.10.1–2 pp.69–71; applicable USB 2/3 Chapter 9 | FS/HS portable endpoint capacity subset; SuperSpeed companions/burst/bytes-per-interval and Windows host-stack integration missing |
| Optional/unknown entities, formats and extensions | C15 Appendix C p.164 | Bounded skipping exists; generic bounded discovery/exposure and matching format handlers still required |
| Function/device strings and language handling | C15 §§3.6,3.11 pp.45–46,72 | Missing project enumeration/user-facing identity policy; diagnostic captures intentionally omit strings |

## Payload formats and consumer contracts

The UVC design is format-agnostic: C15 Appendix C p.164 explicitly leaves
new/proprietary payload definitions outside the core. Thus “all through 1.5”
means every standardized family below, with extensible identification and
explicit unsupported-format behavior. It cannot honestly mean decoding every
unknown future/vendor codec without its payload specification and consumer.

| Standardized family/capability | Reference | Current status / remaining gate |
|---|---|---|
| YUY2 uncompressed, packed macropixel boundaries | U11 §§2.2–2.4 pp.2–3, §3.3.2 p.8; U15 §§2.2–2.4 pp.1–3 | Portable format/frame subset and exact-byte frame assembly; synthetic bulk model; real native Brio YUY2 reference only |
| NV12 planar 4:2:0 | U11 §2.2 p.2; U15 §§2.2–2.3 pp.1–2, §3.3.1 p.8 | Portable full-GUID/bpp/frame validation, Y/UV row geometry and bounded row lookup tested; transport/consumer integration missing |
| M420 packed 4:2:0 | U15 §§2.2–2.3 pp.1–2, §3.3.2 p.8 | Portable full-GUID/bpp/frame validation and two-Y/one-UV row groups tested; packed payload packetization, transport/consumer integration missing |
| I420 planar 4:2:0 | U15 §§2.2–2.3 pp.1–2, §3.3.1 p.8 | Portable full-GUID/bpp/frame validation and separate Y/U/V geometry tested; transport/consumer integration missing |
| Other YUV GUIDs with defined payload semantics | U15 §2.3 p.2 | Missing extensible format/consumer validation; do not infer geometry from a GUID alone |
| MJPEG frame descriptors, sizes, intervals and compressed sample boundaries | J15 §§2.2,3.1–3.3 pp.2–9; J11 corresponding sections | Portable descriptor subset and variable-size assembler; no JPEG parsing/decoder/OS compressed pin/real project streaming |
| Generic frame-based payload, GUID, bits-per-pixel, variable-size and bytes-per-line semantics | F11/F15 §§2.1,3.1–3.2 pp.2–8 | Missing descriptor/frame handler and consumer mapping |
| Generic stream-based payload, byte-oriented and packet-oriented units | S11 §§2.1–3.2 pp.2–4; S15 §§2.1–3.2 pp.2–5 | Missing; frame-only assembler must not be applied blindly to a byte stream |
| DV: SD/SDL 525/625 systems, HD 1125/1250 systems, source blocks and timing | D11/D15 §§2,3.1–3.2.7 pp.3–12 | Missing format/descriptors/source-block/time handling and consumers |
| MPEG-2 transport stream: packet length, stride, prefix/APT and application-specific stride data | T11/T15 §§2.2–2.4,3.1–3.2 pp.2–6 | Missing; transport clock/application packet timing is separate from common UVC timestamps |
| Legacy USB-IF H.264 payload v1.00, elementary and multiplexed payloads, SVC and XU negotiation | H10 §§2–3.6 pp.5–42, August 2011 errata | Missing; separate extension-unit profile, not automatically implemented by 1.5 encoding units |
| UVC 1.5 H.264 descriptor/frame family and simulcast subtype | H15 §§2–3.2 pp.4–22; C15 Appendix A.6 pp.157–158 | Descriptor family skipped by the parser (counted, not refused; `desctest.c` `Uvc15`); access-unit/header semantics and compressed consumer missing. Post-release, after VP8 and a licensing/compiler review (uvc.md milestone 6) |
| H.264 SVC layers/sub-bitstreams, throughput, runtime configuration | H15 §3.3 pp.23–29 | Missing |
| H.264 MVC/views and simulcast combinations | H15 §3.4 pp.30–32 | Missing |
| VP8 descriptor/frame family and simulcast subtype | V15 §§2.1–2.3,3.1–3.2 pp.2–6,8–13 | Descriptor family skipped by the parser (counted, not refused); header/payload/consumer missing. Post-release, first compressed format: frame delivery, then a libvpx decoder in `uvcdec` if it builds for 98 SE (uvc.md milestone 6) |
| VP8 temporal layers, reference pictures and error resilience | V15 §§2.4–2.5,3.3 pp.6–7,14–22 | Missing |
| Interlace, field order, aspect ratio, copy-protection and format flags as applicable | C15 §3.9.2.3 p.65 and each payload's format descriptor section | Full propagation and validation missing; no silent conversion to progressive square-pixel YUY2 |
| Capture and render/output directions for applicable payloads | C15 §§3.9.2.1–2,4.3.1.1 pp.61–65,133–149; payload transfer sections | Capture-only portable subset; project capture transport and render both missing |

## Common USB transport, synchronization and still capture

| Capability | Reference | Current status / remaining gate |
|---|---|---|
| EP0 standard/class request engine, timeout/cancel/remove lifecycle | C15 §§2.4.4,4.1 pp.35–42,73–76 | Missing project USB engine; native/model behavior is separate |
| PROBE/COMMIT successful negotiation, returned values, no silent payload clamp | C11 §4.3.1.1 pp.102–113; C15 §4.3.1.1 pp.133–149 | Wire codec and bounded capture selection only; model implements fixed IN negotiation; project live negotiation missing |
| Hints, format/frame/interval, key/P-frame rate, compression quality/window, delay, frame/payload limits | C15 Table 4-75 pp.134–139 | Codec fields exist; semantic validation and live request policy missing |
| Clock frequency, framing information, preferred/min/max payload versions (1.1 extension) | C11 Table 4-47 pp.103–110; C15 Table 4-75 pp.139–140 | Codec fields exist; version/clock/framing policy missing |
| Usage, luma bit depth, settings, reference count, per-layer rate modes and stream layout (1.5 extension) | C15 Table 4-75 pp.140–143 | Typed wire access exists; encoder/throughput/layout negotiation missing |
| Isochronous IN bandwidth/alternate selection and submission | C15 §§2.4.3.1,3.10.1.1 pp.19,69–70 | Portable smallest-sufficient alternate capacity selection; project URBs, scheduling and errors missing |
| Isochronous OUT/video render and underrun policy | C15 §§2.4.3.2,3.10.1.1 pp.20–28,69–70 | Missing |
| Bulk IN/OUT, payload boundaries across URBs, short packet/ZLP/maximum-payload rules | C15 §2.4.3.2.1 pp.23–25, §3.10.1.2 pp.70–71 | Synthetic FS bulk IN model only; project bulk framing/reassembly and OUT missing |
| Common header length/EOH/FID/EOF/PTS/SCR/STI/ERR | C11 §2.4.3.3 pp.31–33; C15 §2.4.3.3 pp.29–31 | Portable common-header parser; format-specific headers and transport association missing |
| Initial partial frames, FID transitions, EOF once, loss/ERR/overflow resynchronization | Same sections, plus payload-specific sample rules | Portable bounded assembler host-tested; not a proof of real camera/USB loss recovery |
| PTS/SCR, 11-bit SOF, wrap, clock recovery, latency and host presentation timestamps | C15 §§2.4.3.4–4.3 pp.32–34 | Fields decoded only; synchronized clocks/KS timestamps/rate matching missing |
| Multiple simultaneous streams, cross-stream synchronization and bandwidth arbitration | C15 §§2.4.1,2.4.3.4 pp.13,32–34; H15 §3.3 | Portable model can list bounded multiple streams; runtime arbitration/sync missing |
| Dynamic frame interval changes | C15 §2.4.3.5 p.34 | Missing runtime negotiation and consumer reconfiguration |
| Device-initiated dynamic format changes | C15 §2.4.3.6 p.34 | Missing status/control/KS consumer handshake |
| Still capture method 1, video-stream sample | C15 §2.4.2.4 pp.17–18 | STI is recognized by assembler; trigger/consumer/still-frame behavior missing |
| Still capture method 2, shared streaming pipe with still PROBE/COMMIT | C15 §§2.4.2.4,4.3.1.2 pp.17–18,150–152 | Missing |
| Still capture method 3, separate bulk still-image endpoint | C15 §§2.4.2.4,3.10.2 pp.17–18,71 | Missing |
| Hardware still triggers, software trigger state, button events and still completion | C15 §§2.4.2.3–4,4.3.1.4 pp.17–18,152–153 | Missing |
| Status interrupt endpoint, VC control value/info/failure changes, VS button/error events | C15 §2.4.2.2 pp.14–17, §3.8.2 pp.59–60 | Missing project status reader/event delivery |
| Device power modes, suspend/resume, stop/reconfigure, surprise removal and cancellation | C15 §§2.4.4,4.2.1.1 pp.35–42,77–78 plus OS contracts | Missing integrated USB/stream/property rundown; unwired worker must not be treated as proof |

## Control request framework and VideoControl interface

Every supported control needs descriptor-based discovery, correct request
length/signedness/units, GET/SET capability checks, legal ranges/steps,
default/current state, auto/manual dependencies, request-error propagation,
status updates and an OS/application representation. None of the control rows
below is currently implemented by the project USB driver.

| Capability | Reference | Status |
|---|---|---|
| SET_CUR; GET_CUR/MIN/MAX/RES/LEN/INFO/DEF, where the individual control permits them | C15 §§4.1–4.2.2 pp.73–81; Appendix A.8 p.158 | Missing; no assumption that every request is legal for every selector |
| 1.5 SET_CUR_ALL and GET_CUR_ALL/MIN_ALL/MAX_ALL/RES_ALL/DEF_ALL batches | C15 §4.2.2 Table 4-8 pp.80–81; Appendix A.8 p.158 | Missing; bitmap-ordered aggregate lengths, `wValue=0`, and per-control contributions must be modeled |
| GET_INFO auto-disabled, auto-update, asynchronous and commit-incompatible state | C15 §4.1.2 p.75; C11 §4.1.2 p.75 | Missing; C15 adds commit-disabled D5, not present in C11 |
| Range resolution/integer step rules and absolute/relative relationships | C15 §4.2.2 p.81 and per-control sections | Missing |
| VC power-mode control | C15 §4.2.1.1 pp.77–78 | Missing |
| VC request-error code and error lifetime | C15 §4.2.1.2 pp.79–80 | Missing |
| Selector-unit input choice and topology route update | C15 §4.2.2.2 p.94 | Missing |
| Extension-unit GUID, supported selectors, lengths, GET/SET and change notifications | C15 §§3.7.2.7,4.2.2.5 pp.58–59,131 | Missing generic bounded exposure; unknown vendor selector semantics cannot be invented |

### Camera Terminal controls

All rows are **missing** as project USB requests/properties/tests. The three
explicit 1.5 additions are identified; remaining rows also need their
version/device applicability established from descriptors. The selector order
is not the same as the explanatory chapter order: C15 Appendix A.9.4,
pp.159–160, supplies selectors; the exact behavioral anchors follow.

| Control capability | C15 section | Printed page |
|---|---|---:|
| Scanning/interlace mode | 4.2.2.1.1 | 81 |
| Auto-exposure mode | 4.2.2.1.2 | 82 |
| Auto-exposure priority | 4.2.2.1.3 | 82 |
| Exposure time, absolute | 4.2.2.1.4 | 83 |
| Exposure time, relative | 4.2.2.1.5 | 83 |
| Focus, absolute | 4.2.2.1.6 | 84 |
| Focus, relative | 4.2.2.1.7 | 84–85 |
| Focus simple-range selection, added in 1.5 | 4.2.2.1.8 | 85 |
| Automatic focus | 4.2.2.1.9 | 86 |
| Iris, absolute | 4.2.2.1.10 | 86 |
| Iris, relative | 4.2.2.1.11 | 87 |
| Zoom, absolute | 4.2.2.1.12 | 87 |
| Zoom, relative | 4.2.2.1.13 | 88 |
| Pan/tilt, absolute | 4.2.2.1.14 | 89 |
| Pan/tilt, relative | 4.2.2.1.15 | 89–90 |
| Roll, absolute | 4.2.2.1.16 | 91 |
| Roll, relative | 4.2.2.1.17 | 91 |
| Privacy state | 4.2.2.1.18 | 92 |
| Digital window, added in 1.5 | 4.2.2.1.19 | 92–93 |
| Digital region of interest and associated auto-control bitmap, added in 1.5 | 4.2.2.1.20 | 93–94 |

Optical/digital zoom relationships and constraints additionally require C15
Appendix D pp.165–169. These are not satisfied by relaying a numeric zoom value.

### Processing Unit controls

All **missing** as project requests/properties/tests. C15 Appendix A.9.5 p.160
defines selectors, with these behavioral sections:

| Control capability | C15 section | Printed page |
|---|---|---:|
| Backlight compensation | 4.2.2.3.1 | 96 |
| Brightness | 4.2.2.3.2 | 96 |
| Contrast | 4.2.2.3.3 | 96 |
| Automatic contrast, added in 1.5 | 4.2.2.3.4 | 97 |
| Gain | 4.2.2.3.5 | 97 |
| Power-line frequency | 4.2.2.3.6 | 98 |
| Hue | 4.2.2.3.7 | 98 |
| Automatic hue | 4.2.2.3.8 | 98 |
| Saturation | 4.2.2.3.9 | 99 |
| Sharpness | 4.2.2.3.10 | 99 |
| Gamma | 4.2.2.3.11 | 100 |
| White balance, temperature | 4.2.2.3.12 | 100 |
| Automatic white-balance temperature | 4.2.2.3.13 | 100 |
| White balance, component pair | 4.2.2.3.14 | 101 |
| Automatic white-balance components | 4.2.2.3.15 | 102 |
| Digital multiplier | 4.2.2.3.16 | 102 |
| Digital multiplier limit | 4.2.2.3.17 | 103 |
| Analog video standard | 4.2.2.3.18 | 103 |
| Analog video lock state | 4.2.2.3.19 | 103 |

### UVC 1.5 Encoding Unit controls

All **missing** as project entity/descriptor/control/consumer integration.
The 1.5 probe tail is not an Encoding Unit implementation. The operating model
itself needs C15 §4.2.2.4.1 pp.104–112: static/dynamic controls, layer selection,
commit interactions, per-layer parameters and status semantics. Each of the
twenty standardized controls has a separate coverage obligation:

| Control capability | C15 section | Printed page |
|---|---|---:|
| Selected layer | 4.2.2.4.2 | 112–114 |
| Video resolution | 4.2.2.4.3 | 114 |
| Encoder profile/toolset | 4.2.2.4.4 | 114–115 |
| Minimum frame interval | 4.2.2.4.5 | 115 |
| Slice mode | 4.2.2.4.6 | 116 |
| Rate-control mode | 4.2.2.4.7 | 116–120 |
| Average bitrate | 4.2.2.4.8 | 120 |
| Coded-picture-buffer size | 4.2.2.4.9 | 120–121 |
| Peak bitrate | 4.2.2.4.10 | 121 |
| Quantization parameters | 4.2.2.4.11 | 121–122 |
| Quantization parameter range | 4.2.2.4.12 | 122–123 |
| Synchronization and reference-frame requests | 4.2.2.4.13 | 123–125 |
| Long-term buffer | 4.2.2.4.14 | 125–126 |
| Long-term reference picture | 4.2.2.4.15 | 126–127 |
| Long-term reference validation | 4.2.2.4.16 | 127 |
| Supplemental enhancement information bitmap | 4.2.2.4.17 | 127–129 |
| Priority | 4.2.2.4.18 | 129 |
| Layer start/stop | 4.2.2.4.19 | 129–130 |
| Level IDC limit | 4.2.2.4.20 | 130 |
| Error resilience | 4.2.2.4.21 | 130–131 |

Payload-specific meaning and allowed combinations additionally require H15
§3.1.3 pp.17–21 and V15 §3.3 pp.14–22. Codec-specific error-resilience flags,
SEI types, SVC/MVC dependencies and simulcast budgets must not be treated as
opaque successful writes with no validation.

### VideoStreaming selectors and media-transport supplement

| Capability | Exact reference | Current status |
|---|---|---|
| VS PROBE / COMMIT | C15 §4.3.1.1 pp.133–149 | Wire/selection subset and fixed model only; project USB missing |
| VS still PROBE / still COMMIT | C15 §4.3.1.2 pp.150–152 | Missing |
| VS synchronization delay | C15 §4.3.1.3 p.152 | Missing |
| VS still trigger | C15 §4.3.1.4 pp.152–153 | Missing |
| VS key-frame generation | C15 §4.3.1.5 p.153 | Missing |
| VS frame-segment update | C15 §4.3.1.6 pp.153–154 | Missing |
| VS stream-error code | C15 §4.3.1.7 pp.154–155 | Missing |
| Media input/output terminal descriptors and transport controls | M11/M15 §§3–4 pp.4–18 | Missing |
| Media transport play/step/forward/reverse/pause, speed families | M15 §4.1.3.1.1 pp.12–14 | Missing |
| Media stop/fast-forward/rewind/high-speed rewind | M15 §4.1.3.1.2 p.14 | Missing |
| Media recording and recording pause | M15 §4.1.3.1.3 pp.14–15 | Missing; not exercised by preview/reference runs |
| Media eject and status-only states/failure notifications | M15 §§4.1.3.1.4–5 p.15; mode catalog pp.10–12 | Missing |
| Absolute track number and set-position behavior | M15 §4.1.3.2 p.16, Appendix B pp.20–23 | Missing |
| Media information/type and time-code information | M15 §§4.1.3.3–4 pp.16–18 | Missing |

H10 additionally standardizes the legacy H.264 XU configuration PROBE/COMMIT,
rate-control, temporal/spatial/SNR scale, long-term buffer/picture controls,
picture type, version, reset, frame-rate/advanced configuration, per-layer
bitrate and quantization-step controls (§§3.3.1–15 pp.12–36), plus multiplexing
and SEI timing (§§3.4–3.6 pp.37–42). All are **missing**; their v1.00 XU wire
profile must not be silently aliased to C15 Encoding Unit selectors.

## OS exposure and evidence needed to close rows

All applicable control, event and format families need an explicit KS
representation, enumeration/intersection/range semantics, streaming buffer
contract and consumer tests on the target systems. Mapping a subset of CT/PU
controls to VideoProcAmp/CameraControl is insufficient for the full ledger;
encoding, generic XUs, media transport, still capture and status require their
own supported interfaces. A consumer need not expose controls the actual
device does not advertise, and unsupported requests must fail honestly.

The tested project M0 STREAM.SYS experiment publishes synthetic metadata and
rejects RUN. The frame worker remains unwired/unapproved; its portable
ownership tests do not establish kernel callback rundown. STREAM.SYS support,
safe MDL mappings, cancellation, power, unplug, service lifetime and OS
compatibility are separate gates. Current `MmGetSystemAddressForMdlSafe`
planning is NT2000+, not Windows 98 proof. No runtime row for 98 SE or ME can
be closed by the native Windows 7 Brio test.

The subsequent [portable dispatch-policy tests](../testing/2026-10-21-uvc-flow.md)
exercise asynchronous control admission, synchronous notification reentry,
delayed mapping cancellation, reserved teardown capacity/generations and
frame deadlines. The implementation still remains outside the kernel build.
Its explicit external worker-join/callback-rundown gates must be implemented
and proven before any kernel RUN claim.

For each row, completion requires the applicable sequence, not a generic
package-wide tick: a normative reference; bounded original implementation;
positive and malformed host fixtures; descriptor/capability-negative cases;
actual target-OS control or streaming evidence; teardown/error tests; and
the specific hardware/USB host stack/date recorded. Formats absent on Brio
need synthetic device coverage and another advertised physical device when
making hardware claims. No release or README Tested cell follows from this
ledger.

# UVC webcam driver (uvc98)

Status: development, 2026-10-21. Portable descriptor, payload and control
code and a STOP-only synthetic STREAM.SYS miniport exist. USB capture and
miniport RUN remain unfinished. Real Brio100 frames and stop/reopen have
been observed through native Windows 7 usbvideo, not this project driver.

Official UVC 1.1/1.5 documents and pinned Linux UVC sources were acquired
on 2026-10-21 under ignored `docs/references/`. See the
[specification record and verification checklist](../testing/2026-10-21-uvc-spec.md)
and [Linux provenance index](../testing/2026-10-21-linux-refs.md).

## 1. Purpose and devices served

A generic USB Video Class driver for webcams on Windows 98 SE, ME and 2000:
any camera that declares the video interface class (`0x0E`) and follows UVC
through 1.5. Owner, 2026-10-21: all standardized UVC features through 1.5
are requested; the [feature coverage ledger](uvc-features.md) supersedes
the narrower initial implementation list below. Typed access to the
48-byte probe/commit tail is implemented, but does not provide encoder or
format support. Post-2005 webcams are almost all UVC; on 98 SE,
ME and 2000 they have no driver at all unless the vendor shipped one, and
vendors stopped doing that around 2006.

Initial implementation priorities, not a completed-support list:

- Uncompressed formats (`VS_FORMAT_UNCOMPRESSED`): YUY2 and NV12 (UVC 1.1
  Uncompressed Payload, section 2.2 lists exactly these two GUIDs,
  `32595559-...` and `3231564E-...`), plus I420/M420 and others where the 1.5
  GUID list names them and a consumer exists.
- MJPEG (`VS_FORMAT_MJPEG`), passed through as compressed frames.
- Isochronous streaming with alternate-setting bandwidth selection, and bulk
  streaming (alternate setting 0 carrying a bulk endpoint).
- Processing Unit and Camera Terminal controls mapped to the KS property sets
  `PROPSETID_VIDCAP_VIDEOPROCAMP` and `PROPSETID_VIDCAP_CAMERACONTROL`.
- Still image method 1 (and 2 where cheap).

Frame-based and stream-based formats (including H.264, MPEG-2 TS and DV),
extension units, UVC 1.5 encoding units and output/video-render terminals
remain in the requested scope and are unfinished. See the ledger for all
families and the original UVC1.0 reference gap. The camera's microphone is
a separate USB audio function, handled by the applicable audio driver.

## 2. Target systems, and whether XP and later need it

### 2.1 The owner's AVStream claim, checked

The claim: `usbvideo.sys` is an AVStream minidriver, AVStream's interfaces
are exported by `KS.SYS` only from XP, so 98/ME/2000's `KS.SYS` lacks them,
and that is the main reason UVC webcams do not work there.

What the headers say:

- The Windows 98 DDK's `ks.h` (`tools/win98ddk-extracted/all/INCS_DDK/INC_WIN98_KS.H`
  in the xHCI98 tree) and the Windows 2000 DDK's `ks.h` (`tools/ntddk/inc/ks.h`)
  declare none of the AVStream API: no `KsInitializeDriver`, no
  `KSDEVICE_DESCRIPTOR`, no `KSFILTER_DESCRIPTOR`, no `KsCreateFilterFactory`.
  Both carry only the older KS helpers (`KsCreateDefaultClock` at 98 line 2394
  and 2000 line 2924, `KsAcquireResetValue`, and so on).
- WDK 7.1's `ks.h` (`tools/WinDDK71/inc/api/ks.h`) declares them, guarded by
  `#if (NTDDI_VERSION >= NTDDI_WINXP)` (the AVStream block opens at line 297
  with `KSDEVICE_DESCRIPTOR`; `KsInitializeDriver` is at line 4933, inside the guard that opens at
  line 4915).

So the in-box `KS.SYS` of 98 SE, ME and 2000, as their own DDKs describe it,
has no AVStream. That part of the claim holds.

What qualifies it: Microsoft's current "AVStream Overview"
(learn.microsoft.com, `windows-hardware/drivers/stream/avstream-overview`,
dated 2017-04-20) says "AVStream drivers build on Microsoft Windows XP,
Microsoft Windows Server 2003, or any platform Windows 98 Gold or later
version that has DirectX 8.0 or later version installed", and that before XP
one should use the latest DirectX DDK. That implies the DirectX 8.0+ runtime
updates `KS.SYS` on 98/ME/2000 with the AVStream exports. Which file versions
that runtime installs, and how complete its AVStream is, is **unverified**
here (settled by the DirectX 8.1 or 9.0c redistributable's file list and the
DirectX 9 DDK's `ks.h`, neither on this host). An NTDEV thread of June 2007
("Can usbvideo.sys work on Win2000/ME?") records a test of `usbvideo.sys` on
2000 failing to load on an unresolved `WmiTraceMessage` import, and on ME not
installing because its INF has only an NT section; one reply holds AVStream
works back to 98 SE. So the main reasons `usbvideo.sys` does not run on 2000
are XP-only imports and that it is an XP system binary (not redistributable,
and not a source here), not only KS.

Consequence for this design: as the owner asked, nothing depends on AVStream
on 98, ME or 2000. Section 3 records the AVStream-with-DirectX-9 route as the
alternative not taken. Owner, 2026-10-20: that route stays closed.

### 2.2 Per-OS plumbing

| System | In-box UVC | Streaming class we plug into | USB host stacks the driver meets |
|---|---|---|---|
| 98 SE | none | `STREAM.SYS` (in box: `tools/win98se-extracted/wdm/stream.sys`), `KS.SYS` without AVStream | `uhcd.sys`, `openhci.sys` (USB 1.1, in box: same folder); USB 2.0 only through xHCI98 or NUSB's back-ported EHCI stack (fully supported, section 11) |
| ME | none | `STREAM.SYS` (expected as on 98 SE; **unverified** locally, `tools/winme-extracted` holds no streaming files) | USB 1.1 in box; no EHCI in box (**unverified**; settled by ME's `layout.inf`) |
| 2000 | none | `STREAM.SYS` (the 2000 DDK documents the stream class and USBCAMD for it: `D_STREAM.CAB`, "Kernel Streaming Drivers: Windows 2000 DDK") | USB 1.1, and EHCI from SP4 (`tools/win2ksp4-extracted/USBEHCI.SYS`); xHCI98 |
| XP SP2+ | `usbvideo.sys`, AVStream (XP SP3's `inf/usbvideo.inf` matches `USB\CLASS_0E`, `Class=Image`) | n/a | n/a |

### 2.3 XP and later: stop at 2000

The package targets 98 SE, ME and 2000 and stops there. XP SP3's
`usbvideo.inf` (`tools/winxpsp3-extracted/inf/usbvideo.inf`) and XP x64's
(`tools/winxp64-extracted/inf/usbvideo.inf`) bind every `USB\CLASS_0E`
function; Vista through 8 carry `usbvideo.sys` in box. That driver is
AVStream-based, handles MJPEG with DirectShow's own decoders, and is what
every XP-era application was tested against; a stream-class driver from here
would be a regression on those systems, not a gain. XP RTM and SP1 lack
`usbvideo.sys` (Microsoft introduced it with SP2: **unverified** locally,
only SP3 is on this host); an XP SP1 user installs SP2 or SP3. Owner,
2026-10-20: there is no XP RTM/SP1 target; the package stops at 2000.

## 3. The OS-facing model: options and choice

### (a) A WDM stream class minidriver on `STREAM.SYS` - chosen

- The interface is documented and identical across 98, 2000 and WDK 7.1:
  `strmini.h` in the 98 DDK (`INC_WIN98_STRMINI.H`), the 2000 DDK
  (`tools/ntddk/inc/ddk/wdm/strmini.h`) and WDK 7.1 all declare
  `StreamClassRegisterAdapter` (= `StreamClassRegisterMinidriver`, line 1155
  and 1161 in both older headers), `HW_INITIALIZATION_DATA`,
  `HW_STREAM_INFORMATION`, `SRB_OPEN_STREAM`, `SRB_GET_DATA_INTERSECTION`,
  `SRB_CHANGE_POWER_STATE`, `SRB_SURPRISE_REMOVAL`,
  `StreamClassQueryMasterClock`. One binary can serve 98 SE, ME and 2000.
- It is the model every 98/2000 USB webcam used (USBCAMD is a library on top
  of it), so the VfW mapper and KsProxy were tested against exactly this
  shape of driver.
- `STREAM.SYS` ships in box on 98 SE (verified) and needs no DirectX.

**USBCAMD is not used.** The 2000 DDK's "Limitations of USBCAMD Version 2.0"
(`usbcmdds_2t9v.htm` in `D_STREAM.CAB`) reads: "USBCAMD Version 2.0 handles
cameras that have a single interface with multiple alternate settings
controlling the video signals. All alternate settings should have the same
type and number of pipes." A UVC camera has a VideoControl interface plus a
VideoStreaming interface whose alternate setting 0 has no endpoint (UVC 1.1
section 2.4.3.1, "Stream Bandwidth Selection") and whose other alternates
each carry one isochronous endpoint, so it fails both conditions. USBCAMD 1.0
on 98 SE (`usbcamd.sys`) is older still. USBCAMD also fixes its own transfer
shape ("two ISO transfers of 32 packets", `usbcmdds_1w6r.htm`), which section
5.4 needs to choose itself. We use `STREAM.SYS`'s export interface directly
and talk to USBD ourselves.

### (b) Our own AVStream-like layer - rejected

Writing a KS filter/pin framework of our own (KS IRP dispatch, property and
method sets, allocator framing, clock, data intersection) is most of
`STREAM.SYS` again, for no user-visible gain over (a), and every bug in it is
a bug KsProxy and the VfW mapper meet first. ReactOS has reimplementations of
both `ks.sys` (`drivers/ksfilter/ks`) and `stream.sys`
(`drivers/wdm/audio/legacy/stream`), both "See COPYING" (GPL-2.0), so the
code exists if the in-box `STREAM.SYS` ever proves unusable; they are kept as
references for how KS behaves, not shipped.

### (c) Video for Windows: how applications see the device

There is no separate VfW driver to write. The 2000 DDK, section 1.1.1
"Relationship to Video for Windows Capture Drivers" (`vidcapds_7a2b.htm`):
"To bridge the VFW and WDM worlds, a mapper is provided as part of the
operating system. This component, called the VFWWDM mapper, makes WDM drivers
appear as VFW drivers for legacy applications." The 98 DDK's copy of the same
page (`wdmvcap_9kab.htm`) omits that sentence but 98 SE and ME ship the mapper
too (`vfwwdm32.dll`; **unverified** locally, settled by the 98 SE and ME CD
`layout.inf`). DirectShow applications reach the same KS filter through
KsProxy (`ksproxy.ax`, CLSID `{17CCA71B-ECD7-11D0-B908-00A0C9223196}`, which
98 SE's in-box `wdma_usb.inf` registers as its KS filter's `CLSID`, line 90,
as `usbmidi.md` section 8 also cites). DirectShow is in box on 2000 (DirectX 7; **unverified** locally)
and on 98 SE
(DirectX 6.1 with DirectShow; **unverified** which DirectShow version 98 SE's
CD installs; any 98 SE with DirectX 8.1 or 9.0c has it).

What the minidriver exposes so both work (from the headers named; DDK sample
sources are not read, per the owner's decision of 2026-10-20 recorded in
`AGENTS.md`, "Sources"):

- One filter registered under `KSCATEGORY_CAPTURE`
  (`{65E8773D-8F56-11D0-A3B9-00A0C9223196}`, `ks.h` line 570 in the 98 DDK)
  and `KSCATEGORY_VIDEO`, through `Include=ks.inf, kscaptur.inf` and
  `Needs=KS.Registration, KSCAPTUR.Registration` (`.NT` variant on 2000).
  The `Include`/`Needs` form is the one 98 SE's `wdma_usb.inf` uses with
  `ks.inf` and `wdmaudio.inf`; the `KSCAPTUR.Registration` section names
  still need a non-sample source, which is the in-box `kscaptur.inf` of each
  system (to be read; **unverified**).
- Pin 0: capture, `PINNAME_VIDEO_CAPTURE`, one instance. Pin 1 (milestone 5):
  still, `PINNAME_VIDEO_STILL`. No preview pin: ICaptureGraphBuilder inserts a
  Smart Tee for a capture-only filter, and USBCAMD exposes the same two pins
  (`usbcmdds_4uk3.htm`, "Dual-mode Support").
- Data ranges `KS_DATARANGE_VIDEO` with specifier
  `KSDATAFORMAT_SPECIFIER_VIDEOINFO` and a `KS_VIDEOINFOHEADER`, one range per
  (format, frame size), with `KS_VIDEO_STREAM_CONFIG_CAPS` giving the frame
  interval range from the frame descriptor. UVC frame intervals are in 100 ns
  units (UVC 1.1, VS frame descriptor table, "Frame interval in 100 ns
  units"), the same unit as `AvgTimePerFrame`, so no conversion.
- `SRB_GET_DATA_INTERSECTION` and `SRB_SET_DATA_FORMAT`, which is where the
  VfW mapper's Format dialog and IAMStreamConfig land.
- Property sets: `PROPSETID_VIDCAP_VIDEOPROCAMP`,
  `PROPSETID_VIDCAP_CAMERACONTROL`, `PROPSETID_VIDCAP_VIDEOCONTROL` (still
  trigger, frame rate list), `PROPSETID_VIDCAP_DROPPEDFRAMES`, and
  `KSPROPERTY_CONNECTION_ALLOCATORFRAMING` on the pin. All five sets are in the
  98 DDK's `ksmedia.h` as well as 2000's.
- Formats offered on the pin: see section 6.

Exactly which properties the VfW mapper requires before it shows a device,
and which dialogs it builds from which set, is not in either DDK's
documentation (**unverified**; milestone 0 measures it with a synthetic
source, section 12).

### (d) An AVStream minidriver requiring DirectX 9.0c on 98/ME/2000 - not taken

Section 2.1 shows this route probably exists. It is not taken because the
owner excluded it, because it would make DirectX 8+ a hard prerequisite on
systems where the stream class needs nothing, because AVStream-on-98 through
the DirectX runtime is a rarely exercised path with no local header to build
against (the DirectX 9 DDK is not on this host), and because its only gain,
running on XP and later, is moot (section 2.3). Owner, 2026-10-20: this
route stays closed.

## 4. Device binding

UVC cameras are composite: device class `EF/02/01` with an Interface
Association Descriptor grouping the VideoControl and VideoStreaming
interfaces (function class `0x0E`, subclass `0x03`
`SC_VIDEO_INTERFACE_COLLECTION`), often followed by a UAC 1 microphone
function.

- **Under xHCI98** the HCD is the bus driver, hub and composite parent on
  every target (xHCI98 `docs/contributing/architecture.md`, opening note).
  It splits a device of class 0 or `EF/02/01` with one configuration and two
  or more interfaces by IAD, gives the function compatible ids
  `USB\Class_0E&SubClass_03&Prot_00`, `USB\Class_0E&SubClass_03`,
  `USB\Class_0E` from the IAD, and answers the function's
  GET_DESCRIPTOR(Configuration) with a synthesised configuration holding only
  its own interfaces with their real numbers (xHCI98
  `docs/contributing/design/13-superspeed-hcd.md`, the `BusQueryCompatibleIDs`
  row and the composite rule at lines 5044-5132 and 5152). The driver binds
  that function like a class driver binds an interface.
- **Under Microsoft's stacks** (98 SE's `usbhub.sys`, 2000 SP4's), the hub
  predates IADs. Whether either splits an `EF/02/01` device, and what ids the
  device node then has, is **unverified** (settled by plugging a UVC camera
  into 98 SE on in-box UHCI and into 2000 SP4 on in-box EHCI and reading the
  `Enum\USB` keys). In the worst case the whole device is one node with ids
  `USB\VID_vvvv&PID_pppp` and `USB\DevClass_EF&SubClass_02&Prot_01`.

Owner, 2026-10-20: the INF matches interface-level (function) ids only, which
exist only where the stack has split the device; a whole device is never
bound generically. An unsplit camera is served under xHCI98, or by a
per-camera `USB\VID_vvvv&PID_pppp` line added on request. So the INF matches
`USB\Class_0E&SubClass_03`, and never `USB\DevClass_EF&SubClass_02&Prot_01`.

Owner, 2026-10-24 (usb-wdm-free 0.1.0): the INFs widen from the Brio
100's lines to the UVC class ids: `USB\Class_0E&SubClass_03&Prot_00` (the
IAD video function xHCI98 splits off) and `USB\Class_0E&SubClass_01&Prot_00`
and `&Prot_01` (a VideoControl interface; protocol 01 is UVC 1.5's), with
the one per-camera whole-device line kept (`046D:094C`, for NUSB). Reading
of 2026-10-24 (2000 SP4, Windows' UHCI, `uvcsim.py` as `FFFF:00E0`): 2000's
hub does not split an `EF/02/01` device; the whole-device node gets the
**first interface's** compatible ids (`USB\Class_0e&SubClass_01&Prot_00`,
`USB\Class_0e&SubClass_01`, `USB\Class_0e`), as NUSB's does
(`2026-10-22-98se-nusb-legs.md`). So the `SubClass_01` lines also bind an
unsplit camera whose first interface is VideoControl, as a whole device, on
2000 and under NUSB: the driver then selects the configuration itself and
serves the video function only (below), and a microphone function stays
unserved. That departs from the 2026-10-20 rule above (Codex rel-fixes
round 1). Owner decision, 2026-10-24: accepted, and stated to users: on
Windows 2000's own USB stack and under NUSB on 98 SE the whole camera binds
to `uvc98`, and its microphone has no driver (under xHCI98, which splits the
camera, the microphone keeps its own function). Not served: a camera a stack splits per
interface (VideoControl and VideoStreaming as separate children, which a
UVC camera's mandatory IAD prevents on these stacks): the VideoControl
child cannot select its VideoStreaming sibling.

The driver code handles both: it walks whatever configuration descriptor it
is given, finds the first `0x0E/0x01` interface and the VideoStreaming
interfaces its VC header (`baInterfaceNr`) names, and uses only those. Bound
to a whole device by a per-camera line, it selects the configuration itself,
opens only the video function's interfaces, and leaves the microphone
unserved. A node that turns out to carry no video function fails start with
`STATUS_DEVICE_CONFIGURATION_ERROR` and a log line.

Whether 98 SE and 2000's USBD accept a SELECT_CONFIGURATION that lists only a
subset of the configuration's interfaces is **unverified**; the driver lists
every interface at alternate 0 and opens pipes only on its own.

## 5. USB side

### 5.1 Descriptors parsed

From UVC 1.1 chapter 3, constants as Linux `include/uapi/linux/usb/video.h`
names them:

- VC interface header (`bcdUVC`, `baInterfaceNr`), Input Terminal / Camera
  Terminal (`bmControls`), Processing Unit (`bmControls`), Selector Unit and
  Extension Unit (walked past; extension units ignored), Output Terminal.
- VC interrupt endpoint (status), optional.
- VS input header (`bEndpointAddress`, `bStillCaptureMethod`,
  `bTriggerSupport`), uncompressed and MJPEG format and frame descriptors
  (`wWidth`, `wHeight`, `dwMaxVideoFrameBufferSize`, discrete or continuous
  frame intervals), still image frame descriptor, color matching descriptor.
- Every alternate setting of the VS interface with its endpoint's
  `wMaxPacketSize`, including the High-Speed additional-transaction bits 12:11
  (bytes per microframe = (bits 10:0) x (1 + bits 12:11)).

Linux's `drivers/media/usb/uvc/uvc_driver.c` (GPL-2.0-or-later) is the
reference for the walk and its tolerance of malformed descriptors.

UVC 1.5 cameras (`bcdUVC` 0x0150, 48-byte probe and commit) work through
their uncompressed and MJPEG formats. Their other standard formats
(frame-based, stream-based, MPEG-2 TS, DV, H.264, H.264 simulcast, VP8,
VP8 simulcast) are counted against `bNumFormats` and skipped with their
frames, and the encoding unit and 1.5 processing unit are walked past
(`uvcdesc.c`; before 2026-10-23 a `VS_FORMAT_H264` or VP8 format made the
whole camera invalid, found by `uvcsim.py --uvc 1.5 --h264 native`).
Delivering those compressed formats is post-release work (section 12,
milestone 6).

### 5.2 Probe and commit

UVC 1.1 section 4.3.1.1, "Video Probe and Commit Controls": SET_CUR(PROBE)
with format index, frame index and frame interval, GET_CUR(PROBE) to read the
device's `dwMaxVideoFrameSize` and `dwMaxPayloadTransferSize`, then
SET_CUR(COMMIT) using the successfully negotiated values, including any
device-adjusted format, frame or interval. Unsupported and negotiable fields
are initialized to zero as specified in the operational model. Reject short
responses, unsupported returned indices/intervals, impossible frame sizes and
payload requirements the available endpoint cannot carry.

The structure is 26 bytes for UVC 1.0, 34 for 1.1, 48 for 1.5; the length
comes from `bcdUVC` and the data transfer's `wLength` must match it. The 34
and 48 byte layouts are in UVC 1.1 Table 4-47 and UVC 1.5 Table 4-75;
the 26 byte baseline is corroborated by Linux's `uvc_video_ctrl_size()`
(the 1.0 core specification has not yet been independently reviewed).
GET_LEN is specified for probe and commit in 1.1, but a length response does
not authorize interpreting a truncated or different-revision layout.

Candidate compatibility fixups from Linux's `uvc_fixup_video_ctrl()` in
`uvc_video.c`, to adopt only with device evidence, are:
`dwMaxVideoFrameSize` taken from the frame descriptor for uncompressed formats
(and for 1.0 devices answering 0), a payload size whose top 16 bits are all
ones masked to 16 bits, and a separately scoped `FIX_BANDWIDTH` quirk.
There is no general clamp of the negotiated payload size to the largest
alternate. For an IN stream the device owns `dwMaxPayloadTransferSize`;
renegotiate a smaller rate/format or fail if it cannot fit. UVC 1.5 Table
4-75 explicitly requires reconfiguration to avoid overshooting that limit.

### 5.3 Alternate setting selection

Linux `uvc_video_start_transfer()`: the smallest alternate whose endpoint
payload per (micro)frame is at least `dwMaxPayloadTransferSize`. Then
SELECT_INTERFACE. If USBD refuses for bandwidth (`USBD_STATUS_NO_BANDWIDTH`),
the stream open fails with `STATUS_INSUFFICIENT_RESOURCES` rather than silently
taking a smaller alternate that cannot carry the stream; the application
offers the user a smaller format. Stopping the stream selects alternate 0,
which frees the bandwidth (UVC 1.1 section 2.4.3.1). The quirks Linux keys on
VID/PID (`UVC_QUIRK_FIX_BANDWIDTH`, `PROBE_MINMAX`, `PROBE_DEF`,
`RESTRICT_FRAME_RATE`, `STREAM_NO_FID`, `MJPEG_NO_EOF`; `uvcvideo.h` lines
66-83) are taken as a table in the same shape, entries copied as cameras
need them, each with its NOTICE row.

### 5.4 Isochronous streaming and buffering

- URBs: `URB_FUNCTION_ISOCH_TRANSFER` with `USBD_START_ISO_TRANSFER_ASAP`
  (98 and 2000 `usbdi.h` line 156-157), one packet per (micro)frame, length
  the endpoint's full payload. N URBs kept queued; each completion's packets
  are parsed at DISPATCH_LEVEL into the current frame buffer and the URB is
  resubmitted at once.
- Shape: Full Speed, 16 packets per URB (16 ms), 3 URBs. High Speed, 8 packets
  per URB (1 ms), up to 6 URBs, cut to what the host controller driver can
  hold (below). Linux uses 5 URBs of up to 32 packets (`UVC_URBS`,
  `UVC_MAX_PACKETS`, `uvcvideo.h` lines 54-56); USBCAMD used 2 of 32.
- Under xHCI98 the in-flight work on one isochronous pipe is bounded by its
  ring: `XHCI_POOL_RING_TRBS` is 64 (`src/xhci.h:927`) and one request may
  use at most 62 TRBs (`XHCI_XFER_MAX_ISO_TRBS`, `src/xhci.h:3991`); a request
  that cannot fit an empty ring is refused and counted (`IsoRefusalsTooLarge`,
  `src/hcd_io.c:1393` and 1595), and a further request waits until the ring
  has room. Isochronous fragments are page-bounded (`src/xhci_xfer.c:3178`), so
  a 3072-byte packet that crosses a page costs two TRBs. The driver lays its
  packets **back to back at their capacity**, high bandwidth included, and
  pays that second TRB. It cannot spend a page per packet. USBD gives an
  isochronous packet no length of its own: its length is the distance
  to the next packet's offset (the last one's, to the buffer's end). A
  4096-byte stride, this design's first layout, made every
  high-bandwidth packet 4096 bytes long to the host controller driver.
  xHCI98's `XhciPipeIsoCheck` (`src/xhci_pipe.c`) refused each such URB
  with `USBD_STATUS_INVALID_PARAMETER`, because a packet was longer than the
  endpoint's payload (`docs/testing/2026-10-23-uvcsim-98se.md`, owner
  decision 2026-10-23). At 3 x 1024 bytes a URB of 8 packets is 24 KB with
  at most 6 page boundaries inside it, so at most 14 TRBs; the 62-TRB ring then holds
  about 4 such URBs, roughly 4 ms of stream in flight, and the other URBs
  wait for room. That is thin for 98 SE's
  DPC latency. Owner, 2026-10-20: xHCI98's isochronous ring is to be
  enlarged now, as a separate xHCI98 task; the 64-TRB figure is today's
  limit, not one this driver designs around permanently, so the URB count
  and shape are parameters, not constants.
  One isochronous request is never split across chunks and must fit the
  adapter's grant and 32 SG elements (`hcdPlanChunk`, `src/hcd_io.c:193`;
  `HCD_SG_ELEMENTS`, `src/hcd.h:198`): 8 packets of 3072 bytes is 24 KB, at
  most 7 pages.
- xHCI98 puts IOC on every packet's TD, so a High-Speed stream is 8,000
  events a second, absorbed by interrupter moderation (`src/xhci_xfer.c`
  around line 3366). Proven for audio; not yet for a camera at 8,000 packets
  a second on 98 SE (milestone 2 measures it).
- High-bandwidth endpoints (2 or 3 transactions per microframe): xHCI98 takes
  the count from `wMaxPacketSize` bits 12:11 (`src/xhci_pipe.c:358`) into Max
  Burst (`src/xhci_ctx.c` around line 666). Under Microsoft's 2000 SP4
  `usbehci.sys` (or NUSB's EHCI stack on 98 SE), high-bandwidth isochronous
  support is **unverified** (settled on the bench: YUY2 640x480 at 30 fps needs
  it, section 5.7).
- `MaximumTransferSize` in the pipe information is set to the URB's byte
  size; the default is `PAGE_SIZE` (`USBD_DEFAULT_MAXIMUM_TRANSFER_SIZE`,
  `usbdi.h` line 324).

### 5.5 Payload headers

UVC 1.1 section 2.4.3.3 and Table 2-5: byte 0 `bHeaderLength`, byte 1 flags
FID (bit 0), EOF (1), PTS (2), SCR (3), RES (4), STI (5), ERR (6), EOH (7)
(`UVC_STREAM_*`, `video.h` lines 167-174), then PTS (4 bytes) and SCR (6) if
flagged. Parse one complete UVC payload transfer, following the boundary
handling of Linux `uvc_video_decode_start()` and `uvc_video_decode_end()`:

- reject a payload shorter than 2 bytes or whose header length is under 2,
  over the received bytes, or smaller than `2 + (PTS ? 4 : 0) +
  (SCR ? 6 : 0)`, even when timestamps are not used. SCR contains an 11-bit
  SOF counter plus reserved bits; do not expose all 16 bits as SOF;
- a FID toggle with data already in the buffer completes the frame (some
  cameras never set EOF). Process that same payload as the start of the
  new frame, rather than losing its data. EOF completes a nonempty frame
  once, including a header-only EOF after earlier frame data; it never
  creates an empty frame. Core Table 2-5 makes EOF optional, while the
  uncompressed/MJPEG payload documents describe setting it at frame end;
- ERR marks the entire current frame bad until its boundary: dropped, counted in
  `PROPSETID_VIDCAP_DROPPEDFRAMES`;
- retain FID synchronization when no output buffer is available; discard the
  initial partial frame. Overflow, malformed headers and failed/missing USB
  payloads corrupt the affected frame. If a missing header could have been
  the next frame's start, discard that uncertain frame too and resume only
  after a trustworthy FID boundary. Otherwise a compressed frame's missing
  first fragment could be delivered as a complete image;
- an uncompressed frame whose byte count is not `dwMaxVideoFrameSize` is
  dropped (Linux `uvc_video_validate_buffer()`), never delivered torn;
- STI routes the frame to the still pin only when method 2 is negotiated.
  No-FID camera workarounds require a separate scoped quirk and tests.

PTS and SCR are not used for timing on 98/2000: the frame's
`PresentationTime` is the stream class master clock
(`StreamClassQueryMasterClock`) at the frame's first packet, `Duration` the
committed frame interval. No 64-bit division appears (xHCI98's rule against
`_alldiv`-style helpers on 98, its `AGENTS.md` line 436, applies equally
here); 64-bit adds and `Int32x32To64` only.

### 5.6 Bulk streaming cameras

UVC 1.1 section 2.4.3.2.1: alternate 0 carries a bulk IN endpoint, no
bandwidth negotiation, a payload header at the start of each payload
transfer, and a payload may span several bulk transfers. Linux
`uvc_video_decode_bulk()` is the model: the header is parsed from the first
chunk of each payload and later chunks append data. Preserve its header
until a short transfer or the negotiated maximum ends the payload, then
apply its EOF. A USB packet, a UVC payload and a Windows URB are distinct
boundaries; do not parse every URB continuation as a new header. Read-buffer
rounding must not change the negotiated UVC payload limit. The future bulk
path needs explicit short/ZLP, maximum-size and fragmented-header tests;
the current bounded payload core accepts complete payloads only.
Milestone 3.

### 5.7 Speed and bandwidth consequences

Arithmetic from USB 2.0's limits (Full Speed: at most 1023 bytes per frame
per isochronous endpoint; High Speed high-bandwidth: at most 3 x 1024 bytes
per microframe):

| Link | Ceiling for one stream | YUY2 160x120 (38,400 B) | YUY2 320x240 (153,600 B) | YUY2 640x480 (614,400 B) | MJPEG 640x480 (about 30-60 KB, scene-dependent) |
|---|---|---|---|---|---|
| Full Speed (98 SE/ME in-box UHCI/OHCI) | about 1.0 MB/s | about 26 fps | about 6 fps | under 2 fps | 15-30 fps |
| High Speed, 1 transaction/microframe | 8.2 MB/s | 30 fps | 30 fps | about 13 fps | 30 fps |
| High Speed, 3 transactions/microframe | 24.6 MB/s | 30 fps | 30 fps | 30 fps (18.4 MB/s) | 30 fps |

So on a 98 SE or ME machine with only its in-box USB 1.1, MJPEG is the only
format that gives a usable picture, and many High-Speed-only cameras offer
little or nothing in their Full-Speed configuration (per-camera,
**unverified**). YUY2 VGA at 30 fps needs High Speed high-bandwidth
isochronous: xHCI98 on any target, or 2000 SP4's or NUSB's EHCI if its
high-bandwidth support holds. Two such cameras on one High-Speed bus need
about 37 MB/s of periodic bandwidth together, within High Speed's 80 %
periodic limit (USB 2.0 section 5.6.4) but leaving little; the second open
will fail at SELECT_INTERFACE before that limit if the host driver's budget
is tighter.

### 5.8 Still image

UVC 1.1 section 2.4.2.4 and Table 2-4: method 1 takes the next video frame;
method 2 streams a still frame on the video pipe after still probe/commit
(section 4.3.1.2) and a trigger (`VS_STILL_IMAGE_TRIGGER_CONTROL`, 4.3.1.4),
marked STI; method 3 uses a dedicated bulk endpoint. The still pin
(`PINNAME_VIDEO_STILL`) does method 1 for every camera, and method 2 where
`bStillCaptureMethod` says 2. Method 3 is left out unless a tested camera
needs it. The trigger arrives as `KS_VideoControlFlag_Trigger` through
`KSPROPERTY_VIDEOCONTROL_MODE`, the route the 2000 DDK documents for USBCAMD
stills (`usbcmdds_7nsj.htm`). The hardware button (VC status interrupt,
section 2.4.2.2-2.4.2.3) raises `KSEVENTSETID_VIDCAPTOSTI` where the camera
reports it (later milestone).

### 5.9 Controls

GET_CUR/GET_MIN/GET_MAX/GET_RES/GET_DEF/GET_INFO on the unit's selector,
read once at start for every control the unit's `bmControls` declares, and
cached; SET_CUR on a property set. The KS property's `KSPROPERTY_*_FLAGS_AUTO`
maps to the UVC auto control where one exists.

| UVC control (`video.h`) | KS property (98 DDK `ksmedia.h` lines 3330-3338 for ProcAmp) |
|---|---|
| PU Brightness, Contrast, Hue, Saturation, Sharpness, Gamma | `KSPROPERTY_VIDEOPROCAMP_` same names |
| PU White Balance Temperature (+ Auto) | `KSPROPERTY_VIDEOPROCAMP_WHITEBALANCE` (+ auto flag) |
| PU Backlight Compensation | `KSPROPERTY_VIDEOPROCAMP_BACKLIGHT_COMPENSATION` |
| PU Gain, Power Line Frequency, White Balance Component | `KSPROPERTY_VIDEOPROCAMP_GAIN`, `_POWERLINE_FREQUENCY`, `_WHITEBALANCE_COMPONENT`: defined only in WDK 7.1's `ksmedia.h`, absent from the 98 and 2000 DDKs; answered with XP's ids, which 98/2000-era property pages will not show (**unverified** whether KsProxy passes unknown ids through) |
| CT Exposure Time Absolute (+ AE Mode) | `KSPROPERTY_CAMERACONTROL_EXPOSURE`: KS uses log2 seconds, UVC 100-microsecond units; converted by table |
| CT Focus Absolute (+ Auto), Iris, Zoom, Pan/Tilt, Roll | `KSPROPERTY_CAMERACONTROL_FOCUS`, `_IRIS`, `_ZOOM`, `_PAN`, `_TILT`, `_ROLL` |
| CT Privacy, Scanning Mode, relative controls, Digital Multiplier, Analog Video Standard, all extension unit controls | not exposed |

Values are restored after a reset or resume (Linux
`UVC_QUIRK_RESTORE_CTRLS_ON_INIT` covers cameras that lose them).

### 5.10 Surprise removal, power, multiple cameras

- Surprise removal: `SRB_SURPRISE_REMOVAL` (98 and 2000 `strmini.h`): every
  queued read SRB completed with `STATUS_CANCELLED` (or the pending-data
  status the stream class expects), URBs aborted with `URB_FUNCTION_ABORT_PIPE`
  and waited for by count, no further USB request issued. On 98, where no
  surprise-removal IRP exists, the stream class delivers the removal; the
  driver treats a USBD error status of "device gone" on any URB the same way.
- Power: `SRB_CHANGE_POWER_STATE` to D3 stops the stream (alternate 0); to D0
  re-runs probe/commit, SET_INTERFACE and control restore before data flows.
  Whether the INF should set the stream class value
  `DontSuspendIfStreamsAreRunning` needs a non-sample source (the DDK's
  stream class documentation, to be searched; **unverified**). No selective
  suspend.
- Multiple cameras: one stream class adapter per device node, the capture pin
  one instance each; the second camera's bandwidth limit is section 5.7's.

## 6. Formats and where conversion happens

Offered on the capture pin, in this order:

1. The camera's uncompressed formats, natively: YUY2 as
   `biCompression = 'YUY2'`, 16 bpp; NV12 as `'NV12'`, 12 bpp.
2. RGB24 (`BI_RGB`, bottom-up DIB) **synthesised in the driver from YUY2**,
   for every YUY2 frame size, because many VfW applications and the VfW
   preview path only handle RGB.
3. The camera's MJPEG formats, natively, as `'MJPG'` (subtype FOURCC `MJPG`):
   usable by any application with an MJPEG decoder installed.

Decisions and reasons:

- **No MJPEG decoding in the kernel.** A baseline JPEG decode in C without
  SIMD costs an estimated 50-100 cycles per pixel at 4:2:2 (an estimate, not
  a measurement; no Pentium-class machine is on the bench, so it stays an
  estimate unless one is obtained), so 640x480 is 15-30 million
  cycles a frame: on a Pentium MMX 200 that is 7-13 fps using the whole CPU,
  at DISPATCH_LEVEL or in a system thread, with the floating-point state
  unsaved on 98 unless every operation is integer. A kernel decoder is
  complexity and system hangs for a frame rate no user wants.
- **A user-mode decoder instead: an ICM (VfW) codec, `uvcdec.dll`**, that
  decompresses `MJPG` (inserting the standard Huffman tables when a frame
  omits DHT, as the MJPEG payload spec section 3.3 allows: "if not specified,
  standard tables as specified in JPEG standard (ISO 10918-1) section K.3.3
  are used") and `YUY2` to RGB24/RGB32. VfW's DrawDib and AVICap find it
  through ICM, and DirectShow's AVI Decompressor wraps ICM codecs, so one
  32-bit DLL serves both on 98, ME and 2000. User mode may use MMX and
  floating point freely. Owner, 2026-10-20: the JPEG core is adapted from
  IJG libjpeg, and the decoder ships as this user-mode codec (user-mode code
  is in scope where it exists to make a driver here work, `AGENTS.md`,
  "Sources"). Built 2026-10-22 as `src/video/uvcdec/`, MJPEG only, to
  RGB24, RGB32 and YUY2, first on IJG libjpeg 9f and the same day on
  libjpeg-turbo 3.2.0 (the libjpeg API and IJG code, with MMX/SSE2 SIMD):
  section 14.
- **RGB24 from YUY2 in the kernel** is the exception because it is cheap
  (Owner, 2026-10-20: kept): an
  integer table conversion at an estimated 10-20 cycles per pixel, 3-6
  million cycles for a 640x480 frame, done at PASSIVE_LEVEL in a system
  worker thread from the completed raw frame into the application's buffer.
  Integer only, no MMX (no floating-point state handling on 98's kernel).
  640x480 at 30 fps would need 90-180 MHz of CPU (estimate), so the driver
  does it only when the application chose RGB24; YUY2 itself costs a copy.
- Whether 98 SE or 2000 ship a YUY2 ICM decoder of their own (`msyuv.dll`)
  is **unverified** (settled by each CD's `layout.inf`); `uvcdec.dll` makes
  the answer not matter.

## 7. Driver structure

- `DriverEntry` fills `HW_INITIALIZATION_DATA` (`BusMasterDMA = FALSE`,
  `SerializationRequired = TRUE`, the extension sizes) and calls
  `StreamClassRegisterAdapter`. All SRBs arrive serialised at the stream
  class's priority; USB completions run at DISPATCH and touch only the
  frame-assembly state under one spin lock.
- Device SRBs: `SRB_INITIALIZE_DEVICE` (read descriptors through the lower
  device object, parse, select configuration, read control ranges),
  `SRB_GET_STREAM_INFO` (one or two `HW_STREAM_INFORMATION` built from the
  parsed formats), `SRB_OPEN_STREAM`/`SRB_CLOSE_STREAM`,
  `SRB_GET_DATA_INTERSECTION`, `SRB_GET/SET_DEVICE_PROPERTY`,
  `SRB_CHANGE_POWER_STATE`, `SRB_SURPRISE_REMOVAL`,
  `SRB_UNINITIALIZE_DEVICE`.
- Stream SRBs: `SRB_SET_STREAM_STATE` (STOP: alternate 0, abort; PAUSE:
  probe/commit; RUN: SET_INTERFACE, start URBs), `SRB_READ_DATA` (queued;
  completed with a whole frame, `KSSTREAM_HEADER.DataUsed`, time stamps and
  `KSSTREAM_HEADER_OPTIONSF_*` flags), `SRB_GET_STREAM_PROPERTY`
  (allocator framing: 3 frames video, 1 still, as USBCAMD,
  `usbcmdds_1w6r.htm`), `SRB_SET_DATA_FORMAT`.
- Frame assembly: a private ring of frame buffers sized
  `dwMaxVideoFrameSize`, filled by the completion path; a full frame is
  copied (or converted) into the oldest queued read SRB, so a slow application
  drops frames rather than stalling isochronous completion. Linux's split of
  header decode and copy (`uvc_video_decode_isoc()`, the copy work) is the
  model.
- Source files (planned): `uvcdrv.c` (entry, SRB dispatch), `uvcdesc.c`
  (descriptor walk; DDK-free, host-testable), `uvcprobe.c`
  (probe/commit and fixups; DDK-free), `uvcstrm.c` (URBs, payload parse,
  frame ring), `uvcfmt.c` (data ranges, intersection, YUY2 to RGB24),
  `uvcctrl.c` (property sets), `uvcquirk.c` (table), `uvc98.h`.

## 8. INF and install

- One INF, `uvc98.inf`, with a 98/ME section (`$CHICAGO$` signature,
  `DevLoader=*ntkern`, `NTMPDriver=uvc98.sys`, the form 98 SE's in-box WDM
  INFs use, for example `hiddev.inf` and `usbcdc.inf`) and an `.NT` section
  with `AddService`, both including `ks.inf` and `kscaptur.inf` and
  registering the capture interface with KsProxy's CLSID and a
  `FriendlyName`, as 98 SE's `wdma_usb.inf` registers its own KS interface,
  plus `KSCATEGORY_VIDEO`.
- `Class=Image`, `ClassGUID={6bdd1fc6-810f-11d0-bec7-08002be2092f}`, as XP's
  `usbvideo.inf` uses (lines 5-6) and USB cameras on 98/2000 commonly did
  (Owner, 2026-10-20: setup class `Image`). Whether the VfW mapper and
  KsProxy enumerate a device regardless of its setup class (they enumerate by
  interface category) is **unverified** and checked in milestone 0.
- `uvcdec.dll` copied to the system directory and registered as `VIDC.MJPG`
  and `VIDC.YUY2`: in `system.ini [drivers32]` on 98/ME, in
  `HKLM\Software\Microsoft\Windows NT\CurrentVersion\Drivers32` on 2000
  (the documented ICM registration places).
- Package: `src/video/uvc98/` (driver, INF and `build.cmd`); the codec
  is its own folder, `src/video/uvcdec/`, with its own build and
  `UVCDEC.INF` (section 14). Released into
  `free/video/uvc98/w98se-me/` and `free/video/uvc98/w2k/` (folder names as
  `README.md`, "Layout"), each folder holding `uvc98.sys`, `uvc98.inf`,
  `uvcdec.dll`. The two folders may hold the same binaries; separate folders
  keep the per-system tested statement honest.

## 9. Build toolchain

As xHCI98 builds its x86 drivers (`AGENTS.md` lines 201-202 there):

- `uvc98.sys`: Windows 2000 DDK in `tools/ntddk` (from `WIN2KDDK.EXE`) with
  MSVC 6.0 in `tools/MSVC600`, linked against the DDK's `stream.lib`,
  `usbd.lib` and `ks.lib`; one binary for 98 SE, ME and 2000. An import gate
  (as xHCI98's) checks every import against the 98 SE export set of
  `ntkern`/`stream.sys`/`usbd.sys`, taken from the 98 DDK's import libraries
  (`LIBS_DDK/LIB_I386_FREE_STREAM.LIB`, `..._KS.LIB`) and headers, and from
  the export tables of 98 SE's in-box `stream.sys`, `ks.sys` and `usbd.sys`,
  read for names and ordinals only (`AGENTS.md`, "Sources"); never from their
  code. The 98 DDK's `strmini.h`/`ks.h`/`ksmedia.h` are the
  reference for what 98 has; the build uses the 2000 headers, which match
  them for everything used here (section 3).
- `uvcdec.dll`: MSVC 6.0 with its own `VFW.H` (`tools/MSVC600/VC98/Include`),
  Win32 only, no CRT dependency beyond what 98 SE ships (`msvcrt.dll`).
- Host tests: the descriptor walk, probe fixups, payload parser and
  conversion are DDK-free C89 and run on the host against captured
  descriptor sets and payload streams.
- C89, no 64-bit division, as the rest of the project.

## 10. Code adapted from Linux and ReactOS, and NOTICE.md rows

| File (planned) | Adapted from | Licence |
|---|---|---|
| `src/video/uvc98/sys/uvcdesc.c` | Linux `drivers/media/usb/uvc/uvc_driver.c` (descriptor parsing) | GPL-2.0-or-later |
| `src/video/uvc98/sys/uvcprobe.c` | Linux `drivers/media/usb/uvc/uvc_video.c` (`uvc_fixup_video_ctrl`, alternate selection) | GPL-2.0-or-later |
| `src/video/uvc98/sys/uvcstrm.c` | Linux `drivers/media/usb/uvc/uvc_video.c` (payload decode, isochronous and bulk) | GPL-2.0-or-later |
| `src/video/uvc98/sys/uvcquirk.c` | Linux `drivers/media/usb/uvc/uvc_driver.c` device table, `uvcvideo.h` quirk flags | GPL-2.0-or-later / GPL-2.0 |
| `src/video/uvc98/sys/uvcctrl.c` | Linux `drivers/media/usb/uvc/uvc_ctrl.c` (control info table) | GPL-2.0-or-later |
| `src/video/uvc98/sys/uvc.h` | Linux `include/uapi/linux/usb/video.h` (constants) | GPL-2.0 WITH Linux-syscall-note |
| `src/video/uvcdec/turbo/` | libjpeg-turbo 3.2.0, four files patched for MSVC 6.0 (section 14.2; `NOTICE.md` rows for it and the configuration headers) | IJG licence; zlib licence for the SIMD; BSD-3-Clause for parts not built; all GPL-compatible |

SPDX tags checked on the current Linux master: `uvc_driver.c`, `uvc_video.c`,
`uvc_ctrl.c`, `uvc_queue.c`, `uvc_status.c` are `GPL-2.0-or-later`;
`uvcvideo.h` is `GPL-2.0`. ReactOS's `ks` and `stream` are read, not copied;
if any is copied, it gets its row. The 98 and 2000 DDK samples (TestCap,
the USBCAMD clients) are not read at all (Owner, 2026-10-20); the DDK
documentation and headers are.

## 11. Test plan

- **No QEMU camera model**: QEMU's tree has no UVC device (a 2021 RFC
  `usb-uvc.c` and a later `dev-video.c` series were proposed; neither is known
  to be merged: **unverified**, settled by the QEMU git log). QEMU is used for
  milestone 0 (a synthetic source, no USB) and for `usb-host` passthrough of a
  real camera to 98 SE and 2000 guests, which xHCI98's QEMU scripts already
  drive (`scripts/local/qemu-win98-*.cmd`, `qemu-win2k-*.cmd`); isochronous
  timing in QEMU needs `-display none` (xHCI98 `lessons.md`, "QEMU's SDL
  display stalls the whole device model").
- **Real cameras** (Owner, 2026-10-20: test hardware is one or two
  mainstream devices per class, with no Linux gadget rig): a mainstream
  YUY2+MJPEG UVC camera with high-bandwidth alternates, and a second
  mainstream camera, ideally one with a microphone. Bulk streaming, still
  method 2 and fault cases (missing EOF, ERR bit, short frames) are tested
  only as far as those cameras show them; the rest is covered by host tests
  on captured payload streams.
- Tested systems (Owner, 2026-10-20, README "Tested by the author"): 98 SE
  and 2000 SP4, the tested systems within this package's range. ME is not
  tested: the `w98se-me` folder is the same build, its ME cell stays "Built,
  not tested", nothing only ME could settle blocks a release, and the
  release notes list what stays unverified there (ME's streaming and USB
  files, the VfW mapper and KsProxy on ME, every run-time reading below).
- Hosts: 98 SE on in-box UHCI and OHCI (Full Speed), 98 SE on NUSB's EHCI
  stack (fully supported, Owner, 2026-10-20) and on xHCI98, 2000 SP4 on
  in-box EHCI and on xHCI98. The EHCI leg on each of 98 SE and 2000 is a
  High Speed camera on an EHCI port, so EHCI carries the stream; a Full
  Speed camera on an EHCI port lands on the companion controller and is a
  UHCI/OHCI leg. No Pentium-class machine is on the bench; the
  CPU cost of RGB24 and `uvcdec.dll` on one stays an estimate (section 6)
  unless such a machine is obtained.
- Applications: DirectShow AMCap and GraphEdit (DirectX SDK tools, run, not
  redistributed); a VfW AVICap application (a small GPL test program in
  `src/video/uvc98/test/`, plus whatever the owner uses); NetMeeting 3 on 98
  and 2000 as the period videoconferencing case.
- Cases: every format and frame size, frame-rate change, start/stop 1000
  times, unplug while streaming and while stopped, two cameras, standby and
  resume while open, bandwidth refusal.

The results go into `README.md`'s "Tested by the author" table.

## 12. Milestones

0. **Plumbing spike, no USB.** The stream class driver with a synthetic YUY2
   and RGB24 colour-bar source, installed on 98 SE and 2000: shows in the
   VfW mapper and in AMCap; records which properties the mapper requires and
   which setup class works. Measures YUY2-to-RGB24 and a libjpeg decode on
   the bench machines. About 2-3 weeks.
1. **First camera (the smallest useful milestone).** One YUY2 640x480 format
   at the camera's committed frame rate, isochronous, High Speed, on 2000 SP4
   (on its in-box EHCI and on xHCI98), seen and captured in AMCap and in the
   VfW test program. Descriptor walk, probe/commit, alternate selection,
   payload parsing, one capture pin. About 4-6 weeks after milestone 0.
2. **All uncompressed formats and the 9x targets.** Every YUY2/NV12 frame size
   and rate, RGB24 synthesis, 98 SE on in-box Full Speed controllers,
   98 SE on NUSB's EHCI stack and on xHCI98, event-rate measurement under
   each High-Speed stack on 98 SE. 3-4 weeks.
3. **Robustness.** Controls (ProcAmp, CameraControl), bulk cameras, surprise
   removal, power, two cameras, quirk table. 4-6 weeks.
4. **MJPEG.** Pass-through on the pin and `uvcdec.dll` for VfW and DirectShow.
   Owner, 2026-10-22: raised in priority, the next webcam milestone, with
   1080p30 on 98 SE as its goal; designed in section 14, the codec built
   and host-tested 2026-10-22, the driver part after milestone 2.
5. **Stills and the button.** Still pin method 1 and 2, status interrupt
   endpoint. 2-3 weeks.
6. **After the release: frame-based compressed formats.** Owner,
   2026-10-23: H.264 and VP8 come after the release, VP8 first.
   1. `uvc98` delivers frame-based compressed frames (the frame-based
      payload and the UVC 1.5 VP8/H.264 descriptor families, passed
      through on the pin as MJPEG is).
   2. A VP8 decoder in `uvcdec`: libvpx, if its decoder can be built for
      98 SE (its C99 source against MSVC 6.0 is the known obstacle).
   3. H.264 after a licensing and compiler review.
   Until then a UVC 1.5 camera works through its uncompressed and MJPEG
   formats (section 5.1).

Frank estimate: a first camera in AMCap is about two months of focused work;
a release worth calling generic (milestones 0-4) is about 5-7 months for one
developer, of which a third is the test matrix (two systems, four host
stacks, several cameras and applications). The kernel driver is roughly
8,000-12,000 lines of C, the codec 2,000-4,000. The largest risks are not
UVC: they are the VfW mapper's undocumented expectations, the isochronous
behaviour of each host stack at High Speed, and per-camera quirks.

## 13. Owner decisions, risks and open questions

Decisions on the questions this draft raised:

1. **AVStream with DirectX 9.0c (section 3 (d)).** Owner, 2026-10-20: the
   AVStream/DirectX route stays closed; the stream-class design needs no
   DirectX prerequisite.
2. **XP RTM/SP1.** Owner, 2026-10-20: no XP RTM/SP1 target; the package
   stops at 2000 (XP SP2 and later have `usbvideo.sys`).
3. **Whole-device binding on Microsoft's stacks.** Owner, 2026-10-20: the
   INF matches interface-level ids only and never binds a whole device
   generically; an unsplit camera is served under xHCI98, or by a per-camera
   VID/PID line added on request (section 4).
4. **xHCI98's isochronous ring.** Owner, 2026-10-20: xHCI98 enlarges it now,
   as a separate xHCI98 task; 64 TRBs is today's limit, not a permanent one
   (section 5.4).
5. **The MJPEG decoder.** Owner, 2026-10-20: adapted from IJG libjpeg and
   shipped as the user-mode codec `uvcdec.dll`; user-mode code that exists to
   make a driver here work is in scope (`AGENTS.md`, "Sources"). Owner,
   2026-10-22: "just go to turbo and see if it builds first": libjpeg-turbo
   (IJG libjpeg's code and API with SIMD) replaced 9f once it built and
   passed (section 14.2).
6. **Kernel RGB24.** Owner, 2026-10-20: the in-driver YUY2-to-RGB24
   synthesis stays.
7. **Setup class.** Owner, 2026-10-20: `Image`.
8. **Test hardware.** Owner, 2026-10-20: one or two mainstream UVC cameras;
   no Linux gadget board and no Pentium-class machine on the bench, so the
   CPU-cost figures stay estimates (section 11).
9. **Back-ported EHCI stacks on 98 SE.** Owner, 2026-10-20: NUSB's EHCI
   stack is fully supported and part of the test matrix.
10. **1080p.** Owner, 2026-10-22: the webcam is to reach 1080p, through
    MJPEG with a decoder shipped here, as the next webcam milestone; 98 SE
    at minimum (section 14).
11. **H.264 and VP8.** Owner, 2026-10-23: after the release, VP8 first
    (section 12, milestone 6); the release serves UVC 1.5 cameras through
    their uncompressed and MJPEG formats.

### Open questions

1. **Faster MJPEG decoding on 98-era CPUs.** Settled 2026-10-22: the
   owner asked for libjpeg-turbo; 3.2.0 builds with MSVC 6.0 and four
   small patches, its MMX/SSE2 routines halve the host's decode time or
   better, and plain C stays as the run-time fallback (sections 14.2,
   14.5). What 98 SE enables (SSE2 or only MMX) is a guest check (14.8).

### Unverified

Each with what settles it: DirectX 8+'s `KS.SYS` on 98/ME/2000
(redistributable file list, DirectX 9 DDK); ME's streaming and USB 2.0 files
(ME `layout.inf`); `vfwwdm32.dll`, `msyuv.dll` and the DirectShow version on
98 SE (CD `layout.inf`); the VfW mapper's required properties (milestone 0);
the `kscaptur.inf` registration sections and `DontSuspendIfStreamsAreRunning`
(in-box INFs and DDK documentation, not samples); IAD splitting on 98 SE's
and 2000's hub and the subset SELECT_CONFIGURATION (bench); high-bandwidth
isochronous on 2000 SP4's and NUSB's EHCI (bench); XP SP2 as
`usbvideo.sys`'s first release (SP2 media); QEMU UVC models (QEMU git log);
the CPU-cost figures (estimates; measured only if a Pentium-class machine is
obtained); every run-time reading on ME (not tested, section 11; a user's
report).

## 14. Milestone 4: MJPEG to 1080p30, and `uvcdec`

Owner, 2026-10-22: the webcam is to reach 1080p; the route is MJPEG
(compressed, so 1080p30 fits USB 2.0) with a decoder this project ships,
and it is the next webcam milestone, after milestone 2's 98 SE legs and ahead of milestone 3. Every driver
works on 98 SE at minimum (`AGENTS.md`), so 98 SE is where 1080p MJPEG is
judged. The codec part exists as `src/video/uvcdec/` (2026-10-22, host
built and tested; on 2026-10-22 run in 98 SE and 2000 guests, section 14.7). The driver part (section 14.6)
waits for milestone 2's 98 SE legs.

### 14.1 Why MJPEG: the bandwidth

Arithmetic, USB 2.0 limits as section 5.7 (High Speed high-bandwidth 3 x
1024 bytes per microframe, 24.6 MB/s; one transaction, 8.2 MB/s; Full
Speed about 1.0 MB/s):

| Format, size | Bytes a frame | At 30 fps | HS, 3 transactions | HS, 1 transaction | Full Speed |
|---|---|---|---|---|---|
| YUY2 1280x720 | 1,843,200 | 55.3 MB/s | about 13 fps | about 4 fps | under 1 fps |
| YUY2 1920x1080 | 4,147,200 | 124.4 MB/s | about 5.9 fps | about 2 fps | under 1 fps |
| MJPEG 1920x1080 | about 200-380 KB (scene and camera dependent) | 6.0-11.4 MB/s | 30 fps | 30 fps only for the smaller frames | about 3-5 fps |

The MJPEG frame sizes are those of this package's 1080p test frames
(199,097 to 379,686 bytes, section 14.5), which were made to be about the
size of a webcam's; a given camera's real sizes, the Brio 100's included,
are **unverified** (bench). So 1080p30 needs MJPEG on a High-Speed
high-bandwidth isochronous alternate: xHCI98 on every target, or NUSB's
EHCI on 98 SE and 2000 SP4's `usbehci.sys` if their high-bandwidth
isochronous support holds (section 5.4, still **unverified**).

### 14.2 The decoder library: libjpeg-turbo 3.2.0

Chosen: libjpeg-turbo 3.2.0 (released 2026-06-30), the libjpeg API and
IJG libjpeg's code with x86 SIMD routines, vendored in
`src/video/uvcdec/turbo/` (owner decision 5: adapted from IJG libjpeg;
owner, 2026-10-22: "just go to turbo and see if it builds first"). It
replaced IJG libjpeg 9f, which the codec was first built on the same day.

- **Why.** 98 SE and ME run one processor, so a faster decode has to come
  from SIMD, not threads. libjpeg-turbo's i386 code has MMX routines (a
  Pentium MMX, Pentium II and III, Athlon) and SSE2 ones (Pentium 4,
  Athlon 64, Core) for the IDCTs, upsampling and colour conversion, plus
  AVX2, and picks them at run time; its C routines remain for a processor
  with none. Section 14.5 has the gain.
- **It builds with MSVC 6.0.** Tried 2026-10-22, the newest release first
  (the brief's order was 3.x, then 2.1, 2.0, 1.5; 3.2.0 built, so no older
  one was tried). The CMake build is not used: `build.ps1` compiles the
  files `CMakeLists.txt` lists, with the configuration headers filled in by
  hand (`jconfig.h`, `jconfigint.h`, `jversion.h`). MSVC 6.0 stopped on
  three C99 features, fixed by four small patches marked in the files and
  listed in the package README: no variadic macros (`SNPRINTF` in
  `jinclude.h`), no `long long` (a `JUINT64` typedef, `unsigned __int64`,
  used in `jdct.h`, `jdapistd.c` and the compressor's `jchuff.c`), and no
  `SIZE_MAX` (`jconfigint.h`, outside the vendored tree). MSVC 6.0's C
  library has no `getenv_s`, so the build defines `NO_GETENV`. Everything
  else compiles at `/W3` as released (two C4761 warnings, in the
  compressor only); NASM 3.02 assembles the 44 i386 SIMD files as released
  (`docs/toolchain.md`). No newer compiler was needed: the DLL keeps
  MSVC 6.0's static C runtime and passes the import gate unchanged
  (KERNEL32 only, every name in 98 SE's and ME's export tables). The free
  DLL is 454,656 bytes: the 12- and 16-bit and lossless decoders that
  `jdmaster.c` reaches are linked too.
- **SIMD selection and the C fallback.** libjpeg-turbo reads CPUID
  (`simd/i386/jsimdcpu.asm`; AVX2 only with the system's XSAVE support).
  CPUID cannot say whether the system saves the XMM registers, and a
  system that does not (Windows 95, or any that has not set CR4.OSFXSR)
  faults on an SSE instruction. So `uvcjSimdSupport` runs one SSE and one
  SSE2 instruction under `__try` and drops those sets if they fault; MMX
  needs nothing from the system. Plain C decodes when no set remains. A
  Pentium III uses MMX (libjpeg-turbo's SSE routines serve only the float
  DCT, which the codec does not use); a Pentium 4 or later SSE2.
- **Output order.** B,G,R and B,G,R,X come from libjpeg-turbo's
  `JCS_EXT_BGR` and `JCS_EXT_BGRX`, so the library's RGB order is not
  changed (9f needed `JPEG_USE_RGB_CUSTOM`); RGB32's fourth byte is set to
  0 after each row batch.
- **Licence.** `turbo/LICENSE.md`: the libjpeg API library is under the IJG
  licence (`turbo/README.ijg`, kept unaltered), the SIMD code under the
  zlib licence, which `LICENSE.md` says the IJG licence subsumes there; the
  Modified BSD licence covers the TurboJPEG API, the programs and the build
  system, none of which is built here. All are permissive and
  GPL-compatible. A binary distribution states that the software "is based
  in part on the work of the Independent JPEG Group" (the package README,
  `UVCDEC.INF` and the DLL's version resource do). `NOTICE.md` has the
  rows.
- **Provenance.** The official release tarball
  `libjpeg-turbo-3.2.0.tar.gz` from the project's GitHub release, SHA-256
  `6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e`, its
  GnuPG signature good for the libjpeg-turbo signing key `0338 C8D8 D9FD
  A62C F9C4 21BD 7EC2 DBB6 F4DB F434`; the 224 files taken and those left
  out are listed in the package README.
- **9f removed.** IJG libjpeg 9f (`ijg/`, from Debian's
  `libjpeg9_9f.orig.tar.gz`) was removed once libjpeg-turbo passed every
  check; it remains in the history (commit `ff28bfa`). Nothing needs it:
  libjpeg-turbo's C path is the fallback and is itself 10-20 % faster
  than 9f's (section 14.5). `exact` mode's chroma upsampling differs: 9f
  scales chroma up in its IDCT, libjpeg-turbo (libjpeg v6b API) uses its
  fancy (triangle) upsampler, so the PSNR figures against the pattern
  changed; every one stays above the host test's thresholds.

### 14.3 Frames without DHT

UVC MJPEG frames often carry no DHT segment. The MJPEG payload document
allows this (section 6 quotes it: "if not specified, standard tables as
specified in JPEG standard (ISO 10918-1) section K.3.3 are used"), as the
older AVI MJPEG convention did: MSVC 6.0's own `MMREG.H` (line 2258)
carries `MJPGDHTSeg`, "JPEG DHT Segment for YCrCb omitted from MJPG data".
The Linux kernel's `uvcvideo` passes frames to user space unchanged;
user-space decoders supply the tables (libuvc's MJPEG conversion installs
them into libjpeg before decoding; recalled, not read for this record).

libjpeg-turbo does it itself: when it sets up the entropy decoder,
`jinit_huff_decoder` calls `std_huff_tables` (`turbo/src/jdhuff.c`,
`turbo/src/jstdhuff.c`), which installs the Annex K.3 tables in every slot
the frame has not defined (9f did the same through
`jpeg_make_d_derived_tbl`). So `uvcdec` inserts nothing and copies no
frame. The host test proves it on
every vector: each frame with its DHT stripped decodes byte for byte the
same as with it, in every output format and both speed modes.

### 14.4 The Video for Windows and DirectShow side

**VfW: an ICM codec driver, `UVCDEC.DLL`.** One 32-bit DLL whose only
export is `DriverProc` (VFW.H, MSVC 6.0):

- `DRV_OPEN` accepts `fccType` `vidc` (and a NULL `ICOPEN`, for the control
  panel); `ICM_GETINFO` names it `uvcdec`, handler `MJPG`.
- `ICM_DECOMPRESS_QUERY`: input `biCompression` `MJPG` (or `mjpg`); output
  `BI_RGB` 24 or 32 bpp (bottom-up, or top-down when `biHeight` is
  negative) or `YUY2` 16 bpp (always top-down, as YUV DIBs are), at the
  input size or at 1/2, 1/4 or 1/8 of it, which libjpeg's IDCT scaling
  makes cheaply. Anything else, RGB555/565 included, is `ICERR_BADFORMAT`
  (DrawDib converts from 24 bpp itself).
- `ICM_DECOMPRESS_GET_FORMAT` offers RGB24 bottom-up at the input size;
  `ICM_DECOMPRESS_BEGIN`, `ICM_DECOMPRESS`, `ICM_DECOMPRESS_END`, and the
  `ICM_DECOMPRESSEX` set for whole-frame rectangles. The compressed size is
  `lpbiInput->biSizeImage`, as VFW.H documents ("biSizeImage has the chunk
  size"); a frame with none is refused.
- `ICDECOMPRESS_HURRYUP` and `_NULLFRAME` frames are not decoded: the
  renderer is not going to show them, and on a slow CPU that is how a
  1080p preview keeps up (section 14.5).
- YUY2 output takes libjpeg's raw (downsampled) data for 2h1v (UVC's
  usual 4:2:2) and 2h2v frames and packs it, skipping upsampling and
  colour conversion; other layouts go through full-resolution YCbCr. JPEG's
  full-range YCbCr is mapped to BT.601 studio range (Y 16-235, C 16-240),
  what consumers of YUY2 expect.
- Two speed modes in the core, `fast` (IFAST IDCT, merged upsampling; the
  codec's mode) and `exact` (ISLOW, fancy upsampling); no setting exposes
  `exact` yet.
- Static C runtime: the DLL imports `KERNEL32.DLL` only, each name in 98
  SE's and ME's export tables (the build gate). No `msvcrt.dll` version
  question arises.
- Registration (`UVCDEC.INF`): 98 SE and ME, `SYSTEM.INI` `[drivers32]`
  `VIDC.MJPG=uvcdec.dll` plus the `MediaResources\icm\VIDC.MJPG` key the
  Multimedia control panel lists; 2000, `VIDC.MJPG` under
  `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32` with its
  `drivers.desc` entry (`[DefaultInstall.NT]`). These are the places
  section 8 already named. An existing `VIDC.MJPG` is left alone (owner,
  2026-10-22): the INF writes it only if absent (98 SE and ME through
  `UpdateInis` lines that act only then, 2000 with `AddReg` NOCLOBBER),
  and the warning is `src/video/uvcdec/README.md`'s, since an INF cannot
  warn only in that case. Uninstall removes it on 98 SE and ME only while
  it names `uvcdec.dll`; on 2000 it leaves it (setupapi deleted it whatever
  it named). Both seen in the guests (section 14.7). Section 8's `VIDC.YUY2` registration is not
  part of `uvcdec` 0.1: YUY2 cameras get RGB24 from the driver (section 6)
  and the codec decodes MJPEG only.

**DirectShow, 98 SE to 2000.** DirectShow applications (AMCap, GraphEdit)
reach the capture pin through KsProxy; an MJPEG pin type
(`MEDIASUBTYPE_MJPG`, `{47504A4D-0000-0010-8000-00AA00389B71}`, MSVC 6.0
`UUIDS.H` line 133) then needs a decoder filter. DirectShow's AVI
Decompressor (`CLSID_AVIDec`, MSVC 6.0 `UUIDS.H` line 492, in
`quartz.dll`) wraps every installed ICM codec, so `UVCDEC.DLL` covers
DirectShow too: no filter of ours is needed. Microsoft's own MJPEG
Decompressor filter (`CLSID_MjpegDec`, `{301056D0-6DFF-11d2-9EEB-006008039E37}`)
is declared in WDK 7.1's `UUIDS.H` (line 949) but in neither MSVC 6.0's
nor the 2000 DDK's, which suggests it came with a later DirectX than
those headers; which DirectX release put it into `quartz.dll`, and so
whether ME has it, is **unverified**. The 98 SE and 2000 SP4 guests of
2026-10-22 do not have it (no `HKCR\CLSID\{301056D0-...}` key; Windows
Media Player finds "no suitable decompressor" for MJPEG without `uvcdec`,
and plays it with `uvcdec` through the AVI Decompressor). Where it is
there, DirectShow may pick it over the AVI Decompressor; either decodes
the stream.

**XP and later** are not `uvc98` systems (section 2.3): `usbvideo.sys`
and Microsoft's MJPEG Decompressor serve them. So `uvcdec` has no x64
build; WDK 7.1's x64 compiler is not used here.

### 14.5 CPU cost at 1080p30: measured on the host, extrapolated

Measured 2026-10-22 by `JDTEST.EXE` under `build.cmd icmx86 free 20`:
best of 20 decodes, one thread, the DLL's own code (MSVC 6.0 `/O2`,
libjpeg-turbo 3.2.0), at each SIMD level through `uvcjSetSimd` (AVX2 is
this host's best; SSE2 is what a Pentium 4 to Core 2 gets; MMX what a
Pentium MMX to Pentium III and an Athlon XP get) and in plain C, on the
build host, an AMD Ryzen AI Max+ 395 (Zen 5; Windows reports 3.0 GHz,
single-thread boost up to about 5.1 GHz; the clock during the run was not
recorded, and runs vary by about 10-15 %). Milliseconds a 1920x1080 frame:

| Frame | Level | RGB24 fast | RGB24 exact | YUY2 fast | YUY2 exact | RGB24 at 1/2 | YUY2 at 1/2 |
|---|---|---|---|---|---|---|---|
| `h1080n.jpg`, FFmpeg 4:2:2, 253,789 bytes | AVX2 | 4.47 | 4.98 | 4.53 | 4.91 | 3.80 | 3.72 |
| | SSE2 | 4.86 | 5.70 | 5.37 | 5.26 | 3.94 | 3.86 |
| | MMX | 6.23 | 8.19 | 5.34 | 6.38 | 4.68 | 4.15 |
| | C | 12.24 | 14.96 | 8.97 | 9.47 | 6.68 | 6.34 |
| `h1080en.jpg`, FFmpeg 4:2:0, 198,677 bytes | AVX2 | 3.25 | 3.53 | 4.35 | 4.47 | 3.11 | 2.83 |
| | SSE2 | 3.60 | 4.68 | 3.81 | 4.16 | 2.85 | 2.88 |
| | MMX | 4.74 | 6.77 | 4.21 | 4.96 | 3.55 | 3.45 |
| | C | 8.25 | 12.15 | 7.18 | 7.10 | 5.45 | 6.01 |
| libjpeg-turbo 2h1v (UVC's 4:2:2), quality 85, about 380,000 bytes | AVX2 | 5.12 | 5.45 | 5.56 | 6.19 | 4.58 | 5.35 |
| | SSE2 | 5.76 | 7.17 | 5.53 | 7.24 | 4.65 | 5.60 |
| | MMX | 7.14 | 8.98 | 7.47 | 7.51 | 5.91 | 5.47 |
| | C | 12.83 | 15.77 | 12.79 | 13.04 | 7.97 | 9.54 |

For comparison, IJG libjpeg 9f (no SIMD) took 13.6, 10.6 and 14.8 ms for
RGB24 fast on the same three frames.

So SSE2 decodes RGB24 2.2-2.5 times as fast as plain C and MMX 1.7-2.0
times; YUY2, which already skips colour conversion and upsampling, gains
1.7-2.3 and 1.7 times. What SIMD cannot touch is the bit-serial Huffman
decoding, now the larger share, which is why the larger frames gain less
and why the 1/2-size decodes gain least. On the host: SSE2 about 9-14
clock cycles a pixel at 5.1 GHz, MMX 12-18, C 20-32.

What a 98-era CPU can sustain is not measured (no such machine on the
bench, owner decision 8). An extrapolation, as before by cycles per pixel,
with assumed ratios to Zen 5 (Pentium III and Athlon XP 2.5-3.5 times as
many cycles, Pentium 4 3.5-5 times, Core 2 1.5-2 times: assumptions, not
measurements), at about 16 host cycles a pixel on MMX and 13 on SSE2 (the
larger frames, RGB24 or YUY2 fast):

| CPU (SIMD level) | 1920x1080 | 1280x720 | 640x480 |
|---|---|---|---|
| Pentium III 1 GHz (MMX) | about 8-12 fps (C: 5-6) | about 19-27 fps | 30 fps |
| Athlon XP 2 GHz (MMX; it has no SSE2) | about 17-24 fps (C: 9-13) | 30 fps | 30 fps |
| Pentium 4 3 GHz (SSE2) | about 22-32 fps (C: 10-14) | 30 fps | 30 fps |
| Core 2 3 GHz (SSE2) | 30 fps, about 55-75 fps of decode (C: 24-30) | 30 fps | 30 fps |

The Pentium 4 figure is the least certain: its SSE2 unit splits 128-bit
operations into two 64-bit halves and its long pipeline pays more for the
Huffman decoder's branches, both outside the assumed ratio.

Stated plainly: with SIMD, 1080p MJPEG on 98 SE becomes a Pentium 4-class
proposition (about 20-30 fps at full size, 30 at half size) rather than
Core 2-only, and 720p30 reaches the Athlon XP and the faster Pentium IIIs;
a Pentium III stays at about 8-12 fps for full 1080p. 98 SE uses one CPU,
so cores do not help. Which SIMD set 98 SE actually enables on an SSE2
processor is checked in a guest (section 14.8).

What makes 1080p usable on the slower machines anyway:

- Capture does not decode: an AVI captured as `MJPG` stores the camera's
  frames as they arrive, so 1080p30 capture to disk costs about 6-11 MB/s
  of disk writes and little CPU, if the application's preview is off or
  slowed.
- Preview drops frames: the renderer marks frames it will not show
  `HURRYUP` and `uvcdec` skips them.
- Preview at half size: a 1/2-size decode costs about 60-85 % of a full
  one with SIMD (the table; Huffman decoding is not scaled), 55-75 % in C.
- YUY2 to a DirectDraw overlay leaves scaling and colour conversion to the
  video card where it supports a YUY2 overlay.

### 14.6 The driver side: MJPEG pass-through (for `uvc98`)

What `uvc98.sys` must do, on top of milestone 2 (the descriptor walk
already keeps MJPEG frames, `UVC_DESC_MJPEG` in `uvcdesc.h`, and the
assembler takes variable-size frames, `uvc-features.md`):

1. **Data ranges.** One `KS_DATARANGE_VIDEO` per MJPEG frame descriptor:
   `MajorFormat` `KSDATAFORMAT_TYPE_VIDEO`, `SubFormat` `MEDIASUBTYPE_MJPG`,
   `Specifier` `KSDATAFORMAT_SPECIFIER_VIDEOINFO`; `bmiHeader`
   `biCompression` `MJPG` (`0x47504A4D`), `biBitCount` 24, `biPlanes` 1,
   `biWidth`/`biHeight` from the frame descriptor (height positive),
   `biSizeImage` the largest frame (below); `bFixedSizeSamples` FALSE,
   `bTemporalCompression` FALSE, `SampleSize` the largest frame; the
   `KS_VIDEO_STREAM_CONFIG_CAPS` intervals from the frame descriptor and
   bit rates from `dwMinBitRate`/`dwMaxBitRate`. No RGB24 range is
   synthesised from MJPEG: the kernel never decodes JPEG (section 6).
2. **Order.** Uncompressed ranges first, as section 6, then MJPEG; the
   frame sizes YUY2 cannot carry at a useful rate (720p and 1080p on USB
   2.0, section 14.1) are reachable only as MJPEG.
3. **Buffer size.** The committed probe's `dwMaxVideoFrameSize`, raised to
   the frame descriptor's `dwMaxVideoFrameBufferSize` where that is larger
   (UVC 1.0/1.1 descriptors); allocator framing and `biSizeImage` use it.
   No equality check on the delivered size: it applies to uncompressed
   frames only (section 5.5).
4. **Delivery.** `KSSTREAM_HEADER.DataUsed` = the assembled frame's bytes,
   `KSSTREAM_HEADER_OPTIONSF_SPLICEPOINT` on every frame (MJPEG is intra
   only), time stamps as for uncompressed frames. The VfW mapper must hand
   `DataUsed` to the codec as `biSizeImage` (`uvcdec` refuses size 0):
   **unverified**, the first thing the 98 SE leg checks.
5. **Frame checks, cheap and in the kernel.** Drop and count (dropped
   frames property) a frame that has the ERR bit, a lost start or an
   overflow (section 5.5 as for every format), that is empty, or whose
   first two bytes are not `FF D8` (SOI). Leave everything else to the
   codec: no DHT insertion, no EOI search, trailing padding passed through
   (libjpeg stops at EOI).
6. **Bandwidth.** Alternate selection as section 5.3 from the camera's
   `dwMaxPayloadTransferSize`; the `FIX_BANDWIDTH`-style reduction stays
   off for MJPEG (`uvcusb.c` already skips it for `UVC_DESC_MJPEG`).
7. **Copy.** A 1080p MJPEG frame is under about 0.5 MB, so the copy to the
   read SRB is cheap; it needs no worker thread.
8. **INF.** `uvc98.inf` installs the codec with the driver: `UVCDEC.DLL`
   in its `CopyFiles` (DIRID 11) and the same `[drivers32]` `UpdateInis`
   (98 SE, ME) and `Drivers32` `AddReg` (2000) lines as `UVCDEC.INF`, so
   one install gives MJPEG; `UVCDEC.INF` stays for installing the codec
   alone.

### 14.7 Tests

Host (`build.cmd`, every build; 336 checks on 2026-10-22, libjpeg-turbo
3.2.0):

- Vectors (`src/video/uvcdec/vectors/`): FFmpeg-made frames of a
  synthetic pattern (no camera capture), 320x240, 640x480 and two 1080p
  frames, with and without DHT; PSNR against the pattern (30.2-48.6 dB),
  DHT and no-DHT outputs identical.
- libjpeg-turbo-made frames in the test: 2h1v, 2h2v, 1h1v, grayscale,
  restart markers; YUY2's raw path taken exactly for the 2h layouts.
- SIMD against plain C: every vector and every generated frame decoded in
  RGB24, RGB32 and YUY2, `fast` and `exact`, at 1/1, 1/2, 1/4 and 1/8, at
  each SIMD level the host has (AVX2, SSE2, MMX) and in plain C; the
  tolerance is none, and none was needed: 462,672,000 output bytes, 0
  different. Every path the codec takes is 8-bit integer arithmetic that
  libjpeg-turbo's SIMD routines compute exactly as its C ones do.
- Errors: a truncated frame decodes with a warning, garbage, all-zero and
  3-byte frames are refused, a size mismatch and an odd YUY2 width are
  refused, and the decoder works again after each.
- ICM: `DriverProc` called directly, every message above, the bottom-up
  RGB24 DIB equal to the flipped core decode, YUY2 through
  `ICM_DECOMPRESSEX` equal to the core's, HURRYUP leaving the buffer
  untouched.

Guests (2026-10-22, `docs/testing/2026-10-22-uvcdec-guest.md`; TCG, no
camera):

- `JDTEST.EXE` with `vectors/` in 98 SE on `-cpu pentium3` (SIMD sets
  `MMX SSE`, 256 checks PASS) and `-cpu core2duo` (`MMX SSE SSE2`, 296
  checks PASS), and in 2000 SP4 on `pentium3` (256 PASS): 98 SE enables the
  XMM state, so SSE2 is used where the CPU has it; every SIMD output equals
  plain C. Under TCG SSE2 took about 0.5-0.7 of C's time and MMX about the
  same as C (TCG emulates MMX slowly); no speed claim.
- `UVCDEC.INF` on 98 SE and 2000: install, reinstall, uninstall, and the
  same with a foreign `VIDC.MJPG` present (left alone with the new INF; the
  old one replaced it).
- MJPEG AVIs made from the vectors (640x480 and 1080p, without DHT) and an
  FFmpeg 4:2:2 test clip decode in Media Player (VfW: `MPLAYER.EXE`,
  `MPLAY32.EXE`) and Windows Media Player (DirectShow); without `uvcdec`
  both fail.

### 14.8 Owed and unverified

- 98 SE VM legs with the Brio 100 in MJPEG, once section 14.6 exists:
  `UVCDEC.INF` installed on 98 SE (and 2000), a VfW capture application's
  format dialog listing the MJPEG sizes, preview through `uvcdec`, an MJPEG
  AVI captured at 1080p, frame rates and dropped frames recorded, under
  NUSB's EHCI and under xHCI98; then AMCap through KsProxy and the AVI
  Decompressor (or Microsoft's MJPEG Decompressor if present).
- Done 2026-10-22 (section 14.7): the codec alone on 98 SE and 2000
  guests (install, playback in Media Player and Windows Media Player) and
  `JDTEST.EXE` in them on `pentium3` and `core2duo`. Still owed without a
  camera: ME; `ICInfo`/`ICOpen` from a program of our own (the players
  stand in for it).
- `UVCDEC.INF` open points (owner): on 2000 the uninstall leaves
  `VIDC.MJPG=uvcdec.dll` behind; on 98 SE and ME it removes the
  `MediaResources\icm\VIDC.MJPG` values even when another codec wrote
  them; no install-time warning when another codec holds `VIDC.MJPG`
  (an installer could check and say so).
- **Unverified:** the VfW mapper passing `DataUsed` as `biSizeImage`;
  `[DefaultInstall.NT]` being chosen by 2000's right-click Install (the
  guest ran the `rundll32` line); `CLSID_MjpegDec` on ME; the Brio 100's MJPEG frame
  sizes and layout (2h1v expected); the 98-era frame rates of section 14.5
  (extrapolated); whether ME sets CR4.OSFXSR (98 SE does, section 14.7);
  the MMX gain on a real Pentium III.

# USB Audio Class 2.0 driver (UAC 2)

Status: design draft, 2026-10-20; experimental X4 M1 integration, 2026-10-21.
The descriptor host tests and initial plumbing builds are recorded in
[the wave 1 log](../testing/2026-10-21-wave1.md). Win7 x86 now exposes the
playback endpoint; two controlled physical X4 real-buffer runs completed
8/8 buffers and 96000 samples, including reopen. Each still recorded nine
packet errors; audible fidelity and general device support remain unverified.
The M1 experiment is narrower than this design's eventual device/OS scope.
Official references and packet checks are recorded in the
[specification audit](../testing/2026-10-21-uac2-spec.md), with pinned
[Linux audio/video sources](../testing/2026-10-21-linux-refs.md) retained
locally for reference. No release yet.

## 1. Purpose and devices served

A WDM function driver for USB Audio Class 2.0 audio functions: playback and
capture on USB DACs, headsets, and multi-channel audio interfaces, for
Windows 98 SE, ME, 2000, XP, Vista, 7 and 8 (x86; x64 from XP).

No target system has a UAC 2 driver. Microsoft's first one is
`usbaudio2.sys`, shipped "starting with Windows 10, release 1703"
(Microsoft Learn, "USB Audio 2.0 Drivers", read 2026-10-20). Every target
ships only the UAC 1.0 driver `usbaudio.sys`, installed by `wdma_usb.inf`,
which matches the bare compatible id `USB\CLASS_01`. Read on this host in the
targets' own INFs (text, under xHCI98's git-ignored `tools\`):

| System | INF | Match | Driver |
|---|---|---|---|
| 98 SE | `tools\win98se-extracted\inf\wdma_usb.inf` | `USB\CLASS_01` plus VID/PID `&MI_00` entries | `HKR,,Driver,,USBAUDIO.sys`, a KS filter (`Needs=KS.Registration, WDMAUDIO.Registration`) |
| 2000 SP4 | `tools\win2ksp4-extracted\inf\wdma_usb.inf` | `USB\CLASS_01` | `usbaudio.sys` |
| XP SP3, XP x64 | `tools\winxpsp3-extracted\inf\wdma_usb.inf`, `tools\winxp64-extracted\inf\wdma_usb.inf` | `USB\CLASS_01` | `usbaudio.sys` |
| Vista SP2, 7 SP1 | `tools\vista-x86-extracted\inf\wdma_usb.inf`, `tools\win7-x86-extracted\inf\wdma_usb.inf` | `USB\CLASS_01` | `USBAUDIO.sys` |
| ME | not extracted on this host | - | unverified; ME's `wdma_usb.inf` would settle it |

None of these INFs excludes protocol `0x20`. The Windows 10 `wdma_usb.inf`
lists `USB\Class_01&SubClass_0x&Prot_20` as exclusions (same Microsoft Learn
page); the older ones do not, so on every target the UAC 1.0 driver is a
candidate for a UAC 2 function, and loses only to a better-ranked match
(section 7).

xHCI98's documents call Windows 98 SE's USB audio driver `USBAUDIO.VXD`
(`docs/using/release-notes.md`, `docs/contributing/test-equipment.md`). The
98 SE INF above names `USBAUDIO.sys`. This design follows the INF.

Owner, 2026-10-21: the requested target is the full standardized UAC2
feature set, including standardized encoded formats, clocks, entities,
controls and endpoint modes. The [feature coverage ledger](uac2-features.md)
supersedes the initial PCM-only exclusions and records implemented versus
missing behavior. Current physical playback is a narrow stereo16/48k
experiment, not completion of that target. UAC 3.0 is a separate class;
vendor-specific DSD semantics require separate evidence rather than being
inferred from UAC2 compliance. UAC 1.0 functions stay with the in-box
`usbaudio.sys` (Owner, 2026-10-20: not handled by this driver in v1).

One device the owner already holds is a UAC 2.0 device: the Sound Blaster X4 (`041E:3278`, High Speed, seven
interfaces in two IADs, isochronous `bInterval` 1, 3 and 4), recorded in
xHCI98 `docs/contributing/test-equipment.md`, "The Sound Blaster X4".

## 2. The decision on the OS-facing model

**Decision: a PortCls adapter driver with a WaveCyclic miniport per stream
direction and a Topology miniport, one source tree for every target.**

### 2.1 The options

1. **A PortCls (`portcls.sys`) adapter with WaveCyclic + Topology
   miniports.** PortCls owns the KS filter and pin plumbing, property
   dispatch, data intersection, the clock and position reporting, and the
   registration that SysAudio, KMixer and WDMAUD consume. The miniport
   supplies data ranges, a cyclic buffer, a position, and node properties.
2. **A PortCls adapter with a WavePci miniport.** Same plumbing, but the
   miniport is handed the client's buffers as scatter/gather mappings. The
   Windows 98 DDK's "Known Issues with WDM 1.0" says "The WavePCI miniport may
   not report positions accurately" (98 DDK CHM, `HELP_WIN98DDK.CHM`, topic
   `audnotes_8t0u.htm`, May 1999). Rejected: more code for a bus that has no
   scatter/gather DMA to benefit, and a documented defect on the oldest
   target.
3. **A raw KS filter on `ks.sys`, as `usbaudio.sys` is.** The driver itself
   implements the filter and pin factories, every property set (pin, connection,
   audio, topology), data intersection, allocator framing and the clock, with
   the `Ks*` helper functions. Both the Windows 98 DDK (`incs\INC_WIN98_KS.H`)
   and the 2000 DDK (`ntddk\inc\ks.h`) declare the helpers, so it is buildable
   for 98 SE, but it is several times the code of option 1 for the same result
   and has no sample in either DDK. Rejected.
4. **AVStream** (`KSDEVICE_DISPATCH`, `KSFILTER_DESCRIPTOR`, declared in WDK
   7.1 `inc\api\ks.h`). Not declared in the 2000 DDK's `ks.h` (no
   `KSDEVICE_DISPATCH` there), so not available to the 98 SE / ME / 2000
   build. Whether a DirectX 8.x redistributable brings an AVStream-capable
   `ks.sys` to 98 SE, ME and 2000 is **unverified** (the DirectX 8.1
   redistributable's file list and its `ks.sys` version would settle it); even
   if it does, it would make the driver depend on a redistributable. ReactOS's
   `drivers/usb/usbaudio` (UAC 1.0) is an AVStream driver, which is why it is
   only a reference here, not a base.

### 2.2 Why PortCls WaveCyclic

- **It exists on every target.** `portcls.sys`, `ks.sys`, `kmixer.sys`,
  `sysaudio.sys`, `wdmaud.sys` and `stream.sys` are present in
  `tools\win98se-extracted\wdm\`. The 2000 DDK (`tools\ntddk\inc\portcls.h`,
  1996-1998 copyright) declares `IPortWaveCyclic`, `IMiniportWaveCyclic`,
  `IMiniportWaveCyclicStream`, `IPortTopology`, `IMiniportTopology`,
  `IDmaChannel`, `IServiceGroup`, `IPortEvents`, `IAdapterPowerManagement`
  and the `Pc*` functions `PcAddAdapterDevice`, `PcInitializeAdapterDriver`,
  `PcNewPort`, `PcNewServiceGroup`, `PcRegisterSubdevice`,
  `PcRegisterPhysicalConnection`, `PcForwardIrpSynchronous`,
  `PcRegisterAdapterPowerManagement`, `PcRequestNewPowerState`,
  `PcGetTimeInterval`, `PcRegisterIoTimeout`. WDK 7.1
  (`tools\WinDDK71\inc\ddk\portcls.h`) keeps all of them and adds WaveRT and
  `IPortClsVersion`.
- **PortCls versions differ, and the header says so.** WDK 7.1 `portcls.h`
  (lines 2758-2793) enumerates `kVersionWin98`, `kVersionWin98SE`,
  `kVersionWin2K`, `kVersionWin98SE_QFE2` ("Hotfix Package 269601"),
  `kVersionWin2K_SP2`, `kVersionWinME`, `kVersionWin98SE_QFE3`,
  `kVersionWinME_QFE1`, `kVersionWinXP` onward, with "DO NOT ASSUME THAT EACH
  SUCCESSIVE ENUM IMPLIES A FEATURE SUPERSET" and `IPortClsVersion` marked
  unsupported on 98, 98 SE, 2000 RTM, 98 SE QFE2 and ME. So the driver may
  not ask PortCls its version on the oldest targets; it must use only what the
  2000 DDK header declares and probe nothing newer. The
  [98 SE/ME export-table run](../testing/2026-10-21-exports.md) now records
  the actual PortCls and KS function names and ordinals. Both provide the
  current M0 PortCls imports and `PcGetTimeInterval`; `PcRegisterIoTimeout`
  is absent on 98 SE but present on ME. The shared oldest-target path
  must not statically import it. Owner, 2026-10-20: the export table of an in-box
  Microsoft binary may be read for names and ordinals only (`dumpbin
  /exports`, no disassembly) and recorded as such (`AGENTS.md`, "Sources").
  The build gate currently checks a declared import allowlist; the
  recorded tables provide separate export-presence evidence for PortCls
  and KS. NTKERN export validation is still pending. The initial PortCls
  set was `PcInitializeAdapterDriver`, `PcAddAdapterDevice`, `PcNewPort`,
  `PcNewServiceGroup`, `PcRegisterSubdevice`, `PcRegisterPhysicalConnection`
  and `PcRegisterAdapterPowerManagement`, all now confirmed in both tables.
- **One INF can serve 98 and 2000.** The INF shape (one `$CHICAGO$` INF with
  `AddInterface` lines for `KSCATEGORY_AUDIO`, `KSCATEGORY_RENDER`,
  `KSCATEGORY_CAPTURE` and `KSCATEGORY_TOPOLOGY`, and the 9x
  `Drivers\wave\wdmaud.drv` / `Drivers\mixer\wdmaud.drv` values) is taken
  from the in-box `wdma_usb.inf` and `wdmaudio.inf` of the targets and the
  DDK's audio INF documentation. DDK sample sources (the 2000 DDK's `sb16`,
  `ac97`, `mssb16.inf`, the XP DDK's MSVAD) are not read (Owner,
  2026-10-20).
- **Microsoft's own later UAC 2 driver is a PortCls miniport** ("The driver
  is a WaveRT audio port class miniport", Microsoft Learn page above), so
  PortCls over a USB function is a shape Microsoft itself chose. WaveRT is
  Vista+, so this design uses WaveCyclic everywhere (WaveCyclic is still
  supported on Vista through 8; WDK 7.1 declares both).
- **The 2000 DDK warns against exactly this, and the warning is about
  compatibility, not capability**: "USB audio devices should conform to USB
  audio device class specifications, therefore no software driver is
  required. Failure to support a USB device in this manner could result in
  future incompatibility between Kmixer and the upper end of a USB driver"
  (2000 DDK streaming CHM, `D_STREAM.CAB`, topic `pcdesign_5pkj.htm`,
  June 2000). A PortCls driver is exactly a sound card driver to KMixer, which
  is the least exposed position available.

### 2.3 What a PortCls driver over USB needs that a PCI one does not

- **No hardware resources and no DMA adapter.** The miniport implements its
  own `IDmaChannel` object around a non-paged buffer, rather than calling
  `IPortWaveCyclic::NewMasterDmaChannel`. `portcls.h` declares `IDmaChannel`
  as an interface a miniport may implement itself; whether the DDK
  documentation describes this for a device with no DMA hardware needs a
  non-sample source (the DDK audio documentation, to be read; the MSVAD
  sample is not read) and is **unverified**; M0 settles it in practice. The
  resource list PortCls hands the start routine is empty and ignored.
- **A lower device object for URBs.** The driver's `AddDevice` calls
  `PcAddAdapterDevice` with the function PDO, keeps the PDO, and sends
  `IOCTL_INTERNAL_USB_SUBMIT_URB` to the device object directly below its
  FDO. `IoGetLowerDeviceObject` is XP+ only; on 98 SE / ME / 2000 the driver
  finds it by walking `AttachedDevice` up from the PDO to the object whose
  `AttachedDevice` is its own FDO. Whether PortCls on 98 SE accepts a PDO
  enumerated by a USB hub or composite parent (rather than by PCI or ISAPnP) is
  **unverified**; milestone M0 exists to settle it on 98 SE first.
- **PnP and power are PortCls's.** Start, stop and remove arrive as PortCls
  callbacks; the driver registers `IAdapterPowerManagement` and stops its
  isochronous streams before D3. Surprise removal (the user pulls the cable
  while playing) is the riskiest path on 98 SE: PortCls must close the pins
  while URBs fail with device-gone status. Unverified on every target; it is
  a named reading in each milestone.

## 3. Target systems and the per-OS plumbing

| System | Stack above the driver | Who groups the UAC 2 function | High-Speed hosts realistically available |
|---|---|---|---|
| 98 SE | PortCls 98 SE, KMixer, SysAudio, WDMAUD (`wdmaud.drv` for WinMM) | Not the in-box hub: it has no IAD support (below) | xHCI98 on xHCI machines; on EHCI machines only third-party stacks (below). In box: UHCI/OHCI, Full Speed only |
| ME | as 98 SE (ME PortCls version) | as 98 SE | xHCI98; no EHCI in box (below) |
| 2000 | PortCls, KMixer, SysAudio, WDMAUD | usbhub's generic parent; IAD unverified | EHCI from SP4 (or the USB 2.0 update), xHCI98 |
| XP / XP x64 | as 2000 | `usbccgp.sys`; IAD per Microsoft Learn (below) | EHCI in box (XP SP1 onward, unverified per SP), xHCI98 |
| Vista, 7, 8 | PortCls, audio engine (`audiodg`) instead of KMixer | `usbccgp.sys` | EHCI in box; Microsoft's xHCI stack only from Windows 8 (not this driver's concern) |

**EHCI on 98 SE and ME: none in box.** Neither
`tools\win98se-extracted\inf\usb.inf` nor `tools\winme-extracted\ui\usb.inf`
contains the EHCI class code match `CC_0C0320`; Windows 2000 SP4's
`usb.inf` does (`grep` on this host). High Speed on 98 SE / ME therefore
means one of:

- **xHCI98** (`xhci98.sys`, the host controller driver of `2.x`), which
  replaces the port, hub and composite drivers and needs no USB 2.0 stack on
  98 SE or ME (xHCI98 `docs/using/release-notes.md`, "USB stack");
- a **back-ported Microsoft EHCI stack** (NUSB's `usbport.sys`,
  `usbehci.sys`, `usbhub20.sys`, `usbccgp.sys`, seen in
  `tools\nusb36-extracted\`; SweetLow's stack), which this project does not
  ship. Owner, 2026-10-20: such stacks are fully supported; every driver here
  is tested under NUSB's EHCI stack as well as under xHCI98 and Microsoft's
  in-box stacks; or
- a chipset vendor's 9x EHCI driver (NEC, VIA, ALi era). Not inventoried;
  unverified which of these schedule High-Speed isochronous transfers at all.

So the realistic UAC 2 targets on 98 SE are: High-Speed devices on NUSB's
EHCI stack and on xHCI98, both fully supported; and Full-Speed UAC 2 devices
on the in-box UHCI/OHCI stack (section 4.7).

**Whether Windows 2000 SP4's and XP's in-box `usbehci.sys` schedule
High-Speed isochronous transfers, and high-bandwidth endpoints (two or three
transactions per microframe), is unverified.** Microsoft's isochronous
how-to (Microsoft Learn, "How to Transfer Data to USB Isochronous Endpoints",
read 2026-10-20) describes the high-speed rules without naming the first
release that implements them. A playback test on each is part of M1 and M3.

**Interface grouping, per stack.**

- Microsoft's generic parent groups by IAD when any IAD is present, else by
  the legacy audio rule (consecutive class-`01` interfaces whose subclass
  differs from the first's), and gives an IAD function the compatible ids
  `USB\Class_cc&SubClass_ss&Prot_pp` from `bFunctionClass` /
  `bFunctionSubClass` / `bFunctionProtocol` (Microsoft Learn, "Overview of
  Enumeration of Interface Collections on USB Composite Devices", read
  2026-10-20). That page does not say from which release IADs are honoured;
  the IAD page ("USB Interface Association Descriptor") says only that the
  `EF/02/01` device class "alert[s] versions of Windows that don't support
  IADs". The widely cited "XP SP2 and later" is **unverified** here.
- xHCI98's bus driver does the same split itself on every target, 98 SE
  included: IAD first, legacy audio rule only when the configuration has no
  IAD, IAD functions' compatible ids from the IAD (xHCI98
  `docs/contributing/design/13-superspeed-hcd.md` section 10.8 and the
  `BusQueryCompatibleIDs` row of 10.7; `src/xhci_func.c`, `XhciFuncSplit`).
- On 98 SE's in-box stack the X4 enumerated as one HID child at Code 10 with
  no audio devnode (xHCI98 `docs/contributing/test-equipment.md`, citing
  `runs/run-13e.md` Finding Y, taken under the miniport era and 98 SE's own
  hub driver).
- NUSB 3.3's hub (98 SE, EHCI) does not split it either: it gives the
  whole X4 the compatible ids of interface 0 only (`USB\CLASS_03...`, HID)
  and no `USB\COMPOSITE`, so no composite parent loads and no `MI_03`
  exists (guest `Enum` export, `docs/testing/2026-10-22-98se-nusb-legs.md`).

Consequence: the driver binds to **an audio function PDO** where a parent
groups it (`usbccgp` from XP; xHCI98 on every target). Owner, 2026-10-20:
INFs match interface-level ids only, which exist only where the stack split
the device, and never bind a whole device generically. On stacks that do not
split an `EF/02/01` device, the device is served under xHCI98, or a
`USB\VID_vvvv&PID_pppp` entry for it is added on request; bound that way the
driver selects the configuration itself and uses only the audio interfaces
(the device's HID and other functions are then lost while it is bound).
The X4's entry `USB\VID_041E&PID_3278` was added for 98 SE under NUSB
(2026-10-22, from the owner rule that every driver's 98 SE legs pass
under NUSB's EHCI; `1344887`): in the undecorated models only, so XP and
later (`[Models.NTx86]`, `[Models.NTamd64]`) keep the function id. With no
`MI_` number in the PDO's hardware id the driver takes the function from
the first UAC2 audio IAD (`UacWholeFirst`), and its SELECT_CONFIGURATION
lists every interface of the descriptor at alternate 0 (`UacAltZero`),
for a function view and the whole device alike. The owner kept this entry
(2026-10-22) as a device id added on request under the 2026-10-20 rule above.
Open (owner, 2026-10-22): whether the driver plays at the nominal rate
(6.0 samples per microframe) when the feedback endpoint never delivers;
decided after a real-EHCI NUSB leg on the owner's bench.

## 4. USB side

### 4.1 Descriptors parsed

From the configuration descriptor the driver is given (`URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE`,
configuration, full `wTotalLength`), restricted to the interfaces of its
function. Section numbers are the UAC 2.0 specification's ("Audio Device
Class Definition for Audio Devices, Release 2.0", ADC-2), as Linux
`include/linux/usb/audio-v2.h` (GPL-2.0) annotates them:

- IAD (`bDescriptorType` `0x0B`): `bFunctionClass` `01`, `bFunctionSubClass`
  `00`, `bFunctionProtocol` `0x20` (ADC-2 Appendix A).
- AudioControl interface (`01/01/20`), class-specific header (4.7.2), and
  every entity: Clock Source (4.7.2.1), Clock Selector (4.7.2.2), Clock
  Multiplier (4.7.2.3), Input Terminal (4.7.2.4), Output Terminal (4.7.2.5),
  Mixer and Selector Units, Feature Unit (4.7.2.8); Effect, Processing and
  Extension Units parsed as pass-through nodes only.
- AudioStreaming interfaces (`01/02/20`), per alternate setting: the
  class-specific AS interface descriptor (4.9.2: `bTerminalLink`,
  `bmControls`, `bFormatType`, `bmFormats`, `bNrChannels`,
  `bmChannelConfig`), the Type I format descriptor (FMT-2 2.3.1.6:
  `bSubslotSize`, `bBitResolution`), the standard isochronous endpoint
  descriptor(s), and the class-specific isochronous endpoint descriptor
  (4.10.1.2).
- An optional interrupt endpoint on the AudioControl interface (interrupt
  data messages, ADC-2 6.1).
- A MIDIStreaming interface (`01/03`) inside the function: handed to the MIDI
  engine (section 11).

Every descriptor is length-checked before it is read, as Linux
`sound/usb/clock.c` (`validate_clock_source`, `validate_clock_selector`,
`validate_clock_multiplier`) does; a malformed entity disables its path, not
the driver.

### 4.2 Clocking and sample-rate requests

In UAC 2 the rate lives on a clock entity, not on the endpoint as in UAC 1.
For a stream, the driver follows the terminal named by `bTerminalLink` to its
`bCSourceID`, through Clock Selectors and Multipliers, to a Clock Source.
Requests are class requests to the AudioControl interface
(`bmRequestType` `0x21` / `0xA1`, `wIndex` = entity ID << 8 | interface,
`wValue` = control selector << 8 | channel 0):

- Clock Source, `CS_SAM_FREQ_CONTROL` (`0x01`): `RANGE` (`0x02`) returns
  `wNumSubRanges` then `dMIN`, `dMAX`, `dRES` per subrange (4 bytes each);
  `CUR` (`0x01`) reads and sets a 4-byte rate.
- Clock Source, `CS_CLOCK_VALID_CONTROL` (`0x02`): read after every rate
  change.
- Clock Selector, `CX_CLOCK_SELECTOR_CONTROL` (`0x01`), one byte, one-based.

(Constants: Linux `audio-v2.h`, `UAC2_CS_CUR`, `UAC2_CS_RANGE`,
`UAC2_CS_CONTROL_SAM_FREQ`, `UAC2_CS_CONTROL_CLOCK_VALID`,
`UAC2_CX_CLOCK_SELECTOR`, ADC-2 A.14 and A.17.)

Planned policy, informed by Linux `clock.c` (`__uac_clock_find_source`,
`set_sample_rate_v2v3`): use the selector's current position; if that clock
reports invalid, try the selector's other inputs; set the rate with the
streaming interface at alternate 0; read the rate back and check validity;
if the device answers the rate with a STALL or a different value, drop that
rate from the advertised set. Clock Multipliers are treated as pass-through,
as Linux does ("multipliers only act as pass-thru element for now").
The M0 parser currently rejects a multiplier path until its ratio is
implemented, rather than advertising rates with an assumed 1:1 ratio.
One-input selectors are walked without a control request; selectors with
several inputs require an explicit one-based CUR value from the USB layer.
Microsoft's `usbaudio2.sys` never moves a Clock Selector (same Microsoft
Learn page); this driver moves it only when the current position is invalid.

The rate set a format advertises is the clock's `RANGE` set. Where an AS
interface supports the "Valid Alternate Settings" control
(`UAC2_AS_VAL_ALT_SETTINGS`), Linux validates rates per alternate setting only
for known devices, because "performing the rate verification may lead to
unexpected USB bus behavior" (`sound/usb/format.c`,
`validate_sample_rate_table_v2v3`); this driver does the same, through a quirk
table.

### 4.3 Alternate setting selection by format

A format is (`bFormatType` 1, the single `bmFormats` bit, `bSubslotSize`,
`bBitResolution`, `bNrChannels`). For a requested format and rate the driver
picks the alternate setting whose format matches and whose `wMaxPacketSize`
(with the transaction count of bits 12:11 at High Speed) is the smallest that
carries the rate's largest packet, as `usbaudio2.sys` is documented to do
(same page) and as Linux computes it (`sound/usb/endpoint.c`,
`data_ep_set_params`: the maximum frequency taken as nominal + 50 % for
asynchronous endpoints, then capped by `wMaxPacketSize`). Selection is
`URB_FUNCTION_SELECT_INTERFACE` with the alternate setting's pipes.

### 4.4 Isochronous scheduling

Facts the scheduler is built on (Microsoft Learn, "How to Transfer Data to
USB Isochronous Endpoints"; USB 2.0 specification 5.6 and 9.6.6):

- Full Speed: one packet per 1 ms frame; at most 255 packets per URB.
- High Speed: one `USBD_ISO_PACKET_DESCRIPTOR` per serviced microframe;
  the period is 2^(`bInterval`-1) microframes; the packet count in a URB must
  be a multiple of 8, 4, 2 or 1 for `bInterval` 1, 2, 3, 4+; at most 1024
  packets; `StartFrame` counts 1 ms frames, not microframes.
- `USBD_START_ISO_TRANSFER_ASAP` lets the stack place a URB right after the
  previous one; "If there is a delay in sending a subsequent isochronous URB
  ... the driver stack considers some or all packets of that URB to be late",
  and ASAP tracking resets after 1024 idle frames.

The driver's rules:

- **Continuous ASAP streams.** Every data and feedback pipe keeps N URBs
  queued (default 4) with `USBD_START_ISO_TRANSFER_ASAP`, refilled from the
  completion routine at DISPATCH_LEVEL. An explicit `StartFrame`
  (`URB_FUNCTION_GET_CURRENT_FRAME_NUMBER` plus a lead) is used only to start
  a stream on a stack that refuses ASAP on a fresh pipe; none is known.
- **URB size.** One URB carries a whole number of frames: default 4 ms (4
  packets Full Speed; 32 at High Speed `bInterval` 1). The upper bound is
  7 ms at High Speed `bInterval` 1 (56 packets), set today by xHCI98 (4.6),
  and the default stays well under every stack's limit. The bound is a
  parameter, since xHCI98's isochronous ring is to be enlarged (4.6).
- **Packet sizes.** For synchronous and adaptive OUT endpoints, the driver
  generates the nominal cadence (44.1 kHz at Full Speed: nine 44-sample
  packets and one 45, Q16.16 accumulator as Linux's
  `get_usb_full_speed_rate` / `get_usb_high_speed_rate`); for asynchronous
  OUT endpoints, from the feedback value (4.5); for IN endpoints, whatever
  arrives (`IsoPacket[i].Length`).
- **The ring.** Playback copies from the WaveCyclic buffer into the URB's
  own non-paged transfer buffer at refill time; capture copies from the URB
  buffer into the cyclic buffer on completion. Position and the PortCls
  notification (`IPortWaveCyclic::Notify` on the service group) advance from
  completions, not from a timer.
- **Underrun.** If PortCls has not written far enough, the URB carries
  silence and the position does not advance past what was written. A failed
  or late packet is counted, never retried.

### 4.5 Feedback and rate matching

- **Explicit feedback** (USB 2.0 5.12.4.2): an isochronous IN endpoint whose
  `bmAttributes` usage type is 01b, in the same alternate setting as the
  data endpoint. Value: 10.14 in 3 bytes at Full Speed (samples per frame),
  16.16 in 4 bytes at High Speed (samples per microframe). Linux detects
  devices that send the other format or a shifted value and corrects it
  ("Because the Audio Class 1 spec was written before USB 2.0, many high
  speed devices use a wrong interpretation ... we cannot predict what format
  any particular device uses and must detect it automatically",
  `sound/usb/endpoint.c`, `snd_usb_handle_sync_urb`); the driver adapts that
  detection.
- The feedback pipe runs as its own continuous ASAP stream, one packet per
  URB period, from stream start to stream stop. It is not reset between
  reads.
- **Implicit feedback**: only an OUT stream without an explicit feedback
  endpoint may use a specifically paired IN data endpoint (usage type 10b,
  or a documented device quirk). An unrelated IN endpoint with usage type
  10b does not put an OUT stream into implicit mode. The X4 has explicit
  feedback on both OUT interfaces even though capture's IN endpoint is
  marked 10b. With an established pairing, the OUT packet sizes copy the sample counts
  of the capture stream's packets, as Linux does (`sound/usb/implicit.c` for
  the device table; `snd_usb_handle_sync_urb`, implicit branch, for the
  mechanism). This requires capture to run while playback runs, so the
  capture pipe is started silently when only playback is open.
  `usbaudio2.sys` "doesn't support implicit feedback" (same page); this
  driver does, in a later milestone.

Alternate discovery skips non-Type-I settings individually. The X4's
interface 6 alternate 2 is Type III; its PCM alternates remain available.

### 4.6 Under xHCI98

Read in xHCI98's source on 2026-10-20; this driver must live within these
limits and must not depend on anything xHCI98 does that Microsoft's stacks do
not:

- **Packets per URB.** `XHCI_XFER_MAX_ISO_PACKETS` is
  `XHCI_POOL_RING_TRBS - 2` = 62 (`src/xhci.h` lines 927, 3991-3992); a
  larger isochronous URB is refused and counted (`IsoRefusalsTooLarge`,
  `src/hcd_io.c`). Hence the 56-packet ceiling at High Speed above. Owner,
  2026-10-20: the isochronous ring is to be enlarged now, as a separate
  xHCI98 task, so this is today's limit, not one to design around
  permanently.
- **High-Speed packets per frame.** `src/hcd_io.c` line 1316:
  `PacketsPerFrame = (interval <= 3) ? (8 >> interval) : 0`, with `interval`
  = `bInterval` - 1 (`src/xhci_pipe.c`, "6.2.3.6 and Table 6-12"), which
  agrees with Microsoft's rule. High-bandwidth endpoints (bits 12:11 of
  `wMaxPacketSize`) are accepted and become Max Burst
  (`src/xhci_pipe.c`, the `transactions` block; Max ESIT Payload =
  `MaxPacketSize * (MaxBurstSize + 1)`), and a packet above Max ESIT Payload
  is refused (`src/xhci_xfer.c`, the check before `tbc` / `tlbpc`).
- **Frame numbers.** `URB_FUNCTION_GET_CURRENT_FRAME_NUMBER` is answered
  (`src/hcd_urb.c` line 757); the microframe query `QueryBusTimeEx` is
  `STATUS_NOT_SUPPORTED` (`src/hcd_urb.c`, `hcdBusifBusTimeEx`). This driver
  never needs microframe time.
- **Explicit `StartFrame`.** Before `2.3.0.0`, xHCI98 refused an explicit
  `StartFrame` on a controller without Contiguous Frame ID, which silenced an
  asynchronous UAC 1.0 device on Windows 7; since `2.3.0.0` it schedules it
  as closely as the controller allows (xHCI98 `docs/issues/14-async-audio-explicit-startframe-no-cfc.md`;
  `src/xhci_xfer.c`, `XhciXferIsoUsesFrameIds`). Running every stream ASAP,
  as 4.4 does, avoids that path altogether.
- **Feedback endpoint errors.** On the one asynchronous device read through
  xHCI98, about half of all feedback reads ended in USB Transaction Error with
  clean playback (issue 14, section 7). The driver keeps the last good
  feedback value on a failed packet.
- **High-Speed isochronous has not been read through xHCI98.** Every audio
  reading in its release notes is a Full-Speed device (QEMU's `usb-audio`,
  the C-Media, the Sound Blaster Play! units); the X4 never bound
  (`test-equipment.md`). Its "Untested ground" table lists SuperSpeed
  isochronous as untested; High-Speed isochronous is not listed either way.
  So this driver's M1 is also xHCI98's first High-Speed isochronous reading,
  and is run on Microsoft's and NUSB's EHCI stacks too, to tell the two
  apart.
- **0.96 controllers** (NEC/Renesas uPD720200, ASMedia ASM1042): xHCI98
  calls isochronous transfers on them "the least certain"
  (`docs/using/release-notes.md`, "Untested ground", the 0.96 row).

### 4.7 Full-Speed UAC 2 devices

UAC 2 permits Full Speed. Then packets are per 1 ms frame, feedback is
10.14, and Microsoft's stacks accept only `bInterval` 1 for Full-Speed
isochronous endpoints ("Microsoft USB stack only supports bInterval value of
1 for full-speed isochronous endpoints", the isochronous how-to's sample).
These devices are the only UAC 2 devices that can work on a stock 98 SE /
ME on UHCI or OHCI, so the driver treats Full Speed as a first-class case,
not an afterthought.

## 5. OS-facing side

### 5.1 Filters and pins

One adapter devnode registers, per function:

- a **WaveCyclic subdevice per streaming direction** present (render,
  capture), each with one streaming pin (sink for render, source for
  capture) and one bridge pin;
- a **Topology subdevice** built from the AudioControl graph: Feature Units
  become `KSNODETYPE_VOLUME` and `KSNODETYPE_MUTE` nodes, Selector Units
  `KSNODETYPE_MUX`, Mixer Units `KSNODETYPE_SUM`, terminals become bridge
  pins with the category GUID of the terminal type;
- `PcRegisterPhysicalConnection` between each wave bridge pin and the
  Topology pin it feeds.

WDMAUD turns the Topology into mixer lines: "KSPIN_DATAFLOW_OUT topology pins
are exposed as DST lines ... The properties supported on each node that is
encountered in the topology traversal are exposed as controls on the SRC
line" (2000 DDK streaming CHM, `pcdesign_9tmb.htm`). Volume and mute therefore
appear in SndVol32 on 98 SE through XP without anything WinMM-specific in the
driver. The same CHM page names the stack this plugs into: KMixer "provides
system-wide audio mixing, bit-depth conversion, sample rate conversion (SRC),
and channel-to-speaker config (supermix) translation for PCM audio streams";
SysAudio "combines the capabilities of KMixer and SWMidi with the installed
audio adapter drivers"; WDMAud is "the interface between the KS portion and
the legacy (WinMM) portion of the audio stack". DirectSound on these systems
uses the same KMixer path; this driver offers no hardware DirectSound pins
(`KSDATAFORMAT_SPECIFIER_DSOUND` ranges, 2000 DDK `aud-design_1msz.htm`).

### 5.2 Data ranges and formats

Each streaming pin offers one `KSDATARANGE_AUDIO` per device format (channels,
container bits in `MaximumBitsPerSample`, the clock's rate set). Facts from
the DDKs:

- The 98 DDK's `INC_WIN98_MMREG.H` and `INC_WIN98_KSMEDIA.H` and the 2000
  DDK's `mmreg.h` / `ksmedia.h` all define `WAVE_FORMAT_EXTENSIBLE` (`0xFFFE`),
  `WAVEFORMATEXTENSIBLE` and `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT`; the 2000 DDK
  documents "PCM High Bitdepth" and "PCM Multichannel" stream formats as
  `WAVEFORMATEXTENSIBLE` and says `MaximumBitsPerSample` "should be set to
  the sample container size and must be a multiple of 8. For example, if the
  device supports 20-bits of valid audio data in 24-bit containers, the value
  ... must be set to 24" (`aud-design_0qlv.htm`, `aud-design_6sab.htm`,
  `aud-design_4n1v.htm`).
- Headers define the structures; they do not say what 98 SE's or ME's KMixer
  accepts at run time. **Which container sizes (16, 24, 32), which
  `WAVEFORMATEXTENSIBLE` formats, and which rates above 48 kHz KMixer
  negotiates with a device pin on 98 SE, ME and 2000 is unverified.** It is
  settled by reading, on 98 SE and 2000 (ME is not tested and stays
  unverified, section 10), the format SysAudio opens the pin with
  (the driver logs every `NewStream` format) against a device offering only
  24-in-32 and against one offering 16 and 24.
- The 98 DDK's WDM 1.0 notes say "Kmixer does not perform format conversion
  (16 bit/8 bit, mono/stereo) on capture streams using WDM 1.0" (CHM topic
  `audnotes_8t0u.htm`).

Design consequence: **every pin also offers 16-bit stereo PCM at 44.1 and
48 kHz (whichever the clock supports), converted in the driver** when the
device has no 16-bit stereo alternate setting (16 to 24-in-32 on render;
24/32 to 16 and channel selection on capture). That format is what the
oldest KMixer surely handles, so 98 SE works whatever the answer to the
open question; the device's native high-resolution formats are offered as
well and used where the system picks them.

Read on 98 SE (builds 10 to 16, simulated devices,
`docs/testing/2026-10-24-uac2-profiles.md`): KMixer opens a render or
capture pin in the **first** data range it can, whatever the client asks
(a 48 kHz client played at 44.1 kHz while 44.1 kHz was listed first; a
96 kHz 24-bit recording ran the device at 16 kHz 16-bit and KMixer
converted, capture included, against the WDM 1.0 note above). So the
order of the ranges is the format choice: the driver lists each
direction's formats in descriptor order (16-bit first where the device
lists it first), playback rates 48 kHz, 44.1 kHz, then ascending, capture
rates ascending. KMixer accepted WAVEFORMATEXTENSIBLE for 8 channels with
the device's `bmChannelConfig` as `dwChannelMask`; with mask 0 it mixed
silence. The driver's in-driver conversion above is not built: a device
without a 16-bit stereo alternate is offered its own formats only.

### 5.3 Mixer controls

- **Volume**: UAC 2 Feature Unit Volume Control is a signed 16-bit value in
  1/256 dB, `0x8000` meaning silence, with `RANGE` as subranges (ADC-2
  5.2.5.7). KS `KSPROPERTY_AUDIO_VOLUMELEVEL` is a `LONG` in 1/65536 dB
  with stepping reported by basic support (`ksmedia.h`, `KSPROPSETID_Audio`).
  Conversion is times 256, per channel; the first subrange is used, as
  `usbaudio2.sys` does.
- **Mute**: Feature Unit Mute Control, one byte, to `KSPROPERTY_AUDIO_MUTE`.
- Per-channel controls take precedence over the master control where both
  exist (as `usbaudio2.sys`).
- The `bmaControls` pairs (readable / programmable) decide which properties
  exist and whether a node is read-only.
- **Change notifications**: interrupt data messages from the AudioControl
  interrupt endpoint are turned into `KSEVENT_CONTROL_CHANGE` through
  `IPortEvents` (2000 DDK CHM, `audmp-routines_8ek3.htm`). The 98 DDK says
  "Event notification is not supported by WDM 1.0"; on 98 SE the driver
  therefore only refreshes its cached values, and whether 98 SE's PortCls
  has `IPortEvents` is unverified.

## 6. Package, files and names

- Source: `src/audio/uac2/` (all names 8.3). Shared USB-audio code that the
  MIDI driver also needs goes in `src/audio/common/` (section 11).
- Binary: `uac2wdm.sys` (7.3), INF `uac2wdm.inf`. The name avoids
  Microsoft's `usbaudio2.sys` / `usbaudio2.inf`, which an 8.1 / 10 machine
  might also carry.
- Released folders, one complete install each:

| Folder | Systems | Binary |
|---|---|---|
| `free/audio/uac2/w98se-me/` | 98 SE, ME | x86 |
| `free/audio/uac2/w2k-xp/` | 2000, XP | x86 (same binary) |
| `free/audio/uac2/wv-8-x86/` | Vista, 7, 8 32-bit | x86 (same binary) |
| `free/audio/uac2/wxp-x64/` | XP x64 | amd64 |
| `free/audio/uac2/wv-8-x64/` | Vista, 7, 8 64-bit | amd64, signed |

  The x86 folders differ only in their INFs, if one INF cannot serve all.
  Folder names follow the one scheme of `README.md`, "Layout" (Owner,
  2026-10-20), which `usbmidi.md` uses too.

## 7. INF and install per OS

- **Class** `MEDIA` (`{4d36e96c-e325-11ce-bfc1-08002be10318}`), as
  `wdma_usb.inf`; `Include=ks.inf, wdmaudio.inf` and
  `Needs=KS.Registration, WDMAUDIO.Registration` (9x) /
  `WDMAUDIO.Registration.NT` (NT), as the in-box `wdma_usb.inf` does;
  `AddInterface` for `KSCATEGORY_AUDIO`, `KSCATEGORY_RENDER`,
  `KSCATEGORY_CAPTURE` and `KSCATEGORY_TOPOLOGY` with the subdevice
  reference strings; on 9x the `Drivers\wave`, `Drivers\mixer`
  `wdmaud.drv` values.
- **Matches**: `USB\Class_01&SubClass_00&Prot_20` (IAD function) and
  `USB\Class_01&SubClass_01&Prot_20` (legacy-grouped function), plus
  `USB\VID_vvvv&PID_pppp` whole-device entries only where added on request
  (section 3).
- **Ranking against `wdma_usb.inf`.** On every target the in-box INF matches
  `USB\CLASS_01`, the last and least specific compatible id, while this INF
  matches the first. On 98 SE, ME, 2000 and XP that should make this driver
  win, by match position; unverified per system, read in M0/M3 on 98 SE,
  2000 and XP (ME stays unverified, section 10). On Vista and
  later "a rank is the sum of a signature score, a feature score, and an
  identifier score", and "an unknown signature score is represented as
  0xFF000000" (Microsoft Learn, "How Windows Ranks Driver Packages"), so an
  **unsigned package loses to the signed in-box `wdma_usb.inf` on Vista, 7
  and 8 even with a better id match**. Users there pick the driver by hand
  ("Have Disk"). x64 Vista+ needs a signature to load at all. Owner,
  2026-10-20: signing on Vista, 7 and 8 x64 follows the same approach as
  xHCI98 and Stor98, carried out in this repository with its own copy of the
  tooling.

## 8. Build toolchain

- **x86 (98 SE through 8, one binary):** the Windows 2000 DDK (installed at
  `tools\ntddk` on the xHCI98 host: `inc\portcls.h`, `inc\ks.h`,
  `inc\ksmedia.h`, `libfre\i386\portcls.lib`, `ks.lib`) in its Windows 2000
  build environment, with the compiler that DDK expects (xHCI98 keeps MSVC
  6.0 at `tools\MSVC600`). C++ restricted: no exceptions, no RTTI, no
  global constructors. The 9x import rule (import only what 98 SE's
  `ntkern` / `portcls.sys` / `ks.sys` / `usbd.sys` export) is checked by a
  script over the `.sys` import table, as xHCI98 does for its own binary
  (xHCI98 `docs/usb-xhci-info/win98-wdm.md`; the script is copied in, not
  referenced); the 98 SE export lists come from those binaries' export
  tables, read for names and ordinals only (section 2.2).
- **amd64 (XP x64 through 8 x64, one binary):** WDK 7.1 (`tools\WinDDK71`),
  `WNET` amd64 environment (`lib\wnet\amd64\portcls.lib`, `ks.lib`), as
  xHCI98 builds its amd64 package.
- Both install into this repository's git-ignored `tools/`; the package's
  build script names the paths.

## 9. What is adapted, and the NOTICE.md rows it will need

| Planned file | Adapted from | Licence |
|---|---|---|
| `src/audio/uac2/uacdesc.c` (descriptor parsing and validation) | Linux `include/linux/usb/audio-v2.h` (structures, constants), `sound/usb/clock.c` (`validate_clock_*`) | GPL-2.0; GPL-2.0-or-later |
| `src/audio/uac2/uacclock.c` (clock path, rate set, validity) | Linux `sound/usb/clock.c`, `sound/usb/format.c` (`parse_uac2_sample_rate_range`, `validate_sample_rate_table_v2v3`) | GPL-2.0-or-later |
| `src/audio/uac2/uacsync.c` (packet sizing, feedback decoding, implicit feedback) | Linux `sound/usb/endpoint.c` (`snd_usb_handle_sync_urb`, `data_ep_set_params`, `get_usb_*_speed_rate`), `sound/usb/implicit.c` | GPL-2.0-or-later |
| `src/audio/uac2/uacquirk.c` (device quirk table) | Linux `sound/usb/quirks.c` and `implicit.c` tables, as needed | GPL-2.0-or-later |
| `src/audio/uac2/uacmixer.c` (Feature Unit to KS nodes) | Linux `sound/usb/mixer.c` (unit walking, volume range quirks), as needed | GPL-2.0-or-later |

Licences read in the files' SPDX lines on 2026-10-20 (GitHub
`torvalds/linux` master): `audio-v2.h` "GPL-2.0", the `sound/usb/` files
"GPL-2.0-or-later", `include/uapi/linux/usb/audio.h` "GPL-2.0 WITH
Linux-syscall-note". All are compatible with GPL-2.0 distribution.

ReactOS (GPL; files say "See COPYING in the top level directory"):
`drivers/usb/usbaudio/` is a UAC 1.0 AVStream driver (`KSDEVICE_DISPATCH` in
`usbaudio.c`), and `drivers/wdm/audio/backpln/portcls/` reimplements PortCls.
Neither is adapted. The PortCls reimplementation is useful to read when
PortCls behaviour must be understood (how `IPortWaveCyclic` drives
`GetPosition` and `Notify`), since Microsoft's binary may not be read; any
line taken from it later gets its own NOTICE row.

## 10. Test plan

**QEMU cannot carry this driver's main path.** QEMU's `usb-audio`
(`hw/usb/dev-audio.c`, read on GitLab master 2026-10-20) is UAC 1.0
(`bcdADC` 0x0100) and Full Speed only (`.full` descriptors only), with an
optional 8-channel "multi" mode, also UAC 1.0. Owner, 2026-10-20: this
driver does not handle UAC 1 devices in v1, so there is no UAC 1.0 test
mode; QEMU serves only for regression that a UAC 1.0 device still binds
`usbaudio.sys`. QEMU
`usb-host` passthrough of a composite audio device on this Windows host is
recorded as shut by xHCI98 (task 9-V.2: libusb could claim only some of the
interfaces of an audio composite, `docs/contributing/roadmap.md`).

Real hardware (Owner, 2026-10-20: one or two mainstream devices per class;
no Linux gadget rig):

- **A common asynchronous High-Speed UAC 2 DAC** with explicit feedback,
  16- and 24-bit alternate settings, 44.1 to 96 kHz (an XMOS- or
  C-Media CM6631-class unit), with capture if one can be had.
- **The Sound Blaster X4** (held): multiple `bInterval` values, a CDC
  function beside the audio function, capture.

Implicit feedback, Full-Speed UAC 2 and adaptive endpoints are tested only
where those two devices offer them; otherwise they rest on host tests and
stay **unverified** on hardware until a user's device reaches them.

Tested systems (Owner, 2026-10-20, README "Tested by the author"): 98 SE,
2000 SP4, XP SP3 32-bit and 7 x64. Each has an EHCI leg, and an EHCI leg
counts only when EHCI carried the traffic; both devices above are High
Speed, so a run with either on an EHCI port is one.

Hosts: an EHCI machine with 98 SE on NUSB's EHCI stack, and 2000 SP4, XP
and 7 x64 on Microsoft's stack; an xHCI machine with xHCI98 on 98 SE,
2000-in-VM (no 2000 metal, per xHCI98) and XP; a UHCI/OHCI machine on
98 SE for the Full-Speed case. Each stack is the control for the others'
readings.

ME, XP x64, Vista (x86 and x64), 7 x86 and 8 (x86 and x64) are not tested;
their folders ship "Built, not tested", and nothing that only they could
settle blocks a release. The release notes list what stays unverified
there: on ME, the KMixer formats of 5.2, the rank against `wdma_usb.inf`
(section 7) and PortCls over a USB PDO; on XP x64, Vista, 7 x86 and 8,
playback and the audio engine's shared-mode format.

Readings per milestone: audible correctness (a sine and a music file,
compared by a loopback capture where possible), drift over 30 minutes
(asynchronous: no buffer creep; adaptive: no clicks), URB packet error
counts from the driver's own counters, unplug while playing, standby and
resume while idle, and the WinMM mixer controls in SndVol32.

The results go into `README.md`'s "Tested by the author" table.

## 11. Shared driver with USB MIDI (`docs/design/usbmidi.md`)

Recommendation: **one shared USB-audio core, and the UAC 2 driver serves a
MIDIStreaming interface that is part of its own audio function; standalone
USB MIDI devices are the MIDI design's own.**

- The binding unit is the function PDO. Under the legacy grouping rule an
  AudioControl followed by AudioStreaming and MIDIStreaming interfaces is one
  function; under an IAD the MIDIStreaming interface is inside the audio
  IAD when the device puts it there. Whichever driver binds that PDO owns all
  of its interfaces, so a separate MIDI driver cannot bind a MIDIStreaming
  interface inside a UAC 2 function. (Windows 10 instead enumerates "the MIDI
  function as a separate multiple function device with usbaudio.sys", per
  the Microsoft Learn UAC 2 page; that is a Windows 10 parent behaviour, not
  available here.)
- PortCls offers a MIDI port (`IPortMidi`, `IMiniportMidi`,
  `IMiniportMidiStream` in the 2000 DDK `portcls.h`), and one adapter may
  register several subdevices, so the UAC 2 adapter can register a MIDI
  subdevice beside its wave and topology subdevices.
- So: the USB MIDI event packet engine (USB MIDI 1.0 class) and its PortCls
  MIDI miniport live in `src/audio/common/`, linked into both `uac2wdm.sys`
  and the standalone MIDI driver `usbmidi.sys` (`usbmidi.md` section 9 lists
  the files). A MIDI-only device (AudioControl + MIDIStreaming, protocol 0)
  is the MIDI design's.
- The id split is the table in `usbmidi.md` section 9: `uac2wdm.inf` claims
  `USB\Class_01&SubClass_00&Prot_20` and `USB\Class_01&SubClass_01&Prot_20`;
  `usbmidi.inf` claims `USB\Class_01&SubClass_03&Prot_00` (on 98 SE and
  2000 only) and listed hardware ids; neither claims a two-part or bare `USB\CLASS_01` id, so they
  never both match one function. Owner, 2026-10-20: the names
  (`uac2wdm.sys`, `usbmidi.sys`), this split and `src/audio/common/` are
  accepted.

## 12. Risks

1. PortCls on 98 SE / ME may not accept a USB-enumerated PDO or may mishandle
   surprise removal. M0 reads this first; if it fails, the fallback for 9x
   is option 3 (a raw KS filter), at large cost.
2. 98 SE's `portcls.sys` export set is unread until its export table is
   read (section 2.2). A missing import means the driver does not load at
   all on 98 SE.
3. KMixer's run-time format support on 98 SE / ME / 2000 is unknown (5.2);
   mitigated by the driver-side 16-bit stereo format.
4. High-Speed isochronous is unread on xHCI98 and unverified on 2000 SP4 /
   XP EHCI and NUSB's EHCI; a fault found by this driver may be the host's.
5. Vista+ ranking against the signed in-box INF (section 7) makes an
   unsigned package a manual install there.
6. DPC-level copy cost on slow 98-era machines at 24-bit 96 kHz multichannel;
   bounded by keeping per-completion work a plain copy.
7. Device firmware deviations (feedback format, clock validity, rate
   read-back), which Linux carries a quirk table for; this driver will need
   one too.

## 13. Owner decisions and open questions

Owner, 2026-10-20, on the questions this draft raised:

- **Q1, export tables.** The export table of an in-box Microsoft binary
  (98 SE's `portcls.sys`, `ks.sys`, `ntkern.vxd` and the like) may be read
  for names and ordinals only and recorded as such; DDK sample sources may
  not be read (section 2.2, `AGENTS.md`, "Sources").
- **Q2, signing.** Vista, 7 and 8 x64 follow the same approach as xHCI98 and
  Stor98 (section 7).
- **Q3, test hardware.** One or two mainstream devices per class; no Linux
  gadget board (section 10).
- **Q4, whole-device binding.** INFs match interface-level ids only and never
  bind a whole device generically; unsplit devices are served under xHCI98,
  or by VID/PID entries added on request (section 3).
- **Q5, UAC 1 devices.** Not handled by this driver in v1.
- **Q6, folder names.** One scheme across all drivers, `README.md`,
  "Layout" (section 6).
- **Q7, back-ported EHCI stacks.** NUSB's EHCI stack on 98 SE is fully
  supported: every milestone is tested under it as well as under xHCI98 and
  Microsoft's in-box stacks.
- **Shared MIDI code and the id split** with `usbmidi.md`: accepted as
  section 11 states it.

### Open questions

None at present.

## 14. Milestones (smallest useful first)

- **M0, plumbing spike (no USB streaming).** The adapter binds the UAC 2
  function PDO, parses descriptors, registers wave and topology subdevices
  with one fixed format, and appears as a sound device, on XP (Microsoft
  EHCI and xHCI98) and on 98 SE (NUSB's EHCI and xHCI98). Settles: PortCls over a USB PDO on
  98 SE, the import set, the INF rank against `wdma_usb.inf`, unplug with
  the device idle.
- **M1, first sound.** 2-channel 16-bit 48 kHz playback on 32-bit XP to one
  common High-Speed asynchronous DAC (explicit feedback), on Microsoft's EHCI
  and on xHCI98; then on 98 SE under NUSB's EHCI. Clock `SET CUR`, alternate setting selection, continuous
  ASAP URBs, feedback decoding.
- **M2, formats.** 44.1 / 88.2 / 96 kHz, 24-in-32 and 24-in-24 containers,
  Clock Selector handling, adaptive and synchronous endpoints, Full-Speed
  UAC 2 devices, the driver-side 16-bit fallback.
- **M3, the 9x and 2000 targets.** 98 SE on NUSB's EHCI and on xHCI98
  (High Speed) and on UHCI/OHCI (Full Speed); 2000 SP4 on Microsoft's EHCI
  (High Speed) and on xHCI98 in a VM; the KMixer format question of 5.2
  read on each. ME is built, not tested (section 10).
- **M4, capture.** Capture streams and headsets; implicit feedback.
- **M5, mixer.** Feature Unit volume and mute in SndVol32 and the Vista+
  volume slider (read on 7 x64); interrupt notifications where
  `IPortEvents` exists.
- **M6, 64-bit and Vista+.** XP x64 and Vista / 7 / 8 x64 builds, signing
  as xHCI98 and Stor98 sign (section 13), the audio engine's shared-mode
  format choice; tested on 7 x64 on Microsoft's EHCI (High Speed), the
  other folders built, not tested (section 10).
- **M7, MIDI.** The shared MIDI engine inside the audio function (section
  11), with the MIDI design's milestones.

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
  buffer into the cyclic buffer on completion. The PortCls notification
  (`IPortWaveCyclic::Notify` on the service group) advances from
  completions, not from a timer. The playback position is how far the
  refills have read the cyclic buffer, the capture position how far
  completions have written it (section 17).
- **Underrun.** The driver cannot see how far PortCls has written: the
  position tells PortCls which bytes the next refill takes, and PortCls
  puts data or silence there. A failed or late packet is counted, never
  retried.

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
  `usbaudio2.sys` does. The top offered is the highest step at or below
  0 dB (section 20, build 25).
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
| `src/audio/uac2/uacdesc.c` (descriptor parsing and validation) | Linux `sound/usb/clock.c` (`validate_clock_*`); structures and constants from the UAC 2.0 specification, not from `include/linux/usb/audio-v2.h` (GPL-2.0, facts only) | GPL-2.0-or-later |
| `src/audio/uac2/uacclock.c` (clock path, rate set, validity) | Linux `sound/usb/clock.c`, `sound/usb/format.c` (`parse_uac2_sample_rate_range`, `validate_sample_rate_table_v2v3`) | GPL-2.0-or-later |
| `src/audio/uac2/uacsync.c` (packet sizing, feedback decoding, implicit feedback) | Linux `sound/usb/endpoint.c` (`snd_usb_handle_sync_urb`, `data_ep_set_params`, `get_usb_*_speed_rate`), `sound/usb/implicit.c` | GPL-2.0-or-later |
| `src/audio/uac2/uacquirk.c` (device quirk table) | Linux `sound/usb/quirks.c` and `implicit.c` tables, as needed | GPL-2.0-or-later |
| `src/audio/uac2/uacmixer.c` (Feature Unit to KS nodes) | Linux `sound/usb/mixer.c` (unit walking, volume range quirks), as needed | GPL-2.0-or-later |

Licences read in the files' SPDX lines on 2026-10-20 (GitHub
`torvalds/linux` master): `audio-v2.h` "GPL-2.0", the `sound/usb/` files
"GPL-2.0-or-later", `include/uapi/linux/usb/audio.h` "GPL-2.0 WITH
Linux-syscall-note". Owner, 2026-10-25: the repository is
GPL-2.0-or-later, so the two headers under GPL-2.0 only (the uapi one
with the syscall note) are not adapted; the structures and constants they
annotate are written from the UAC 2.0 specification. The `sound/usb/`
files are GPL-2.0-or-later.

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

## 15. On-board codecs with several jacks: the Realtek ALC4080

Status: design, 2026-10-29, reviewed and revised, before any descriptor
of a real ALC4080 has been read (xHCI98's roadmap task 45.25; the
real-board reading is its 45-E.13). Every point the device's own
descriptors decide is marked as the capture's, and the design below
holds only where the capture shows the layout it assumes.

### 15.1 What is known

Facts only, from Linux `sound/usb` at commit `c339f401`
(`docs/references/linux-c339f401/`, files under GPL-2.0-or-later, read for
facts; nothing adapted):

- `mixer_maps.c` renames three entities of the MSI MPG X570S Carbon Max
  WiFi "with ALC4080": unit 29 "Speaker Playback", unit 30 "Front
  Headphone Playback" and unit 32 "IEC958 Playback", and applies that map
  to `0DB0:419C` (that board), `0DB0:A073` (MSI MAG X570S Torpedo Max) and
  `0DB0:CC78` (MSI MAG B850M Mortar WiFi): the codec appears under the
  board maker's vendor and product ids, not Realtek's. Three named units
  do not say how many streaming interfaces or terminals feed them: three
  paths may share one streaming terminal, or sit in alternates of one
  interface. The capture says.
- For those ids the inspected `card.c`, `quirks.c` and `mixer_quirks.c`
  carry no start-up request, quirk flag or mixer quirk; Linux runs the
  device with its class driver. That does not show that every ALC4080
  board needs none. `mixer_quirks.c`'s Realtek HD Audio verb requests (62,
  88 and 89 with `wValue` 0x38, and 72 with 0) belong to the ALC4020 of two docks, a USB
  Audio 1.0 device, where they read jacks and also write the codec's
  power state and manual mode; nothing in those files sends them to an
  ALC4080.
- Not known here, and the capture's (15.5): whether the ALC4080 is USB
  Audio 2.0 (Linux's class driver takes 1.0 and 2.0 alike), the
  streaming interfaces, terminals, alternates, sync types and feedback,
  clock entities and rates, the capture paths, which terminal is which
  jack, and the ids of other makers' boards.

### 15.1a The capture, and other boards (2026-10-29)

The owner's Linux capture of one board's ALC4080 arrived (`0B05:1B9B`,
`bcdDevice` 0013, High Speed, one configuration): interface 0
AudioControl (`01/01/20`), interfaces 1 to 6 AudioStreaming (`01/02/20`),
interface 7 HID. By `bTerminalLink` and the terminal types: playback IF4
to Output Terminal 21, speaker (0x0301); IF5 to OT 22, headphones
(0x0302, to 384 kHz); IF6 to OT 24, S/PDIF (0x0605); capture IF1 from
Input Terminal 9, analog connector (0x0601), IF2 from IT 11, line
connector (0x0603), IF3 from IT 12, microphone (0x0201). Linux lists
every playback stream ADAPTIVE. Its Feature Units 29, 30 and 32 feed
OT 21, 22 and 24 - the unit ids Linux's `sound/usb/mixer_maps.c` (facts
only) names for three other boards' ALC4080 (`0DB0:419C`, `A073`,
`CC78`), so the layout looks like one firmware template; but each board
maker gives its own ids, and its jacks, inputs, rates and HID interface
may differ. So `uac2` serves the codec from its descriptors - several
AudioStreaming interfaces, named by terminal type - and a board with two
outputs or no S/PDIF works the same way; a vid:pid table is only for a
quirk, never the reason the codec works (15.3 item 7 is revised by
this). The jack names come from the terminal types; no listening check
is owed before the real-board reading (xHCI98 45-E.13).

Before `815957c` `uac2` did not start on this codec at all: its 68
streaming alternates (one of them skipped) passed the parser's 64
(`UAC_TOO_MANY_ALTS`). With `UAC_MAX_ALTS` 128 and each terminal's own
formats (`UacPlanFormatsFor`), the capture replayed by `uacemu.py`
(profile `alc4080cap`) on XP SP3 x86 under xHCI98 bound `uac2` from the
Found New Hardware wizard and played to the first playback terminal
(13, the speaker, IF4: 2 channels 16-bit 48 kHz, adaptive), 2026-10-29.
Every terminal on its own clock source (playback 5, 6, 8; capture 1, 3,
4) means the clock-domain rule of 15.3 item 5 has nothing to share on
this board. Recording and the other outputs wait for the endpoints of
15.3.

### 15.2 What `uac2` does with it today

- Binding: if the function is USB Audio 2.0 and its PDO offers
  `USB\Class_01&SubClass_00&Prot_20` (or the AudioControl interface's
  id), by class, as section 3 says; a device whose function the parent
  does not present that way needs the whole-device path of section 3
  (`UacWholeFirst` takes its first audio function only). A runtime table
  cannot make the driver bind.
- Streams: one wave filter with one render and one capture pin.
  `UacPlanTerminal` takes the terminal of the first runnable streaming
  alternate of each direction in descriptor order, and the stream picks
  only alternates linked to it (`UacPlanPick`). If the ALC4080 has one
  streaming interface per jack, only the first in descriptor order plays.
- Volume and mute: `uacvol.c` walks from a physical Output Terminal back
  to the playback streaming terminal (render only); one devnode value,
  `Uac2Level`, keeps the level.

### 15.3 The design (applies only to a device in the table, 15.3.7)

1. **One owner of the function.** The adapter, not each wave filter,
   opens the function once (`UacUsbOpen`: the configuration, the
   descriptor model, the transport, the clock and selector claims and
   the per-interface alternates) and holds it across power and removal;
   every filter refers to it. Today each `Wave` opens its own, which
   with several filters would select the configuration under another's
   pipes.
2. **Endpoints are streaming interfaces.** An endpoint is a streaming
   interface whose runnable alternates all link one terminal, and no
   other interface's alternates link that terminal. If the capture shows
   anything else (two terminals in one interface's alternates, one
   terminal behind two interfaces), the device keeps today's one
   endpoint per direction. `UacPlanTerminals` lists a direction's
   terminals in that order; the interface check is step 2's.
3. **Indexed duplex pairs.** Pair k is the k-th playback endpoint and the
   k-th capture endpoint (either may be absent), as one wave filter and
   one topology filter, k = 1 to 4. Pair 1 keeps today's reference
   strings "Wave" and "Topology", so a one-endpoint device registers
   exactly as now; pairs 2 to 4 are "Wave2"/"Topology2" to
   "Wave4"/"Topology4", unique on the adapter for its life.
   `PcAddAdapterDevice` is given 8 subdevices (it is 2 today); endpoints
   past the fourth of a direction are not served. The INF provisions the
   interfaces of all eight reference strings; a string the driver does
   not register stays not enabled.
4. **What each pair shows.** Its wave filter offers only its endpoints'
   formats (`UacPlanFormats` per terminal). Its topology filter's bridge
   pins take the category of the physical terminal the endpoint reaches
   (Terminal Types 2.0 to `KSNODETYPE_*`: 0x0301 speaker, 0x0302
   headphones, 0x0603 line connector, 0x0605 S/PDIF interface, 0x0201
   microphone; anything else, or several reachable terminals, the
   generic speaker or microphone, as today), so Vista's endpoint builder
   names the jacks; on 98 SE to XP the interface's `FriendlyName` carries
   the same word, two endpoints of one type numbered.
5. **One clock, claimed at open.** Endpoints whose terminals reach the
   same clock entities (`UacClockPath`: sources, selectors and
   multipliers) form a domain. A stream reserves its rate and selector
   positions in the domain when it is created (`NewStream`), and a second
   stream asking another rate or another position is refused there; the
   reservation is dropped when the stream closes or fails, and taken
   again at RUN under `UAC_DEV`'s lock, so a stream opened before a
   reset can still fail at RUN. For a device in the table both
   directions list 48 kHz first, so KMixer's usual open of a duplex pair
   asks one rate (today capture lists its rates ascending). The rule
   holds on every target; Vista's shared-mode format is one more opener.
6. **Volume and mute per endpoint.** The playback walk as now from each
   playback endpoint; a capture walk from each capture streaming
   terminal back to its physical Input Terminal (new); the level kept per
   endpoint, keyed by direction and streaming terminal (`Uac2LevelP<id>`
   and `Uac2LevelC<id>`, the id in hex), and shared where two endpoints
   reach one Feature Unit. Today's `Uac2Level` (playback only) is read
   once for the first playback endpoint when its own value is absent, and
   then written under the new name.
7. **Behind a device table.** Several endpoints change what every such
   device shows - the Sound Blaster X4 already plays to two terminals, 3
   and the S/PDIF 17 (`desctest`), and its legs ran with one - so the
   driver serves every endpoint only for the ids of a table
   (`UacPlanQuirks`, flag `UAC_QUIRK_ENDPOINTS`): the three MSI ids
   above, and the owner's board once the capture names its ids. The key
   is the device's idVendor and idProduct; the function is found from
   the PDO's descriptors as today, never by an assumed `MI_nn`; a later
   entry may also name a configuration, an interface and a `bcdDevice`.
   Every other device keeps one endpoint per direction. A second audio
   function of the same device (a front headphone presented as its own
   function) is its own devnode and adapter; a separate USB device needs
   its own capture.
8. **No vendor request** at start or at a stream's start, until a capture
   shows one is needed.

### 15.3a As built (step 2b, 2026-10-29)

- Items 1 to 4 as designed: pair 0 owns the function, later pairs borrow
  it; `UacPlanEndpoints` applies item 2; reference strings `Wave` to
  `Wave4`; bridge categories from `UacVolJack` (item 4). The interfaces'
  `FriendlyName` is the INF's (`UAC2 USB audio (experiment) 2` to `4`);
  a name from the terminal type is still owed.
- Item 5, narrowed: until a clock reservation at stream open exists,
  a device gets several pairs only when every endpoint reads a clock
  source of its own (`UacClock`, no selector choice); otherwise it keeps
  one pair, as before. The ALC4080 capture qualifies (each terminal its
  own source); the `multi` profile (two outputs on one clock) does not.
  Likewise when two playback paths meet in the Feature Unit either would
  drive for its volume or mute, and (item 2, over both directions) when
  an interface's alternates link two terminals of either direction.
- Item 6, narrowed: playback levels per pair (`Uac2Level` for pair 0,
  `Uac2LevelPnn` after); a Feature Unit an earlier pair drives is left
  to software in a later one. The capture walk is still owed.
- Item 7 is gone (15.1a): pairs come from the descriptors; the table
  stays for quirks only.
- A later pair that fails to come up fails the start as a whole (a
  registered subdevice cannot be withdrawn on every target).

**The leg (2026-10-30, `b9f6ae3`, `uac2wdm.sys` x86 `3a78670d...`).** XP
SP3 x86 under xHCI98 (`3.4.0.0` tree), the capture replayed
(`alc4080cap`), `uac2` installed from Found New Hardware: Sounds and Audio
Devices lists three playback devices ("UAC2 USB audio (experiment)", "...
2", "... 3") and three recording ones. With each made the default,
`sndrec32` played through IF4 (speaker), IF5 (headphones) and IF6
(S/PDIF) in turn, 2 channels 16-bit 48 kHz, signal at the simulator;
`UACWAV rec 2 16 48000 3` recorded from the first input (IF1, analog
connector): 8 of 8 buffers, the 1 kHz tone. Not read yet: two outputs at
once, the second and third inputs, the bridge categories as Vista and 7
show them, 98 SE and 7 x64, and the Sound Blaster X4 layout, which now
gets two playback pairs (analog and S/PDIF).

**The Sound Blaster X4 gate (2026-10-30).** Under build 18 the X4 gets
two playback pairs (analog terminal 3, S/PDIF terminal 17). XP SP3 x86
under xHCI98, readings with `MIXLIST` (a test program listing each
waveOut's mixer and every mixer line) and `UACWAV`:

- The cross-pairing first reported was not one: each waveOut maps to its
  own pair's mixer. What failed was the S/PDIF pair's topology bridge,
  `KSNODETYPE_SPDIF_INTERFACE` on an output: WDMAud made its mixer
  destination a `WAVEIN` line with no waveOut target, so that output had
  no speaker line (`mix` and `ramp` failed, the mixer looked foreign).
  Fix: a playback bridge is `KSNODETYPE_SPEAKER`, or
  `KSNODETYPE_HEADPHONES` for a headphone terminal (0x0302), only; the
  capture bridges keep their jack categories. After it each pair's mixer
  has a speaker (or headphones) destination with the Wave source and its
  waveOut target.
- The S/PDIF pair's slow stream after a replug (about 400 packets per
  second against about 2000, feedback normal at about 1000 per second)
  did not recur on a fresh overlay: five replugs, every stream at about
  2000 packets per second and 8 of 8 buffers. It was read once, on the
  overlay that had first had the mixer failures, and is not attributed;
  nothing in the driver was changed for it.
- Gate, simulated X4 (`x4full`), the fixed build: two playback pairs;
  `play` 8 of 8 and `ramp` (volume and mute writes to Feature Unit 1) on
  pair 0; `play` 8 of 8 and `ramp` (writes to Feature Unit 14) on the
  S/PDIF pair (`UACWAV_PAIR=1`); two hot replugs, each followed by `play`
  8 of 8 and `ramp` on pair 0, the waveOut order stable.
- The real X4 passed through from the host (QEMU `usb-host`, the owner's
  device): two playback pairs, each mixer paired with its waveOut; `ramp`
  (8 of 8 longer buffers, volume and mute) on both pairs; a 3-second
  `play` completed 6 of 8 buffers on both pairs, and the 0.1.4 driver
  does the same on that rig (6 of 8, twice): not new with this build;
  its cause (the passthrough, or a defect both builds share) is not read.
  One reading since (2026-10-30): the same real X4 through the same
  passthrough under XP's own `usbehci.sys` (no xHCI98), this build,
  completed the same 3-second `play` 8 of 8 in about 3.3 s, three runs
  on both pairs. The short play is so far seen only on the virtual xHCI
  path (xHCI98 on QEMU's `qemu-xhci`, with the passthrough), where the
  simulated X4 also takes about 11.5 s for the same command; its cause
  is not read. During one run the passthrough re-enumerated the
  device once.
- The ALC4080 capture replayed (`alc4080cap`) with the fixed build: three
  playback pairs (Speaker, Headphones, Speaker for S/PDIF) and three
  inputs (Analog Connector, Line, Microphone); on each pair `play`, `rec`
  (the 1 kHz tone) and `ramp` (Feature Units 29, 30 and 32) passed.
- Outputs at once (2026-10-30), the same capture and build, XP under
  xHCI98: the three pairs started together (`UACWAV_PAIR` 0, 1 and 2, a
  5-second `play` each, 2 channels, 16 bits, 48 kHz). All three opened and
  streamed at the same time on their own endpoints (interfaces 4, 5 and
  6), and the simulator read the tone on each (-20 dBFS). Each completed
  4 of 8 buffers in `UACWAV`'s 15-second wait, as one pair played alone
  just after did (4 of 8). The simulator counted 69,418 packets over
  the three streams in that time and 24,824 for the one, about 2.8 times;
  it counts all endpoints together, so how the 69,418 split between the
  streams, and whether they slowed one another at all, is not read. Each
  ran at about the slow rate the virtual xHCI path shows for one stream
  (xHCI98 roadmap 45.39).

### 15.4 The simulated device

`uacemu.py`'s profile `alc4080` is an assumed layout, not a capture: High
Speed, one programmable clock (44.1 and 48 kHz families to 192 kHz), three
playback streaming interfaces - 8-channel speaker (Output Terminal 0x0301
through Feature Unit 29), 2-channel front headphone (0x0302 through 30),
2-channel S/PDIF (0x0605 through 32), as Linux's map numbers them - and
two capture interfaces, line in (0x0603) and microphone (0x0201), one
terminal each; asynchronous playback with explicit feedback, 16- and
24-bit alternates, ids `FFFF:0208` as every built profile (`--vid 0db0
--pid 419c` gives a board id the table lists). The capture replaces it
with the recorded configuration.

### 15.5 What the capture must answer

The owner's capture script (xHCI98 task 45.25) packs, for every USB audio
device, `lsusb -v`, the sysfs `descriptors` bytes and identity, and for
every USB card `stream*`, `usbmixer`, `usbid` and the `amixer` controls.
From it:

- `descriptors-*.bin` holds the device descriptor and then every
  configuration: each is cut out by its own `wTotalLength` and kept as
  `testdata/<board>/config<value>.bin` beside the original dump, and read
  with the parser in a test of its own (`desctest` is the X4's): the
  class version, functions and their interfaces, streaming interfaces,
  terminals and which alternates link them, sync types, feedback (and
  implicit feedback pairing), clock sources, selectors and multipliers,
  units on each path (selectors, extension and vendor units included),
  and an AudioControl interrupt endpoint;
- `lsusb -v`: the same, decoded, as a check on the cut;
- `stream*`: the formats and rates Linux lists per endpoint (offered,
  not shown to work at each rate);
- `usbmixer` and `amixer`: the units Linux kept, their ids against the
  map above, the selectors' current positions and the controls' ranges;
- `card*-usbid` against the descriptors: which card is which function or
  device, when the board has two;
- the owner's note of which jack is which, and whether two jacks play at
  once (for 15.3's endpoints and names).

Nothing in the archive records the control transfers Linux made, or a
clock change from outside; an AudioControl interrupt endpoint, if
present, is noted and not served at first.

### 15.6 Steps

1. Now: `UacPlanQuirks`, `UacPlanTerminals` and the `alc4080` profile,
   with host tests. Nothing the driver does changes yet.
2. After the capture: its fixture and test; 15.3 checked against it and
   changed where it differs; the adapter-owned function, the pairs, the
   clock reservation, the capture walk and the per-endpoint levels,
   behind the table.
3. Legs with the simulated device (and the capture's layout once it is a
   profile) on 98 SE under NUSB's EHCI and under xHCI98, on XP and on 7
   x64 under xHCI98 and Windows' own stack: each endpoint alone, two
   playbacks, two captures, duplex, conflicting rates opened in both
   orders, one stopped while another runs, pause and restart, levels
   after a restart, unplug and standby with several streams running.
4. 45-E.13: the real board.

## 16. Keeping Windows' USB Audio 1.0 driver off a UAC 2.0 function: a MEDIA class co-installer

Status: step 1 (the gating probe) read on XP SP3 x86, 2026-10-29; the
co-installer itself not designed yet (owner, 2026-10-29; xHCI98 task
45.24's follow-up).

### 16.1 The problem

Under Windows' own USB stack on 2000 to 8.1 a UAC 2.0 function offers the
bare `USB\Class_01` compatible id, and Windows' signed `wdma_usb.inf`
(`usbaudio.sys`, USB Audio 1.0 only) outranks the unsigned `uac2wdm.inf`.
Once `usbaudio` holds the function no change of that devnode completes
until a restart (`docs/testing/2026-10-29-uac2-xp-hang.md`). Changing one
devnode does not help the next: a device without a serial number on
another port, a second device, or a devnode installed again all get
`usbaudio` again. xHCI98 from 3.4.0.0 drops the bare id itself (its
design record 13 section 10.7.2); this section is for Windows' own stack.

### 16.2 The goal

A MEDIA class co-installer DLL shipped with `uac2`: for a function whose
compatible ids carry exactly `USB\Class_01&SubClass_0x&Prot_20`, it marks
`usbaudio`'s driver node `DNF_BAD_DRIVER`, so the default handler selects
`uac2` on every new install. `Prot_30` (USB Audio 3.0) is left out of the
rule, an owner question. Registration: `HKLM\SYSTEM\CurrentControlSet\
Control\CoDeviceInstallers`, value `{4D36E96C-E325-11CE-BFC1-08002BE10318}`
(`REG_MULTI_SZ`, the entry appended, others kept), by `uac2`'s setup step;
`usbaudio`'s node identified by its INF and section (and service), never
by its description or provider; USB Audio 1.0, MIDI, composite parents
and other MEDIA devices left alone; the uninstall removes only this
entry.

### 16.3 Step 1: the probe (`uacci.c`, test-only)

`uacci.c` logs every DIF code the class co-installer receives, before and
after the default handler, with the device's instance id and class GUID,
its compatible ids, every node of the compatible driver list (INF,
section, hardware id, rank, `DNF_` flags) and the selected node, to
`%windir%\UACCI.LOG`. It changes nothing. (The XP reading below ran
the first build, which also asked for post-processing of
`DIF_ALLOW_INSTALL` and wrote each record in two writes; the probe now
does neither, and logs a driver node's detail error.)

**XP SP3 x86, 2026-10-29** (q35 guest, Windows' own `usbehci.sys`, no
xHCI98; `uac2` 0.1.4.0's `w2k-xp` files in `C:\UAC2`, added to
`DevicePath`; `UACCI1.DLL` registered as the only MEDIA class co-installer,
none being there before; a never-seen simulated UAC 2.0 device,
`uacemu.py` `hs24`, `FFFF:0201`, on the EHCI port):

- The co-installer is loaded in the device installer's process and
  called at the first plug at `DIF_SELECTBESTCOMPATDRV`, before and after
  the default handler. At that point the device's class GUID is already
  MEDIA, its compatible ids are `USB\Class_01&SubClass_00&Prot_20`,
  `USB\Class_01&SubClass_00`, `USB\Class_01`, and the compatible list
  holds both nodes: `wdma_usb.inf`, section `USBAudio`, hardware id
  `usb\class_01`, rank `0x2002`, flags `0x2244`; and `uac2wdm.inf`,
  section `UAC2`, rank `0xA000` (unsigned), flags `0x240`. After the
  default handler `wdma_usb.inf`'s node is selected.
- Then `DIF_ALLOW_INSTALL` (the default handler's status `0xE000020E`),
  `DIF_INSTALLDEVICEFILES`, `DIF_REGISTER_COINSTALLERS`,
  `DIF_INSTALLINTERFACES` and `DIF_INSTALLDEVICE`, each with `usbaudio`'s
  node selected.
- With `uac2` a candidate the Found New Hardware wizard was shown (the
  earlier legs without it installed `usbaudio` silently).
- The first install of `usbaudio` itself never finished: the log has the
  `DIF_INSTALLDEVICE` call before the default handler and none after it,
  for more than five minutes, the wizard at "USBAUDIO.sys To
  C:\WINDOWS\system32\drivers". Where in that request it waits (the
  class installer, the file copy, the default handler or the driver's
  start) was not read; `usbaudio`'s start on a UAC 2.0 function is the
  hypothesis, pending a stack.

**The gate: passed on XP.** A first plug reaches the class co-installer at
`DIF_SELECTBESTCOMPATDRV` with both nodes in the list, which is where the
design's `DNF_BAD_DRIVER` acts; `DIF_ALLOW_INSTALL` also comes before any
`usbaudio` code runs. The real co-installer is designed next, and the same
reading is owed on 98 SE, ME, 2000, Vista, 7, 8 and 8.1.

### 16.4 The co-installer (design)

One source, `uacci.c`, builds two DLLs: `UACCI1.DLL` (the co-installer,
shipped with `uac2` in its `w2k-xp`, `wxp-x64`, `wv-8-x86` and `wv-8-x64`
folders) and, with `UACCI_PROBE` defined, the probe of 16.3 (test-only).
98 SE and ME have no co-installers; 16.5 records what they do instead.

**The rule.** A device is `uac2`'s case when its compatible ids
(`SPDRP_COMPATIBLEIDS`) hold, case-insensitively, a string of exactly the
form `USB\Class_01&SubClass_0h&Prot_20` (`h` one hex digit). Nothing
else is read from the device. USB Audio 1.0 functions (`Prot_00`), MIDI
(subclass 03, protocol 00), composite parents (no class 01 id) and every
other MEDIA device fail the rule and are left alone. `Prot_30` is not in
the rule (an owner question).

**Windows' node.** A compatible driver node is `usbaudio`'s when the base
name of its INF (`SP_DRVINFO_DETAIL_DATA.InfFileName`, any directory, the
driver store's included) is `wdma_usb.inf` and the install section it
would run (`SetupDiGetActualSectionToInstall` on that INF and the node's
`SectionName`) has a `.Services` section whose `AddService` names the
service `usbaudio`. Never by description or provider.

**What it does.**

1. `DIF_SELECTBESTCOMPATDRV`, before the default handler: for a device
   under the rule, every compatible node that is `usbaudio`'s gets
   `DNF_BAD_DRIVER` (`SetupDiSetDriverInstallParams`); the default handler
   then picks the best remaining node, `uac2` where its INF is on the
   machine. If none remains, the default handler fails with
   `ERROR_NO_COMPAT_DRIVERS` and the device is left without a driver
   (code 28), which is better than the wait.
2. `DIF_ALLOW_INSTALL`: for a device under the rule whose selected node is
   `usbaudio`'s (a path that skipped step 1, or a user's own pick in the
   Update Driver list), it returns `ERROR_DI_DONT_INSTALL`, so nothing of
   `usbaudio` is installed on it.
3. Every other request, and every device outside the rule: `NO_ERROR`,
   nothing touched, no post-processing.
4. Each decision (rule met, node marked, install refused) is one line in
   `%windir%\UACCI.LOG`, bounded and serialised as the probe's; the file
   grows only on audio installs. An error before the rule is decided (the
   ids cannot be read) leaves the device as Windows would have it. Once
   the rule is met, an error (a node whose INF or section cannot be read,
   a mark that fails) fails the request: step 1 returns the error, so the
   selection fails and the device stays without a driver until the next
   plug or Update Driver retries; step 2 refuses the install when the
   selected node, its INF name or (for a `wdma_usb.inf` node) its
   services cannot be read. A node from another INF whose details read
   is never refused. Both are logged. A test build injects each failure
   (the details of the selected node, the section of a `wdma_usb.inf`
   node) and the legs check that `usbaudio` is not installed.

**Registration.** `UACCI.INF`, a non-device INF beside `uac2wdm.inf`, run
by `uac2`'s setup step (right-click Install, or `rundll32
setupapi,InstallHinfSection DefaultInstall 132 <path>\UACCI.INF`; xHCI98's
installer when it stages `uac2`), never by `uac2` winning a device
install:

- `[DefaultInstall]` (and `.NTamd64`): `UACCI1.DLL` to `%11%`; `AddReg`
  `HKLM,SYSTEM\CurrentControlSet\Control\CoDeviceInstallers,`
  `{4D36E96C-E325-11CE-BFC1-08002BE10318},0x00010008,"UACCI1.DLL,UacCoInstaller"`
  (`FLG_ADDREG_TYPE_MULTI_SZ | FLG_ADDREG_APPEND`: appended once,
  other entries kept);
- The file name carries the co-installer's version (`UACCI1.DLL` now;
  the next is `UACCI2.DLL`, 8.3): a loaded DLL is never replaced in
  place. An upgrade runs the new DLL's `rundll32 %11%\UACCI2.DLL,UacInstall`
  after its `UACCI.INF` copied the file: it removes every `UACCIn.DLL`
  string other than its own, appends its own once, and reads the value
  back; the old file stays in the system folder. (The first version's INF
  appends by `AddReg`; `UacInstall` exists from it on, so later versions
  need no `AddReg`.)
- The uninstall is the DLL's own helper, `rundll32 %11%\UACCI1.DLL,UacUninstall`,
  on every system: it reads the value, removes every string naming a
  `UACCIn.DLL` and nothing else, and writes the rest back (removing the
  value only when nothing else was in it). `DelReg`'s
  `FLG_DELREG_MULTI_SZ_DELSTRING` is not used: before XP it is not
  documented, and on 2000 a plain `DelReg` of the value would drop the
  other installers' entries too. xHCI98's uninstall and the README call
  the helper; its file stays (a reinstall before a restart reuses it).
- The co-installer only steers a device to `uac2` when `uac2wdm.inf` is
  where Windows looks (the driver store, or `DevicePath`): the setup step
  stages it too (xHCI98's installer already does, `SetupCopyOEMInf`;
  standalone, the user's Have Disk once, or the README's step).

**What the legs read, per system** (2000, XP, XP x64, Vista, 7, 8, 8.1,
x86 and x64 where the system has it): a never-seen simulated UAC 2.0
device on Windows' own USB 2.0 stack, `uac2` staged, `UACCI.INF`
installed: the log's decisions, `uac2wdm` running, playback; the same
device plugged into a second port and a second device; a USB Audio 1.0
device still on `usbaudio`; the uninstall leaving the other class
co-installer entries. Vista to 8.1: whether an unsigned class
co-installer DLL loads at all, and the prompts.

### 16.5 The legs of the co-installer (2026-10-29)

q35 guests, stock QEMU 11.1.0, Windows' own USB 2.0 stack (`ich9-usb-ehci1`
with a UHCI companion; no xHCI98), `uac2` 0.1.4.0 put where Windows looks
(`DevicePath` on 2000 and XP, `pnputil -a` on 7), `UACCI.INF` installed by
`InstallHinfSection DefaultInstall` (the co-installer's test build of
`42ff9d2`, `UACCI1.DLL` x86), a never-seen simulated UAC 2.0 device
(`uacemu.py` `hs24`, `FFFF:0201`). Evidence:
`E:\xhci98-vm\p45\w45-uac2\` (`shots\r1-*`, `gw2k-*`, `gw7-*`; `ev\`).

| System | Result |
|---|---|
| XP SP3 x86 | `UACCI.INF` asked once ("has not passed Windows Logo testing"), then registered as the only MEDIA class entry. First plug: the log reads `select ... usbaudio nodes marked=1`; the wizard offered "UAC2 USB audio (experiment)" by itself, `uac2wdm` RUNNING, `usbaudio` never installed (`sc query`: 1060). A second device (`FFFF:0299`, another port): the same. `UacUninstall` and `UacInstall` with a foreign entry (`Other.dll,X`) in the value: install appends once, uninstall leaves `Other.dll,X` alone; with only ours, uninstall removes the value. Playback through `uac2` was read on the earlier XP legs, not again here |
| XP SP3 x86, UAC 1.0 control | The XP row's guest (`UACCI1.DLL` registered, `uac2` where Windows looks), Windows' EHCI with a UHCI companion, QEMU's own `usb-audio` (a UAC 1.0 Full Speed device) plugged: the wizard installed "USB Audio Device", `usbaudio` RUNNING; `UACCI.LOG` has no line for it (the co-installer logs a device only when its rule matches or its ids cannot be read), so a UAC 1.0 function keeps Windows' driver |
| 2000 SP4 | `UACCI.INF` asked once (Digital Signature Not Found, Yes). First plug: `usbaudio nodes marked=1`, the wizard named the device "UAC2 USB audio (experiment)" and installed `uac2` (2000's own hub does not split the IAD device: `uac2` takes the whole device, as section 3 says). Control, the co-installer removed by `UacUninstall`, a second never-seen device: the wizard chose `uac2` all the same and the log has no call: **2000 already ranks `uac2` above `usbaudio`** (no signature penalty there). Playback on 2000 not read (`uac2` has no 2000 leg yet) |
| 7 SP1 x86 | `pnputil -a` and `UACCI.INF` each asked "Windows can't verify the publisher" (Install anyway). The unsigned class co-installer DLL loads: first plug, `usbaudio nodes marked=1`, "Your device is ready to use", `uac2wdm` RUNNING, `usbaudio` not installed, playback through `Media.SoundPlayer` reached the device |
| Vista SP2 x86 | `pnputil -a` asked "can't verify the publisher"; `UACCI.INF` registered after Windows' own `mmcico.dll,MediaClassCoInstaller`, which it kept. First plug: `usbaudio nodes marked=1`, `uac2wdm` RUNNING, `usbaudio` not installed; playback reached the device (Media Player) |
| 8.1 x86 | as Vista for the setup (`mmcico.dll` entry kept). First plug: the co-installer was called, but `usbaudio nodes marked=0`: no `wdma_usb.inf` node with the `usbaudio` service was in the list (why - 8.1's own INF excluding the UAC 2.0 ids, or the node being another INF's - not read); `uac2wdm` RUNNING, playback reached the device. So 8.1 does not need the co-installer for this device; it does no harm there |
| 98 SE | Stock 98 SE (the `pc` pristine base, Windows' own UHCI, no NUSB, no xHCI98), a never-seen Full Speed UAC 2.0 device (`fs`): 98's hub presents "USB Composite Device" (`USB.INF`), then a child "USB Audio Device". With `uac2wdm.inf` only copied into `INF\OTHER` (the driver index deleted), a search of the driver database offers `WDMA_USB.INF`: a copied INF is not in the database. Given `uac2`'s folder as a location, the wizard recommends `uac2` ("The updated driver") over Windows' driver (one of "the other drivers"). After that one install, a further never-seen device gets `uac2` on its first plug without a choice ("New Hardware Found", then a prompt for `uac2`'s folder). So `uac2`, once Windows' setup has installed it, outranks `WDMA_USB.INF`. (Corrected 2026-10-30: the first reading put the INF where Windows does not read it - `INF\OTHER` was a file, not a folder - and so found `uac2` no candidate.) With the Full Speed rule of the ME row and 98 SE's KS files put on the guest (from 98 SE's cabinets): `uac2` starts. The first session, just after the install and with the first build of the rule, did not stream: `play` 0 of 8 (67 packets in 0.11 s, then the stream stopped with the alternate setting left at 1), `rec` failed to start (`STATUS_NOT_SUPPORTED`), `ramp`'s controls worked; waveIn then reported a single format. With the committed build (`50282c8`): the first start hung the guest after the device was configured (hard reset); after a cold start `play` 8 of 8 three times, `rec` 8 of 8 with the tone, `ramp` passed once and once stalled at 1 of 8 buffers in 40 s (controls written). Repeats (2026-10-30, cold starts, the same device on its first plug of the boot): the committed build hung after the device was configured in 3 of 5 plugs; in each hang the start had completed on the bus, the screen and keyboard stopped, the processor sat mostly in the system's virtual machine manager with occasional user-mode samples, the UHCI controller raised no interrupts and the timer ran. A build that writes markers to the debug port at each start stage, level property, intersection, stream creation and state change did not hang in 6 plugs (including a hot replug and plugs during start-up); an earlier marker build without the start markers hung once, on a replug after the driver file had been replaced in that session, before any marker. So the wait was not located, and it looks timing-sensitive. The ramp stall was not re-run. Neither is read further |
| ME | Stock ME (the q35 base on stock QEMU, Windows' own UHCI, no xHCI98), `fs`: ME's hub does not split the device; it is one "USB Audio Device" with interface 0's compatible ids (`USB\CLASS_01&SUBCLASS_01&PROT_20`, `USB\CLASS_01&SUBCLASS_01`, `USB\CLASS_01`). With `uac2wdm.inf` only copied into `INF\OTHER`, the automatic search installs `WDMA_USB.INF`. Given `uac2`'s folder, the wizard recommends `uac2` ("The updated software") over Windows' "USB Audio Device". After that install, a further never-seen device gets `uac2` silently on its first plug. In build 18 `uac2` did not start there: Problem 10, no `SET_CONFIGURATION`; its record read stage 13 (`Wave::Prepare`, which opens the USB side) `STATUS_NOT_SUPPORTED`: the speed query (the USBDI bus interface) found no `IsHighSpeed`: ME's hub answers with a version 0 interface (32 bytes, read with a diagnostic build), which predates USB 2.0, and `uac2` refused to guess the speed. Since then, on a system below WDM 1.10 (98 SE, ME), whose own stack has no USB 2.0, no answer or a version 0 answer means Full Speed; NUSB's EHCI stack and xHCI98 give version 1, and on 2000 and later a missing speed is still refused. A first build of the rule (any failed query below WDM 1.10 as Full Speed) started and played on this guest; `9d87fac`, which refused every answered but unusable interface, failed again at stage 13, which is how the version 0 answer was found and then admitted (`50282c8`). With the committed build (`50282c8`, `uac2wdm.sys` SHA-256 `f4ad9c65...`) on the same guest: `uac2` starts, `play` 8 of 8, `rec` 8 of 8 with the 1 kHz tone, `ramp` (volume and mute) passed |
| 8 x86 | as 8.1 x86 (`mmcico.dll` entry kept; `pnputil -a` asked "can't verify the publisher"): first plug, the co-installer called, `usbaudio nodes marked=0`, `uac2wdm` RUNNING |
| 7 SP1 x64 | Test signing on. First guest: `uac2` 0.1.4.0's released `wv-8-x64` files (the owner's test-signed image) and its certificate in the machine Root store; `UACCI1.DLL` x64 (test build) registered after `mmcico.dll`. On the first two first plugs `usbaudio nodes marked=1` (the x64 co-installer loads in the 64-bit device installer), `uac2` selected and installed, `usbaudio` not installed (a third device was plugged after the date change below; its log line was not read); `uac2` did not start, Problem 52 (`CM_PROB_UNSIGNED_DRIVER`). Cause: the q35 base launcher pins the guest's clock to 2026-09-20 (activation grace), and the 0.1.4 test certificate's validity starts 2026-10-28 (SignTool: not within its validity period). Setting the date inside that guest made SignTool verify the image but the device still reported 52 after a restart, and Windows then showed a not-genuine notice; not read further. Second guest (a fresh overlay, clock as pinned): this tree's `uac2wdm.sys` x64 (`fd4b00c`) signed inside the guest with a throwaway certificate whose validity starts 2026-01-01: a never-seen `hs24` device, `usbaudio nodes marked=1`, `uac2wdm` RUNNING, `rec` 8 of 8 with the tone, `play` 6 of 8 in the 13-second wait (the simulator counted about 25,800 OUT packets on that High Speed `bInterval` 1 endpoint, about 3 seconds of audio, between the stream's start and stop about 20 s apart; so the stream ran slower than real time on that rig, but whether all of the audio arrived and why it was slow are not read; this is Windows' EHCI path, not xHCI98's; the ALC4080's pairs, also High Speed, completed 8 of 8 on the same guest); the ALC4080 capture (`alc4080cap`), `marked=1`, three playback pairs and two inputs listed (the Analog Connector's missing, as on Vista x64: see that row), `play` 8 of 8 on each output, `rec` with the tone on each listed input, `ramp` (volume and mute) on each output |
| Vista SP2 x64 | Fresh overlay, test signing on, this tree's x64 image (`fd4b00c`) signed inside the guest with the throwaway certificate of the 7 x64 row (the guest's clock is pinned to 2026-09-20 as on 7); `UACCI1.DLL` x64 registered after `mmcico.dll`. Never-seen `hs24`: `usbaudio nodes marked=1`, `uac2wdm` RUNNING, `rec` 8 of 8 with the tone (the `play` result was not captured). `alc4080cap`: `marked=1`; three playback pairs, `play` 8 of 8 and `ramp` on each; two of the three inputs listed (Line and Microphone), `rec` with the tone on both. The third input, the Analog Connector (terminal 9, `KSNODETYPE_ANALOG_CONNECTOR`), is an audio endpoint in the registry with `DeviceState` `0x10000001`, where the others read 1, and WinMM does not list it; why it is in that state (a default for that category, say) was not read, nor was it enabled and retried. 7 x64 lists the same two inputs; its endpoint states were not read |
| 8 x64, 8.1 x64 | Their clocks follow the host's. `pnputil -a` on 8 and later refuses an INF with no catalog ("The third-party INF does not contain digital signature information"), so a throwaway catalog (`CatalogFile=` added to a copy of the x64 INF, built and signed inside the guest with the same throwaway certificate) was used; test signing on and a restart first. Both: never-seen `hs24`, `usbaudio nodes marked=0` (as 8 and 8.1 x86), `uac2wdm` RUNNING; `alc4080cap`, three playback pairs and two of the three inputs listed (as on Vista and 7 x64), `play` 8 of 8 and `ramp` on each output, `rec` with the tone on both inputs. On 8 x64 one `play` on the S/PDIF pair did not return for several minutes once; stopped and run again it passed twice; not read further |
| XP x64 | No `pnputil`: `uac2` on `DevicePath`, the throwaway-signed x64 image, `UACCI.INF` by `InstallHinfSection` (registered as the only MEDIA class entry). Never-seen `hs24`: the wizard named the device "UAC2 USB audio (experiment)" (`usbaudio nodes marked=1`), "has not passed Windows Logo testing" once, `uac2wdm` RUNNING, `play` 8 of 8, `rec` with the tone. `alc4080cap`: `marked=1`, all three outputs and three inputs listed; `play` 8 of 8, `rec` with the tone and `ramp` passed on every device listed |

**Known issue (0.1.5; owner, 2026-10-30).** 98 SE with Windows' own USB 1.1 stack: the first start of a Full Speed UAC 2.0 device can hang the system; use NUSB or xHCI98. Evidence: the 98 SE row above, and a further 16 cold-start plugs on 2026-10-30 (no hang in 11 plugs without a driver-file copy in the session, 5 of them with a full-screen DOS box open; 1 hang in 2 plugs after a copy of the same driver file in the session). The hunt stopped there (owner); the cause is not read.

**Windows' USB Audio 1.0 driver on a UAC 2.0 device, 98 SE and ME (2026-10-30; owner's question).** Fresh overlays of the 98 SE pristine base (`pc`) and the ME base (q35), Windows' own UHCI only (no xHCI98, no NUSB), `uac2` not on the system, a never-seen `fs` device (`FFFF:0402` on 98 SE, `FFFF:0501` on ME; `hs24` was not run: a High Speed device does not attach to a UHCI-only machine in QEMU). On both the interface's compatible ids are `USB\CLASS_01&SUBCLASS_01&PROT_20`, `USB\CLASS_01&SUBCLASS_01`, `USB\CLASS_01`, and the node installed is `WDMA_USB.INF` section `USBAudio` with `MatchingDeviceId` `USB\CLASS_01`: the bare class line of Windows' own INF is what matched.

- **What Windows' driver does.** No hang. 98 SE: the wizard installed "USB Audio Device", asked for a restart; before and after it the device reported Code 10 (failed start), the device was configured (`SET_CONFIGURATION`) but WinMM listed no wave device. ME: installed without asking for files and asked for a restart; Code 10, no `SET_CONFIGURATION` reached the device, no wave device.
- **Update Driver to `uac2`** (Device Manager, the device's Update Driver, `uac2`'s folder given): on both the wizard found "UAC2 USB audio (experiment)", completed at once (no wait; the kernel-streaming files it asked for were skipped, Windows' own driver having installed them) and asked for a restart. Declining it, unplugging the device and shutting down completed on both (no wait at "shutting down").
- **After that.** 98 SE, after the shutdown and a cold start: the replugged device reported Problem 2 because `uac2wdm.sys` was missing (the rig's keystrokes had skipped it during the update); with it copied in by hand and the device replugged in that session, `uac2` started (one input, one output), `play` completed 0 of 8, and then the guest stopped echoing keystrokes and its clock stood still for about a minute; not followed further (possibly the known issue above). ME, after a cold start: the first plug listed one input with one format and no output, and a capture open failed (`STATUS_INVALID_PARAMETER`); after an unplug and replug both directions were listed, `play` 8 of 8 (in about 9 s), `rec` 2 of 8 with signal; not read further.
- So on 98 SE and ME Windows' driver takes a UAC 2.0 device by its bare `USB\CLASS_01` line and fails to start on it (Code 10), but nothing waits as on XP (45.24), and the change to `uac2` completes; what follows the change is the 9x own-stack behaviour this section already records.

### 16.6 USB Audio 3.0 devices (owner, 2026-10-29: a reading only)

**The specification.** USB Audio Device Class 3.0 (with errata to
2019-04-05, usb.org, `Audio30-Errata.pdf`), section 3.3 "Backwards
Compatibility": "The first USB Configuration descriptor (index 0) exposed
by the Device shall contain an Audio Interface Association (AIA), which is
compliant with ADC 1.0 or ADC 2.0", and a further configuration shall
expose a BADD 3.0 AIA; section 4.1: such a device "is required to contain
multiple Configuration descriptors" and a BOS Configuration Summary. So a
3.0 device has a 1.0 or 2.0 function, and that configuration comes first.
Linux (`drivers/usb/core/generic.c`, `usb_choose_configuration`, v6.12;
facts only) picks the first UAC3 configuration when there is one, else
the first configuration.

**The reading** (XP SP3 x86, the 45.24 fix-leg overlay with this release's
xHCI98 and `uac2` installed, plus Windows' own EHCI; `uacemu.py`
`uac3dual`: configuration 1 = `hs24`'s UAC 2.0 function, configuration 2 =
a 3.0 function by its standard descriptors, no BOS; `FFFF:0230`):

- Under xHCI98: only configuration index 0 is read; no configuration is
  selected; the device is one PDO (the bus splits a device only when it
  has one configuration, design record 13 section 10.8), Found New
  Hardware finds no driver for it.
- Under Windows' own EHCI: only index 0 is read, the device is
  deconfigured (`SET_CONFIGURATION 0`), and the whole device becomes
  "USB Audio Device" on `usbaudio`, code 10.
- Neither selects configuration 2, and neither shows protocol 30h to a
  driver; but neither gives the 2.0 function to `uac2` either, because
  neither splits a device with two configurations. Serving the 2.0
  function of a 3.0 device would need the parent to select configuration
  1 and split it (xHCI98's design record 13 section 10.8; on Windows'
  stack, not this repository's to change). No 3.0 driver work (owner).

## 17. The play position is the fetch cursor (seek report, 2026-10-31)

### 17.1 The report

A user of `uac2` 0.1.5.0 on 32-bit XP with a Conexant CX31993 DAC
(`docs/status.md`, "Open report: `uac2` playback garbled after seeking"):
playback starts clean, but seeking within a track can leave the sound
garbled; VLC with 16-bit 44.1 kHz WAV and FLAC, and Windows Media Player
with FLAC through the K-Lite codecs; Windows Media Player with WAV not.

### 17.2 What a seek does at the driver

Rig: XP SP3 x86 (q35), Windows' own `usbehci.sys`, no xHCI98; `uac2`
0.1.5.0 as released (`free/audio/uac2/w2k-xp`, `uac2wdm.sys`
`528f2ad0...`); `uacemu.py --profile cx31993` (the layout of 2026-10-28:
one synchronous playback endpoint, no feedback, High Speed `bInterval`
1) with its received-PCM capture; `UACSEEK.EXE` (`uacseek.c`, test
only) playing a counted signal and seeking the way a player does:

- WinMM: eight 20 ms buffers kept queued; a seek is `waveOutPause`,
  `waveOutReset`, new buffers from 30011 frames further on,
  `waveOutRestart` (or `waveOutReset` alone);
- DirectSound: a one-second looping secondary buffer kept full behind
  the play cursor; a seek is `Stop`, `SetCurrentPosition(0)`, a refill
  from the new place, `Play`.

`uacseek.py` reads the capture back to the source frame numbers (or, when
KMixer converts 44.1 kHz to the device's 48 kHz, to the right channel's
staircase, which survives the conversion).

What was read:

- **No KS state change at a seek.** In both APIs the device's
  alternate setting stayed selected from the first buffer to the close:
  KMixer keeps its pin on `uac2` in `KSSTATE_RUN` and mixes silence
  while the client is paused or reset. So a seek reaches the driver only
  as a gap in the data PortCls writes into the cyclic buffer.
- **The garble is old ring data.** The received stream holds blocks of
  2 to 8 ms (whole or half URBs) whose frames are exactly one ring older
  than their neighbours (16 KiB, 4096 frames at 48 kHz 16-bit stereo, the
  ring's minimum size), then the correct sequence again. DirectSound at
  48 kHz: 330 such blocks in 8 s, every 20 to 60 ms, before and
  after each seek; WinMM at 44.1 kHz (converted to 48 kHz by KMixer): 269
  blocks in 8 s; WinMM at 48 kHz: a few, around the seeks. That is the
  sound of the report: the music with short pieces of 85 ms ago spliced
  in, for as long as it lasts.

### 17.3 The cause

`UacStream` (`uacstream.h`) keeps two cursors in the cyclic buffer:
`filled`, how far the USB side has copied bytes out into URBs (each URB is
filled when it is submitted), and `completed`, how far URBs have come
back. `GetPosition` reported `completed`. With four data URBs queued
(section 4.4) the copy runs about 16 ms ahead of `completed` (32 ms at High
Speed `bInterval` 4).

Read from the measurements (PortCls's code is not read, and its write
scheduling is inferred, not documented): PortCls's WaveCyclic port treats
the reported position as the point up to which the device has taken the
data, and writes the next data, or silence when it has none, from there
on. While it has plenty of data queued it has written well ahead and
nothing shows. When its lead shrinks below the URB queue (a
client that flushed and restarted, DirectSound's streaming), it writes the
bytes at `completed` + a few ms just before they are due by its account,
which is after the URB that carries them was filled: that URB carries what
the ring held one lap earlier. Nothing in the driver resynchronises it; it
lasts while the lead stays short, which after a seek can be the rest of
the track.

Why Windows Media Player with WAV was spared was not read (no player was
run): the lead PortCls has depends on the client's own queueing, which
differs between renderers.

### 17.4 The change

1. **The render position is the fetch cursor.** `GetPosition` of a
   playback stream returns `filled`: every byte before it has been copied
   into a URB and can no longer change what is sent, every byte from it on
   is still read from the ring. PortCls then writes data or silence at
   exactly the bytes the next URB will take. Capture is unchanged: its
   position is where received frames have been written (`completed`, which
   `WriteBytes` keeps equal to `filled`).

   This departs from the documented contract on purpose. Microsoft Learn
   (`IMiniportWaveCyclicStream::GetPosition`, read 2026-10-31) describes
   the position as the driver's best estimate of the sample at the
   converter, as distinct from the DMA position, buffering delays taken
   into account. A device whose data must be handed over 16 to 32 ms
   before it is played cannot give both: reporting the played position
   lets PortCls overwrite, or leave unwritten, bytes already handed over
   (17.2), which is audible; reporting the fetch cursor makes positions
   early by the queue (below), which is not. The second is chosen, on the
   measurements of 17.2 and the legs of this change.
2. **No rewind on pause.** `RUN` to `PAUSE` (or `STOP`) used to set
   `filled = completed`, so a paused stream resent the data of the URBs
   the stop cancelled. With the position at `filled` that would move the
   position backwards, which PortCls cannot take (a smaller position reads
   as a wrap of nearly the whole ring). The cursor now stays: the cancelled
   URBs' data (at most the queue, 16 ms, 32 ms at High Speed `bInterval`
   4; some of it may have been played)
   is dropped, and a resumed stream goes on from where PortCls believes
   the device is. `STOP` still resets both cursors to 0.
3. **A byte taken is cleared.** `FillBytes` zeroes the ring bytes it
   has copied out, under the ring lock and before the new position can be
   read. A byte nobody has written since a URB took it a lap earlier goes
   out as silence in the next lap, not as that lap's audio: an underrun
   (PortCls's data late) is heard as a gap. It is no guarantee against
   every stale byte: a write PortCls began before the fetch and finished
   after it (through `CopyTo`, which takes the same lock, but still after
   the clear) lands in a lap already taken and is sent a lap late if
   nothing replaces it; the lock orders the copies, it cannot tell which
   lap a write meant. With 1 and 2 alone one such block was still read in
   8 s of DirectSound pausing (17.2's rig, `ds ... h`: a 2 ms block one
   ring old right after a resume). Zero is silence for every format the
   stream takes (signed 16-, 24- and 32-bit PCM). For playback nothing
   reads a byte behind the position again; capture does not use
   `FillBytes`.
4. **What stays.** `completed` still counts completed URBs (for render it
   no longer equals `filled` minus the queue after a pause, and nothing
   uses it so); notifications (`IPortWaveCyclic::Notify`) still come from
   completions, never a timer, at most one per completion when the
   completed bytes reach the `SetNotificationFreq` interval (several
   intervals in one URB give one). The URB queue, its depth, the packet
   sizes and the transport are unchanged.
5. **Not covered: a ring no larger than the queue.** `SetBufferSize` can
   make the ring (`used`) no larger than the bytes the four queued URBs
   take (for example 3072 bytes, exactly the 16 ms queued of 16-bit
   stereo at 48 kHz, or 1536 bytes, 8 ms). The first `RUN` then takes a
   whole lap or more at once and the position comes back to where it was
   or wraps past bytes PortCls has not refilled; no position can say that,
   and with the clearing of item 3 the next URBs take zeros. The ring must
   be larger than the queue, with room for PortCls to see the position
   move between its writes. This was so before the change (section 4.4's "not guarded")
   and stays so; the legs did not meet it (KMixer's pins keep the
   allocated ring).

Consequences:

- The position a client reads (`KSPROPERTY_AUDIO_POSITION`, WinMM's
  `waveOutGetPosition`, DirectSound's play cursor) runs ahead of what the
  device has received by the queue (up to about 16 ms, 32 ms at
  `bInterval` 4), as the DMA position of a PCI device runs ahead of its
  converter by its FIFO. Before, it ran behind what had been taken from
  the ring by the same amount.
- A pause and resume skips the queued audio (up to the same 16 or 32 ms)
  instead of repeating part of it.
- An underrun plays silence instead of old audio, for the bytes nobody
  wrote since the last lap (item 3's late write excepted).
- `uacstream.h` is one header for every system and both architectures
  (98 SE, ME, 2000, XP, Vista to 8.1, x86 and x64), so the change applies
  to all; it does not depend on the host controller or its driver.

### 17.5 Tests

- `streamtest.cpp`: a render stream's position follows `FillBytes`, not
  `CompleteBytes`; the bytes taken read zero afterwards; a pause keeps the
  position; `STOP` resets it; capture keeps its write position;
  notifications still follow completed bytes.
- `UACSEEK.EXE` and `uacseek.py`: a stale block is a run of the counted
  signal that is older than the run before it while the run after it
  joins the run before (as if the block were not there, or replaced
  nothing); with KMixer converting the rate, a backward jump of the
  right channel's staircase undone 0.5 to 100 ms later. Backward jumps
  alone are not counted against a leg: a seek's jump of the 16-bit count
  can read as one. A leg passes with no stale block across its seeks. Legs: XP under Windows' EHCI and under
  xHCI98 (both APIs, 16-bit and float clients, 44.1 and 48 kHz); 2000 and
  7 x86 for regressions; 98 SE (the same header runs there).

## 18. Windows 2000: the install asks for a restart instead of starting the device (2026-10-31)

### 18.1 The problem

On Windows 2000 under Windows' own USB 2.0 drivers, starting `uac2` on a
device that had been started before and stayed plugged in stops Windows
(STOP 0x1E in `usbhub20.sys`): Update Driver from Windows' "USB Audio
Device" (`usbaudio`), and Enable after Disable. The stop comes inside
`PcAddAdapterDevice`, in the hub driver's answer to the
`IRP_MN_QUERY_INTERFACE` PortCls sends to the PDO; `uac2` has sent
nothing by then
([testing/2026-10-31-uac2-2000-stop.md](../testing/2026-10-31-uac2-2000-stop.md)).
Windows 2000 binds `usbaudio` to a UAC 2.0 device by itself at the first
plug unless `UACCI.INF` was installed first, so Update Driver is the
ordinary way a 2000 user reaches `uac2`.

### 18.2 The change (owner, 2026-10-31)

The install on 2000 carries the INF `Reboot` directive: Setup then marks
the device as needing a restart and does not start it on the new driver;
after the restart the hub enumerates the device afresh and `uac2` starts
on a new PDO, which does not stop (the first-plug legs). Read once with
the released 0.1.5.0 INF plus `Reboot` (the record above, leg k6c):
Update Driver from `usbaudio` ended with "will not work until you
restart", no stop, and after a cold start `uac2` played.

Only 2000 may get it: XP and later do not stop, and a restart prompt at
every install there would be a regression. Which sections a system reads:

- Windows 2000 reads only the undecorated models section (`[Models]`): the
  `TargetOSVersion` decorations of the `[Manufacturer]` entry (`NTx86`,
  `NTamd64`, `NTx86.5.1`) are read from XP on (the driver kit's INF
  Manufacturer section documentation; `release\release.ps1`,
  `Get-SystemModelIds`, encodes the same rule). 98 SE reads `[Models]` too.
  A 2000-only `[Models.NTx86]` is therefore not possible: 2000 would not
  read it.
- From `[Models]`, 98 SE installs `[UAC2]`, 2000 `[UAC2.NT]` (the `.NT`
  install-section decoration is read from 2000 on, 9x reads none).
- XP and later x86 read `[Models.NTx86]`; x64 reads `[Models.NTamd64]`
  and installs `[UAC2.NTamd64]` (an architecture decoration wins over
  `.NT`).

So:

1. `[UAC2.NT]` gains `Reboot`. Only 2000 reaches it once item 2 is done.
2. `[Models.NTx86]` points its three ids at a new install section
   `UAC2XP`: `[UAC2XP.NT]`, `[UAC2XP.NT.Services]` and
   `[UAC2XP.NT.Interfaces]` are `[UAC2.NT]`'s, `[UAC2.NT.Services]`'s
   and `[UAC2.NT.Interfaces]`'s lines without `Reboot`. Device, ids,
   files, registry and interfaces are unchanged for XP and later x86.
3. `[Models]`, `[UAC2]` (98 SE), `[Models.NTamd64]` and the NTamd64
   sections are unchanged. The x64 copy (`x64\uac2wdm.inf`) takes the same
   edit, as `release.ps1` requires it to agree.
4. `UACCI.INF` has no models (a `DefaultInstall` setup INF) and is
   unchanged; the co-installer does not name install sections.

On 2000 every `uac2` install now asks for a restart, also the first plug
after `UACCI.INF` (which never stopped): the device works after the
restart. Enable after Disable is not an install, so `Reboot` does not
reach it; that stays a known issue (unplug instead of Disable).

### 18.3 Legs

- 2000 under Windows' EHCI: `usbaudio` bound, Update Driver to the new
  INF: a restart prompt, no stop; after the restart (cold start on the
  q35 base, whose warm restart hangs) `UACSEEK` plays.
- XP x86 and 7 x64: install and play with no restart prompt (XP through
  `[Models.NTx86]`/`UAC2XP`, 7 x64 through the x64 INF).

## 19. The queue depth: `Uac2QueueMs`, and the worker's priority (2026-11-04)

### 19.1 The report

A user report (Windows XP x64 SP2 on an AMD Phenom II X4 955 with an
AMD 790X/SB750 board, a Fresco FL1100 card on a shared level interrupt,
xHCI98 3.4.0.0): a Conexant CX31993 DAC (`3302:12C3`, High Speed,
`bInterval` 1) on `uac2` pops often under heavy CPU load (a game). The
same card in a Xeon 5080 system, and a Phenom II X6 on another board with
a VL805, are fine. xHCI98's snapshot shows 7743 Ring Underrun (completion
code 14) and 3 Missed Service (23) completions on the isochronous OUT
endpoint, and a longest completion pass of about 16 ms on the bulk path
(a busy UAS drive on the same controller).

### 19.2 What the queue covers

Each data URB carries about 4 ms (section 4.4: 32 packets at High Speed
`bInterval` 1, 16 at 2, 8 at 3, 4 at Full Speed; 8 ms at High Speed
`bInterval` 4), and four were kept queued: 16 ms (32 ms at `bInterval`
4). A URB is refilled and resubmitted by the stream's worker thread after
its completion. If, from one completion on, the next submissions reach
the controller more than about 12 ms late (16 ms less the URB that just
completed), at any layer - the host controller driver's completion
processing, a DPC, the worker waiting for a processor - the endpoint has
nothing scheduled: xHCI reports Ring Underrun (xHCI 4.10.3.1), and the
device gets no packets for that time, a gap that is heard.

The worker is a system thread (`PsCreateSystemThread`) at the priority
it is created with, the system process's normal priority; the completion
routine wakes it with `IO_NO_INCREMENT`. Under a load of threads at
normal priority and above (a game) it waits for a processor like them.


### 19.3 The value

- **`Uac2QueueMs`** in the device's driver key (`HKR` of the INF;
  `IoOpenDeviceRegistryKey` with `PLUGPLAY_REGKEY_DRIVER`, where the
  driver already records its diagnostics): the milliseconds of data each
  stream keeps queued to the USB stack. Default **32**; range **16 to
  128**, a value outside it is clamped to the nearer end; absent, or of
  another type or size, means the default. Read as a `REG_DWORD`, or a
  4-byte little-endian `REG_BINARY` (what a 9x INF writes).
- One value for playback and capture, for every endpoint of the device.
- Read once when a stream is created (`NewStream`, PASSIVE) and kept with
  the stream: that one reading sizes the stream's ring (19.5) and, at
  `RUN`, its transport (19.4). A change takes effect at the next stream a
  client opens; a running or created stream keeps its reading.
- Written at the default by every install section, never over a value
  already there (no-clobber, `0x2`), so the setting is visible and an
  update keeps a user's choice: the NT sections (`[UAC2.NT]`,
  `[UAC2XP.NT]`, `[UAC2.NTamd64]`, both INFs) with
  `HKR,,Uac2QueueMs,0x00010003,32` (`REG_DWORD`); 98 SE / ME's `[UAC2]`
  with `HKR,,Uac2QueueMs,0x00000003,20,00,00,00` (binary, the four bytes
  of 32), since 9x does not expand a number into a `DWORD`'s bytes.
- The values in force are recorded: `Uac2UsbStats` / `Uac2UsbStatsIn`
  (written when a stream stops) gain `QueueRead` (the raw value read,
  `0xFFFFFFFF` when none was usable), `QueueMs` (in force),
  `DataTransfers` and `TransferUs` (one data URB's span in
  microseconds), appended after the existing fields.

### 19.4 Transfers from milliseconds

The size of one data URB is unchanged (section 4.4): `DataPackets` is 32
>> (`bInterval` - 1), at least 8, at High Speed, and 4 at Full Speed. Its
span `T` is `DataPackets` x 2^(`bInterval` - 1) x 125 us at High Speed
and `DataPackets` x 2^(`bInterval` - 1) x 1 ms at Full Speed. The number
of data URBs kept queued is `ceil(QueueMs x 1000 / T)`, at least 2 (one
refilled while one is on the bus) and at most 32. The feedback URBs (two
of 8 packets) are unchanged.

| Speed, `bInterval` | `T` | 16 ms | 32 ms (default) | 128 ms |
|---|---|---|---|---|
| High, 1-3 | 4 ms | 4 | 8 | 32 |
| High, 4 | 8 ms | 2 | 4 (as before) | 16 |
| High, 5 | 16 ms | 2 | 2 | 8 |
| Full, 1 | 4 ms | 4 | 8 | 32 |

**Bounded by the ring.** At `RUN`, the count is then lowered so that the
queue's bytes stay below the ring PortCls uses (`used`, after any
`SetBufferSize`) by at least one URB: at most `floor((used - B) / B)`
URBs, where `B` is the most one URB can take: the stream's nominal bytes
for `T` (rounded up), plus an eighth (the fastest feedback `uacrate.c`
accepts is the nominal rate + 12.5%) and one PCM frame per packet (the
cadence), but no more than the endpoint's packet capacity times the
packets. The floor of 2 stays: a ring under three URBs (12 ms at
`bInterval` 1) is the "not guarded" case of 17.4 item 5, as before.
Examples: a 4096-byte ring of 16-bit stereo at 48 kHz (`B` = 768 + 96 +
128 = 992 bytes) allows 3 URBs, 12 ms (16 ms before this change: this is
the one case the change shortens); the default ring (62 ms, here the 16
KiB minimum, 85 ms) allows 15, so 8 at 32 ms.

For capture the queue takes nothing from the ring ahead of time (each
completion writes what it received), so the bound only keeps the two
directions alike.

The computation is `UacPlanQueue` in `uacplan.c` (host-tested); a span
above 32 ms (`bInterval` 6 and up at High Speed) gives the minimum of 2.

### 19.5 The ring

The cyclic buffer must be larger than the queue (17.4 item 5: the first
`RUN` takes the whole queue at once). It stays about 62 ms of the format
(16 KiB to 1 MiB) up to a 32 ms queue, and grows by 1.25 times the
queue above 32 ms: 62 ms + 1.25 x (`QueueMs` - 32) ms, so 182 ms at 128.
The quarter is the room 19.4's bound needs (one URB spare and `B`'s
margins): with the queue alone (158 ms at 128), 48 kHz 16-bit stereo got
29 URBs, 116 ms, in the first legs; with the quarter it gets all 32.
Every default (32 ms) ring is the size it was. Where the 1 MiB cap keeps the ring
shorter (above about 5.7 MB/s, for example 384 kHz, 8 channels, 32-bit
at 12.3 MB/s, 85 ms), 19.4's bound lowers the queue (to 17 URBs, 68 ms,
at 128 ms there, `B` = 56320 bytes) instead of letting the first `RUN`
lap the ring.

The queue is fixed when the pin's first `RUN` creates its transport; a
`SetBufferSize` after that is not followed (17.4 item 5, unchanged)
until the pin is closed and a new one opened.

### 19.6 The worker's wake (a separate step)

The completion routine wakes the worker with `IO_SOUND_INCREMENT`
instead of `IO_NO_INCREMENT` in `KeSetEvent` (the documented priority
increment for a sound driver's waits; `KeSetEvent` is imported on every
system already, 98 SE and ME included). On NT the woken worker runs at
its base priority plus the increment, at most 15 (the top of the dynamic
range), the boost decaying by one at each quantum it uses up; so a refill
after a completion is ahead of normal-priority threads (a game's) but
never above a real-time thread, and a worker that keeps running falls
back to its base priority. On 98 SE and ME the increment is accepted;
its effect there is not measured. No new import, the same binary for
every system.

**The real-time worker, `Uac2Realtime`** (owner, 2026-11-04, build 20).
The user who reported the pops found that lowering the game's priority
cured them: the increment lifts the worker to at most 15, which a game
running at High priority class (base 13, boosted to 15) only ties. So the
worker, as it starts, sets itself to `LOW_REALTIME_PRIORITY` (16) with
`KeSetPriorityThread(KeGetCurrentThread(), ...)`, above every
normal-class thread whatever a game's class; the increment on the wake
then changes nothing (a real-time thread takes none). The work per wake is
bounded - the completed slots retired, the free ones refilled and
submitted, then a wait - so the worker never holds a processor. It is
the lowest real-time level; Vista's own audio engine runs its threads in
that range (MMCSS).

- The value: `Uac2Realtime` in the driver key, `1` on (the default), `0`
  off; read at each RUN (`UacUsbSetRealtime`, `UacPlanRealtime`), so a
  change takes effect at the next open. Anything but `0` is on; nothing
  usable read is the default. The INFs write `1` no-clobber in the same
  sections as `Uac2QueueMs`, the 9x binary form in `[Queue9x]`.
- The import: `KeSetPriorityThread` joins both import lists. xHCI98's
  import records find the name among 98 SE's `NTKERN` exports
  (xHCI98 record 13, the import table's row 114, graded "ntkern-name
  only", the weakest evidence), so the one x86 binary is expected to load
  on 98 SE and ME; the 98 SE and ME legs prove the load and read whether
  it has any effect there.
- The stats (build 20): `RealtimeRead`, `Realtime` and `WorkerPriority`
  (16, or 0 when the worker set none) appended to `Uac2UsbStats`.
- The fairness rule (build 24; builds 21 and 22 slept the worker after a
  count of passes, which a wake racing the wait could reset without the
  worker ever blocking, and whose 10 ms sleep could itself drain a queue
  a small client ring holds to 8 or 12 ms; build 23 measured in
  interrupt time, whose 10 to 15.6 ms ticks before Windows 8 can miss a
  loop's time inside waits that return at once, and let a later RUN turn
  the rule off under a real-time worker; Codex's reviews of builds 21 to
  23). A stack that completes URBs inline again and again with nothing
  delivered (capture with zero-length packets, no `Fail`) leaves `Wake`
  signalled, so a real-time worker would hold a single processor, the
  thread that asks Stop included. So the worker:
  - keeps its own copy of `Uac2Realtime` from its start: a later RUN's
    value applies to the next worker only;
  - measures with `KeQueryPerformanceCounter` (HAL; stock 98 SE drivers
    import it, xHCI98 record 13, the import table's row 102) the time it
    runs between waits, and counts as busy a wait shorter than 50 us (a
    return at once, not a block);
  - in windows of at least 200 ms (`UacPlanDuty`), a window busy for 60 %
    or more demotes it to the priority it was created at (the value
    `KeSetPriorityThread` returned) for the next window, where the normal
    threads share the processor with it; a demoted window always ends in
    real time again, so a runaway loop holds real time for at most every
    other window, and a recovered stream is never held down by time it
    spent preempted while demoted;
  - never sleeps, so a working stream keeps refilling.
  A normal stream (a pass well under a millisecond, at most about 1.25 a
  millisecond) is far below 60 %. `WorkerThrottles` counts the demotions
  (0 in normal running); while demoted, `WorkerPriority` reads the
  created priority. The x64 build links `hal.lib` for the counter.
- Read at every RUN (build 21): a stream kept across a pause or stop
  (`Drain` without destroy) takes the value again at its next RUN, so
  `Uac2Realtime` 0 applies at the next start of playback.

A test build of this (2026-11-04, XP x64 under CPU load) gave 13 Ring
Underruns against the increment's 62, and 32 against 34 under CPU and
disk load.

### 19.7 Consequences

- The position a client reads runs ahead of what the device received by
  up to the queue (17.4): 32 ms by default instead of 16 ms; a pause and
  resume skips up to the queue. Start-up is unchanged (the first URB goes
  out at once).
- Non-paged memory: one buffer of `DataPackets` x the endpoint's packet
  capacity per data URB, 8 instead of 4 by default (for example 8 x 32 x
  294 bytes for 24-bit stereo at 48 kHz), up to 32 at 128 ms.
- Under xHCI98 each pipe has four transfer records (xHCI98 `src/hcd.h`,
  `HCD_PIPE_XFERS`); a URB beyond them waits on the pipe and is handed a
  record by xHCI98's own completion processing as one retires
  (`src/hcd_io.c`, the done path's `hcdRecordRelease` and `hcdLaunch`).
  There the deeper queue does not lengthen what is on the controller's
  ring (at most 4 URBs, 16 ms at `bInterval` 1); it takes `uac2`'s
  worker out of the refill for the URBs queued behind them. A completion
  pass of xHCI98's that is itself late by more than the ring holds still
  underruns. Microsoft's stacks schedule every queued URB.
- What is not changed: the URB size, the feedback stream, the transport,
  `ASAP` scheduling.

### 19.8 Tests

- `desctest` (host), `UacPlanQueue`: 19.4's table; clamping of 0, 15, 16,
  17, 33, 127, 128, 129 and `0xFFFFFFFF`; never fewer than 2 or more than
  32; the ring bound at its boundary (a ring of exactly `(n + 1) x B`
  bytes allows `n`), the 4096-byte ring, the 1 MiB cap with 384 kHz 8
  channels 32-bit, Full Speed and `bInterval` 4 and 5.
- `streamtest` (host): the ring is the old size at 16 and 32 ms and grows
  by the queue above 32 ms, whole frames, within the cap.
- An INF check (host, in `test.cmd`): both INFs write `Uac2QueueMs` in
  every install section, no-clobber, at 32 (the NT `DWORD` form in the NT
  sections, the 9x binary form in `[UAC2]`), and the driver's default is
  32.
- VM legs, XP x64 under xHCI98 with the CX31993 simulator: the old
  driver, the queue alone, the queue and the wake increment, and the
  test-only real-time build, under disk load, CPU load, both and neither
  (xHCI98's Ring Underrun and Missed Service counts, the simulator's gaps
  between OUT packets, each 60 s play's elapsed time); `Uac2QueueMs` 16,
  32 and 128 read back from the stats; then play, record, ramp and seek
  on XP x64 (and under Windows' EHCI there) and on a 32-bit system.

## 20. No gain above 0 dB: the top of the volume range (build 25, 2026-11-05)

### 20.1 The report

A user on Windows XP x64 SP2, xHCI98 3.4.0.0 on a Fresco FL1100, `uac2`
build 24: a headphone dongle with a CX31993 DAC and a MAX97220 headphone
amplifier (`3302:12C3`, "USB Audio") sounds distorted, even at a low
volume, and badly at the maximum. A plain CX31993 dongle on the same
system is fine. XHCITR's report holds the dongle's descriptors
(`src/audio/uac2/testdata/cx31993m`): Feature Unit `0x10` has volume on
each channel and nothing on the master (no mute), an Extension Unit
(`wExtensionCode` `0x0BDA`) sits between it and the headphones, and the
one playback interface is ADAPTIVE at 16, 24 and 32 bits. The volume
RANGE is a control answer, not a descriptor, and the report does not
have it.

### 20.2 What the driver did

Up to build 24 the volume node's KS range was the device's RANGE as
read (section 5.3, times 256), up to the highest step the device
reaches. A device that lists gain above 0 dB (a RANGE MAX above 0) was
offered that gain and set to it.

What Windows XP does with that range was measured on the emulator (XP
x64, `uacemu.py --profile cx31993m`, a 440 Hz tone at -1 dBFS from
`UACWAV`, the mixer set through the mixer API as the Volume Control
sets it). The mixer's slider maps to the KS range from its top:
level = MAX + 20 log10(v / 65535) x W dB, snapped to a step, with W
= 1 for the ranges of 60 and 66 dB measured and (MAX - MIN) / 96 for
those of 127 and 139 dB. With -96..0 dB, 32768
gave -6 dB. With -127..+12 dB, 32768 gave +3 dB, 6554 gave -17 dB, and
the device's own 0 dB read back as 25240 (38 %). So on such a device
the top 62 % of the slider is gain:

| Range (simulated) | Build | Slider 100 % | 50 % | 10 % | Tone samples past full scale (100 % / 50 % / 10 %) |
|---|---|---|---|---|---|
| -127..+12 dB, 1 dB | 24 | +12 dB | +3 dB | -17 dB | 82 % / 42 % / 0 |
| -60..+6 dB, 0.5 dB | 24 | +6 dB | 0 dB | -14 dB | 62 % / 0 / 0 |
| -127..0 dB, 1 dB | 24 | 0 dB | -8 dB | -26 dB | 0 / 0 / 0 |
| -127..+12 dB | 25 | 0 dB | -8 dB | -26 dB | 0 / 0 / 0 |
| -60..+6 dB | 25 | 0 dB | -6 dB | -20 dB | 0 / 0 / 0 |
| -127..0 dB | 25 | 0 dB | -8 dB | -26 dB | 0 / 0 / 0 |

The PCM the driver sent was clean in every row: THD+N -90.5 dB at -1
dBFS, alternate 1 (16-bit), 48 kHz, 24-byte packets in every
microframe. That rules out the adaptive packet sizes and the sample
packing at that format. "Past full scale" counts the samples that the
volume in force takes over full scale, which a DAC that clips
digitally cuts off. With the tone at +12 dB, THD+N after the cut is
-9.4 dB; at +3 dB it is -20.7 dB. A gain above 0 dB on a full-scale
signal is distortion at any loudness the amplifier then makes, and
this device's amplifier stage makes a lot. The cause is not proven on
the user's unit until its RANGE is read (20.5); it is the one cause
the descriptors, the symptom and these numbers agree on.

### 20.3 The change

- **The top offered is 0 dB.** `UacVolRange` keeps the device's
  highest step as `KsTop`, and sets `KsMax`, the top the KS node
  reports and the driver writes, to the highest step at or below 0 dB
  across the subranges. Where no step is at or below 0 dB (a range
  that starts above it), `KsMax` stays `KsTop`. Every level is clamped
  to `KsMax` (`UacVolFromKs`), so a SET, a restored `Uac2Level` or the
  0 dB a master-mode extra channel is normalized to never writes gain.
  For a device whose range tops out at or below 0 dB nothing changes.
- **A device left above it is lowered at start.** At start, each
  volume control the driver writes (the master, the channels, or a
  master-mode extra with a RANGE) whose CUR reads above `KsMax` is set
  to `KsMax`. Only that control is set. Nothing is taken over (the
  driver does not own the volume until a SET, section 5.3), and no
  other control changes, so a master-mode extra keeps its attenuation:
  every channel ends at most as loud as before. A CUR that cannot be
  read is left as it is, because writing a target computed without it
  could raise the level. The device keeps its own setting wherever
  that setting is inside the range offered.
- **The unknown is assumed at the device's top.** Where a CUR is not
  known, the software gain takes the device to be at `KsTop`, not
  `KsMax`, so it is never louder than asked (`Assumed`).
- **`Uac2VolAboveZero`** in the driver key (`REG_DWORD`, or 4-byte
  `REG_BINARY` on 9x, read as `Uac2QueueMs` is). 1 offers the whole
  range again (`KsMax` = `KsTop`), gain included; anything else, 0 by
  default, tops it at 0 dB. It is read when the device starts. Every
  install section writes 0 no-clobber, next to `Uac2QueueMs` and
  `Uac2Realtime` (`[Queue9x]`, `[QueueNT]`), so the setting is visible
  and an update keeps a user's 1.

Linux's PulseAudio is commonly configured to treat a mixer's 0 dB as
100 % and the steps above it as over-amplification. That is the same
policy, but it is not a source for this change; the change rests on the
measurements above.

### 20.4 What is not changed

The RANGE requests, the channel/master choice (sections 5.3 and 15),
software volume, mute and the streaming path. The Extension Unit stays
a pass-through node (section 4.1). Its one control on this device
(enable) is read-only, so there is nothing to set.

### 20.5 What the driver records

- `Uac2VolMap` (written at each level change) gains `KsTop`, then
  `AboveZero` (the option in force) and `Lowered` (1 when the start
  lowered a control), after the existing fields.
  `KsMax` there is now the top offered.
- `Uac2UsbStats` / `Uac2UsbStatsIn` (written at each stream stop) grow
  from 65 to 88 words, append-only. Word 65, `VolLayout`, is 25 when
  the words after it are filled. Then come `VolUnit`, `VolMode` (0
  software, 1 master, 2 per channel), `VolChannels`, `VolAboveZero`,
  `VolLowered`, `VolRangeRead` and `VolStartKnown` (bit n: slot n's
  RANGE was read / its CUR was read at start; slot 0 is the master, 1
  and 2 the logical channels). Then, three words each for slots 0, 1
  and 2: `VolCount` (subranges), `VolMin` (first MIN, silence read as
  the next step), `VolMax` (last MAX), `VolRes` (smallest RES),
  `VolTop`, `VolOffered`, and `VolStartCur` (CUR at start), all in
  1/256 dB, signed. `VolStartCur` is the CUR as read, before any
  lowering. So the next report from a device shows its RANGE and its
  level at plug-in.

### 20.6 Tests

- `voltest` (host): ranges of -127..+12, -60..+6, -16.5..+5.5 (no step
  at 0 dB: the top is -0.5 dB), +1..+6 dB (nothing at or below 0: the
  whole range) and two subranges either side of 0 dB. `KsTop` and
  `KsMax` for each. A sweep over every level checks that the answer is
  never above `KsMax` and is the brute-force nearest step. The same
  sweep with `KsMax` = `KsTop` (the `Uac2VolAboveZero` case) reproduces
  build 24. Existing vectors whose range reached above 0 dB now expect
  0 dB at the top.
- `infchk` (host): `Uac2VolAboveZero` is written once, no-clobber, at
  0, in every install section of both INFs. Six more mutations are
  refused per INF.
- `uacsimtest` (host): the `cx31993m` profile through the driver's
  parser (three adaptive alternates, 248/372/496 bytes), its volume
  map (per channel on unit `0x10`, no mute, output `0x0F` across the
  Extension Unit, headphones), and the simulator's rendering and
  over-full-scale count through that unit.
- Emulator legs on XP x64 under xHCI98 (20.2's table; the plug-in
  lowering, `Uac2VolAboveZero` = 1 and devices whose range tops at
  0 dB unchanged: `docs/testing/2026-11-05-uac2-gain.md`).

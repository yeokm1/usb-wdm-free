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
`usbaudio.sys` (Owner, 2026-10-20: not handled by this driver in v1;
revisited 2026-11-07, section 26).

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
  16.16 in 4 bytes at High Speed (samples per microframe); at Full Speed
  `uac2` also takes 16.16 in 4 bytes, per frame (section 23). Linux detects
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
10.14 in 3 bytes or, from devices made for Windows, 16.16 in 4 (section
23), and Microsoft's stacks accept only `bInterval` 1 for Full-Speed
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
- **Q5, UAC 1 devices.** Not handled by this driver in v1. Revisited
  2026-11-07: section 26.
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

### 15.1b Linux since the capture (2026-11-06, xHCI98 task 47.18)

Read again at Linux commit `f66469d9` (`torvalds/linux` master,
2026-11-06; `sound/usb/quirks.c`, `usbaudio.h`, `stream.c`,
`mixer_quirks.c`, `mixer_maps.c` and `card.c`, all GPL-2.0-or-later;
read for facts, nothing adapted), against `c339f401` of 15.1:

- **One new entry for an ALC4080.** `quirks.c` gives `0DB0:A073` (the
  MSI MAG X570S Tomahawk and Torpedo Max, "ALC4080")
  `QUIRK_FLAG_SKIP_IFACE_SETUP`. `usbaudio.h` defines it as skipping the
  probe-time interface setup (`usb_set_interface`, `init_pitch`,
  `init_sample_rate`) as redundant with the setup done when a stream
  opens. Without it, `stream.c` does for each format it finds: alternate
  0; for a USB Audio 2.0 device (no `QUIRK_FLAG_SET_IFACE_FIRST`) the
  pitch and the format's highest rate; then the format's alternate, left
  selected. With it, none of that happens until a stream opens.
- **No start-up request, no jack or output selection.** No ALC4080 id
  reaches `mixer_quirks.c`. Its Realtek HD Audio verb requests (62, 72,
  88, 89) and the jack controls built on them are still the ALC4020's of
  two docks (`0BDA:4014`, `0BDA:402E`, a USB Audio 1.0 device). `card.c`
  and `mixer_maps.c` change names only (15.1).

What `uac2` does with it: nothing new is needed for what Linux shows. At
start `uac2` selects the configuration with every interface at alternate
0 (`Configure` in `uacusb.c`) and reads the clock entities' ranges; it
sets a rate and selects a streaming alternate only when a stream starts
(`UacUsbStart`: `SetClock`, which writes the frequency only when the
clock's current one differs, then `SelectAlt`), and goes back to
alternate 0 when the stream stops. So `uac2` never does the probe-time
setup the quirk skips, on any device. For `0DB0:A073` the files read add
no vendor initialisation either. Whether that board's topology is the
captured one is not known: one streaming interface and terminal per jack,
each on its own clock and with no selector, is what the capture of
`0B05:1B9B` shows (15.1a), and another board may differ.

Read on XP x64 under xHCI98 3.4.1.0 with `uac2` 0.1.8.26, 2026-11-06
(the rig of [uac2-swap](../testing/2026-11-06-uac2-swap.md)): the capture
replayed (`uacemu.py --profile alc4080cap --vid 0db0 --pid a073`) installed
three output and three input devices; a 5-second play (2 channels, 16-bit,
48 kHz) on each output and a 3-second record on each input passed, the
simulator streaming interfaces 4, 5 and 6 and 1, 2 and 3.

Still owed (15.6 step 3): the same on 98 SE under NUSB's EHCI and under
xHCI98 and on 7 x64; the real board (xHCI98 45-E.13).

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
  the rule off under a real-time worker). A stack that completes URBs
  inline again and again with nothing delivered (capture with zero-length packets, no `Fail`) leaves `Wake`
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
microframe: the payload the driver builds (sample values, packing,
frames a packet) is clean at that format. The simulator does not play
packets against a DAC clock, and the emulated controller is not the
user's FL1100, so packet timing on that controller is not measured
here. "Past full scale" counts the samples that the
volume in force takes over full scale, which a DAC that clips
digitally cuts off. With the tone at +12 dB, THD+N after the cut is
-9.4 dB; at +3 dB it is -20.7 dB. A gain above 0 dB on a full-scale
signal distorts however low the analog volume after it is. This is a
hypothesis for the user's unit, not a finding. It holds only if that
unit's RANGE goes above 0 dB, which the next report shows (20.5).
Packet timing on the user's controller is a separate candidate that
these legs do not measure. Build 25's change is safe either way: for
a device whose range tops out at 0 dB it changes nothing.

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
  from 65 to 94 words, append-only. Word 65, `VolLayout`, is 25 when
  the words after it are filled. Then come `VolUnit`, `VolMode` (0
  software, 1 master, 2 per channel), `VolChannels`, `VolAboveZero`,
  `VolLowered`, `VolRangeRead` and `VolStartKnown` (bit n: slot n's
  RANGE was read / its CUR was read at start; slot 0 is the master, 1
  and 2 the logical channels). Then seven arrays of three words (slots
  0, 1 and 2), words 73 to 93, in this order: `VolCount` (subranges), `VolMin` (first MIN, silence read as
  the next step), `VolMax` (last MAX), `VolRes` (smallest RES),
  `VolTop`, `VolOffered`, and `VolStartCur` (CUR at start), all in
  1/256 dB, signed. `VolStartCur` is the CUR as read, before any
  lowering. So the next report from a device shows its RANGE and its
  level at plug-in.

Readings since: a "CX31993+MAX97220 PRO" (2026-11-17, `uac2` 0.1.11.0;
Feature Unit 2, no Extension Unit, so not the layout of 20.1) reads
-74.0 to 0.0 dB in 0.5 dB steps on both channels: no gain above 0 dB, so
the cap and `Uac2VolAboveZero` change nothing on it; the range of the
unit of 20.1 is still unread ([status](../status.md)).

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

## 21. The device's name: its USB product string (build 26, 2026-11-05)

### 21.1 The problem

Up to build 25, every `uac2` device appeared under the INF's one name,
"UAC2 USB audio (experiment)" ("... 2" and so on for a later endpoint
pair, section 15.3). That name showed in Sounds and Audio Devices, in
the Vista+ sound panel, in `waveOutGetDevCaps` and in the device menus
of applications such as VLC. Devices on Windows' own USB Audio 1.0
driver show their product names.

### 21.2 Where each system takes the name

Measured on the emulator (21.5):

- **98 SE and XP x64** show the `FriendlyName` of the filter's
  `KSCATEGORY_AUDIO` device interface key. That is where the INF writes
  its name (`AddInterface=%Audio%,Wave,...` with
  `HKR,,FriendlyName,,%Device%`; `HKR` of an `AddInterface` section is
  the key `IoOpenDeviceInterfaceRegistryKey` opens). Writing it there
  before the interface arrives changed `szPname` and the control
  panel's list on both systems. 98 SE, 2000 and XP share this WDMAud
  path; 2000 and XP x86 were not run.
- **7 x86** ignores that value. The endpoint is named "Headphones
  (<devnode FriendlyName>)", read at each arrival. With the interface
  keys renamed, the panel kept "4- UAC2 USB audio (experiment)". With
  the devnode's `FriendlyName` set by hand (as SYSTEM) and the device
  replugged, the panel and `szPname` showed "Headphones (Devnode Named
  DAC)". Vista, 8 and 8.1 have the same audio endpoint builder but
  were not run.

Not used:
- `KSPROPERTY_GENERAL_COMPONENTID` with a `MediaCategories` Name GUID,
  which needs a registry entry per name.
- `IoSetDevicePropertyData`, which exists from Vista only and would
  need `MmGetSystemRoutineAddress`. The one x86 binary cannot import
  that, because 98 SE's NTKERN lacks it (98 DDK `wdm.lib`).

### 21.3 The change

- **The string.** At each start (`ReadName`, before the first pair
  registers), the device descriptor is read for `iProduct`, then string
  descriptor 0 for the languages: 0x0409 when it is listed, else the
  first, and 0x0409 when the list cannot be read. Then the product
  string is read. Each read goes through `UacUsbDescriptor`, the
  transport's request path: cancelled after 5 s and drained, as every
  request of the driver.
- **The name** (`UacName`, uacdesc.c, host-tested):
  - The UTF-16 string is cleaned: control characters (below U+0020,
    U+007F to U+009F) become spaces; surrogates, U+FEFF, U+FFFE and
    U+FFFF are dropped; runs of spaces collapse to one and the ends are
    trimmed.
  - It is then cut to fit 31 characters, the most `szPname` holds
    (`MAXPNAMELEN` 32). A later pair's " 2" to " 4" stays inside those
    31, cut from the name before it.
  - A malformed descriptor, or one with nothing printable left, gives
    no name.
- **The interfaces.** The name is written as `FriendlyName` on the
  pair's two `KSCATEGORY_AUDIO` interfaces (reference strings
  `Wave`/`Topology`, `Wave2`/`Topology2` and so on), before
  `PcRegisterSubdevice` enables them. `IoRegisterDeviceInterface`
  returns the link PortCls registers next, and
  `IoOpenDeviceInterfaceRegistryKey` opens it.
- **The devnode, Vista and later.** When `IoIsWdmVersionAvailable(6,
  0)`, pair 0's name is also written as `FriendlyName` in the devnode's
  instance key (`NameDevnode`). The key's path comes from the devnode's
  `Device Parameters` key (`PLUGPLAY_REGKEY_DEVICE`, `ZwQueryKey`
  `KeyNameInformation`) less its last component, and is opened with
  `ZwOpenKey`. Device Manager reads the same value; that was not
  checked here. Earlier systems are left alone.
- **Fallback.** Without a usable string (no `iProduct`, a read that
  fails, nothing printable), the INF's own name is written in both
  places, replacing a name from an earlier start. Like every write
  here, this is best effort: a failed interface write is not recorded,
  and the start goes on. The driver's copy of that name is checked
  against both INFs' `Device` string by `infchk.ps1`. The name is
  "USB Audio 2.0 (uac2)" (`uac1`: "USB Audio 1.0 (uac1)"), a later pair
  " 2" to " 4", and the install disk "usb-wdm-free uac2 driver" (owner,
  2026-11-09); before that it was "UAC2 USB audio (experiment)"
  ("UAC1 USB audio (experiment)"), as the records up to 2026-11-09 show.
  From 2026-11-15 the name is "USB Audio 2.0 (wdm-free)" ("USB Audio 1.0
  (wdm-free)"), and every name the driver writes carries that mark
  (21.6).
- **Recorded** in the driver key:
  - `Uac2Name`: the name chosen for pair 0, recorded before it is
    written (not proof that the writes succeeded).
  - `Uac2NameStatus`: 0 the product string, 1 no `iProduct`, 2 device
    descriptor unread, 3 string unread, 4 nothing printable.
  - `Uac2NameDevnode`: the devnode write's status, `STATUS_NOT_SUPPORTED`
    (0xC00000BB) before Vista.
- **Imports.** `IoRegisterDeviceInterface`,
  `IoOpenDeviceInterfaceRegistryKey`, `ZwQueryKey` and `ZwOpenKey` are
  all in the 98 DDK's `wdm.lib`, the NTKERN export set. Build 26 loaded
  and named its devices on 98 SE.
- **Test tools.** UACWAV and UACSEEK found their device by "UAC2" in
  the name. That still holds for the built profiles, `x1` and `x4`, but
  not for `alc4080cap` ("ALC4080 capture - SIMULATED"), a `--product`
  without "UAC2" or a real device. For those, `UACWAV_NAME=<part of
  the shown name>` names the device to use.
  `uacemu.py --product TEXT` / `--no-product` set the simulated
  `iProduct`.

### 21.4 Not covered

- Vista, 8 and 8.1, 2000, XP x86 and ME were not run. Each shares a
  measured system's path (21.2).
- An endpoint the Vista+ endpoint builder already holds keeps its
  cached properties until it arrives again; the next plug or start
  shows the new name (seen on 7).
- Real devices.

### 21.5 Tests

- `desctest` (host), `UacNameLanguage` and `UacName`:
  - lists with 0x0409 first, later or absent;
  - malformed lists and strings (type, odd or short `bLength`, past the
    buffer, empty);
  - control characters, collapsing and trimming;
  - non-ASCII kept and surrogates dropped;
  - the 31-character cut with and without " 2", " 10";
  - a sweep of every `bLength` checking the length cap, the NUL and
    that no control character or end space survives.
- `infchk`: the driver's fallback equals both INFs' `Device` string.
- `uacsimtest`: `--product` / `--no-product` and the string over
  `GET_DESCRIPTOR`.
- Emulator legs (`docs/testing/2026-11-05-uac2-names.md`): XP x64, 7
  x86 and 98 SE, with distinct product strings, no product, an empty
  product, a long one, and a rename on the same instance.

### 21.6 The project's mark, "(wdm-free)" (owner, 2026-11-15)

Every name the project's drivers show carries "(wdm-free)", so that a
device on one of them can be told from one on a maker's driver from
usb-wdm-nonfree. Where a name had the driver's short name in brackets,
the mark replaces it; elsewhere it is appended.

- **The INFs.** `Device` is "USB Audio 2.0 (wdm-free)" (`uac1`: "USB
  Audio 1.0 (wdm-free)"), `Device2` to `Device4` "USB Audio 2.0
  (wdm-free) 2" to "... 4": 24 to 26 characters.
- **The driver's names.** `UacName` appends `UAC_NAME_MARK` (`uacdesc.h`,
  " (wdm-free)") to the cleaned product string, and a later pair's " N"
  after it, as the INF does: "Sound Blaster X1 (wdm-free)", "Sound
  Blaster X1 (wdm-free) 2". The 31 characters `szPname` holds leave 20
  for the product string (18 with " 2" to " 9"). A longer one first
  loses the maker's name from its front (21.7: "Creative Sound Blaster
  X4" shows as "Sound Blaster X4 (wdm-free)"), and only what is still
  too long is cut from the end. The mark is never cut. These are the names of 21.3: the interfaces'
  `FriendlyName` (the wave and mixer devices on 98 SE to XP x64) and,
  on Vista and later, the devnode's `FriendlyName` (Device Manager, and
  the endpoint, "Speakers (Sound Blaster X1 (wdm-free))", which WinMM
  cuts at 31 in `szPname`).
- **The fallback.** `NameFallback` is the INF's name less the mark ("USB
  Audio 2.0"); `UacName` adds the mark to it as to a product string.
  `infchk.ps1` reads the mark from `uacdesc.h` and checks that each
  INF's `Device` is `NameFallback` and the mark, `Device2` to `Device4`
  it numbered, each at most 31 characters.
- **Settings kept by name.** Windows keeps the preferred playback and
  recording device by name (22.2, `Sound Mapper`'s `Playback` and
  `Record` on XP x64; `UserPlayback` on 98 SE, read with the mark in
  the record below). A device installed before the rename no longer has
  the name stored there once the new driver names it, so Windows can
  take another device as the default until the user picks the renamed
  one again; the no-clobber `SetupPreferredAudioDevices` of an update
  does not take it back (22.3). Vista and later were not read.
- **Test tools.** UACWAV, UACSEEK and UACPLAY also take a device whose
  name holds "(wdm-free)" when `UACWAV_NAME` is not set, so a device
  shown under the fallback is found as before.
- **Tests.** `desctest`: the mark on a product string, the fallback and
  its pairs, the cut at 20 and 18 characters with a space at the cut,
  no name when only the mark would fit, and the sweep checks that every
  pair-0 name ends with the mark. Emulator legs:
  [wdm-free-names](../testing/2026-11-15-wdm-free-names.md).

### 21.7 A name too long: the maker's name off first (owner, 2026-11-15)

The owner, on "Creative Sound Blaster X4" shown as "Creative Sound Blast
(wdm-free)": "It should be called Sound Blaster X4. Chop the manufacturer
name instead."

- **The maker's name.** At start, after the product string, `ReadName`
  reads the device's `iManufacturer` string in the same language
  (`UacUsbDescriptor`, 5 s at most) into the extension (`Maker`). None
  is read when the product string is not used, `iManufacturer` is 0 or
  the read fails.
- **The rule** (`UacNameFrom`, `UacNameMaker`, `uacdesc.c`, host-tested):
  only when the cleaned product string does not fit the room before the
  mark (20, or 18 with " 2" to " 9"), its front loses the most leading
  whole words of the cleaned maker's name that it begins with (any case),
  where the product goes on with a separator (space, `-`, `,`, `:`, `;`,
  `_`, `/`), and the separators after them: "Creative Technology Ltd"
  and "Creative Sound Blaster X4" share "Creative", so the name is
  "Sound Blaster X4 (wdm-free)". If the rest still does not fit, it is
  cut from the end as before. A name that fits keeps the maker's name
  ("Creative Pebble X (wdm-free)").
- **Not taken off:** a maker's word that is only part of a product's
  word ("C-Mediatek ..." by "C-Media Electronics Inc."), a product that
  is all the maker's name or a leading part of it ("Creative Technology
  Ltd" stays and is cut from the end), and a strip that would leave no
  letter or digit. With no maker's string, or none shared, the name is
  cut from the end as in 21.6.
- **What stays:** a later pair's " N" after the mark; the fallback name,
  which has no maker. Each pair's name is built alone, so a product of
  19 characters keeps the maker on pair 0 (room 20) and loses it on pair
  1 (room 18).
- **Tests:** `desctest`: the X4 (pairs 0, 1 and 9), the X1, a fitting
  name that keeps its maker, no maker's string and an unreadable one, a
  product equal to the maker's name and one that is a leading part of
  it, no shared prefix, whole words only, any case with " - ", the strip
  then the cut with a pair, and nothing left after the separators;
  `UacNameMaker` alone. Emulator legs in the record of 21.6.

## 22. Windows' preferred device: `SetupPreferredAudioDevices` (xHCI98 task 47.11, 2026-11-06)

### 22.1 The report and the legs

A user reported, on XP x64, that swapping to a different USB audio device
needs VLC restarted, and that a swap to or from a UAC 1.0 device under
`usbaudio` does not. The legs on XP x64
([uac2-swap](../testing/2026-11-06-uac2-swap.md): the real X4 and X1 and a
UAC 1.0 headset passed through, VLC 3.0.11) read:

- **One device at a time:** no difference. Whatever was swapped in, under
  `uac2` or `usbaudio`, was the only sound device and so Windows' default,
  and VLC reached it at its next Stop and Play.
- **Two devices attached:** the headset (`usbaudio`) attached after the
  X1 was Windows' default playback device at once; the X1 (`uac2`)
  attached after the headset was not, and VLC's Default output stayed on
  the headset. A default chosen by hand in Sounds and Audio Devices
  (Audio, Default device) was not followed by the running VLC at Stop and
  Play, in either direction (to the X1 and to the headset); a VLC restart
  reached it.

So `uac2` differs from `usbaudio` in one measured way: a `usbaudio` device
takes Windows' default, a `uac2` device does not. On a PC with another
sound device this leaves the user to pick the `uac2` device by hand, and
VLC then needs a restart to follow (the second point). That this is the
user's symptom is a likely reading, not a reproduction of his PC.

### 22.2 Where the difference comes from

Windows' own USB audio INF (`wdma_usb.inf`, read as text in the XP x64
guest; the 98 SE copy carries the same line, `2026-10-22-usbmidi-m1.md`)
has `HKR,,SetupPreferredAudioDevices,3,01,00,00,00` in its install
sections; `uac2wdm.inf` has no such line. Microsoft's driver kit
documentation (*SetupPreferredAudioDevices*, audio device INF keywords):

- the keyword makes the device's functions the defaults for sound
  playback, sound recording and MIDI music playback when the driver is
  installed, displacing the device that held the role; the user can
  change them in Control Panel;
- the value must be nonzero; flags 3 (binary, no-clobber) keep a device's
  existing preferred-device entries when its driver is merely upgraded;
- it is supported by 98/ME, 2000, XP and Vista, not by Windows 7;
- on Vista any endpoint whose value is odd can become the default, and
  the KS filter exposed last decides.

In the guest, after both devices' installs, `HKCU\Software\Microsoft\
Multimedia\Sound Mapper` held `Playback` and `Record` = "Logi USB
Headset", the X1's driver key `SetupPreferredAudioDevicesCount` 0
(written by Windows). Adding the value by hand to the already installed
X1's key changed nothing at its next arrival, as the documentation's
"when the driver is installed" says. The preferred device is kept by
name, so the headset was the default again at each later arrival.

### 22.3 The change

`[AudioSupport]` of both INF copies (`uac2wdm.inf`, and `x64\uac2wdm.inf`
for the 64-bit systems; every install section, 9x, 2000, XP and x64,
uses it) gains:

    HKR,,SetupPreferredAudioDevices,3,02,00,00,00

- **Value 2, not 1.** Nonzero, so 98 SE, ME, 2000, XP and XP x64 act on
  it; even, so Vista's endpoint builder does not (22.2). `uac2` registers
  pair 0 first and later pairs after it (15.3), so on Vista a value of 1
  could make a later pair (the X4's S/PDIF, say) the default; Vista keeps
  its own choice instead. 7 and 8 do not read it.
- **Flags 3** as Windows' INF: a later update of a device whose driver key
  already holds the value does not take the defaults again.
- **What users see:** on 98 SE to XP x64, installing `uac2` for a device
  makes it the default for playback, and for recording where it records,
  as a UAC 1.0 device under Windows' own driver does. MIDI too may move:
  the INF registers WDMAud's MIDI path (`Drivers\midi`), through which
  Windows' software synth plays on the device
  ([uac2-swmidi](../testing/2026-10-28-uac2-swmidi.md)), and the keyword
  covers the MIDI default; 22.4 reads it. This is a behaviour change (release notes). Updating a
  device installed with an older `uac2` writes the value for the first
  time: whether Windows takes that as an install and moves the defaults
  once is read in 22.4.
- **Which pair.** A device with several pairs (15.3) registers several
  wave filters; the one meant to become the default is pair 0, the first
  output in descriptor order (the X4's analog output, not its S/PDIF).
  22.4 reads which one Windows takes on XP.
- Windows reads the value, the driver does not: no tuning value, no
  `tuning.md` row.

### 22.4 Tests

- The INF gate (`infchk.ps1`) requires the line in `[AudioSupport]` of
  both copies and rejects its removal or a value of 0.
- XP x64, the real headset installed and Windows' default:
  - a `uac2` device new to the system (the simulated CX31993 with a
    MAX97220 under a new id) installed with the changed INF: the Audio
    tab's default playback device is it, recording stays the headset's
    (the profile has no capture); a VLC started before the install plays
    to it at its next Stop and Play;
  - a duplex `uac2` device (the `hs24` profile under a new id): playback
    and recording both it;
  - a device with two playback pairs (the `x4full` profile under a new
    id): playback is pair 0's waveOut, and a play to the default reaches
    pair 0's endpoint at the simulator;
  - each case also records the MIDI default (`MIDIMap`) before and after;
  - the real X1, installed earlier without the value, updated to the
    changed INF: the defaults before and after, recorded either way.
- Not read: ME, 2000, Vista. 98 SE: read below.

Read on XP x64, 2026-11-06 ([uac2-swap](../testing/2026-11-06-uac2-swap.md)):
each case as expected, with the released INF as the control (the default
stays the headset's), and the real X1 installed fresh with the change
taking playback and recording; the update of an installed X1 left the
defaults alone; MIDI stayed Windows' GS synth throughout. The `Sound
Mapper` registry values were not seen to change when read soon after; the
Audio tab is the reading.

Read on 98 SE under xHCI98 3.4.1.0 and under NUSB's EHCI, 2026-11-06
(simulators, same record): a new device installed with the changed INF
became the preferred playback and recording device, one with the released
INF did not; updating a device that was not the preferred one left the
choice alone until the restart it asked for (after it, under xHCI98, the
recording choice had moved, cause not settled; under NUSB a new device
installed at that start took both). 98 SE installs further devices from
its one `INF\OTHER` copy of the INF, so the changed INF reaches a system that has `uac2` already
by Have Disk or Update Driver once; Windows sets the value to 0 after
acting on it; updating the preferred device itself moved the preference to
another device while its driver was replaced, and from one starting state
the released INF does the same (D / D before the restart, A / A after a
cold start, both INFs): that move is 98 SE's own. With the other device
installed with the changed INF and the user's choice made by hand, the move
lasted past the cold start in one run and not in a repeat from a clean
state; with that device on the released INF the user's choice came back
after the start. So the change is not shown to make the move last; after
updating the preferred device's driver on 98 SE the user may have to check
the Multimedia panel. ME and 2000 not read.

## 23. Full Speed feedback in 4 bytes (xHCI98 Phase 47, 2026-11-07)

### 23.1 The reading

USB Audio 2.0 section 3.16.2.2 takes the explicit feedback format from
USB 2.0 section 5.12.4.2: at Full Speed 3 bytes, 10.14 samples per frame;
at High Speed 4 bytes, 16.16 samples per microframe. Windows 10's own
UAC 2.0 driver does not start a Full Speed device whose feedback endpoint
is 3 bytes (Code 10 on the simulator's `fs` profile), and starts it when
the endpoint is 4 bytes carrying 16.16 samples per frame (the simulator's
`--fs-feedback 4`; xHCI98 Phase 47, the inbox reference runs). Devices
made for Windows therefore send 4 bytes at Full Speed. Up to 0.1.9.0
`uac2` asked 3 bytes per Full Speed feedback packet and decoded only 3, so
such a device's feedback was too long for the request.

### 23.2 The change

- **Request.** Per feedback packet the driver asks 4 bytes at High Speed;
  at Full Speed 4 bytes when the feedback endpoint's capacity
  (`wMaxPacketSize` bits 10..0) is at least 4, else 3. The packet stride,
  the request's length and the feedback slots' allocation all follow that
  one value (`UacRateFeedbackStride`). An endpoint below 3 bytes at Full
  Speed or 4 at High Speed is not runnable, as before (4.3).
- **Decode by the length received.** A Full Speed packet of 3 bytes is
  10.14, one of 4 bytes is 16.16, both in samples per frame; High Speed
  takes 4 bytes 16.16 per microframe only, as before. Any other length, a
  length above the stride, or a packet outside the slot's allocation is
  rejected before the buffer is read (`UacRateFeedbackPacket`).
- **Bounds unchanged.** A value is accepted only within 12.5% of the
  nominal rate (the driver's policy, not the specification's) and only if
  one service interval at that rate fits the data endpoint, both checked
  before the value is multiplied. A rejected value leaves the last valid
  cadence and does not count as feedback for the deadline (`uacwatch.h`:
  1 s before the first valid value of a stream, 200 ms after); a
  zero-length packet counts only once a valid value has arrived (4.5).
- **Not taken:** a 4-byte Full Speed value in 10.14 (a device padding the
  USB 2.0 form). It decodes to a quarter of the rate, is rejected by the
  window, and the stream ends at the deadline as any device without
  usable feedback does.

### 23.3 Tests

- `rtest.cmd`: Full Speed 3 and 4 bytes at 44.1 and 48 kHz, fractional
  values in both forms giving the same cadence, a 4-byte value read up to
  a guard page's edge, lengths 0 (before and after a valid value), 1, 2, 5
  and above the stride, packets outside the buffer, values outside the
  window or the endpoint, the stride per speed and capacity, and the High
  Speed rules unchanged (3 bytes rejected there); with `uacwatch.h`'s
  deadline, a stream fed only rejected packets is late at 1 s, and one
  fed valid values and keepalives is not.
- Legs on 98 SE and XP, under NUSB's or Windows' EHCI and under xHCI98
  3.4.1.0, against the simulator's `fs` profile with 3-byte and with 4-byte
  feedback: playback, recording, a rate change and unplug; the record says
  which.

Read 2026-11-07 ([uac2-fs-feedback](../testing/2026-11-07-uac2-fs-feedback.md)):
on 98 SE under xHCI98 and under NUSB's stack (on UHCI: QEMU's EHCI takes
no Full Speed device) and on XP under xHCI98, both endpoint sizes played,
recorded, survived an unplug and played again, and the packets followed
the feedback (49 frames in about 71% of the packets at 48 kHz with the
device 1.5% fast); a device at 44.1 kHz likewise on 98 SE. The released
0.1.8.26 fails the 4-byte endpoint. On XP's own UHCI playback fails with
either size, and 0.1.8.26 fails the 3-byte case the same way; the cause
is not settled.

## 24. 98 SE and ME: no KS file prompts (2026-11-07)

Installing `uac2` on 98 SE asked for `ks.sys`, then `ksclockf.ax`, and
neither was to be found on the Windows CD's `\WIN98` folder or in
`C:\WINDOWS\OPTIONS\CABS` (owner, 2026-11-07). The record
`docs/testing/2026-11-07-98se-ks-prompts.md` has the runs.

**Cause.** The 9x section included Windows' `ks.inf` and `wdmaudio.inf`
and needed their `KS.Registration` and `WDMAUDIO.Registration` sections.
On 9x, Setup looks for the files those sections copy (`ks.sys`,
`ksproxy.ax`, `ksclockf.ax` and the rest) and for the included INFs'
catalogs (`ks.cat`, `wdmaudio.cat`) on this package's own disk: the
prompt reads "The file 'ks.cat' on UAC2 experimental build folder cannot
be found". The files are Windows' own and sit inside the CD's CAB files
(`BASE4.CAB`, `BASE5.CAB`, `DRIVER11.CAB`, `CATALOG3.CAB`, ...), never
loose, so pointing the prompt at the CD's `\WIN98` does not find them.
Skipping them was harmless only because Windows brings the same files
itself (below). Including `wdmaudio.inf` alone still asks for
`wdmaudio.cat`.

**What a clean 98 SE has.** Before any audio install: `ks.sys`,
`portcls.sys`, `stream.sys`, `sysaudio.sys`, `kmixer.sys`, `swmidi.sys`,
`wdmaud.sys`, `wdmaud.drv` and `ksuser.dll` are present; the DirectShow
side of KS (`ksproxy.ax`, `ksclockf.ax`, `ksdata.ax`, `ksinterf.ax`,
`ksvpintf.ax`), `mspclock.sys`, `mskssrv.sys` and the catalogs `ks.cat`
and `wdmaudio.cat` are not, and no software device (SysAudio, KMixer,
the synth, the streaming clock and service proxies) is installed.

**What brings them.** `WDMAUDIO.Registration` copies nothing: it writes
a RunOnce entry that has Windows install SysAudio's software device
(SWENUM) from its own `wdmaudio.inf`, and that install, being Windows'
own, copies the KS files from Windows' installation files (CAB-aware:
silent when the `SourcePath` holds them, else Windows' own "Windows 98
Second Edition CD-ROM" prompt, which the CD's `\WIN98` answers), runs
`KS.Registration` and installs KMixer, the software synth and the KS
clock and service proxies. 9x runs the entry at the end of the device's
install.

**Change.** The 9x section (`[UAC2]`) includes no Microsoft INF. It
writes that RunOnce entry itself (`[SysAudio9x]`, value name
`WDM_SYSAUDIO`, the name Windows' own audio INFs use, so the entry runs
once when both write it), and its device loader is the pair every WDM
audio device has on 9x, `*NTKERN,MMDEVLDR.VXD` (the earlier INF wrote
`*NTKERN`, and `WDMAUDIO.Registration` overwrote it with the pair). The
entry's parts are identifiers: SysAudio's software device id, the KS
filter reference string, SysAudio's interface category, and the install
section that `wdmaudio.inf` (98 SE and ME alike) gives for the device's
interface. The NT sections are unchanged.

On a system where some WDM audio device was installed before (Windows'
own USB audio, for one), everything is already there and the install
asks for nothing but this package's own file.

## 25. A URB the stack fails whole, and Full Speed under 2000's own UHCI (xHCI98 Phase 47, 2026-11-07)

### 25.1 The reports

- XP, 7 and Vista under Windows' own EHCI and UHCI: playback of the
  asynchronous profiles (an explicit feedback endpoint) stopped after a
  few milliseconds to a few seconds and did not start again until the
  stream was closed; under xHCI98 the same profiles played. Recording
  passed everywhere.
- 2000 under its own UHCI, Full Speed (`fs`, `fs4`): `uac2wdm` was the
  device's service, yet the device was never configured (no
  `SET_CONFIGURATION`) and no wave device appeared.

### 25.2 What the trace showed

A trace build (`build.cmd wdmx86 trace`, test-only: every URB's submit
and completion, written to QEMU's debug console port 0xE9 at an anomaly)
on XP under Windows' EHCI and UHCI, on 7 under Windows' EHCI and on 2000
under its UHCI:

- When the host controller falls behind its schedule (here QEMU's EHCI
  and UHCI skipping frames under load; on real hardware a resubmission
  delayed past its frames does the same), every packet of one or more
  queued URBs comes back failed: `USBD_STATUS_ISO_NOT_ACCESSED_BY_HW`
  (EHCI) or `USBD_STATUS_NOT_ACCESSED` (UHCI) per packet, and the URB
  with `USBD_STATUS_ISOCH_REQUEST_FAILED` (IRP `STATUS_UNSUCCESSFUL`).
  `usb.h` defines that URB status as "all packets in an iso transfer
  complete with an error". The URBs submitted after it were scheduled
  and completed normally.
- `uac2` treated any failed URB as fatal: the worker stopped, cancelled
  the queue and exited, and nothing resubmitted until the client stopped
  the stream. A URB with only some packets failed was already counted
  and passed over (4.4, "never retried").
- On 7 under Windows' EHCI the stack at times held every queued URB,
  feedback and data alike, for 0.6 to 1.2 s with none coming back, then
  completed them and went on. The feedback deadline (23.2, 200 ms after
  the last usable packet) ended the stream inside such a hold, though no
  feedback URB had come back unusable: the time counted was the stack's,
  not the device's.
- On 2000 and the controller's own UHCI, the device sits under 2000's
  USB 1.1 stack (its hub service is `usbhub`; the EHCI controller's
  devices are under 2000's USB 2.0 hub driver, `usbhub20`, as the
  device registry of these legs shows). That hub does not answer the
  USBDI bus interface query (`STATUS_NOT_SUPPORTED`; version 1 carries
  `IsHighSpeed`), and `UacUsbOpen` refused an unknown speed on 2000 and
  later, so the start failed before the configuration was selected; the
  same as ME before its rule (16.5, the ME row).

### 25.3 The change

- **A URB failed whole is a lost interval.** A completion whose URB
  status is `USBD_STATUS_ISOCH_REQUEST_FAILED`, whose every packet
  carries a failure of its own, and which was not cancelled
  (`UacPlanIsoLost`) is handled as a URB whose packets all failed: its
  packets are counted as errors, a render stream's position advances
  past its bytes (they are not sent again), a capture stream delivers
  nothing for it, a feedback URB counts its packets as bad feedback, and
  the slot is resubmitted. Any other failed URB status (a bad start
  frame, a device gone, a halted pipe) and every failed IRP without that
  URB status stay fatal, as before.
- **Bounded.** Lost data URBs in a row are counted; a run covering
  `UAC_LOST_LIMIT_MS` (1 s) of data (`UacPlanLostLimit`: 250 URBs of
  4 ms, at least 2) stops the stream as a failure did before. A good
  data URB ends the run. Feedback has its own deadline (next item).
- **The feedback deadline in bus time.** The deadline keeps its lengths
  (23.2: 1 s before the first valid value of a stream, 200 ms after) but
  counts the service time of the feedback URBs that came back (not
  cancelled) with no usable packet since the last usable one: each such
  URB adds its packets times one packet's service time
  (`UacFeedbackPacketUs`: 125 << (`bInterval` - 1) microseconds at High
  Speed, a frame at Full Speed; `UacFeedbackAdd`, `UacFeedbackStale`,
  `uacwatch.h`), and a usable packet resets the count. The data URBs
  that come back after the last usable packet count the same way (their
  span each, reset only by a usable packet; data that came back before
  it but is retired after it, as the worker retires feedback first, is
  not counted: `UacFeedbackAfter` on each completion's stamp. The
  worker retires the completed feedback URBs in the order they came back
  (the oldest stamp first, chosen in one look under the stream's lock,
  then the data in sequence), and a feedback
  URB that came back before the newest one already retired changes
  neither count;
  `UacFeedbackStaleBus`, the
  larger of the two counts), so feedback held, or trickling back
  unusable, while data plays still ends the stream after 200 ms of bus
  time. A time in which
  the stack holds every URB and none comes back, or the guest's clock
  jumps, no longer counts against the device; a device whose feedback
  comes back empty, failed or out of range for 200 ms of bus time still
  ends the stream, as before. A stack that never returns the URBs leaves
  the stream silent until it is stopped, where before it failed after
  200 ms; neither plays.
- **Both clocks (2026-11-08).** Bus time alone ended streams on 98 SE
  under NUSB 3.3: in 3 of 12 plays the first play stopped within 0.3 s
  (a burst of OUT packets, then none), where the build before this
  section stopped in none of 30. The count of service time can run ahead
  of the time that passed where a stack returns URBs faster than their
  span. The deadline is now passed only when both the bus-time count and
  the interrupt time since the last usable packet (or the stream's start)
  are past it (`UacFeedbackStaleBoth`): never earlier than before this
  section, and still not inside a hold in which nothing comes back.
- **The speed under 2000's USB 1.1 stack.** The rule that a missing or
  version 0 USBDI answer means Full Speed (98 SE and ME, below WDM 1.10)
  now also applies on 2000 (WDM 1.10, below 1.20) when the device's PDO
  belongs to the driver object `\Driver\usbhub` (`UacPlanUsb11Hub`),
  2000's USB 1.1 hub driver, which runs only Full and Low Speed devices.
  Under any other hub on 2000 (a USB 2.0 stack that does not answer, for
  one) and on XP and later an unknown speed is still refused: a missing
  interface alone does not prove Full Speed.
- The trace flavour (`build.cmd <profile> trace`) is test-only and never
  released; a free or checked build compiles none of it.

### 25.4 Not changed

- Explicit `StartFrame` scheduling: the URBs after a lost one were placed
  by `USBD_START_ISO_TRANSFER_ASAP` and completed, so 4.4's rule stays
  (except after a refused start frame: 26.13).
- Packet counts (multiples of 8 at High Speed `bInterval` 1) and the
  queue (19.4).
- The deadline's lengths. Under QEMU on a loaded host a controller that
  skips more than 200 ms of frames still returns that much unusable
  feedback and ends the stream; a stall that long is not expected on
  real hardware.
- When a stream stops on a failure, the KS pin stays in RUN with a
  position that no longer moves, until the client stops or closes it;
  with Windows' mixer (XP) that can be much later than the client's own
  close. Not changed here.

### 25.5 Tests

- `test.cmd` (`watchtest.c`): `UacFeedbackStale` at the 200 ms and 1 s
  boundaries and never at 0; `UacFeedbackPacketUs` for each High Speed
  `bInterval` (and out of range) and Full Speed; 25 then 26 unusable
  8-packet URBs of 1 ms packets once active, 125 then 126 at start;
  `UacFeedbackAdd` saturating; `UacFeedbackStaleBus` on either count,
  and 50 then 51 data URBs of 4 ms with feedback held, also with an
  unusable feedback URB back after every 49th; a 128 ms backlog of data
  older than a usable feedback URB not counted, also with the two
  feedback slots retired in reverse of their order; an older usable and
  a newer unusable feedback URB retired oldest first near the boundary;
  the stamps' wrap; `UacFeedbackStaleBoth` only with both clocks past
  the deadline, at each clock's boundary.
- `test.cmd` (`desctest.c`): `UacPlanIsoLost` true only for that URB
  status with every packet failed (not with one packet good, no packets,
  success, a bad start frame, device gone, a cancelled or a halted
  pipe status); `UacPlanLostLimit` for 1, 3, 4 and 8 ms spans, a span of
  a second or more, and 0; `UacPlanUsb11Hub` for the name in either case,
  and not for `usbhub20`, a shorter or other name, or none.
- Legs: [uac2-iso-ehci](../testing/2026-11-07-uac2-iso-ehci.md).

### 25.6 Empty feedback before the first value (2026-11-08)

On 98 SE under NUSB 3.3 with QEMU's EHCI, some plays stopped at the
start deadline: the stream's stats (Uac2UsbStats, read from the
registry after the play) showed every feedback packet of the stream
successful and zero-length (6016 of them in its first second, no value),
`LastStatus` `STATUS_IO_TIMEOUT`, while the device sent a value in every
frame. NUSB polled the 1 ms feedback endpoint at every microframe, and
the emulator's passthrough answered most polls empty; in the plays that
went on, values came about one poll in 600. The start rule of 23.2
counted those empty answers as bad; it does so in every build since 23,
the deadline's form (25.3) only changed how soon.

The change: before the first valid value of a stream, a successful
zero-length feedback packet is `UAC_FEEDBACK_EMPTY` (`uacrate.c`): not
counted as bad, and usable for the deadline, as a keepalive is after a
value. The stream goes on at the nominal cadence until a value comes,
as it does for the first second anyway. A device that answers its
feedback endpoint only with empty packets now plays at the nominal rate
instead of stopping after 1 s; one whose packets fail, or carry values
outside the window, still stops at the deadline. `ratetest.c`: empty
answers for 5 s keep the start deadline away and leave BadFeedback at 0,
a value then makes the stream active and later empties keepalives; the
start deadline still fires for rejected packets; `uac1test.c` likewise.

### 25.7 Feedback refused for its start frame: the stream goes on without it (2026-11-08)

On 98 SE under NUSB's UHCI a stream sometimes ended when a feedback URB
came back with `USBD_STATUS_BAD_START_FRAME`, after a guest stall (the
stream's stats: failed feedback URBs, all of that status,
`STATUS_DEVICE_DATA_ERROR`). Every build since the first ended the stream
on it. A first try (resubmitting the feedback URB, up to 8 in a row) did
not recover: once NUSB refused a feedback URB's start frame it refused the
next ones too, 9 in a row in one play, which fits the USBD header's note
that the status halts the pipe.

Now, for a feedback URB, and only a feedback URB, that comes back with
that status (`UacPlanFbRefused`): the first 7 in a row are passed over and
their slot resubmitted (ASAP; since 26.13 at the resync's explicit frames),
and a good feedback URB ends the run; at the
8th the stream stops polling feedback for the rest of the stream instead
of ending: no feedback slot is resubmitted, the deadline (25.3) no longer
applies, and the data cadence keeps its last good feedback value, or the
nominal one if none came, as it does before the first value (25.6). The
stats' word 94, `FeedbackOff`, reads 1 then. Data URB failures, other
feedback URB statuses and out-of-window values still end the stream as
before.

Feedback is not polled again in the same stream. A pipe that refuses
start frames in a row is halted at the stack (the status's own definition);
polling it again without a pipe reset fails the same way, and a reset
needs every feedback URB on the pipe aborted first, with the data stream
running on the same interface: more risk than a stream at its last good
rate for the rest of a play. The next start of the stream selects the
alternate again, which opens the pipes afresh, and polls feedback from
the start. A device whose clock drifts from the last good value then
plays with a slow buffer drift until that next start, as an adaptive
device does.

`desctest.c`: resubmitted for that status at 0 to 6 refusals before,
feedback off at 7 and above (the 8th), the count wrap-safe; any other
status not handled here.

### 25.8 A data URB refused for its start frame (2026-11-08)

With 25.7 in place, 98 SE under NUSB's UHCI, on a heavily loaded host,
still ended some streams when a data URB came back with
`USBD_STATUS_BAD_START_FRAME` (the stats: one failed data URB of that
status, `STATUS_DEVICE_DATA_ERROR`; in some feedback had already turned
off). Now a data URB, as a whole, that comes back with that status
(`UacPlanDataRefused`) is a lost interval, exactly as 25.3's whole-URB
failure: its packets counted as errors, the render position advanced past
its bytes, a capture stream delivering nothing for it, and the run of lost
data URBs in a row still bounded by `UAC_LOST_LIMIT_MS` (1 s), after which
the stream ends as before. Other data URB statuses still end the stream.

The slot is resubmitted (ASAP; since 26.13 at the resync's explicit
frames) with fresh data, the refused bytes not sent
again: the time they were for has passed (4.4: never retry time-specific
PCM), and the stream's position must keep pace with the clock the client
reads. A pipe the stack keeps halted refuses the next URBs too and reaches
the run limit, ending the stream as it did at the first refusal, a second
later at most. `desctest.c`: lost for that status with packets, not with
none, not for any other status.

## 26. USB Audio Class 1.0 devices (xHCI98 Phase 47, 2026-11-07)

Status: design, 2026-11-07; the options of 26.5 and 26.6 are the
owner's to choose; no code yet. Owner, 2026-11-07: "Please support UAC1
in the UAC2 driver", and "Or have a separate UAC1 driver, depending on
which is easier"; for release 0.1.9. This revisits Q5 (section 13).

### 26.1 Why

- **98 SE, unplug while playing.** With Windows' own `usbaudio.sys`
  playing a USB Audio 1.0 device, an unplug froze 98 SE or ended in a
  fatal exception 0E in every valid trial but one, on the real Logi USB
  Headset (`046D:0A64`) under xHCI98, and no change in `xhci98.sys` cured
  it (xHCI98 `docs/issues/25-9x-usbaudio-unplug-while-playing.md`
  sections 5 to 7). xHCI98 `3.4.2.0` ships it as a known limitation:
  "On Windows 98 SE, stop audio playback before unplugging a USB audio
  device."
- **`uac2` survived the same act on 98 SE**: 1 of 1 on xHCI98 `3.4.1.0`,
  5 of 5 while playing and 3 of 3 while recording on `3.4.2.0`'s option
  B, all with a UAC 2.0 device (the Sound Blaster X4, High Speed; same
  issue, section 4). Whether that holds for a Full Speed UAC 1.0 device
  is this design's first leg (26.7).
- **ME** stopped at about one unplug in four with `usbaudio.sys`, on
  xHCI98 and on ME's own UHCI stack; xHCI98 `3.4.2.0`'s option B fixes it
  under xHCI98 (0 in 54), not on ME's own stack (issue 25, sections 3
  and 4).
- **98 SE's `usbaudio.sys` faults once it streams to QEMU's
  `usb-audio`** (issue 25 section 2; xHCI98 `lessons.md`), so no UAC 1.0
  playback on 98 SE can be read in QEMU today; `uac2` plays the
  simulated devices there.
- **Not a reason**: 98 SE's first-audio-arrival wedge (xHCI98
  roadmap-hcd 36.41) comes with `uac2` as with `usbaudio.sys`
  (`docs/testing/2026-10-23-brio-null.md`); this work neither cures nor
  causes it, and the documents must not say otherwise.

### 26.2 What was built before

A separate UAC 1.0 driver, `uac1wdm.sys`, was designed and built on
2026-10-23/24 (owner decision of 2026-10-23: a separate driver from a
library shared with `uac2wdm.sys`; its design `docs/design/uac1.md` and
record `docs/testing/2026-10-23-uac1-98se.md`, both on that branch only).
It was left out of 0.1.0 (2026-10-24): it played and recorded on 98 SE
under xHCI98, but playback failed on XP and 7 x86 and under NUSB's EHCI
in QEMU, and its copies of `uac2`'s transport in `src/audio/common/`
stood apart from the `uac2` that shipped. Its branch was never merged;
its last commit, `f7c024f` (2026-10-24), is kept on the local branch
`uac1-prior`.

What it holds that is still of use, all original or adapted under
GPL-2.0-or-later: the UAC 1.0 parser (`uac1desc.c`), the Feature Unit
requests (`uac1ctl.c`), feedback format detection (`uac1fb.c`, adapted
from Linux `sound/usb/endpoint.c`), the simulator `uac1emu.py`
(profiles `headset`, `speaker`, `mic`, `midi`) with its host tests, and
a 1155-check host test of the three C files. The original files
(`uac1desc.c`, `uac1ctl.c`, `uac1emu.py`, the tests) carry GPL-2.0-only
SPDX lines (they predate the relicensing of 2026-10-25); taken over, they
get this repository's line, the copyright holder being the same.
`uac1fb.c` is already GPL-2.0-or-later and names its Linux source and
commit; it keeps that notice and gets its `NOTICE.md` row here. What is not of use: its transport,
stream and adapter copies, which lack everything `uac2` gained since
(builds 14 to 26 and 0.1.10.0: the queue and the real-time worker,
section 19; the fetch-cursor position, 17; several pairs per device, 15;
the device's name, 21; the preferred device, 22; Full Speed 4-byte
feedback, 23).

### 26.3 UAC 1.0 against UAC 2.0, and what the driver does

Specifications: USB-IF "Device Class Definition for Audio Devices" 1.0
(ADC-1), "... for Audio Data Formats" 1.0 (FMT-1) and "... for Terminal
Types" 1.0, section numbers theirs (copies kept outside the repository,
`E:\usb-wdm-free\uac1-refs`, as for `uac1.md`).

| Topic | UAC 2.0 (`uac2` today) | UAC 1.0 | What `uac2` does with UAC 1.0 |
|---|---|---|---|
| Interface protocol | `0x20` | `0x00` | The family is the AudioControl interface's protocol and its header's `bcdADC` (`0x0100`), read once at parse; a function whose interfaces disagree is refused |
| AC header | `bcdADC` 0x0200, `bCategory`, `bmControls` | `bcdADC` 0x0100, `wTotalLength`, `bInCollection`, `baInterfaceNr(n)` (ADC-1 4.3.2) | The function is the AC interface plus the interfaces its header lists; an IAD over it must agree |
| Input Terminal | 17 bytes, `bCSourceID`, `bNrChannels` at 8, `bmChannelConfig` 4 bytes | 12 bytes, `bNrChannels` at 7, `wChannelConfig` 2 bytes (Table 4-3) | Per-family field offsets; an OUT stream's channel mask from its streaming Input Terminal, an IN stream's from the cluster its Output Terminal's source chain carries; a mask of 0 above 2 channels replaced by the usual mask for the count (KMixer mixed silence with mask 0, 5.2) |
| Output Terminal | 12 bytes, `bSourceID` at 7, `bCSourceID` at 8 | 9 bytes, `bSourceID` at 7 (Table 4-4) | Same source offset; no clock |
| Units | Mixer 4, Selector 5, Feature 6, Effect 7, Processing 8, Extension 9, clock entities 10 to 12, SRC 13 | Mixer 4, Selector 5, Feature 6, Processing 7, Extension 8; no clocks | The USB entity walk lives in `uacvol.c` (`Sources`, `Walk`, and the terminal length checks of `UacVolFind` and `UacVolJack`, which ask UAC 2.0 minimums today: a 9-byte Output Terminal is refused); these take the family. Units are traversed only: the KS topology is `uactopo.c`'s fixed graph (volume, mute, jack), not the device's entities, for both families |
| Feature Unit | 4-byte `bmaControls` per channel, 2 bits per control (readable, programmable) | `bControlSize` bytes per channel, 1 bit per control (D0 Mute, D1 Volume, D2 Bass ... D6 AGC ... D9 Loudness), no read-only form (Table 4-7) | Mute and Volume as today; a set bit counts as read/write; other controls not exposed (as today) |
| Rates | The clock's `RANGE`, one set per terminal; set by `SET CUR` on the Clock Source (4.2) | Per alternate, in the Type I format descriptor: `bSamFreqType` 0 = continuous (`tLowerSamFreq`, `tUpperSamFreq`), 1..n discrete, 3 bytes each (FMT-1 2.2.5) | The parser keeps each alternate's rate list; the data ranges are built per (format, rate) as now, a continuous range offering the usual rates (8 to 192 kHz) inside it; a rate is offered only where an alternate of that format carries it |
| Setting the rate | Clock Source `SET CUR` before the alternate, read back, validity | The endpoint's Sampling Frequency control: `SET_CUR` 0x01, `bmRequestType` 0x22, `wValue` 0x0100, `wIndex` the endpoint address, 3 bytes (ADC-1 5.2.3.2.3.1); only when the class-specific endpoint descriptor sets D0 | After `SELECT_INTERFACE` (the endpoint exists only then), before the first isochronous URB; `GET_CUR` (0x81, 0xA2) read back, a STALL there accepted; a `SET_CUR` failure fails the start; no clock claim between directions (the reservation of 15.3 is UAC 2.0's) |
| Volume and mute | `CUR` and `RANGE` (requests 1 and 2), 2-byte volume in 1/256 dB, `RANGE` subranges | `SET_CUR` 0x01, `GET_CUR` 0x81, `GET_MIN` 0x82, `GET_MAX` 0x83, `GET_RES` 0x84, `bmRequestType` 0x21/0xA1, `wValue` selector << 8 \| channel, `wIndex` unit << 8 \| interface; volume signed 16-bit 1/256 dB, 0x8000 silence (ADC-1 5.2.2.4.3.2) | MIN, MAX and RES read once and held as one subrange of `UAC_VOL_RANGE` under `UacVolRange`'s rules, as for UAC 2.0 (a RES of 0 on a span reads as 1; MIN above MAX or a malformed answer leaves the software range); everything after that (the 0 dB top of section 20, software gain where there is no control, the KS conversion) unchanged |
| Mixer and Selector controls | `uacmix.c` (UAC 2.0 layouts) | Other layouts | Not exposed for UAC 1.0 in this work: these entities are traversed to resolve the signal paths, with no KS nodes made for them and no controls |
| Standard isochronous endpoint | 7 bytes; a feedback endpoint by usage type 01b | 9 bytes with `bRefresh` and `bSynchAddress` (Table 4-20); the synch endpoint is the one the data endpoint's `bSynchAddress` names, often with usage bits 0 (USB 1.1 had none) | The feedback endpoint taken from `bSynchAddress` (an isochronous IN endpoint of the same alternate); usage bits not required |
| Class-specific endpoint | `bmControls` pairs | `bmAttributes` D0 Sampling Frequency, D1 Pitch, D7 MaxPacketsOnly; `bLockDelayUnits`, `wLockDelay` (Table 4-21) | D0 decides the rate request; D7 on OUT: each non-null packet padded with zeros to `wMaxPacketSize` (ADC-1 4.6.1.2; a null packet stays null), the PCM bytes of each packet kept apart from its USB length, so the ring is read, the position advanced and completions counted by PCM bytes only (today `SubmitSlot` hands `Fill` the URB's whole length and the completion reports `slot->Bytes`, `uacusb.c`; `uac1-prior`'s `uacxfer.c` kept the two apart); an IN alternate with D7 skipped (padding cannot be told from audio); the lock delay not waited (the stream starts with what PortCls has written, as now) |
| Speeds | Full and High Speed | Full Speed nearly always; High Speed UAC 1.0 exists (the Brio 100's microphone, `bInterval` 4) | Unchanged: `uacrate.c`'s cadence, `bInterval` 1 at Full Speed, 1 to 4 at High Speed, the packets-per-URB rules of 4.4 and 4.6 |
| Synchronisation, OUT | Async with explicit feedback, adaptive, synchronous | The same three; async feedback 3 bytes 10.14 at Full Speed, renewed every 2^`bRefresh` ms | As now (4.5 and 23): feedback read every frame, 3 or 4 bytes at Full Speed, 4 bytes 16.16 at High Speed. Linux's detection of a shifted High Speed value (`uac1fb.c`) waits for a device that needs it |
| Synchronisation, IN | Async, synchronous; adaptive needs a synch OUT | The same; an adaptive source needs its synch OUT endpoint fed (ADC-1 3.7.2.2) | Async and synchronous sources as now. An adaptive source whose endpoint names a synch OUT endpoint (`bSynchAddress`) is skipped: `uac2` sends no Ff (today it reads any IN endpoint the same way, whatever its sync type, `uacplan.c` `Runnable`) |
| Feedback loss | The stream ends 1 s without a first valid value, 200 ms after one (`uacwatch.h`, 23.2) | Many UAC 1.0 devices answer feedback only while locked | Owner question U4 (26.9) |
| Capture | As now | Mono microphones at 8 to 48 kHz are common | As now: native formats; 98 SE's KMixer converted capture in the 5.2 readings |
| 8-bit, float | Not offered (`Subslot` below 2 skipped, 5.2) | PCM8 (`wFormatTag` 2), IEEE_FLOAT (3), ALAW, MULAW | Only `wFormatTag` 1 (PCM), 2 to 4-byte subframes; the rest skipped |
| HID controls on the device | A separate function | The same (the Logi's interface 3, consumer keys) | Untouched: the parent gives HID its own function, `hidclass` binds it |
| UAC 1.0 and 2.0 on one device | - | Separate audio functions in one configuration (ADC-1 allows several Audio Interface Collections at once), two configurations, or a firmware mode that re-enumerates; within one function every interface has one protocol | The family is decided per function (the PDO's), so a UAC 1.0 and a UAC 2.0 function of one configuration each get their own devnode and family; a function that mixes protocols is refused. A device with two configurations is not split by xHCI98 or Windows' stack (16.6), so its function never reaches `uac2` by class. A mixed-function fixture is in the tests |
| MIDI in the function | Not served (section 11, M7 not built) | AC + AS + MS, or AC + MS only (a MIDI-only device has the same AC id `01/01/00` as a speaker) | MIDIStreaming ignored, as for UAC 2.0. A function with no usable audio alternate (MIDI-only) fails its start, the reason recorded; `usbmidi` serves listed devices by hardware id, which outranks a class id |

The Logi headset's descriptors (`testdata/logi0a64/CONFIG.BIN`, decoded
for this design): AC header `bcdADC` 0x0100, interfaces 1 and 2
collected; capture: Input Terminal 1 (microphone) to Feature Unit 2
(mute, volume, AGC) to Output Terminal 3 (USB streaming); playback:
Input Terminal 4 (USB streaming) and a sidetone path (Input Terminal 10,
Feature Unit 11) into Mixer Unit 12, then Feature Unit 5 (mute, volume)
to Output Terminal 6 (speaker). Interface 1 alternate 1: mono 16-bit at
8, 16, 32, 44.1 and 48 kHz, endpoint `0x81` async IN, 144 bytes;
interface 2 alternate 1: stereo 16-bit at seven discrete rates from 8 to
192 kHz, endpoint `0x01` adaptive OUT, 768 bytes, Sampling Frequency
control, lock delay 4 ms. Interface 3: HID.

### 26.4 `uac2`'s pipeline against UAC 1.0

| Part | UAC 1.0 | New or parameterised |
|---|---|---|
| Function and parse (`uacdesc.c`: `UacFunction`, `UacWholeFirst`, `UacParse`) | A second parser fills the same `UAC_DESC` (`UAC_ALT` plus each alternate's rate list, sync type, `bSynchAddress`, CS endpoint attributes); `UAC_DESC` gains `Family` | New: `uac1desc.c`, ported from `uac1-prior` onto `UAC_DESC`; the UAC 2.0 parser unchanged |
| Rates (`UacUsbRates`, the clock walk) | The alternate's list, no requests | Parameterised: a family branch before the walk |
| Rate set (`SetClock`, before `SelectAlt`) | Endpoint `SET_CUR` after `SelectAlt` | New function; the order by family |
| Alternate choice (`uacplan.c`) | The smallest `wMaxPacketSize` alternate that carries the format and lists the rate | Parameterised: the rate check per alternate |
| Stream, ring, worker, `Uac2QueueMs`, `Uac2Realtime`, the position (17, 19) | Unchanged | MaxPacketsOnly OUT only: per-packet PCM lengths beside the padded USB lengths, so fill, position and completion count PCM bytes (26.3) |
| Feedback (`uacrate.c`, `uacwatch.h`) | Unchanged at Full Speed (3 or 4 bytes) | None, unless U4 changes the loss rule |
| Volume and mute (`uacvol.c`, the volume object in `uac2wdm.cpp`) | Feature Unit layout and MIN/MAX/RES | Parameterised: `Access`, `UnitChannels`, `Sources` and the range read by family |
| Topology to KS (`uactopo.c`'s fixed graphs; `Learn`; jacks from `UacVolJack`) | The fixed graphs unchanged; the terminal checks of `uacvol.c` by family; the terminal type codes used here (0x0101, 0x0201, 0x0301, 0x0302, 0x0402, 0x0601, 0x0603, 0x0605) are the same in both versions | Parameterised (`uacvol.c`) |
| Several pairs per device (15) | `Wave::Pairs()` gives one pair unless every endpoint resolves to a clock source of its own (`UacClock`); UAC 1.0 has none, so a UAC 1.0 function is one pair: its first playback and first capture terminal, as `Learn` picks them | None; several UAC 1.0 pairs are not in this work (a rule of their own, with the shared-unit check of 15.3 item 6, when a device needs it) |
| Names (21), `SetupPreferredAudioDevices` (22) | The same INF values and product string | None (INF lines only) |
| PortCls side (WaveCyclic per direction, Topology; KMixer, SysAudio and WDMAud on 98 SE to XP, the audio engine on Vista and later) | Unchanged: `uac2` is a PortCls WaveCyclic driver on every system, a KS audio device to the stack above | None |
| Co-installer `UACCI1.DLL` (16) | Its rule is `Prot_20` only; UAC 1.0 functions are left to Windows | None, unless option c of 26.6 |
| Host tests | Parser on the Logi and Brio 100 fixtures, QEMU's layout, the simulator's profiles and malformed variants; endpoint and Feature Unit setup packets; volume ranges | New tests beside `desctest.c` and `voltest.c` |

Size, by the `uac1-prior` files and `uac2`'s: about 500 lines of parser
(mostly ported), 150 in the USB layer, 200 for controls and topology,
INF lines; about 400 lines of simulator profiles.

### 26.5 Option 1: inside `uac2`, or a separate driver

| | A: inside `uac2` (one binary, one package) | B: a separate package (`uac1wdm.sys` and its INF), built from `uac2`'s sources with the family fixed at build time |
|---|---|---|
| Code | The work of 26.4, once | The same work, plus a second build target, resources, INF, folders and import gate; the old `uac1-prior` driver is not a base (26.2) |
| Shared or duplicated | Nothing duplicated | Nothing duplicated if built from the same files (one source, two binaries, as `AGENTS.md` allows); duplicated if it were the old copies |
| Risk to UAC 2.0 users | Their binary carries the new branches; the family is chosen once at parse and the UAC 2.0 paths keep their code; host tests and regression legs | The same source changes compiled into `uac2wdm.sys` too, unless guarded out at build time; `uac2`'s INF untouched |
| INF and rank | The UAC 1.0 lines go into `uac2`'s 98 SE INF, and a by-hand model on NT and ME (26.6) | Its own INF and `STAGE.TXT` rows; the rank facts of 26.6 are the same |
| Signing | No new image: `uac2`'s x64 image changes in 0.1.9 already (0.1.10.0, section 23) and is signed in the same pass | One more x64 image in the owner's signing pass |
| Packaging here | `free/audio/uac2/` as now (with the 9x folder split of 26.6 if chosen) | New `free/audio/uac1/` folders, `README.md` rows, release notes, `STAGE.TXT` |
| xHCI98's installer | No new row; the `uac2` row's words gain UAC 1.0, the pin moves | A row: the withdrawn `free:audio/uac1` revived, which carries `EXCLUDES free:audio/usbmidi` on every system ("uac1 serves MIDI-only functions by class") and needs that line lifted by the cut's rules (xHCI98 design record 26, 6.7.11 and 6.7.14); or a new id with its own lines |
| For the user | One Audio box, one set of registry values | Two Audio boxes; its own registry value names (`Uac2QueueMs` would need a twin) |
| Maintenance | One driver | Two packages to version, release and test, from one source |

**Recommendation: A.** It is the smaller job (no package, no second
image, no installer row), every fix `uac2` has gained applies to UAC 1.0
devices at once, and the risk to UAC 2.0 users is bounded by deciding
the family once at parse. B's isolation is mostly nominal: built from
the same sources it carries the same changes, and `uac2`'s binary is
rebuilt in 0.1.9 either way. The 2026-10-23 choice of a separate driver
went with splitting the transport into a library first; with `uac2` now
the only home of that transport, A is that shared library without a
second package.

### 26.6 Option 2: which systems bind UAC 1.0 to `uac2`

**Facts the options rest on.**

- Compatible ids of a UAC 1.0 function: `USB\Class_01&SubClass_01&Prot_00`
  (an AC interface heading a legacy-grouped function: xHCI98, `usbccgp`,
  98 SE's composite parent), `USB\Class_01&SubClass_00&Prot_00` or
  `&SubClass_02&Prot_00` (an IAD function; the Brio 100's IAD names
  subclass 02), then the two-part and the bare `USB\Class_01`. xHCI98
  keeps the bare id for protocol 00 (it drops it only for 20 and 30, its
  design record 13 section 10.7.2).
- Windows' `wdma_usb.inf` matches the bare `USB\CLASS_01` on every
  target, and also ten devices by `&MI_00` hardware id on 98 SE and about
  twenty on XP (`tools\win98se-extracted\inf\wdma_usb.inf`,
  `tools\winxpsp3-extracted\inf\wdma_usb.inf`, read on the xHCI98 host); a
  hardware-id line outranks a class line of ours, so those devices keep
  `usbaudio`.
- **98 SE and ME**: a more specific compatible id in an INF Windows'
  setup has read outranks `WDMA_USB.INF`'s bare one on a new install
  (read for a UAC 2.0 function, 16.5, the 98 SE and ME rows). xHCI98's
  installer puts `uac2`'s 9x folder on `DevicePath`, so the INF is read
  without a choice in the wizard. A device already installed on
  `usbaudio` keeps it until Update Driver (16.5: Update Driver from
  `usbaudio` to `uac2` completes on both).
- **2000** reads the undecorated `[Models]` (the 98 SE section) and has
  no signature penalty: `uac2`'s class line outranked `usbaudio` on a
  plain plug (16.5, the 2000 row).
- **XP to 8.1, x86 and x64**: Windows' signed `wdma_usb.inf` outranks an
  unsigned INF whatever its id (16.1; the XP, Vista and 7 rows of 16.5);
  with `uac2` a candidate, XP showed the Found New Hardware wizard where
  it had installed `usbaudio` silently (16.3). Only the co-installer
  changes the choice, and its rule is `Prot_20` only.
- **ME's `usbaudio.sys` serves USB MIDI; 98 SE's does not** (Microsoft
  Learn, "USB Audio Class System Driver (Usbaudio.sys)", as read for
  `uac1.md`). `uac2` serves no MIDI (section 11).
- A Full Speed UAC 2.0 device's first start on 98 SE's own USB 1.1 stack
  can hang 98 SE (the known issue of 16.5, owner 2026-10-30); a UAC 1.0
  device there takes the same path. XP's own UHCI does not play a Full
  Speed device through `uac2` (23, unsettled).

**The options.**

| | Which systems | 98 SE | ME | 2000 | XP to 8.1 |
|---|---|---|---|---|---|
| a | By class on 98 SE (and ME, U2), automatic; NT by hand | `uac2` on a new install; `usbaudio` stays where installed until Update Driver | as 98 SE, or by hand (U2) | `usbaudio` unless the user picks `uac2` | `usbaudio`; `uac2` by Update Driver, Have Disk |
| b | Every system, below `usbaudio` | not possible by class: our id is the more specific, so it ranks above | as 98 SE | as 98 SE | holds by signature: a's NT half |
| c | Every system, above `usbaudio` | as a | as a | as a, automatic | needs `UACCI1.DLL`'s rule widened to UAC 1.0, and not as `Prot_00` alone: its predicate takes any subclass digit (`uacco.c`), which would catch a lone MIDIStreaming interface (`01/03/00`) that `uac2` does not claim and refuse it `usbaudio`; it would have to name subclasses 00, 01 and 02 only and act only where a `uac2` node is in the list, and a rollback would first remove the rule. Windows' working UAC 1.0 driver replaced for every user who sets the co-installer up |
| d | Listed devices only (the Logi, `USB\VID_046D&PID_0A64&MI_00`, on request) | that device on a new install, above `usbaudio`'s class line | as 98 SE | as 98 SE | below `usbaudio` by signature; by hand |

**How a is built.**

- 98 SE: the UAC 1.0 class lines in `[Models]` of the 9x folder's
  `uac2wdm.inf`. 2000 reads `[Models]` too, and the 9x and 2000/XP
  folders ship the same INF today; so the 9x folder's INF gains the
  lines and the `w2k-xp` folder's does not. One 9x INF cannot tell 98 SE
  from ME; ME by hand (U2) means splitting `w98se-me/` into `w98se/` and
  `wme/` (names of `README.md`'s "Layout", as `usbmidi` ships), the ME
  INF without the lines.
- NT, and ME by hand: a model in `uac2wdm.inf` with an id no device
  reports, `UAC2_UAC1_BY_HAND`, as `usbmidi` ships `USBMIDI_BY_HAND`
  (`usbmidi.md` 4.3): the user picks it by Update Driver, Have Disk,
  "show all" (Windows warns that the model may not fit the device). It
  installs the same sections and service. A class id in a by-hand INF
  would not stay by hand: once one device installed from it, Windows
  keeps the INF, and on 2000 and ME (16.5's ME row: a later unseen
  device took the installed driver silently) every later UAC 1.0 device
  would get `uac2`. The non-matching id never matches a device by
  itself, so a second unseen device still gets `usbaudio`; a leg reads
  that after a by-hand install and after the rollback.
- On 98 SE, with a MIDI device beside a UAC 1.0 audio device:
  `uac2`'s INF registers the `midi` subclass (`Drivers\midi`, the
  software synthesiser through WDMAud), and the `uac1-prior` legs read
  MIDI opens failing with `MMSYSERR_ALLOCATED` after the removal of an
  audio devnode so registered (`uac1-prior`'s
  `docs/testing/2026-10-23-uac1-98se.md`; fixed there by dropping the
  registration at start). UAC 2.0 devices carry this already; class
  binding of UAC 1.0 devices makes it common. Read in leg 4a before
  deciding whether `uac2` drops it on 9x.
- `STAGE.TXT`: the 9x row's text says UAC 1.0; a `COEXISTS` line for
  `uac2` and `usbmidi` on 98 SE (`usbmidi`'s hardware ids outrank
  `uac2`'s class line; its `SubClass_03` id is a lone MIDIStreaming
  interface, which `uac2` does not claim).
- xHCI98: no change to `xhci98.sys`; the installer's `uac2` row words and
  pin; its README's 98 SE known limitation narrowed to devices on
  `usbaudio` (listed ones, or installed before), with the Update Driver
  step.
- Rollback: Update Driver back to Windows' "USB Audio Device"
  (`WDMA_USB.INF`) on any system; on 98 SE, xHCI98's uninstall takes the
  folder off `DevicePath`, and a device installed from it keeps `uac2`
  until Update Driver, as 16.5 reads for `uac2` today.

**Recommendation: a, behind release gates**: 98 SE automatic, NT by
hand, and ME by hand (U2). 98 SE is where the limitation remains and
`usbaudio` cannot be helped below it; ME's unplug is fixed under xHCI98
`3.4.2.0`, and ME's `usbaudio` carries USB MIDI that `uac2` lacks; on
2000 to 8.1 Windows' driver works for UAC 1.0 and nothing reported asks
for a change. This is the rule chosen for `uac1` on 2026-10-23 (its Q1:
98 SE and ME automatic, NT by hand), but for ME.

**What a costs, stated for the owner's choice.** Automatic class
binding takes devices from a driver that serves them (on real 98 SE
machines `usbaudio` plays; it lacks only the unplug). A UAC 1.0
function whose only formats `uac2` does not run (8-bit, float, A-law or
mu-law only; an adaptive source with a synch endpoint; MaxPacketsOnly
capture only) would, on 98 SE, lose `usbaudio` to a failed start (Code
10, no fallback, as `usbmidi.md` records for a refused start) until the
user goes back by Update Driver. No test sample can rule such devices
out; they are taken to be rare (common UAC 1.0 devices are 16-bit PCM),
not shown to be absent. The conservative alternative, **d plus the
by-hand model** (the devices read in the legs by hardware id, every
other UAC 1.0 device by hand), loses nothing but leaves every unlisted
device with the unplug limitation. The recommendation is a, as an
explicit choice with this loss in the release notes; if the owner does
not accept the loss, d plus the by-hand model. Either way the 98 SE
class lines ship only when these gates pass, else the release falls
back to d plus the by-hand model:

- **G1, the unplug**: leg 2 passes on the real Logi (and the Play! 2
  where it is at hand): no stop in 5 valid trials each.
- **G2, 98 SE's own USB 1.1 stack**: leg 3 counts the first-start hang
  of 16.5 for UAC 1.0 devices in 10 cold-start first plugs with no
  driver file copied in the session (16.5 read 0 in 11 such plugs for
  UAC 2.0). The class lines ship only with no hang there, and the known
  issue's words then name UAC 1.0 devices too; a hang withholds them
  (the owner may still choose to ship with the known issue).
- **G3, the failure path for what `uac2` cannot play**: negative
  fixtures (a PCM8-only, a float-only and an adaptive-source-with-synch
  UAC 1.0 function) show on 98 SE a recorded start failure with its
  reason, no hang, and Update Driver back to "USB Audio Device"
  completing (the device bound to `usbaudio`, started). In QEMU that is
  the rebinding only: 98 SE's `usbaudio` cannot stream there (26.1), so
  the functional recovery (playback for a render fixture, recording for
  a capture one, the adaptive source's included) is read on XP's
  `usbaudio` with the same fixtures for the fixture's own soundness,
  and on 98 SE only on a real machine (a bench leg, owed and labelled
  apart).
  A function with one runnable direction starts with that one.

Option d alone would leave every other UAC 1.0 device on 98 SE with the
limitation.

### 26.7 Tests

**Models first, each checked against a driver written without it** (the
owner's rule of 2026-11-06, `docs/testing/2026-11-06-inbox-reference.md`):

- `uacemu.py` gains UAC 1.0 profiles (the `uac1-prior` simulator's moved
  into it, or kept beside it): `logi` (the Logi's captured descriptors
  replayed: adaptive OUT with Mixer and sidetone, async mono capture, its
  HID interface answered as the captured layouts' are); `play2`, the
  owner's Creative Sound Blaster Play! 2 (UAC 1.0), from the real
  device's descriptors where they can be read, else from public listings
  (built alongside, owner 2026-11-07); `headset1` (async OUT with a
  3-byte 10.14 synch endpoint named by `bSynchAddress`, usage bits 0);
  `speaker1` (adaptive, a continuous rate range, MaxPacketsOnly as an
  option); `mic1` (the Brio 100's layout, IAD subclass 02, High Speed
  `bInterval` 4). Each answers endpoint `SET_CUR`/`GET_CUR` and Feature
  Unit `CUR`/`MIN`/`MAX`/`RES`, and stalls anything else.
- Each checked against Windows' `usbaudio.sys` before `uac2` runs on it:
  XP SP3 x86 (EHCI and UHCI), 7 SP1 x86 and 10 22H2 x64, on the inbox
  record's rig: binding, a play and a record, and the requests Windows
  sent (`usb-redir`'s `pcap=`); the model fixed where Windows asks what it
  does not answer.
- QEMU's own `usb-audio` (UAC 1.0, Full Speed, `hw/usb/dev-audio.c`) as
  a model of another's making, and the control on XP.

**Legs** (development legs in QEMU on the test-matrix kits' bases, `mx9x`
and `mxnt`, fresh overlays; QEMU ports 51310 to 51329; 4 vCPU at most):

1. **98 SE under xHCI98 `3.4.2.0`**: the simulator's UAC 1.0 profiles
   (`play2` and `logi` first) and QEMU's `usb-audio`: plug, `uac2` taken
   with no choice in the wizard (`DevicePath`), play, record, volume and
   mute, a rate change, unplug idle, unplug while playing (5), while
   recording (3), replug and play.
2. **98 SE, real devices passed through**, each only when
   `E:\xhci98-vm\devices.txt` shows it free, claimed there for the leg:
   the Logi and, where available, the Play! 2: issue 25's trial, an
   unplug about 12 s into a play with the play seen running, 5 valid
   trials each; then a device installed earlier on `usbaudio` moved by
   Update Driver, and the trial again. The Play! 2 also on XP (by hand):
   play, record, unplug while playing.
3. **98 SE under NUSB's stack** and **98 SE's own UHCI**: plug, play,
   record, unplug while playing; the first-start hang of 16.5 counted,
   not hunted. In QEMU a Full Speed device on NUSB's EHCI port goes to
   its UHCI companion, so these are UHCI legs and are labelled so (as
   in 23's record); Full Speed through a High Speed hub's transaction
   translator on real EHCI is a bench leg, owed and labelled apart.
4. **ME** under xHCI98 and its own UHCI, by hand (or by class, per U2):
   play, record, unplug while playing; after one by-hand install, a
   second unseen UAC 1.0 device still gets `usbaudio`, and so does a
   UAC 1.0 device with MIDI.
4a. **MIDI and HID beside it** (98 SE, xHCI98): `midiemu.py`'s loopback
   through `usbmidi` (a listed device) before and after a UAC 1.0 audio
   device on `uac2` is plugged, played and unplugged; a MIDI-only AC+MS
   function by class gets `uac2`'s recorded start failure; the Logi's
   HID consumer keys (or the simulator's HID interface) work while audio
   plays and after a replug.
4c. **Unsupported layouts** (G3): each negative fixture first under
   Windows' `usbaudio` on XP (play for render, record for capture); then
   on 98 SE under xHCI98: the start fails with its reason recorded, the
   system goes on, and Update Driver back to `usbaudio` completes (bound
   and started; no stream read there, 26.1).
4b. **Mixed functions**: a fixture with a UAC 1.0 and a UAC 2.0 function
   in one configuration (host test; a simulator profile if cheap): each
   function parsed in its own family.
5. **2000, XP, XP x64, Vista, 7, 8, 8.1** (x86, and x64 where `uac2`
   ships): a plain plug keeps `usbaudio` with no wizard; Update Driver to
   `uac2` by Have Disk (the by-hand model); play, record, unplug while
   playing; a second unseen device still on `usbaudio`; back to
   `usbaudio`. Under xHCI98 and Windows'
   EHCI (UHCI for Full Speed on XP: the open case of 23 read again).
6. **UAC 2.0 regression**: `hs24`, `fs` (3 and 4 bytes), `x4full` and
   `alc4080cap` on 98 SE (xHCI98, NUSB) and XP: play, record, ramp,
   unplug; host tests all pass unchanged.
7. Host tests: the parser on every fixture and malformed variants
   (9-byte Output Terminals, synch endpoints with usage bits 0, discrete
   and continuous rates, mixed functions), the setup packets, the volume
   range from MIN/MAX/RES, the per-alternate rate pick, MaxPacketsOnly
   padding with PCM and USB lengths apart and null packets kept.

### 26.8 Estimate and risks

Agent time with reviews, option A with a: parser and its tests 5 h;
USB layer 4 h; controls and topology 4 h; INFs, folders, `STAGE.TXT`,
README and notes 2 h; simulator profiles 3 h and their check against
Windows 3 h; legs 10 to 12 h (two or three guests at a time); reviews
3 h. About 34 to 36 hours, two to three days. Option B adds about 5 h
(package, build, INF, folders, rows, a second image to sign) and a
coexistence leg per system. Option c adds the co-installer's rule and
its legs on every NT system (about 6 h).

Risks:

1. **UAC 2.0 regression** from shared changes: the family decided once,
   the UAC 2.0 code paths kept, host tests and leg 6.
2. **98 SE's own USB 1.1 stack**: the first-start hang of 16.5 reaches
   UAC 1.0 devices there, where most of them are; today those devices
   get `usbaudio`, which plays on real 98 SE machines. Read in leg 3;
   the INF cannot see the host stack, so only documentation can steer
   that stack's users.
3. **The unplug itself**: the X4 result is High Speed UAC 2.0; a Full
   Speed adaptive device may differ. Leg 2 decides before release.
4. **Device deviations** UAC 1.0 is known for (volume ranges, rates that
   do not read back, feedback only while locked, a rate request wanted
   before the alternate): each one met is a named quirk with the device
   in hand; no blanket tolerance.
5. **High Speed UAC 1.0 async OUT** with a shifted feedback value:
   refused by the 12.5% window today (23.2); `uac1fb.c`'s detection is at
   hand if a device needs it.
6. **Already-installed devices**: a user whose UAC 1.0 device is on
   `usbaudio` runs Update Driver once; the notes say how.
7. **MIDI**: a MIDI-only function on 98 SE gets `uac2` by class and fails
   its start (before: `usbaudio`, also without MIDI there); listed
   devices keep `usbmidi`; the `Drivers\midi` removal effect of 26.6 on
   98 SE (leg 4a).
8. **Formats `uac2` does not run** (G3): with option a such a device on
   98 SE loses `usbaudio` to a failed start until the user goes back;
   rare, not shown absent (26.6).
9. **Time**: 0.1.9's signing pass waits for the x64 image.

### 26.9 Owner questions

- **U1. A or B** (26.5). Recommended: A.
- **U2. ME automatic or by hand** (26.6). Recommended: by hand, with the
  9x folder split into `w98se/` and `wme/`.
- **U2a. 98 SE: class binding (a) with its stated loss, or d plus the
  by-hand model** (26.6). Recommended: a, behind G1 to G3.
- **U3. 2000 to 8.1**: by hand (recommended), or option c.
- **U4. Feedback loss on a UAC 1.0 asynchronous sink**: end the stream
  at `uacwatch.h`'s deadline as for UAC 2.0 (recommended: one rule,
  revisited with a device that needs it), or keep the last good value
  and count the loss (the 2026-10-23 rule for `uac1`).
- **U5. The xHCI98 installer row's words** (option A): "USB audio
  (uac2): USB Audio Class 1.0 and 2.0 sound cards, headsets and DACs".

### 26.10 The owner's decisions (2026-11-07)

- **U1: B**, a separate driver, `uac1`, built from source shared with
  `uac2`, not the `uac1-prior` copies; `uac2`'s behaviour unchanged, shown
  by its build (26.11) and its host tests and regression legs.
- **Scope: Windows 98 SE only.** One x86 package; no ME, 2000, XP, Vista
  to 10 package, no by-hand models there, no x64 image. ME keeps Windows'
  `usbaudio`, which xHCI98 `3.4.2.0`'s option B fixed for the unplug.
- **U2/U2a: 98 SE automatic by class, only if G1 to G3 pass**; otherwise
  option d (the devices read, by hardware id) and nothing by class.
- **U4: the stream ends at `uacwatch.h`'s deadline** when feedback stops,
  as for UAC 2.0.
- **MIDI-only functions** (owner, 2026-11-07): 98 SE's
  `usbaudio` has no USB MIDI ("Support for MIDI devices was added in
  Windows Me", Microsoft Learn, "USB Audio Class System Driver
  (Usbaudio.sys)"; `usbmidi.md` 3.1), so a MIDI-only (AC + MS) function
  loses no working MIDI to `uac1`. `uac1` starts such a function as a
  null function (no Code 10, the reason recorded); `usbmidi` keeps the
  devices it lists by hardware id, which outranks a class id, and its
  by-hand model (`USBMIDI_BY_HAND`) serves the rest. No MIDI engine in
  `uac1`. G3 carries the MIDI-only case.
- **Installer row**: "USB audio (uac1, Windows 98 SE): USB Audio Class 1.0
  sound cards, headsets and microphones" (xHCI98 revives
  `free:audio/uac1`; its `EXCLUDES` against `usbmidi` becomes `COEXISTS` on
  98 SE, by the reasons above).

### 26.11 As built (uac1 0.1.0.1)

**Sharing.** `uac2`'s driver sources moved to `src/audio/common/`
(`uacwdm.cpp` is the former `uac2wdm.cpp`; `uacdesc`, `uacusb`,
`uacstream.h`, `uacplan`, `uacvol`, `uacctl`, `uacrate`, `uacwatch.h`,
`uactopo`), compiled into each package by its own `build.cmd`. Every UAC
1.0 difference is under `#ifdef UAC_V1`, which only `uac1`'s build
defines, so `uac2`'s compiler input is its old one with the files in
another folder. The check: `uac2wdm.sys` x64 is byte-identical to the
image built before the move (time stamp and checksum aside); x86 is
identical but for the order of six compiler-generated adjustor thunks
(the same six, each jumping to the same function, and the vtable entries
that name them) - MSVC 6.0 orders them by where its headers were found.
`uac2`'s x64 include path takes `common` last, so the WDK's own
`punknown.h` (and with it its `IID_IUnknown`) is the one used, as before.
`uac2`'s host tests (`test.cmd`, `rtest.cmd`, `ctest.cmd`,
`mtest.cmd`) pass from the new paths.

**What `UAC_V1` changes** (`src/audio/common/`):

| File | UAC 1.0 |
|---|---|
| `uacdesc.h` | `UAC_ALT` gains the alternate's rates (`RateCount`, `RateMin`, `RateMax`, `Rate[16]`) and the class-specific endpoint `bmAttributes` (`EpControls`); `UAC_DESC` gains `Speed` |
| `uacplan.c` | `UacPlanHasRate`; `UacPlanPick` takes only an alternate that lists the rate |
| `uacusb.c` | `UacUsbRates` from the alternates' lists; no clock walk; the rate set by the endpoint's `SET_CUR` after `SELECT_INTERFACE` (`EndpointRate`, `GET_CUR` advisory, its answer in `ClockHz`); the speed from `UAC_DESC.Speed`; `UacUsbBusSpeed` for the parse; the diagnostic Feature Unit read skipped |
| `uacvol.c` | UAC 1.0 entity layouts: Output Terminal 9 bytes, Processing 7 and Extension 8, Feature Unit `bControlSize` bitmaps one bit per control |
| `uacwdm.cpp` | Feature Unit requests `SET_CUR`/`GET_CUR`/`GET_MIN`/`GET_MAX`/`GET_RES`; the volume range from MIN/MAX/RES under `UacVolRange`'s rules; one pair; the UAC 1.0 parse in `ParseDevice`; a null start (stages `0x40`, `0x41`) for a function with no alternate the driver runs; `Uac2Build` holds `uac1`'s build number, `NameFallback` is `uac1`'s |

**`src/audio/uac1/`**: `uac1desc.c` and `uacfmt.c` (the `uac1-prior`
parser, its SPDX line now GPL-2.0-or-later; its review notes left out),
`uac1model.c` (the parse as the shared model: PCM with 2- to 4-byte
subframes; each alternate's cluster `wChannelConfig`; an OUT alternate
with MaxPacketsOnly is not carried in 0.1.0.1, a G3 layout like PCM8 and
float), `uac1test.c` with `test.cmd` (the Brio 100 and Logi captures,
QEMU's layout, an asynchronous headset, a MIDI-only function, a mixed UAC
1.0 / 2.0 configuration, the unsupported layouts, hostile variants; built
with `UAC_V1` against the shared core), `build.cmd` (`wdmx86` only),
`uac1wdm.inf`, `uac1wdm.rc`.

**The speed.** The bus's answer (`USB_BUS_INTERFACE_USBDI_V1`: xHCI98, NUSB);
without one, the speed the function's descriptors alone fit
(`Uac1GuessSpeed`); else Full Speed, the only one 98 SE's own stack has.

**Registry values.** `uac1` reads the core's values under `uac2`'s names
(`Uac2QueueMs`, `Uac2Realtime`, `Uac2VolAboveZero`, `Uac2Level`) in its
own driver key; its INF writes the same defaults. The diagnostics
(`Uac2M0Stage`, `Uac2M0Parse`, `Uac2UsbStats`, `Uac2Build`) keep their
names too. In `Uac2M0Parse` the second word is the view (1 AudioControl, 2
AudioStreaming, 3 MIDIStreaming) with the speed (1 Full, 2 High) in bits
8 to 15, the third the alternates carried with the skipped ones in the
high half.

**INF.** `uac1wdm.inf`, undecorated `[Models]` only (no NT section):
`USB\Class_01&SubClass_01&Prot_00`, `&SubClass_00&Prot_00` and
`&SubClass_02&Prot_00`, and empty `[Models.NTx86]` and
`[Models.NTamd64]`, so XP and later never match it (read on XP: Windows'
`usbaudio` took a UAC 1.0 device with the INF staged); a function whose
PDO names an AudioStreaming interface starts as a null function. The
install section includes no Microsoft INF: 98 SE's Setup looks for an
included INF's `Needs` files and catalogs (`ks.sys`, the KS filters,
`ks.cat`, `wdmaudio.cat`) on the driver's own disk and asks for them, so
the section writes the device loader pair (`*NTKERN,MMDEVLDR.VXD`)
itself and a `RunOnce` entry has Windows install SysAudio from its own
`wdmaudio.inf` at the end of the install, as `uac2wdm.inf`'s 9x section
does (section 24).

**Gates and the owner's ruling (2026-11-07).** G1 failed (the real Logi
froze Windows at 2 of 5 unplugs while playing, and `uac2` froze the same
way); G2 met 98 SE's own first-plug freeze at 4 of 10, as Windows'
`usbaudio` did in the same test; G3 passed. A recording can stay stuck
after an unplug with `uac1` and `uac2` alike, Windows running on. The
owner ruled: `uac1` ships in 0.1.9 as an option, its INF by class as
built; Windows' `usbaudio` stays the default unless the user installs
`uac1` (xHCI98's installer row unticked); the three behaviours are known
issues. Record: [uac1-98se](../testing/2026-11-07-uac1-98se.md).

**Adaptive sources.** An adaptive IN endpoint whose `bSynchAddress`
names a synch OUT endpoint (UAC 1.0 Table 4-22; the `adsrc` fixture) is
read as a plain IN endpoint: the synch endpoint is selected with its
alternate and never fed, as Windows' own driver does on XP (record
[uac1-models](../testing/2026-11-07-uac1-models.md)).

### 26.12 The layouts `uac1` did not run (xHCI98 Phase 48, task 48.1, 2026-11-09)

Status: design, 2026-11-09, reviewed and revised. Owner, 2026-11-09:
close `uac1`'s format gap against Windows 98 SE's own `usbaudio.sys` (the
layouts of 26.6, "What a costs", that `uac1` 0.1.0.1 does not run);
decide in the design whether the driver converts or exposes the device's
own format. `uac1` stays a 98 SE package for testing in QEMU (README,
owner 2026-11-09).

**Where 0.1.0.1 stands.**

| Layout | 0.1.0.1 | Where |
|---|---|---|
| PCM8 (`wFormatTag` 2) | parsed, not carried: a function with nothing else starts as a null function (26.11, G3) | `uac1model.c` |
| IEEE_FLOAT (3) | skipped by the parser | `uac1desc.c` |
| ALAW (4), MULAW (5) | skipped by the parser | `uac1desc.c` |
| Adaptive source with a synch OUT endpoint | carried as a plain IN endpoint; the synch endpoint is selected with its alternate and never sent anything (26.11) | `uac1desc.c`, `uacusb.c` |
| MaxPacketsOnly IN (capture) | skipped by the parser | `uac1desc.c` |
| MaxPacketsOnly OUT (playback) | parsed, not carried | `uac1model.c` |

**What the specifications ask.**

- FMT-1 2.2.5 and 2.2.6: a Type I subframe is 1 to 4 bytes
  (`bSubframeSize`) holding `bBitResolution` significant bits,
  left-justified (the most significant byte of the little-endian
  subframe); PCM is signed two's complement; PCM8 is unsigned, 8 bits,
  0 to 255 (2.2.6.2); IEEE_FLOAT is IEEE-754 single precision, 32 bits in
  4 bytes (2.2.6.3); ALAW and MULAW are 8 bits a sample companded by
  ITU-T G.711 (2.2.6.4). All are Type I: the same format descriptor,
  frames of channels times subframe bytes, the same packet cadence as
  PCM. The G.711 octet is taken as G.711 transmits it (A-law with its
  even bits inverted, mu-law with all bits inverted), the form Linux
  exchanges for these two tags (`sound/usb/format.c` maps them to its
  A_LAW and MU_LAW sample formats; PCM8 to U8, IEEE_FLOAT to FLOAT_LE:
  facts only).
- FMT-1 2.2.1: at a constant rate a packet holds INT(nav) or INT(nav) + 1
  samples, nav the average a frame; where nav is a whole number, nav - 1,
  nav or nav + 1; the cumulative difference between nav and the packets
  stays within 1.5 samples.
- ADC-1 3.7.2.2: an adaptive source IN endpoint "is accompanied by an
  associated isochronous synch OUT endpoint that carries Ff", the
  average number of samples a frame the source must produce, 3 bytes in
  10.14 at Full Speed; a new value every 2^(10-P) frames, `bRefresh`
  reporting that exponent (1 to 9). USB 3.2 4.4.8.4.1, which restates USB
  2.0 5.12.4 (USB 2.0's own 5.12.4.2 and 5.12.4.3, which the owner
  named, were not at hand for this design): "an adaptive source endpoint
  must receive explicit feedback from the host so that it can accurately
  generate the number of samples required by the host", and each bus
  interval the source adds Ff to the fraction left from the previous one
  and sends the integer part. At High Speed the value is 4 bytes, 16.16
  samples a microframe (the form `uac2` reads from a feedback endpoint
  there, 23).
- ADC-1 4.6.1.2: with D7 (MaxPacketsOnly) set the host pads "all
  potential short packets (except null packets) with zero bytes to
  `wMaxPacketSize`" before sending to an OUT endpoint, and from an IN
  endpoint "must be prepared to receive more bytes than expected and
  discard the superfluous zero bytes".

**What 98 SE's audio stack takes above the driver.** `uac1` is a PortCls
WaveCyclic driver; on 98 SE every client reaches it through KMixer
(SysAudio builds the graph; WDMAud for waveOut and waveIn; DirectSound
and DirectSoundCapture through the same KMixer pin, as `uac1` offers no
hardware mixing pins). Read before (5.2): KMixer opens a device pin in
the first data range it can and converts between the client's PCM and
the pin's, capture included. Not read, and not knowable from headers:
whether 98 SE's KMixer opens a device pin in 8-bit PCM, in IEEE float
(`KSDATAFORMAT_SUBTYPE_IEEE_FLOAT` is defined in the 98 DDK's headers,
5.2), or in A-law or mu-law (no KS conversion filter for G.711 is in 98
SE's kernel audio stack; G.711 there is the user-mode ACM codec's).
What every 98 SE leg of `uac1` and `uac2` has run is a 16-bit PCM pin.

**Decision: convert in the driver, expose 16-bit PCM.** For an alternate
of PCM8, IEEE_FLOAT, ALAW or MULAW the pin offers 16-bit PCM (container
16, 16 valid bits) with the alternate's channels, channel mask and rates,
and `uac1` converts each sample between that and the wire format.

- It is the one pin format known to work on 98 SE for waveOut, waveIn
  and DirectSound alike; the alternatives each rest on an unread KMixer
  behaviour, and for A-law and mu-law there is none.
- Nothing is lost against exposing the native format: for playback
  KMixer mixes in at least 16 bits and would itself reduce to 8 bits or
  compand; for capture, PCM8 and G.711 decode exactly into 16 bits (G.711
  reconstructs at most 13 or 14 significant bits). IEEE_FLOAT capture is
  rounded to 16 bits (a float device's resolution beyond 16 bits is not
  carried; a 24-bit pin would rest on 5.2's open question).
- `UacStream` (`uacstream.h`) already takes only 16/24/32-bit PCM; WAVE's
  8-bit is unsigned and would need its own path there.
- The conversion is integer only (the driver uses no floating point and
  links no floating-point support): float bits are built and read with
  shifts.

**Where conversion happens.** In `uac1`'s build only (`#ifdef UAC_V1` in
the shared files, as 26.11; `uac2`'s compiler input unchanged):

- A new shared file `src/audio/common/uacconv.c` (`uacconv.h`), pure C
  with no Windows types, host-tested: `UacConvOut` (16-bit PCM to the
  wire format, n samples of the alternate's subframe), `UacConvIn` (the
  wire format to 16-bit PCM), the MaxPacketsOnly count below, `UacConvFf`
  (the synch value). Compiled by `uac1`'s `build.cmd`
  and test only.
- The plan (`uacplan.c`): a `UAC_FORMAT` holds the pin view in `Subslot`
  and `Bits` (2 and 16 for a converted format), so `Lookup`,
  `BuildRanges` and `DataRangeIntersection` work on what the pin offers
  unchanged, and the wire's subframe, bits and tag beside them;
  `UacPlanPick` matches the tag as well as channels, subframe and bits
  (PCM8, A-law and mu-law can share a subframe layout, IEEE_FLOAT and
  32-bit PCM do).
- The USB layer (`uacusb.c`) keeps counting wire bytes: its frame size
  is the alternate's (channels times subframe), its cadence, queue and
  statistics as now.
- The wave side (`uacwdm.cpp`) converts in its callbacks, in chunks of
  whole frames through a small buffer on the worker's stack: render
  (`Fill`) reads 16-bit frames from the ring, applies the software
  volume (`Soften`, on the 16-bit samples, as for a PCM device), and
  writes the wire format into the URB; capture (`Captured`) converts the
  received frames and writes 16-bit frames into the ring; render's
  `Progress` turns the wire bytes a URB completed into ring bytes (frames
  times the pin's block). Every wire byte of a render packet is written
  by the conversion, so silence goes out as the format's own (0x80 for
  PCM8, 0.0, 0xD5 for A-law, 0xFF for mu-law), not as the zeros the USB
  layer clears a buffer with.
- The queue (19.4) compares URB bytes with the client's ring: `Run`
  passes the ring's size in wire bytes (its frames times the wire frame).
  The queue's floor of two URBs stands as for PCM (a ring shrunk below
  two URBs is not followed, 17.4 item 5); with the ring's minimum of 16
  KiB and a Full Speed packet of at most 1023 bytes, two URBs of four
  packets fit any ratio here (at most 2 wire bytes per ring byte, IEEE
  float).

Sample rules (16-bit sample s, wire sample w, its significant byte b, the
other bytes of the subframe zero on output and ignored on input):

| Format | Render (s to w) | Capture (w to s) |
|---|---|---|
| PCM8 | b = (s + 128) >> 8, saturated to 127, plus 128 | (b - 128) << 8 |
| IEEE_FLOAT | s / 32768, exact (0 as +0.0) | w x 32768 rounded to nearest (half away from zero), saturated to -32768..32767; NaN, zero and denormals as 0 |
| ALAW | b = G.711 A-law of s >> 3 (13-bit) | G.711 A-law decode, << 3 |
| MULAW | b = G.711 mu-law of s >> 2 (14-bit) | G.711 mu-law decode, << 2 |

PCM8 out and back is s rounded to a multiple of 256 (saturating at the
top); float is exact both ways; G.711 decode then encode returns every
octet but mu-law's negative zero `7F`, which comes back as `FF` (both
decode to 0).

**Which formats a pin offers.** Per direction, as now, the first
terminal that a runnable alternate serves (26.4), its formats in
descriptor order, plus the converted ones. A converted format is offered
at a rate only where no format of the same pin view (16-bit, the same
channels and mask) ranked above it carries that rate; the rank is native
PCM, then IEEE_FLOAT, then ALAW and MULAW, then PCM8. So a terminal with
16-bit PCM alternates is offered what 0.1.0.1 offered, and among wire
formats with the same pin view the most precise is used. One change for
a device 0.1.0.1 did run: a function whose first terminal of a direction
had only formats 0.1.0.1 skipped, before a PCM terminal, now exposes that
first terminal (its alternates run now); the rule (the first runnable
terminal) is unchanged. A host test covers it.

**MaxPacketsOnly.**

- *OUT.* Each packet whose cadence count is not zero is laid out as
  `wMaxPacketSize` bytes: its audio first, zero bytes after (zero bytes
  as 4.6.1.2 says, for PCM8 too: the device discards them); a null
  packet stays null. The ring is read, the position advanced and
  completions counted by the audio bytes only, kept per URB beside its
  USB length (26.3).
- *IN, synchronous and adaptive sources, PCM and IEEE_FLOAT.* Padding
  cannot be told from audio by content, so the count rests on the rate,
  which the host knows for these sources: a synchronous one produces at
  the bus's rate, an adaptive one at the Ff it is sent (the cadence is
  then Ff itself, 10.14 or 16.16, not the nominal rate it was rounded
  from: 44.1 kHz's `0B0666` asks 44099.98 Hz). The stream keeps the
  frames owed: the cadence's sum (a running fraction advanced every
  interval) since the stream started or a packet was lost, less the
  frames taken. Each packet's audio is the count nearest the frames
  owed within FMT-1 2.2.1's bounds (INT(nav) to INT(nav) + 1, or nav - 1
  to nav + 1 for a whole nav), never fewer than up to its last frame with
  a nonzero byte, never more than it holds. A failed, lost or null
  packet starts the balance afresh (the frames it held are unknown;
  writing off a nominal count would carry a frame of debt per loss when
  losses fall on the short packets). Against a source within 2.2.1 the
  balance stays within a frame or two; it is held to two packets' worth
  against one that is not (the excess dropped, never repaid). So every
  frame with a nonzero byte is taken (inside the bounds), the frames
  taken follow the source's rate with no drift, and only frames of zero
  bytes at a packet's end are ever moved between packets, counted or
  dropped: in PCM and IEEE_FLOAT a frame of zero bytes is silence, so
  what moves is a frame of silence.
- *IN, asynchronous sources, and PCM8, ALAW or MULAW* stay not carried:
  an asynchronous source's rate is the device's, which the host can only
  estimate, and a content rule fails on silence (a source sending a zero
  last frame in every packet would lose a frame each packet); in PCM8 and
  G.711 a zero byte is a full-scale sample (PCM8 `00` is -1.0, mu-law
  `00` -32124), so a padding frame taken for audio, or a run of genuine
  `00` samples taken for padding, would be a loud error. A function with
  nothing else starts as a null function, as 0.1.0.1 did.
  Known limit (owner, 2026-11-09): these two cases stay not carried;
  such a device loads under `uac1` with no sound device (no hang).

**The synch endpoint of an adaptive source.** A capture stream whose
alternate is adaptive and names a synch OUT endpoint sends Ff on it for
as long as it runs: at Full Speed 3 bytes, round(rate x 16384 / 1000) in
10.14 (48 kHz is `0C0000`, 44.1 kHz `0B0666`); at High Speed 4 bytes,
round(rate x 65536 / 8000) in 16.16 a microframe. The value is the
nominal rate in bus time, which is the rate the host takes: the stream's
position is the frames received (17), with no host clock of its own to
report a difference against. It is sent in every service interval of
the synch endpoint (two URBs of eight packets kept in flight, the slots
the feedback of an asynchronous sink uses), not only every 2^`bRefresh`
frames: USBD has no per-endpoint interval to skip frames with, and the
value never changes within a stream, so repeating it carries the same
information. A failed or refused synch URB is counted (`Uac2UsbStats`'
feedback counters) and the slot sent again; 8 in a row end the stream
with an error, as the loss of an asynchronous sink's feedback does
(26.10, U4): a source that waits for Ff would otherwise leave a
recording that never advances.

**What does not change.** `uac2` (its build defines no `UAC_V1`: the
image built after this change must equal the one built before, as 26.11
checked); `uac1`'s PCM devices but for the terminal case above (their
pin formats, rates and transfers are those of 0.1.0.1); MIDI-only
functions (null start, 26.10); the gates' rulings of 26.11.

**Tests.**

- Host (`src/audio/uac1/test.cmd`): each conversion against fixed
  values (G.711 reference points: mu-law `FF` is 0, `80` is +32124, `00`
  is -32124; A-law `D5` is +8, `55` is -8, `AA` is +32256, `2A` is
  -32256; float `3F800000` saturates to 32767, `BF800000` is -32768, NaN
  is 0) and round trips as stated above (every 16-bit value through PCM8
  and float; every octet through A-law and mu-law decode and encode);
  wider subframes; the MaxPacketsOnly count (bounds, the frames owed,
  nonzero frames kept, a second of silence without drift, a lost packet,
  2.2.1's 47/49 sizes with every short packet lost then silence, and an
  adaptive source following Ff `0B0666` for five minutes with one
  packet in seven silent); the Ff values; the parser and model carrying
  the four tags and MaxPacketsOnly both ways (and not a MaxPacketsOnly
  source that is asynchronous or in PCM8); the format ranking; the
  terminal case.
- Simulator (`uac1emu.py`): `pcm8` and `float` (playback) and `adsrc`
  (capture) become positive profiles; new profiles `alaw` and `mulaw`
  (playback and capture), `pcm8in` and `floatin` (capture), `mpoin`
  (MaxPacketsOnly capture, synchronous, 44.1 and 48 kHz) and `mpoout`
  (MaxPacketsOnly playback in PCM8, so padding and conversion meet). The
  model decodes every format, reports the received level and the tone's
  frequency (zero crossings) for playback, sends its 1 kHz capture tone
  in the format (padded for MaxPacketsOnly, with FMT-1 2.2.1's packet
  sizes), and an adaptive source produces its frames at the Ff it was
  last sent, logging the values.
- Legs, 98 SE under xHCI98 in QEMU, each profile: installed from the
  folder on `DevicePath`, `uac1` bound, not a null start; playback at 48
  and 44.1 kHz with the model reading UACWAV's -20 dBFS 440 Hz tone
  within 1 dB and 2% and every buffer completed in its time (the
  position across ring wraps), played twice (stop and restart);
  recording with UACWAV reading the 1 kHz tone within 2% and its level
  within 1 dB of the model's -6 dBFS (UACWAV's `rec` gains a peak level
  line); `adsrc`'s synch values equal to the rate, also at High Speed.
  The legs under NUSB's EHCI (UHCI for Full Speed in QEMU) and 98 SE's
  own UHCI, and failure injection on the synch pipe, are owed unless run
  and recorded. `uac2`: the image comparison above, its host tests, and
  a play and record of `hs24` on the same base.

### 26.13 A pipe the stack refuses start frames on: a resync at explicit frames (xHCI98 Phase 48, task 48.1, 2026-11-09)

Status: design and code, 2026-11-09, reviewed and revised; revised
2026-11-10 (items 4, 5 and 8: a recording's empty URBs, and frame requests
the worker does not wait for; build stamps `uac2` 28, `uac1` 4). Applies to
`uac2` and `uac1` alike (`uacusb.c` is shared).

**The report.** Under 98 SE's own UHCI driver some `uac1` streams stopped
completing buffers mid-stream (A-law play at 44.1 kHz: 1 of 8 buffers, the
stream stopped after 240 packets; mu-law `ramp`; the adaptive source's
recording at 48 kHz: 0 of 8), the next stream of the same device passing
([uac1-formats](../testing/2026-11-09-uac1-formats.md), "Under 98 SE's own
UHCI"). `uvc98` met the same stack refusing every transfer resubmitted
with `USBD_START_ISO_TRANSFER_ASAP` after a guest stall, as a whole, with
`USBD_STATUS_BAD_START_FRAME`, and recovered by resubmitting at explicit
start frames ([uvc.md](uvc.md) 5.4a).

**What the counters showed.** `uac2` 0.1.10.0 (the release image) with
`uacemu.py --profile fs` on 98 SE's own UHCI, a recording at 44.1 kHz that
completed none of its buffers (`Uac2UsbStatsIn`, read from the registry
after the step): 384 data URBs, of which 250 failed as a whole, every one
with URB status `C0000A00` (`USBD_STATUS_BAD_START_FRAME`), and 134 did
not; `LastStatus` `C000009C` (`STATUS_DEVICE_DATA_ERROR`). 250 is
`UacPlanLostLimit` for 4 ms URBs: 25.8's run limit ended the stream. (The
packet counter `DataPktOk` is no measure here: it counts packet status
fields, and those of a URB refused whole are left at 0, success.) The
simulator polled the endpoint for 0.49 s of the stream (486 packets) and
not again until the client closed it 13 s later: the 250 refusals came
back within that half second. The same device's other 23 streams in the
leg passed, as in the report.

**Why 25.8 does not recover.** 25.8 makes a data URB refused for its start
frame a lost interval and resubmits the slot ASAP. Once this stack
refused one, it refused every resubmission at once, so the run of lost
URBs reached `UAC_LOST_LIMIT_MS` (250 URBs of 4 ms) in under half a second
of real time and the stream ended with `STATUS_DEVICE_DATA_ERROR`. Read
(as `uvc98`'s 5.4a reads it; a reading, not shown from the stack's own
code) as the stack placing an ASAP transfer after the last frame it had
scheduled for the pipe, a frame already past after a stall. 25.7 does the same for feedback (the
pipe turned off after 8) and 26.12's synch OUT endpoint ends the stream
after 8 failed synch URBs in a row.

**The change.** For a URB that comes back with URB status
`USBD_STATUS_BAD_START_FRAME`, on any pipe of the stream:

1. It is handled as before: a data URB is a lost interval (25.8: counted,
   the render position advanced past its bytes, nothing delivered for
   capture, the run of lost URBs bounded by `UAC_LOST_LIMIT_MS`); a
   feedback URB counts toward 25.7's run of 8 (at the 8th feedback turns
   off for the stream, as before); a synch URB to an adaptive source
   counts toward 26.12's run of 8.
2. The stream is marked for a resync. While marked, no idle slot of the
   stream (data or feedback) is resubmitted; the worker goes on retiring
   completions and waiting as before, so a Stop wakes it and drains as
   before.
3. Once no slot is with the stack, the worker reads the bus's frame
   number (`URB_FUNCTION_GET_CURRENT_FRAME_NUMBER`, as `UacUsbStart`
   already does; sent and not waited for, item 8) and, once it is back
   with no Stop seen, submits every slot of each pipe, in slot order,
   without `USBD_START_ISO_TRANSFER_ASAP`, slot k of a pipe at
   `current + 8 + k * F` (`UacPlanResyncFrame`), F being the frames one
   URB of that pipe covers (`UacPlanSpanFrames`: a data URB's span, 4 or
   8 ms; a feedback or synch URB's packets times their service interval),
   rounded up to whole 1 ms frames. A render slot is filled afresh as it
   is submitted (4.4: the refused bytes are not sent again). Later
   resubmissions are ASAP again: the stack continues after the explicit
   ones, which lie ahead.
4. Bounded: `UAC_RESYNC_LIMIT` (4) resyncs with no data URB that carried
   audio between them end the stream with `STATUS_DEVICE_DATA_ERROR`, as
   do a failed frame request and a failed submission. A render URB
   carries audio when it comes back successful; a capture URB only when
   it delivered at least one PCM frame to the client: a successful capture
   URB with nothing in it is no evidence the stream recovered (98 SE's own
   UHCI returns a recording's URBs placed behind the bus so, between its
   refusals; one recording resynced 22 times and captured 192 bytes,
   [uac-uhci-stalls](../testing/2026-11-09-uac-uhci-stalls.md)). The hold
   too: a resync still wanted `UAC_RESYNC_WAIT_MS` (1 s, interrupt time)
   after the hold began ends the stream the same way
   (`UacPlanHoldExpired`), whether a slot or the resync's frame request is
   still with the stack or the frame came back only after the deadline;
   it is read again just before the resync runs (the stop's drain then
   cancels what is out), where before 25.8's run limit ended it. A Stop
   seen during the resync keeps its own status.
5. A data URB lost whole (25.3: URB status `USBD_STATUS_ISOCH_REQUEST_FAILED`,
   every packet failed), or a capture URB back successful that delivered
   no PCM frame, that the stack had placed on frames ending more than 8
   frames before the bus frame read before it was submitted
   (`UacPlanBehind`) marks the resync too (below, "The lag before the
   refusal"). The frame is read (one frame request before each ASAP data
   submission) only from such a URB until the next data URB that carried
   audio (item 4), so a stream that loses none makes no extra request;
   the first such URB of a run only arms it, and empty capture URBs keep
   it armed. A frame read before the submission can only make a URB look
   less behind than it was. `UacPlanDataDone` holds the rules of items 4
   and 5, `UacPlanResyncStart` the limit.
6. Stats words 95 to 97 (`Resyncs`, `ResyncFrame`, `ResyncBehind`): the
   resyncs of the stream, the frame the last one read, and how many of
   them a URB behind the bus began.
7. `uac1`, an adaptive source's synch OUT endpoint (26.12): a synch URB
   that comes back successful with at least one packet delivered ends
   26.12's run of 8 failures, as a whole good one does (its failed packets
   counted, code `10000000`). Once a synch packet has been delivered in the
   stream, a failed synch URB, whatever its status, is a lost interval:
   counted (`BadFeedback`; its URB status with bit 28 set, and its IRP
   status when that failed), never ending the stream; the source runs at
   the last Ff it was sent (26.12), and the data stream's own rules still
   end a stream whose data fails. Before the first delivery the run of 8
   stands, so a source that never gets an Ff (26.12: it would never start)
   still ends the stream. A refused start frame marks the resync either
   way. Read on 98 SE's own UHCI: with 0.1.1.0 (leg `uac1-w98-own-b2`) 7 of
   8 recordings from `adsrc` failed, four at 25.8's run limit as above and
   three within 26 data URBs of their start, at 26.12's run (8 to 10 of
   their 9 to 13 synch URBs back failed, no data URB failed, `LastStatus`
   `C000009C`); with the build before this item (leg `uac1-w98-own-a4`) a
   recording ended after 121 data URBs, none failed, at the same run: 51
   synch URBs delivered, then 9 failed in a row, URB status `00000B00`
   (`USBD_STATUS_ISOCH_REQUEST_FAILED`'s code without its error state).
8. The worker never waits on a frame request (item 3's and item 5's).
   It sends one (an IRP of its own, the URB in the stream's storage, one
   out at a time), goes on retiring completions, enforcing the hold's
   deadline and seeing a Stop, and takes the frame when the request comes
   back (`FrameDone` wakes it; the worker takes it, freeing its IRP, at
   the start of a pass or wherever it next wants a frame). A request back
   and not yet taken still holds the storage: no other is sent over it.
   A synchronous request (`UsbRequest`'s 5 s
   wait, then a drain with no deadline) on the sole worker would have
   left it unable to cancel the data IRPs out, keep the 1 s hold or
   answer a Stop, and `UacUsbStop` waits for the worker. While item 5's
   frame is wanted, an idle data slot is not submitted until the request
   is back (`UacPlanDataGate`); the slot is published as the stack's only
   in the step that passes it on, and a Stop is read after the frame is
   taken and again, under the stream's lock, just before the slot is
   published. A Stop cancels the request out at once, and a request for
   item 5 out more than `UAC_FRAME_WAIT_MS` (100 ms) is cancelled too, the
   data slots then going without a frame (their completions cannot start
   a resync by lag) and no other request sent until it is back; the
   resync's own request is bounded by the hold (item 4). The stack reads
   the frame somewhere in the time the request was out, so the resync
   takes the frame read plus those milliseconds when they are at most
   `UAC_FRAME_FRESH_MS` (64) (`UacPlanResyncAhead`): the first slot is then
   never behind the bus, and the last slot of the largest queue (32 URBs
   of 8 frames) at most 64 + 8 + 31 x 8 = 320 frames ahead, inside
   the stack's start frame range (`USBD_ISO_START_FRAME_RANGE`, 1024). A
   request out longer is read afresh, the hold bounding the retries. A cancelled
   request keeps its storage until the stack gives it back, and the
   worker does not exit while one is out (`UacPlanWorkerExit`), as for a
   data IRP. A frame not used in the pass that took it is dropped: a
   submission or a resync reads one afresh.

**The lag before the refusal (read with the first build of this
section).** That build resynced only at a refusal, and its streams went
on where the release's ended (a 5 s play: 8 resyncs, no stream end), but
the simulator saw about 1.3 s with no packet before each resync. The
trace build (`build.cmd wdmx86 trace`) read why: after a stall the stack
went on placing ASAP URBs right after the last frame it had scheduled,
by then behind the bus (start frame `0x31259` with the bus at `0x312b2`,
then `0x312a5` at `0x3180c`), so each came back lost whole (URB status
`C0000B00`, every packet `C000000F`, not accessed), and the lag grew until
the stack refused a start frame, about a frame list (1024 frames) later.
The resync then ran at once (the held slots came back within 70 ms) and
the explicit URBs played. A URB lost whole on frames that far behind the
bus is therefore taken as the same state, and resynced at once. The
test is against the frame at submission, not at completion: a stack that
holds URBs and gives them back late (7's EHCI, 25.2) or a controller that
skips frames under load placed those URBs ahead of the bus when they were
submitted, and is not resynced (the trace's URBs after the resync: placed
ahead, some skipped by the emulated controller, none behind).

A stack that never refuses a start frame (xHCI98; Windows' EHCI and UHCI on
2000 and later in the legs of section 25) never takes this path. Explicit
`StartFrame` scheduling for normal running stays as 25.4 has it (ASAP);
this section is the one exception to 25.4's first item and to the ASAP
resubmissions of 25.7 and 25.8.

**Recordings behind the bus.** On this stack a recording's URBs placed
behind the bus came back successful with empty packets, not lost. The
first revision did not see them: the lag test took only URBs lost whole,
and such a URB reset the resync limit's run, so a recording resynced 22
times with nothing captured between. Items 4 and 5 now take them (an
empty capture URB arms the lag test and, placed behind the bus, starts a
resync; it never clears the run). Not yet read on the stack with a trace
([uac-uhci-stalls](../testing/2026-11-09-uac-uhci-stalls.md), "Left").

**Tests.** `desctest.c`: the recorded failure as a sequence (refusals
alternating with successful empty capture URBs: four resyncs, the fifth
ends the stream; with an audio URB between, never), the lag test armed
and kept by empty URBs, started once by one behind the bus, cleared only
by audio; `UacPlanFrameCancel`, `UacPlanDataGate`, `UacPlanResyncGate`
and `UacPlanWorkerExit`, and the worker's passes modelled with a frame
request the stack holds for 300 ms, a Stop arriving 30 ms in (cancelled
in that pass, no slot submitted, the worker leaving only once the request
is back) and with no Stop (cancelled after 100 ms, the slots going without
a frame, no second request until it is back); the request completing
at every point of the worker's passes (before and after a pass's take,
between idle slots, inline), none sent over one back and not taken, each
taken exactly once; the hold's deadline with the resync's frame back
just before it (the resync runs) and just after it (the stream ends, no
resync), and the resync's frame aged while fresh, read afresh past 64 ms,
never behind the bus and inside 1024 frames for the largest queue
wherever in the time out the stack read it. These are vectors of the
worker's rules, not of `uacusb.c` itself: no host harness runs it. Also
the constants; `UacPlanResyncFrame` at index 0,
later slots, the 32-bit wrap, 32 slots back to back; `UacPlanBehind` on
the trace's frames, at and past the slack, ahead, across the wrap and
past 2^31; `UacPlanSpanFrames` for 4 and 8 ms spans, a part frame
rounded up, and 0. Legs: 98 SE's own UHCI with `uac1`'s Full Speed
profiles and `uac2`'s `fs`, several rounds of playback and recording,
before and after, with the stream counters read after every failed step
([uac-uhci-stalls](../testing/2026-11-09-uac-uhci-stalls.md)).

### 26.14 Recording level, other playback units and the input chooser (2026-11-17)

Section 28 applies to `uac1` as to `uac2`: the capture walk, the
recording volume and mute, the first-arrival pass, the playback units
beyond the one controlled, and the input chooser all live in the shared
core (`uacvol.c`, `uactopo.c`, `uacwdm.cpp`). What differs for USB Audio
1.0 is listed in 28.13: the Selector Unit's layout and requests, the
Feature Unit's MIN/MAX/RES, the INF lines, and the systems (98 SE only,
26.10). The `uac1` layouts read so far: the Logi headset (capture Input
Terminal 1, Feature Unit 2, Output Terminal 3; no selector), the Play! 2
(Input Terminal 1, Feature Unit 6, a one-input Selector Unit 10, Output
Terminal 3: no chooser), and `uac1test`'s two-input capture selector
fixture (`selcfg`).

## 27. A pin opened while the one before is still being destroyed (xHCI98 task 49.8, 2026-11-11)

Status: design and code, 2026-11-11; build stamps `uac2` 29, `uac1` 5.
Applies to `uac2` and `uac1` alike (`uacstream.h` and `uacwdm.cpp` are
shared).

**The report.** Under ME's own UHCI driver `UACWAV rec 1 16 44100 3` came
back `open=8` 3 times in 18, the pin's `NewStream` refused with
`STATUS_DEVICE_BUSY` at the capture direction's one-stream gate
([uac-uhci-stalls](../testing/2026-11-09-uac-uhci-stalls.md), "ME's,
2000's and NUSB's UHCI"). ME's audio stack creates and closes several
capture pins at each 44.1 kHz open.

**The cause, by reading.** The gate (`UAC_DIR.Active`) is taken by
`UacStream::Create` and freed only in the stream's destructor, after the
transport's `Close` hook. A published stream's final `Release` never
destroys inline (the transport may be its caller): it queues the destroy
to a work item (`DelayedWorkQueue`; on x86 an executive work item). Until
that item runs, the gate stays held, so a pin created right after the
close of the one before is refused busy. On 98 SE, 2000 and NUSB the item
ran in time in every leg; ME's back-to-back pin creation at 44.1 kHz met
it 3 times in 18. `Close` takes the wave filter's operation mutex, which
`NewStream` holds around `Create`, so waiting under that mutex would only
delay the refusal.

**The change.**

1. The gate has three values: free (0), open (1), and closing (2). A
   published stream's final `Release` sets closing before it queues the
   destroy; the destructor frees the gate as before, after `Close`.
2. `NewStream`, before it takes the operation mutex, waits while the gate
   reads closing (`UacStream::WaitClosing`: 10 ms sleeps at PASSIVE, up to
   `UAC_CLOSE_WAIT_MS`, 2 s), then takes the mutex and calls `Create` as
   before. A gate held by an open stream is refused at once, as before: a
   second stream a direction is still busy. A destroy that has not run
   after 2 s leaves the refusal as before.
3. Diagnostics, written and never read: `Uac2Waits` (REG_BINARY, three
   DWORDs of the direction last opened: opens that met a closing stream,
   those still closing after the wait, the last wait in milliseconds).

**Tests.** `streamtest.cpp`: an open gate returns from the wait at once
and stays refused; a closing gate is waited for until its queued destroy
frees it (the clock moved only by the waits), then the open succeeds; a
destroy that never runs ends the wait at the bound and the open is refused
busy; no wait above PASSIVE. Legs: ME's own UHCI, 18 or more recordings at
44.1 kHz with the counters and `Uac2Waits` read, a regression round at
48 kHz and on 98 SE.

## 28. Recording level, the other playback units, and the input chooser (2026-11-17)

Status: design, 2026-11-17, revised five times, with the owner's
answers; the playback engine is moved (28.15) without a change of
behaviour, and no new device-facing behaviour exists yet: stage 1 of the
wiring (28.18) builds the capture graph at start and writes its route
diagnostics, and makes no request to the device; stage 2 (the route
table's open and close reservations) is designed in 28.19 to 28.25 and
not wired. Applies to `uac2`
and `uac1` alike (the shared core; `uac1`'s differences and scope are
28.13). The owner's decisions are D1 to D3, his answers A1 to A6 to the
first draft and B1 to B6 to the second (28.2); every other choice here
is provisional and marked so. 28.17 lists what is still open, and 28.25
stage 2's questions.

### 28.1 What was found

Read in the sources (`uacvol.c`, `uactopo.c`, `uacwdm.cpp`) and shown by
host checks against the code as it is (the X4 and Logi fixtures):

1. **Nothing reads or writes a Feature Unit on the capture path.**
   `LevelSetup` runs for the render direction only (`dir[0]`), and
   `UacVolFind` walks from an Output Terminal that is *not* USB
   streaming back to a USB streaming Input Terminal, so a capture path
   (ending at a USB streaming Output Terminal) is never walked: for the
   Logi's capture Output Terminal 3 it finds nothing, and for the X4's
   microphone Input Terminal 4 it finds the monitoring path to the
   speaker (Output Terminal 5 through Feature Unit 13 and Mixer Unit 7),
   not the capture path. The topology's capture line is a bridge pin, a
   SUM node and the line out (`uactopo.c`): no VOLUME, MUTE or MUX node.
   A device whose capture unit powers up muted, or at the bottom of its
   range, records silence, and Windows shows no recording control.
2. **On playback only one Feature Unit is controlled.** `UacVolFind`
   takes the first path to an Output Terminal and on it the unit nearest
   the Output Terminal with the control (`Pick`); the extra channels of
   that one unit are normalised once the driver owns the master
   (`VolumeExtra`, `MuteExtra`). Any other Feature Unit on the path keeps
   its power-up state, so a second unit that powers up muted or at its
   minimum keeps playback silent or quiet whatever the user sets.
3. **No Selector Unit is read or set.** The X4's capture Output Terminal
   6 is fed by Selector Unit 26 with four inputs (Feature Units 2, 23, 25
   and 8, behind Input Terminals 4 microphone `0x0201`, 18 digital audio
   interface `0x0602`, 24 line connector `0x0603` and 19 S/PDIF
   `0x0605`); the driver never asks its position and offers no choice,
   and the capture bridge's category comes from `UacVolJack`, which
   returns the first Input Terminal by id that the walk reaches through
   any input of the selector, whatever its position.

Also found while reading, for correction with this work:

- Section 5.3 says per-channel controls take precedence over the
  master; `Pick` does the opposite (a read/write master on any unit of
  the path wins, and the same unit's channel controls are normalised to
  0 dB as extras), which is what the playback legs ran. 5.3 is
  corrected to the code.
- `Drain` fills the render level's volume words (`VolumeStats`) into
  `Uac2UsbStatsIn` too, the capture stream's record; 28.12 gives the
  capture record its own.
- `Walk` keeps one visited set for the whole search from an Output
  Terminal, so a unit reached a second time by another route is not
  entered again: two routes that converge (a diamond) yield one. That
  is harmless for "the first path" playback takes today, wrong for the
  coverage rule of 28.4; the new walk keeps a per-route stack (28.3).

### 28.2 The owner's decisions (2026-11-17)

His decisions, recorded as given:

- **D1. Capture level.** Recording volume and mute controls that
  Windows shows, from a capture walk from the USB streaming Output
  Terminal back to the selected Input Terminal, the level saved per
  endpoint; and on a device's first arrival only (no saved level yet)
  the capture path unmuted (master and per channel) and a volume found
  at its minimum or at silence raised to the top step at or below 0 dB.
  After that the saved level rules.
- **D2. Playback.** Once the driver owns the level, every other Feature
  Unit on the playback path normalised to unmuted and 0 dB (or the top
  step at or below 0 dB), as extra channels are today. (Bounded by
  B4; 28.9.)
- **D3. Capture source.** An input chooser now: a MUX node with one
  source per selector input, named by terminal type.

His answers to the first draft's questions:

- **A1. Names.** Windows' own localised names where a standard one
  exists; the driver's own strings (drafts for him) only for kinds
  Windows has no name for, and for "Stereo Mix" (28.10).
- **A2. No cap on recording.** The whole capture range is offered;
  `Uac2VolAboveZero` stays playback-only; only the automatic
  first-arrival adjustment stops at 0 dB.
- **A3. Per input.** Each input gets the first-arrival pass the first
  time it is chosen. This extends D1's "device's first arrival" wording,
  with the owner's agreement.
- **A4. An opt-out.** A visible, documented setting the INF writes
  without overwriting, `Uac2CapFirst` (draft name), default 1.
- **A5. Numbering.** Inputs of the same kind are numbered, in the form
  he chose: the first unnumbered, then 2, 3 ("Line In", "Line In 2").
  Numbering every one ("Line In 1", "Line In 2") was put back to him;
  his form stands (B5).
- **A6. The recording control's unit.** The unit nearest the Output
  Terminal, as playback does; it must be the nearest *eligible* unit
  (whole-signal coverage, channel compatibility, ownership; 28.4), a
  soundness condition, not a change of the decision.

His decision on the third revision (2026-11-17):

- **C1. A live change of the recording input is refused** while a
  recording that depends on the route is open, on every system,
  accepted as provisional: to be confirmed or changed after the mixer
  legs on each system (28.7, 28.16).

His answers to the second draft's questions:

- **B1. The saved input is restored at start**, as the levels are,
  after the saved route is validated and the selectors confirmed (28.7).
- **B2. The reduced forms only where a leg shows the need**: no chooser
  on 98 SE and ME, and a single endpoint without a MUX on Vista and
  later, each accepted only where a guest leg shows it is needed, with
  the limitation recorded (28.10).
- **B3. More than eight sources: no chooser** (28.7).
- **B4. Playback normalisation is bounded**: 0 dB only before the
  controlled volume and before any join, elsewhere mutes released only;
  the existing behaviour for extra channels kept (28.9).
- **B5. His numbering form stands** ("Line In", "Line In 2"; 28.10).
- **B6. Eventual discovery** of a device-side switch is accepted with
  the stated recording limitation; no polling during recording; the
  interrupt endpoint is later work of its own (28.8).

### 28.3 The capture walk as a graph

Entities and layouts are ADC-2 4.7.2 (UAC 2.0) and ADC-1 4.3.2 (UAC
1.0); the walk checks every entity it reaches in full ("Well-formed
entities", below) and reads the Selector Unit's control bitmap (28.7).

**The graph.** For each capture endpoint (its USB streaming Output
Terminal, `wTerminalType` `0x0101`; one per pair, section 15.3) the walk
builds the upstream graph by `bSourceID`/`baSourceID`, never by
`bCSourceID`: Clock Sources, Clock Selectors and Clock Multipliers (UAC
2.0 `0x0A` to `0x0C`) are not on the audio path and never a source or a
MUX; a Clock Selector stays section 4.2's. Each entity is classified:

| Class | Entities | What the walk does |
|---|---|---|
| Pass | Feature Unit; Sampling Rate Converter and Effect Unit (UAC 2.0); a one-input Selector Unit (no request) | one source; the channel cluster unchanged |
| Opaque | a Processing or Extension Unit with one input | one source; what it does to level and channels is unknown (28.4) |
| Join | a Mixer Unit; a Processing or Extension Unit with two or more inputs | every input entered in `baSourceID` order; a Mixer Unit is taken to sum its inputs (ADC-2 4.7.2.6), a multi-input Processing or Extension Unit is not assumed to sum: it is a join and opaque |
| Choice | a Selector Unit with two or more inputs | each input a branch; 28.7 makes the branches sources or fixes them |
| End | an Input Terminal | a non-streaming terminal ends a route; a USB streaming one too (the playback signal fed back: 28.10's "Stereo Mix") |

**Routes, not one path.** A route is one sequence of entities from the
Output Terminal to an End. The walk enumerates every route with a
per-route stack, so two routes that converge on one unit are both kept;
the unit is listed once in the graph's entity set. An entity already on
the current route is a loop: that route is cut there (it reaches no
End), the walk goes on with the other routes so that the diagnostics
list them, and the graph is incomplete (below): a unit fed through its
own output has no signal the walk can account for, so what the looping
route carries into the recording is unknown. Bounds: depth 32 per route, at
most 64 routes per endpoint, and the layout checks below.

**Well-formed entities.** An entity is malformed unless its descriptor
lies inside the configuration and holds every field its subtype
defines through its last string index, each variable part sized from
what determines it (the ADC-1 size in parentheses, n its
`bControlSize`), with the one tolerated exception named below (a
master-only Feature Unit):

- Input Terminal 17 (12); Output Terminal 12 (9); Selector Unit 7 + p
  (6 + p); SRC 8; Extension Unit 15 + p (13 + p + n).
- Mixer Unit 13 + p + N (10 + p + N): N the bytes of `bmMixerControls`,
  one bit per input channel of all input pins together and output
  channel (`bNrChannels`), rounded up (ADC-2 Table 4-11, ADC-1 4.3.2.3).
- Feature Unit 6 + (ch + 1) * 4 (7 + (ch + 1) * n) and Effect Unit
  16 + 4 * ch, ch the channel count of the cluster actually entering the
  unit, not any count the length allows: a stereo cluster needs the
  master's and both channels' entries.
- The Effect Unit's length: ADC-2's tables contradict themselves. Tables
  4-15 to 4-19 state `bLength` 16 + 4 * ch (`iEffects` at 15 + 4 * ch),
  while their fields laid out contiguously (`bmaControls(0)` at 7,
  `bmaControls(1)` at 11, four bytes each) end with `iEffects` at
  11 + 4 * ch, implying 12 + 4 * ch. The larger is required here, as the
  conservative choice: a device built to the shorter reading gets an
  incomplete graph and so no write eligibility. That is a compatibility
  limitation of this driver, not a finding that such a device is
  invalid; accepting the shorter layout would take an explicit
  compatibility decision.
- A Feature Unit with the master's entry alone (ch 0) on a cluster of one
  or more channels is a **tolerated nonconforming layout**, not a well
  formed one: both specifications require the master's entry and one
  entry per logical channel (a channel without controls has a zero
  entry). It is accepted because recorded devices have it (the Logi
  headset and the Brio 100, on mono and stereo clusters), and accepting
  it is bounded: the master bitmap lies whole at its fixed offset for
  any cluster, and no channel's controls are read from the unit (it
  offers none). A partial channel array (some channels' entries, not
  all) stays malformed.
- Processing Unit: the common part (16 + p; 13 + p + n) and the
  process-specific part its process type defines, on the one input pin
  every defined type has: Up/Down-mix and Dolby Prologic `bNrModes` and
  that many modes (18 + 4 * m; 15 + n + 2 * m); the UAC 2.0 Stereo
  Extender and the UAC 1.0 3D-Stereo Extender, Reverberation, Chorus and
  Dynamic Range Compressor nothing more. A process type neither
  specification defines (`PROCESS_UNDEFINED` included) has no tail that
  can be checked, so that unit is malformed, as is a Processing Unit with
  two or more inputs: in practice the multi-input opaque join of the table
  above is an Extension Unit.

A cluster's channel count is established upstream: an Input Terminal,
Mixer, Processing or Extension Unit states its own `bNrChannels`; a
Feature Unit, SRC or Effect Unit passes its input's; a Selector Unit its
inputs' where they agree (ADC-2 4.7.2.7 and ADC-1 4.3.2.4 require it).
Where none is defined (a Selector whose inputs differ, a count of 0) a
layout that depends on it is malformed. A check that meets a route the
walk cuts for its own reason (a loop, a missing or unlisted entity)
leaves the reason to the walk.

**An incomplete graph writes nothing.** The graph is incomplete when
any route was cut: more than 64 routes, a route deeper than 32, a loop
(an entity met again on its own route), an entity whose descriptor is
short, malformed or out of the configuration, a source id naming no
entity, or an entity class the table above does not list. One cut route
is enough: a graph with one valid branch and one loop is incomplete as a
whole, since the cut branch may join the signal the valid branch
carries. Coverage (28.4) and the pass's target set
(28.6) cannot be known from an incomplete graph, so for that endpoint:
no chooser, no first-arrival pass, no hardware write of any capture
control (level, pass or selector), and the level in software only
(28.11); the selectors and controls may still be read for the
diagnostics. The reason is recorded (`GraphFlags`, 28.12). The same
holds for D2 on the playback side: a playback route the new walk finds
incomplete gets no normalisation write (28.9); the playback level
control `UacVolFind` picks today is kept as shipped (28.15).

**The selected graph** of an endpoint is the union of the routes the
selected source has (28.7: through the chooser at that source's input,
inner selectors at the source's route, selectors behind a join at their
read position). A source's own graph is the same with that source
chosen. Every rule below names which graph it uses.

### 28.4 Which control carries the level: eligibility

On a source's graph, a Feature Unit control (unit, control, channel) is
**eligible** to carry that source's recording level only when:

1. **Whole-signal coverage.** The unit lies on every route of the
   source's graph (it dominates every End). A unit on one input of a
   Mixer Unit controls one part of the recording only and is never the
   level's control; a unit after the join is.
2. **No opaque entity between it and the Output Terminal** for a
   per-channel control, and none that is a join: across a multi-input
   Processing or Extension Unit nothing upstream is eligible (the
   output need not depend on any one input linearly). Across a
   one-input opaque unit a master control upstream stays eligible (it
   still sets the level going in), a per-channel control does not.
3. **Channel compatibility** for a per-channel control: the unit's
   logical channels are the cluster the Output Terminal receives, with
   only Pass entities between (a Mixer Unit, a Processing up/down-mix,
   an Extension Unit or a Selector whose inputs differ change the
   cluster; ADC-1 and ADC-2 both define channel-changing processing).
   Correspondence must be proved, not inferred from equal channel
   counts: so per-channel eligibility is given only across one-input
   Pass entities (Feature, SRC, Effect, a one-input Selector), and
   refused across any multi-input Selector even where its inputs'
   counts agree (provisional: a cluster-identity check could later
   admit a selector whose inputs carry the same cluster). A master
   control needs no channel correspondence.
4. **Ownership** (28.5): the control is not owned by another endpoint
   or by playback.

Among the eligible controls, as on playback and as A6 decides: **volume**
on the eligible unit nearest the Output Terminal with a read/write
master volume; failing that, the nearest eligible unit with volume
read/write on every logical channel (at least the node's channels, at
most `UAC_VOL_CHANNELS`). **Mute** likewise, on its own (it may be
another unit). A master wins over channels (`Pick`; 5.3 corrected).
Nothing eligible: software volume and mute on the captured samples
(28.11), with `UacVolSoftRange` (-96 to 0 dB, 0.5 dB steps).

Examples: X4: no unit after Selector Unit 26, so each source's control
is its own unit (microphone Feature Unit 2, digital 23, line 25, S/PDIF
8; master mute and volume each). Logi: Feature Unit 2. Play! 2: Feature
Unit 6 through the one-input Selector Unit 10.

**Other controls of the controlled unit** (its channel volumes and
mutes beside a controlled master): on capture they are *not* normalised
to 0 dB once the level is owned, unlike playback's extras: D1 keeps a
volume between minimum and top as the device has it, and normalising
the extras would undo that. They get the first-arrival pass (28.6) and
otherwise keep the device's setting; the software gain accounts for
them as read.

**No cap on capture (A2).** The capture nodes offer the device's whole
range (`KsMax` = `KsTop`); `Uac2VolAboveZero` stays playback-only (its
INF comment and the user documentation say so). Only the first-arrival
pass stops at 0 dB. A range that claims large gain is offered as read.

**Accounting.** The software gain on a source accounts the selected
graph's controls it knows: on the level's own route the controlled and
the other controls (CUR as read; unknown as the range's top), and an
opaque unit as unity, an assumption stated in the diagnostics; branch
controls behind a join are not accounted (they set a part's level, not
the whole). So "never louder than asked" holds for what the driver
controls and knows, across Pass entities only.

### 28.5 Ownership per hardware control, adapter-wide

A **hardware control** is (entity, control selector, channel): a
Feature Unit's mute or volume on its master or one logical channel, or
a Selector Unit's selector control. The adapter (pair 0, which owns
the function) keeps one table of claims for every hardware control, so
two endpoints never write one control.

**Built once, before any write.** The pairs are known before any of
them starts: `Pairs()` reads them from the descriptor, and PnP start
creates and prepares the pairs one after another in one thread (pair 0
first, `Start` and `StartPair`). Pair 0 builds the whole table in its
`Prepare`, after the descriptor model and before its own first control
request that writes (its `LevelSetup` reads ranges and CURs, writes
nothing before the table exists), for every endpoint of every pair
(`UacPlanEndpoints` for each direction and index), in this fixed order,
each entry only on a control nobody holds yet:

1. Each playback pair's level controls (volume and mute, master and
   extras: today's `UnitTaken` made per control), pair by pair.
2. Each capture pair's chooser and inner selectors (28.7), then its
   sources' level controls (28.4), pair by pair, source by source.
3. Each playback pair's normalised controls (28.9), pair by pair.
4. **Reservations** for the first-arrival pass: each capture pair's
   pass targets (28.6) still unheld, reserved for the first pair (and
   within it the first source) whose graph holds them. A reservation
   lets only its holder's pass write the control; it is not a level.

The table is read-only after that until the next start (a stop and
start, a removal and arrival build it again from scratch): later pairs
only look up, so no two endpoints can race to claim. A control held by
another endpoint (claim or reservation) is never written by this one,
by level, normalisation or first-arrival pass alike. It is per control,
not per unit: a unit whose mute is pair A's level control may have its
channel volumes normalised by pair A, claimed by another endpoint for
its own use, or left; each control has at most one holder.

**Invariants this rests on, to keep if preparation is refactored.**
The order above is this driver's own control flow, not a Plug and Play
guarantee (Windows may run starts and restarts asynchronously on Vista
and later; the PnP start contract promises no order between one
device's internal objects, which are this driver's): `Start` creates
pair 0, calls its `Prepare`, registers pair 0's subdevices
(`StartPair`), and only then creates, prepares and registers each later
pair, one after another in the same call. So:

1. **Pair 0 builds the whole table inside its `Prepare`, before
   `StartPair(0)`**: once pair 0's subdevices are registered, KS
   requests (a level SET, a MUX SET) can reach pair 0 while the later
   pairs are still being prepared, and the table they consult is
   already complete and immutable.
2. **A later pair's `Prepare` only looks up**; it never adds a claim, so
   its running beside pair 0's KS traffic cannot change a holder.
3. **A return to D0 does not prepare again**: `PowerChangeState` calls
   `PowerLocal`, which marks the device state unknown and leaves the
   table, the claims and the route table as they are; the state is read
   again at the next apply (28.8).
4. **A stop and start creates new objects**: `Start` runs
   `ShutdownPairs` (every old pair drained, its level work and pended
   requests run down, 28.7) and then constructs new `Wave` objects, so a
   table is never rebuilt under live objects; it dies with pair 0.

A refactoring that prepares pairs concurrently, re-prepares at D0, or
reuses objects across a stop must rebuild this guarantee first (a table
built once before any pair registers), and the claims tests (28.16)
cover the order.

**What software cannot do.** Where a source's level control is claimed
elsewhere, or a selected-graph control another endpoint holds muted or
attenuated, this endpoint's software gain can only attenuate further:
it cannot restore gain or unmute hardware another endpoint attenuated
or muted. The diagnostics record such a control (28.12); the user
documentation says that on a device whose recording and playback share
a unit, the playback control also affects the recording.

### 28.6 The first arrival (D1, A3, A4)

**How "first" is known.** Per source, by the endpoint's saved capture
value (28.12): no value, or a value with that source's "pass done" flag
clear, means that source has never had its pass. The value belongs to
the devnode's driver key: the same device replugged into the same port
(or any port, for a device with a serial number) is not a first arrival;
the same model on a new port without a serial number is a new devnode
and gets the pass again, as it gets a new driver key. A device installed
before this change has no value, so its next start is a first arrival.
A malformed or stale value is treated as absent (28.12). Deleting the
value runs every source's pass again at the next start (documented as
the reset).

**When.** The pass runs on the selected source at start, and on any
other source the first time it becomes the confirmed selected source
(A3; 28.7). Never on a source whose route is unknown: an unknown
selector position (28.7) never triggers a pass on an assumed input; the
pass waits until a read confirms which source is selected.
`Uac2CapFirst` = 0 (A4) skips the pass for good: the capture controls
stay as the device has them until the user sets one.

**What is written**, on the source's graph (every route, so every
branch of a join too), each control only where 28.5 allows it:

1. Every read/write mute, master and each logical channel: set to 0.
2. Every read/write volume, master and each logical channel, whose CUR
   reads the silence value (`0x8000`) or at or below the first level of
   its RANGE (a CUR below MIN counts): set to the top step at or below
   0 dB of that control's own RANGE (`UacVolRange`'s `KsMax`, whatever
   `Uac2VolAboveZero` says). Where no step is at or below 0 dB the
   control is left. A RANGE is read as `UacVolRange` reads it today: a
   RES of 0 on a subrange wider than one value reads as 1 (1/256 dB);
   a one-value subrange keeps RES 0; a MIN of silence reads as its next
   step; a malformed answer (MIN above MAX, subranges out of order, a
   short answer) means no range, and that volume is left.
3. Order: volumes raised first, then mutes cleared, from the Input
   Terminal side to the Output Terminal side.
4. **Readback** after every write: GET CUR of the control. A write the
   device stalls is a failure; a write that succeeds but whose readback
   still shows the old value (an ignored SET) is a failure too, counted
   apart (`Ignored`); a readback that cannot be read leaves the control
   unknown.

**What the pass never writes**: a volume between its minimum and its
top; a volume above 0 dB (left, not lowered: A2); a volume whose CUR or
RANGE could not be read; any control other than mute and volume (AGC,
bass, the rest); any Selector Unit (the pass never chooses an input);
any Mixer crosspoint; any control outside the source's graph (the
monitoring branches into a playback Mixer Unit, other sources' units,
the playback path); any control 28.5 gives to another endpoint.

**One transaction, and the user wins.** The pass runs under the
operation mutex, which the level work item, RUN and start also take; a
KS SET is not under it (SETs land in the cached state under the level
spin lock and set `Pending`). So the pass works on a generation: it
reads the endpoint's change counter (`Change`, incremented by every
SET) when it begins; at its end, under the spin lock, it makes its
results the source's saved level only if the counter has not moved. If
a SET came during the pass, the SET's values stand: the pass's writes
to that control are superseded by the apply that `Pending` runs next,
and the saved value is written from that apply's state, never from the
pass's. The same check guards the registry save of the pass.

**After the pass**, three things are kept apart in the state and in the
saved value:

- **Pass done** (per source, saved): set once the pass ran with the
  device ready, whatever each write's outcome, so a control that stalls
  is not asked again at every start.
- **Owned** (per level control): the source's volume and mute controls
  (28.4) become owned with the readback value as their level where the
  readback confirms a value (written or left), and are saved; a level
  control whose write failed or whose state is unknown is not owned
  (the device's own state stands, read again at the next apply). From
  then on an owned control is written at every start, after D3 and when
  stale, as a restored playback level is.
- **Pending retry** (per owned control, not saved): an owned control
  whose later write fails stays dirty and is retried at the next apply
  (a RUN, a SET, a return to D0), as on playback today. Controls the
  pass wrote that are not level controls are never retried (one shot).

### 28.7 The input chooser (D3)

**The chooser.** The first Choice entity (a Selector Unit with two or
more inputs) the walk meets from the capture Output Terminal without
crossing a Join. It becomes the topology's MUX node (28.10). A selector
behind a Join is not a chooser: it is followed at its read position,
never set, and its branch is part of one source. A chooser left with
one source (the other inputs reach nothing) is none.

**Selectors in series.** A further Choice inside a chooser branch, met
before any Join, is flattened into the chooser: each source is then a
route of (selector, input) pairs. The inner selector may be shared by
several routes (input 1 of the chooser reaching inner selector X, whose
inputs are two sources). Sources are numbered in route order: the
chooser's inputs in order, and within one input the inner selectors'
inputs in order; that order is stable for a given descriptor and is the
one the names (28.10) and the saved value (28.12) use.

**Limit (B3).** At most 8 sources: a device with more gets no MUX node and keeps one line at the selectors' read
positions (software level if nothing eligible covers every source),
recorded. A chooser shared by two capture endpoints belongs to the first
(28.5); the second records at its read position with no MUX, and the
route lock below keeps the first's MUX from moving it while the second
records.

**KS property.** `KSPROPERTY_AUDIO_MUX_SOURCE` (`KSPROPSETID_Audio`,
`KSNODEPROPERTY`, a `ULONG`), GET and SET on the MUX node; the value is
the MUX node's logical input pin, 1 to N in source order (published kit
documentation: "the pin ID of the selected input pin on the MUX node";
a node's logical pins as in `PCCONNECTION_DESCRIPTOR`, output 0, inputs
from 1; distinct from the mixer API's line indexes). A pin outside 1..N
is refused (`STATUS_INVALID_PARAMETER`). `KSPROPERTY_AUDIO_CPU_RESOURCES`
answers `KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU` on the MUX node.

**The requests.** UAC 2.0: Selector Unit Selector Control
(`SU_SELECTOR_CONTROL` 0x01; ADC-2 5.2.5.6.1, A.17.6), CUR request,
`wValue` 0x0100, `wIndex` unit << 8 | AudioControl interface, one byte,
one-based; writable only where `bmControls` D1..0 read 11 (01: read
only, a SET is refused with `STATUS_INVALID_DEVICE_REQUEST`; 00: no
request, the position unknown unless the selector is on no other route).
UAC 1.0: 28.13.

**Three route states.** Kept apart in the state and the diagnostics:

- **Requested**: the source of the SET being applied, or the saved
  source being restored at start (B1); none otherwise.
- **Switching**: a requested switch not yet confirmed or failed.
- **Confirmed**: the source whose every selector read back at its input
  at the last read.
- **Unknown**: no source matches the positions read (a selector stalled,
  answered outside 1..`bNrInPins`, has no readable control, or an inner
  selector shared by routes reads an input of no offered route).

GET answers the confirmed source, also while a switch is in progress
(it reports what the device was last read to hold, never a request);
when unknown, it answers 1 and the diagnostics say the route is
unknown. While unknown, no
first-arrival pass runs, no per-source unit is written, shared controls
keep what they hold, and the software gain takes the quietest of the
sources' states, so an unknown route is never louder than any source
asked.

**Synchronisation contract: a SET completes when the switch is
known.** No MUX SET is answered `STATUS_SUCCESS` before the readback
confirms the new source.

- At PASSIVE the switch runs synchronously and the SET returns its
  outcome: `STATUS_SUCCESS` when the readback confirms the new source,
  `STATUS_DEVICE_DATA_ERROR` when a selector write failed or the
  readback does not confirm.
- At raised IRQL (PortCls may call there; today's level SETs allow for
  it) the handler cannot wait. **Decision for now: no asynchronous
  path.** Such a SET is refused at once with `STATUS_INVALID_DEVICE_STATE`
  (counted, `MuxRefusedIrql`; the IRQL recorded, `MuxIrql`), nothing
  written, the confirmed source kept; see "Cancellation" below for why,
  and for what would let the pended path below be enabled. Microsoft's
  published PortCls documentation does describe pending property
  completion for Windows 98/ME and for Windows 2000 and later
  (`PcCompletePendingPropertyRequest`; `PCPROPERTY_ITEM`: "The Handler
  routine can also return STATUS_PENDING, in which case the miniport
  driver is obliged to call PcCompletePendingPropertyRequest at a later
  time"), and says the miniport must not touch the request after that
  call; so the lifetime of a pended request is covered on every target
  system, and only its cancellation is not.

**The pended path, specified for when it is enabled: one protocol for
admission, rundown and publication.** Each endpoint has one switch
record, every field of which is read and written only under the
endpoint's switch spin lock (`SwitchLock`, non-paged, never held across
a wait, a USB request or a completion call):
`State` (Idle, Sync, Pended, Running), `Request`, `Source`, `Irql`,
`Epoch`, and the rundown count `Work` and flag `Closed` of the level
work (28.15's work item, shared with the level SETs).

**Lock order.** `SwitchLock` is taken either with no driver lock held
(admission, the taking of the work, completion, rundown) or with the
endpoint's operation mutex held (the epoch check of "Running" below and
`PowerLocal`'s change of the epoch), never with another spin lock held;
the route lock (below) may be taken inside it, never the other way. So
the one order is: the operation mutex, then `SwitchLock`, then
`RouteLock`. Nothing waits for the operation mutex while holding either
spin lock (a wait at DISPATCH is not allowed anyway), and the level spin
lock is never held together with either of them. (An earlier wording,
"taken under nothing", did not allow the epoch check under the
operation mutex that the power rule needs; this order replaces it.)

- **Admission** (the handler, at any IRQL up to DISPATCH), in one hold
  of `SwitchLock`: if `Closed`, answer `STATUS_DEVICE_NOT_READY` (not
  pended, nothing published); if `State` is not Idle, answer
  `STATUS_DEVICE_BUSY`; otherwise write `Request`, `Source`, `Irql` and
  `Epoch` (the endpoint's power epoch, below), then `State` = Pended,
  then take one `Work` reference and, if the work item is not queued,
  mark it queued. Release the lock; queue the work item if this hold
  marked it; return `STATUS_PENDING`. A request is never visible in the
  record without its `Work` reference, and never counted without being
  fully published, because both happen in the hold that checked
  `Closed`.
- **Taking the work** (the work item, at PASSIVE): in one hold of
  `SwitchLock`, if `State` is Pended, copy `Request`, `Source`, `Irql`
  and `Epoch` into the work item's own locals and set `State` = Running
  (the completion's diagnostics take `MuxIrql` from those locals, never
  from the record, which a new SET may have filled by then). A worker
  that is already running and loops (the coalescing of level SETs)
  takes the switch the same way, so it sees Idle or a fully published
  Pended record, never a half-written one. The worker's exit test
  (nothing Pended, no level SET pending) and its clearing of the
  queued mark are in one hold of the same lock, so an admission either
  finds the item still queued and running (and is taken by its loop) or
  finds it not queued and queues it.
- **The worker's own reference.** The hold that marks the work item
  queued (an admission's, or a level SET's in `Changed`) takes, besides
  any request's reference, one `Work` reference for the worker's
  execution, and the endpoint and device object are referenced before
  `IoQueueWorkItem` as `Changed` does today. A request's reference is
  dropped after that request's completion (below); the execution's is
  kept through the whole final loop (the exit test, the clearing of the
  queued mark and every access to the endpoint after them) and dropped
  as the worker's last access to the endpoint, after which it touches
  only its locals (the release of the endpoint and device object
  references, as `LevelWork` does). So `Work` reaching 0 when a
  request completes cannot let `Shutdown` finish while the worker is
  still looping over the endpoint's state.
- **Running** (the worker, at PASSIVE): the worker first waits for the
  operation mutex (the time-out of "What bounds the wait": then
  `STATUS_DEVICE_BUSY`, nothing written). Then, **holding it and before
  any hardware work**, in one hold of `SwitchLock` it reads `Closed` and
  the current power epoch: if `Closed` is set, the copied `Epoch` is not
  the current one, or the device is not ready (`DeviceReady` false), no
  hardware work is done and the outcome is `STATUS_DEVICE_REMOVED`
  (removal), `STATUS_CANCELLED` (stop) or `STATUS_DEVICE_NOT_READY` (a
  power cycle came between, or the device is down); otherwise the
  switch runs (28.7, "What a switch does") in that same hold of the
  operation mutex and its outcome is its readback's. `PowerLocal`
  changes the epoch only while it holds the operation mutex (below), so
  the check holds for as long as the worker holds the mutex: no power
  transition comes between the check and the end of the hardware work.
  A check made before the wait would not do: the worker can wait for
  the mutex across a whole D3 and D0, after which the device is ready
  again.
- **Completion**: in one hold of `SwitchLock`, the diagnostics
  (`MuxStatus`, `MuxIrql`, counters) are written from the worker's
  locals, `Request` is cleared and `State` = Idle; the lock is released;
  then `PcCompletePendingPropertyRequest(local request, local status)`
  is called with no driver lock held; then, after that call has
  returned, the worker drops the request's `Work` reference. So the
  record can take a new SET as soon as it is Idle, while the old
  completion uses only the worker's locals and never the record; and
  `Work` reaching 0 means every completion has finished, not only been
  claimed. The worker is the only completer.
- **Rundown** (`Shutdown`, at PASSIVE, stop and removal): in one hold of
  `SwitchLock` set `Closed`; release; wait for `Work` to reach 0. No
  admission can follow (it sees `Closed`), every admitted request holds
  a reference, and the worker completes each one (with the outcomes
  above); `Shutdown` never completes a request itself. The wait has the
  bounds and the limits of "What bounds the wait" below, as today's
  wait for a level work item has.
- **Power**: `PowerLocal` increments the endpoint's power epoch at
  every transition it makes, leaving D0 and returning to D0, while it
  holds the endpoint's operation mutex (as it does today for the rest of
  its work), in one hold of `SwitchLock`. A switch admitted before a
  power-down, or while the device is down, therefore never matches the
  epoch after the return to D0 and runs no hardware work, whenever the
  worker reaches it: it completes `STATUS_DEVICE_NOT_READY`, the
  requested source dropped, the confirmed one kept and read again at D0
  (28.8). A switch that passed its check holds the operation mutex, so
  `PowerLocal` waits for that switch to end before it changes the epoch
  and `powerBlocked`.
- **PASSIVE switches** use the same record: `State` = Sync from
  admission to the end, under the same lock rules, with no `Request`.
  Admission copies the epoch as a pended one does; the check is the
  same as "Running"'s, made after the operation mutex is acquired and
  before any hardware work, in one hold of `SwitchLock`: a power cycle
  while the caller waited for the mutex fails the SET
  `STATUS_DEVICE_NOT_READY` with nothing written.

**Cancellation.** PortCls documents no cancel callback for a property
request a miniport has pended, and **no adequate supported cancellation
protocol has been established** for one. That is not shown to be
impossible: the request exposes its IRP (`PCPROPERTY_REQUEST`'s `Irp`
member, in both the 2000 DDK's and the WDK 7.1's `portcls.h`), and an
IRP's `Cancel` flag is documented cancellation state, so a worker could
observe that a cancellation was asked. But that gives no callback
(nothing runs when the cancellation is asked), it does not authorise the
miniport to set or replace the cancel routine of an IRP the port owns,
and it does not make a worker that is not yet scheduled, or that is
blocked in a transfer, respond. Microsoft's cancellation guidance
("Canceling IRPs") asks a driver that holds a request for an indefinite
time to make it cancelable for that time; a check of the flag once when
the worker starts does not do that. A pended switch can wait for an
indefinite time before its hardware work starts (the system's work
queue, a lower stack that does not complete a cancelled transfer:
below). So the pended path stays disabled (raised-IRQL MUX SETs
refused, above) until an adequate arrangement is established from
Microsoft's documentation; finishing a hardware transaction already
started would remain acceptable then. The synchronous PASSIVE path
holds the caller's own thread, with the waits below; it queues
nothing (and is no more responsive to a stuck lower transfer than the
bounds below say).

The refusal at raised IRQL is **provisional and accepted (owner,
2026-11-17)**; it changes what a user can see: on a system whose mixer or audio
engine sets the MUX at raised IRQL, the source cannot be changed there
at all until a supported completion arrangement exists. **Which systems
send the MUX SET at raised IRQL is unknown until measured**: neither
the level handlers' present allowance for raised IRQL nor
`PcCompletePendingPropertyRequest`'s `<= DISPATCH_LEVEL` says at which
IRQL any system's mixer sets a MUX, and no system (98 SE, ME, 2000, XP,
Vista, 7) is labelled either way before its handler has been observed
there (`MuxIrql`, `MuxRefusedIrql`). The recommendation recorded for
the owner's decision: accept the refusal provisionally for development,
with explicit failure and no deferred write (`STATUS_INVALID_DEVICE_STATE`,
nothing written and nothing kept to be written later); measure the
MUX SET's IRQL early, on every target system, in the first legs that
expose the MUX; and where a normal system panel consistently sends it at
raised IRQL, treat input selection as unsupported on that system and
claim no readiness there until a supported completion arrangement
exists. If every system calls at PASSIVE, the refusal is never seen.

**What bounds the wait, and what does not.** Two different bounds are
kept apart: a **count** of transfers (fixed by the code), and **time**
(bounded only where every transfer ends).

- **One transfer**: `CallAndDrain` waits 5 s, then cancels the IRP and
  waits without limit for the lower stack to complete it. So one
  transfer is bounded in time only on a lower stack that completes a
  cancelled IRP; on one that does not, it is not bounded at all.
- **The device lock** (`UacUsbClassRequest`'s mutex, shared by every
  pair) is not held for one transfer only: `UacUsbRates` holds it
  across the whole rate walk of a start (each clock entity's RANGE, two
  passes for stepped ranges), and `SetClock` across a stream start's
  clock requests (clock selector reads and writes, SAM_FREQ SET and
  readback, CLOCK_VALID). A switch waiting for it waits for that whole
  sequence: a count of transfers the walk and the clock path fix, and
  in time that count times one transfer's bound.
- **The operation mutex and the switch record**: taken with a time-out
  (provisional: 2 s), then `STATUS_DEVICE_BUSY`, nothing written,
  counted (`MuxTimeouts`).
- **A level apply** holding the operation mutex before the switch:
  `ReadState` reads every controlled slot not known before it writes
  (up to 9 volume and 9 mute GET CURs), then up to 9 volume and 9 mute
  SET CURs, so up to 36 transfers, plus at a start the RANGE reads (up to
  9, each tried twice) and the start's lowering writes.
- **The switch itself**: at most 8 selector writes and 8 readbacks, the
  new source's level reads, writes and readbacks, and a first-arrival
  pass (28.6: a read, a write and a readback per control of the
  source's graph): a fixed count.
- **The system's delayed work queue** (the pended path only): no bound
  from the driver.

So the synchronous path's time is bounded by a fixed transfer count only
on a lower stack that completes cancelled transfers, and its lock waits
by the 2 s time-outs; nothing else is claimed.

**The route lock: every open recording is protected, adapter-wide,
on every system.** A recording depends on every hardware control
(28.5: a Feature Unit's mute or volume on one channel, a selector) of
its endpoint's selected graph (28.3): its own chooser and inner
selectors, selectors behind a join at their read position, a selector
another endpoint holds (a chooser shared by two capture endpoints
belongs to the first, 28.5, and the second records through it at its
read position), and every Feature Unit control on its routes, also on a
route with no selector at all. Claims (28.5) keep two endpoints from
writing one control; they do not protect a reader. So the adapter
(pair 0) keeps, beside the claims, a **route table** with one entry per
hardware control of every capture graph:

- `Readers`: the open capture streams, of any pair, whose locked graph
  holds the control (a count, and the endpoints, at most 8);
- `Writer`: the endpoint whose own write of the control is in progress,
  or none, with its **kind**: `Switch` (a selector switch's
  reservation: the selectors it moves and the level writes it makes as
  part of the switch, held from its check until the switch has ended)
  or `Level` (an automatic level write of the endpoint's own, below,
  which moves no selector);
- for a selector, `Position` (the last confirmed one, 0 unknown) and the
  table's `Version`, increased by every change of any `Position`.

One spin lock guards the table (`RouteLock`, pair 0's, non-paged); it is
held only to check and change the table, never across a wait or a USB
request, and it is the innermost lock (taken under the operation mutex,
under `SwitchLock`, or under none; never the other way).

- **Selector state is the adapter's, not an endpoint's.** Every read or
  readback of a selector, by any pair, publishes its `Position` in the
  table under `RouteLock` (and increases `Version` when it changed);
  every endpoint computes its selected graph from the table's positions,
  never from a copy of its own. So when pair A moves a shared selector,
  pair B's next look at its route is the route A left, inner selectors
  included.
- **A capture stream's open** (`NewStream` for the capture pin, before
  the operation mutex and before `UacStream::Create`): in one hold of
  `RouteLock` it computes its selected graph from the table (every
  route of its graph when a position is unknown) and checks it. It
  must wait if any control of that graph has a `Switch` writer, **of
  any endpoint, its own included**, or a `Level` writer of another
  endpoint; then it releases the lock and waits (PASSIVE; an event every
  `Writer` clearing sets) up to the time-out above, then fails
  `STATUS_DEVICE_BUSY`. Otherwise, in the same hold, it adds itself to
  `Readers` of every control of that graph and keeps that set, with the
  `Version` it was computed at, as the stream's locked graph. The
  computation, the check and the registration are always one hold: a
  wait ends by starting again with a new hold that computes the graph
  from the table as the switch left it, so the open never registers a
  set computed before it waited, and locks the route the switch actually
  left (the new source's, or every route where the switch ended
  unknown). The endpoint's own `Level` writes (a restored level, a pass
  or a D0 rewrite on its confirmed source) are the one permitted
  activity an open does not wait for: they move no selector, so the
  graph it computes stays the graph it records from. Its own `Switch`
  reservation is not permitted: without that wait, an open that came
  while its own endpoint's switch was moving selectors would register
  `Readers` on the old graph, the switch (whose check came before those
  `Readers`) would finish, and the stream would record the new route
  while its locked set protected the old one. Every switch that changes
  an endpoint's selected graph reserves at least one selector of that
  graph as it stands (the chooser, or an inner selector on the
  chooser's present input) before its first write, so this check on the
  computed graph sees every such switch. A create that then fails gives
  the set back at once; a stream's close (`Close`, after `Drain`) gives
  it back. A pause keeps it.
- **A switch** (a MUX SET, the restore at start, any selector write): in
  one hold of `RouteLock`, for every selector it would set to a position
  other than the table's, if another endpoint is in its `Readers` (or
  this endpoint is: its own stream open, or opening, since an open
  registers before its create) or another `Writer` is set, the switch is
  refused `STATUS_DEVICE_BUSY` at once (counted, `MuxRefusedLocked`),
  nothing written; otherwise it sets itself as `Writer` of kind `Switch`
  on each of them in the same hold, releases the lock, does its
  hardware work under the operation mutex, and in one hold publishes
  every position it read back and clears its `Writer` marks (setting the
  event) when the switch has ended, confirmed or not. A SET to the
  confirmed source moves nothing and succeeds at once.
- **Automatic writes** (writes the driver makes on its own, not a
  user's SET of that very control: a switch's level writes, the restore
  of a saved level at start, a first-arrival pass, a playback
  normalisation; 28.6, 28.9): before each such write, in one hold of
  `RouteLock`, the writer checks that no other endpoint is in the
  control's `Readers` and no other `Writer` is set, and sets itself as
  its `Writer`, of kind `Level` (of kind `Switch` for a switch's own
  level writes, which belong to its reservation); it releases the lock,
  writes and reads back, and clears
  `Writer` in one hold. A control another endpoint reads is skipped: a
  level control stays dirty and is written at the first apply after
  that reader closes, the software gain meanwhile taking the device's
  state as read; a pass leaves that control for later with its source's
  "pass done" clear, so the pass runs again when nothing reads it. An
  open of another endpoint that meets such a `Writer` waits for it, as
  for a switch; an open of the writer's own endpoint does not (above). This
  holds on routes with no selector too, since the table holds every
  control. A user's SET of a control another endpoint's recording also
  passes through is applied: that is the shared-unit case of 28.5's
  "What software cannot do", documented.
- So stream open, selection and automatic writes are arbitrated
  atomically across every pair and within one endpoint: either the open
  sees the writer (and waits for its end, then locks the graph the writer
  left; a switch of its own endpoint included), or the
  writer sees the open (and is refused or skips); neither can slip
  between the other's check and its mark, and a graph is always
  computed and locked in one hold of the lock that publishes positions.

- **On every system.** On 98 SE to XP as on Vista and later, while any
  recording that depends on a selector is open, no MUX SET moves that
  selector: several capture pairs can record at once (several waveIn
  devices, section 15.3), several programs can drive one mixer, and a
  system may give one recording device to more than one client, so a
  SET while recording is not assumed to come from the person who is
  recording. A user who picks another source in a recording panel while
  any program records from that device's route is refused (the panel's
  call returns an error; what each panel then shows is read in the legs,
  28.16), and picks again once the recording is closed: the SET returns
  failure and the device keeps, and GET reports, the actual source.
  **Decided as provisional by the owner, 2026-11-17** (C1, 28.2): to be
  confirmed or changed after the mixer legs on each system (28.16,
  "Rapid and concurrent selection" and "Cross-pair route lock"), which
  settle it; the user documentation says so.
- On Vista and later this also covers the per-source endpoints of one
  pair: if the audio system sets the MUX for endpoint B before it opens
  B's pin while A records, that SET is refused, so A's route is never
  switched by an open that then fails busy; in shared mode, where the
  engine keeps A's pin open and B's client would need another pin
  (refused, the one-instance pin and the gate), B's MUX change is
  refused first.
- A device-side change found while a route is locked (28.8) is taken as
  found: the device moved its own selector; the locked set is not
  changed by it (the stream keeps its readers' counts until it closes).

**Integration contracts of the route table** (the host modules
`uaccap.c` and `uacroute.c` state them too):

- **Exhaustion disables, never weakens.** The table and each endpoint's
  routes are bounded (`uacroute.h`). When a control cannot be entered
  (`UacRouteControl` answers -1) or an endpoint's routes do not fit
  (`UacRouteFromGraph` answers 0, leaving that endpoint with no route),
  the result is never ignored: an endpoint with no route would register
  an empty set at its open and record unprotected. So any such failure,
  for any endpoint, disables every selector switch and every automatic
  capture write on the adapter until the next start (refused or skipped,
  counted, recorded in the diagnostics); recording and the user's own
  level SETs go on.
- **One registered stream per endpoint.** `Readers` keeps one bit per
  endpoint, so a close clears that endpoint's bit whatever else it
  holds: correct while the capture pin is one-instance (today's rule). A
  second simultaneous stream of one endpoint needs a per-endpoint count
  before it is allowed.
- **Storage and locking.** The route table (about 64 KiB at the default
  bounds) and each capture graph are allocated once from nonpaged pool
  at PnP start, at PASSIVE, before any spin lock is taken, and are never
  kernel-stack locals. Everything `RouteLock` guards (the table, each
  stream's locked set) and every function called under it is nonpaged.
  Discovery and the claims build run at PASSIVE, in `Prepare`.

**What a switch does**, in order, under the operation mutex:

1. The software gain for the quieter of the old and new state, per
   channel, published.
2. The new source's per-source controls and the shared controls
   lowered where its level is lower than they hold (a per-source unit
   not yet selected is inaudible, so writing it first is safe).
3. The new route's selectors, the innermost first and the chooser last,
   each read back.
4. If every selector confirmed: the raisings and mute releases of the
   new source's level; its first-arrival pass if not done (28.6); the
   software gain for the new state; the saved value.

**Partial failure** (selectors in series): at the first selector write
that fails or does not read back, the switch stops; no later selector
is written and nothing is retried on its own. Every selector of the old
and the new route is read again: if the old route still reads whole
(the failed or changed selector is not on it), the old source stays
confirmed and the requested source is dropped; if a shared inner
selector moved (the old route went through it) or a read fails, the
state is unknown as above. The user's next SET tries again.

**Restoring the saved input at start (B1).** At start, after the
selectors are read: if the saved value is valid and current (28.12: its
descriptor hash and the saved source's route and end terminal match the
descriptors) and names a source the user chose (`Selected` not 0), and
every selector on that route is writable, the driver switches to it as
a switch above (requested, the writes innermost first, readback). A
confirmed restore leaves that source selected; a failed one follows the
partial-failure rule, the device's position read is what stands, and
the failure is recorded. An invalid or stale value restores nothing.
The restore at start is the one time the driver moves a selector on its
own, and only to a source the user chose; otherwise selectors move only
on a SET.

### 28.8 What is refreshed, and what is not

**Reads** of every selector on every route: at start; at each capture
RUN, before the stream starts; after a return to D0; after a failed
selector write; and on a MUX GET at PASSIVE, at most once a second
(provisional: a bounded refresh, so an opening or refreshing mixer panel
sees the device; a GET at raised IRQL, or within the second, answers
the cached state). A device-side change found by a read is taken like a
user's switch: the MUX reports the new source and its level applies
(28.7 steps 1 to 4, without step 3's writes).

**What this gives: eventual discovery, not tracking.** The AudioControl
interrupt endpoint that would report a change (ADC-2 6.1; the X4 has
one, `0x84`) is not served (section 5.3; the feature ledger's "M"), and
the driver does not poll during a recording. So, accepted as a
limitation (B6):

- A device-side switch **during a recording** (a button, a jack): the
  recording goes on with the new input's audio at the old source's
  software gain and with the new source's units as the device holds
  them, until the next RUN or the next MUX GET at PASSIVE.
- An open mixer panel shows the old source until it reads again, and no
  `KSEVENT_CONTROL_CHANGE` tells it to (not supported, 5.3); a panel
  that never reads again stays stale.
- Serving the interrupt endpoint is the proper fix; it is not in this
  work.

### 28.9 The other playback units (D2) and its bounds

**When "owns the level" begins**: as today for the extra channels,
separately for volume and mute: the volume from the first KS SET of the
render VOLUME node or the restore of a saved `Uac2Level`/`Uac2LevelPnn`
with its volume owned; the mute likewise.

**Which controls.** Every hardware control (28.5) on the playback path
`UacVolFind` takes (the USB streaming Input Terminal to the first
non-streaming Output Terminal) that is not a level control and not one
of the level unit's extras: other units whole, and a level unit's
controls of the other kind (a mute-carrying level unit's channel
volumes, say). The extras (`VolumeExtra`, `MuteExtra`: the same unit's
channel controls beside a controlled master) keep their shipped rule
unchanged (B4): set to `UacVolFromKs(0, range)` and unmuted once the
master is owned, lowered at start when above the top offered (section
20.3), `NoRange` extras read and not written. Never: a control on a branch that
joins the path from a non-streaming Input Terminal (a sidetone or
monitoring branch: the Logi's Feature Unit 11, the X4's 11, 12 and 13,
the Play! 2's microphone branch through Selector Unit 12); a control on
another Output Terminal's path after a split; a control another
endpoint claims (28.5); a capture-walk control.

**The bounded scope (B4).** One zone gets volumes set; everywhere else
mutes are released only:

- **Z1** exists only when the playback route has a controlled hardware
  volume (`VolumeMode` not software) and the route's segment from the
  USB streaming Input Terminal to the controlled volume unit contains no
  Join and no opaque entity (28.3) at all. Then Z1 is every unit on that
  segment other than the controlled one. If the segment contains a Join
  or an opaque entity, wherever on it, Z1 is empty. So no mix and no
  unknown processing lies between a Z1 unit and the control that takes
  over its attenuation, and a Z1 unit carries only the playback stream
  (a unit after a join is on a segment with a Join, so never Z1).
- **Everything else on the route** (after a join, between a join and
  the controlled volume, behind or across an opaque entity, nearer the
  Output Terminal than the controlled volume, or any unit when the
  volume is in software): mutes released only; volumes left as the
  device holds them, one at silence or at its minimum reported in the
  diagnostics, not raised.
- **Precedence**: the rule that writes less wins. The playback walk
  (28.3's, from the Output Terminal `UacVolFind` picks back to the USB
  streaming Input Terminal) may find a unit on more than one route (a
  diamond): it is Z1 only if it passes Z1's test on every route that
  holds it; a control held by another endpoint gets nothing; and a
  control on a monitoring branch (below) gets **no write at all**,
  neither a volume nor a mute release, whatever zone or rule the
  playback route alone would give it, on any route of a diamond: the
  exclusion in "Which controls" is absolute, and here "the rule that
  writes less" means no write. Releasing such a mute would unmute the
  sidetone, which is not the playback's to change. The control is listed
  in the diagnostics (`OtherCount`, with neither `OtherZ1` nor
  `OtherDone` set) and left as the device holds it.
- **Monitoring branch**, exactly: the playback walk's routes that end
  at a non-streaming Input Terminal are monitoring routes, those that
  end at the USB streaming Input Terminal playback routes. A unit on a
  monitoring route upstream of the join where that route meets a
  playback route (so a unit that carries the monitoring signal without
  the playback: the Logi's Feature Unit 11, the X4's 11, 12 and 13) is on
  a monitoring branch. A unit after such a join, on both kinds of route,
  carries the playback and is a playback-route unit "after a join"
  (mutes released only, as above); the walk gives it that rule only
  when it is on no route as a branch unit, so a descriptor in which one
  unit is both (through a diamond) gets no write on it.

**Why bounded** (the owner's B4; the reasons recorded):

- Raising a unit after a join raises the sidetone with the playback,
  and the playback's software gain cannot compensate a signal it does
  not carry.
- Raising a unit while the controlled unit takes its attenuation puts
  unity into any mix between them: a full-scale playback plus a sidetone
  can clip in the device's mixer. Z1's segment rule excludes any mix
  between.
- A unit nearer the Output Terminal than the controlled volume may be
  an amplifier's output stage: 0 dB is not acoustically safe
  everywhere, and the user's slider would then start from the loudest
  baseline.

**Targets** where a Z1 volume is set: the top step at or below 0 dB of
its own RANGE (0 dB where the range has it), whatever `Uac2VolAboveZero`
says; a range wholly above 0 dB: its minimum (the least gain). This is a
new rule for new controls and differs from the shipped ones in section
20 in two places, both kept as shipped: the *controlled* volume offers a
positive-only range whole (`KsMax` stays `KsTop`, 20.3), and the extras
are set to `UacVolFromKs(0, range)`, which gives the minimum in a
positive-only range too but, with `Uac2VolAboveZero` = 1, the step
nearest 0 dB, which may lie just above it (20.3, "Every level is
clamped to `KsMax`"). A volume without a usable RANGE is not set
(counted, as `NoRange` extras are).

**Order and loudness**, in `ApplyLocked`'s four passes as the extras:
volume lowerings with the main lowerings, raisings after the main
control holds its level, mute releases last; every write read back
(28.6's readback rule). The guarantee, bounded: **the playback signal at
the Output Terminal is never louder than asked, for the controls the
driver knows**, on the playback route through Pass entities; the
software gain takes each other control on that route as part of what
the device does (CUR as read; unknown as its RANGE's top). Not covered:
a monitoring signal after a join (its unit's mute released only, its
level the device's), clipping inside the device's mixer from the
device's own levels, opaque units (taken as unity), and a control the
driver can neither read nor range.

**No silencing by accounting.** Today an extra channel volume with no
RANGE and an unknown CUR silences its channel until read (`Gains`,
+128 dB assumed). For the other units that would silence playback for
good on a device that never answers: so an other-unit volume with no
RANGE and no readable CUR is taken as 0 dB (not counted against the
level, an exception to the guarantee, recorded in `Uac2VolMap`'s
appended words), and its CUR is tried again at each apply; when it is
read, it counts. The existing extra-channel rule is unchanged.

**Units shared by two paths**: two playback pairs per 28.5 (the first
claims; `Pairs()` still refuses several pairs when two paths meet in a
level control); playback and monitoring per the zones above; playback
and capture: a control on both is claimed in 28.5's order (playback's
level first, then capture's chooser and level, then playback's
normalisation), never written by both.

### 28.10 The topology, names and identifiers

**The capture line**, built at start by a builder in `uactopo.c`
(host-tested) instead of the fixed tables:

- No chooser: the capture bridge pin -> VOLUME -> MUTE -> SUM -> the
  capture line out to the wave filter.
- Chooser with N sources (2 to 8): source pins, each -> its VOLUME ->
  its MUTE -> the MUX node's logical input k -> the capture line out.
  The MUX node keeps a node between the pins (the 98 SE rule of
  `uactopo.h`).
- `UacLinksCheck` admits a node input pin above 1 only on a MUX node
  (logical inputs 1..N), with exactly one link into each.

WDMAud's documented parsing supports this graph: a source line runs
from its pin downstream to a SUM or MUX node, and "A MUX node generates
a MUX control in the destination line that contains references to each
of the source lines controlled by the MUX" (published kit
documentation, "WDMAud Topology Parsing"). It does not say how each
system's panels present every control; the legs establish that (28.16).

**Identifiers kept.** The reference strings ("Wave", "Topology",
"Wave2" ...) and the wave filter's pins are unchanged. The topology
filter keeps its pin numbers where they exist today: 0 render in,
1 speaker, 2 the capture bridge, 3 the capture line out; with a chooser
source 1 is pin 2, and sources 2 to N are pins 4 to N + 2. So the pin an
existing endpoint was built on stays, with the first source on it.
Node numbers change (new nodes); nothing outside the driver keeps them.

**Names (A1, A5).** A pin's name is `KSPROPERTY_PIN_NAME`, which WDMAud
uses for the line and Vista and later for the endpoint ("based on the
friendly names of the bridge pins", "Audio Endpoint Builder
Algorithm"). The pin's category and its `Name` GUID are distinct: the
category is the terminal type's `KSNODETYPE_*` (section 15.3 item 4:
`_MICROPHONE`, `_LINE_CONNECTOR`, `_ANALOG_CONNECTOR`,
`_DIGITAL_AUDIO_INTERFACE`, `_SPDIF_INTERFACE`); the `Name` is either
none (Windows names the pin from its category) or a GUID:

| Kind (Terminal Types 2.0 / 1.0) | First of its kind | 2nd to 8th |
|---|---|---|
| Microphone: `0x0201` to `0x0206`; `0x0401` to `0x0405` (a handset's, headset's or speakerphone's input) | no `Name`: Windows' name for `KSNODETYPE_MICROPHONE` | the driver's "Microphone 2" ... "Microphone 8" |
| Line In: `0x0601` analog connector, `0x0603` line connector, `0x0604` legacy audio connector | `KSAUDFNAME_LINE_IN`: Windows' name | "Line In 2" ... "Line In 8" |
| Digital In: `0x0602` digital audio interface, `0x0606` and `0x0607` (1394), `0x0608` to `0x060A` (ADAT, TDIF, MADI) | the driver's "Digital In" | "Digital In 2" ... |
| S/PDIF In: `0x0605` | the driver's "S/PDIF In" | "S/PDIF In 2" ... |
| Input: any other type (`0x05xx` telephony, `0x07xx` embedded functions, unknown) | the driver's "Input" | "Input 2" ... |
| Stereo Mix: a branch reaching only the USB streaming input | the driver's "Stereo Mix" (A1) | "Stereo Mix 2" ... |

A source behind a join that reaches several terminals takes the kind of
the first non-streaming one in route order. Numbers follow route order
(28.7). Whether each system has a name for `KSNODETYPE_MICROPHONE` and
`KSAUDFNAME_LINE_IN` (MediaCategories) is read in the legs; a system
that has none shows what it shows, and the record says so. The driver's
strings are drafts for the owner: "Digital In", "S/PDIF In", "Input",
"Stereo Mix", and each kind with " 2" to " 8" appended (all six kinds,
the two Windows-named ones included): 4 + 42 strings, each with its own
GUID. The MUX node is named `KSAUDFNAME_RECORDING_SOURCE`; VOLUME and
MUTE carry no name.

**What each system is given, and what the legs must show.** The table
says the topology; the panels' presentation (sliders, balance, mute and
Select boxes, dB readouts) is a hypothesis each leg confirms or
corrects:

| System | Given | To be read |
|---|---|---|
| 98 SE, ME (WDMAud; Volume Control, Options, Properties, Recording) | one source line per source with VOLUME and MUTE; a MUX control on the recording destination | which controls the panel shows per line; that Select picks the source; that WDMAud is stable at arrival, replug and removal |
| 2000, XP (`sndvol32 /r`; Sounds and Audio Devices, Audio, Sound recording, Volume) | the same | the same |
| Vista, 7, 8 (Recording tab) | one bridge pin per source, each with its VOLUME and MUTE; the endpoint builder enumerates the pins ("Multiplexed (MUX) and de-multiplexed (DEMUX) pins are managed automatically and enumerated by the AudioEndpointBuilder service") | the endpoints and their names; Levels against the node's range; whether the audio engine uses the node or its own software volume ("Software Volume Control Support"); whether and when the audio system sets the MUX for an endpoint (enumeration does not say); two endpoints opened at once |

**Endpoint compatibility on Vista and later.** The sources share one
wave capture pin, which admits one stream a direction (the gate of
section 27, and one pin instance): two source endpoints are mutually
exclusive ("streaming to one of them makes it impossible to stream to
the other at the same time", "Audio Endpoint Builder Algorithm"), and
opening a second while the first records is refused (by the pin's
instance count, or by the gate with `STATUS_DEVICE_BUSY`); the route
lock (28.7) keeps the second endpoint's MUX change from reaching the
first's recording. What Windows tells the second program, and how
shared mode behaves, are read in the 7 leg. If that leg shows the audio
system does not set the MUX when an endpoint opens, the per-source
endpoints would record whatever the device has selected; then, and only
then (B2), the reduced form is one endpoint (no MUX node on WDM 6.00
and later), the choice left to the device, which loses D3 there; the
limitation is recorded with the leg.

**What an existing user sees after the update.** On 98 SE to XP the
waveIn devices are unchanged (one wave filter); the recording panel
gains lines. On Vista and later a device with a chooser gains recording
endpoints (one per source past the first); the first source keeps the
pin, so the existing endpoint is expected to keep its identity and stay
the default (read in the upgrade leg); applications that saved a waveIn
index rather than an endpoint may find another device at that index,
and a device without a chooser whose capture category changes (the
selected path's terminal instead of the first by id) may get a new
endpoint name. The release notes say so.

**Fallback on 98 SE and ME.** 98 SE's WDMAud faulted on capture
topologies twice before (`uactopo.c`'s notes, builds 14 and 15). The
98 SE leg runs first; if, and only if (B2), WDMAud faults on the MUX
topology there, the reduced form is no MUX node on WDM 1.0 systems
(98 SE and ME, `IoIsWdmVersionAvailable(1, 0x10)` false): the selected
source's line only, which loses D3 there; the limitation is recorded
with the leg.

### 28.11 Software level on capture

Where a source has no eligible writable volume or mute (or a control
fails), the level is applied to the captured samples on the USB worker
(`Captured`, before `WriteBytes`; for `uac1`'s converted formats on the
16-bit frames after `UacConvIn`), with `UacVolGains` and `UacVolScale`
as on playback: never above unity, the known state accounted (28.4),
and a mute the device does not hold zeroes the samples.

### 28.12 Registry values: the complete contract

Every value this work adds, in the device's driver key unless said:

| Value | Written by | Type | Meaning, and what the driver does with it |
|---|---|---|---|
| `Uac2CapFirst` | the INF, no-clobber, `1` | REG_DWORD (9x: 4-byte REG_BINARY, as `Uac2QueueMs`) | read at start: 1 runs the first-arrival pass (28.6), anything else skips it; a user's 0 survives an update |
| `Uac2LevelC` (pair 0), `Uac2LevelCnn` (pair nn, two hex digits) | the driver | REG_BINARY, layout below | the saved recording level and source of that capture endpoint; absent, malformed or stale: treated as absent |
| `Uac2VolMapC` (pair 0), `Uac2VolMapCnn` (pair nn) | the driver, written, never read | REG_BINARY, layout below | diagnostics of the capture level |
| `Uac2VolMap` (existing) | the driver | REG_BINARY | grows from 232 to 272 bytes, append-only (layout below) |
| `Uac2UsbStats`, `Uac2UsbStatsIn` (existing) | the driver | REG_BINARY | grow from 94 to 100 words, append-only (layout below); in `Uac2UsbStatsIn` words 65 to 93 carry the capture level and `VolLayout` reads 28 |
| `MediaCategories\{GUID}` `Name`, 46 keys under HKLM (9x: `System\CurrentControlSet\Control\MediaCategories`) | the INF | REG_SZ, from `[Strings]` | the driver's source names of 28.10 |

**`Uac2LevelC` layout**, little-endian DWORDs unless said:

| Offset | Field |
|---|---|
| 0 | `Version` = 1 |
| 4 | `Size` (the whole value's bytes) |
| 8 | `DescHash`: CRC-32 of the AudioControl interface's class-specific descriptors as parsed |
| 12 | `Channels` (node channels, 1 or 2) |
| 16 | `Sources` (0 = no chooser, else 2..8) |
| 20 | `Selected` (source 1..N the user last chose; 0 none) |
| 24 | `PassDone` (bit k-1: source k had its pass) |
| 28 | per source k = 1..max(1, N), 64 bytes each: `RouteLen` (1 byte, selectors on the route, 0..7), `Route` (7 x 2 bytes: selector id, input), `EndTerminal` (1 byte), 1 pad byte, then `Owned` (DWORD: 1 volume, 2 mute), `Mute` (DWORD), `Level[8]` (LONGs, KS units), and 0 to pad to 64 |

**Malformed** (wrong `Version` or `Size`, `Channels` or `Sources` out of
range, a route naming an entity that is not a selector, an input out of
range, a level outside the node's range): the value is ignored as
absent (every source's pass runs again; the driver writes a fresh value
at its first save). **Stale** (`DescHash` differs: new firmware, another
configuration; or a source's route or end terminal no longer matches the
descriptors): ignored as absent the same way. **A failed write**
(`PutValue` failing: a full or locked registry, a removed devnode): the
in-memory state goes on, the failure is counted in `Uac2VolMapC`, and
the next change tries again; if no save ever succeeds, the next start
is a first arrival again (the pass runs again: defined, not harmful).

**Saving under the generation.** A save takes, under the level spin
lock, the change counter and a copy of the state to save; it writes the
copy outside the lock; then, under the lock again, if the counter moved
in between (a SET landed after the copy was taken, even after the check
and before the registry write), it marks `Pending` so the work item
saves again from the newer state. So the value in the registry always
ends as the latest state, and a pass's result is never written over a
user's SET (28.6).

**Diagnostic layouts.** All little-endian, 4 bytes a field (DWORD or
LONG; NTSTATUS as DWORD), byte offsets, no padding; every array bounded
as given, unused entries 0. Readers take a value's length as its
layout: a shorter value is an older build's.

`Uac2VolMap` (playback; offsets 0 to 228 are build 25's
`UAC_LEVEL_DIAG`, unchanged):

| Offset | Field | Bounds |
|---|---|---|
| 0 to 48 | `Channels`, `Output`, `VolumeUnit`, `VolumeMode`, `VolumeChannels`, `VolumeExtra`, `MuteUnit`, `MuteMode`, `MuteChannels`, `MuteExtra`, `Owned`, `Known`, `MuteKnown` | as shipped |
| 52, 56, 60 | `KsMin`, `KsMax`, `KsStep` | KS units |
| 64, 68, 72, 76 | `Change`, `Writes`, `Failures`, `LastStatus` | |
| 80 | `Mute` | 0, 1 |
| 84 to 112 | `Level[8]` | KS units |
| 116 to 144 | `Gain[8]` | Q16 |
| 148 to 180 | `Actual[9]` | 1/256 dB, slot 0 master |
| 184 to 216 | `ActualMute[9]` | |
| 220, 224, 228 | `KsTop`, `AboveZero`, `Lowered` | |
| 232 | `OtherCount`: other controls on the route (28.9) | 0..255; the masks below cover the first 32 in route order |
| 236 | `OtherZ1`: bit i, other control i is in Z1 | 32 bits |
| 240 | `OtherDone`: bit i, its normalisation write confirmed by readback | |
| 244 | `OtherAssumed`: bit i, unreadable with no RANGE, taken as 0 dB | |
| 248 | `OtherLow`: bit i, a volume outside Z1 at silence or minimum, left | |
| 252, 256, 260 | `NormWrites`, `NormFailures`, `NormIgnored` | counters |
| 264 | `NormStatus`: last failing write's status | |
| 268 | `GraphFlags` of the playback walk (as `Uac2VolMapC`'s) | |

`Uac2VolMapC` (capture, per pair: pair 0 `Uac2VolMapC`, pair nn
`Uac2VolMapCnn`; the selected source's level in `Uac2VolMap`'s first 232
bytes, then):

| Offset | Field | Bounds |
|---|---|---|
| 232 | `Sources` | 0 (no chooser), 2..8 |
| 236 | `Confirmed` | 0 unknown, 1..8 |
| 240 | `Requested` | 0 none, 1..8 |
| 244 | `RouteState` | 0 confirmed, 1 unknown, 2 switching, 3 incomplete graph (no hardware writes, 28.3) |
| 248 | `PassDone` | bit k-1, source k |
| 252 | `GraphFlags` | bit 0 more than 64 routes, 1 deeper than 32, 2 a malformed or missing entity, 3 an unlisted entity class, 4 more than 8 sources, 5 chooser held by another pair, 6 a loop (a route cut where an entity repeats), 7 no graph (its storage could not be allocated at start, 28.18); bits 0 to 3, 6 and 7 make the graph incomplete (28.3) |
| 256 to 351 | `Selector[8]`, 12 bytes each at 256 + 12i: `Id`, `Position` (0 unknown), `Status` (last request's) | the selectors of every route, first 8 in route order |
| 352, 356, 360 | `PassWrites`, `PassFailures`, `PassIgnored` | counters |
| 364 | `HeldElsewhere`: controls on the source's graph another endpoint holds (28.5) | count |
| 368 | `MuxIrql`: the IRQL of the last MUX SET | 0..31 |
| 372 | `MuxStatus`: its completion status | |
| 376 | `MuxRefusedLocked`: SETs refused by the route lock | counter |
| 380 | `SaveFailures`: failed registry writes of `Uac2LevelC` | counter |
| 384 | `Generation`: the change counter at the last save | |
| 388 | `MuxTimeouts`: switches that did not get the operation mutex or the switch slot in time (28.7) | counter |
| 392 | `MuxPended`: raised-IRQL SETs pended (0 while the pended path is disabled, 28.7) | counter |
| 396 | `MuxCompleted`: pended SETs completed (equal to `MuxPended` when none is outstanding) | counter |
| 400 | `RouteReaders`: open capture streams, of any pair, whose locked graph holds a control of this endpoint's graph | 0..8 |
| 404 | `MuxRefusedIrql`: MUX SETs refused at raised IRQL (no asynchronous path, 28.7) | counter |
| 408 | `AutoSkipped`: automatic writes skipped for another endpoint's reader (28.7) | counter |
| 412 to 439 | the route table's fields of stage 2 (28.23): `RouteControls`, `RouteRoutes`, `RouteLocked`, `RouteOpens`, `RouteCloses`, `RouteAnomalies`, `RouteFlags` | as 28.23 |

Size 412 bytes (stage 1), 440 from stage 2 (28.23; provisional, as
stage 2 is).

`Uac2UsbStats` and `Uac2UsbStatsIn`, words after build 25's 94 (word =
4 bytes, word n at byte 4n):

| Word | Field | Render record | Capture record |
|---|---|---|---|
| 65 | `VolLayout` | 25 | 28 |
| 66 to 93 | the level fields of 20.5 | the render level | the selected source's capture level |
| 94 | `CapSources` | 0 | 0, 2..8 |
| 95 | `CapConfirmed` | 0 | 0..8 |
| 96 | `CapRequested` | 0 | 0..8 |
| 97 | `CapRouteState` | 0 | as `RouteState` |
| 98 | `CapPassDone` | 0 | bits |
| 99 | `GraphFlags` | the playback walk's | the capture walk's |

**The INF.** `uac2wdm.inf` writes `Uac2CapFirst` in `[Queue9x]` and
`[QueueNT]`, `uac1wdm.inf` in its `[Queue9x]`, beside
`Uac2VolAboveZero`, with a comment line each; `Uac2VolAboveZero`'s
comment says playback only. Both write the 46 names (an `AddReg` of
HKLM lines; `uac1wdm.inf` includes no Microsoft INF, 26.11, and writes
them itself); whether 98 SE's KS answers `KSPROPERTY_PIN_NAME` from
there is read in the 98 SE leg. The saved levels are state: the INF
writes none of them, as it writes no `Uac2Level` today. `infchk` checks
every line.

**Reset** (user documentation, README's settings list and
`docs/release-notes.md`): delete `Uac2LevelC` (and `Uac2LevelCnn`) in the
device's driver key and restart the device: every source's pass runs
again and the device's own levels are read; set `Uac2CapFirst` to 0
first to keep the device's levels instead. The names and every value
above are listed there.

### 28.13 USB Audio 1.0 (`uac1`), and its scope

- **Selector Unit** (ADC-1 4.3.2.4): `bLength` 6 + p, `bNrInPins` at 4,
  `baSourceID` from 5, `iSelector` last; no `bmControls`: the selector
  control is taken as read/write. Requests (ADC-1 5.2.2.3): `SET_CUR`
  0x01 / `GET_CUR` 0x81, `bmRequestType` 0x21 / 0xA1, `wValue` 0,
  `wIndex` unit << 8 | interface, one byte, one-based. A stall as 28.7
  (the Play! 2 stalls `GET_CUR` on its Selector Unit 12, a sidetone
  branch the capture walk never meets).
- **Feature Unit**: `bControlSize` bitmaps (D0 mute, D1 volume), MIN,
  MAX and RES as today (26.11).
- **Processing Unit** (ADC-1 4.3.2.6): up/down-mix and the other
  processes change or may change the cluster (28.4's channel rule).
- **Scope**: `uac1` is a 98 SE package (26.10; `STAGE.TXT`, the
  `w98se` folder). Its INF's undecorated `[Models]` is also read by ME
  and 2000, where nothing stages it and Update Driver to it is not
  supported (the INF's own comment); XP and later never match it. So
  this work's `uac1` legs are 98 SE only, ME and 2000 are not served,
  and the 98 SE fallback of 28.10 applies to it.

### 28.14 Failure handling

- **A SET that stalls, or succeeds and is ignored** (readback shows the
  old value): an owned control is kept dirty and retried at the next
  apply (a RUN, a SET, a return to D0); a pass or normalisation write is
  not retried (28.6, 28.9); both are counted (`Failures`, `Ignored`,
  `LastStatus`). The software gain takes the readback, or the range's
  top where unknown; a failed capture mute zeroes the samples.
- **A GET that stalls**: unknown; no pass raise on that volume (a mute
  is still cleared); the software gain takes the range's top.
- **A RANGE that lies**: malformed is no range (28.6's reading,
  `UacVolRange` as today: software for a level control, untouched for a
  pass or normalisation target). A CUR outside the range is snapped for
  the node; below MIN counts as minimum for the pass; above the top is
  left. Steps the device then refuses are the stalled or ignored SET.
- **A selector**: 28.7's states and partial-failure rule.
- **A failed registry write**: 28.12.

### 28.15 Code shape (so the rules can be executed on the host)

The level engine leaves `uacwdm.cpp` for a portable module (`uaclevel.c`,
no Windows types, compiled into both drivers and the host tests): the
cached levels and generations, the claims table, the source routes and
their states, the first-arrival transaction, the switch with its
partial-failure rule, the D2 zones and targets, the gain accounting,
and the saved-value encode, check and decode. It talks to the device
and the registry through a small table of callbacks (control GET/SET,
value read/write). `uacwdm.cpp` keeps the KS properties, the locking,
the work item and the USB requests. So the host tests drive the real
rules with a fake device. The engine's apply (`UacLevelApply`, the
Wave's `ApplyLocked`) runs at PASSIVE with the Wave's operation mutex
held (`Apply` takes it through `TakeOperation`), with one exception, the
exclusive startup: `Prepare` applies the levels without the operation
mutex before the pair is published, when no other caller can reach it.

**The playback engine moves without changing behaviour.** The shipped
playback level code (`LevelSetup`, `ReadState`, `ApplyLocked`, `Gains`,
`Assumed`, the slot helpers, `LevelProperty`'s SET path, the save and
`RecordLevel`) moves into `uaclevel.c` first, alone, in a commit that
changes no behaviour; the new rules come in later commits.

**The build switch, as the build does it.** The default build's Wave
calls the engine; `UAC_LEVEL=retained` in the environment makes
`build.cmd` (`uac2` and `uac1`) define `UAC_LEVEL_RETAINED`, and the
Wave then keeps the shipped methods. Both objects are linker inputs in
every build: `uaclevel.c` is compiled and `uaclevel.obj` given to the
linker in a retained build too. Preprocessing selects one Wave path in
`uacwdm.cpp`, the retained methods or the calls into the engine, so
exactly one of the two is referenced; in a retained image the engine's
functions are unreferenced and the linker removes them (`/Gy` with
`/OPT:REF`). No setting selects neither path.

What must stay identical, the preservation contract:

1. The saved value `Uac2Level`/`Uac2LevelPnn`: its version 2 layout,
   read only when `Version` is 2 and `Channels` match, the `Owned` bits
   and what each restores; written when `Change` differs from `Saved`.
2. Range handling: `UacVolRange` (a RES of 0 on a span read as 1, a MIN
   of silence read as its next step, `KsTop`, `KsMax` at or below 0 dB,
   `Uac2VolAboveZero` = 1 setting `KsMax` = `KsTop`), the one-subrange
   retry of `ReadRangeOnce`, and the software range where none is read.
3. The start-time lowering of section 20.3: only a written control whose
   CUR reads above `KsMax`, that control alone, a failed write leaving it
   unknown.
4. The extras: `VolumeExtra`/`MuteExtra` set to `UacVolFromKs(0, range)`
   and 0 once owned, `NoRange` extras read and never written, the
   `VolumeNormal`/`MuteNormal` flags and their reset at D3.
5. The gains: `UacVolGains` with unknown volumes as `Assumed` (`KsTop`, or
   +128 dB for a `NoRange` extra), an unowned volume leaving the device's
   setting (no software volume), the silencing of a `NoRange` extra with
   an unknown CUR, and the lower of before and after when writes follow.
6. Order and retry: the four passes (volume lowerings, mutes on, volume
   raisings, mutes off), the 40 ms wait before a louder write while
   running, a failed write holding every later pass and leaving the
   control unknown and dirty, `LevelStale` and its use at RUN.
7. The KS answers: basic support ranges and steps per channel, GET of
   the loudest channel for channel -1, a SET snapping to the range and
   owning the control, `KSPROPERTY_AUDIO_CPU_RESOURCES`.
8. The diagnostics: `Uac2VolMap`'s first 232 bytes and the stats words
   65 to 93 of a render record, field for field.

**Preservation tests pass before and after.** Two kinds:

- Host: the pure functions already portable (`UacVolRange`,
  `UacVolFromKs`, `UacVolTargets`, `UacVolGains`, `UacVolScale`,
  `UacVolFind`) get vectors for every item above they cover; they pass
  on the code as it is and after every commit.
- Traces: before the move, scripted level sequences (a start with each
  device state, SETs of volume and mute, a restore, a refused write, a
  D3 and RUN, `Uac2VolAboveZero` 0 and 1) are run against the shipped
  build in a guest with the simulator, and the simulator's log of every
  Feature Unit request (order, unit, channel, value) is kept as the
  baseline (`cx31993m`, `hs24` with `--fu full`, `master`, `channels`,
  `x4full`). After the move, `leveltest` replays the same scripts through
  `uaclevel.c` with a fake device answering as the simulator did and
  must produce the same request sequence and the same saved values and
  `Uac2VolMap` bytes; and the guest run of the moved build must log the
  same sequence as the baseline. Any difference stops the move.

### 28.16 Tests

**Host tests**: the preservation tests above pass before and after each
commit; the behaviour tests below are written to fail on the code before
the change they test and pass after it:

- `voltest` (uac2) and `uac1test` (uac1), the walk: the X4 capture graph
  per source (controls 2, 23, 25, 8; four sources typed `0x0201`,
  `0x0602`, `0x0603`, `0x0605` in that order); the X4 playback other
  controls (none on Input Terminal 3's path besides Feature Unit 1's
  own; 11, 12, 13 never listed); the Logi (Feature Unit 2; playback's
  Feature Unit 11 never listed), the Play! 2 (Feature Unit 6 through the
  one-input Selector Unit 10; Selector Unit 12's branch never listed),
  `selcfg`'s two sources; built graphs: a diamond (both routes kept, the
  unit once), a loop on one route (the other routes listed for the
  diagnostics, the graph incomplete, `GraphFlags` bit 6), a unit on one mixer
  input (not eligible, software level), a unit after the mixer
  (eligible), a multi-input Extension Unit (nothing upstream eligible),
  a multi-input Processing Unit (malformed, 28.3),
  a one-input Extension Unit (master upstream eligible, per-channel not),
  a channel-changing mixer (per-channel upstream not eligible), a common
  unit after a chooser, two selectors in series with a shared inner
  selector, a selector behind a join (fixed), 8 and 9 sources, 65 routes,
  a "Stereo Mix" branch, a clock selector never a source; and for each
  variable layout of 28.3's well-formed entities, in both families, valid
  vectors and their truncations (the tail, part of an entry, whole
  entries) making the graph incomplete with nothing eligible and nothing
  claimed (`captest`, built by `uac2` and, with `UAC_V1`, by `uac1`).
- `leveltest` (new, both families), the engine of 28.15 against a fake
  device that can stall, ignore a SET, answer outside a range, report a
  malformed RANGE, change a selector itself, and run a callback between
  any two requests: a KS SET arriving between the pass's writes (the
  user's value is applied and saved, the pass's is not); the pass's
  "pass done", owned and pending states after each failure kind; an
  ignored SET on a pass target and on an owned control (counted, not
  looping); readback values becoming the owned level; a switch failing
  at the inner and at the outer selector, with and without a shared
  inner selector on the old route (confirmed, unknown); an unknown route
  never passing an assumed input; a device-side change found at RUN; D0
  (state unknown, re-read, owned controls rewritten); two endpoints
  claiming one unit's controls (first wins, the other never writes);
  the D2 zones and targets (mute-only, volume-only, positive-only range,
  no RANGE, an other unit unreadable taken as 0 dB and later counted);
  the full-route gain accounting across Pass, opaque and join entities;
  the saved value's encode, decode, malformed and stale cases and a
  failing registry write; a SET landing after the save's generation
  check and before its registry write (the value ends as the SET's);
  the claims table built for two playback and two capture pairs before
  any write (reservations, a unit whose controls go to different
  holders, the later pair never writing a held control; KS traffic to
  pair 0 between its registration and a later pair's preparation
  finding the table complete); an incomplete
  graph (65 routes, depth 33, a malformed entity, and **one valid branch
  beside one loop**) making no hardware write: no selector, level, pass
  or normalisation request at all in the fake device's log, the level in
  software, `RouteState` 3; a MUX SET at raised IRQL refused
  `STATUS_INVALID_DEVICE_STATE` with nothing written while the pended path
  is disabled; and, for the pended path when it is enabled, the switch
  record's protocol (28.7) at every interleaving the fake's callback can
  make: admission against `Shutdown` (never `Work` 0 with a published
  request), admission against a running coalesced worker (never a
  half-published record taken), a new SET admitted while the old
  completion runs (the old one's status and diagnostics its own), a
  power cycle between admission and the worker (no hardware work after
  D0, `STATUS_DEVICE_NOT_READY`); a SET at
  raised IRQL pended and completed with the readback's outcome; every
  pended request completed exactly once (the fake PortCls counts
  completions per request and poisons a request once completed, so any
  later access fails the test); a second SET answered busy at raised
  IRQL, and at PASSIVE served after the first within the time-out or
  answered busy after it; `Shutdown` at removal and at stop with the
  switch queued and not started (completed `STATUS_DEVICE_REMOVED` /
  `STATUS_CANCELLED`, nothing written) and with the switch mid-way
  (finished, completed with its outcome, completed once); power down with
  a switch queued (`STATUS_DEVICE_NOT_READY`, nothing written); the
  caller's cancellation at each step (queued, between selector writes,
  after the last readback: the switch runs to its end, one completion,
  the state as confirmed); the operation mutex held past the time-out
  (busy, nothing written, `MuxTimeouts`); the route lock across pairs:
  endpoint B recording through A's chooser at its read position, A's SET
  to another source refused busy while B is open and admitted after B
  closes, A's SET to the confirmed source admitted; an open of B while A
  switches (waits, then locks the route A left; or busy after the
  time-out); the open and the switch interleaved at every step through
  the fake's callback (never a moved selector under an open stream);
**the same endpoint's open during its own switch**, in exactly this
order: A's switch passes its check and reserves its selectors (`Switch`
writer), A's capture open then runs its hold (it must wait, not register
`Readers` on the old graph), the switch moves the selectors and ends,
and the open then registers on the graph the switch left (the new
source's; every route where the switch ended unknown), the stream's
locked set and `Version` being those of that graph; the same with A's
own `Level` write (a restored level on its confirmed source) in progress
instead, which the open does not wait for; and the reverse order (the
open registers first, then A's switch is refused busy); the worker
waiting for the operation mutex across a whole D3 and D0 (the epoch
checked after the mutex: `STATUS_DEVICE_NOT_READY`, nothing written), a
switch admitted while the device is down, and a PASSIVE switch whose
caller waits across a power cycle (the same); `MuxIrql` taken from the
worker's locals while a new SET at another IRQL is admitted; the
worker's own `Work` reference: a request completed and its reference
dropped while the worker still loops over a level SET, `Shutdown` not
finishing before the worker's last access; a
  pass and a restored level skipping a control on B's locked route and
  writing it after B closes; a failed create giving its route back; the
  same on WDM 1.x and on WDM 6.00 (one rule); two per-source endpoints of
  one pair on WDM 6.00 with the SET before a pin open that then fails
  busy; the restore of the saved input at start (valid, stale, a
  selector that refuses); the Z1 rule (a mixer between a unit and the
  controlled volume: Z1 empty; an opaque unit on the segment; software
  volume: no Z1; a diamond); a monitoring-branch unit that a playback
  rule would unmute (built through a diamond: no write at all, listed,
  neither `OtherZ1` nor `OtherDone`).
- `topotest`: the built capture lines for 0, 1, 2 and 8 sources with and
  without render, the kept pin numbers, `UacLinksCheck` with MUX inputs
  (refused on other nodes), and a parse as the WDMAud page describes
  (each source line holds its VOLUME and MUTE, the destination its MUX).
- `infchk`: `Uac2CapFirst` once, no-clobber, at 1, in every install
  section of both INFs; the 46 names; mutations refused.
- `uacsimtest.py` / `uac1simtest.py`: the simulators below.

**Simulators** (`uacemu.py`, `uac1emu.py`). Capture today streams a
1 kHz tone whatever the units hold. Changed: each capture IN endpoint's
signal is computed through its graph: each Input Terminal its own tone
(a frequency per terminal, logged), selectors at their positions, a
Mixer Unit summing, Feature Unit mute and volume per channel, silence on
mute or the silence value, gain above 0 dB clipped and counted. Playback
rendering gains the monitoring branches (each Input Terminal's tone
through its branch units into the mixer), so the log carries per window
the rendered playback peak and the sidetone peak apart, and the tests
assert that no window during a level apply, a normalisation or a switch
peaks above both its ends, and that a sidetone peak moves only where a
rule allows it. New options: power-up state of any unit and channel
(`--unit-mute ID[:CH]`, `--unit-vol ID[:CH]=V`), per-unit ranges
(`--unit-range ID=MIN:MAX:RES`), a selector's position (`--sel ID=N`),
a selector whose GET stalls (`--sel-stall ID`), a selector that ignores
SET (`--sel-ignore ID`), a device-side move (`--sel-move ID=N@S`), an
ignored volume SET (`--unit-ignore ID`). Built profiles: `levels` (a
playback path with Z1, Z2 and Z3 units and a sidetone branch; a capture
chooser with a common unit after it, two selectors in series with a
shared inner selector, a one-input selector); `many` (a chooser of
`--sources N`, 2 to 9); `twocap` (two capture endpoints sharing a unit);
`twosel` (two capture endpoints, each with its own clock so that they
make two pairs, the second's route passing through the first's
chooser: the cross-pair route lock); `loop` (one valid capture branch
and one looping branch: no request to any capture unit or selector).
`uac1emu.py`: a two-input capture selector variant of `headset`. Every
Selector Unit `SET_CUR` and every write to a unit a rule keeps untouched
is logged, the latter as an error the tests fail on.

**Guest legs** (QEMU, at most two guests at once, fresh overlays of the
installed bases; `UACWAV` gains the recording lines, the MUX and the
endpoint opens):

- **98 SE under xHCI98** (`uac2`: `x4full`, `levels`, `many` with 8 and
  9; `uac1`: `logi`, `play2`, the selector variant) and **98 SE under
  NUSB's EHCI** (`uac2` `x4full`): first arrival with the capture unit
  muted and at minimum; the panel's lines, names and Select; each source
  recorded with its tone; series selectors; a level set, replug, the
  level and source restored and no second pass; `Uac2CapFirst` 0; a
  device-side switch during a recording and the next RUN; playback with
  Z1, Z2 and Z3 units powered up muted or low; the sidetone never
  written; WDMAud stable at arrival, replug and removal; `twosel`'s
  cross-pair lock (below); `loop` writing nothing.
- **XP SP3 x86** (Windows' EHCI and xHCI98): the same through
  `sndvol32 /r` and the mixer API, and `twocap`.
- **Rapid and concurrent selection** (98 SE and XP, through the mixer
  API, `x4full` and `levels`): one program setting the MUX many times in
  quick succession, and two programs (or two threads) setting it at
  once; for every call its return value, then the final
  `mixerGetControlDetails`, the simulator's log of the selector's last
  `SET_CUR` and its position (the hardware), and the recording panel
  reopened, compared: they must agree on one source. A call refused busy
  is acceptable; that a mixer retries is not assumed, and what the open
  panel shows after a refused call is recorded per system. Each run
  records the IRQL of the SETs (`MuxIrql`) and `MuxPended` =
  `MuxCompleted` at the end.
- **Cross-pair route lock** (98 SE and XP: two waveIn devices; 7: two
  endpoints of the two pairs; `twosel`): pair 1 recording through pair
  0's chooser; pair 0's MUX set to another source from its panel: refused,
  and pair 1's recorded tone unchanged throughout (the simulator's
  per-endpoint tone log); the same SET after pair 1 closes: applied; pair
  1 opened while pair 0's switch runs (scripted): the recording carries
  the source the switch left. On XP and 7 also two programs on one
  recording device where the system admits a second client.
- **7** (x86 or x64): one endpoint per source and their names; Levels
  and mute against the node; recording from each endpoint (whether the
  MUX follows); shared and exclusive mode opens; two endpoints opened at
  once by two programs (the second refused busy); the upgrade from
  0.1.11.0 with a chooser device installed (the default recording device
  and its identity kept or not).
- **Owed, not run in this work**: ME, 2000 and Vista (their behaviour
  is expected to follow 98 SE, XP and 7 respectively; not shown), 8, and
  the x64 systems other than the 7 leg.
- Regression: section 20's legs (`cx31993m`, `hs24`), the ALC4080
  capture's three inputs (no chooser), the host gates.

### 28.17 What is still open

The owner's questions of both drafts are answered (A1 to A6, B1 to B6).
The refusal of a live source change while a recording depends on the
route, on every system, is decided as provisional by the owner (C1,
2026-11-17), to be confirmed or changed after the mixer legs on each
system. **Accepted (owner, 2026-11-17)**: the refusal of a MUX SET made
at raised IRQL, provisional, until an adequate supported cancellation protocol
for a pended property is established (28.7, "Cancellation"); it changes
what a user sees on a system that sets the MUX at raised IRQL. The
recommendation recorded with it (28.7): accept it provisionally for
development, with explicit failure and no deferred write; measure the
MUX SET's IRQL early on every target system; where a normal system
panel consistently sends it at raised IRQL, input selection is
unsupported on that system and no readiness is claimed there until a
supported completion arrangement exists.
Open, and settled by the legs, not by a choice: whether 98 SE's WDMAud
takes the MUX topology (B2's 98 SE and ME form only if not); whether
the audio system on Vista and later sets the MUX for an endpoint (B2's
single endpoint only if not); at which IRQL each system sets the MUX,
unknown on every system until measured there (28.7: a raised-IRQL SET
is refused while no adequate supported cancellation protocol exists);
which system names exist for `KSNODETYPE_MICROPHONE` and
`KSAUDFNAME_LINE_IN`; what each panel shows (28.10), also after a MUX
SET refused busy or by the route lock (28.7); and the 2 s time-outs of
28.7, provisional until the legs show what mixers do with them; and the
automatic-write and route-publication rules of 28.7's route table, in the
cross-pair legs. Each answer is
recorded with its leg, and a reduced form, where one is taken, as a
limitation in the user documentation. Stage 2's own questions (S1 to
S4) are in 28.25; 28.23 records that 28.12's stats words 94 to 99
collide with words already in use.

### 28.18 Wiring in stages, and stage 1: discovery and diagnostics

The host modules (`uaccap.c`, `uacroute.c`) go into the driver in
stages, each with its own gate before the next: **1** discovery and
diagnostics; **2** the route table's open and close reservations (28.7);
**3** recording levels and automatic writes, with the claims (28.4 to
28.6, 28.5, 28.9); **4** the synchronous MUX and the topology (28.7,
28.10). Stage 1 is wired (2026-11-17); stages 2 to 4 are not, and
nothing in this subsection is a part of them. Stage 2 is designed in
28.19 to 28.25; it moves the building of every endpoint's graph into
pair 0's `Prepare` (28.20), which this subsection gives to stage 3.

**Where discovery runs.** `Wave::Discover`, called once by each pair's
`Prepare` as its last step, only when `Prepare` has otherwise succeeded:
at PASSIVE, in the PnP start thread, after the descriptor model was
parsed (`ParseDevice`) and the pair's terminals learned (`Learn`), after
the render level's setup and first apply, with no driver lock held. A
pair with no capture line (no capture format) does nothing. Discovery is
`UacCapStart`: the graph of the pair's capture endpoint (the USB
streaming Output Terminal of its capture formats) built from the
descriptors by `UacCapBuild` (28.3), and the `Uac2VolMapC` record filled
from it.

**Storage.** The graph (`UAC_CAP_GRAPH`, 7424 bytes) comes from
nonpaged pool (`ExAllocatePoolWithTag`, tag `'g2aU'`) at PASSIVE with no
spin lock held, never from a kernel stack; it is kept for the pair's life
(the later stages read it) and freed with the pair (`~Wave`). A stop and
start, or a removal and arrival, makes new pairs and so a new graph
(28.5's invariants). The record (412 bytes) is a member of the pair
object, itself in nonpaged pool; the two members come last in the object,
so every earlier member keeps its offset. There is no claims table
(2572 bytes) and no route table (65068 bytes at the default bounds) in
stage 1: the claims build makes the first-arrival reservations (28.5,
step 4) and exists to arbitrate writes, which stage 1 does not make, and
the route diagnostics need neither. Each pair builds its own graph in its
own `Prepare`, touching only its own object and the descriptor model,
which nothing writes after the parse; stage 3's claims build moves the
building of every endpoint's graph into pair 0's `Prepare` (28.5).

**Stack.** The walk recurses per entity on a route (`Enter`) and per
channel-count lookup (`Width`), each bounded by the depth of 32 (28.3).
From the compiler's listings with the drivers' own flags: on x86 about
40 bytes a level of each and 424 bytes for `UacCapBuild`'s frame, so
about 3.2 KB at the bounds (a route 32 deep whose deepest unit looks up a
chain 32 deep), and under 1 KB on the recorded devices' routes (3 to 5
entities); on x64 about 96 bytes a level and about 6.9 KB at the bounds.
Those figures are the walk's own frames, measured from the listings and
added on top of whatever the PnP start's call chain already holds when
`Prepare` calls `Discover`; that chain's own depth, and so the room left
on 98 SE's and ME's kernel stack, is not measured here and stays open
([open-items](../testing/open-items.md), "kernel stack room for the
capture discovery on 98 SE and ME").

**Failure.** An allocation failure leaves the pair without a graph and
the record says so (`RouteState` 3, `GraphFlags` bit 7). An incomplete
graph (28.3) is recorded with its flags and `RouteState` 3. Neither
changes anything else: nothing reads `UacCapStart`'s answer or the graph,
`Prepare` returns what it returned before, and the pair starts as it did.
A failed registry write is not retried (the record is diagnostics only).

**The diagnostics exported.** `Uac2VolMapC` (pair 0) and `Uac2VolMapCnn`
(pair nn, two upper-case hex digits), REG_BINARY in the device's driver
key, written once per start through the driver's existing `PutValue`,
exactly 412 bytes (28.12's layout). Stage 1 fills:

- `Channels` (offset 0): 1 or 2, from the capture formats as the render
  level counts its own; `Output` (4): the capture Output Terminal's id.
- `Gain[8]` (116 to 144): 65536 (unity): the driver does not scale
  captured samples.
- `Sources` (232), `RouteState` (244: 0 with no chooser; 1 with a
  chooser, its position not read; 3 incomplete or not built),
  `GraphFlags` (252), and `Selector[i].Id` (256 + 12i) for the first 8
  selectors on any route, as `UacCapDiag` gives them.
- Every other field 0, and `Confirmed`, `Position` and `Status` 0 in
  particular: no selector is read. The level fields at 0 (`Owned`,
  `VolumeMode`, `MuteMode` and the rest) mean that nothing is controlled:
  the device keeps its own settings, as it does today.

No selector position is read in stage 1, provisionally: a GET CUR to a
Selector Unit at start would be a new request on the device's control
pipe (and a stall a new event at start), so stage 1 makes no request at
all; the reads of 28.8 come with the stage that acts on them.

**What stage 1 does not change.** No control is written or read, and no
other request is made. No property handler changes its answers (the KS
property tables and handlers are untouched). No topology node or pin
appears or disappears (`uactopo.c` and the filter descriptors are
untouched). No captured sample is scaled. No existing registry value
changes: `Uac2VolMap`, `Uac2UsbStats` and `Uac2UsbStatsIn` keep their
sizes and contents (the words 94 to 99 of 28.12 are not added yet);
`Uac2LevelC`, `Uac2LevelCnn` and `Uac2CapFirst` are neither written nor
read; the INFs are unchanged. The code is in the shared `uacwdm.cpp` and
`uaccap.c`, so `uac2wdm.sys` and `uac1wdm.sys` both change (`uac1` with
the USB Audio 1.0 layouts), in default and retained builds alike: the
level switch (28.15) selects the playback level engine only. What shows
it, on the host and in one XP leg with the simulator, and what is not
shown yet: `docs/testing/2026-11-17-uac2-cap-stage1.md`.

**Choices in stage 1, provisional:** no selector read at start (above);
the record written even without a graph, with `GraphFlags` bit 7; the
level fields at 0 with the gains at unity; the graph built per pair until
stage 3; the stack depth of the walk at the bounds of 28.3 (whether 98 SE's
kernel stack at a PnP start has room for the 3.2 KB worst case is not
established here; the recorded devices need under 1 KB).

### 28.19 Stage 2: the route table's open and close reservations

Status: design, 2026-11-17, revised 2026-11-18 (the reservation's
ownership, incomplete discovery, the work under the lock, the
diagnostics, the tests and legs), and again the same day (the
reservation's exact boundary, the lifecycle schedules, the failed-state
leg's evidence); not wired. Stage 2 of 28.18's plan
puts the route table of 28.7 ("The route lock") into the drivers with
its first users only: a capture stream takes a **reservation** (it
registers as a reader of every hardware control of its endpoint's graph)
for its whole life and gives it back when it is destroyed. Nothing yet
writes a control, so nothing yet consults a reservation; stage 2 exists
so that the table, its lock, its storage and the reservations' lifetime
are in the drivers, shown on the host and in guests, before stage 3's
first automatic write and stage 4's first switch depend on them. 28.19
to 28.25 are stage 2; where 28.7 already decides a point they follow it
and say so, 28.21 says where they depart from it and why, and the
choices still open are the owner's questions of 28.25.

**What is reserved.** A reservation is 28.7's registration: the
endpoint's bit in `ReaderMask` and one count in `Readers` on every
entry of its locked graph. Stage 2 reads no selector (below), so every
position is 0 (unknown), every route is admitted (28.7: "every route of
its graph when a position is unknown"), and the locked graph is the same
at every open: **every hardware control on every stored route of the
endpoint's capture graph**, as `UacRouteFromGraph` enters them: each
Feature Unit's mute and volume present on its master and on each logical
channel up to `UAC_VOL_CHANNELS` (8), and each Choice's selector, cut
routes included. Where that is not every control the endpoint's signal
can pass (a graph incomplete by 28.3, or a unit with a control on a
channel above 8), the endpoint's protection is **incomplete** and the
adapter's arbitration is latched off (28.22). So the set is fixed at
start: it is computed once into the table (28.20), and an open or a
close only applies or removes it.

**By whom.** One reservation per capture endpoint, owned by the one
capture stream a pair can have (the direction's one-stream gate, section
27, and the one-instance capture pin). The endpoint number in the table
is the pair's index (pair k's capture endpoint is endpoint k,
k < `UAC_PAIRS`); the numbering of playback writers is stage 3's and is
not fixed here. A render stream takes nothing.

**When taken and given back.** Taken inside `NewStream`, under the
pair's operation mutex, right after `UacStream::Create` has succeeded,
and owned from then by the stream `Create` returned; given back by that
stream's `Close` hook, under the operation mutex after `Drain`, when the
stream's queued destroy runs (section 27), and by nothing else. A pause,
a stop, a D3 drain and a PnP stop's `Shutdown` keep it (28.7: "A pause
keeps it"). 28.21 gives the protocol.

**What stage 2 does not do.** No request to the device: no selector is
read (positions stay 0, as in stage 1; 28.8's reads come with the stage
that acts on them, 28.18) and no control is written. No claims table
(28.5), no `Writer` mark of either kind, no switch record and no
`SwitchLock`, no MUX node, no topology or pin change, no KS property
change, no level or first-arrival pass, no software gain on capture.
`Uac2LevelC`, `Uac2LevelCnn` and `Uac2CapFirst` are neither read nor
written, the INFs are unchanged, and no `Uac2UsbStats` word is added
(words 94 to 97 keep their present meaning; 28.23). **No `NewStream`
outcome changes**: the reservation is taken only after `Create` has
succeeded, cannot fail in a way that fails the open, and no new refusal
or wait is added (28.21). No captured sample changes. What changes
besides the table is the diagnostics record (28.23), the start's work
and the memory the table and graphs take (28.20; 28.25, S3).

### 28.20 Stage 2: building the table at start

**Every capture graph in pair 0's `Prepare`.** 28.5's invariants put the
adapter-wide tables in pair 0's `Prepare`, complete before
`StartPair(0)` publishes pair 0, with later pairs only looking up; and
28.18 said stage 3's claims build would move every endpoint's graph
there. The route table needs every capture endpoint's graph for the
same reason (a later pair's controls may be pair 0's too), so stage 2
makes that move now rather than in stage 3:

1. Pair 0's `Discover` (still the last step of a `Prepare` that has
   otherwise succeeded, at PASSIVE, no driver lock held, in the PnP start
   thread) lists the capture endpoints as `Learn` does:
   `UacPlanEndpoints(descriptor, 1, UacUsbHighSpeed(dev), ends,
   UAC_PAIRS)`, the first `min(count, Pairs())` of them, endpoint k at
   terminal `ends[k]` (the terminal pair k's `Learn` takes; `uac1`'s
   `Pairs()` is 1). This is done **whether or not pair 0 itself has
   capture formats**: a pair 0 with none (its rates failed, or a device
   whose first capture terminal belongs to no format pair 0 can run) still
   builds every capture endpoint's graph and the table, because a later
   pair may capture; pair 0 then writes no record of its own (stage 1's
   rule). For each endpoint it allocates a graph (`UAC_CAP_GRAPH`,
   nonpaged, tag `'g2aU'`, as stage 1) and builds it (`UacCapBuild`).
   Graphs are built also for endpoints whose pair will later fail its
   own `Prepare` or is never created; they are walked, entered and
   freed with pair 0 like the others.
2. With `count` 0 (a render-only adapter, or one whose capture terminals
   `UacPlanEndpoints` does not list) **nothing is allocated**: no graph,
   no table, no record; that is not a failure and sets no latch
   (nothing can record, so nothing needs arbitrating). Otherwise it
   allocates the route table (nonpaged, new tag `'r2aU'`) and builds it
   with one new call, `UacRouteBuild` (28.24): `UacRouteInit` (the whole
   table zeroed: entries, counters, flags, owners, sets), then for each
   endpoint in order `UacRouteFromGraph` on its graph (complete or not:
   what is stored is entered), the endpoint's **protection status**
   (complete; incomplete, and why: 28.22), and its **reservation set**,
   `Set[e]`: the union of its stored routes' controls, computed once
   here, sorted, at most 128 table indexes (the `UacRouteGraph` result
   with every position 0). No `RouteLock` is held: this is the exclusive
   startup of 28.15, before any other caller can reach pair 0.
3. It fills its own record (28.23), when it has capture formats, and
   writes it.

A later pair's `Discover` builds nothing: its `capGraph` points at pair
0's graph for its endpoint, and it fills and writes its own record from
that graph and its own capture formats' channel count (stage 1's
fields, through `UacCapRecord`, below) and the table's route fields,
which it reads under pair 0's `RouteLock`, since pair 0 is already
published and its capture opens may run. A pair whose capture formats
are empty writes no record, as in stage 1; its endpoint's routes stay in
the table, never reserved.

`UacCapStart` is split so a record can be filled from a graph built
elsewhere: `UacCapRecord(graph, output, channels, record)` writes
exactly the first 412 bytes stage 1 writes (no graph: `RouteState` 3,
`GraphFlags` bit 7) and nothing past them; `UacCapStart` becomes
`UacCapBuild` then `UacCapRecord`, its answers and its 412 bytes
unchanged (stage 1's `captest` vectors pass as they are).

**Ownership and lifetime.** Pair 0 owns the graphs (`capGraphs[k]` for
every k it built, whether or not pair k was ever created or published)
and the table, initialises every such pointer to NULL in its
constructor, and frees every non-NULL one in its destructor; a later
pair's `capGraph` is borrowed and its destructor frees nothing of it
(stage 1's `~Wave` frees `capGraph` for every pair: that line becomes
pair 0's, over the whole array). A later pair holds a reference to pair
0 (its constructor's `AddRef`), and every stream holds its own pair, so
the table outlives every stream holding a reservation in it, also
through a failed start's cleanup and a removal with streams retained. A
stop and start, or a removal and arrival, builds new pairs and a new
table from nothing (28.5, invariant 4); a stream of the old pairs gives
its reservation back to the old table.

**Storage.** The table at 28.7's default bounds is 65,068 bytes
(28.18); stage 2 adds, per endpoint, `Set[e]` (`UAC_ROUTE_SET`, 136
bytes), the owner token, the protection status and the counters of
28.23: about 66 KiB at the default bounds, and each capture graph 7424
bytes; all of it nonpaged, allocated at PASSIVE with no spin lock held,
never a kernel stack local. How large the table is made, and whether it
is made on every device that captures, is question S3 (28.25).

**The work under the lock.** Because `Set[e]` is computed at start,
nothing done under `RouteLock` scans routes or sorts. `Set[e]` holds at
most 128 distinct entries; the visits a hold makes are counted apart
from that: a reservation makes two passes over `Set[e]` (the anomaly
preflight that looks for a `Writer`, then the registration: a count
incremented and a bit set), so at most 256 entry visits; a give-back
one pass, at most 128; the record's route fields a copy of a few
counters and one pass for `RouteReaders` (the OR of the entries'
masks), at most 128. So at most 256 entry visits a hold, independent of
the routes' number and depth; host tests count every visit of every
pass at the worst size (28.24). That bound is stated, not measured on
the target systems; at a few hundred simple updates it is not expected
to matter for 98 SE's interrupt latency, and the testing record states
the visits actually made on each leg's device.

**Stack and startup.** The walk's own frames are stage 1's (28.18):
the up to four walks run one after another, not nested, so the walk's
depth is unchanged, but the start does up to three more walks, a
`UacRouteBuild`, and up to three more graph allocations than stage 1,
and the frames of `Discover`, `UacRouteBuild` and `UacRouteFromGraph`
are new or larger. So neither the start path's total stack nor its
time is claimed unchanged: the code worker reads the new functions'
frames from the compiler listings (x86 and x64, the drivers' flags) into
the stage's testing record; whether 98 SE's and ME's kernel stack at a
PnP start has room stays open (open-items), and the X4 and ALC4080 legs
do not close it.

**The lock.** `RouteLock`, a `KSPIN_LOCK` in pair 0, initialised in the
constructor of every pair (only pair 0's is used). It is the innermost
lock (28.7): taken under the pair's operation mutex or with no driver
lock held, never across a wait, a registry write, a trace output or a
device request (each of those comes after the hold). It is never held
together with the level spin lock, the gate lock (`g->Lock`, 29.5.2),
a stream's `s->Lock`, the claim lock (`d->ClaimLock`, 29.5.2 as built)
or the device mutex, and nothing takes any of them inside it. Every
function called under it (the whole of `uacroute.c`) and the driver code
that holds it are nonpaged: neither is moved into the `PAGE` section.
28.7's event that every `Writer` clearing sets is not added in stage 2
(no writer exists; 28.21).

### 28.21 Stage 2: the ownership protocol

**The serialisation already there.** The driver already admits at most
one stream per capture endpoint at a time: `UacStream::Create` takes the
direction's one-stream gate (`d->Active`, `GateFree` to `GateOpen`)
before it allocates anything, and the gate becomes free again only at
the end of that stream's destroy, after its `Close` hook has returned
(`~UacStream`: the hook, then `GateFree`); every failure inside `Create`
after it has acquired the gate frees that invocation's gate itself and
publishes no hook, and a gate-busy failure leaves the other stream's
gate alone. And `NewStream` calls
`Create` with the pair's operation mutex held, the mutex `Close` also
holds. Stage 2 takes the reservation **inside that serialisation**
instead of adding a guard of its own:

- **Who owns the endpoint's reservation at each instant.** Nobody, until
  a `NewStream` invocation's `Create` succeeds. From that success to the
  end of the same operation-mutex hold, the invocation, which then holds
  the gate; it reserves (`UacRouteOpenEp`, one hold) and, in the same
  hold, records the new stream as the owner (the table's `Owner[e]`, the
  `UacStream` pointer `Create` returned, which is the pointer its `Close`
  hook later receives). From `NewStream`'s return, that stream, until its
  `Close` gives the reservation back in one hold under the operation
  mutex. Then nobody, before the gate is freed. **The boundary, exactly:**
  the reservation is registered before the successful open releases the
  operation mutex (between `Create`'s success and that release the
  stream exists unregistered, which is harmless: `Run` takes the same
  mutex, so nothing records in that interval, and no writer exists in
  stage 2); it is kept through every pause, stop, drain and `Shutdown`;
  and it is released in `Close`, after `Drain`, before the destroy frees
  the gate. While registered, its owner is the stream that holds the
  gate. The exceptions are stated, not hidden: with no table nothing is
  registered, and after an anomaly (below) a stream may run unregistered
  or a stale owner token may remain, both under the latch. The token is
  compared, never dereferenced.
- **A second opener** of the same endpoint meets the gate, never the
  reservation: while a stream exists (open, paused, or closing with its
  destroy queued), its `Create` fails as it does today
  (`STATUS_DEVICE_BUSY`, after `NewStream`'s `WaitClosing` of section 27)
  and it reserves nothing and gives nothing back; once the old `Close`
  has given back and the gate is free, its `Create` can succeed and it
  reserves as the first did. An opener that fails before or inside
  `Create` (the failed check, `WaitClosing`, `TakeOperation`, the
  readiness check, any `Create` failure) never reserved anything. There
  is no pending opener holding a reservation, so no ownership transfer
  between openers and no retry is needed.
- **Give-back is authorised per invocation.** `UacRouteCloseEp(t, e,
  owner)` releases the reservation only when `Owner[e]` is that owner, and
  then clears it; any other call changes nothing (counted as an anomaly,
  28.22). Only `Close` calls it, with its own stream; `NewStream` never
  gives back, since nothing can fail after it reserves.
- **`UacRouteOpenEp(t, e, owner)`** reserves `Set[e]` and sets
  `Owner[e]` in one hold; it answers `UAC_ROUTE_OK`. It reserves nothing
  and answers an anomaly (`UAC_ROUTE_ANOMALY`, new) when `Owner[e]` is
  already set or a `Writer` is set on an entry of `Set[e]`: the first is
  impossible while the gate holds (one stream per endpoint), the second
  while no writer exists (stage 2); either sets the latch (28.22).

**`NewStream`, capture pin**, in this order; every step but 6 is
today's, unchanged:

1. The failed device refuses at once (`STATUS_DEVICE_NOT_READY`, 29.9).
2. `WaitClosing` for the previous stream's destroy.
3. The operation mutex (`TakeOperation`).
4. The readiness check (`removed`, `pnpBlocked`, `powerBlocked`, the
   failed flag).
5. `UacStream::Create`.
6. **New:** if step 5 succeeded and a table exists, one hold of pair 0's
   `RouteLock`: `UacRouteOpenEp(t, k, stream)`. Its answer never changes
   the status `NewStream` returns: on `UAC_ROUTE_ANOMALY` the stream goes
   on unreserved, the anomaly counted and the latch set. With no table,
   nothing. Then, outside the hold and still under the operation mutex,
   the record (28.23) is filled and written beside the records step 5's
   end already writes (`Record`, `RecordStreams`), only while not
   `removed`; on a failed step 5 the record is written as well, as those
   are, so its counters are current.
7. The operation mutex released; the status of steps 1 to 5 returned.

**Status precedence** is therefore today's: the failed check, then
`WaitClosing`'s gate-aware end, then `TakeOperation`, then readiness,
then `Create`; the reservation adds no status.

**`Close` (the stream's destroy, PASSIVE)**, capture direction, under
the operation mutex: `Drain(d, TRUE)` and `d->Closed++` as today; then
one hold of `RouteLock`: `UacRouteCloseEp(t, k, stream)`; then the
record, while not `removed`. The table reached is always the stream's
own pair's pair 0 (`primary`, or the pair itself when it is pair 0),
never looked up afresh through the device extension, so a stream of an
old start gives back to the table it took from. `Close` succeeds on a
failed device (29.9's table) and gives back there too.

**Where this departs from 28.7, and what stage 3 inherits.** 28.7 put
the open's registration before the operation mutex and before `Create`,
so that an open could wait for a writer without holding the mutex the
writer needs; a registration there is outside the gate's serialisation,
and its pending state then needs ownership rules of its own (an opener
that reserved, then lost the gate to another, or failed after it). Stage
2 has no writer, so it takes the reservation after `Create`, where the
gate already gives one owner. Consequences, binding on stages 3 and 4:

- An open never waits for a writer in stage 2 (none exists), and 28.7's
  wait (the event, the 2 s) is not built. Stage 3, which adds the first
  writer, must specify where an open of endpoint k meets one: if under
  pair k's operation mutex, then every `Writer` mark must be set and
  cleared only while its writer holds its own pair's operation mutex,
  so that no mark of pair k's own exists while an open of pair k holds
  that mutex, and a writer of another pair must never wait for pair k's
  mutex while marked; or another placement with its own ownership
  protocol. Stage 3 decides it and its tests show it.
- 28.7's switch rule "this endpoint is: its own stream open, or
  opening, since an open registers before its create" then reads "its
  own stream open": an own switch and an own open cannot interleave,
  both running under pair k's operation mutex, provided stage 4 takes
  its switch reservation (`UacRouteSwitchBegin`) under that mutex.
  `routetest`'s existing model of 28.7's order stays as a model; stage 4
  reconciles it.
- 28.7's "A create that then fails gives the set back at once" has no
  case left: nothing is reserved before `Create` succeeds.

**Against the rest of the driver.**

- **The gate and the failed state (29.5.5, 29.9).** A failed device is
  refused at step 1, 3 or 4 and reserves nothing. A stream open at the
  failure keeps its reservation until its `Close`, which gives it back.
  Stage 2 sends nothing, so the gate's admission has nothing of it to
  refuse. On 98 SE and ME the gate is inactive (29.17); nothing of the
  protocol depends on it.
- **The level engine.** Untouched: stage 2 does not call `uaclevel.c`,
  `Prepare`'s first apply (28.15's exclusive startup) comes before
  `Discover` as today, and the level work item and level SETs take no
  route lock; the level spin lock and `RouteLock` are never held
  together (28.7).
- **Power.** `PowerChangeState` and `PowerLocal` do not touch the
  table: a stream drained below D0 keeps its reservation, as a pause
  does; an open below D0 is refused at step 4 and reserves nothing.
- **PnP.** `Shutdown` does not touch the table: it drains and destroys
  the transports, and each stream's `Close` gives its reservation back
  later, when PortCls releases the stream, sending nothing. The next
  start's table is new and empty; a stream of the old pairs is neither
  registered in it nor able to record (its transport is gone).

### 28.22 Stage 2: incomplete protection, exhaustion and failures

28.7's integration contract decides the principle: **exhaustion
disables, never weakens**; recording goes on. Stage 2 extends it to
discovery that cannot list every control, because a control missing
from one endpoint's set is unprotected against another endpoint's
automatic write even when that endpoint's own writes are off (28.3).
In stage 2 nothing is yet disabled; the table carries the **latch**
that stages 3 and 4 obey (scope: question S4, 28.25), and no open
changes its outcome.

**An endpoint's protection is incomplete** when any of:

- its graph could not be allocated (`GraphFlags` bit 7);
- its graph is incomplete by 28.3 (`UAC_CAP_INCOMPLETE`: more than 64
  routes, of which only 64 are stored; a route deeper than 32; a loop; a
  malformed, missing or unlisted entity): the stored routes are entered,
  but a cut or omitted route may hold controls the set lacks;
- a Feature Unit on a stored route has a mute or volume control on a
  logical channel above `UAC_VOL_CHANNELS`, which `UacRouteFromGraph`
  does not enter (it reports such an omission, new);
- its routes do not fit (`UacRouteFromGraph` 0: the table's 128 controls,
  a route's 48, or the endpoint's routes), and it is then left with no
  route at all.

`GraphFlags` bits 4 (more than 8 sources) and 5 (chooser held by another
pair) do not make protection incomplete: every route is still stored.
**A complete graph with no control** (no Feature Unit control and no
Choice on any route) is complete protection with an empty set: its
reservation is empty and correct, and sets no latch.

**The latch** is set, once and until the next start, when any capture
endpoint built at start has incomplete protection, when the table could
not be allocated, or when an anomaly occurs (28.21). It is the table's
(`Latched`), read under `RouteLock`; with no table it is implied.

| Case | When | What stage 2 does | Recorded (28.23) |
|---|---|---|---|
| A graph's allocation fails | pair 0's `Discover` | that endpoint: no graph (stage 1's record: `RouteState` 3, `GraphFlags` bit 7), no route, empty set; latch | `RouteFlags` bits 1 and 2 on that endpoint, bits 4 and 2 on the others |
| A graph incomplete, or a channel above 8 omitted | pair 0's `Discover` | stored routes entered, set as computed; latch | the same |
| An endpoint's routes do not fit | pair 0's `Discover` | that endpoint: no route, empty set; endpoints entered before it keep theirs; latch | the same |
| The table's allocation fails | pair 0's `Discover` | no table: no stream reserves, every open as today; latch implied | bits 0 and 2 |
| An anomaly: `Owner[e]` already set, or a writer found, at a reservation; or a give-back by a stream that is not the owner | `NewStream` step 6; `Close` | nothing reserved or released; the stream records; latch | `RouteAnomalies`, bits 3 and 2 |
| A render-only adapter | pair 0's `Discover` | nothing allocated; no latch (nothing captures) | no record (no capture line) |

None of them fails the PnP start or changes what `Prepare` returns
(stage 1's rule); nothing is allocated at an open or a close. A failed
registry write of the record is not retried (diagnostics only, as in
stage 1).

### 28.23 Stage 2: diagnostics and registry values

**Two contracts, kept apart.** `UacCapRecord` (and `UacCapStart`)
write exactly 412 bytes, stage 1's layout, into a buffer at least that
long and touch nothing past it. `UacRouteDiag(t, e, record)` writes
exactly offsets 400 to 403 and 412 to 439 of a buffer at least 440 bytes
long and touches nothing else. The driver's buffer (`capDiag`) grows to
440 bytes, zeroed in the constructor, filled by both, and written as
**`Uac2VolMapC` / `Uac2VolMapCnn` of 440 bytes**, append-only (28.12:
"a shorter value is an older build's"); every stage 1 field keeps its
offset and meaning. No reader in this tree requires exactly 412 bytes
(the tests encode the size and change with it); a reader elsewhere that
does is not known and not established either way.

| Offset | Field | Bounds |
|---|---|---|
| 400 | `RouteReaders` (28.12): endpoints, of any pair, holding a reservation that includes a control of this endpoint's set, this one included | 0..`UAC_ROUTE_ENDPOINTS` |
| 412 | `RouteControls`: entries in the adapter's table | 0..128 |
| 416 | `RouteRoutes`: this endpoint's stored routes in the table | 0..64 |
| 420 | `RouteLocked`: entries in this endpoint's set while it holds its reservation, else 0 | 0..128 |
| 424 | `RouteOpens`: reservations taken | counter |
| 428 | `RouteCloses`: reservations given back | counter |
| 432 | `RouteAnomalies`: reservations refused or give-backs ignored as anomalies (28.21); 0 expected | counter |
| 436 | `RouteFlags`: bit 0 no table; bit 1 this endpoint's protection incomplete; bit 2 the latch; bit 3 an anomaly on this endpoint; bit 4 another endpoint's protection incomplete; bits 5 to 31 zero | bits |

With no table, `UacRouteDiag(NULL, e, record)` writes 0 at 400 and at
412 to 435 and `RouteFlags` 5 (bits 0 and 2). `UacRouteInit` zeroes
every counter, flag, owner and set; `UacRouteBuild` sets the protection
bits and `RouteRoutes`, `RouteControls` and the latch, and nothing else.
The record's route fields are copied in one hold, so a reservation is
never half counted in it. **Written** at start (each pair's `Discover`,
when that pair has capture formats), at each capture `NewStream` that
reaches step 5 (28.21, step 6) and at each capture `Close`, by that pair,
under its operation mutex, outside `RouteLock`, and only while not
`removed`. Another pair's record is not rewritten when this pair opens:
its `RouteReaders` is as of its own last write.

**Trace builds** gain three events, `rtopen` (pair, answer, set size)
and `rtclose` (pair, answer, `RouteReaders` after), sent after the hold,
and `rtrefuse` (pair, the step of 28.21 that refused, the status), sent
by a capture `NewStream` that returns a failure at its step 1, 3 or 4
(the failed check, `TakeOperation`, the readiness check), holding no
lock; each as
`lvset` is traced today; they reach QEMU's debug console (port `0xE9`)
whether or not the device is still present, so they show a give-back
that no record can after a removal.

**Registry values involved in stage 2:**

| Value | Written by | Read by | Stage 2 |
|---|---|---|---|
| `Uac2VolMapC`, `Uac2VolMapCnn` | the driver | nothing (diagnostics) | 440 bytes; written as above |
| `Uac2FailOnTimeout` (29.17) | a test only; no INF | the driver at PnP start | unchanged; its gate decides steps 1 to 4 of 28.21 as today |
| `Uac2Streams`, `Uac2Waits` (section 27) | the driver | nothing | unchanged |
| `Uac2CapFirst`, `Uac2LevelC`, `Uac2LevelCnn`, the 46 `MediaCategories` names (28.12) | - | - | not written, not read, not in the INFs (stages 3 and 4) |
| `Uac2VolMap`, `Uac2UsbStats`, `Uac2UsbStatsIn` | the driver | nothing | unchanged in size and content |

**No new option.** Stage 2 supports no setting a user could choose: the
reservation has no effect a user can see until a later stage writes, and
its bounds are constants, not options; the standing rule (every option
configurable in Device Manager, written by the INF, documented) does not
make an internal mechanism or a capacity a setting. So `uac2wdm.inf` and
`uac1wdm.inf` are unchanged and `infchk` needs no new case. Whether
stage 2 should nonetheless be behind a setting is question S2 (28.25).
Stage 2 adds no user-visible string.

**The stats words, for the first stage that adds any.** 28.12 gives
section 28 `Uac2UsbStats` words 94 to 99 and says the stats "grow from
94 to 100 words"; words 94 to 97 are already in use (`uacusb.h`,
`UAC_USB_STATS`: 25.7's `FeedbackOff`, 26.13's `Resyncs`, `ResyncFrame`
and `ResyncBehind`). Stage 2 adds no word and keeps 94 to 97 as they
are; the layout is settled before the first stage that adds words
(29.11, O12).

### 28.24 Stage 2: tests and legs before merge

**Host modules.** In `uacroute.c` / `uacroute.h`: in `UAC_ROUTE_TABLE`,
per endpoint `Set[e]`, `Owner[e]` (an opaque pointer-sized token), the
protection status, the counters of 28.23, and the table's `Latched`;
`UacRouteBuild`, `UacRouteOpenEp`, `UacRouteCloseEp`, `UacRouteReaders`,
`UacRouteDiag`; `UAC_ROUTE_ANOMALY`; `UacRouteFromGraph`'s report of
controls above `UAC_VOL_CHANNELS`. In `uaccap.c`: `UacCapRecord`. The
headers' notes that no driver file calls them yet (`uacroute.h`;
`uaccap.h`'s "the driver calls `UacCapStart` only") are updated.
`uacroute.c` joins both drivers' builds (`build.cmd` of `uac2` and
`uac1`, compiled and linked as `uaccap.c` is) and `uac1`'s `test.cmd`
(`routetest` built with `UAC_V1`).

**Host tests**, each written to fail on the code before the change it
tests:

- **Family fixtures.** Descriptor-dependent vectors use each family's
  own descriptors: `uac2` the recorded X4 (`testdata/sbx4`) and ALC4080
  (`testdata/alc4080`) and built UAC 2.0 descriptors; `uac1` the recorded
  Logi headset, Play! 2 and Brio 100 (`uac1/testdata`) and built UAC 1.0
  descriptors, including a UAC 1.0 pair of capture paths sharing a unit.
  The table-only schedules below use tables built directly, so they run
  unchanged in both builds.
- **The lifecycle model** (`routetest`, both families): a model of the
  driver's order in 28.21, with the direction's gate (`GateFree`,
  `GateOpen`, `GateClosing`) and the operation mutex as actors' steps,
  and the reservation calls. Deterministic schedules, each checked after
  every step against 28.21's boundary: the reservation's entries carry
  the endpoint's bit and count exactly from the registering hold (before
  the successful open releases the operation mutex) to the give-back
  hold in `Close` (before the gate is freed), and the owner in that span
  is the stream holding the gate; between `Create`'s success and that
  hold the stream is unregistered and no `Run` step can occur; nothing is
  released by a non-owner; returned statuses are the exact ones 28.21
  lists for each step; and at the end every count, mask and owner is
  clear:
  - two openers of one endpoint at once, each order through the gate
    (one creates and owns; the other fails busy and reserves and releases
    nothing);
  - an opener while the previous stream closes: before its `Close`, between
    its give-back and the gate's freeing (the opener fails busy), and
    after (it creates and owns);
  - the two interleavings that defeated the first draft of this design:
    an opener reserving before the operation mutex while a second gets
    the mutex first and creates; and an opener meeting an old reservation
    that is then given back before its `Create`. Run against the order of
    28.21 they cannot leave a stream unreserved or a reservation
    released under it; run against that draft's order (counted apart) they
    do, so the vector shows the order matters;
  - every failure an opener can meet (the failed check, `WaitClosing`'s
    gate-aware end, `TakeOperation` refused, each readiness refusal, each
    `Create` failure: format, gate busy, stream, work item, buffer and
    service-group allocation) followed by a second opener that succeeds:
    no reservation taken by the failed one, the second owns;
  - a `Close` by a stream that does not own (anomaly, nothing released,
    latch), and `UacRouteOpenEp` with the owner set or a writer marked
    (anomaly, nothing reserved, latch);
  - two endpoints sharing a control: reservations and give-backs in
    every order, the shared entry's count and both bits right throughout;
    `RouteReaders` 2 on each while both hold, 1 after either gives back;
  - **a retained owner across a PnP stop and start**: a stream owns its
    reservation; `Shutdown` drains it (the reservation kept, the old table
    and its pair 0 kept alive by the stream's references); a new start
    builds a new pair 0 and a new table while the old owner survives; an
    opener on the new start creates and owns in the new table; the old
    stream's `Close` then gives back in the old table only (the new
    table's counts, masks and owner unchanged), after which the old table
    has no reference left and is freed exactly once; the same with the
    new opener's close coming first;
  - **a failed device and its recovery**: with the failure latched, an
    opener is refused at each place 28.21 refuses it (step 1;
    `TakeOperation`, including after `WaitClosing` ended early on the
    failure; the readiness check), with exactly `STATUS_DEVICE_NOT_READY`, reserving nothing; a
    second opener is refused the same while the failure stays latched,
    whatever the gate's state; an open stream's `Close` still gives back
    (owner matched); and an opener succeeds again only after a new PnP
    start (`Start`'s `UacGateReset` clears the failure; new pairs and a
    new table), owning in the new table, while a stream of the failed
    start, if retained, gives back to the old one;
  - **partial construction and failed-start cleanup**: the graph
    allocation failing for endpoint 0, 1 or 2 of three; the table
    allocation failing after the graphs were built; the table filling at
    each endpoint; a later pair's own `Prepare` failing after pair 0
    built its graph; a latched start torn down after pair 0, and after a
    later pair (29.17's startup latch); each with the model counting the
    allocations of graphs and table: every allocation freed exactly once,
    when pair 0's last reference goes, none freed by a later pair, and
    the latch and flags as 28.22 gives them for what was built.
- **The build** (`routetest`, both families, each family's fixtures):
  every endpoint's set from the recorded devices, compared with constants
  fixed from the fixtures when the test is written; a NULL graph; a
  table too small (smaller `UAC_ROUTE_CONTROLS`): the overflowing
  endpoint empty, those before it kept, the latch; an incomplete graph
  of each kind (more than 64 routes stored as 64; a route deeper than 32;
  a loop beside a valid branch; a malformed and an unlisted entity),
  latch set; **omitted shared controls**: two endpoints sharing a Feature
  Unit control that one endpoint's graph omits (it lies beyond that
  graph's 64th route, beyond its depth bound, behind its malformed entity,
  or on a channel above 8), checked to be missing from that endpoint's
  set and the latch set; a unit with controls on a ninth channel; a
  complete graph with no control (empty set, no latch); an adapter with
  no capture endpoint (nothing built, no latch); a pair 0 without
  capture formats whose later pair captures (the table built, pair 0 no
  record).
- **The work under the lock**: a table at the worst size (128 controls,
  one endpoint's set of 128) reserved, read for its record and released,
  the instrumentation counting every entry visit of every pass in each
  hold: at most 256 for a reservation, 128 for a give-back or a record,
  and no route scanned.
  Also built at the drivers' own sizes (`UAC_ROUTE_*` as the drivers
  compile them) besides the small model sizes.
- **The records** (`captest` and `routetest`, both families): each
  written into a buffer filled with a canary byte beyond and around the
  bytes it owns: `UacCapRecord` changes only bytes 0 to 411 and equals
  `UacCapStart`'s 412 bytes for every stage 1 fixture and for graphs built
  for a later pair's terminal (the ALC4080's second and third capture
  terminals); `UacRouteDiag` changes only 400 to 403 and 412 to 439, with
  the expected bytes written out literally in the test (not computed by
  the code under test), for no table, each fixture after start, while
  reserved, after give-back, after an anomaly, and with each flag.
- **Mutations**, each to fail a test in both families: the owner check of
  `UacRouteCloseEp` dropped; `UacRouteOpenEp` reserving with an owner
  already set; a give-back counted twice; `UacRouteReaders` counting its
  own endpoint twice or missing a shared control; `UacRouteBuild`
  ignoring an incomplete graph, a channel omission or a
  `UacRouteFromGraph` 0 (no latch); the set computed from one route only;
  each `UacRouteDiag` field shifted by 4 bytes; `UacCapRecord` writing a
  byte past 411.
- **Gates**, as stage 1's: `uac2`'s and `uac1`'s `test.cmd`, `streamtest`,
  `uacsimtest.py`, `uac1simtest.py`, `testgate.ps1`; the 16 builds
  (`uac2` `wdmx86` and `wdm64` free, checked and trace; `uac1` `wdmx86`
  free and checked; each with the default level engine and with
  `UAC_LEVEL=retained`), every import list unchanged; and the object
  comparison of stage 1's record: every object but the drivers' own,
  `uaccap.obj` and the new `uacroute.obj` byte for byte the same.

What the host cannot show: the driver's own code in `NewStream`,
`Close`, `Discover` and `~Wave` (`uacwdm.cpp`) is not run by any host
test; the model shows the protocol, and the code is shown to follow it by
reading and by the legs below.

**Guest legs before merge** (QEMU, at most two guests at once, fresh
overlays of the installed bases; each recorded with its result in a
dated record `docs/testing/<date>-uac2-cap-stage2.md`, a failure or a leg
not run named as such, together with the frames and the hold sizes of
28.20). Image A: main's at the branch point; image B: stage 2's; image
B': stage 2's trace build. Unless said, `uac2` with `uacemu.py
--profile x4full`, `UACWAV rec` and `play` as stage 1's leg:

1. **XP SP3 x86, Windows' EHCI, A against B**: the simulator's control
   log the same request for request (stage 2 sends nothing); playback
   and recording pass with the same figures; `Uac2VolMapC` 440 bytes:
   after the plug, stage 1's fields as before and the route fields as
   the X4 vector; read during a 10 s recording: `RouteOpens` 1,
   `RouteLocked` as the vector, `RouteReaders` 1; after it: `RouteCloses`
   1, `RouteLocked` 0, `RouteReaders` 0, `RouteAnomalies` 0.
2. **XP SP3 x86 under xHCI98**: leg 1 with image B.
3. **98 SE under NUSB's EHCI, and under xHCI98**: leg 1 with image B for
   `uac2`; `uac1` with `uac1emu.py --profile hs` (High Speed: the EHCI
   or xHCI path itself), and with `logi` and `play2` (Full Speed:
   under NUSB's EHCI these run through the companion controller, and the
   record says so; they are not an EHCI leg); with them, stage 1's owed
   checks on these binaries (the record written by 98 SE's registry, no
   fault at arrival, replug and removal).
4. **Two capture pairs at once** (XP, Windows' EHCI, `alc4080cap`, whose
   three capture inputs are three pairs, as recorded in 15.3a's XP runs):
   two inputs recording at once from two `UACWAV` processes; each pair's
   record against the ALC4080 vectors; closes balanced. A shared control
   across pairs is shown on the host only; `twosel` (28.16) belongs to
   stage 4's cross-pair legs.
5. **Removal while recording** (XP, Windows' EHCI; and 98 SE under
   NUSB's EHCI; image B'): the simulator stopped mid-recording; no fault;
   the give-back seen as `rtclose` on the debug console; after a replug
   the new record starts at `RouteOpens` 0 and a recording passes.
6. **The failed state** (XP, Windows' EHCI, image B', `Uac2FailOnTimeout`
   1, `--stop-after-s` with a recording running): after the stop, a
   device request is made while the recording stays open (`UACWAV ramp`
   on the playback line, whose volume SETs are class requests), and the
   expiry is confirmed (`Uac2Failed` written) before anything else.
   Then a new capture open is attempted and what `UACWAV` reports is
   recorded; after the failure Windows may already be taking the device
   down (29.9), so the attempt need not reach `NewStream`, and neither its
   result nor the absence of an `rtopen` shows `NewStream`'s status. It
   counts as reaching the driver only if an `rtrefuse` event (28.23)
   shows it, and then that event's status is the evidence. The running
   stream's close shows its `rtclose` (answer OK, owner matched). The
   record cannot be relied on here; the debug console is the evidence, and
   without it the leg does not show the give-back and says so. The exact
   refusal status at each step is the host model's to show (the failed
   device schedule above), not this leg's.
7. **7 x64, Windows' EHCI**: leg 1's recording with the x64 image B.
8. **Opportunistic**, not a gate: 200 cycles of a short recording in a
   loop (XP, Windows' EHCI): no fault, `RouteOpens` equal to
   `RouteCloses` and `RouteAnomalies` 0 at the end. It does not force
   either ordering of 28.21; the host model does.

**Not shown, and why.** A PnP stop and start with a capture stream
retained (the old stream's give-back into the old table after the new
start): no leg here can force a PnP stop while a recording is open (a
disable or restart goes through a query that may be vetoed), so the
driver's integration of it is left unverified in guests; the protocol is
shown by the host model's retained-owner schedule (above) and the code by
reading. A power
transition while recording (no standby rig for these guests is recorded
here; the reservation's keeping across D3 rests on reading 28.21).
Discovery at the depth and route bounds in a guest (no profile reaches
them; host only), and so the 98 SE and ME stack-room item (open-items).
ME, 2000 and Vista (expected to follow 98 SE, XP and 7; not shown). The
retained images in a guest.

### 28.25 Stage 2: questions for the owner

The rest of 28.19 to 28.24 follows 28.3, 28.5, 28.7, 28.15 and 28.18,
departs from 28.7 where 28.21 says why, or is the code's own detail.
Four choices are open; each user-visible consequence is stated, and none
is decided here.

- **S1. Stage 2's gate and stage 1's owed legs.** Stage 1 is on main as
  a development step with its 98 SE legs owed (its record, "After the
  merge with main"); 28.18 says each stage has its own gate before the
  next. AGENTS.md distinguishes a development merge from readiness: no
  option below makes either stage ready, which needs actual 98 SE passes
  under both NUSB's EHCI and xHCI98.
  1. Stage 2 is merged only after stage 1's owed legs have run and
     passed on main's binaries, and then after 28.24's legs on its own.
     The user meets nothing of either stage before a release; the cost is
     one more round of guest sessions before stage 2 lands.
  2. Stage 2's legs (28.24) carry stage 1's owed checks on stage 2's
     binaries, and stage 1 counts as shown only through them. The user
     meets the same; one round of sessions; a failure there leaves both
     stages unshown until it is found.
  3. Stage 2 is merged as stage 1 was, as a development step: host-tested
     and 28.24's leg 1 only. This **replaces 28.24's guest gate**: legs 2
     to 7 and stage 1's owed checks become owed for both stages. The user
     meets nothing in a release, but main then holds two unshown steps on
     the start and open paths on 98 SE, which a release from main would
     carry until those legs pass.

  Recommended: 2.
  **Decided (owner, 2026-11-18): 2.**
- **S2. A setting for stage 2.**
  1. None: always on, as stage 1. The user meets nothing new and no new
     entry in Device Manager.
  2. A Device Manager setting the INFs write without overwriting (draft
     name `Uac2CapRoute`, default 1), documented in the README's settings
     list. At 0 no reservation is taken and the table is treated as
     latched, so its disabled behaviour is the latch's, in the scope S4
     chooses (with S4's option 1, also no D2 normalisation): stages 3
     and 4 make no automatic capture write and no switch on that device:
     no first-arrival pass, no restored recording level or source, and no
     input chooser that moves a selector; only the user's own level SETs
     reach the device. The user meets one more
     advanced setting, useful to rule the mechanism out when a fault is
     diagnosed, which at 0 turns off those later features with it.
  3. A test-only value with no INF line, off unless a test writes it,
     until stage 4 ships. The user meets nothing until then. It sets the
     standing rule on options aside for the staging period, which needs a
     fresh, explicit exception from the owner: the one given for the
     failed state's staging (29.17) does not extend to this.

  Recommended: 1.
  **Decided (owner, 2026-11-18): 1, none.**
- **S3. The table's storage.** 28.7 states about 64 KiB at the default
  bounds, from nonpaged pool, on every device with a capture line. A
  device with no capture endpoint allocates nothing in any option
  (28.20, decided by the design: nothing can record there). The choices
  below are not exclusive: 3 can be combined with 1 or 2.
  1. As stated: 8 endpoints, 128 controls, 64 routes each: about 66 KiB
     with stage 2's additions, plus 7424 bytes per capture graph, on every
     device with a capture line.
  2. The same table with its endpoints bounded by the drivers' four
     pairs (`UAC_ROUTE_ENDPOINTS` 4 in the drivers' builds, and a host
     build at that size): about 35 KiB plus the graphs; nothing is lost
     today, since a driver has at most four capture endpoints;
     `RouteReaders` then reads 0 to 4.
  3. No table where nothing can be arbitrated (one capture endpoint with
     complete protection, no Choice on its graph, no capture control on
     a playback path): no table on a device such as the recorded Logi
     headset (28.4's examples); stages 3 and 4 must then tell "no table,
     nothing to arbitrate" from "no table, allocation failed" (the latter
     latched), a second state to carry and test.

  The user meets the nonpaged memory the device takes while it is
  present (about 74 KiB with 1, about 42 KiB with 2, and on a simple
  device about 7 KiB with 3).

  Recommended: 2, without 3.
  **Decided (owner, 2026-11-18): 2, without 3.**
- **S4. What the latch turns off (28.22).** Stage 2 only records it;
  stages 3 and 4 obey it. It is set when any capture endpoint's
  protection is incomplete (a malformed or very large descriptor, a unit
  with more than 8 channels), when the table could not be allocated, or
  on an anomaly.
  1. Every automatic write the table arbitrates, on the whole adapter:
     switches, a switch's level writes, restored recording levels,
     first-arrival passes, and playback normalisation (D2, 28.9). The
     user meets, on such a device only, no automatic recording level,
     source or pass and no D2 normalisation of other playback units:
     levels as the device keeps them, as before section 28; the user's
     own level SETs still apply.
  2. Every automatic capture write and every switch on the adapter, but
     playback normalisation continues on controls on no stored capture
     route. The user meets D2 kept on such a device; but a normalisation
     write may then reach a control a recording passes through that
     discovery could not list.
  3. Only the incomplete endpoint's own writes, as 28.3 already rules
     for it, and no adapter-wide latch. The user meets the most features
     kept; another endpoint's automatic write may reach an unlisted
     control under a recording, the case 28.7's contract exists to
     exclude.

  Recommended: 1.
  **Decided (owner, 2026-11-18): 1.**

## 29. The alternate kept after a stop, the hold, and a device that stops answering (2026-11-17)

Status: design, 2026-11-17; revised the same day (the lifetime of the
deferred work, the locking of the failure, the restart of a kept pipe,
power and PnP, the settings, the evidence wording), and again (the
threads' rundown, which is open on 98 SE and ME, O14; a notifier of its own; conflict discovery in
the clock walk; admission across waits and the start's commit). The
pure state machine of 29.12 is written and host-tested on its own; of
the driver code only the failed state is integrated so far, as 29.17
says: **default off, on Windows 2000 and later only, and not complete**
(the owner-selected event log entry and README text are not implemented,
so the feature is not to be enabled or called finished until they are;
on 98 SE and ME nothing of it is active). The section is numbered 29 because 28,
the recording-controls work, was written beside it; both are on main
now (of 28, the level engine's move and stage 1, 28.15 and 28.18; of
this section, what 29.17 says). This
section does not change 28 and lists where the two meet (29.11). Applies to `uac2` and
`uac1` alike: the stream start and stop (`UacUsbStart`, `StopLocked`,
`SelectAlt` in `uacusb.c`) and the pin hooks (`Run`, `Halt`, `Close`,
the power and PnP paths in `uacwdm.cpp`) are the shared core; `uac1`'s
only difference on this path is its endpoint rate request after the
alternate (`EndpointRate`, 26.4), which a kept alternate skips too
(29.4.9).

The owner's decisions (2026-11-17):

- **D1, the linger, about 2 s:** after a stream stops or its pin closes,
  the alternate setting (and the clock claim) stays selected for about
  2 s and the switch to alternate 0 is deferred, so that a start of the
  same format inside that time sends nothing.
- **D2, a minimum hold, about 200 ms:** never switch an interface back
  to alternate 0 sooner than about 200 ms after an alternate was
  selected. Whether the hold also spaces a selection after alternate 0
  (both directions) is not decided (O2).
- **D3, a clean failed state:** when the device stops answering (a
  control request is not answered in time), stop sending to it, fail
  opens at once, tell Windows the device has failed, record it in the
  driver key, and tell the user to unplug and replug it.

Not decided, and written here with a provisional choice each (29.16
puts them to the owner, recommendation first): the hold in both
directions (O2); the probe off by default and on only in a test build
(O8); a stream running at the failure stopped (O5); the message to the
user as drafted in 29.9 (O7); the reporting user's test build staged as
in 29.15 (O13); no new thread on 98 SE and ME (O14). Everything else
marked "proposed" is open too (29.16).

Each mechanism has a documented setting (29.8); with all four at 0 a
device that answers gets the request sequence of before this section.

What this does and does not claim: it removes the rapid alternate
switching `uac2` itself adds when programs start and stop streams
quickly, and the slow failures after a device stops answering. Whether
the field devices then stop failing is not established; the device's
fault is not known (29.1, 29.2), and only the real devices can show it
(29.15).

### 29.1 The field reports

The first reports and what `uac2` sends are in
[uac2-device-stops-answering](../testing/2026-11-17-uac2-device-stops-answering.md):
a USB-C headphone adapter (USB Audio 2.0, High Speed, CX31993 with a
headphone amplifier, a synchronous OUT endpoint), Windows 7 x64, xHCI98
as host controller driver. Three times a SET_INTERFACE to alternate 0
got no completion at all; the device then never completed Address
Device again, through the host driver's port resets; only unplugging it
brought it back. In a fourth save the device answered no control request
while its isochronous stream went on being sent (the quiet form: the
user heard silence and `uac2` saw no failure). Every fatal request ended
a short stream session; `uac2` had nothing else outstanding and its
request order is correct (record, sections 1 and 2).

**The newest report (saved 2026-11-16 22:23 local).** The same reporter
reproduced it on purpose with a second adapter of theirs, by playing
Windows' system sounds as fast as they could: about one start in twenty
"sounded all weird", and then the device stopped answering. The second
adapter is gone from the save's device list (it had stopped answering),
so its chip is not read; it is taken here as another of the same chip
family, not established. Read from the Intel controller's note ring with
the scratch timeline parser of the record (text only):

- 61 stream sessions from 06:22:39 to 06:22:52 UTC, about 9 alternate
  changes a second: first 43 sessions, all but one of 37 to 50
  publications (about 120 to 170 ms of streaming each), closes 0.15 to
  0.20 s apart; then 18 sessions of 67 to 97 publications (about 240 to
  360 ms), closes 0.27 to 0.39 s apart.
- **Each session began within a few milliseconds of the previous
  session's alternate 0.** The close notes are written when the switch
  to alternate 0 has ended; in a normal session the longest gap between
  publications (about 4 ms, the first refill after the 8 transfers of the
  start) ends 3 to 10 ms after the previous session's close notes. So the
  device saw: SET_INTERFACE(0), then within a few ms `uac2`'s next start
  (by the code, not seen on the bus: the clock walk's two RANGE reads of
  the frequency, `uacusb.c`, `SourceRanges`: its RANGE request for the
  subrange count and then the one for the whole; GET_CUR of the
  rate and of its validity; GET_CUR of the mute: five requests; then
  SET_INTERFACE(1)) and the stream at once; about 0.3 s of stream;
  SET_INTERFACE(0) again.
- The fatal session had 75 publications (about 270 ms of streaming).
  Its longest gap, an ordinary 4 ms one, ended 0.289 s after the
  previous close, at the end of its stream rather than at its start;
  its close notes came 5.011 s after that, `uac2`'s 5 s request
  time-out, as in the first reports (5.012 and 5.047 s). So it most
  likely also began right after the previous close, streamed about
  0.27 s, and its SET_INTERFACE(0) at about 06:22:52.41 was the
  unanswered request (`cfg.select.end` `05010407`: slot 5, interface 1,
  not established). The host driver's three recoveries, the device's
  Address Device time-outs and the latched controller followed, as in
  the first reports.

So the patterns before the four failures were: closes 0.68 s apart in a
cluster; one session after 329 s of quiet; five 36 ms cycles (about 28
sessions a second, so about 55 alternate changes a second; the record's
"about 27 alternate changes per second" counts sessions); and now
sessions of about 0.3 s back to back for 13 s. What they share is an
alternate change close to another one: alternate 0 some 5 to 40 ms
after alternate N in the first reports, and alternate N a few ms after
alternate 0 in the newest.

### 29.2 Hypotheses, widened

Two adapters now show it, both the same reporter's, on the same host and
under the same host controller driver, and the second one's chip is not
read (29.1). So one unit's defect is less likely; neither a common chip
nor a fault independent of that host is established.

1. **The chip family's firmware** wedges its control endpoint, and then
   ignores even a bus reset, after alternate changes in quick
   succession. For: every fatal request was a SET_INTERFACE during
   rapid changes; Linux lists the maker's other adapter (3302:17c2)
   with `QUIRK_FLAG_FORCE_IFACE_RESET | QUIRK_FLAG_IFACE_DELAY` (50 ms
   after every SET_INTERFACE), and a dozen other USB-C headset adapters
   with the same pair (record, section 3). Against: dozens of identical
   changes passed each time; one failure came after 329 s of quiet.
   Confidence medium that alternate changes are the trigger, low on the
   rule.
2. **`uac2`'s own pace.** At a stop it cancels every transfer and selects
   alternate 0 at once; at a start it sends three GET_CURs and
   SET_INTERFACE(N) within a few ms of that, and the stream starts on
   the next frame. None of it breaks the USB or USB Audio rules, but it
   is a pace Linux never produces: Linux selects alternates only at
   prepare and at close, never at a trigger start or stop
   (`snd_usb_endpoint_stop`, `snd_usb_endpoint_close`,
   `endpoint_set_interface`), and its desktop sound servers keep the
   device open across system sounds. Windows' own UAC 2.0 driver
   (`usbaudio2.sys`, Windows 10 and later) is not read; what it sends
   per stream start is not known here.
3. **The host controller driver's part.** xHCI98's switch
   (`hcdCfgSelectInterface`, `src/hcd_cfg.c`, read 2026-11-17): the
   interface's pipes are aborted and detached, the new setting's pipes
   made, one Configure Endpoint drops the old endpoints and adds the
   new, then SET_INTERFACE goes out on the control pipe. Linux's
   `usb_set_interface` (`drivers/usb/core/message.c`) has the same order:
   `usb_disable_interface`, `usb_hcd_alloc_bandwidth` (which on xHCI
   issues the Configure Endpoint), then the SET_INTERFACE with a 5000 ms
   time-out. A Configure Endpoint is a command to the controller and
   nothing of it reaches a High Speed device on the bus; Windows' EHCI
   stack has no such command and only unlinks the endpoint from its
   schedule. What the device sees is the same under all three: its
   isochronous packets end (`uac2` has already cancelled them), then the
   SETUP of SET_INTERFACE. The only difference is the time between the
   two, the abort and the command under xHCI98, a frame boundary under
   EHCI, around a millisecond either way. After the wedge the device did
   not complete Address Device after port resets. Address Device is a
   command to the controller, and its failure is what the host driver
   observed; a device must come out of a bus reset in its Default state
   (USB 2.0 9.1.1, 7.1.7.5), so a failure that persists through resets
   supports a fault in the device, but does not on its own exclude a
   controller or host driver recovery problem without the EHCI
   comparison or bus evidence. The host side's order and timing may be a
   trigger; the test on Windows' own EHCI driver (29.15, U1) helps tell
   these apart, though no one observation separates every hypothesis.
4. **Power.** On a bus-powered adapter each alternate change may switch
   the codec and the headphone amplifier on or off; rapid cycling loads
   its supply, and a hang that only removing power clears fits a
   brown-out as well as a firmware fault. Low confidence; nothing in the
   saves can separate it. A kept alternate cycles them less.

**The rough playback (about one start in twenty).** With the alternate
kept, sounds inside the linger restart nothing on the device but the
packets: no SET_INTERFACE, no clock request; between two sounds it sees
only a pause in the isochronous packets at an unchanged alternate. So:

- if the roughness is the device's own start-up after an alternate
  change (codec, amplifier, its clock or data path starting out of
  step), the linger removes it for sounds inside it (hypothesis);
- if it is the device recovering from the pause in packets, the linger
  does not remove it;
- `uac2`'s side, by reading: every start computes the packet cadence
  afresh (`UacRateInitialize`; a synchronous endpoint has no feedback to
  settle), every packet carries whole frames (measured in the record's
  guest runs from the first packet), the first 32 ms are taken from the
  ring at once (17.4 item 5) and the ring is zeroed at STOP and when
  allocated (`uacstream.h`), so a start sends silence, never an earlier
  session's samples, where the client has not written yet; the data of
  cancelled transfers is dropped, never sent again. What the simulator
  can test of this side is in 29.14 (the cadence check and the received
  samples, with and without the linger); it cannot show the device's.

### 29.3 What the device hears while the alternate is kept

- **Isochronous OUT with no packets.** Isochronous transfers have no
  handshake and no retry (USB 2.0 5.6); a (micro)frame in which the host
  sends nothing to an endpoint is the same, to the device, as a lost
  packet or an underrun. No requirement to keep sending while an
  alternate is selected was found in USB 2.0 or the USB Audio 2.0
  documents. The device already hears it today, for the few ms between
  `uac2`'s cancel and its SET_INTERFACE(0), and at every underrun.
- **Per synchronisation type** (the ones `uac2` drives, 4.4 and 4.5):
  synchronous and adaptive sinks run out of data and play silence, or
  hold or repeat their last samples, as at any underrun; an asynchronous
  sink does the same and its feedback IN endpoint is not polled (an IN
  endpoint with no token sends nothing). An asynchronous source on the
  capture side is not read during the linger either: `uac2` stops the
  IN transfers as today.
- **What Linux does.** Between a trigger stop and the close or the next
  prepare, Linux leaves the interface at its alternate with no
  transfers (record, section 3: trigger start and stop only submit or
  kill URBs); `QUIRK_FLAG_IFACE_SKIP_CLOSE` leaves it there even at
  close for the devices listed. So "alternate N, no packets" for seconds
  is a state every device on Linux meets when a program stops and starts
  a stream without closing it.
- **Clicks.** A device may click when its data stops, but it hears that
  stop today too; what the linger removes is the SET_INTERFACE pair that
  follows.
- **Not a guarantee.** Keeping an alternate selected with no transfers
  is defensible for the endpoint types `uac2` drives; it does not
  guarantee a clean restart on every device. A sink that underruns plays
  silence, repeats or clicks as that device does; an unpolled capture or
  feedback endpoint sends nothing while the device's own clock and
  buffers go on; an adaptive sink may need to reacquire its rate after
  the gap, and a synchronous one is not thereby click-free. Linux's
  practice shows that devices commonly meet this state, not that every
  device handles it well. On the host side every start begins afresh
  (29.4.8).
- **Taken further.** The same idea can select the alternate only when a
  stream is opened with a format and return to alternate 0 only at its
  close, never at a start or a stop. `uac2` selects at RUN, and on 7 the
  engine closes the pin after each burst of sound (record, section 4),
  so a rule tied to the close alone would still switch at every sound;
  the linger covers the pause and the close alike.

### 29.4 One state machine for the linger and the hold

**29.4.1 The record.** Kept per streaming interface of the function, in
the transport's device record (`UAC_DEV`), not in the stream: a stream
(`UAC_USB`) is destroyed at its pin's close (section 27), and the
alternate must outlive it. The record, not the stream, owns the
selection: the alternate and rate selected, the terminal, the pipe
handles the selection returned with what it returned for each pipe
(endpoint, `MaximumPacketSize`, `Interval`) and the
`MaximumTransferSize` it asked, the clock path it claims, and:

- `AttemptAt`: the interrupt time (`KeQueryInterruptTime`: monotonic,
  100 ns units, 64 bits) at which the interface's last selection
  attempt completed: the last SELECT_INTERFACE actually submitted to the
  stack, successful or not (29.4.6). A selection refused before
  submission (an allocation failure, the gate's refusal) is no device
  interaction and does not move it. Set at `UacUsbOpen` to the
  completion of SELECT_CONFIGURATION, which puts every interface at
  alternate 0. Differences are taken in 64 bits and turned into
  milliseconds without a 64-bit divide, as `WaitClosing` does
  (`uacstream.h`, `UacStream::WaitClosing`: its milliseconds from the
  interrupt time). The machine's clock is a non-wrapping 64-bit
  millisecond count derived from the full 64-bit interrupt time, never
  truncated to 32 bits (a 32-bit millisecond clock wraps after 49.7
  days); 2^64 ms is about 584 million years, so wrap is not handled.
  Deadline arithmetic saturates at "never": a time near the top of the
  range would give a deadline that never comes, the stated and tested
  limit, chosen over modular arithmetic with a horizon contract because
  no real clock reaches it;
- `StopAt` and `Deadline` (29.4.3), set once at a hand-back and never
  recomputed;
- `Adoptable` (29.4.4), and `Epoch`, the device's power epoch at the
  selection (29.5.5, 29.6);
- `RetryAt` and `RetryMs`: a hold-back after an alternate 0 that could
  not be submitted (29.4.6);
- `Gen`, a count of changes of state, owner and retry time: bookkeeping
  for the counters and tests only. No action is validated by it;
  correctness rests on the actor owning the record (29.5.3) from its
  decision, across its wait and its request, to the completion it
  applies;
- `Owner`: the actor that owns the record's next transition (29.5.3);
- the settings snapshot of the RUN that made or adopted the selection
  (`L`, `H`; 29.8).

A stream holds a link to its record only while the record is `Active`
for it.

**29.4.2 States.**

| State | The interface | Clock claim | Pipes |
|---|---|---|---|
| `Idle` | alternate 0, by a selection that succeeded | none | none |
| `Active` | alternate N, a stream running on it | held by the record | the record's, lent to the stream |
| `Lingering` | alternate N, no stream, no transfer; adoptable or not (29.4.4) | held by the record | held by the record |
| `Unknown` | uncertain: a SET_INTERFACE failed without a time-out (29.4.6) | none | none: the stack's handles are not used again |

The failed state (29.9) is the device's, not a record's: a device-wide
flag; once it is set no record makes a transition that sends.

**29.4.3 The deadline and the hold.** At a hand-back (29.4.5), with the
snapshot's `L` (`Uac2LingerMs`) and `H` (`Uac2HoldMs`), `StopAt` is now
and:

    Deadline = max(StopAt + L, AttemptAt + H)    the record adoptable
    Deadline = AttemptAt + H                     not adoptable

If the deadline has passed (both 0, or the hold long over and `L` 0),
the stopping caller selects alternate 0 at once, as before this
section. Otherwise the record becomes `Lingering` and the device's alt
thread (29.5.4) switches it at the deadline. So alternate 0 never comes
sooner than `L` after the stop of an adoptable stream nor sooner than
`H` after the interface's last alternate change.

The hold (D2): no SET_INTERFACE(0) before `AttemptAt + H`. Provisionally
(O2) also no SET_INTERFACE(N) from `Idle` or `Unknown` before
`AttemptAt + H`, so that no two alternate changes of one interface are
closer than `H`, in either order; the newest report's changes were
alternate N a few ms after alternate 0. An event that needs alternate 0
before the deadline (a format change, a clock conflict, a power change,
a PnP stop) waits only for the hold, at PASSIVE, holding neither the
device mutex nor a spin lock (29.5.3), then switches. The hold spaces alternate changes only:
a start's clock and mute requests and the level writes go out between
them as before (with the symmetric hold a start's clock requests come
after its wait, so at least `H` after the alternate 0).

**29.4.4 Adoption.** A start adopts a `Lingering` record, sending
nothing to the device (29.4.9), only when all of these hold, read with
the record owned by the start:

- the same alternate and rate, on the same terminal;
- `Adoptable`: the last stream on it ended clean: stopped by its client
  or drained by a power or PnP path, its `LastStatus` success, feedback
  not turned off for it (25.7: a pipe that refused start frames in a
  row is halted at the stack), no resync pending or bounded out (26.13),
  every transfer back;
- the geometry fits, pipe by pipe: for the data pipe, the new stream's
  transfer size (`SlotBytes`) at most the `MaximumTransferSize` the
  selection asked for that pipe, and its packet size (`Capacity`) at
  most the returned `MaximumPacketSize`; for the feedback pipe, or
  `uac1`'s synch OUT pipe (26.12), the same with that pipe's own
  transfer size (its packets times `FeedbackBytes`) and packet size
  (`FeedbackBytes`). `SelectAlt` asks the stream's `SlotBytes` for every
  pipe (`uacusb.c`, `SelectAlt`: each pipe's `MaximumTransferSize` set
  to `SlotBytes`), and a later stream can differ by `Uac2QueueMs`
  or its ring (19.4);
- the same power epoch, the admission gate open, the device not failed
  (29.5.5).

Otherwise the record is ended (wait for the hold, SET_INTERFACE(0), the
claim released) and the start goes on as from `Idle`.

**29.4.5 Transitions.**

| From | Event | Who acts | What is sent | To |
|---|---|---|---|---|
| `Idle` or `Unknown` | a start | the starting caller | (symmetric hold: wait to `AttemptAt + H`); the clock (`SetClock`, with its walk's reads), the diagnostic reads (`ReadFeature`), SET_INTERFACE(N); `uac1`: its endpoint rate | `Active` |
| `Active` | the stream stops (PAUSE or STOP from RUN), its pin closes, or a power or PnP path drains it: the hand-back | the stopping caller | the transfers cancelled and drained (as today); alternate 0 only if the deadline has passed | `Lingering`, or `Idle` |
| `Lingering` | a start that meets 29.4.4 (the pin reopened, or a paused pin run again) | the starting caller | nothing to the device; the first batch at explicit frames (29.4.8) | `Active` |
| `Lingering` | the deadline | the alt thread | SET_INTERFACE(0); the claim released | `Idle` |
| `Lingering` | a start that does not meet 29.4.4 (a format or rate change: exclusive mode, a new engine format; or not adoptable) | the starting caller | wait for the hold; SET_INTERFACE(0); the claim released; then as from `Idle` | `Active` |
| `Lingering` | another interface's start needs a clock value this record's claim holds otherwise (the capture and playback pair on one clock at another rate, a selector at another input) | that starting caller (29.4.7) | wait for the hold; SET_INTERFACE(0); the claim released | `Idle` |
| `Lingering` | a power change below D0, system sleep included | the power caller (29.6) | wait for the hold; SET_INTERFACE(0) | `Idle` |
| `Active` | a power change below D0 | the power caller | the stop as today, then as the row above | `Idle` |
| `Lingering` or `Active` | PnP stop | the PnP caller (29.6) | the stop if `Active`; wait for the hold; SET_INTERFACE(0) | `Idle`, then discarded with the device record |
| any | surprise removal, or a remove on any system (on 98 SE and ME a remove with no surprise removal before it, 29.6) | the PnP caller | the transfers cancelled and drained; nothing sent | discarded |
| any | a device request's deadline expires (29.9) | the expiry hook, in the requesting thread | nothing, from then on | the device failed |
| `Active` | a start while running | the starting caller | nothing (as today), after the gate (29.5.5) | `Active` |

Only the hand-back of a record that is `Active` for the stopping stream
sets `StopAt` and the deadline, and it clears the stream's link
(29.4.1). Every later stop of the same stream finds no link and does
nothing to the record: `Halt` then `Close` (`uacwdm.cpp`, `Wave::Halt`
and `Wave::Close`, each draining its direction under the operation mutex),
the `UacUsbStop` inside `UacUsbDestroy` (`uacusb.c`, `UacUsbDestroy`:
its `UacUsbStop` before the stream count is lowered and the stream
freed), the `StopLocked` that begins every `UacUsbStart` (`uacusb.c`,
`UacUsbStart`: its `StopLocked` right after the "already running"
shortcut), a power drain
followed by the client's own STOP (`uacwdm.cpp`, `Wave::PowerLocal`:
the drain of both directions below D0), and `Shutdown`'s drain
(`uacwdm.cpp`, `Wave::Shutdown`: its drain of both directions under the
operation mutex). So closing a pin already paused
does not restart its linger, and repeated stops never read the settings
again.

**29.4.6 Steps that fail.** Without a time-out; a time-out of a device
request is the failed state (29.9) when `Uac2FailOnTimeout` is on, and
the same step's failure below when it is off.

| Step | Fails how | The record after | Claim and pipes | The caller | Next |
|---|---|---|---|---|---|
| `SetClock` (its walk, a selector, the rate, the validity) | any status | as before the start | claim released (as today, `uacusb.c`, `SetClock`: its `ReleasePath` on failure when `ClockHeld`); no pipes | the start fails with that status | a later start tries again |
| SET_INTERFACE(N) | refused by the stack or the device | `Unknown`, `AttemptAt` set | released; none | the start fails | a later start selects N afresh |
| SET_INTERFACE(N) | succeeds, but the pipes returned do not fit (`uacusb.c`, `SelectAlt`'s checks after a successful SELECT_INTERFACE: every pipe isochronous, the data pipe and any feedback pipe found) | `Lingering`, not adoptable, deadline `AttemptAt + H` | held until the switch | the start fails at once | the alt thread selects alternate 0 at the deadline |
| after a fresh selection: `uac1`'s endpoint rate, a slot or worker allocation, the worker's creation | any | as the row above | as above | the start fails | as above |
| the same, after an adoption | any | `Lingering` again, with its earlier deadline (no transfer was sent) | kept | the start fails | adoptable as before |
| the stream ends on its own (`Fail`: a transfer error, a feedback loss, the resync bound) | any | at its hand-back: not adoptable | held until the switch | | alternate 0 at `AttemptAt + H` |
| SET_INTERFACE(0) | refused by the stack or the device | `Unknown`, `AttemptAt` set, counted | released; the handles not used again | goes on (a power change cannot fail, 29.6) | no retry; the next start selects N afresh; a teardown sends nothing for it |
| ABORT_PIPE (only where a leg keeps it, 29.4.8) | any, or its time-out | not adopted: ended as a record not adoptable | held until the switch | | host-local: never the failed state |

`Unknown` is not `Idle`: the device may still be at alternate N, so the
symmetric hold counts from the failed request, nothing is adopted, and
the counters show it (`Uac2Alts`, 29.9). Its claim is released because
no stream depends on it; a later stream may set the clock to another
value, which the device, at worst still at alternate N with no
transfers, receives as at any rate change. A refused SET_INTERFACE(0)
may leave the stack's bandwidth for alternate N reserved until the
configuration is torn down; the record cannot tell.

`AttemptAt` moves after a failed selection too, not only after a
successful one: a SET_INTERFACE refused after submission may still have
reached the device (a STALL is its answer to the SETUP), so counting
from it errs toward more spacing, never less. That says nothing about
whether the alternate changed, which is why the record goes `Unknown`.
A selection never submitted (refused by the gate, or no memory for the
URB) leaves `AttemptAt` and the record's state unchanged.

**An alternate 0 that could not be submitted.** A due record whose
SET_INTERFACE(0) was never submitted (no memory for the URB or IRP)
stays `Lingering` with its deadline and `AttemptAt`, since nothing
reached the device; but it is held back until `RetryAt`, 100 ms after
the failure, the interval doubling at each further failure to a ceiling
of 1 s, and cleared by the next submission. So the alt thread, which
works until nothing is runnable (29.5.4), never loops on a record that
keeps failing to submit; and the delay is not made by moving
`AttemptAt`, which would misstate when the device last saw a selection.
The same holds when the stopping caller's own immediate alternate 0
could not be submitted: the record is left to the thread at `RetryAt`.

**The gate is the authority for the failure.** A record's failed state
is kept against a late completion (a request that completes, even
successfully, after its deadline expired changes nothing), but every
action that sends, the thread's claim and the start's commit refuse on
the gate's failed flag alone: a late success never authorises another
request or a successful start.

**29.4.7 A clock conflict with a lingering record.** Today `SetClock`
walks the clock path under the device mutex (`uacusb.c`, `SetClock`:
its `LockDev`, then `Walk`, then `ClaimPath`), and the walk itself
rejects a path another claim holds at another value: `WalkEntity` calls
`PathFree` at the source and tries the next path, so a walk that finds
only such paths fails and `SetClock` returns `STATUS_NOT_SUPPORTED`
(its answer when `Walk` fails) before `ClaimPath` (with
`ClaimPathClaims`, which checks every entity before it takes any) ever
sees a conflicting path. Conflict discovery therefore lives in the walk:

1. `PathFree` returns one of three answers, read under the gate lock
   (29.5.2): free; in conflict only with claims held by `Lingering` records
   (with the set of those records); in conflict with an `Active` record.
2. The walk treats the second answer as a candidate: it keeps the first
   such path and its record set in the walk's state and goes on
   searching, so a free path anywhere in the walk still wins, as today.
   A path in conflict with an `Active` record is rejected as today.
3. A walk that ends with a free path claims it (`ClaimPath`, which
   checks again atomically under the spin lock) and goes on as today. A
   walk that ends with only a candidate returns the candidate's record
   set: `SetClock` releases the device mutex and frees its walk storage
   (no claim was taken) and returns that set to `UacUsbStart`.
4. `UacUsbStart`, holding its stream's operation mutex (and its pair's
   Wave mutex) but neither the device mutex nor a spin lock, gives up
   the ownership of its own record (still `Idle` or `Unknown`: nothing
   to undo); takes the ownership of each record of the set in index
   order (29.5.3: waiting, if another actor owns it, re-checking the
   failed flag and the gate at each sleep); checks each again under the
   spin lock (still `Lingering`, its claim still in conflict); waits for
   its hold, selects alternate 0, releases its claim and gives up its
   ownership.
5. It then takes its own record again and runs the whole of `SetClock`
   once more: a fresh walk, whose requests go out again, and an atomic
   claim. A second walk that again ends with only a candidate, or any
   conflict with an `Active` record, ends the start with
   `STATUS_DEVICE_BUSY`, as two running streams in conflict do today:
   one eviction round per start, no loop.

The simplest case is the one to test first: a playback and a capture
interface on one clock source, the playback lingering at 48 kHz and a
capture starting at 44.1 kHz (29.14). The device mutex is never held
while an ownership is awaited, and no lock is taken against the order
of 29.5.2. `UacUsbRates`' walk (rates at PnP start, `w->Rate` 0) takes
no claim and is unchanged.

**29.4.8 The restart of an adopted pipe.**

- **The old transfers are back first.** The stream worker exits only
  when no transfer and no frame request of it is with the stack
  (`uacusb.c`, `Worker`: its exit test, `UacPlanWorkerExit`), and the
  stop waits for that exit (`StopLocked`: its wait for `Exited`) before
  the hand-back: a record is adopted
  with nothing outstanding on its pipes.
- **The first batch at explicit frames.** The isochronous how-to of
  Microsoft Learn (4.4) says the stack resets its ASAP tracking only
  after 1024 idle frames and offers an explicit start frame instead,
  counted in 1 ms frames at every speed; URB_FUNCTION_ABORT_PIPE is
  documented as cancellation, leaving the pipe's and the endpoint's
  state unchanged, so it is no basis for scheduling. On an adopted
  record the stream's worker places its first batch as 26.13's resync
  does: it reads the bus frame (sent and not waited for, 26.13 item 8;
  read afresh when older than `UAC_FRAME_FRESH_MS`,
  `UacPlanResyncAhead`), then submits every slot of each pipe, data and
  feedback, in slot order without `USBD_START_ISO_TRANSFER_ASAP`, slot k
  of a pipe at `current + 8 + k * F` (`UacPlanResyncFrame`), contiguous
  within the pipe, with 26.13's frame arithmetic across the counter's
  wrap; later submissions are ASAP again, after the explicit ones. That
  first placement does not count toward `UAC_RESYNC_LIMIT`. A fresh
  selection keeps today's ASAP start: the selection opens the pipes
  afresh.
- **Late packets of the first batch.** A first-batch transfer refused
  whole for its start frame, or back with packets the stack did not send
  in their frame (`USBD_STATUS_ISO_NOT_ACCESSED_BY_HW`, or a stack's own
  late status), is counted (`FirstLate`, 29.9) and handled by 26.13's
  rules (a resync). The start of a short sound can be lost then; the
  count lets the legs see whether any stack does it.
- **ABORT_PIPE** is not sent by default. A build for the legs may send
  it before the first batch, to compare; it is kept for a stack only if
  a leg there shows it needed and harmless, and then only as a
  precaution behind the explicit frames.
- **Nothing of the previous stream is reused but the pipes.** Every
  start computes its cadence afresh and starts its feedback filter and
  the feedback watchdog from nothing (`uacusb.c`, `UacUsbStart`: the
  render cadence's `UacRateInitialize`, the slots, and the feedback and
  watchdog state reset before the worker is created): no feedback value
  measured before the pause is used, whatever
  the pipe handle.
- Whether each stack accepts explicit frames on an adopted pipe at once
  (Windows' EHCI on 2000, XP and 7; NUSB's on 98 SE; xHCI98; the UHCI
  and OHCI drivers of 98 SE, ME and 2000; other xHCI drivers) is not
  established for any of them: each is a compatibility leg (29.14), and
  a stack not run there stays unverified.

**29.4.9 What a start inside the linger sends.** None of: the clock
walk's RANGE reads (`uacusb.c`, `SourceRanges`: the count, then the
whole) and its
GET_CURs, any selector GET_CUR or SET_CUR, the source's SET_CUR, the
validity read, the diagnostic GET_CUR of mute and volume
(`ReadFeature`: its last values carried into the new stream's stats,
marked as carried), SET_INTERFACE, and for `uac1` the endpoint's
SAMPLING_FREQ SET_CUR and GET_CUR. It still does what is not traffic to
the device or is needed on its own account: the host-local
GET_CURRENT_FRAME_NUMBER of the stats and of the first batch (29.4.8);
a level write when a level is stale (`LevelStale`, after a D3:
unchanged); section 28's reads at a capture RUN, as 29.11 says; then
the isochronous stream. The clock is not read again because the
record's claim was held throughout: no stream of this driver can have
moved it, and nothing else of the driver writes the clock. A device
whose clock fails on its own during the linger (an external clock
source lost) is found by the same means as one whose clock fails while
streaming today, which is to say not; accepted.

**29.4.10 The device's other streams.** Each streaming interface has
its own record: a playback interface lingering does not stop a capture
start on another interface unless their clock values conflict
(29.4.7); several endpoint pairs (15.3) each linger on their own. A
start at the same rate on the same clock shares a lingering record's
claim as two running streams share one today.

**29.4.11 When `L` and `H` are both 0** every hand-back finds its
deadline passed and selects alternate 0 at once; no record lingers,
nothing is adopted, every start sends everything: today's sequence,
which the host tests check request for request (29.14). The exact
baseline needs the other two settings at 0 too (29.8).

### 29.5 Actors, locks and lifetime

**29.5.1 Who runs what.**

| Actor | Runs at | Does | Never |
|---|---|---|---|
| The alt thread, one per device record (29.5.4) | PASSIVE, a system thread | deadlines, a lingering record's end, the probe (29.10) | takes a Wave or stream operation mutex; notifies the failure |
| The notifier, one per device record (29.9) | PASSIVE, a system thread | the failure's notification only: the record, the event log entry, `IoInvalidateDeviceState` | sends any USB request; takes a transport, Wave or stream mutex |
| `NewStream`, `SetState` and the `Run`, `Halt`, `Close` hooks | PASSIVE (the miniport's contract) | starts, hand-backs, adoptions, the forced switches of 29.4 | |
| A stream's final COM `Release` | up to DISPATCH | queues the destroy, as today (section 27) | runs a stop or a join inline |
| USB completion routines (`SlotDone`, `FrameDone`, `SyncDone`) | up to DISPATCH, possibly inline | as today; nothing new | a teardown, a join, a wait |
| The expiry hook (29.9) | PASSIVE, in the thread whose request's deadline expired, with whatever that thread holds | sets the failed flag, marks every registered stream to stop, wakes the notifier and the alt thread | takes a mutex, waits, sends |
| `PowerChangeState` | PASSIVE | 29.6 | |
| `DispatchPnp` (stop, surprise removal, remove, the state query) | PASSIVE | 29.6, 29.9 | |
| A stream's worker | PASSIVE, real time (19.6) | as today; it sees the stop mark | |

The 9x WDM path has the same actors: a uniprocessor 98 SE still
preempts a thread with DPCs and completion routines, so nothing here
relies on one processor.

**29.5.2 Lock order.** From first taken to last:

1. a Wave filter's operation mutex (pair 0's before a later pair's, as
   `PowerChangeState` takes them, `uacwdm.cpp`, `Wave::PowerChangeState`:
   pair 0's held while each later pair's `PowerLocal` takes its own);
2. a stream's operation mutex (`s->Operation`);
3. a record's ownership (29.5.3);
4. the device mutex (`d->Lock`: control requests and the configuration);
5. the gate lock (`g->Lock`, new: a spin lock in the gate's own
   storage in the FDO's extension, initialised at AddDevice, 29.5.5;
   it protects the gate's state, the failed flag, and, while a device
   record exists, that record's records, clock claims and registered
   streams; "the spin lock" in this section means this one);
6. a stream's spin lock (`s->Lock`, a leaf as today).

The expiry hook takes only 5 and 6, whatever its thread holds. The alt
thread takes 3 to 6 and never 1 or 2; the notifier takes 5 only. An ownership is awaited only with
nothing of 4 to 6 held and never while owning another record, so no
actor waits for an ownership while holding something its owner needs.
The clock claims move from the device mutex to a spin lock (designed as
this one; as built, a claim lock of their own, `d->ClaimLock`, taken
alone and never with the gate lock, while the gate is active): the walk
checks them under it (`PathFree`), the claim re-checks and takes them
atomically under it (`ClaimPath`, still called inside the device mutex,
so starts walk one at a time as today), and a release takes the spin
lock alone (today `ReleasePath` takes the device mutex: `uacusb.c`,
`ReleasePath`, which as built does so while the gate is inactive and
takes the claim spin lock `d->ClaimLock` while it is active, 29.17);
the stream count (`UacUsbCreate`'s raise and `UacUsbDestroy`'s lowering
of `d->Streams`, under the device mutex while the gate is inactive)
becomes interlocked. So neither a
stop nor a destroy waits behind a hung request for them. The code these
run in under the spin lock is nonpaged.

**29.5.3 Ownership of a record's transition.** A record's `Owner`,
set and cleared under the spin lock, says which actor may change the
record next: none, the stream it is `Active` for, the alt thread, a
starting caller (for its own record or one it evicts, 29.4.7), the power
path or the PnP path. An actor that wants a record owned by another:

- the alt thread skips it, and is woken when it is released;
- any other actor waits at PASSIVE in 10 ms sleeps (as `WaitClosing`
  does, `uacstream.h`, `UacStream::WaitClosing`: its loop of 10 ms
  delays), holding nothing of 4 to 6 and owning
  no other record (a starting caller gives its own up first, 29.4.7).
  A starting caller re-checks the failed flag and the gate at each
  sleep, either of which ends its wait with `STATUS_DEVICE_NOT_READY`.
  The power and PnP paths, which closed the gate themselves, wait for
  the release: the alt thread finishes its one action, and a start,
  refused by the closed gate, gives up. So a power change or a teardown
  never waits behind a start that waits for a record; it waits behind a
  request in flight, and behind a hung one until the lower driver
  completes it (29.9).

The owner sleeps for a hold, and sends its SET_INTERFACE, holding its
ownership and, for a starting caller, the mutexes it came with; the spin
lock is never held across a wait or a request. A hold on one interface
therefore delays no transition of another; requests still go one at a
time under the device mutex, as today. A pair's own start waits for its
own hold holding that pair's Wave mutex, so that pair's opens and runs
wait with it, by design. There is no device-wide transition mutex.

**29.5.4 The alt thread.** One system thread per device record on 2000
and later (on 98 SE and ME, 29.5.6 and O14). `UacUsbOpen` creates it
directly in the PnP start: the PnP manager sends IRP_MN_START_DEVICE at
PASSIVE_LEVEL in the context of a system thread (Microsoft Learn,
IRP_MN_START_DEVICE), so `PsCreateSystemThread` is called there and its
handle closed there; no executive work item stands between. As the
stream worker does (`uacusb.c`, `Worker`: `PsGetCurrentThread` and,
where `WaitThread`, `ObReferenceObject` of it), the thread references its
own thread object for the join (29.5.6) before it sets its ready event,
and `UacUsbOpen` waits for that event. (The stream worker is created through an executive work item,
`CreateWorker`, because `SetState` runs in the client's process; that
callback's return path is an existing residual, 29.5.6.)

It loops:

1. clear its wake event;
2. under the gate lock, look for runnable work. A record is runnable
   only when it is `Lingering`, unowned (`Owner` none), its deadline
   has passed, its `RetryAt` (29.4.6) has passed, its epoch is the
   gate's, and the gate allows it (open,
   not failed; not quiescing, closed or below D0). For the first
   runnable record it claims the ownership and reads state, deadline,
   gate and epoch again in the same critical section: what the scan saw
   is not authority to send. The probe is runnable on the same terms
   (29.10);
3. release the lock and carry the work out (SET_INTERFACE(0), the claim
   released, the ownership given up; or one GET_STATUS); go back to 2
   until nothing is runnable;
4. compute the next wake only from records that are `Lingering`,
   unowned and not held back by the gate, and from the probe's due time
   when the probe is admitted; an owned record contributes no time (its
   release sets the wake event), and with nothing pending the thread
   waits without a time-out. So an expired deadline that cannot run now
   never turns into a loop of zero-length waits;
5. wait on the wake event with that time-out, as the stream worker
   waits (`uacusb.c`, `Worker`: the wake event cleared at the top of
   each pass, then waited on with the pass's time-out).

Posters (a hand-back, a released ownership, the expiry hook, the gate,
a reopened gate at D0) change state under the gate lock and then set
the wake event. A post made before step 1 is already in the state step
2 reads; one made after it leaves the event set, so the wait returns at
once: no wake is lost. Since the thread reads every record each time,
wakes coalesce by construction: there is no per-event context to
overwrite, and an obsolete wake finds nothing runnable. The deadline's
precision is the system clock's tick (about 10 to 16 ms), immaterial
against 200 ms and 2 s.

Why a thread and not a kernel timer, its DPC and an executive work item
per expiry: a DPC can already be queued or running when `KeCancelTimer`
returns (Microsoft Learn, KeCancelTimer: the caller must synchronize
with a DPC that is running), and the x86 build's executive work item
holds neither the device object nor the image while its routine returns
(`uacwdm.cpp`, the x86 work item behind `IoQueueWorkItem`: `UacAllocItem`,
`UacQueueItem` and `UacExItem`, and the comment above them); each would need a reference and rundown
protocol of its own. A thread joined through its thread object has a
documented end, on 2000 and later. It needs no new import:
`KeInitializeTimer`, `KeSetTimer`, `KeCancelTimer` and `KeInitializeDpc`
are not used. Its cost, with the notifier (29.9), is two
sleeping threads per audio device. Failing to create either fails the
PnP start (`STATUS_INSUFFICIENT_RESOURCES`), as a level work item that
cannot be allocated does (`uacwdm.cpp`, `Wave::Prepare`: the level work
item's `IoAllocateWorkItem`); a thread
already created is stopped and joined first.

**29.5.5 The admission gate.** Kept with the failed flag in a small
structure (`UAC_GATE`) in the FDO's extension, with its own spin lock
(`g->Lock`), both initialised at AddDevice: so the gate and its lock
exist before any device record (the start's own descriptor reads use
them, 29.9) and after one is retired, as long as the device object. A
device record points at the gate and uses its lock for its own records,
claims and stream list (29.5.2). It holds:

- `Admit`: open; quiescing (a PnP stop: starts, adoptions, probes and
  alt thread actions refused, while the PnP caller ends the records);
  closed (surprise removal or remove: nothing sent at all); below D0
  (29.6);
- `Failed`, with the expiry's record (29.9);
- `Life`: starting, started, tearing down, surprised (29.6, 29.9);
- `Epoch`, incremented at each power change below D0.

Every entry checks the gate before it waits on anything: `NewStream`
before its wait for a closing stream (`uacwdm.cpp`, `Wave::NewStream`:
its `UacStream::WaitClosing` call; today the readiness check of
`removed`, `pnpBlocked` and `powerBlocked` comes after it, before
`UacStream::Create`) and again under the Wave mutex; `Run` before the
Wave mutex (`uacwdm.cpp`, `Wave::Run`: its take of the operation mutex,
`TakeOperation`) and again under it; `UacUsbStart`
before its "already running" shortcut (`uacusb.c`, `UacUsbStart`: the
`already` test under `s->Lock` and the early return); each
adoption, alt thread action, probe and failure notification. The early
reads are interlocked reads that can only refuse; the read under the
lock decides. Every teardown and power-down closes the gate first,
before it drains anything (29.6), and the gate is the device's, not a
Wave pair's: the gaps between one pair's `PowerLocal` and the next
(`uacwdm.cpp`, `Wave::PowerChangeState`: its loop over the later pairs'
`PowerLocal`, then pair 0's own after the mutex is released) are covered. The waits themselves, and the
start's last step, are in 29.5.7 and 29.5.8.

**29.5.6 Lifetime and rundown.** At every PnP teardown (stop, surprise
removal, remove, a failed start; 29.6), in order: close the gate; drain
each pair's streams (as `Shutdown` does today: each worker has exited
and its transfers are back before its slots are freed, `uacusb.c`,
`FreeSlots`, called by `StopLocked` after the worker's exit); stop the alt thread and the notifier (exit
flag, wake event, then the join below); end the records as 29.6 says,
in the PnP caller's own thread. A power change stops neither thread;
the gate keeps them from sending below D0 (29.6). Only after a teardown
may `UacUsbClose` free the device record, asserting no stream, no owned
record and no thread. The threads are stopped outside every Wave and
stream mutex, as `Shutdown` already waits for the level work item before
it takes the Wave mutex (`uacwdm.cpp`, `Wave::Shutdown`: `level.Closed`
set and `level.Work` waited out before the operation mutex). All of it happens
in `DispatchPnp` before the IRP goes on to PortCls; the expiry hook adds
no deferred callback.

**The join on 2000 and later** (x86 and x64) waits for the thread
object, which is signalled when the thread has terminated, as
`StopLocked` waits for the stream worker there (`uacusb.c`,
`StopLocked`: where `WaitThread`, its wait on the thread object after
`Exited`, then the object's dereference): after it, no code of the thread is left to run in the image,
whatever it was preempted at. Neither thread is created through an
executive callback (29.5.4), so no callback's return path is added. An
alt thread blocked in its own expired request is joined only when the
lower driver completes that request (29.9): the teardown waits with it,
under the blocked-caller contract; the notification does not (29.9).

**On 98 SE and ME no rundown contract can be shown.** A system thread's
object is not waited on there (a wait faulted: `uacusb.c`, `Worker`,
the comment before its `PsGetCurrentThread`), and the stream worker
signals `Exited` (`Worker`'s last access to the stream) before it calls
`PsTerminateSystemThread` (its next statement): a thread preempted
between the two still has instructions of the driver to run when a
teardown that waited for `Exited` completes, and a delay of any length
(today 10 ms: `StopLocked`'s `KeDelayExecutionThread` where it does not
wait on the thread object) narrows that window without closing it. A reference the thread
drops itself before it ends recreates the same last-instruction window.
Microsoft requires a driver's threads to have terminated before it
unloads (Microsoft Learn, "Releasing Driver-Allocated Resources"). No
mechanism documented as available on 98 SE and ME has been found here
that observes a system thread's termination, or that keeps a WDM
driver's image resident until such a thread has ended; none is
asserted, and a leg can only fail to show a fault, not prove its
absence. The same window exists today: on 98 SE and ME for the stream
worker, and on every x86 system for the executive work items that
create stream workers (`uacusb.c`, `CreateWorker` signals `Done`, its
last statement, before it
returns, and on a creation failure no thread exists to keep a teardown
waiting while the callback returns) and that run the level work and the
stream destroy (`uacwdm.cpp`, the x86 work item behind `IoQueueWorkItem`:
`UacAllocItem`, `UacQueueItem` and `UacExItem`). This section must not widen it.
The options (O14, OPEN FOR THE OWNER):

1. **Recommended: no new thread on 98 SE and ME** (below WDM 1.10, the
   test `StopLocked` already makes). The linger has no timer there: a
   record handed back with its deadline ahead stays `Lingering` until an
   event needs alternate 0 (a start of another format, a clock conflict,
   a power change below D0, a PnP stop; at a remove nothing is sent), so
   an idle device stays at alternate N, with no transfers and its
   bandwidth held, until then. A start of the same format adopts it as
   on NT; the hold applies, in the caller. The probe is not available.
   The failure is published by the hook and noticed by the next passive
   entry (29.9). No thread tail is added.
2. Both threads on 98 SE and ME too, joined by `Exited` and the 10 ms
   delay of the stream worker, stated as an unproven residual equal to
   that worker's, with a leg that removes and reinstalls the driver
   while sounds play.
3. Linger and probe off on 98 SE and ME (their settings read as 0
   there), the failed state kept as in option 1.

The existing residuals (the stream worker on 98 SE and ME, `CreateWorker`
and the executive work items on x86) are outside this section and stay
as they are; whether to address them is part of O14.

**29.5.7 Admission across waits, and the start's commit.**

- **Gate-aware waits.** `NewStream`'s wait for a closing stream
  (`WaitClosing`, 10 ms sleeps) reads the gate at each sleep: failed or
  closed ends it, and `NewStream` refuses. The Wave mutex in `NewStream`
  and `Run`, and the stream's operation mutex in `UacUsbStart`, are taken
  by timed waits that read the gate between tries; the device mutex is
  taken the same way (29.5.8). Only `STATUS_SUCCESS` from
  `KeWaitForSingleObject` means the mutex was acquired: `STATUS_TIMEOUT`
  is also a success status (Microsoft Learn, KeWaitForSingleObject), so
  `NT_SUCCESS` cannot decide it. The interval (100 ms) is a polling
  interval, not a scheduling guarantee. `Halt`, `Close`, the power path
  and the PnP path take the Wave and stream mutexes untimed: they must
  run to release what they release, and may stay blocked behind a hung
  request (29.9).
- **The start's commit.** Today `UacUsbStart` clears `s->Stop` while it
  initialises the stream (`uacusb.c`, `UacUsbStart`: `s->Stop` cleared
  and `Stats.Running` set after the slots are built; as built, still so
  while the gate is inactive, and through the commit below while it is
  active, 29.17), so an expiry's mark made just before that statement
  would be undone. Instead, `s->Stop` is cleared
  only in a commit made under the gate lock and then `s->Lock`, which
  first checks the gate (not failed, admission open, the epoch
  unchanged). The expiry hook marks streams under the same two locks, so
  either the commit sees the failure (the start fails with
  `STATUS_DEVICE_NOT_READY`, and its stop sends nothing, the gate
  refusing SET_INTERFACE(0)) or the mark comes after the commit and the
  worker drains. After the worker reports ready, `UacUsbStart` reads the
  gate once more under the lock: a failure published meanwhile makes it
  stop the stream and return `STATUS_DEVICE_NOT_READY`. `Run` checks the
  gate again after `UacUsbStart` returns success, before it returns; an
  adoption is decided under the lock (29.4.4). So a request that
  completes successfully after its deadline, whose raw status is
  returned as 29.9 says, never lets a RUN succeed on a failed device:
  these commits override it.

**29.5.8 Admission at each device mutex site.** Every device request
also checks admission under the gate lock immediately before it is
submitted, so a refusal never submits anything.

**The admission rule, as built (2026-11-17).** A request is admitted at
one point only. For a synchronous request (`UsbRequest`, every class,
standard and select request and the start's frame number) that point is
`GateAdmit`, the last step of `CallAndDrain` before `IoCallDriver`: under
the gate lock, with the IRP and URB built and nothing between it and the
call that waits or blocks; the lock is released before the call, and the
gate lock is never held across anything that blocks. For the stream
worker's own submissions (`SubmitSlot`, and `FrameSend`'s
GET_CURRENT_FRAME_NUMBER) the point is the stream lock, where the slot
or request is published as the stack's only when `s->Stop` is clear; the
expiry hook sets `s->Stop` of every registered stream under the gate
lock and then the stream lock, a stream registered after the failure
starts stopped, and the start's commit never clears it on a failed gate,
so a clear `s->Stop` there is the gate's "not failed". Admitted means
the device had not failed at that point; a failure published after it is
ordered after the request and does not recall it (it is that request's
own expiry if it does not complete). The earlier checks (`UsbRequest`
before its allocation, `LockDev` between its tries, `UacGateAcquire`)
can only refuse. `FrameSend` used to publish its request from the
worker's earlier read of `s->Stop` and could send after a failure; it now
admits at the stream lock as `SubmitSlot` does, while the gate is
active. With the gate inactive (98 SE and ME always) nothing of this
applies: no admission check, and `FrameSend` as before (a frame request
sent at a stop is cancelled by the stop's drain). Outside the rule: the
bus interface query (`BusSpeed`), a PnP request to the stack, not a
request to the device, and never gated.

The acquisitions of the device mutex before this section (`uacusb.c`; as
built, the timed ones go through `LockDev`, which waits untimed while the
gate is inactive, 29.17) become:

| Site | Treatment |
|---|---|
| `UacUsbRates`, before its walk | timed acquisition; on a refusal it frees its walk storage and returns `STATUS_DEVICE_NOT_READY`, never partial rates as a successful discovery |
| `UacUsbClassRequest`, around its one request | timed acquisition; releases only a mutex it acquired; on a refusal the returned length is 0 |
| `ReleasePath`, around the claims' release | with the gate active, the claim lock `d->ClaimLock` taken alone (as built; this table first named the gate lock, 29.5.2), never the device mutex; with the gate inactive, the device mutex as before this section (29.17); never refused, gate or not: cleanup cannot fail |
| `EndpointRate` (`uac1`), around its SET and advisory GET | timed acquisition; admission checked again before the advisory GET that follows the SET |
| `SetClock`, before its walk, through the claim and the clock requests | timed acquisition; on a refusal it frees the walk storage and releases any claim taken; every request of the walk and of the sequence after it is admitted on its own |
| `SelectAlt`, around its SELECT_INTERFACE | timed acquisition; a refusal before submission leaves the record and `AttemptAt` unchanged. The power and PnP paths' SET_INTERFACE(0) carries a teardown permission: admitted while that caller has the gate quiescing or going below D0, never when failed or closed |
| the stream count (`UacUsbCreate`'s raise, `UacUsbDestroy`'s lowering) | interlocked, with the stream list under the gate lock; no device mutex |

### 29.6 Power and PnP

**Power, as it is.** `uac2` submits no USB idle request, and the INFs
set no PortCls idle times (no `ConservationIdleTime` or
`PerformanceIdleTime` in `uac2wdm.inf` or `uac1wdm.inf`); without them
PortCls's inactivity timer stays disabled (Microsoft Learn, "Audio
Device Class Inactivity Timer Implementation"). So PortCls takes the
adapter below D0 with the system. A lingering record holds no D0
reference and keeps nothing awake. If selective suspend or a PortCls
idle time is ever enabled, the linger must count as activity or be ended
before the suspend: a selected alternate does not by itself prevent one.

**Leaving D0** (`PowerChangeState` to D1, D2 or D3, system standby and
hibernation included). The miniport may still reach the hardware in this
callback, and the callback cannot fail (Microsoft Learn,
IAdapterPowerManagement::PowerChangeState). Pair 0's callback:

1. closes the gate to "below D0" (no start, adoption, probe or alt
   thread action) and increments the epoch, before it visits any pair;
2. visits the pairs as today (`uacwdm.cpp`, `Wave::PowerLocal`: each
   pair's streams drained); each running stream's hand-back finds
   the gate closed and leaves its record `Lingering` for the next step;
3. ends every `Lingering` record in its own thread: takes its ownership
   (waiting for an alt thread action in progress), waits for its hold,
   selects alternate 0; a refusal leaves `Unknown`, an expiry fails the
   device (29.9); either way the power change goes on, since it cannot
   fail;
4. from then on, until D0, the alt thread and the probe send nothing.
   Level writes keep their present rules (unchanged here).

The delay this adds to a power change is not only `H`: it adds the
selection's own time, any transition in progress and, behind a hung
request, the cancellation drain, which is unbounded on a lower driver
that never completes the request (29.9).

**Back to D0.** The gate reopens unless the device has failed. No record
survives the sleep: each is `Idle` or `Unknown` (and the epoch would
refuse a record from before it), so the next start sends everything,
selects afresh and gets new pipe handles; no pipe handle is kept across
a configuration the stack may have lost. The failed state survives sleep
and wake.

**PnP stop** (2000 and later; the device is present). `DispatchPnp`
sets `Life` to tearing down and the gate to quiescing before
`ShutdownPairs`; the pairs drain as today; the alt thread is stopped;
the PnP caller ends each `Lingering` record (hold, alternate 0) as for a
power change. A later start builds a new device record (`uacwdm.cpp`,
`Start`: `StartSteps` builds new pairs, and pair 0's `Wave::Prepare`
opens the record with `UacUsbOpen`) whose records begin `Idle`.

**Surprise removal and remove, on every system.** `DispatchPnp` sets
`Life` (surprised, or tearing down) and closes the gate to "nothing
sent" before `ShutdownPairs`: the drains cancel the transfers and wait
for them, the hand-backs send nothing (today's stop sends
SET_INTERFACE(0) here: `uacusb.c`, `StopLocked`, its `SelectAlt(s, FALSE)`
while `ActiveAlt`, so the
shared stop path changes, not only the records' cleanup), the threads
are stopped, the records are discarded. 98 SE and ME never send a
surprise removal (Microsoft Learn, "Handling an IRP_MN_SURPRISE_REMOVAL
Request"): an unplug reaches the driver as a remove alone, so a remove
with no surprise removal before it does not mean the device is present,
and nothing is sent at any remove (on 98 SE and ME the driver then delays PortCls's own remove,
a bounded mitigation, section 30). The cost
is on 98 SE and ME: a remove
with the device still plugged in (Device Manager's remove, a driver
update) leaves its interface at alternate N with nothing streaming until
the next configuration, the state 29.3 describes; the next start
configures afresh (SELECT_CONFIGURATION, every interface at alternate
0).

On 2000 and later `Shutdown` runs twice (surprise removal, then remove:
`uacwdm.cpp`, `DispatchPnp`: its `ShutdownPairs` for a stop, a surprise
removal and a remove); every step above is idempotent. A start that
fails is torn down through `ShutdownPairs` as today (`uacwdm.cpp`:
`Start`'s `ShutdownPairs` after a failed outcome, and `StartSteps`' own
for `uac1`'s null function), the gate's teardown handling a
device record whose thread never started.

### 29.7 Costs: power, bandwidth, Windows' audio stack, latency

- **Power.** A configuration's `bMaxPower` covers every alternate of
  it (USB 2.0 9.6.3), so a kept alternate never takes a device over the
  budget it declared; it keeps its streaming path powered for up to `L`
  longer (a few mA to tens of mA on a headphone adapter, for 2 s). The
  device never sleeps with a record lingering, and a lingering record
  holds no D0 reference (29.6).
- **Bandwidth.** The kept alternate's isochronous bandwidth stays
  reserved while its pipes stay configured: under EHCI from the pipe's
  opening, under xHCI98 from the Configure Endpoint that added it.
  Another device's selection that needs periodic bandwidth meanwhile can
  be refused by its stack (USBD_STATUS_NO_BANDWIDTH), as while `uac2`
  streams today; ending the linger does not make Windows retry that
  other device's failed selection, which its driver or its user must
  try again. At High Speed the adapter's largest alternate is 384 bytes
  a microframe, about 5 % of a microframe's periodic share (80 %, USB
  2.0 5.6.4): a payload estimate, without the transactions' overhead or
  the host's own scheduling cost. A two-device leg reads it (29.14).
- **The audio engine and KMixer.** The pin's states, its position
  (17.4) and its notifications are as before; a start inside the linger
  is only faster (no control requests). On 7 the engine opens a new pin
  for each burst of sound (record, section 4), so the linger covers pin
  close and reopen; a paused pin run again (KMixer, 98 SE to XP) is
  covered by the same row.
- **Exclusive mode and format changes.** A start at another format or
  rate ends the linger, waiting for the hold, then sets the clock and
  selects the new alternate as today. With the hold in one direction
  that costs up to `H` more. With the symmetric hold (provisional, O2) a
  change made less than `H` after the last selection waits the rest of
  that hold, selects alternate 0, then waits a full `H` again before the
  new alternate: up to `2H` (400 ms at the defaults) more than today,
  plus the requests' own time. Never `L`.
- **The playback level engine** (section 20 and the level work item):
  unchanged. Volume and mute writes go out when set, inside or outside a
  linger; a level marked stale is written at the next RUN, a start inside
  the linger included.

### 29.8 The settings

In the device's driver key, read with `GetDwordRaw` (a `REG_DWORD`, or
the 4-byte `REG_BINARY` a 9x INF writes; `uacwdm.cpp`, `GetDwordRaw`).
`GetDwordRaw` returns `0xFFFFFFFF` for a value missing or unusable, so a
stored `0xFFFFFFFF` cannot be told from none: it means the default,
never the top of the range, as for `Uac2QueueMs` (`uacplan.c`,
`UacPlanQueueMs`: `UAC_QUEUE_NONE` read as the default). Any other value outside a range is clamped. Each
INF install section writes each value at its default without
overwriting (`0x00010003` in `[QueueNT]`, the binary form `0x00000003`
in `[Queue9x]`, beside `Uac2QueueMs`; `uac1wdm.inf`'s `[Queue9x]` too),
each with a comment line, and README's settings list and the release
notes name them: the project's rule that no setting is hidden.

| Value | Default | Range | 0 | Read |
|---|---|---|---|---|
| `Uac2LingerMs` | 2000 (D1) | 0 to 10000 ms | no linger | at each RUN, with the hold, as one snapshot (below) |
| `Uac2HoldMs` | 200 (D2) | 0 to 1000 ms | no hold | with the linger |
| `Uac2FailOnTimeout` | 1 | 0 off, 1 on; any other value, `0xFFFFFFFF` included, the default | a time-out is that one request's failure, as before (29.9 does not apply) | at PnP start, for the device's life |
| `Uac2ProbeMs` | 0 (provisional, O8; the reporting user's test build writes 10000) | 0, or 2000 to 60000 ms; 1 to 1999 read as 2000 | no probe | at PnP start, for the device's life |

**One snapshot per RUN.** `Run` reads the linger and the hold once,
beside `Uac2Realtime` (19.6), and hands them to the transport with the
stream: the start uses them, the record takes them on, and its
hand-back computes the deadline from them once (29.4.3). An edit in the
registry applies from the next RUN: it never moves a deadline already
set, and repeated stops never read the settings (29.4.5). A forced
switch of a lingering record uses that record's snapshot.

**The exact baseline needs all four at 0.** `Uac2LingerMs` 0 alone still
leaves the hold, which defers alternate 0 when a stream stops sooner
than `H` after its selection; the linger and the hold at 0 restore the
stop's and the start's sequence; `Uac2FailOnTimeout` 0 restores the
requests after a time-out; `Uac2ProbeMs` 0 sends no GET_STATUS. With all
four at 0 the request sequence of a device that answers is today's,
request for request (29.14); the alt thread runs and sends nothing. With
`Uac2FailOnTimeout` 0 the start's configuration descriptor reads
(`uacwdm.cpp`, `Descriptor`, which `ConfigDescriptor` calls while the
gate is inactive) keep today's wait without a
time-out; with 1 they get the 5 s deadline of every other request
(29.9).

The 9x lines: `HKR,,Uac2LingerMs,0x00000003,D0,07,00,00`,
`HKR,,Uac2HoldMs,0x00000003,C8,00,00,00`,
`HKR,,Uac2FailOnTimeout,0x00000003,01,00,00,00`,
`HKR,,Uac2ProbeMs,0x00000003,00,00,00,00` (the test build:
`10,27,00,00`, 10 s); the NT lines `HKR,,Uac2LingerMs,0x00010003,2000`
and so on. `infchk.ps1` checks each line, its no-clobber flag, its
default against the driver's, once per section, with mutations as for
`Uac2QueueMs`.

On 98 SE and ME and on 2000 and later the values, the mechanism and the
binary are the same, except as O14 decides for 98 SE and ME (29.5.6).
The threads add no import (29.5.4); 29.9 adds `IoInvalidateDeviceState`
and the error log pair.

### 29.9 The failed state

**What counts as "stopped answering".** A request to the device whose
5 s deadline expires (`uacusb.c`, `CallAndDrain`: its 5 s wait, then the
cancellation and the untimed wait for the completion): a
class request (to an interface or an endpoint), a standard request (a
descriptor; the probe's GET_STATUS), a SELECT_INTERFACE or a
SELECT_CONFIGURATION. One is enough (D3; O4). Host-local requests are
not requests to the device and never count; their expiries are only
recorded (`HostTimeouts`): GET_CURRENT_FRAME_NUMBER (`uacusb.c`,
`UacUsbStart`: its frame number read before the slots are built; the
worker's own, 26.13), the bus interface query (`uacusb.c`, `BusSpeed`),
and ABORT_PIPE where a leg keeps it (29.4.8).
Not counted either: a STALL (the device answered), a request the stack
refuses at once (a parameter, no bandwidth), a failure because the
device is gone (removal follows), and transaction errors
(USBD_STATUS_DEV_NOT_RESPONDING, USBD_STATUS_XACT_ERROR: no handshake),
which come fast and are what a dead device can also give under EHCI;
counting them is option O4, not proposed by default because a removal
in progress and a poor cable give them too.

**A policy, with a known cost.** USB 2.0's limits concern the device's
handling of a request once it has it: 50 ms for a standard request
without a data stage, 500 ms a data packet to the host, 5 s with data to
the device (9.2.6.4), applied to class requests unless the class exempts
them (9.2.6.5), with the host's own delays added. They give no bound
from a Windows URB's submission to its completion, least of all for
SELECT_INTERFACE and SELECT_CONFIGURATION, which carry host-side work
(the stack's endpoint changes; under xHCI98 a controller command). A
slow host, a controller recovery, a scheduling delay or a cancellation
race can therefore expire a healthy device's request; that device is
then failed and needs a replug. The cost is accepted (fail closed); the
record and the messages say that a request was not answered in time,
not that the device's firmware died.

**The expiry, apart from the final status.** Today `CallAndDrain` waits
5 s, requests cancellation, waits without bound for the completion and
maps `STATUS_CANCELLED` to `STATUS_IO_TIMEOUT`, but returns any other
final status as it comes, a success included, so a request that
completes just after the cancellation escapes. The change: the expiry
is recorded when the 5 s wait ends, before `IoCancelIrp`, and acts there
(the expiry hook, below); the final status after the drain is recorded
beside it (`Status`, `UrbStatus`, `LateMs`: expiry to completion) and
returned to the caller as today. A request that completes successfully
after its deadline still fails the device. The decision is made at the
expiry, so the translations above it (`uacusb.c`, `UsbRequest`'s
`STATUS_DEVICE_DATA_ERROR` for a success whose URB status failed;
`SetClock`'s `STATUS_DEVICE_CONFIGURATION_ERROR` for each of its four
reads that fails or answers wrongly: a clock selector's position, the
rate, the rate read back after its SET, the validity) change
nothing, and the record keeps the original statuses.

**Who stays blocked: the chosen contract.** The caller whose request
expired stays blocked until the lower driver completes the cancelled
request; the request's ownership is not redesigned. Its IRP, URB,
buffers and event live in that caller's frame (`UsbRequest`,
`EntityRequest`), and `CallAndDrain` frees nothing before the completion
is seen (`uacusb.c`, the comment above `CallAndDrain`). Letting the caller return earlier
would need request objects of their own, with references, on every
request path, and the device record kept past the remove until the
lower driver completes, to gain only an earlier return into a device
being failed anyway. The failure does not wait for that return: it is
published at the expiry, and notified by a thread that sends no
request (below), so it is notified even when the blocked thread is the
alt thread itself, in its own probe or lingering alternate 0. What does
wait, until the lower driver completes the cancellation: the blocked
thread; `Halt`, `Close`, a power change, a PnP stop or remove of a pair
whose Wave or stream mutex that thread holds; the drain of its record
(anything else waiting for that record gives up, 29.5.3); the join of a
blocked alt thread at a teardown (29.5.6); and a `NewStream` or `Run`
already past its gate checks into a wait it cannot leave (29.5.7 makes
their waits gate-aware; one already inside the lower driver's call
cannot be). On a stack that completes a cancelled IRP, as the IRP rules
require, that is soon after the 5 s; on one that never does, they wait
forever, as today. This section promises no bounded stop, removal or
power change, and no other timing.

**The hand-off at the expiry.** The expiry hook runs in the requesting
thread, which may hold a Wave mutex, a stream's operation mutex, a
record's ownership and the device mutex; so it takes only spin locks
and never waits (29.5.1). With `Uac2FailOnTimeout` off it only records
the expiry. With it on, under the gate lock, it sets `Failed` with the
expiry's record and marks every registered stream to stop with the
bounded part of the worker's own `Fail` (`uacusb.c`, `Fail`:
`LastStatus` if still success, `s->Stop`, the wake event, under
`s->Lock`; not its trace dump, `TR_DUMP`); the worker then cancels and
drains its transfers and exits, through the same synchronization that
guards every submission (`SubmitSlot`: `s->Stop` read under `s->Lock`
just before the IRP is passed on). Then it sets the notifier's and
the alt thread's wake events. Streams join a list in the device record at
`UacUsbCreate` and leave it at `UacUsbDestroy`, under the spin lock, so
none is freed under the mark. No peer stream is stopped synchronously
from the hook; no mutex is taken; nothing is sent. The workers' threads
are joined later by the ordinary stops (`Halt`, `Close`, the drains),
whose SET_INTERFACE(0) the gate then refuses.

**Afterwards: nothing is sent.** Every device request path checks the
failed flag before it waits for the device mutex, and every request is
admitted under the gate lock just before it is submitted (29.5.8): the
class requests, the clock walk and `SetClock`, `SelectAlt`, `uac1`'s
`EndpointRate`, `UacUsbRates`, the probe and the descriptor reads. Each
waits for the device mutex by timed waits that check the flag between
tries, so a request queued behind the hung one is refused
(`STATUS_DEVICE_NOT_READY`, counted) at the next try after the expiry
(100 ms apart, a polling interval, not a guarantee) rather than when
the lower driver completes. No record makes a transition that sends: no
SET_INTERFACE(0), now or at a stop, power change or removal; clock
claims and pipes are released in the driver only.

**The start's own reads, and its latch.** With `Uac2FailOnTimeout` on
(29.8), the reads before a device record exists go through the same
gate, which exists from AddDevice (29.5.5): `ParseDevice`'s
configuration descriptor reads and `ReadName`'s descriptor reads
(`uacwdm.cpp`, `ReadName`'s `UacUsbDescriptor` calls). `ParseDevice`'s reads
then use the shared owned-IRP path (`UsbRequest`: an IRP from
`IoAllocateIrp`, completed back to the driver and freed by it after
`CallAndDrain`), not a cancellation grafted onto today's `Descriptor`
(`uacwdm.cpp`, `Descriptor`, called by `ParseDevice` for the
configuration's header and then for the whole, as built through
`ConfigDescriptor` while the gate is inactive), whose
`IoBuildDeviceIoControlRequest` IRP the I/O manager completes and frees
(Microsoft Learn, IoBuildDeviceIoControlRequest). With the setting off
`Descriptor` stays as it is, without a time-out. An expiry during the
start sets the gate's failure as at any time (the startup latch), and
`Start` tests it after each step that may tolerate a failure: after
`ParseDevice`; after `ReadName`, which never fails a start (`uacwdm.cpp`, the comment above
`ReadName`) and falls back to the INF's name; after `Prepare` (the transport's
open, the rates of `Learn`, which skips a format whose rates fail, the
level setup); after each pair's start; and last, before it returns
success. A latched expiry wins over a fallback name, partial formats,
`uac1`'s null-function success (`uacwdm.cpp`, `StartSteps`: its
success returns recorded as stages `0x40` and `0x41`) and any
late successful completion: `Start` tears down what it built and
returns `STATUS_IO_TIMEOUT` (Device Manager shows Code 10). It never
calls `IoInvalidateDeviceState`.

**What each entry point returns.**

| Entry | In the failed state |
|---|---|
| `NewStream` (a pin opened) | `STATUS_DEVICE_NOT_READY`, before its wait for a closing stream and before the Wave mutex |
| `Run` (RUN of a pin already open) | `STATUS_DEVICE_NOT_READY`, before the Wave mutex |
| `UacUsbStart` | the same, before its "already running" shortcut |
| `Halt`, `Close` | succeed; nothing sent; they wait for the Wave mutex like any stop (above) |
| A volume or mute SET (KS property) | accepted into the cached level, nothing written, counted; the saved level is still written, so the next plug restores it. A GET answers the cache. A failed SET would make mixers show errors for a device that is going away |
| `PowerChangeState` | nothing sent, either way |
| PnP stop, surprise removal, remove | nothing sent |
| Section 28's selector and level requests | refused by the same gate; 28's failure rules apply with no traffic |

**A stream running at the failure** (provisional: stopped, O5). Its
worker cancels and drains its transfers (above). Its client then sees:
the pin still in RUN for PortCls; `GetPosition` succeeding with a
position that no longer moves (render: the bytes fetched; capture: the
bytes written; `uacstream.h`, `UacStream::GetPosition`); no
notification, since notifications come only from completed transfers
(`uacstream.h`, `WriteBytes` and `CompleteBytes`). On 2000 and later the PnP failure path
follows (below) and PortCls's surprise-removal handling takes the
stream down; what an application gets then is that system's. On 98 SE
and ME nothing follows: a program waiting for its buffers to complete
may wait until it gives up or is closed. Not proposed: a position kept
moving with nothing sent, which hides the failure from the program and
is no clean indication of it. The client contract on each system is
read on the legs before the code is called ready (29.14), and the
choice stays open (O5).

**What is released, what stays pending.**

- Released in the driver at once: the streams' submissions (stopped),
  the records' clock claims and pipe handles, the probe's schedule.
- Pending until the lower driver completes: the expired request (its
  IRP, URB, buffers and event, and its caller's frame), and each
  worker's transfers until they are back; their slots are freed only
  then, as today.
- Released only by the bus or PnP teardown: the host stack's
  configuration of the device and the bandwidth of any alternate left
  selected, since no SET_INTERFACE(0) is sent; and on 98 SE and ME
  everything a client still references (the Wave, its streams, the
  device record), kept until the client closes and the device is
  removed.

**How Windows is told.**

- **The notifier.** The notification has its own actor, independent of
  every thread that may be blocked in an expired request, the alt thread
  included: a second system thread per device record on 2000 and later
  (29.5.1), created and joined as the alt thread is (29.5.4, 29.5.6). It
  sends no USB request and takes no transport, Wave or stream mutex, so
  nothing it does can wait on the lower driver. Woken by the hook, it
  calls the adapter layer's failure callback once (a callback given at
  open, holding references on nothing it does not own): the record
  (below), the event log entry (O7), and `IoInvalidateDeviceState`. On
  98 SE and ME, without threads (O14, option 1), the same callback is
  made once by the next passive entry that meets the failed gate while
  holding no transport or Wave mutex (`NewStream` and `Run` before the
  Wave mutex, `Close` before it, `PowerChangeState` before it, and
  `DispatchPnp`); there it may come late or, with no further entry, not
  at all before the remove, which that system's contract accepts.
- **The admission point.** The callback calls `IoInvalidateDeviceState`
  only after it has marked its call in progress under the gate lock
  while `Life` reads started: that mark is the call's admission point.
  `DispatchPnp` sets `Life` (tearing down, or surprised) under the same
  lock before anything else, so no call is admitted after `Life` has
  changed, and never before a start has succeeded; a call admitted
  before the change may still be executing when `DispatchPnp` changes
  it, and the teardown waits for it to finish (the notifier's join, or
  on 98 SE and ME the in-progress mark cleared) before the IRP goes on,
  so the PDO is still attached while it runs.
- **2000, XP, Vista, 7 and later.** The PnP manager then sends
  IRP_MN_QUERY_PNP_DEVICE_STATE, which the top of the stack handles first
  and passes down (Microsoft Learn, "IRP_MN_QUERY_PNP_DEVICE_STATE": a
  driver modifies the flags, it does not overwrite the bitmask).
  `DispatchPnp` ORs `PNP_DEVICE_FAILED` into `IoStatus.Information`,
  keeping the flags already there, sets the status to success, and hands
  the IRP to PortCls as before; while the device is not failed the IRP
  passes untouched. Whether PortCls passes the flags down unchanged is
  read on the legs (Device Manager's code). The PnP manager answers a
  function driver's `PNP_DEVICE_FAILED` with its surprise-removal
  handling (Microsoft Learn, "Handling an IRP_MN_SURPRISE_REMOVAL
  Request"); the final remove can wait indefinitely while a handle stays
  open. The documented resulting problem is Code 43, "Windows has
  stopped this device because it has reported problems"
  (`CM_PROB_FAILED_POST_START`). The mixer's and the Sound panel's
  behaviour, the default endpoint's reassignment and what applications
  get are version-specific, and each system's leg reads them.
- **98 SE and ME.** `IoInvalidateDeviceState` exists in NTKERN by name
  (stock 98 SE `usbhub.sys` imports it: xHCI98 design record 13, the
  import table's row 21, "evidenced"), so the one x86 binary loads. Those
  systems never send a surprise removal (the same Microsoft page), and
  whether NTKERN and the configuration manager query the state and show
  anything for `PNP_DEVICE_FAILED` is not established: the 98 SE and ME
  legs read Device Manager. Nothing here depends on a later surprise
  removal or query there: the failure itself stops the streams and the
  probe, drains what can drain, and keeps what clients reference (above).
  What the user meets: opens failing at once, the mixer still listing
  the device (its SETs cached), and the record in the driver key.

**How the state ends.** With the device record: an unplug (the PDO
removed), on 2000 and later a disable and enable in Device Manager, or
any new PnP start, each of which builds a new device record with the
flag clear. A device still wedged then fails its start (Code 10) at its
first expired request (above).

**The record in the driver key** (written, never read for behaviour;
`PutValue`, best effort, by the notifier, the alt thread or a passive
entry, never by the hook):

- `Uac2Failed` (REG_BINARY, little-endian DWORDs), written at the
  notification and again once the expired request has completed:
  `Version` (1); `Count` (failures recorded in this key: the previous
  value's count plus one, read with `GetValue`); `TimeLow`, `TimeHigh`
  (`KeQuerySystemTime`, UTC, for the report only: a new import, whose
  export on 98 SE and ME is not established here, so the import lists
  decide it, else 0); `Kind` (1 class
  request, 2 standard request, 3 SELECT_INTERFACE, 4
  SELECT_CONFIGURATION, 5 the probe of 29.10);
  `Setup0` (`bmRequestType` | `bRequest` << 8 | `wValue` << 16; for a
  select, interface << 8 | alternate); `Setup1` (`wIndex` | `wLength`
  << 16); `Status` and
  `UrbStatus` (the final ones, after the drain; 0 until then; `Status`
  is the lower stack's own completion status, `STATUS_CANCELLED` when the
  cancellation took the request, not the `STATUS_IO_TIMEOUT` its caller
  is given); `LateMs`
  (expiry to completion, the full-width interval divided before it is
  narrowed and saturated at `0xFFFFFFFE`; `0xFFFFFFFF` until it completes);
  `MsSinceAltChange`; `AltChanges10s` (alternate changes in the 10 s
  before); `Streams` (bit 0 playback running, bit 1 capture running,
  bit 2 a record lingering); `Build`. 14 DWORDs, 56 bytes.
- `Uac2Alts` (REG_BINARY, written when a record leaves `Lingering`, at a
  failure and at a stop): `Version` (1); `AltSelects`; `AltZeros`;
  `LingerHits` (starts that sent nothing); the ends of a linger by cause
  (`ByTimer`, `ByFormat`, `ByClock`, `ByPower`, `ByPnp`, `ByRemove`,
  `ByFailure`); `NotAdopted` (starts that found a lingering record they
  could not adopt: the geometry, an unclean end, the epoch); `Unknowns`
  (SET_INTERFACE failures that left `Unknown`); `HoldWaits`,
  `HoldWaitMsMax`; `SpacingWaits` (the symmetric hold's, 0 otherwise);
  `FirstLate` (29.4.8); `LingerRead`, `LingerMs`, `HoldRead`, `HoldMs`,
  `ProbeRead`, `ProbeMs`, `FailRead`, `FailOn` (each raw value read,
  `0xFFFFFFFF` when none was usable, and the value in force); `Probes`,
  `ProbeFails`; `Refused` (requests the gate refused); `OpensRefused`;
  `HostTimeouts` (host-local requests that expired: not failures).
- `Uac2UsbStats` and `Uac2UsbStatsIn` gain `LingerHit` (this start sent
  nothing) and `StartRequests` (control requests this start sent),
  appended after the last word in force when this merges (29.11, O12).

**What the user is told: drafts for the owner** (O7). Every string this
adds:

1. Event log entry, 2000 and later (a message table in the driver,
   registered as `EventMessageFile` by the INF's service install
   section; `IoAllocateErrorLogEntry` and `IoWriteErrorLogEntry`, which
   stock 98 SE drivers also import, xHCI98 record 13 rows 41 and 42, so
   the binary loads there; 9x has no event log), error, `%2` the device's
   name: "%2 did not answer a request in time and has been stopped.
   Unplug it and plug it in again."
2. The same, when the probe found it while playing (29.10): "%2 did not
   answer a request in time while it was playing, so its sound stopped.
   Unplug it and plug it in again."
3. README, troubleshooting: "If a USB audio device goes silent, or
   Windows says it has stopped it (Code 43 in Device Manager), and it
   comes back only after you unplug it and plug it in again, the device
   did not answer the computer in time. The driver then stops sending to
   it and records it in the device's driver key (`Uac2Failed`). Unplug
   it and plug it in again. If it keeps happening, send that key and the
   settings `Uac2LingerMs` and `Uac2HoldMs` with your report."
4. README, settings list: one line each for `Uac2LingerMs`,
   `Uac2HoldMs`, `Uac2FailOnTimeout` and `Uac2ProbeMs`: name, default,
   range, what 0 does, when it is read (29.8), and that all four at 0
   give the behaviour before this change.
5. Release notes (worded for the provisional symmetric hold, O2): "uac2
   and uac1 keep a stream's alternate setting selected for 2 s after it
   stops (`Uac2LingerMs`), so programs that start and stop sounds
   quickly no longer switch the device's interface at each one, and
   keep at least 200 ms between two switches of one interface
   (`Uac2HoldMs`). A device that does not answer a request within 5 s
   is now stopped at once, instead of every later open waiting 10 to
   15 s (`Uac2FailOnTimeout`)."
6. The INF comment lines for the four values.

Device Manager's own text (Code 43) is Windows'. Not used: a
problem description of the driver's own (`IoSetDevicePropertyData`
exists from Vista only, 21.2), and renaming the device at run time (7
caches an endpoint's name until it arrives again, 21.4).

### 29.10 The quiet form: a device that stops answering while its stream runs

In the silence report `uac2` went on submitting the isochronous stream
(an OUT packet has no handshake: nothing fails on the host side) while
the device answered no control request, and `uac2` sent none for over a
minute. The save shows the host's submissions; whether the device took
the packets is not known. Without control traffic the condition cannot
be seen; for an asynchronous device the feedback watch of section 25
sees a feedback endpoint that stops, but the field adapter is
synchronous.

- **The probe** (provisional: off by default, `Uac2ProbeMs` 0, and on
  only in the reporting user's test build, O8). One schedule per device,
  run by the alt thread, only while at least one stream of the device is
  `Active`: GET_STATUS to the device (USB 2.0 9.4.5), one at a time, the
  next due an interval after the previous one completed, so none
  overlaps and none is made up after a delay. Just before each, under
  the spin lock: a stream still `Active`, the gate open (D0, started,
  no teardown), not failed. It stops at the last stream's hand-back and
  at every teardown, and the thread's stop drains it. An expiry is the
  failed state with `Kind` 5 and event log string 2; the alt thread
  then stays in that request until the lower driver completes it, and
  the notifier reports the failure meanwhile (29.9). Without an alt
  thread (98 SE and ME, O14 option 1) there is no probe.
- **What it shows.** That the control endpoint answers a standard
  request; nothing about the audio path, and a device's standard and
  class requests may be handled by the same firmware. GET_STATUS is
  answered in the Address and Configured states; in the Default state
  its behaviour is not specified (9.4.5), and a streaming device is not
  in it.
- **Detection time.** The interval, plus the wait for the device mutex
  behind other requests, plus the 5 s deadline; the failure is published
  at the expiry (29.9). Not a guaranteed bound.
- **Does it risk provoking the device?** Not known either way. In the
  silence report the control endpoint stopped with no request of
  `uac2`'s for about 70 s before: traffic was not needed that time. That
  does not show added requests cannot trigger a failure, and their rate
  against a volume slider's is no evidence that a device tolerates them.
  Hence off by default.
- **Options (O8):** off by default and on in the test build
  (provisional); on for every device at 10 s; or a class GET_CUR (the
  clock's validity) instead of GET_STATUS, which tests the audio path
  but involves it.

### 29.11 Where this meets section 28 (the recording controls)

Section 28 is not changed here. Both sections are on main, each only in
part (28.15's engine move and 28.18's stage 1; 29.17's failed state).
The points of contact, for
the owner and for whichever of the two next adds what is listed here:

- 28.8 reads every selector "at each capture RUN, before the stream
  starts". A capture start inside a linger would still send those
  reads; proposed (O11): skip them on a start that adopts a lingering
  record only while that record's clock and selector claim has stayed
  valid since its selection. Any selector write the driver allows in the
  meantime (28's own, or another stream's start) ends that, and the
  reads go out. A device-side switch is found at the next MUX GET, as
  28.8 already accepts.
- 28's requests go through the same request paths and the same device
  mutex, so the failed state's gate refuses them at once (29.9); its
  retry rules then cost no traffic.
- The stats words: 28 takes `Uac2UsbStats` words 94 to 99 (28.12); this
  section appends after them (29.9). Neither has added its words yet
  (28.18, 29.17), and words 94 to 97 are already in use (`uacusb.h`,
  `UAC_USB_STATS`: 25.7's `FeedbackOff`, 26.13's `Resyncs`, `ResyncFrame`
  and `ResyncBehind`), so 28.12's numbers no longer hold as written.
  Whichever adds its words second renumbers, keeps the word indices in
  one header, and tests every reader of the stats against it (O12).
- `uacemu.py` gained a control log with each request's data stage with
  section 28's work, now on main; the device models of 29.14 are added on
  top of whichever `uacemu.py` is current when the code is written.

### 29.12 Code shape

- `uacalt.c` / `uacalt.h` (common, new; compiled into both packages):
  the record's state machine as plain C with no kernel calls, so the
  host tests run it: given a record, an event, the time, the gate and
  the settings snapshot, it returns the actions (switch to 0 now, select
  N, wait until T, wake the thread at T, adopt, end and select, nothing)
  and the new state; the deadline, the adoption conditions (29.4.4), the
  table of failed steps (29.4.6), the clamping of the settings; the alt
  thread's scan (what is runnable, the next wake, 29.5.4) and the
  ownership rules, as pure functions. The hold's direction is a
  parameter. Written and host-tested (`alttest.c`, 246 checks of
  decisions, not of kernel lifetime or locking); no driver file includes
  or calls it yet. Its header states the adapter's contracts, which the
  wiring must keep: the non-wrapping 64-bit millisecond clock from the
  full interrupt time (29.4.1); the gate, not the record, as the
  authority for the failure, a late success never authorising a request
  or a start (29.4.6); `Gen` as bookkeeping, with ownership held across
  each decision, wait, request and completion (29.4.1, 29.5.3); the
  geometry passed at a successful selection being what the pipes retain
  (the `MaximumTransferSize` asked, the `MaximumPacketSize` returned,
  per pipe; 29.4.4); the start's commit as a predicate whose locked
  steps are the adapter's (29.5.7); the conflict mask's limit of 32
  records, its discovery, candidate, eviction round and second walk
  being the adapter's (29.4.7).
- `uacusb.c`: in `UAC_DEV` the records, the list of streams, the gate's
  pointer, the alt thread and the notifier; a stream borrows a record's
  pipes and claim instead of owning them; the claims under the gate lock
  and the stream count interlocked (29.5.2); the walk's three-way
  `PathFree` and its kept candidate (29.4.7); `CallAndDrain` with the
  expiry hook and each request's kind (device or host-local); the timed
  acquisitions and per-request admission of 29.5.8; the start's commit
  (29.5.7); GET_STATUS for the probe; the first batch at explicit frames
  on an adopted record (29.4.8); `StopLocked` without SET_INTERFACE(0)
  when the gate says so (29.6). `UacUsbOpen` takes the gate and starts
  the threads (on 2000 and later, O14); new `UacUsbQuiesce` (the gate's
  steps of 29.6) and `UacUsbEndRecords`.
- `uacwdm.cpp`: `UAC_GATE` and its lock in the extension, set up at
  AddDevice; `Start` reads `Uac2FailOnTimeout` and `Uac2ProbeMs` before
  `ParseDevice`, reads the configuration descriptor through the
  owned-IRP path when the setting is on, and tests the startup latch
  after each step (29.9); `NewStream`'s gate-aware `WaitClosing` and
  timed Wave mutex (29.5.7); `Run` reads the
  linger and the hold beside `Uac2Realtime` and passes them down;
  `NewStream` and `Run` check the gate first; `PowerChangeState` calls
  the transport before and after it visits the pairs; `DispatchPnp` sets
  `Life` and the gate before `ShutdownPairs`, ends the records after
  it, and handles QUERY_PNP_DEVICE_STATE while failed; the failure
  callback (the record, `IoInvalidateDeviceState`, the event log entry).
- INFs: the four values; on the NT service sections the event log
  registration (O7). `infchk.ps1`; both import lists
  (`IoInvalidateDeviceState`, `IoAllocateErrorLogEntry`,
  `IoWriteErrorLogEntry`).

### 29.13 What this does for the four failures, by the notes

Reading each save's timeline against the deadline rule (`L` 2 s, `H`
200 ms). The saves did not record every session's alternate and rate,
so each line below assumes that the format, the rate and the terminal
stayed the same and that no power change or clock conflict came between;
each is conditional on that.

- **The newest (system sounds, 0.15 to 0.39 s apart for 13 s):** each
  start came within a few ms of the previous stop, inside the linger, so
  one SET_INTERFACE(1) at the first sound and one SET_INTERFACE(0) 2 s
  after the last: 2 alternate changes instead of 122, and none of the
  starts' clock and mute requests after the first (five a start by the
  code, 29.1: about 300). The fatal SET_INTERFACE(0), 0.27 s after its
  alternate, would not have been sent then. The hold alone (linger off)
  would not have changed it: its alternate had been selected for longer
  than 200 ms; the symmetric hold alone would have spaced every restart
  200 ms after the alternate 0 before it.
- **The first (closes 0.58 to 2.2 s apart in clusters, the fatal one
  0.68 s after the previous):** inside the linger; no alternate change
  at the fatal point.
- **The second (after 329 s of quiet, a session of about 40 ms):** not
  inside any linger; its alternate 0 is deferred, not avoided: 2 s after
  the stop instead of about 40 ms after the alternate.
- **The third (five 36 ms cycles behind a hub):** inside the linger; no
  alternate change.

In each, the request that went unanswered would not have been sent when
it was. Whether the device would then have kept working is not
established.

### 29.14 Tests

**Host tests of the state machine** (`alttest.c`, in `test.cmd`;
`uacalt.c` with an injected clock and a recorded action list). They
check decisions, not kernel lifetime, IRQL or cancellation, which the
interleavings and the legs below cover.

- every row of 29.4.5 and 29.4.6; the deadline at its boundaries (a
  stop at `AttemptAt + H - 1`, `+ H`, `+ H + 1`; `L` 0 with `H` running;
  `H` 0 with `L`); adoption refused for each condition of 29.4.4; the
  clamping and normalisation of each setting (0, 1, 1999 and 2000 for
  the probe, the top, the top + 1, `0xFFFFFFFF`, nothing read);
- all four settings at 0: the action list equals today's sequence
  request for request, for the rapid open and close pattern and for a
  pause and run;
- idempotent stops: `Halt` then `Close`; `Close` of a paused pin (its
  deadline unchanged); a power drain, then the client's STOP; the
  start's own leading stop;
- one snapshot: a setting changed between RUN and stop, and while a
  deadline is set (the deadline unchanged);
- the symmetric hold: a format change with a hold running (the wait,
  alternate 0, the second wait, N: up to `2H`); a start after an
  alternate 0 (the spacing wait);
- clock conflicts with a lingering claimant (the eviction of 29.4.7)
  and with an active one (`STATUS_DEVICE_BUSY`); two pairs lingering at
  once;
- the failed state: an expiry in each state; afterwards no action that
  sends; every entry point's result (29.9); the record's fields; the
  probe's schedule while streaming, its absence while not, no overlap
  and no burst after a slow GET_STATUS.

**Deterministic interleavings** (a host harness that runs the
transport's ownership, gate and alt thread logic over injected
primitives, each step forced in a chosen order; in `test.cmd`):

- a hand-back racing the thread's scan; the thread waking while a start
  owns the record; a start waiting while the thread owns it; repeated
  re-arming; an adoption, a stop and a re-arm in turn; a teardown in
  every interval of each of these;
- an expiry while each lock of 29.5.2 is held: by a level write, a
  selector request, the probe, the alt thread's SET_INTERFACE(0),
  another pair's start; every waiter in a gate-aware wait (29.5.7, 29.5.8)
  then ends with `STATUS_DEVICE_NOT_READY` while the expired request
  stays blocked, the waiters 29.9 lists stay blocked with it, and the
  blocked caller returns only when its request completes;
- an expiry during the PnP start, a power-down, a PnP stop and a
  remove; a start failing at each step of 29.4.6; repeated `Halt` and
  `Close`;
- a power change and a PnP stop racing starts on several pairs (the
  gate closed before the first pair is visited);
- an allocation failure, a thread that cannot be created, a thread slow
  to run, and a teardown's stop right after the thread's last action;
- the alt thread's probe, and its lingering alternate 0, expiring with a
  cancellation that never completes: the notifier still writes the
  record and calls the failure callback, and the teardown's join waits
  for the lower driver;
- a thread preempted after its ready or `Exited` event and before it
  ends, and a creation callback preempted after its `Done` and before it
  returns, each followed at once by a teardown: on 2000 and later the
  join holds the teardown until the thread object is signalled; on 98
  SE and ME the case records the window O14 describes, not a pass;
- a failure between a start's early gate check and its taking of the
  Wave or stream mutex, and between an expiry's stop mark and the
  start's commit: the start fails, the mark stands, nothing is sent;
- a `NewStream` waiting for a closing stream, and a `Run` waiting for
  the Wave mutex, when the failure is published: both refuse at their
  next try; a caller already inside the lower driver's call stays until
  it returns (29.9);
- a conflicting path rejected by today's `PathFree`: the walk keeps it
  as the candidate, a free path found later wins, and with none the
  lingering owner is evicted and the second walk claims; the shared
  source at 48 kHz lingering against a capture at 44.1 kHz; a second
  candidate after eviction ends in `STATUS_DEVICE_BUSY`;
- an expired deadline whose record stays owned, and deadlines held back
  below D0 or while quiescing: the thread waits without a time-out or
  to the next runnable deadline, never in a loop of zero-length waits;
- a startup expiry swallowed by an advisory step (`ReadName`'s fallback,
  `Learn` skipping a format, `uac1`'s null function), and one followed
  by a successful completion: `Start` fails with `STATUS_IO_TIMEOUT`;
- each device-mutex site of 29.5.8 refused before submission: no
  request sent, the record and `AttemptAt` unchanged, the walk storage
  freed, no partial rates returned;
- a first batch late in part, on the data pipe and on the feedback
  pipe: counted in `FirstLate` and resynced (26.13), which today's
  aggregate-loss handling does not do on its own.

**The simulator** (`uacemu.py`; the device models become committed,
documented options; `uacsimtest.py` covers each option without a guest).
It reproduces the request-density trigger hypothesis, not the field
device or the controller's wedge:

- `--stop-after-alts N --stop-window-ms T`: the device stops answering
  every control request after `N` alternate changes within `T` ms, stays
  connected and keeps accepting the isochronous stream (the scratch
  model of the record, made an option).
- `--stop-alt0-within-ms X`: it stops when an alternate 0 arrives within
  `X` ms of an alternate N; `--stop-altn-within-ms X`: when an alternate
  N arrives within `X` ms of an alternate 0 (the newest report's
  millisecond spacing).
- `--stop-sessions K --session-max-ms S --session-gap-ms G`: the newest
  report's recipe: it stops after `K` sessions in a row each streamed at
  most `S` ms (default 400) and begun within `G` ms (default 400) of the
  alternate 0 before it. With the linger on no SET_INTERFACE separates
  two sounds, so sessions are also told apart by the gap in isochronous
  packets.
- `--stop-at-request N` and `--stop-after-s S`: it stops regardless (the
  forced stop of the failed-state legs), at the `N`th control request or
  `S` seconds after the first stream.
- `--delay-request N --delay-ms D`: the `N`th control request answered
  after `D` ms (4900 to 5100 around the deadline, and an answer just
  after the cancellation); `--stall-request N` and
  `--xact-error-request N`.
- The control log tells a request on the bus from the host's
  cancellation of one.
- `--rough-check` (always on, logged): a stream whose first 8 ms of
  packets do not match the selected alternate's nominal cadence within
  a cumulative tolerance (8 ms of a fractional rate need not hold its
  exact long-term ratio), or carry partial frames, is logged as rough;
  with `--pcm` and a known test signal the received samples of each
  session, by packet gap, are checked for data that did not belong to
  it.
- `--rough-every N --rough-kind KIND`: one start in `N` the device
  shows a condition the driver can see: `clock-invalid` (its validity
  reads 0 for that start) or, on an asynchronous profile,
  `feedback-skew` (its feedback off by 1 % for the first 100 ms). Neither
  is the field roughness, which is not known; they exercise `uac2`'s
  handling of a start that goes wrong. Plus a feedback and a capture
  stream restarted after a linger.
- Section 28's selector and level requests against a lingering record,
  once section 28's stage that makes them is wired (28.18).

**Host driver injection** (xHCI98's own work, arranged separately): a
cancellation completed late, or never; a stale ASAP schedule after an
idle pipe; a request held by a controller recovery.

**The rig's test programs** (committed with `src/audio/uac2`, built by
its `build.cmd`): the rapid open and close program of the record
(`waveOut` open, 5 ms written, reset, close; the DirectSound variants)
and a system-sounds program (a 250 ms WAV played with `PlaySound` and
with `waveOut`, again and again about 0.3 s apart, and as fast as
possible).

**The joint legs** (after the code). Windows 7 SP1 x86 and x64, XP SP3,
Vista, 2000, 98 SE and ME as far as the rig allows, each under the host
controller drivers it runs: Windows' own UHCI, OHCI and EHCI drivers,
NUSB's EHCI on 98 SE, and xHCI98 (under QEMU with xHCI98's QEMU build).
Other xHCI drivers are named where a leg runs them and are otherwise
listed as unverified.

1. **Baseline**, released `uac2` 0.1.12.0, the system-sounds program and
   `--stop-sessions`: the device stops answering; opens fail after 10 to
   15 s each while `uac2` keeps sending; with the host driver's injection
   (built by xHCI98's own work, arranged separately) its present handling
   takes the controller down.
2. **With the change, defaults:** the system-sounds program: the
   simulator's log shows one SET_INTERFACE(N) at the first sound, none
   during the run, and SET_INTERFACE(0) no sooner than 2 s after the last
   stop; the stop condition is never reached; playback correct (the
   rough check clean, `--pcm` content checked). The rapid open and close
   program: no alternate change inside the linger and none sooner than
   the hold, read from the log's times.
3. **The forced stop** (`--stop-at-request`): `uac2` enters the failed
   state at the first expiry; the simulator logs no request after it;
   opens fail at once (each `waveOutOpen`'s time, against 10 to 15 s
   before); Windows is told (the Device Manager code read on each NT
   system; whatever 98 SE and ME show); `Uac2Failed` and `Uac2Alts`
   read; what a program playing at the failure gets, on each system
   (O5); the same with a handle left open on 98 SE and ME; the simulated
   device unplugged and plugged again (QMP `device_del` and `device_add`
   of its `usb-redir`) works again.
4. **With the host driver's new handling on** (xHCI98's change): the
   forced stop again: only the audio device fails; the other devices on
   the controller (a simulated tablet and a disk) keep working.
5. **The quiet form,** with the probe on (the test build's setting): the
   simulator stops answering in the middle of a stream; the probe finds
   it after its interval, the wait for the device mutex and the
   deadline.
6. **Regressions:** all four settings at 0 give the baseline's request
   sequence; an exclusive-mode format change on 7 (alternate 0 after the
   hold, then the new alternate after the second hold); playback and
   capture at once on a shared clock.
7. **Lifecycle:** sleep, hibernation and resume during a linger and
   while streaming (no record survives; the next start sends
   everything); disable and enable; a PnP stop and restart where the
   system offers one; a removal during a linger (no request after it, no
   crash), on 98 SE and ME an unplug, which arrives as a remove alone.
8. **The restart of an adopted pipe** on each stack: the explicit first
   frames accepted, `FirstLate` read, the start of a short sound present
   in the `--pcm` capture; the same with ABORT_PIPE before the first
   batch, to compare (29.4.8).
9. **Bandwidth:** a second device that needs periodic bandwidth (a
   second simulated audio device, or a webcam under `uvc98`) opened
   during a linger: whether it is refused, and whether it succeeds when
   opened again after the linger ends.

**What the emulation cannot show:** the real chip's firmware, whether a
kept alternate keeps it answering, whether the rough playback is the
device's, anything electrical, and the timing of real hardware (QEMU's
TCG guests run far slower, record section 4).

### 29.15 What only the real adapters can show

A test build for the reporting user (provisional, O13), in stages, each
with the request to repeat the system-sounds recipe on the same port and
send the driver key (`Uac2Failed`, `Uac2Alts`, `Uac2UsbStats`) and a
host driver report:

1. the mechanism at its defaults, the probe off;
2. then, as a separate comparison, the same with the probe on
   (`Uac2ProbeMs` 10000);
3. then the two discriminating tests below, with the same recipe and
   the released driver, on Windows 7 x64 as the reporter runs it.

- **U1:** the same adapter on a port run by Windows' own USB 2.0 driver
  (an EHCI port on the same board) instead of xHCI98, same `uac2`: does
  it still stop? A useful comparison of hypothesis 3 against 1 and 2;
  alone it separates neither cleanly.
- **U2:** another make of USB Audio 2.0 device on the same port: does it
  survive? Useful for 1 against 2; alone not conclusive either.

### 29.16 Open points for the owner

**Decided (owner, 2026-11-17):** D1, the linger, about 2 s; D2, a
minimum hold, about 200 ms; D3, a clean failed state.

**Decided later the same day (owner, 2026-11-17):**

- **O2:** the hold applies in both directions: no two alternate changes
  of one interface closer than `H`; a format change can wait up to `2H`.
- **O5:** a stream running at the failure is stopped. What its client
  sees is read on each system's legs, 98 SE above all, before release.
- **O7:** an event log entry in the driver's own words on 2000 and
  later, plus README and release-note text; on 98 SE and ME the README
  only. Every string stays a draft for the owner.
- **O8:** the probe is off by default and on only in a test build. The
  feature is kept.
- **O13:** a test build for the reporting user: not yet. The owner
  decides after the emulator results; nothing is prepared for it.
- **O14:** the linger and the probe are off on 98 SE and ME for now, and
  no new thread is created there; those systems behave as today, and the
  failed state still applies there, noticed at the next call into the
  driver. This is not the option the text below recommended (a linger
  without a timer): where 29.5.6, 29.9, 29.10 and 29.14 are written with
  that earlier provisional choice they are to be brought to this
  decision.

The integrated step of 29.17 does not yet carry O14's failed state to 98
SE and ME: there, nothing of the feature is active in this step, and how
the failed state reaches those systems is OPEN (29.17).

The points below keep their text as it was written before these
decisions, each with its recommendation first; O2, O5, O7, O8, O13 and
O14 are settled as above, and the rest is open and written into the
section provisionally.

- **O2, the hold's direction. OPEN FOR THE OWNER.** Recommended: both
  directions, 200 ms: no two alternate changes of one interface closer
  than `H`; the newest report's changes were alternate N a few ms after
  alternate 0. Its costs: a format change can wait up to `2H` (29.7),
  and the hold spaces alternate changes only, not the clock and mute
  requests between them. Or: alternate N to 0 only.
- **O8, the probe's default. OPEN FOR THE OWNER.** Recommended: off by
  default; on only in the reporting user's test build (GET_STATUS every
  10 s while streaming), as a separate comparison (29.15). Or: on for
  every device; or a class GET_CUR instead of GET_STATUS.
- **O5, a stream running at the failure. OPEN FOR THE OWNER.**
  Recommended: stopped at once, its position frozen and its
  notifications ended (29.9), with what its client sees read on each
  system's legs, 98 SE and ME above all, before the code is called ready.
  Or: left running until its program stops it. Not recommended: a
  position kept moving with nothing sent.
- **O7, the message to the user. OPEN FOR THE OWNER.** Recommended: an
  event log entry with the driver's own text on 2000 and later (a
  message table and the INF's event log registration), plus the README
  and release-note drafts of 29.9, all saying that a request was not
  answered in time rather than that the device is broken. Or: Windows'
  generic time-out text from `IoLogMsg.dll` (no resources of our own,
  no advice to replug); or none. On 98 SE and ME only the README can
  say it. Every string is a draft.
- **O13, the reporting user's test build. OPEN FOR THE OWNER.**
  Recommended: in stages (29.15): the mechanism at its defaults first,
  then the probe as a separate comparison, then U1 and U2, on Windows 7
  x64. Or: one build with everything on.
- **O14, threads on 98 SE and ME. OPEN FOR THE OWNER.** No mechanism
  documented as available there has been found that observes a system
  thread's end or keeps the image resident until it has ended (29.5.6),
  so no rundown of a new thread can be shown. Recommended: no alt thread
  or notifier there; the linger without a timer (alternate 0 at the next
  event that needs it), no probe, the failure noticed by the next
  passive entry. Or: both threads with the stream worker's `Exited` and
  10 ms practice, as a stated unproven residual; or the linger and probe
  off there. Also whether the existing residuals (the stream worker on
  98 SE and ME; `CreateWorker` and the executive work items on x86) are
  to be addressed, which this section leaves as they are. Until decided,
  integrated transport code is written for 2000 and later only.
- **O1, the ranges. OPEN FOR THE OWNER.** The defaults are D1 and D2;
  proposed ranges 0 to 10000 ms for the linger and 0 to 1000 ms for the
  hold (29.8). Of the four failures the linger avoids the fatal change
  in three and only defers it in the fourth (29.13).
- **O4, what counts as stopped answering. OPEN FOR THE OWNER.**
  Recommended: one expired 5 s deadline of a request to the device,
  recorded at the expiry, accepting that a slow host can fail a healthy
  device (29.9). Or: an expiry confirmed by a second request; or
  transaction errors as well.
- **O6, telling Windows on 2000 and later. OPEN FOR THE OWNER.**
  Recommended: `IoInvalidateDeviceState` and `PNP_DEVICE_FAILED`, from
  the alt thread, with the lifecycle guards of 29.9. Or: only failing
  the entry points and leaving the device in place.
- **O9, `Uac2FailOnTimeout`. OPEN FOR THE OWNER.** Recommended: a
  setting, 0 restoring today's behaviour after a time-out; all four
  settings at 0 for the exact baseline (29.8).
- **O3, a pause after each SET_INTERFACE. OPEN FOR THE OWNER.**
  Recommended: none for every device. Linux applies its 50 ms only per
  device, through a quirk flag (`QUIRK_FLAG_IFACE_DELAY`, 29.2); the
  linger and the hold do not replace a settle between alternate N and
  the first packet, so if rough starts persist with the linger, a
  setting in the test build for the reporting device alone.
- **O10, the restart of an adopted pipe. OPEN FOR THE OWNER
  (confirmation).** Changed by this revision: an explicit start frame
  for the first batch on each pipe (29.4.8) instead of ABORT_PIPE first,
  which stays only as a precaution a leg shows to be needed.
- **O11, section 28's selector reads at a capture RUN inside a linger.
  OPEN FOR THE OWNER.** Recommended: skipped on an adoption only while
  the record's claim has stayed valid; any permitted selector write ends
  that (29.11). Agreed with that work.
- **O12, the stats words. OPEN FOR THE OWNER.** Recommended: appended
  after section 28's and renumbered by whichever adds its words second,
  the indices in one place and every reader tested (29.11).

### 29.17 The integrated step: the failed state alone (2026-11-17)

What the driver carries of this section so far, so that nothing above
is read as built when it is not. Applies to `uac2` and `uac1` alike.

**Scope.** Only the failed state (29.9) with its gate (29.5.5),
admission (29.5.7, 29.5.8) and notifier. Not integrated: the linger,
the hold, the probe, the alt thread, the records of 29.4 and `Uac2Alts`;
`uacalt.c` is not wired.

**Default off, Windows 2000 and later only.** The gate is active only
when `IoIsWdmVersionAvailable(1, 0x10)` holds and the driver key's
`Uac2FailOnTimeout` reads exactly 1 at the PnP start. No INF writes the
value, so a device behaves as before unless a test writes it; a missing,
malformed or other value is off. This is a staging rule for this step,
not 29.8's eventual default of 1, and it must survive the wiring of
`uacalt.c`: `UacAltFailOn` turns a missing value (`0xFFFFFFFF`) or any
value but 0 into on (`UAC_FAIL_DEFAULT` 1), so its normalisation is not
to decide this setting until the owner enables the feature; until then
the driver's own "exactly 1" stays the test.

**Not complete, not to be enabled.** The owner chose (O7) an event log
entry in the driver's own words on 2000 and later and README and
release-note text (29.9, drafts 1 to 5). None of it is implemented: no
message table, no event log registration in the INFs, no README or
release-note text, no INF line for any of the four settings. Until they
are, the failed state is an incomplete, experimental foundation: it is
not to be enabled by an INF or a default, and not to be described as the
owner's feature.

**98 SE and ME.** Nothing of the feature is active there in this step:
the gate is never active below WDM 1.10, no notifier is created, nothing
is refused, and no device-state call is made. The feature stays
inactive and the old functional paths remain; the gate's own setup and
its inactive checks still run there and refuse nothing (so "no new code
runs there" would not be true). 29.16 records the owner's decision that
the failed state applies on 98 SE and ME, noticed at the next call into
the driver (O14); this step does not do that. **How the failed state
reaches 98 SE and ME (which passive entries notice it, what they record,
what refuses there): OPEN, still to be designed.** The binary's import
of `IoInvalidateDeviceState` is called on 2000 and later only; its
presence on 98 SE and ME rests on the import evidence of 29.9 and needs
this binary's loader legs there.

**As built, against the text above.**

- The notifier is created by the PnP start right after the gate's reset,
  before the start's first request (the configuration descriptor), not
  by `UacUsbOpen`; Life stays starting until the start has succeeded, so
  a startup expiry is recorded and no device-state call is made for an
  unstarted device. Every failed start joins it. Each wake reads the exit
  flag first and writes a pending record, first or final, before it
  honours the exit (`UacNotifyStep`), so a join never discards it.
- The startup latch (29.9) overrides every outcome of a start's step
  (`UacStartVerdict`): after `ParseDevice`, `ReadName`, `Prepare` (before
  `uac1`'s null-function test), pair 0's ports and power registration,
  each later pair's `Prepare` and ports, and last.
- Gate-aware acquisition (`UacGateAcquire`) reads the gate before each
  100 ms try and once after acquiring; the Wave operation mutex in
  `NewStream`, `Run` and a level SET (`Apply`, and with it the level work
  item `Shutdown` waits for) is taken so.
- The admission rule is the one 29.5.8 states as built (`GateAdmit`;
  `FrameSend` and `SubmitSlot` at the stream lock).
- `Uac2Failed`: `Status` is the lower stack's own completion status;
  `LateMs` is saturated at `0xFFFFFFFE`; `TimeLow`, `TimeHigh`,
  `MsSinceAltChange`, `AltChanges10s` and `Streams` read `0xFFFFFFFF`
  (not recorded in this step); `Build` is 29 for `uac2`, `uac1`'s own
  build number for `uac1`.
- A volume or mute SET on a failed device stays in the cache and writes
  nothing, as 29.9's table says, but in this step the saved level is not
  written either (the SET is refused before the operation mutex, which
  the hung request's thread may hold).

**Tests of this step.** `failtest.c` (in `test.cmd`) runs the decisions
`uacgate.h` gives the driver: the acquisition, the notifier's wakes and
join, the record's status and `LateMs`, the latch. The kernel's waits,
locks and threads are read and run on the legs (the dated records under
`docs/testing`), with the simulator's forced stop (29.14) as the device.

## 30. A remove while a pin is open, on 98 SE and ME (2026-11-18)

### 30.1 What was seen

On 98 SE in QEMU, removing a simulated device while `uac2` recorded from
it ended in "A fatal exception 0E has occurred at 0028:FF018C77 in VXD
ks(05) + 00004C17", or in a stall: the application's next WinMM call
(`waveInReset`) did not return within the observation window and
Explorer's clock stopped, while the processor and the driver's own
threads went on ([uac-98se-removal](../testing/2026-11-18-uac-98se-removal.md);
first seen in leg 5 of the guest legs of section 28.24). Idle removals
under NUSB's EHCI ended the same two ways.

### 30.2 What the markers observed, and the explanation proposed

98 SE and ME send IRP_MN_REMOVE_DEVICE with no IRP_MN_SURPRISE_REMOVAL
before it, while the system's audio components may still hold the
device's pins and filters (Microsoft Learn, "Understanding When Remove
IRPs Are Issued"). The driver's own part of the remove (29.6) runs, then
it passes the remove to PortCls.

**Observed** (lifecycle markers of a diagnostic trace build, four
`x4full` removals while recording under NUSB's EHCI):

- after the forward, another thread sent the open pin state requests
  (RUN to PAUSE, ACQUIRE, STOP), then its cleanup and close, then the
  cleanup and close of its filter;
- a final release of a `Wave` was logged (the marker carries no thread);
- where the client's pin close returned before that final release, the
  remove returned (1 removal); where the final release came first, either
  the pin close did not return and the fault screen came up (2), or the
  client's first state request returned `STATUS_PENDING` without reaching
  the driver's `SetState` and no further marker came in the observation
  window (1);
- the driver had no transfer or work item outstanding: its transport was
  drained and destroyed before the forward.

**Explanation proposed, not established:** the outcome depends on whether
the client's teardown of the pin and filter completes before PortCls's
remove releases the subdevices. The traces do not show PortCls's or KS's
internals (their locking, which object the fault touched, why a request
stayed pending). The same ordering for idle removals, the other devices
and the other moments is inferred, not traced. One variant withdrew the
pairs' interfaces before the forward and waited 10 s: no client closed a
handle in that time (1 trial).

### 30.3 The mitigation

On a remove with no surprise removal before it (below WDM 1.10: 98 SE
and ME), the driver delays PortCls's remove at the one place it was seen
to call back into the driver early in it, its first release of a
miniport (`Wave` or `Topology`), on the PnP thread, at PASSIVE, once per
remove. It waits while:

- a stream of the device is not yet destroyed (counted from its
  publication by `UacStream::Create` to the end of its destroy; 30.6), or
- a create or close is executing in PortCls's dispatch (counted from the
  wrapper's entry to its return, failed and pending creates included), or
- one of those ended less than `UAC_REMOVE_QUIET_MS` (250 ms) ago,
  counted from the later of that and the hold's start;

and never longer than `UAC_REMOVE_HOLD_MS` (5 s). `UacPlanRemoveHold`
decides; `RemoveHold` (`uacwdm.cpp`) polls every 10 ms. The hold is not
taken at all (`UacPlanRemoveHoldTake`, read once at its start) when the
last device power state PortCls gave the device (`PowerChangeState`) is
not D0 even after the driver asked for D0 before the forward, or when
nothing of the device is left to tear down: no stream, no open handle, no
create or close executing (30.6). Open handles are
counted and traced, not waited for: a handle a client keeps (one did:
opened with the recording and not closed at the remove) does not hold it.

This is a bounded timing mitigation, not a teardown barrier:

- a client that starts after the quiet time, or pauses more than 250 ms
  between its pin close and its filter close (or anywhere in its
  teardown), can still meet the order the markers saw fail, and nothing
  excludes a request arriving right after the last poll;
- at the 5 s cap the remove goes on whatever is left, so a stream or a
  close still in progress then meets that order too; the cap only keeps
  the wait from being endless;
- the wait delays the PnP thread; a client that needed that thread to go
  on would be released only by the cap.

Each device's accounting entry is allocated at AddDevice, before
`PcAddAdapterDevice`: a device for which none can be allocated is not
added (`STATUS_INSUFFICIENT_RESOURCES`), so no device runs without the
mitigation. The entries are in a list outside the device extension; a
remove's forward marks its thread there, and the entry is unlinked and
freed when PortCls's remove returns, so a close or destroy that comes
later finds nothing and touches nothing. The list holds no reference on
the device object and no generation number: an entry is found by the
device object's address, which relies on the device's own lifetime (a
new device at the same address before a late close of the old one would
be counted against the new one). On 2000 and later the remove follows a
surprise removal and nothing waits; the counting runs everywhere. The
trace flavour writes one line per hold (`rmhold`: streams and executing
operations, open handles, milliseconds held) and one per hold not taken
(`rmskip`: the same counts and whether the device was not in D0).

### 30.4 What it does not change

The driver's own teardown (29.6) is as before; nothing is sent to the
device at a remove. A remove of a device with no stream, no handle and
nothing executing does not wait; one with an open handle and no client
activity waits the quiet time once (250 ms). ME is covered by the version
test but was not run.

### 30.5 Tests

`desctest.c`: `UacPlanRemoveHoldTake`'s cases (a device in D0 with a
stream, a handle or an executing operation is held; one with nothing left,
or powered down, is not), `UacPlanRemoveHold`'s cases with streams, executing
operations, the quiet time and the bound, and the teardown as the
markers read it, modelled at `RemoveHold`'s 10 ms polls: the hold ends
250 ms after the last destroy; a close executing for 400 ms keeps it
through and 250 ms after; a client starting after the quiet time is not
waited for (the window above); a stream or a close left ends at the
bound. `streamtest.cpp`: the stream count stays untouched by every
injected creation failure and is balanced over a stream's life (30.6).
What the host checks cannot reach: the driver's calls into PortCls (the
request for D0, the hold's place in PortCls's remove), the list's locking
and lifetime, and the timing of real clients; those rest on the guest
legs: the record above.

### 30.6 Two devices removed together: a device PortCls had powered down

With the hold as first made, a `uac2` device (`x4full`) and a `uac1`
device (`hs`) removed together under NUSB's EHCI stalled every time, and
with `main`'s images less often (the record, "Two devices removed
together"). Read again with the lifecycle markers in trace builds of both
drivers (the `uac1` lines told apart from the `uac2` ones), the earlier
hold against `main`'s code, each device in turn the one recording:

**Observed:**

- 98 SE removed the two devices one after the other on the same thread:
  the `uac2` device's remove first, the `uac1` device's 630 to 930 ms after
  the first had returned.
- During the `uac2` device's remove, the `uac1` driver logged
  `PowerChangeState` with D3 for its device, and no D0 came before that
  device's own remove. When the `uac1` device was the one recording, its
  pin was stopped on the remove's thread during the `uac2` device's remove
  (the transport drained for about 2 s).
- At the `uac1` device's remove, a client thread's cleanup and close of one
  of its handles came 6 to 10 ms after the remove's entry; the close
  returned `STATUS_PENDING`, and the remove did not return in the window
  watched: the stall. With the earlier hold this happened at all 6 such
  removes read (the close came inside the hold); with `main`'s code at 1 of
  3 (after PortCls's releases); at the other 2 no client request reached
  the driver during the remove (28 and 52 ms), and it returned.
- A `uac1` device alone: no power change before its remove, the client's
  closes inside the hold returned success (3 of 3). Two `uac2` devices
  (`x4full`, `hs24`): during the first device's remove the `uac2` driver
  logged a D3 and, about 750 ms later and before the second device's
  remove, a D0 (that build's line does not name the device); the second
  device's closes inside its hold returned success (3 of 3).

**Explanation proposed, not established:** a close that reaches PortCls
during the remove of a device it has set to D3 is not completed, and the
remove does not finish while it is outstanding; the hold, by keeping the
remove going for 250 ms or more, made such a close certain to arrive
inside it, where `main`'s 28 to 52 ms remove met it only sometimes. The
traces show only the calls into the driver, not what PortCls or KS do with
them; why the second device stayed in D3 is not known.

**The change:** PortCls's last device power state for each device is
recorded with its entry (`PowerChangeState`, pair 0's, the one PortCls
calls). At a remove with no surprise removal before it, a device whose
last state was not D0 is asked back to D0 before PortCls is given the
remove (`UacPlanRemoveWake`; `PcRequestNewPowerState`, which 98 SE's and
ME's PortCls export, `docs/testing/exports/w98port.txt` and
`wmeport.txt`). The hold is then taken as for any device in D0; it is not
taken when the device is still not in D0, nor when nothing of the device
is left to tear down (`UacPlanRemoveHoldTake`). With the markers, at 6 of 6
such removes the request returned success (after 1 to 37 ms), the driver
logged D0 before the forward, the client's closes after the forward came
inside the hold and returned success (their cleanups returned
`STATUS_INVALID_DEVICE_REQUEST`, as cleanups did in every trace, passing
or not; in one of the 6 the client had begun closing during the request,
before the forward), and the remove returned 262 to 320 ms after its
entry.

The stream count runs from `UacStream::Create`'s publication of a stream
(`UAC_STREAM_BORN`) to the end of a published stream's destroy
(`UAC_STREAM_GONE`); a creation that fails after taking its device
reference is destroyed without touching the count (`streamtest.cpp`
checks the balance on each injected failure and over a stream's life).

What it leaves open:

- the request waits on the PnP thread for PortCls's power request to
  complete; it returned in 1 to 37 ms in the 6 traced, and nothing bounds
  it (the 5 s cap of the hold comes after it);
- the request's return status is not used: whether the hold may be taken
  follows the last `PowerChangeState` call alone (a D0 call taken as D0
  whatever the request returns; none, and the device stays not in D0);
- a device left in D3 (the request refused) has no hold and `main`'s risk:
  a client close inside its remove can still stall it (with the first step
  of this change, which had no request, 2 of 3 such removes in a trace run
  stalled that way);
- creates and closes count as executing from their entry into the
  driver's dispatch to its return, not to the request's completion; a
  pending create or close is not counted while it pends, and the open
  count is approximate;
- a device left in D3 (the request refused) has no hold and `main`'s risk:
  a client close inside its remove can still stall it (with the first step
  of this change, which had no request, 2 of 3 such removes in a trace run
  stalled that way);
- a device whose power changed without a call to `PowerChangeState` is
  not seen as powered down;
- the windows of 30.3 (a late client, a pause over 250 ms, the 5 s cap)
  for a device in D0.

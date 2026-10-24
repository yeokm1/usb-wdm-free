# USB MIDI class driver (usbmidi)

Status: design draft, 2026-10-20. Implementation of M1 (class-compliant
MIDI, one x86 binary for 98 SE to 8 and an x64 build) under way since
2026-10-21; the run records are in `docs/testing/2026-10-21-usbmidi-*.md`
and `docs/testing/2026-10-22-usbmidi-m1.md`. Section 18 lists where the
code departs from or settles this draft.

A WDM driver for USB MIDI 1.0 devices (keyboards, controllers, sound
modules, multi-port DIN interfaces) on Windows 98 SE, ME, 2000, XP, Vista,
7 and 8, x86 everywhere and x64 from XP. It is much smaller than the UAC 2
driver (no isochronous streaming, no clocks, no feedback, no mixer), it
needs only control, bulk and interrupt transfers, which every host
controller driver offers, and on two of the target systems (98 SE and 2000)
there is no in-box USB MIDI at all. It is proposed as the first audio
deliverable of this repository, ahead of UAC 2.

## 1. Decisions in brief

1. **OS-facing model: a PortCls adapter with a MIDI miniport
   (`IMiniportMidi`) on every system.** Owner, 2026-10-20: no
   DirectMusic-native ports (`IMiniportDMus`).
   PortCls supplies the KS filter, pin and property plumbing that WDMAUD and
   SysAudio expect on every target; a raw KS or AVStream filter would
   rebuild that by hand (section 5).
2. **One KS filter (PortCls subdevice) per MIDI port**, each with its own
   interface friendly name, so every port reaches WinMM under its own name
   (section 5.4). A port is one cable number on one endpoint.
3. **Binding: generic on the MIDIStreaming subclass on 98 SE and 2000
   only, by hardware id for everything else.** Owner, 2026-10-20: a lone
   MIDIStreaming function (`USB\Class_01&SubClass_03&Prot_00`) is matched
   generically on 98 SE and 2000, which have no USB MIDI in the box; on ME
   and XP to 8 the INF lists only specific devices Windows gets wrong, by
   VID/PID. An AudioControl-plus-MIDIStreaming function, which Windows'
   composite parents group under the AudioControl interface's ids, is
   matched by hardware id from a device list, or installed by hand ("Have
   Disk", documented; no helper) (section 4). No generic match on
   `USB\Class_01&SubClass_01`, which would take USB speakers away from
   `usbaudio.sys`.
4. **Quirks: carry the families that cover most hardware in use** (the
   standard protocol with the M-Audio Uno and CH345 input fixes, Roland's
   and Yamaha's vendor-class interfaces, the fixed-endpoint table and the
   4-byte-packet list) in the first quirk milestone; the vintage vendor
   protocols (Emagic, CME, Novation, Akai, FTDI, raw bytes) later and only
   against hardware someone holds (section 7). Owner, 2026-10-20: M-Audio
   MidiSport devices are not served; M-Audio's own driver covers them on
   98 to XP, and a free driver would need the same restricted firmware.
5. **Output: no host pacing by default**; a USB MIDI device flow-controls
   its bulk OUT endpoint by NAK. A per-device pacing quirk, 3125 bytes a
   second per DIN port, for devices known to drop data (section 6.6).
6. **MIDI 2.0 (UMP) is out of scope**; the driver always selects the
   USB MIDI 1.0 alternate setting (section 6.2).
7. **One client per output port**, as WinMM's own model (Owner, 2026-10-20:
   no multi-client output port sharing).

## 2. Purpose and devices served

USB-MIDI functions as the USB Device Class Definition for MIDI Devices,
Release 1.0 (Nov 1, 1999, "the MIDI spec" below) defines them: a
MIDIStreaming interface (audio class 01, subclass 03) beside an
AudioControl interface (MIDI spec section 3; Appendix B.3 makes the AC
interface mandatory even when the function has no audio), with bulk MIDI
endpoints carrying 32-bit USB-MIDI Event Packets (section 4). In practice:

- Class-compliant keyboards and pad/knob controllers (one or two ports,
  usually embedded jacks only).
- Class-compliant DIN interfaces, from one-port cables to 8x8 rack units
  (one port per DIN jack, each with an External jack behind it).
- Sound modules and keyboards with an internal synth plus DIN through
  ports (several cables per endpoint).
- Vendor-class (bInterfaceClass FF) MIDI interfaces from Roland/Edirol/BOSS,
  Yamaha, Korg and others that speak the standard packet format without the
  class descriptors, and a smaller set with their own protocols. Linux's
  `sound/usb/quirks-table.h` and `sound/usb/midi.c` are the record of these
  (section 7).
- The MIDIStreaming interface of an audio interface or keyboard that also
  has audio, where the audio half is someone else's (section 9).

Not served: USB MIDI 2.0 / UMP endpoints (alternate setting 1 of a
MIDI 2.0 function), the MIDI spec's Element model and Transfer bulk
endpoints (DLS download; section 3.2.2 and 3.4), which Microsoft's own
driver does not support either ("USB MIDI Elements" are excluded in the
Usbaudio.sys page cited in 3.1), and serial/game-port MIDI.

## 3. Target systems and what each has in the box

### 3.1 In-box USB MIDI per system

| System | `usbaudio.sys` and USB MIDI | Evidence |
|---|---|---|
| Windows 98 SE | `usbaudio.sys` present (audio only). **No USB MIDI.** The INF binds every audio-class interface (`USB\CLASS_01`) to `USBAudio` and registers only the wave interface (`KSCATEGORY_AUDIO`, "Curve"); no MIDI interface or MIDI model exists | Microsoft, "USB Audio Class System Driver (Usbaudio.sys)" (learn.microsoft.com, windows-hardware/drivers/audio): "In Windows 98 Usbaudio.sys introduced support for USB devices such as speakers and microphones. Support for MIDI devices was added in Windows Me", and the 98 feature list "does not support ... USB MIDI", which the page carries to 98 SE ("all the same features as Windows 98, with one exception": WAVEFORMATEXTENSIBLE). 98 SE `wdma_usb.inf` lines 25 and 75 (read on the xHCI98 host, `tools/win98se-extracted/inf/`) |
| Windows ME | **USB MIDI supported** by `usbaudio.sys` | Same Microsoft page: "Windows Me and Windows XP support USB MIDI but do not support USB MIDI Elements". ME's own `wdma_usb.inf` was not read (the xHCI98 host's `tools/winme-extracted/` holds only `usb.inf`); **unverified** from the INF, settled by reading ME's `wdma_usb.inf` from the ME CAB set |
| Windows 2000 (SP4) | `usbaudio.sys` present (audio only). **No USB MIDI.** INF as 98 SE's: `USB\CLASS_01` to `USBAudio`, one wave interface | Same Microsoft page (MIDI named for Me and XP and later only; 2000 grouped with 98 SE). 2000 SP4 `wdma_usb.inf` (`DriverVer` 5.00.2184.1) lines 27, 89, 173 |
| Windows XP | **USB MIDI supported.** `USB\CLASS_01` to `USBAudio`; a `USBMIDI` model with `KSCATEGORY_AUDIO/RENDER/CAPTURE` interfaces, "USB MIDI Device", bound by hardware id to Yamaha `0499:1FFF` (Roland `0582:0002` commented out) | Microsoft page as above. XP SP3 `wdma_usb.inf` (`DriverVer` 5.1.2535.0) lines 36, 98, 109, 137, 915-927, 1224-1227 |
| Windows Vista, 7 | **USB MIDI supported**, same INF shape (`USB\CLASS_01` generic, Yamaha `0499:1FFF` by id) | Vista `wdma_usb.inf` 6.0.6002.18005 lines 52, 122; 7 `wdma_usb.inf` 6.1.7601.17514 lines 49, 122 (UTF-16 files, read on the xHCI98 host) |
| Windows 8 | Presumed as 7 | **Unverified**: no Windows 8 media is extracted on the host; settled by reading 8's `wdma_usb.inf` |

So the driver is **required** on 98 SE and 2000, and **optional** on ME
and XP to 8, where it adds the vendor quirks, per-port names and the
devices Microsoft's driver does not handle. Owner, 2026-10-20: the generic
match ships on 98 SE and 2000 only; on ME and XP to 8 the INF lists, by
VID/PID, only the specific devices Windows gets wrong, so every other
device keeps the in-box driver there.

### 3.2 The audio stack the driver plugs into

| System | KS | PortCls | WDMAUD (`midiIn`/`midiOut`) | DMus port | Evidence |
|---|---|---|---|---|---|
| 98 SE | `ks.sys` in box | `portcls.sys` in box | `wdmaud.drv` + `wdmaud.sys`; "Beginning with Windows 98, the WDMAud system driver translates calls to [midiOut/midiIn] into commands to WDM audio drivers" | yes: "DMusUART (in Windows 98 SE and later, and in Windows 2000 and later)" | 98 SE files `wdm/ks.sys`, `wdm/portcls.sys`, `wdm/wdmaud.sys` on the host's extraction; `wdmaudio.inf` line 146 (`midi*=wdmaud.drv`); Microsoft, "MIDI and DirectMusic Components" |
| ME | as 98 SE | as 98 SE | as 98 SE | as 98 SE | same page; ME files not extracted (**unverified** beyond the page) |
| 2000 | in box | in box | in box | yes (same quotation) | same page; 2000 DDK `inc/portcls.h`, `inc/dmusicks.h` |
| XP to 8 | in box | in box; MIDI and DMus ports share one implementation from XP | in box | yes | "MIDI Port Driver" and "DMus Port Driver" pages ("In Windows XP and later, the IPortMidi and IPortDMus interfaces are both implemented in a single internal driver module") |

The PortCls exports the design needs (`PcInitializeAdapterDriver`,
`PcDispatchIrp`, `PcAddAdapterDevice`, `PcNewPort`, `PcRegisterSubdevice`,
`PcNewServiceGroup`, `PcGetTimeInterval`, `PcForwardIrpSynchronous`,
`PcRegisterAdapterPowerManagement`) are all in the Windows 98 SE DDK's
import library (`LIBS_DDK/LIB_I386_FREE_PORTCLS.LIB` in the host's
`tools/win98ddk-extracted/`, the 98 SE DDK per xHCI98's
`docs/references/README.md`, "Windows 98 DDK"), and in the 2000 DDK's
`libfre/i386/portcls.lib`. `PcRegisterIoTimeout` is in 2000's and not in
98 SE's, so the driver keeps its own timer instead. The 98 SE DDK carries
no `portcls.h`; the build uses the 2000 DDK's headers, and the gate is the
98 SE import library together with the export tables of 98 SE's and ME's
in-box `portcls.sys`, read for names and ordinals only (Owner, 2026-10-20;
`AGENTS.md`, "Sources") (section 11).

## 4. How a MIDI function reaches the driver (binding)

### 4.1 What the composite parents present

Microsoft's composite parent groups audio interfaces "even when the device
does not have an interface association descriptor": consecutive class-01
interfaces, each with a subclass different from the first, become one
function, whose hardware ids are `USB\VID_v&PID_p[&REV_r]&MI_z` and whose
compatible ids are **the first interface's** class triple; with any IAD in
the configuration that rule is off and IADs group instead (Microsoft,
"Overview of Enumeration of Interface Collections on USB Composite
Devices", sections "Enumeration of interface collections on audio devices
without IADs" and "... with IADs"). xHCI98's bus applies the same rule
(`src/xhci_func.c`, the `cls == 0x01UL && iads == 0` branch; design record
13 section 10.8).

So a class-compliant MIDI device (AC interface 0, MS interface 1, no IAD)
appears as **one function with compatible id
`USB\Class_01&SubClass_01&Prot_00`**, the same id a USB speaker's audio
function carries. Its INF match cannot tell the two apart. Where the MS
interface stands alone - after an IAD-grouped UAC 2 function, in an IAD of
its own, or a device whose MS interface is not preceded by an AC interface
- it carries `USB\Class_01&SubClass_03&Prot_00`.

Whether 98 SE's and 2000's own `usbhub.sys` group audio interfaces the same
way is **unverified**; the `&MI_00` hardware ids in their `wdma_usb.inf`
are consistent with it. Settled by plugging an AC+MS device into each
under the stock stack and reading its Enum ids. The design works either
way: a lone MS interface PDO takes the generic match.

### 4.2 The matching policy

| Function shape | INF match | Result |
|---|---|---|
| Lone MS interface | 98 SE and 2000: `USB\Class_01&SubClass_03&Prot_00` (generic; the only compatible id this package claims). ME and XP to 8: hardware ids of listed devices only | This driver. On 98 SE and 2000 it outranks the in-box `USB\CLASS_01` (a more specific compatible id). On Vista and later an unsigned driver ranks below a signed one; signing follows xHCI98's and Stor98's approach (Owner, 2026-10-20) |
| AC + MS (the usual MIDI-only device) | Hardware ids from the device list (`USB\VID_v&PID_p&MI_z`; the whole-device form only where added on request for a device a stack does not split) | This driver for listed devices; others by hand (4.3) |
| AC + AS + MS (audio and MIDI, no IAD) | Not matched by this package | The in-box `usbaudio.sys` (audio; MIDI too on ME and XP+), or the UAC driver if it carries the MIDI core (section 9). By hand, this driver gives MIDI only, the AS interfaces left at their zero-bandwidth alternate 0 |
| Vendor-class MIDI interface (Roland, Yamaha, Korg...) | Hardware ids from the quirk table (section 7) | This driver |

The device list is generated from the quirk table and a list of known
class-compliant MIDI-only products, by a build script, so the INF and the
driver's table cannot drift apart (a build gate compares them).

A generic match on `USB\Class_01&SubClass_01` with the driver refusing
functions that have AudioStreaming interfaces was rejected: a refused start
leaves the speaker at Code 10 with no fall-back to `usbaudio.sys`. A filter
under `usbaudio.sys` that creates a sibling device for MIDI was rejected as
fragile (a PnP device of its own is needed for PortCls). Owner,
2026-10-20: no installer helper for now; unlisted AC+MS devices are
installed by hand, and the release notes document how (4.3).

### 4.3 Installing by hand

The INF carries a model "USB MIDI device (class compliant)" with
`ExcludeFromSelect` off, so Update Driver / "Have Disk" with "show all
hardware" (98 SE, ME) or "Show compatible hardware" cleared (2000 to 8)
offers it for any audio function. This manual install is the documented
route for unlisted AC+MS devices (Owner, 2026-10-20). Whether 98 SE's
wizard installs a model whose ids do not match the device is
**unverified**; settled on the first 98 SE leg.

## 5. OS-facing side

### 5.1 Why PortCls with a MIDI miniport

| Option | 98 SE | 2000 | XP-8 | Cost |
|---|---|---|---|---|
| PortCls + `IMiniportMidi` | yes | yes | yes | Small: the miniport implements `Init`, `NewStream`, `Service`, and per stream `Read`, `Write`, `SetState`; PortCls owns the KS filter, pins, properties, state machine, the MIDI sequencer ("schedule the raw MIDI messages with a timer resolution of one millisecond") and WDMAUD's expectations |
| PortCls + `IMiniportDMus` | yes (DMusUART exists from 98 SE) | yes | yes | Larger: `IMXF` streams, `DMUS_KERNEL_EVENT` lists, `IAllocatorMXF`, 100 ns timestamps; gives DirectMusic native ports. Not planned (Owner, 2026-10-20) |
| Raw KS 1.0 filter (`ks.sys` helpers) | yes | yes | yes | Every property set, pin factory, data intersection, clock and state transition by hand; no gain for MIDI |
| AVStream minidriver (what XP's `usbaudio.sys` is) | **unverified** availability on 98 SE / 2000 (AVStream is documented from XP; earlier systems with a DirectX runtime are not confirmed) | as 98 SE | yes | Medium; not on all targets |

PortCls has no bus requirement: the adapter's resource list is simply
empty for a USB function (as for a root-enumerated virtual device). That
`portcls.sys` on 98 SE starts an adapter with an empty resource list is
**unverified**; the first milestone (M0) proves it before anything else is
built.

DirectMusic still reaches a MIDI-port filter through WinMM: DirectMusic
and WinMM are both SysAudio clients (Microsoft, "MIDI and DirectMusic
Components"), and an application can "run through a midiOutXxx device".
Owner, 2026-10-20: `IMiniportDMus` (DirectMusic-native ports with precise
output timing) is not planned; WinMM is enough.

### 5.2 Adapter structure

- `DriverEntry` calls `PcInitializeAdapterDriver` with the driver's own
  `AddDevice`.
- `AddDevice`: `IoGetAttachedDeviceReference(PDO)` first (the device the
  new FDO will sit on, which PortCls does not hand to the adapter; stock
  98 SE drivers import it, xHCI98 design record 13 appendix A row 33), then
  `PcAddAdapterDevice` with a device extension of
  `PORT_CLASS_DEVICE_EXTENSION_SIZE` (256, 2000 DDK `portcls.h` line 35)
  plus the driver's own state. URBs go to that lower device in IRPs the
  driver allocates itself, so they never pass PortCls' dispatch.
- `StartDevice` callback (PASSIVE_LEVEL): read the device and configuration
  descriptors, parse (section 6), select the configuration with the
  function's interfaces, read strings, create one MIDI port
  (`PcNewPort(CLSID_PortMidi)`) and one miniport object per MIDI port,
  `PcRegisterSubdevice` each, start the IN pipes.
- Power: `PcRegisterAdapterPowerManagement`; on D3 cancel the URBs, on D0
  resubmit (no state to restore on the device; MIDI has no class
  controls).
- Stop and remove: PortCls tears the subdevices down; the driver cancels
  its URBs, waits for them, releases the lower-device reference.

Written in C with explicit vtables laid out as `punknown.h`'s
`DECLARE_INTERFACE` gives them, or in a C++ subset (no exceptions, no RTTI,
the driver's own `operator new` on non-paged pool, its own `IUnknown`
rather than the DDK's `stdunk` library, which is Microsoft code linked into
the binary). The choice is made in M0 by what builds cleanly with the 2000
DDK compiler and MSVC 6.0.

### 5.3 The MIDI filter of one port

Each port's filter has, as the DDK's "MIDI and DirectMusic Filters" page
lays a MIDI filter out: for an output port, a render streaming pin (sink,
`KSDATAFORMAT_TYPE_MUSIC` / `KSDATAFORMAT_SUBTYPE_MIDI`) and a bridge pin to
the physical output; for an input port, a bridge pin from the physical
input and a capture streaming pin. A port with both directions on the same
cable number and the same jack chain (a DIN pair) is one filter with both
pairs; otherwise one filter per direction. No topology filter: that
WDMAUD on 98 SE enumerates a MIDI filter without a topology subdevice is
**unverified**, and the DDK sample adapters that might show it are not read
(Owner, 2026-10-20)
(M1 test; the fall-back is a trivial topology miniport and
`PcRegisterPhysicalConnection`).

Stream behaviour, all at DISPATCH_LEVEL as the DDK requires:

- `Write` (render): the port driver hands raw MIDI bytes. The stream
  appends them to its port's output byte ring (4 KiB) and kicks the
  endpoint's packetizer (6.5). If the ring cannot take everything it
  accepts a multiple of four bytes, or zero ("the device is too busy ...
  try again later"; `BytesWritten` rules from
  `IMiniportMidiStream::Write`'s remarks), and calls the port's `Notify`
  when an OUT URB completes and space frees, so the port retries. Whether
  the port retries on `Notify` or on its own timer is **unverified** and
  is read in M1.
- `Read` (capture): copy bytes from the port's input ring; zero bytes means
  none ("returns STATUS_SUCCESS and a BytesRead count of zero"). The IN
  completion routine fills the ring and calls `IPortMidi::Notify`
  (`IMiniportMidiStream::Read` remarks). The port driver timestamps the
  data.
- `SetState`: RUN enables the stream; STOP on an output stream flushes its
  ring and, if a SysEx was cut, sends nothing further for it (the parser
  resets); STOP on input discards buffered bytes.
- Instances: one render and one capture stream per port, which is what
  WinMM's exclusive `midiOutOpen` needs. Owner, 2026-10-20: no sharing of
  one output port by several clients.

### 5.4 Port names and how WDMAUD sees them

`MIDIOUTCAPS.szPname` holds 31 characters (`MAXPNAMELEN` 32 with the NUL,
2000 DDK `mmsystem.h` line 70). How WDMAUD forms the name differs by
system and is **unverified** in detail: XP's INF sets a `FriendlyName` on
each MIDI interface key (`USBAudio.Interface.AddReg.MIDI`, XP
`wdma_usb.inf` near line 591), which shows the interface friendly name is
read; whether a per-pin name (`KSPROPERTY_PIN_NAME`, the pin descriptor's
`Name` GUID under `MediaCategories`) is used for MIDI on any target is not
established. Hence one filter per port: each port's KS interfaces carry
their own `FriendlyName`, which works whatever per-pin naming does.

Mechanics: before `PcRegisterSubdevice` the driver calls
`IoRegisterDeviceInterface` for the subdevice's reference string under the
categories PortCls will register (the call is idempotent and returns the
same symbolic link), opens the interface key
(`IoOpenDeviceInterfaceRegistryKey`) and writes `FriendlyName` and the KS
proxy `CLSID`; PortCls then enables the interfaces. `IoRegisterDeviceInterface`
is evidenced on 98 SE (xHCI98 design record 13 appendix A row 17);
`IoOpenDeviceInterfaceRegistryKey` on 98 SE is **unverified** and goes
through the same import gate (section 11). If it fails on 98 SE, the 98 SE
INF pre-declares sixteen interfaces per direction with generic names
(`AddInterface` with reference strings `MIDIO0`..`MIDIOF`, `MIDII0`..`MIDIIF`)
and the runtime name is lost there.

Name source, first that exists (truncated to 31 characters, converted to
ANSI on 98 SE / ME):

1. A per-device port-name entry in the quirk table (from Linux's
   `snd_usbmidi_port_info`, 91 entries in `sound/usb/midi.c`, e.g. Roland
   SC-8850 "Part A".."Part D", "MIDI 1", "MIDI 2").
2. The embedded jack's `iJack` string (what Linux uses,
   `find_usb_ijack`).
3. The `iJack` string of the External jack wired to that embedded jack
   (an addition of this design: many interfaces name only the DIN jacks).
4. The product string, plus " MIDI n" when the device has more than one
   port per direction (Linux's `"%s %s %d"` form), the product string
   alone when it has one.

If a jack name already begins with the product name, it is used alone
(Linux's `strncmp(card->shortname, jack_name, ...)` rule).

## 6. USB side

### 6.1 Descriptors parsed

From the configuration descriptor, restricted to the function's interfaces
(the PDO's configuration descriptor request returns only those under a
composite parent):

- Standard MS interface (MIDI spec Table 6-1): class 01, subclass 03.
- Class-specific MS interface header (Table 6-2: `bcdMSC`, `wTotalLength`),
  MIDI IN Jack (Table 6-3: `bJackType` EMBEDDED 01 / EXTERNAL 02,
  `bJackID`, `iJack`), MIDI OUT Jack (Table 6-4: `bNrInputPins`,
  `baSourceID`/`baSourcePin` pairs, `iJack` at offset `5+2p`). Element
  descriptors (6.1.2.4) are skipped.
- Standard MS endpoint (Table 6-6; nine bytes in practice, audio-style)
  and the class-specific MS endpoint descriptor (Table 6-7: `bNumEmbMIDIJack`,
  `baAssocJackID(n)`).

Validation follows Linux's `snd_usbmidi_get_ms_info`: endpoints that are
neither bulk nor interrupt, or lack a CS endpoint descriptor, or claim more
than 16 jacks, or whose descriptor is shorter than `4 + n`, are skipped,
not fatal. Every length is checked against `wTotalLength` and the
descriptor walk stops on a zero `bLength`.

### 6.2 Configuration and alternate settings

- Select the device's configuration through the function PDO with every
  interface of the function, AC and any AS interfaces at alternate 0.
- The MS interface: alternate 0 unless a quirk says otherwise. A USB MIDI
  2.0 function puts USB MIDI 1.0 on alternate 0 and UMP on alternate 1
  (USB-IF, USB Device Class Definition for MIDI Devices 2.0; section
  number not checked); the driver additionally requires `bcdMSC` 0x0100
  on the alternate it uses and ignores any alternate with 0x0200.
- Roland quirk (Linux `snd_usbmidi_switch_roland_altsetting`): on vendor
  0582 with exactly two alternates, where alternate 1 has two endpoints of
  which one is interrupt, select alternate 1.

### 6.3 Ports from cables and jacks

A port is (endpoint, cable number). For each MS data endpoint, cable `n`
(0..`bNumEmbMIDIJack`-1) is associated with embedded jack
`baAssocJackID(n)` (MIDI spec 3.2.1: "Each Embedded MIDI Jack connected to
one MIDI Endpoint is assigned a number from 0 to 15"; 5.1: "There is a one
to one relationship between cable numbers and JackIDs").

- OUT endpoint, cable `n` -> embedded MIDI IN jack -> (optionally) an
  External MIDI OUT jack whose `baSourceID` is that jack: a DIN output.
- IN endpoint, cable `n` -> embedded MIDI OUT jack, whose `baSourceID`
  names (optionally) an External MIDI IN jack: a DIN input.
- An embedded jack with no external jack wired to it is internal (a synth,
  a controller's own surface): no pacing (6.6), named from its own string.

A device may have several MS data endpoints per direction (one port each
is common on vendor interfaces); Linux allows two pairs
(`MIDI_MAX_ENDPOINTS`); this driver allows four pairs, 16 cables each, and
caps ports at 32 per direction per device.

### 6.4 Transfer types and the IN side

The MIDI spec says bulk (3.2.1, 6.2.1); real devices also use interrupt
endpoints, and Linux handles both. On Windows both are
`URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER` on the pipe handle the
configuration returns, so the driver has one path. Low-Speed devices that
declare bulk endpoints (Linux forces interrupt for them, "ESI MIDI Mate")
are at the mercy of the host controller driver's handling of an illegal
descriptor; not worked around.

Per IN endpoint, three URBs outstanding (Linux keeps seven), each of
`wMaxPacketSize` (64 at Full Speed, 512 at High Speed), with
`USBD_SHORT_TRANSFER_OK`. The completion routine (DISPATCH_LEVEL) decodes
the packets (6.5) into the per-port input rings, notifies the port drivers
and resubmits. Overflow of a ring drops whole messages and counts them.

### 6.5 USB-MIDI Event Packets

Encoding and decoding follow MIDI spec section 4, Table 4-1 (CIN) and
Table 4-2 (examples); the byte state machine follows Linux's
`snd_usbmidi_transmit_byte` and `snd_usbmidi_standard_input`
(`sound/usb/midi.c`):

| CIN | Bytes | Meaning |
|---|---|---|
| 0x0, 0x1 | - | Reserved; ignored on input |
| 0x2 / 0x3 | 2 / 3 | System common (F1, F3 / F2) |
| 0x4 | 3 | SysEx start or continue |
| 0x5 | 1 | Single-byte system common (F6), or SysEx end with one byte |
| 0x6 / 0x7 | 2 / 3 | SysEx end with two / three bytes |
| 0x8-0xE | 3 or 2 | Channel voice (C and D two bytes) |
| 0xF | 1 | Single byte (real-time F8-FF on output) |

Output (bytes from the port driver to packets), per port:

- **Running status**: the byte stream from WDMAUD may omit repeated status
  bytes. The parser keeps the last channel status and, as Linux's
  `STATE_2PARAM_1` loop does, emits a full packet with the status byte
  re-inserted for every complete data pair (or single for C/D). The device
  never sees running status.
- **Real-time** bytes (F8-FF) are sent at once as CIN F, without
  disturbing a message or SysEx in progress.
- **SysEx**: three data bytes per CIN 4 packet; the end packet is CIN 5, 6
  or 7 by remainder (Table 4-2, including the `F0 F7` and `F0 mm F7`
  special cases). A status byte inside a SysEx ends it implicitly; the
  parser emits no end packet for an unterminated SysEx (the receiver sees
  the new status), as Linux does.
- F4, F5 (undefined) are dropped; F7 outside SysEx is dropped.
- **Fairness**: each OUT URB is filled round-robin across the endpoint's
  ports, a packet at a time (Linux rotates its start port per URB), so one
  port's long SysEx does not starve the others.
- URB size: `wMaxPacketSize`, or 4 bytes for chips listed as failing with
  more (Linux's `max_transfer = 4` list: Medeli DD305, ESI M4U, RedOctane GH
  MIDI, Textech cables, QinHeng CH345, `fc08:0101`). Two OUT URBs in flight
  per endpoint.

Input (packets to bytes): cable selects the port; CIN gives the length;
CIN 0/1 packets and packets for cables with no port are dropped; a packet
of all zeros ends the useful part of a buffer (some devices pad).
Real-time bytes are delivered in place. Bytes go to the port's ring; the
port driver's `Read` returns them, and WDMAUD assembles SysEx into
`midiInAddBuffer` buffers.

### 6.6 Output pacing and DIN ports

A DIN MIDI port runs at 31250 bit/s, 10 bits per byte: 3125 bytes a
second, 320 microseconds a byte. A Full-Speed bulk packet of 64 bytes holds
16 events, up to 48 MIDI bytes, about 15 ms of DIN time. The MIDI spec's
model is bulk endpoints, so a compliant device that cannot accept more
NAKs the OUT packet and the host retries; that is the default and the
driver adds no pacing: an OUT URB pending is the back-pressure, the rings
fill, `Write` takes fewer bytes, and the port driver holds the rest.

For devices known to accept and then drop (cheap single-cable adapters,
long SysEx dumps), a quirk flag (and a per-device registry value to turn
it on for a device not in the list) paces each DIN-backed port (6.3) with
a token bucket at 3125 bytes a second, a 64-byte burst, refilled from
`KeQueryPerformanceCounter` (evidenced on 98 SE, record 13 appendix A row
102) at each URB completion and from a 1 ms kernel timer while data waits.
The real resolution of a kernel timer on 98 SE and ME is **unverified**; if
it is coarser, the bucket still holds the average rate, with bursts up to
the timer period. Internal (embedded-only) ports are never paced.

### 6.7 Errors, stalls, removal

- STALL on a data endpoint: a work item at PASSIVE_LEVEL sends
  `URB_FUNCTION_RESET_PIPE` (clear halt), then the pipe restarts. After
  three stalls within a second the port is reported broken (logged) and
  left idle.
- Other errors: 100 ms back-off before resubmitting (Linux's
  `ERROR_DELAY_JIFFIES`), which also lets the hub report a departure first.
- Device removal: `STATUS_DEVICE_NOT_CONNECTED`, `STATUS_CANCELLED` and
  surprise removal stop resubmission; PortCls fails open streams. How
  98 SE's KS/PortCls cope with a removed device whose pins are open is a
  test item (risk R4).
- Under xHCI98 specifically: a bulk or interrupt transaction error is
  retried by the controller driver up to three times, and an endpoint the
  controller halts with no transfer to blame re-enumerates the device
  (xHCI98 `docs/using/release-notes.md`, the controller-tolerance list,
  "A transfer error on a bulk or interrupt endpoint" and "A report the
  driver cannot pin on a transfer"); an interrupt endpoint asking for more
  than 32 ms is polled every 32 ms on AMD controllers (`XhciIntervalCap`,
  `docs/using/tuning.md`), harmless for MIDI. The driver needs nothing
  xHCI98-specific: control, bulk and interrupt are what every USBD stack
  (Microsoft's UHCI/OHCI on 98 SE, UHCI/OHCI/EHCI from 2000 on) offers.
  On 98 SE without a USB 2.0 stack, High-Speed MIDI devices run at Full
  Speed.

## 7. Vendor-specific devices: which quirk families to carry

From Linux `sound/usb/midi.c` (the `QUIRK_MIDI_*` switch in
`__snd_usbmidi_create`), `sound/usb/quirks.c` (the Yamaha and Roland
autodetection) and `sound/usb/quirks-table.h` (counts are entries in the
current upstream file):

| Family (Linux name) | What it is | Devices | Carry |
|---|---|---|---|
| Standard class (`QUIRK_MIDI_STANDARD_INTERFACE`) | The class protocol; table entries for devices whose descriptors are otherwise wrong | 15 table entries plus every class-compliant device | **M1** |
| M-Audio Uno broken running status (`snd_usbmidi_maudio_broken_running_status_input`) | Input packets with data bytes and no status, marked CIN 4 | `0763:0150` | **M2** |
| CH345 (`QUIRK_MIDI_CH345`) | The ubiquitous cheap "USB2.0-MIDI" cable: every second SysEx packet repeats the previous CIN | `1a86:752d` | **M2** |
| 4-byte OUT packets (`max_transfer = 4`) | Chips that declare a larger packet and fail with it | 7 ids | **M2** |
| Fixed endpoint (`QUIRK_MIDI_FIXED_ENDPOINT`) | Vendor-class interface, standard packets, cable masks from the table | 46 entries (Roland/Edirol/BOSS, Korg, others) | **M2** |
| Yamaha (`QUIRK_MIDI_YAMAHA`) | Vendor-class interface with jack descriptors to be counted; plus vendor 0499 autodetection | 108 entries (101 `YAMAHA_DEVICE`, 7 `YAMAHA_INTERFACE`) | **M2** |
| Roland (`QUIRK_MIDI_ROLAND`) | Vendor-class interface with a `06 24 F1 02 <in> <out>` descriptor or class descriptors; vendor 0582 autodetection; interrupt alternate switch | Roland's vendor-class range (58 table lines name 0582) | **M2** |
| MIDIMAN (`QUIRK_MIDI_MIDIMAN`) | M-Audio MidiSport 1x1/2x2/4x4/8x8: own 4-byte packet layout (`snd_usbmidi_midiman_input`) | 14 entries. **Needs firmware loaded first** (the pre-firmware ids are different; the firmware is not free) | **Not carried**. Owner, 2026-10-20: M-Audio's own driver serves these devices on 98, ME, 2000 and XP (`existing.md` section 3.3), and a free driver would need the same restricted firmware |
| Emagic (`QUIRK_MIDI_EMAGIC`) | Unitor8, AMT8, MT4: F5 port-select, init/finish SysEx | 4 entries | M4, on demand |
| Novation (`QUIRK_MIDI_NOVATION`) | ReMOTE/XStation, Speedio, Nocturn: length-prefixed packets | 3 | M4, on demand |
| CME (`QUIRK_MIDI_CME`) | CME keyboards: standard plus SysEx in one CIN F packet | 3 | M4, on demand |
| Akai (`QUIRK_MIDI_AKAI`) | MPD16 | 1 | M4, on demand |
| FTDI (`QUIRK_MIDI_FTDI`) | Starr Labs: FTDI serial set to 31250 baud, raw bytes | 1 | M4, on demand |
| Raw bytes (`QUIRK_MIDI_RAW_BYTES`) | MOTU Fastlane and similar: one byte stream per endpoint | 1-2 | M4, on demand |
| US-122L (`QUIRK_MIDI_US122L`) | Tascam US-122L/US-144 MIDI inside their own audio protocol | 2 | **Not carried**: belongs with a driver for those audio devices |

The table is compiled into the driver and keyed by VID, PID (with an
optional bcdDevice range and interface number), with: family, cable masks,
fixed endpoints, flags (`MAXPKT4`, `PACE_DIN`, `ALT_INT`, `RS_FIX`,
`SYSEX_FIX`), and optional port names. A registry value under the device's
driver key can add flags for a device not in the table (`UmFlags`, DWORD),
read at start.

## 8. INF and install per system

One INF per release folder, `usbmidi.inf`, `Class=MEDIA`
(`{4d36e96c-e325-11ce-bfc1-08002be10318}`), modelled on the stock
`wdma_usb.inf` sections cited in 3.1:

- 98 SE / ME: `Include=ks.inf, wdmaudio.inf`, `Needs=KS.Registration,
  WDMAUDIO.Registration` (98 SE `wdmaudio.inf` line 24 defines it); `AddReg`
  with `Driver`/`NTMPDriver` = `usbmidi.sys`, `AssociatedFilters` =
  `"wdmaud"`, `Drivers\SubClasses` = `"midi"` and
  `Drivers\midi\wdmaud.drv` `Driver` and `Description` (as XP's
  `USBMIDI.AddReg`, lines 915-927, less the `mixer` subclass - whether
  WDMAUD needs a mixer entry for a MIDI-only device is an M1 test item);
  `CLSID` = the KS proxy `{17CCA71B-ECD7-11D0-B908-00A0C9223196}` (98 SE
  INF line 90). The 98 SE fall-back interface list of 5.4 if needed.
- 2000 to 8: the `.NT` / `.NTx86` / `.NTamd64` decorations of the same,
  `WDMAUDIO.Registration.NT` where the stock INF uses it, `Services` with
  `usbmidi` (demand start, `SERVICE_KERNEL_DRIVER`).
- Models: on 98 SE and 2000 only, generic
  `USB\Class_01&SubClass_03&Prot_00` (the one compatible id claimed; no
  `Prot_20` id and no `SubClass_00` / `SubClass_01` id, which belong to the
  UAC 2 package or stay with `usbaudio.sys`, section 9); on every system the
  generated hardware-id list (4.2), which on ME and XP to 8 holds only the
  devices Windows gets wrong; the "class compliant, by hand" model (4.3).
- No co-installer, no user-mode component.

Release folders (`free/<class>/<package>/<systems>/`, 8.3):

| Folder | Systems | Binary |
|---|---|---|
| `free/audio/usbmidi/w98se/` | 98 SE | x86; INF with the generic match |
| `free/audio/usbmidi/wme/` | ME | x86 (same binary); INF with the id list only |
| `free/audio/usbmidi/w2k/` | 2000 | x86 (same binary); INF with the generic match |
| `free/audio/usbmidi/wxp/` | XP | x86 (same binary); INF with the id list only |
| `free/audio/usbmidi/wv-8-x86/` | Vista, 7, 8 | x86 (same binary), id list only, catalog if signed |
| `free/audio/usbmidi/wxp-x64/` | XP x64 | x64, id list only |
| `free/audio/usbmidi/wv-8-x64/` | Vista, 7, 8 x64 | x64, id list only, must be signed to load |

Files in each: `usbmidi.inf`, `usbmidi.sys`, and `usbmidi.cat` where
signed. The folder names follow the one scheme of `README.md`, "Layout"
(Owner, 2026-10-20), shared with `uac2.md` section 6. 98 SE and ME (and
2000 and XP) do not share a folder because their INFs differ in the
generic match.

## 9. Sharing with the UAC 2 driver

This section agrees with `docs/design/uac2.md` section 11. Composite
audio+MIDI devices come in two shapes:

- **MS interface outside the audio function** (a UAC 2 function grouped by
  an IAD with the MS interface after it on its own, or in an IAD of its
  own): Windows gives the MS interface its own PDO with compatible id
  `USB\Class_01&SubClass_03&Prot_00` (4.1). This driver binds it; the UAC 2
  driver binds the audio function. No conflict.
- **MS interface inside the audio function** (an IAD holding AC, AS and MS,
  or a legacy-grouped AC+AS+MS function): one PDO, one function driver. A
  separate MIDI driver cannot reach that MS interface; the driver that
  binds the function must serve it. For a UAC 2 function that is
  `uac2wdm.sys`, which registers a PortCls MIDI subdevice (`IPortMidi`)
  beside its wave and topology subdevices, using the shared MIDI code
  below.

The split, as both documents state it:

| Function | Compatible id it carries | Claimed by |
|---|---|---|
| UAC 2 function under an IAD (MS inside or not) | `USB\Class_01&SubClass_00&Prot_20` | `uac2wdm.inf` |
| UAC 2 function grouped by the legacy rule | `USB\Class_01&SubClass_01&Prot_20` | `uac2wdm.inf` |
| Lone MIDIStreaming interface | `USB\Class_01&SubClass_03&Prot_00` | `usbmidi.inf`: generic on 98 SE and 2000; by hardware id only on ME and XP to 8 |
| MIDI-only AC+MS function (protocol 0) | `USB\Class_01&SubClass_01&Prot_00` | Neither generically (it is also every UAC 1 speaker's id); `usbmidi.inf` by hardware id only (4.2) |
| UAC 1 audio, with or without MS | `USB\Class_01&SubClass_01&Prot_00` | Neither generically; in-box `usbaudio.sys` (`USB\CLASS_01`) |

Neither INF claims a bare `USB\Class_01&SubClass_xx` two-part or
`USB\CLASS_01` id, so the two packages can never both match one function.
A device listed by hardware id in `usbmidi.inf` must not be a UAC 2 device
(the id-list gate of 4.2 checks the quirk table's entries are protocol-0
functions).

**Shared code** lives in `src/audio/common/`, the folder `uac2.md` names.
The MIDI parts are the descriptor parser (6.1-6.3), the packet codec and
parsers (6.5), the quirk table (7), port naming (5.4) and the PortCls MIDI
miniport object (5.3), with no dependency on either adapter. Both drivers
are PortCls adapters, so the miniport object is shared as is. AGENTS.md's
"each package has its own build script" is kept: each package's build
compiles the shared sources into its own binary. The MIDI files are
prefixed `um` / `ms` / `mp` so they do not collide with the UAC 2 files in
the same folder.

## 10. Package and source files

`src/audio/usbmidi/` (8.3 throughout), shared code in
`src/audio/common/`:

| File | What |
|---|---|
| `src/audio/usbmidi/adapter.c` | `DriverEntry`, `AddDevice`, `StartDevice`, power, remove |
| `src/audio/usbmidi/usbio.c` | URB building, configuration selection, IN/OUT pipes, stall recovery |
| `src/audio/usbmidi/build.cmd` | x86 build (2000 DDK + MSVC 6.0) and x64 build (WDK 7.1) |
| `src/audio/usbmidi/usbmidi.inf` | INF source; the device list is generated into it |
| `src/audio/usbmidi/mkids.ps1` | Generates the INF hardware ids from the quirk table, and the gate that compares them |
| `src/audio/common/msdesc.c` | MS descriptor parser, ports from jacks (6.1-6.3) |
| `src/audio/common/umpkt.c` | Event packet encoder and decoder, running status, SysEx (6.5); adapted from Linux |
| `src/audio/common/umquirk.c` | Quirk table and vendor protocols (7); adapted from Linux |
| `src/audio/common/mpmidi.c` | The `IMiniportMidi` / `IMiniportMidiStream` objects (5.3) |
| `src/audio/common/umname.c` | Port naming (5.4) |
| `src/audio/common/test/` | Host-side tests of the codec, parser and quirks (section 12) |

The binary is `usbmidi.sys`, service `usbmidi`. No Windows release ships a
file or service by that name (Windows 10's later MIDI driver is
`usbmidi2.sys`, outside the targets); a third-party driver of that name on
a user's system is possible. Owner, 2026-10-20: the names are accepted.

## 11. Build toolchain

As this repository's AGENTS.md "Standalone" requires, toolchains are
installed per host into the git-ignored `tools/`, and the build documents
what goes there:

- **x86 (98 SE to 8), one binary**: Windows 2000 DDK (`WIN2KDDK.EXE`) with
  Visual C++ 6.0, as xHCI98 builds its x86 drivers (xHCI98
  `docs/contributing/build-and-test.md`, "Visual C++ 6.0 unpacked from
  `tools/MSVC600.zip`", "Windows 2000 DDK unpacked from `tools/WIN2KDDK.EXE`
  into `tools/ntddk`"). Headers `portcls.h`, `dmusicks.h`, `ks.h`,
  `ksmedia.h`, `usbdi.h`; libraries `portcls.lib`, `ks.lib`, `usbd.lib`,
  `ntoskrnl.lib`.
- **Import gate for 98 SE / ME**: every import must be named by the 98 SE
  DDK's import libraries (`tools/win98ddk-extracted/all/LIBS_DDK/`,
  `LIB_I386_FREE_PORTCLS.LIB`, `..._KS.LIB`, `..._USBD.LIB`), or by the
  export tables of 98 SE's in-box binaries read for names and ordinals only,
  or evidenced as xHCI98's gate evidences kernel imports (xHCI98
  `scripts/import-gate/`, `win98-evidence.list`, design record 13 appendix
  A); the load on a 98 SE machine is the final word. The gate script is
  copied into this repository, not referenced (AGENTS.md "Standalone").
- **x64 (XP x64, Vista to 8 x64)**: WDK 7.1 (`WinDDK71`, `lib/wnet/amd64`
  for XP x64, `lib/wlh/amd64` and `lib/win7/amd64`), whose `portcls.lib`
  exists for each.
- Host tests: the codec, parser and quirk logic built as a console program
  with MSVC 6.0 (or any C compiler) from the same sources.

## 12. Adapted code and NOTICE.md rows

Linux `sound/usb/midi.c` carries "Copyright (c) 2002-2009 Clemens Ladisch"
under a dual licence (a BSD-style licence or "the GNU General Public
License ... version 2 of the License, or (at your option) any later
version"; `MODULE_LICENSE("Dual BSD/GPL")`). `sound/usb/quirks-table.h` and
`sound/usb/quirks.c` are `SPDX-License-Identifier: GPL-2.0-or-later`. All
three are GPL-2.0-compatible. The adapted files keep the original notices
verbatim, take the GPL option, and carry an SPDX tag:

| File | Adapted from | Licence |
|---|---|---|
| `src/audio/common/umpkt.c` | Linux `sound/usb/midi.c` (`snd_usbmidi_transmit_byte`, `snd_usbmidi_standard_input`, `snd_usbmidi_cin_length`, the CH345 and M-Audio Uno input fixes) | Dual BSD-style / GPL-2.0-or-later, used under GPL-2.0-or-later |
| `src/audio/common/umquirk.c` | Linux `sound/usb/quirks-table.h` (MIDI entries), `sound/usb/quirks.c` (Yamaha and Roland detection), `sound/usb/midi.c` (`snd_usbmidi_port_info`, the `max_transfer` list, vendor protocol handlers) | GPL-2.0-or-later; `midi.c` parts as above |
| `src/audio/common/msdesc.c` | Linux `sound/usb/midi.c` (`snd_usbmidi_get_ms_info`, `find_usb_ijack`, `snd_usbmidi_detect_*`), restructured | as `umpkt.c` |

ReactOS was checked and gives nothing to adapt: its `drivers/usb/usbaudio`
(GPL, Johannes Anderwald) is an AVStream audio driver that only recognises
the MIDIStreaming subclass (one match for "MIDI" in `usbaudio.c`); its
PortCls reimplementation (`drivers/wdm/audio/backpln/portcls/`) contains
`miniport_dmus.cpp`, headed "Copyright (c) 1997-2000 Microsoft
Corporation", which must not be used. ReactOS PortCls files may be read to
understand behaviour, not copied, unless a file's own header is checked
GPL-compatible first.

Microsoft's DDK sample sources (the MPU-401 and DMusUART samples) are not a
source and are not read (Owner, 2026-10-20; `AGENTS.md`, "Sources"): the
DDK documentation and headers are.

## 13. Test plan

QEMU has no MIDI device model: `hw/usb/dev-audio.c` declares AudioControl
and AudioStreaming interfaces only (no MIDIStreaming). Passthrough of a
real device is the VM path, with a limit xHCI98 measured: on a Windows
QEMU host, passthrough of a multi-interface audio function fails, because
libusb cannot claim interfaces grouped into one function (xHCI98
`docs/contributing/build-and-test.md`, "Passthrough of a multi-interface
function is shut on a Windows host, and USB Audio is always that shape").
An AC+MS MIDI device is that shape.

Owner, 2026-10-20: MIDI testing runs on a real 98 SE / 2000 PC with the
devices; there is no Linux QEMU host (where libusb claims every
interface). Vendor-class single-interface devices (many Roland and Yamaha
interfaces) may still pass through to a guest as single-function
devices.

Test devices (Owner, 2026-10-20: one or two mainstream devices per class;
no Raspberry Pi gadget rig):

- A CH345 cable (cheap, the commonest DIN adapter; tests the SysEx quirk
  and the 4-byte packet rule).
- One class-compliant keyboard or controller (AC+MS).

Multi-port interfaces, port naming from `iJack` and the Roland and Yamaha
vendor-class families are covered by host tests on captured descriptor sets
and tested on hardware only when a user's device reaches them.

Host tests (no Windows): the packet encoder against MIDI spec Table 4-2's
examples; running status, interleaved real-time, SysEx of every length
0..300 and cut SysEx; decoder against the CH345 and Uno captures in
Linux's comments; descriptor parser against captured descriptor sets
(f_midi, keyboard, interface, the Roland `F1 02` form, malformed lengths).

Tested systems (Owner, 2026-10-20, README "Tested by the author"): 98 SE,
2000 SP4, XP SP3 32-bit and 7 x64. On each: install on the generic id
(98 SE and 2000) or the id list (XP, 7 x64) and by hand; ports and names
as listed in
`midiOutGetDevCaps` / `midiInGetDevCaps` (a small test program, built with
MSVC 6.0); a 10-minute loopback of note, controller and SysEx traffic
through a DIN cable from output to input, compared byte for byte; a 64 KiB
SysEx dump at full rate through a DIN port with and without pacing;
unplug with ports open; replug; standby and resume; under Microsoft's
stack, under NUSB's EHCI stack on 98 SE (fully supported, Owner,
2026-10-20) and under xHCI98. On XP and 7 x64, coexistence with the in-box
driver (which one binds, switching back with Update Driver).

Every tested system has an EHCI leg: NUSB's EHCI on 98 SE, Microsoft's on
2000, XP and 7 x64. Both test devices are Full Speed, so an EHCI leg puts
them behind a High Speed hub on the EHCI port, where its transaction
translator makes EHCI carry the traffic; the same device on a bare EHCI
port (or a QEMU EHCI port) lands on the companion UHCI/OHCI controller and
is not an EHCI leg.

ME, XP x64, Vista (x86 and x64), 7 x86 and 8 (x86 and x64) are not tested.
Their folders ship "Built, not tested"; no reading that only they could
settle blocks a release, and the release notes list what stays unverified
there: coexistence with the in-box driver, WDMAUD's port names (5.4),
removal with open handles, and on ME the kernel timer resolution (6.6).

Known limitation the 98 SE release notes state (Owner, 2026-10-22,
section 18): on 98 SE, restart Windows after replugging a USB MIDI
device, because 98 SE's own MIDI path (WinMM and its WDM audio clients)
refuses every MIDI open on a device that arrives after any USB MIDI
device has been removed (the same one or another); the driver is not
reached.

The results go into `README.md`'s "Tested by the author" table.

## 14. Risks

- R1. PortCls on 98 SE may not start an adapter without resources, or may
  assume hardware resources somewhere (5.1). M0 settles it before any other
  work; the fall-back is a raw KS filter, which multiplies the work.
- R2. WDMAUD naming per system (5.4) may differ from the design's
  assumption; worst case the ports all read the device name with WinMM's
  numbering.
- R3. The AC+MS binding gap (4.2): unlisted class-compliant devices on
  98 SE and 2000 need a manual install, documented in the release notes.
  This is the main user-visible limitation.
- R4. Hot-unplug with open MIDI handles on 98 SE/ME may crash in KS or
  WDMAUD (system code, not ours); test early.
- R5. Kernel timer granularity on 9x for pacing (6.6).
- R6. Vista+ x64 refuses unsigned drivers (signed as xHCI98 and Stor98
  are); Vista+ x86 ranks an unsigned driver below the in-box one (4.2).

## 15. Owner decisions and open questions

Owner, 2026-10-20, on the questions this draft raised:

- Q1. Names: package `usbmidi`, binary `usbmidi.sys`, accepted. Release
  folders follow the one scheme of `README.md`, "Layout" (section 8).
- Q2. ME and XP to 8: the generic `SubClass_03` match ships on 98 SE and
  2000 only; on ME and XP to 8 the INF lists only specific devices Windows
  gets wrong, by VID/PID.
- Q3. Unlisted AC+MS devices: manual ("Have Disk") install for now,
  documented; no helper.
- Q4. No DirectMusic-native ports (`IMiniportDMus`).
- Q5. Signing for x64 Vista to 8: the same approach as xHCI98 and Stor98.
- Q6. M-Audio MidiSport: dropped from this driver's scope. M-Audio's own
  driver supports 98, ME, 2000 and XP and is officially downloadable
  (archived), but its licence forbids redistribution, and a free driver
  would need the same restricted firmware. `usbmidi` covers class-compliant
  devices and the vendor-class families of section 7.
- Q7. DDK sample sources (MPU-401, DMusUART) may not be read; export tables
  of in-box binaries may be read for names and ordinals only.
- Q8. No sharing of one output port by several clients.
- Q9. Test hardware: one or two mainstream devices per class; no Raspberry
  Pi gadget rig.
- Q10. The id split and the shared `src/audio/common/` of section 9:
  accepted, as `uac2.md` section 11 also states.
- Back-ported EHCI stacks: NUSB's EHCI stack on 98 SE is fully supported
  and in the test matrix.
- Test machines: AC+MS devices are tested on a real 98 SE / 2000 PC; no
  Linux QEMU host (section 13).
- Owner, 2026-10-22: the restart Windows 2000 asks for when a new
  instance installs (the device on another port or hub port, or a second
  device) is accepted for M1 and documented, not fixed (section 18).
- Owner, 2026-10-22: on 98 SE a replugged USB MIDI device fails every
  MIDI open until Windows restarts (section 18); a known limitation, no
  code change, stated in README and the release notes (section 13).

### Open questions

None at present.

## 16. Milestones

Smallest useful first. Each is built and gated on the systems named, and
tested on the tested systems among them (section 13), before the next
starts.

- **M0 - PortCls spike** (98 SE, 2000, XP): an adapter that starts on a
  USB function with an empty resource list, registers one MIDI subdevice,
  and appears as one `midiOut` device; `Write` discards. Settles R1, the
  topology question (5.3), the C or C++ choice (5.2) and the 98 SE import
  gate.
- **M1 - class-compliant MIDI, first release** (98 SE and 2000; XP x86
  with the id list only, and ME's id-list folder built, not tested):
  descriptor parsing, ports from cables, output
  and input, running status, SysEx, real-time, round-robin, stall and
  removal handling, per-port filters and names, generic `SubClass_03` match
  (98 SE and 2000) and documented manual install, host tests. This alone gives 98 SE and 2000 USB MIDI they never had.
  Estimated a few thousand lines of C; an estimate, not a measurement.
- **M2 - common quirks and the device list**: CH345, M-Audio Uno, 4-byte
  packets, fixed-endpoint, Yamaha, Roland (with the interrupt alternate),
  port-name table, generated INF hardware ids and their gate; DIN pacing
  quirk and `UmFlags`.
- **M3 - all targets**: Vista, 7, 8 x86; XP x64, Vista to 8 x64 (signed
  as xHCI98 and Stor98 are), tested on 7 x64 and the rest built, not
  tested (section 13); power (standby/resume) on every tested system.
- **M4 - vintage vendor protocols**, each only with a device in hand:
  Emagic, Novation, CME, Akai, FTDI, raw bytes.
- **M5 - shared core in the UAC 2 driver** (section 9), when that driver
  reaches its PortCls stage.

## 17. Not verified in this draft

Collected from the sections above, each with what settles it:

- ME's `wdma_usb.inf` and Windows 8's (3.1): read the files from the
  install media.
- Audio-interface grouping by 98 SE's and 2000's own `usbhub.sys` (4.1):
  enumerate an AC+MS device under the stock stacks.
- 98 SE's Update Driver installing a non-matching model (4.3): 98 SE leg.
- AVStream on 98 SE / 2000 (5.1): not needed by the design; DDK
  documentation of AVStream's supported systems.
- PortCls on 98 SE with an empty resource list (5.1, R1): M0.
- WDMAUD needing a topology filter or a `mixer` subclass for a MIDI-only
  device (5.3, 8): M0/M1.
- When the port driver retries a refused `Write` (5.3): M1 trace.
- WDMAUD's naming source per system (5.4): M1 on each tested system; on
  the others it stays unverified (section 13).
- `IoOpenDeviceInterfaceRegistryKey` on 98 SE (5.4): import gate and load.
- Kernel timer resolution on 98 SE / ME (6.6): measure on 98 SE in M2; ME
  is not tested and stays unverified (section 13).
- The USB MIDI 2.0 class definition's section for the alternate-setting
  rule (6.2): read the specification.
- Passthrough with WinUSB on the whole device (13): try once.

## 18. Implementation notes (2026-10-22)

What the M1 code settled or changed against this draft; the evidence is in
`docs/testing/2026-10-22-usbmidi-m1.md`.

- Source layout: the MIDI core is still in `src/audio/usbmidi/`
  (`msdesc.c`, `umpkt.c`, `umusb.c`, `usbmidi.cpp`), not yet in
  `src/audio/common/` (section 10), because that folder belongs to the
  UAC 2 work at present; it moves when M5 shares it. The adapter is C++
  with explicit COM classes and its own `IUnknown` (5.2), no `stdunk`.
- Imports: one x86 binary keeps the 98 SE import set. Final Release above
  PASSIVE_LEVEL uses `ExQueueWorkItem` with an FDO reference and a work
  routine that tail-jumps to `ObfDereferenceObject`, instead of
  `IoAllocateWorkItem`, which 98 SE lacks; x64 uses `IoQueueWorkItem`.
  The build gates this against the 98 SE DDK import libraries (11).
- Parser tolerance (6.1) is as written: skipped, not fatal, except a
  duplicate jack ID, a bcdMSC other than 1.00 on the chosen alternate and
  malformed lengths. `wTotalLength` is not enforced.
- Filters (5.3): one filter per port pair, output port i with input port
  i; render-only or capture-only descriptors where a direction is missing.
- IN side (6.4): two requests per IN endpoint, not three.
- Stop/restart (5.2): `IRP_MN_QUERY_STOP_DEVICE` is refused; there is no
  resource to rebalance and no restart path.
- Power (5.2): D3 cancels the IN requests and holds OUT; D0 resubmits.
- Restart on a new instance (accepted for M1, owner 2026-10-22): on
  Windows 2000 a new instance (the same device on another root or hub
  port, or a second device), and any install while another WDM audio
  device runs, ends with Windows asking for a restart. The cause is the INF's
  `Needs=KS.Registration,WDMAUDIO.Registration.NT`: those sections copy
  the system's KS and WDM audio files, which are loaded whenever any WDM
  audio device runs, and setupapi treats them as in use ("files in use,
  or treated as in use"), even when `usbmidi.sys` itself is not copied.
  The device works after the restart. Replugging into a port where it
  was installed before needs no restart. What the user sees: one
  restart prompt per new port. 98 SE asks for no restart on a new port:
  it names a device without a serial number `INST_0` on every port, so
  it is the same devnode.
- 98 SE replug (2026-10-22 test log, "Windows 98 SE replug and freezes"):
  once a devnode has been removed, every WinMM MIDI open on it (input and
  output) fails with MMSYSERR_ALLOCATED until Windows restarts, without
  reaching the driver; other devices are not affected. New interface
  reference strings or names per arrival and the stock `WDMA_USB.INF`
  devnode values did not change it. Owner, 2026-10-22: a known 98 SE
  limitation, no code change; README and the release notes say to
  restart Windows after replugging a USB MIDI device on 98 SE (section
  13, 15). The QUERY_STOP refusal also means Device Manager cannot disable
  the device on 98 SE (a disable is a STOP there).
  Fix attempt (owner, 2026-10-23; test log 2026-10-23-usbmidi-98se-replug):
  a devnode new to the boot fails the same way after another MIDI
  devnode's removal, an orderly removal (query remove first) does not
  help, and hiding the filters from the client that registers on their
  target device removes the WinMM ports altogether. So the state is in
  98 SE's WDM MIDI client side, shared by every later arrival, and a child
  devnode per arrival (built and run, not kept) cannot help. No driver fix;
  the limitation stands.
- Select configuration (6.2) on ME: ME's composite parent completes the
  function's SELECT_CONFIGURATION with a configuration handle but leaves
  the pipe entries empty; when a mapped endpoint has no pipe handle the
  driver sends SELECT_INTERFACE for the MIDIStreaming interface on that
  handle and takes the pipes from it (2026-10-22 test log, "Windows ME").
  ME's replug works; 98 SE's limitation is its own.
- Worker thread stop (6.7): 98 SE faulted waiting on a thread object;
  the worker now sets an `Exited` event, which the stop waits for, and
  only WDM 1.10 systems also wait for the thread object.
- INF (8): one template, `usbmidi.inf`, and `mkinf.ps1`, which writes the
  per-folder INFs and checks the matching policy of 4.2. The manual model
  (4.3) carries the id `USBMIDI_BY_HAND`, which no device reports. The
  M1 device list holds the Roland UM-ONE mk2 in TAB mode (0582:0168,
  MI_00) on 98 SE and 2000 only.
- Not yet done from M1: stall/removal handling is written but only the
  removal path has run; quirks (M2), pacing (6.6) and `UmFlags` are M2.

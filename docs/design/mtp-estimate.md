# MTP and PTP on Windows 98 SE, ME and 2000: an estimate

Status: estimate of 2026-11-06, no code (owner, 2026-11-06: "an estimate
only"). It sizes what an MTP (Media Transfer Protocol) and PTP (Picture
Transfer Protocol) package would take on 98 SE, ME and 2000, how a phone's
or camera's files could be presented on each, and what could test it. Every
size below is an estimate, not a measurement.

## 1. The devices and what they speak

- **Digital cameras** speak PTP, the USB Still Image class: interface
  `06/01/01`, a bulk OUT, a bulk IN and an interrupt IN pipe.
- **Android phones** offer "File transfer" (MTP) and "PTP" modes. In PTP
  mode the interface is `06/01/01`. In MTP mode the interface is commonly
  vendor class `FF`, and the host finds MTP through Microsoft's OS
  descriptors (a string descriptor at index `0xEE` and an Extended
  Compatible ID feature descriptor naming `MTP`); some phones present
  `06/01/01` in MTP mode too. Which form a given phone uses is not
  verified here.
- **Media players** of the 2005 to 2012 period (and some current ones) speak
  MTP, often only MTP.
- **iPhones** present a PTP interface for their camera roll.

MTP is an extension of PTP: the same containers on the same three pipes,
the same session and object model, plus object properties, more formats
and some more operations. A driver that speaks PTP's core reads files from
most MTP devices too, with two limits: objects of 4 GB or more need MTP's
64-bit object size property, and MTP-only devices that do not fill PTP's
ObjectInfo fully need MTP's property operations to list sensible names and
sizes (not verified per device).

## 2. Specifications needed

All public:

- **USB Still Image Capture Device Definition 1.0** (USB-IF): the
  container format on the bulk pipes (command, data, response, event
  blocks), the interrupt event pipe, and the four class requests (Cancel
  Request, Get Extended Event Data, Device Reset Request, Get Device
  Status).
- **PTP**, ISO 15740 (first published as PIMA 15740:2000): operations,
  responses, events, datasets (DeviceInfo, StorageInfo, ObjectInfo), object
  formats, the association (folder) model. The parts used: OpenSession,
  CloseSession, GetDeviceInfo, GetStorageIDs, GetStorageInfo,
  GetNumObjects, GetObjectHandles, GetObjectInfo, GetObject,
  GetPartialObject (optional on devices), DeleteObject, SendObjectInfo,
  SendObject, and the events ObjectAdded, ObjectRemoved, StoreAdded,
  StoreRemoved, DeviceInfoChanged.
- **MTP 1.1** (USB-IF): object property operations
  (GetObjectPropsSupported, GetObjectPropDesc, GetObjectPropValue,
  GetObjectPropList), the 64-bit Object Size property, the Object File
  Name and Date Modified properties. Beyond MTP 1.1, Android adds
  extensions (`GetPartialObject64`, `SendPartialObject`, `BeginEditObject`,
  `EndEditObject`, `TruncateObject`; AOSP `mtp.h`); a driver finds them in
  the device's DeviceInfo operation list and must work without them.
- **Microsoft OS Descriptors** (Microsoft's published specification): to
  recognise an MTP-mode device that uses vendor class `FF`.

Reference code for facts: libgphoto2's `camlibs/ptp2` (LGPL-2.0-or-later
per `ptp.c`'s header; `candidates.md` 2) and libmtp (LGPL-2.1-or-later);
Linux has no kernel PTP/MTP host driver (it is done in user space).

## 3. What each system already has

From the install media (file names and INF text only, as `candidates.md`):

| | 98 SE | ME | 2000 SP4 |
|---|---|---|---|
| Kernel transport for `06/01/01` | `usbscan.sys` (STI), no INF that matches the class | `usbscan.sys` | `usbscan.sys` |
| PTP above it | none | WIA with a PTP driver: `sti.inf` has an `STI.PTPUSBSection` that registers `ptpusd.dll` and `ptpusb.dll`; no INF in ME's set matches `USB\Class_06&SubClass_01&Prot_01` to it (ME's `kdk2x0.inf` lists Kodak cameras by id with their own driver) | none (STI only; WIA came with ME and XP) |
| Where files appear | - | WIA's "Scanners and Cameras" and the camera's folder in My Computer, for a device WIA has | - |
| MTP | none | none | none |

Windows Media Device Manager (WMDM) ships with Windows Media Player 7 to 9
on these systems and lets the player sync to a device through a service
provider DLL. Microsoft's paper "MTP Implementation Guidelines" (revision
1.2, read here only as a third-party mirror) describes MTP drivers for
98 SE, ME and 2000 that expose devices through WMDM; whether Microsoft
still offers them, on what terms, and how well they work is a survey item
to settle first (the makers'-drivers-first policy of 2026-11-06), before
any route below is scheduled.

## 4. The ways to present a device's files, per system

Every route shares one piece: a **PTP/MTP protocol library** in C
(sessions, containers, datasets, transactions, events) and a **transport**
to the device's three pipes and its class requests.

The transport is not settled. `usbscan.sys`, Microsoft's in-box still
image driver, gives user mode the bulk pipes, the interrupt pipe
(`IOCTL_WAIT_ON_DEVICE_EVENT`), pipe reset and cancel, but the 98 DDK's
`usbscan.h` has no request that sends a class request to the interface:
its only control requests are the vendor register reads and writes
(`IOCTL_READ_REGISTERS`, `IOCTL_WRITE_REGISTERS`). PTP's recovery needs
the class requests Cancel Request, Device Reset Request and Get Device
Status; without them a cancelled or failed transfer can leave the session
unusable until the device is replugged. Usable as is only if that is
accepted; otherwise a small WDM transport driver of ours (bulk, interrupt
and the four class requests, one interface, user-mode IOCTLs) at about
1,500 lines, with its own legs on every system, replaces it. On 98 SE a
composite device (a phone's MTP interface beside ADB or a modem) also
needs the interface split, which `usbscan.sys` does not do: Microsoft's
documentation of the 98 imaging architecture names a separate driver
(`usbscn9x.sys`) for composite imaging devices (whether 98 SE ships it
is not read here); under NUSB and xHCI98 the host stack splits composites, under 98's
own stack the interface would need our transport driver bound by
`&MI_xx`. A vendor-class (`FF`) MTP interface needs an INF that names its
VID/PID (and interface) either way.

| Route | Systems | What the user sees | Size (estimate) | Risk |
|---|---|---|---|---|
| A. INF only: `06/01/01` to `usbscan.sys` with ME's own WIA PTP driver | ME | the camera under Scanners and Cameras, its pictures through WIA's camera wizard and the camera's folder | an INF, about 100 lines; 1-2 days with legs | low; whether ME's `ptpusd.dll` copes with a current phone's PTP mode is unknown; MTP-mode phones not covered |
| B. A user-mode transfer tool over the transport | 98 SE, ME, 2000 | a program: pick a device, browse storages and folders, copy files to and from a folder | library 2,500-3,500 lines, tool UI 1,500-2,500, INFs; with our own transport driver about 1,500 more; 4-7 weeks | medium: the transport (above); no shell integration; MTP property quirks per device |
| C. A shell namespace extension over the library | 98 SE (IE4 shell or later), ME, 2000 | the device as a folder under My Computer; copy, drag and drop, delete | library as B, plus 6,000-10,000 lines of COM (`IShellFolder`, `IEnumIDList`, `IExtractIcon`, `IDataObject` with file contents, `IDropTarget`), a background thread for the device; 2-3 months | medium-high: file contents can be streamed (`CFSTR_FILECONTENTS` with an `IStream`) or staged in a temporary file, but which 98 SE and 2000 Explorer versions accept which is to be read; per-shell-version behaviour |
| D. A file system: an IFS (98 SE, ME) or redirector-like file system driver (2000) giving a drive letter | 98 SE, ME, 2000 | a drive letter that every program can open | library in ring 0, 9x IFS manager hooks or an NT file system, a write-back cache (baseline MTP has no partial write: a changed file is sent again whole unless the device offers Android's edit extensions), a metadata cache; 15,000-25,000 lines, two different kernels; 6+ months | high: object model is not a block or byte-addressed file system; random writes, rename and timestamps do not map; a crash corrupts nothing on the device but loses cached writes |
| E. A WIA driver for ME of our own (MTP, or a fuller PTP) | ME | WIA's camera wizard for MTP devices | library plus a WIA minidriver (COM, the ME WIA DDK interfaces); 4-6 weeks | medium: WIA on ME is camera-shaped (pictures), so music and documents fit badly |
| F. A WMDM service provider | 98 SE, ME, 2000 with WMP 7-9 | the device in Windows Media Player's portable device sync | library plus the WMDM SP interfaces; 4-8 weeks | medium-high: needs the WMDM SDK's interfaces and certificates of the period; music only |

A TWAIN data source (for imaging programs to acquire pictures) is a variant
of B for cameras, about 1,500 lines more.

## 5. Testing

- **QEMU `usb-mtp`** (`hw/usb/dev-mtp.c`) is an MTP responder over a host
  directory, with `readonly` off it accepts SendObject and DeleteObject.
  QEMU does not build it for a Windows host (`hw/usb/meson.build`:
  `if host_os != 'windows'`), so neither this host's QEMU 11.1 nor the
  usb4-emu fork builds list it (`-device help`); a Linux host's QEMU would
  run it.
- **A simulator of this repository's kind** (`mtpemu.py` over `usb-redir`,
  like `uacemu.py` or `acmemu.py`) is the practical vehicle here: a PTP
  responder with a small object tree, both `06/01/01` and the vendor-class
  MTP shape with OS descriptors, events, and a mode that is camera-like
  (PTP only) and one that is phone-like (MTP properties, 64-bit sizes).
  About 800-1,200 lines.
- Real devices for the bench: one PTP camera, one Android phone in MTP and
  PTP modes, an iPhone (PTP).

## 6. Recommendation

1. **ME first, INF only (route A)**: bind `06/01/01` to ME's own
   `usbscan.sys` and WIA PTP driver. It is small, ships nothing of
   Microsoft's, and gives ME users camera import at once. Read on ME with
   the simulator before deciding anything else.
2. **Then, after the WMDM survey item of section 3, the protocol library
   and a transfer tool (route B)** for 98 SE, ME and 2000, with the
   simulator, once the transport question of section 4 is read on a
   guest (whether `usbscan.sys` without class requests is enough). It
   covers cameras and phones in both modes and is the base every other
   route needs.
3. **Not now**: the shell namespace extension (C), a drive letter (D), a
   WIA driver (E) or a WMDM provider (F). C is the natural next step if B
   is used; D is the largest and least reliable and is not recommended.

Owner questions (for when this is taken up):

- Q1. Route A for ME on its own, before anything else? Recommended: yes.
- Q2. Route B's tool: a GUI program (like CamCheck) or a command-line copy
  tool first? Recommended: GUI, with a command-line mode for scripts.
- Q3. Writing to the device (SendObject, DeleteObject) in the first tool, or
  read-only first? Recommended: read-only first.

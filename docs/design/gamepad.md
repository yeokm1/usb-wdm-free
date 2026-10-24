# USB gamepad driver (xpad98)

Status: design draft, 2026-10-20; implementation in progress under
`src/hid/xpad98/` (work log `docs/testing/2026-10-21-xpad98.md`).

## 1. Purpose and devices served

A WDM driver that lets Xbox-protocol USB gamepads act as ordinary game
controllers on Windows 98 SE, ME and 2000, and Xbox One and Series pads on
XP and Vista as well, for retro gaming: DirectInput
games, games that use the WinMM joystick API (`joyGetPosEx`), and the Game
Controllers control panel (`joy.cpl`).

Owner, 2026-10-20, after the survey of existing drivers (`existing.md`
section 6): the scope is the gap other drivers leave.

- **Xbox One and Series pads** on 98 SE, ME, 2000, XP and Vista: no other
  driver serves them there. Microsoft has no Xbox One driver for XP or
  Vista; Windows 7 and 8 have Microsoft's own, so they are not targets
  (Owner, 2026-10-20).
- **The Xbox 360 Wireless Gaming Receiver** on 98 SE and ME only. On 2000
  (and on to 7), Masahiko Morii's `x360wc` serves it, so this driver
  leaves it alone there.
- **Xbox 360-protocol pads outside the ID list of Morii's `x360c`**, such
  as 8BitDo pads in XInput mode and current clones, on 98 SE, ME and
  2000. `x360c`'s final 2010 builds match 25 VID/PIDs (Microsoft
  `045E:028E`, HORI, Mad Catz, Logitech, Saitek; listed in section 5.1);
  those pads are left to it.
- **No Xbox 360-protocol pads on XP and later.** The opt-in DirectInput
  mode once planned there is dropped: XBCD, a free (GPL-2.0-or-later)
  driver, serves them. On XP and Vista this driver's INF matches only Xbox
  One and Series (GIP) ids.
- **Not 7 or 8**: Microsoft's own driver serves Xbox One and Series pads
  there, and XBCD the 360-protocol pads.

Morii's drivers are not free software; `x360c` (360 wired pads, 98 SE to
7) and `x360wc` (the 360 receiver, 2000 to 7) are carried in
`usb-wdm-nonfree`. This driver works without them.

These pads are not HID devices. They use vendor-specific interfaces, so no
target system's in-box HID stack touches them, and on 98 SE, ME and 2000
nothing else does either.

| Family | Interface | Protocol | First release |
|---|---|---|---|
| Xbox 360-protocol wired pads outside `x360c`'s ID list: 8BitDo and others in XInput mode, current clones | class `0xFF`, subclass `0x5D`, protocol `0x01` | XUSB: 20-byte input report on interrupt IN, LED and rumble commands on interrupt OUT | yes |
| Xbox 360 Wireless Gaming Receiver for Windows | class `0xFF`, subclass `0x5D`, protocol `0x81` (129), one interface per pad slot | XUSB wrapped in a wireless envelope, with presence events | yes, after wired; 98 SE and ME only |
| Xbox One, Series X\|S and compatible wired pads | class `0xFF`, subclass `0x47`, protocol `0xD0` (208), interface 0 | GIP: needs an init sequence before it sends input | yes, after wireless; the only family on XP and Vista |
| Original Xbox pads (with a USB adapter cable) | class `0x58`, subclass `0x42` (`'X'`, `'B'`) | XID | not planned (owner) |
| PlayStation 3 Sixaxis/DualShock 3 over USB, Switch Pro Controller over USB | HID class, but unusable without init | vendor init over HID | not planned (owner; section 2) |

All matching rules above are Linux `drivers/input/joystick/xpad.c`
(`xpad_table[]`, macros `XPAD_XBOX360_VENDOR_PROTOCOL` and
`XPAD_XBOXONE_VENDOR_PROTOCOL`, the `USB_INTERFACE_INFO('X', 'B', 0)` entry,
and `xpad_probe`, which picks `XTYPE_XBOX360W` for protocol 129 and
`XTYPE_XBOXONE` for protocol 208). Read from GitHub `torvalds/linux` master
on 2026-10-20; SPDX `GPL-2.0-or-later`. 8BitDo's XInput-mode pads appear in
the same file's `xpad_device[]` (for example `2DC8:3106` as `XTYPE_XBOX360`,
`2DC8:2000` as `XTYPE_XBOXONE`).

## 2. Scope decision: HID-class pads

Pads that are already USB HID devices (generic USB pads, DualShock 4 and
DualSense over USB, most PC-branded pads) bind on every target to the in-box
HID stack: `hidusb.sys` under `hidclass.sys`, whose game-controller
collections reach DirectInput and, on 98 SE and ME, the legacy joystick API
through `joyhid.vxd` (section 4.1). **They are out of scope for this driver.**
They may show odd mappings (DualShock 4 reports its triggers as Rx/Ry and has
14 buttons), but that is the game's problem as on any HID pad, and a
replacement driver for a working device is a poor trade.

Two HID-class pads are different, because the in-box stack cannot make them
send input at all:

- **DualShock 3 / Sixaxis over USB.** Linux sends a GET_REPORT for feature
  report `0xF2` before the pad streams input (`drivers/hid/hid-sony.c`,
  `sixaxis_set_operational_usb`; it also reads `0xF5`), and replaces the
  report descriptor (`sixaxis_fixup`). SPDX `GPL-2.0-or-later`.
- **Switch Pro Controller over USB.** Linux sends USB-only commands
  `0x80 0x02` (handshake), `0x80 0x03` (3 Mbit), `0x80 0x02` again, then
  `0x80 0x04` (no timeout) before the pad talks HID (`drivers/hid/
  hid-nintendo.c`, `joycon_init`, `JC_USB_CMD_*`). SPDX `GPL-2.0+`.

These would fit this driver's model unchanged (section 3): a front end that
owns the USB device, performs the init, and hands the common back end a pad
state. Owner, 2026-10-20: neither is planned, and neither are original Xbox
pads (XID).
Whether 98 SE's `hidparse.sys` accepts the DualShock 4 and DualSense report
descriptors (vendor usage pages, 64-byte reports, many feature reports) is
**unverified**; one plug-in on 98 SE settles it, and the answer belongs in the
release notes either way, not in this driver.

## 3. The decision on the OS-facing model

**Decision: one WDM HID minidriver, registered with `HidRegisterMinidriver`,
that sits under the in-box `hidclass.sys` as the function driver for the pad's
USB device and presents a report descriptor of its own making.** `hidclass.sys`
then creates a HID collection PDO per top-level collection, the system's
compatible id `HID_DEVICE_SYSTEM_GAME` matches it, and every consumer that
already handles HID game controllers sees a standard one.

Options considered:

1. **HID minidriver (chosen).** One WDM binary serves every target: the
   minidriver interface is in the 98 DDK (`HID_DDK\SRC_HID_INC_HIDPORT.H`:
   `HID_MINIDRIVER_REGISTRATION` with `Revision`, `DriverObject`,
   `RegistryPath`, `DeviceExtensionSize`, `DevicesArePolled`;
   `HidRegisterMinidriver`; `IOCTL_HID_GET_DEVICE_DESCRIPTOR`,
   `_GET_REPORT_DESCRIPTOR`, `_READ_REPORT`, `_WRITE_REPORT`, `_GET_STRING`,
   `_ACTIVATE_DEVICE`, `_DEACTIVATE_DEVICE`, `_GET_DEVICE_ATTRIBUTES`) and
   an import library (`LIBS_DDK\LIB_I386_FREE_HIDCLASS.LIB`); the DDK's
   `hidmini` sample is not read (Owner, 2026-10-20). WDK 7.1's
   `inc\ddk\hidport.h` has the same IOCTLs plus
   `IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST`.
   Everything above the minidriver (VJoyD, DirectInput, `joy.cpl`) is the
   operating system's own, already tested with HID game controllers.
2. **A VJoyD mini-driver (VxD).** The DirectX DDK documents it ("The DirectX
   5 Interface", `VJPOLLDATA`), but it is 9x only, a VxD rather than WDM,
   and would leave 2000 and later with nothing. Rejected.
3. **A bus driver that creates a virtual HID device for a separate
   minidriver.** Needed only if one USB function had to become several
   independent devnodes. The multi-collection report descriptor (section
   6.5) does that inside one minidriver, so the extra driver buys nothing.
   Rejected.

## 4. Target systems and the per-OS plumbing

### 4.1 Windows 98 SE

- **The HID stack is in the box.** `hidclass.sys`, `hidparse.sys` and
  `hidusb.sys` are on the 98 SE CD (`BASE4.CAB` and following, read on
  `D:\isos\Win98SE.iso` 2026-10-20) and extracted on the xHCI98 host
  (`tools\win98se-extracted\wdm\`). The 98 DDK's `DOC_DDKGUIDE.DOC` file
  table lists `HIDCLASS.SYS`, `HIDPARSE.SYS`, `HIDUSB.SYS` (in
  `\system32\drivers`) and `JOYHID.VXD`, `VJOYD.VXD`, `HID.DLL` (in
  `\system`) as the HID components.
- **How a minidriver is bound on 9x.** `hiddev.inf` (98 SE,
  `[USBHIDDevice]`) copies `hidclass.sys`, `hidusb.sys`, `hidparse.sys` and
  sets `HKR,,DevLoader,,*ntkern` and `HKR,,NTMPDriver,,"hidusb.sys"`. This
  driver's 9x INF follows that in-box pattern with its own binary as
  `NTMPDriver`.
- **How a HID game controller reaches the legacy API.** `joystick.inf`
  (98 SE, `PRECOPY2.CAB`) matches `HID_DEVICE_SYSTEM_GAME` to
  `[HIDGameDevice]`, which copies `joyhid.vxd` and sets
  `DevLoader="*ntkern,mmdevldr.vxd"`, `NTMPDriver="joyhid.vxd"`,
  `Driver=vjoyd.vxd` and the `Drivers\joystick\msjstick.drv` entries. The
  DirectX DDK ("Joystick Driver Model", `HELP_DXDDK.CHM` topic
  `di_ddk_5e7o`) says: "Dinput.dll uses VJoyD and, if available, the Human
  Input/Interface Device (HID) stack to provide polling services. HID devices
  are also reported through VJoyD so that applications using the older APIs
  will still be able to read the new devices." So a collection that gets
  `HID_DEVICE_SYSTEM_GAME` serves WinMM and DirectInput both, with nothing of
  ours above the minidriver. The same INF binds the SideWinder Game Pad USB
  (`USB\VID_045E&PID_0007`, `[MSUSBGAMEPAD1]`) to `hidusb.sys` by the same
  route, a 1998 precedent for a gamepad on this chain.
- **Which top-level usages get `HID_DEVICE_SYSTEM_GAME` on 98 SE**: Joystick
  (`0x01/0x04`) certainly by its name; Game Pad (`0x01/0x05`) **unverified**
  for Microsoft's 98 SE `hidclass.sys`. ReactOS's reimplementation emits it
  for both (`drivers/hid/hidclass/pdo.c`, `case HID_USAGE_GENERIC_GAMEPAD:
  case HID_USAGE_GENERIC_JOYSTICK:`), which is evidence of the convention
  but not of the 98 SE binary. M1 reads the devnode's compatible ids on 98 SE
  with both usages; the top-level usage is a registry option (section 6.3)
  so the answer costs no code.
- **How `joyhid.vxd` assigns HID usages to WinMM's six axes** (X, Y, Z, R,
  U, V in `JOYINFOEX`) is **unverified**: no DDK page found says. The
  DirectX DDK documents only the analog defaults ("R (rudder) defaults to the
  J2 X axis. Z defaults to the J2 Y axis", `di_ddk_01is`). M1 settles it
  empirically with a `joyGetPosEx` test program against each axis usage.
- DirectInput: 98 SE ships `dinput.dll` (DirectX 6.1 era); DirectX up to
  9.0c can be installed. DirectInput reads HID collections directly through
  `hid.dll` (`DIOBJECTATTRIBUTES` carries `wUsagePage`/`wUsage`,
  `di_ddk_0yes`).

### 4.2 Windows ME

The ME CD carries `hidclass.sys`, `hidusb.sys`, `hidparse.sys`, `hid.dll`,
`joyhid.vxd`, `vjoyd.vxd` and DirectInput (file names read from
`D:\isos\Windows Me OEM Full.iso` by the project survey, 2026-10-20). ME's
`hiddev.inf` and `joystick.inf` were not read here; the plumbing is assumed
to be 98 SE's and is **unverified** until those two INFs are read (one
extraction, as for 98 SE above). The same 9x binary and INF are planned.

### 4.3 Windows 2000 and XP

- HID stack in the box (`win2ksp4-extracted\class\HIDCLASS.SYS`,
  `winxpsp3-extracted\hid\hidclass.sys`). `input.inf` binds HID minidrivers
  as services: `[HID_Inst.NT]` copies `hidusb.sys`, `hidclass.sys`,
  `hidparse.sys` and `hid.dll`, and `[HID_Inst.NT.Services]` has
  `AddService = HidUsb,%SPSVCINST_ASSOCSERVICE%,HID_Service_Inst` (XP SP3
  `input.inf`). This driver's NT INF does the same with its own service.
- Game collections get no driver of their own: `input.inf` matches
  `HID_DEVICE_SYSTEM_GAME` to `HID_Raw_Inst`, a name only ("no service
  added, no functionality added", 2000 SP4 and XP SP3 `input.inf`).
  DirectInput opens the collection through `hid.dll`. How 2000/XP's WinMM
  joystick functions reach a HID controller (there is no VJoyD on NT) is
  **unverified** here; the M4 test reads `joyGetPosEx` on 2000.
- No XInput driver on either CD (project survey of `D:\isos`, 2000 SP4 and
  XP SP3). XP adds `hidgame.sys` and `gameenum.sys`, for game-port devices,
  irrelevant here.

### 4.4 Windows Vista, 7, 8

- Microsoft's 360-pad driver (`xusb21`/`xusb22`) is not part of these
  systems' base install, as far as read: Microsoft states "the driver is
  available through Windows Update" (Microsoft Learn, "Comparison of XInput
  and DirectInput features", read 2026-10-20). The Windows 7 SP1 x86 image on
  this host (`D:\isos\en_windows_7_professional_with_sp1_vl_build_x86_dvd_u_677896\sources\install.wim`,
  an NTLite-modified image) holds `XInput9_1_0.dll` and `xnacc.inf` but no
  `xusb*.inf`. `xnacc.inf` (class `XnaComposite`, `DriverVer
  06/21/2006,6.1.7600.16385`) matches `USB\Vid_045E&Pid_028E` and
  `USB\MS_COMP_XUSB10` and installs `xnacc.sys` with
  `AddProperty ... GenericDriverInstalled,,,,1`, which reads as a placeholder
  that lets Windows Update supply the real driver. An unmodified 7 image,
  Vista's, and 8's are **unverified**; no Windows 8 image is on this host.
  Reading `install.wim` of each for `xusb*.inf` settles it.
- Microsoft's driver, once installed, exposes the pad to DirectInput with
  "The left and right trigger buttons will act as a single button, not
  independently" and "The vibration effects will not be available" (same
  Microsoft Learn page), the combined-trigger behaviour this design offers as
  one option among several (section 6.3).

### 4.5 Decision: which systems

Owner, 2026-10-20:

- **98 SE, ME, 2000** first, in the scope of section 1. Order of families:
  360-protocol wired pads outside `x360c`'s list, then the 360 wireless
  receiver (98 SE and ME), then Xbox One and Series pads.
- **XP and Vista: Xbox One and Series pads only**, after 2000 (M6).
  Microsoft has no Xbox One driver there. x86 with the same binary as
  98 SE to 2000; x64 (XP x64, Vista x64) as signing allows, by the same
  approach as xHCI98 and Stor98. XP x64 does not enforce kernel signing;
  Vista x64 is planned once that approach is shown to load the driver
  there, and until then `README.md` lists it as not a target.
- **360-protocol pads on XP and later**: XBCD's (`existing.md` section
  6.2). The opt-in "DirectInput mode" once planned is dropped.
- **7 and 8: not targets.** Microsoft's own driver serves Xbox One and
  Series pads there.
- **Not planned**: DualShock 3, Switch Pro, original Xbox pads.

## 5. USB side

### 5.1 Binding level and configuration

The Xbox pads declare a vendor-specific device class, so Windows does not
treat them as composite devices and creates one devnode for the whole device:
Windows 7's `xnacc.inf` matches the whole-device id `USB\Vid_045E&Pid_028E`,
not an `&MI_xx` id. The device-level class/subclass/protocol of each family
(expected `0xFF/0xFF/0xFF` for the 360 wired pad and the receiver) is
**unverified** here and is read with xHCI98's `scripts\local\lsusb-desc.py`
on each test pad in M0. Consequences:

- The driver binds the **device**, so it must read the configuration
  descriptor, find its interface (class/subclass/protocol as in section 1;
  for GIP, interface number 0, since "The Xbox One controller lists three
  interfaces all with the same interface class, subclass and protocol",
  `xpad_probe`), and send `SELECT_CONFIGURATION` itself, built with
  `USBD_CreateConfigurationRequestEx` (98 DDK `INC_WIN98_USBDLIB.H` line
  349). All interfaces are selected at alternate setting 0, so no stack is
  asked to accept a partial configuration; the audio and security
  interfaces' endpoints are opened and never used.
- **Windows offers no class-based compatible id that names these pads**
  (the device-level class `0xFF` gives a useless `USB\DevClass_FF...`
  id), so **the INF must list VID/PID ids**, taken from Linux `xpad.c`'s
  `xpad_device[]` table, filtered to the three families, less the 25 ids
  `x360c` matches (`existing.md` section 6.1). A pad missing from
  the list can still be forced through "Have Disk"; the driver then checks
  the interface triple at start and fails the start with
  `STATUS_DEVICE_CONFIGURATION_ERROR` if it is not one it drives.
  Owner, 2026-10-22: the start also fails when any interface of the
  configuration is not vendor class `0xFF`, so a composite device (a
  keyboard with an XInput function) forced on by "Have Disk" is refused
  rather than losing its other functions. Keyboards and keypads in
  Linux's table (Wooting, Azeron) are left out of every INF.
- **The ids every `xpad98` INF leaves out**: the 25 distinct VID/PIDs
  Morii's final `x360c` builds match (owner, 2026-10-20, from the
  non-free collection's check of those builds; on 98 SE/ME they are split
  across `x360c.inf`, `x360c_hori.inf` and `x360c_mdcz.inf`). None is an
  8BitDo pad or a current clone.

  | Vendor | VID:PID |
  |---|---|
  | Microsoft | `045E:028E` |
  | Logitech | `046D:C242` |
  | Saitek | `0738:CB02` |
  | Mad Catz | `0738:4716`, `0738:4718`, `0738:4726`, `0738:4728`, `0738:4736`, `0738:4738`, `0738:A728`; `1BAD:F028`, `1BAD:F029`, `1BAD:F039` |
  | HORI | `0F0D:000A`, `0F0D:000B`, `0F0D:000C`, `0F0D:000D`, `0F0D:000F`, `0F0D:0016`, `0F0D:0031`; `1BAD:F500`, `1BAD:F501`, `1BAD:F502`, `1BAD:F504`, `1BAD:F506` |

  The receiver `045E:0719`, which `x360wc` matches on 2000 to 7 (with
  `045E:028F`, the Play & Charge Kit, from XP on), is in the `w98se-me`
  INF only (section 7).
- If M0 finds a pad family that is composite (device class `0x00`), the INF
  gets `&MI_00` ids for it and the driver works on the interface PDO
  instead; on 98 SE the split is then the host stack's (Microsoft's
  `usbhub.sys` generic parent, NUSB's `usbccgp.sys`, or xHCI98's own), and
  this driver needs nothing beyond whichever stack is installed.

### 5.2 Xbox 360 wired (XUSB)

From `xpad.c` (`xpad360_process_packet`, `xpad_start_input`,
`xpad_send_led_command`, `xpad_play_effect`):

- **Start.** One vendor control request: `bmRequestType` `0xC1` (vendor,
  IN, interface), `bRequest` `0x01`, `wValue` `0x0100`, `wIndex` 0, length
  20, result ignored: "Some third-party controllers Xbox 360-style
  controllers require this message to finish initialization." Then a
  continuously re-armed interrupt IN transfer of 64 bytes (`XPAD_PKT_LEN`).
- **Input report** (`data[0] == 0x00` marks pad data):
  - `data[2]`: bit 0 up, 1 down, 2 left, 3 right, 4 Start, 5 Back, 6 left
    stick press, 7 right stick press.
  - `data[3]`: bit 0 LB, 1 RB, 2 Guide, 4 A, 5 B, 6 X, 7 Y.
  - `data[4]`, `data[5]`: left and right trigger, 0-255.
  - `data[6..7]`, `data[8..9]`: left stick X, Y, signed 16-bit little-endian;
    `data[10..13]` the right stick. Linux inverts Y (`~value`) so that up is
    negative; this driver does the same, matching HID's convention.
- **LED ring.** OUT `01 03 nn`. Linux lights the segment for pad number `n`
  with `nn = (n % 4) + 2` (`xpad_identify_controller`).
- **Rumble** (later, section 6.6). OUT `00 08 00 LL RR 00 00 00`, `LL`
  strong (left) and `RR` weak (right) motor, 0-255.

### 5.3 Xbox 360 wireless receiver

- One vendor interface per pad slot, protocol 129. Each slot has its own
  interrupt IN/OUT pair; the driver opens all slots of the receiver in one
  device instance and presents them as four top-level collections (section
  6.5), so four joysticks appear whether or not pads are switched on.
- **Presence.** `data[0] & 0x08` signals a connect/disconnect;
  `data[1] & 0x80` is pad present. Pad data is valid when `data[1] == 0x01`
  and is the wired format starting at `data[4]` (`xpad360w_process_packet`).
  A slot with no pad reports centred axes and no buttons.
- **On start**, per slot, the presence query OUT
  `08 00 0F C0 00 00 00 00 00 00 00 00` (`xpad_inquiry_pad_presence`).
- **LED** OUT `00 00 08 (0x40 + nn) 00 ...` (12 bytes). **Rumble**
  `00 01 0F C0 00 SS WW 00 00 00 00 00`. Linux also turns a pad off when
  Guide is held for a timeout (`xpad360w_poweroff_controller`); optional,
  M3.

### 5.4 Xbox One and Series pads (GIP)

From `xpad.c` (`xboxone_init_packets[]`, `xpad_start_xbox_one`,
`xpadone_process_packet`, `xpadone_ack_mode_report`):

- **Init sequence**, sent in order on interrupt OUT, each with the next
  sequence number in byte 2: power on `05 20 SS 01 00` (all pads with 2015
  or later firmware); for `045E:02EA` and `045E:0B00` also `05 20 SS 0F 06`;
  LED on `0A 20 SS 03 00 01 14`; authentication done `06 20 SS 02 01 00`;
  plus the per-VID/PID extras in the table (Hori/PDP ack-identify, PowerA
  rumble begin/end). Owner, 2026-10-22: the code adapts Linux master
  (`a804c5742bff224d4cb4d25dd383554d598be32b`), not v6.12: LED-on and
  authentication-done go to every One/Series pad (v6.12 sends them to PDP
  pads only). Before that, some pads need interface 1 (audio) set to
  alternate 0, which section 5.1's configuration already does.
- **Input** (`data[0] == 0x20`, `GIP_CMD_INPUT`): `data[4]` bit 2 Menu,
  3 View, 4 A, 5 B, 6 X, 7 Y; `data[5]` bits 0-3 D-pad up/down/left/right,
  4 LB, 5 RB, 6/7 stick presses; `data[6..9]` triggers, unsigned 16-bit
  little-endian, 0-1023; `data[10..17]` sticks as in XUSB.
- **Guide** arrives as `data[0] == 0x07` (`GIP_CMD_VIRTUAL_KEY`) with the
  state in `data[4]` bits 0-1; if `data[1] == 0x30` the pad expects an
  acknowledgement `01 20 SS 09 00 07 20 02 00 00 00 00 00` echoing its
  sequence number, or it repeats the report and stops sending Guide.
- `data[0] == 0x02` (`GIP_CMD_ANNOUNCE`) starts input on pads flagged for
  delayed init (Owner, 2026-10-22: only those, as Linux master does; one
  pad, `366C:0005`, has the flag). Elite paddles and the Share button are later work.

### 5.5 Transfers and host controllers

Only control and interrupt transfers; no isochronous, no bulk. The pads are
Full Speed. Everything is plain USBD (`URB_FUNCTION_SELECT_CONFIGURATION`,
`URB_FUNCTION_VENDOR_INTERFACE`, `URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER`,
`URB_FUNCTION_ABORT_PIPE`, reset pipe), so the driver depends on nothing
specific to xHCI98 and runs under Microsoft's UHCI/OHCI/EHCI stacks and under
NUSB on 98 SE.

Under xHCI98: interrupt transfers have been supported since its Phase 7
(xHCI98 `docs/contributing/build-and-test.md`, "Phase 7 (interrupt transfers
+ HID validation)"). Under its frozen 1.2.0.0 miniport a Full-Speed device on
a root port had its interval re-bucketed (`bInterval` 1-4 to 1 ms, 5 to
2 ms, 6 and above to 4 ms; xHCI98
`docs/contributing/design/12-virtual-hub-on-root-ports.md`, "The interrupt
interval"); faster polling than asked is harmless for a pad. The 2.x HCD is a
monolithic bus driver offering the standard USBD interface (xHCI98
`docs/contributing/architecture.md`, header note); no limit there is known
to affect a pad.

Interrupt IN is kept armed with two transfers in flight per endpoint so no
report is lost while one completes. OUT packets (LED, init, rumble) go
through a small queue, one in flight, as Linux does (`xpad_try_sending_next_out_packet`).

## 6. OS-facing side

### 6.1 Minidriver mechanics

- `DriverEntry` fills `HID_MINIDRIVER_REGISTRATION` (`DevicesArePolled =
  FALSE`) and calls `HidRegisterMinidriver`. `hidclass.sys` owns
  `AddDevice` and the FDO; the minidriver's state lives in
  `HID_DEVICE_EXTENSION.MiniDeviceExtension`, and `NextDeviceObject` is the
  USB PDO that URBs go to.
- PnP: on `IRP_MN_START_DEVICE`, forward first, then read descriptors,
  select the configuration, run the family's init (5.2-5.4) and arm the
  interrupt IN. On stop/remove, abort pipes and drain.
- `IOCTL_HID_GET_DEVICE_DESCRIPTOR` / `_GET_REPORT_DESCRIPTOR` return the
  synthesized descriptors (6.2); `_GET_DEVICE_ATTRIBUTES` returns the pad's
  real VID/PID/bcdDevice; `_GET_STRING` the product string (or the
  `xpad_device[]` name).
- `IOCTL_HID_READ_REPORT` IRPs are queued; each completed interrupt IN that
  changes the normalised pad state completes one with a fresh report. A
  read that finds an unchanged state waits. Reads are cancel-safe.
- `IOCTL_HID_WRITE_REPORT` returns `STATUS_NOT_SUPPORTED` in v1 (no
  rumble, 6.6); feature reports and
  `IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST` return
  `STATUS_NOT_SUPPORTED`.
- The 9x import rule applies: the binary imports only what 98 SE's
  `ntkern`, `hidclass.sys` and `usbd.sys` export, checked by script as in
  the UAC 2 design (section 8).

### 6.2 Report descriptor and report

One top-level collection per pad (four for the receiver, each with report
IDs 1-4; a single pad uses no report ID). The input report, little-endian,
15 bytes:

| Bytes | Field | HID usage | Range |
|---|---|---|---|
| 0-1 | axis 1 | X (`0x30`) | 0-65535 |
| 2-3 | axis 2 | Y (`0x31`) | 0-65535 |
| 4-5 | axis 3 | per layout (6.3) | 0-65535 |
| 6-7 | axis 4 | per layout | 0-65535 |
| 8-9 | axis 5 | per layout, or absent | 0-65535 |
| 10-11 | axis 6 | per layout, or absent | 0-65535 |
| 12 low nibble | D-pad | Hat Switch (`0x39`), logical 0-7, physical 0-315 degrees, unit English Rotation degrees, Null State | 8 directions, out of range = centred |
| 12 high nibble | padding | constant | 0 |
| 13-14 | buttons 1-13, 3 bits padding | Button page `0x09`, usages 1-13 | 0/1 |

Choices with a reason:

- **Axes 0-32767 in 16-bit fields** (measured 2026-10-23, replacing the
  first choice of 0-65535). 98 SE's `hidparse.sys` reads a 16-bit field as
  signed, so 0-65535 put every value from 32768 up out of range (HIDTEST:
  `HidP_GetScaledUsageValue` `C0110005`), and VJoyD maps a signed
  -32768..32767 range wrongly (centre 0). 0-32767 reads the same either
  way; WinMM still reports 0-65535. Signed stick values are re-biased by
  `+32768` and halved.
- **On 98 SE and ME the right stick is Rz and Slider**, not Rx/Ry (measured
  2026-10-23): `joyhid.vxd` refuses a collection with Rx or Ry ("not
  connected correctly"); Rz lands on WinMM R and Slider on U. Layout 2,
  where Rz carries RT, uses Slider and Dial (unverified). NT keeps Rx/Ry
  (WinMM: U/V on 2000, R/U on XP and Vista).
- **Axes absent from a layout are absent from the descriptor**, not
  reported as constant, so `joy.cpl` and DirectInput count exactly what is
  there.
- **Button order** A, B, X, Y, LB, RB, Back, Start, left stick, right stick,
  then Guide (11) and the optional trigger buttons (12, 13 if both).
  Owner, 2026-10-22: with `GuideButton` 0 the trigger buttons become 11
  and 12, so the numbering stays contiguous. A, B, X,
  Y first because the oldest API, `joyGetPos`, returns three axes and four
  buttons only (`JOYINFO` has `wXpos`, `wYpos`, `wZpos`, `wButtons`; 98 DDK
  `INC_WIN98_INC16_MMSYSTEM.H`).
- Top-level usage Game Pad (`0x05`) by default, Joystick (`0x04`) by
  registry option, until M1 shows which 98 SE maps (4.1).
- Owner, 2026-10-20: the default layout combines the triggers on Z
  (layout 0, 6.3); the other layouts and trigger buttons are registry
  options.

### 6.3 Axis layouts and the trigger question

The limits the layouts must fit (all from headers in the 98 DDK):

| API | Axes | Buttons | POV |
|---|---|---|---|
| WinMM `joyGetPos` (`JOYINFO`) | X, Y, Z | 4 | none |
| WinMM `joyGetPosEx` (`JOYINFOEX`) | X, Y, Z, R, U, V | 32 (`dwButtons`, `JOY_BUTTON32`) | 1 (`dwPOV`, hundredths of a degree) |
| VJoyD poll (`VJPOLLDATA`, `di_ddk_40xg`) | X, Y, Z, R, U, V | 4 doublewords | 4 |
| DirectInput `DIJOYSTATE` (`INPT_DDK\SRC_INPUT_INC_DINPUT.H`) | X, Y, Z, Rx, Ry, Rz + 2 sliders | 32 | 4 |
| DirectInput `DIJOYSTATE2` | same | 128 | 4 |

The DirectX DDK states the VJoyD model handles "up to six axes, a point of
view (POV) hat, and a doubleword of buttons" ("Joystick Support",
`di_ddk_6oc4`).

The trigger problem is Microsoft's own: triggers rest at minimum, not centre,
so "Older games would therefore assume user interaction" and Microsoft
combined them, "setting one trigger to a positive direction and the other to
a negative direction" (Microsoft Learn page cited in 4.4). Each answer hurts
some game, so the layout is a registry choice. Owner, 2026-10-20: the
default is layout 0, triggers combined on Z; layouts 1 and 2 and
`TriggerButtons` are registry options.

| `Layout` | Axis 3 | Axis 4 | Axis 5 | Axis 6 | Triggers | Suits |
|---|---|---|---|---|---|---|
| 0 (default) | Z = triggers combined, centred: `32768 + (LT - RT) * 128` (8-bit) or `* 32` (10-bit) | Rx = right X | Ry = right Y | absent | combined | games tested against Microsoft's driver on XP, and anything that calibrates centred axes |
| 1, "4-axis" | Z = right X | Rz = right Y | absent | absent | buttons 12, 13 (threshold) | 1990s games reading X, Y, Z, R as two sticks |
| 2, "separate" | Rx = right X | Ry = right Y | Z = LT | Rz = RT, both resting at 0 | separate | games that let the user bind each axis |

`TriggerButtons` (0/1, default 0 except in layout 1) adds LT and RT as
buttons 12 and 13 in any layout, pressed above `TriggerThreshold` (default
30 of 255, XInput's `XINPUT_GAMEPAD_TRIGGER_THRESHOLD`; the value is from
Microsoft's XInput documentation and **unverified** here). Which WinMM axis
letter (R, U, V) each HID usage lands on under `joyhid.vxd` is unverified
(4.1); M1/M2 measure it and the table above is corrected to match. The
sign convention of the combined axis (LT positive) is a choice; Microsoft's
page does not say which trigger is positive.

The descriptor is built once, when `hidclass.sys` asks for it, so a layout
change takes effect after the device restarts (unplug, or disable and enable).
A small `xpadcfg.exe` that writes the values and restarts the device is M4.

### 6.4 Registry values

Under the device's driver key (`HKR` in the 9x INF; `Device Parameters`
on NT): `Layout` (0-2), `TriggerButtons`, `TriggerThreshold`, `TopUsage`
(4 or 5), `GuideButton` (0/1, default 1), `Deadzone` (0-32767 radial,
default 0, Owner, 2026-10-20: off by default). All optional; absent means
default.

### 6.5 Multiple pads

Each wired pad is its own device and its own joystick. The receiver's four
slots are four top-level collections in one report descriptor; `hidclass.sys`
makes one collection PDO per top-level collection (the `&Col01`-style
children that composite keyboards with consumer controls already produce),
each matching `HID_DEVICE_SYSTEM_GAME`. That multi-collection devices work on
98 SE's `hidclass.sys` is expected from that precedent and is **verified in
M3**. WinMM's joystick count (16 ids, `JOYSTICKID1`..) and how `joy.cpl`
orders them are the system's. The player LED is assigned per driver instance
from a driver-wide bitmap (lowest free number, freed on remove), which is
what Linux does with an IDA (`xpad_led_probe`); the LED number need not match
the WinMM joystick id, and the release notes say so.

### 6.6 Guide button, LEDs, rumble, deadzones

- **Guide**: button 11, on by default (`GuideButton`). No system action is
  attached to it on these systems.
- **LEDs**: set on start (5.2, 5.3), never changed by applications in v1.
- **Rumble: not in v1** (Owner, 2026-10-20). DirectInput force feedback on 9x needs an OEM
  effect driver registered under `OEMForceFeedback` (DirectX DDK,
  "OEMForceFeedback Registry Settings", `di_ddk_01x0`), and HID PID
  (Physical Interface Device usage page) support in DirectInput on 98 SE is
  **unverified**; either is far more work than the pad's two motors earn
  for retro games, most of which predate force feedback. Owner,
  2026-10-20: rumble comes later as a plain HID output report only (a
  vendor-defined report carrying the two motor bytes, usable by a test
  tool); no DirectInput force-feedback effect driver is planned.
- **Deadzones**: off by default, left to applications (`Deadzone` 0; Owner,
  2026-10-20). Many 1990s games apply none and a worn 360 stick drifts, so
  the optional radial deadzone exists as a registry value.

## 7. Package, files and names

- Source: `src/hid/xpad98/` (8.3 names): `xpad98.c` (DriverEntry, PnP,
  HID IOCTLs), `usbio.c` (configuration, URBs, IN/OUT queues), `xusb.c`
  (360 wired and wireless), `gip.c` (Xbox One), `report.c` (normalised
  state, descriptor and report synthesis, layouts), `devtab.c` (VID/PID and
  quirk table), `xpad98.h`, `xpad98.rc`, `xpad98.inf`, `build.cmd`.
- Binary `xpad98.sys`, INF `xpad98.inf`.
- Released folders, one complete install each:

| Folder | Systems | Binary |
|---|---|---|
| `free/hid/xpad98/w98se-me/` | 98 SE, ME | x86 |
| `free/hid/xpad98/w2k/` | 2000 | x86 (same binary) |
| `free/hid/xpad98/wxp/` | XP 32-bit; Xbox One and Series pads only | x86 (same binary) |
| `free/hid/xpad98/wv-x86/` | Vista 32-bit; Xbox One and Series pads only | x86 (same binary) |
| `free/hid/xpad98/wxp-x64/` | XP x64; Xbox One and Series pads only | x64 |
| `free/hid/xpad98/wv-x64/` | Vista x64; Xbox One and Series pads only, once signing allows (section 4.5) | x64 (same x64 binary), catalog |

Folder names follow the one scheme of `README.md`, "Layout" (Owner,
2026-10-20). The receiver's ids are in the `w98se-me` INF only; the `w2k`
INF lists the 360-protocol pads outside `x360c`'s 25 IDs and the GIP ids,
not the receiver (`x360wc` serves it on 2000); the `wxp`, `wv-x86`,
`wxp-x64` and `wv-x64` INFs list only the GIP ids.

## 8. INF and install per OS

- **Class `HIDClass`** (`{745a17a0-74d3-11d0-b6fe-00a0c90f57da}`, as
  `hiddev.inf` and `input.inf`), so the pad appears under Human Interface
  Devices and its game collection under Sound, video and game controllers
  (9x, via `joystick.inf`) or Human Interface Devices (NT).
- **Ids**: `USB\VID_vvvv&PID_pppp` per supported pad (5.1).
- **9x section**: `AddReg` `HKR,,DevLoader,,*ntkern` and
  `HKR,,NTMPDriver,,xpad98.sys`; `CopyFiles` `xpad98.sys` to
  `10,system32\drivers`. `hidclass.sys` and `hidparse.sys` are not shipped
  (they are Microsoft binaries); the INF must make sure they are installed
  from the Windows media. How an OEM INF does that on 98 SE (copy them
  through `LayoutFile=layout.inf` as `hiddev.inf` does, or rely on their
  presence after any HID device install) is **unverified** and is M1's
  first install test, on a fresh 98 SE with no HID device ever installed.
- **NT section**: `Include=input.inf`, `Needs=HID_Inst.NT` to get the HID
  class files (the section in-box `input.inf` defines, 4.3), then
  `AddService = xpad98,0x00000002,...` (kernel driver, demand start). The
  `Include`/`Needs` directives are as the DDK's INF reference documents
  them; WDK samples are not read (Owner, 2026-10-20). That `Needs` of
  `input.inf`'s section works from an OEM INF on 2000 is **unverified**
  and is M4's first install test.
- **XP and Vista**: the same NT install (`Include=input.inf`,
  `Needs=HID_Inst.NT`, the service) in `.NTx86` and `.NTamd64` sections,
  whose models list only the Xbox One and Series (GIP) ids of section 5.1,
  so a 360-protocol pad is never matched there and stays with XBCD or
  whatever driver it has. No in-box driver claims a GIP pad on XP or Vista
  (section 4.4 and `existing.md` section 6.3), so ranking is not a
  question; that Vista's `input.inf` keeps the `HID_Inst.NT` section is
  read from Vista's media in M6, or left unverified in the Vista folders'
  release notes (section 11).
- No co-installer, no user-mode component in v1. `xpadcfg.exe` (M4) is a
  separate download in the same folder.

## 9. Build toolchain

- **x86 (one binary for 98 SE, ME and 2000):** the 98 DDK headers and
  `hidclass.lib` (`LIBS_DDK\LIB_I386_FREE_HIDCLASS.LIB`, installed on the
  xHCI98 host under `tools\win98ddk-extracted`) or the 2000 DDK's
  equivalents (`tools\ntddk`), with MSVC 6.0 (`tools\MSVC600`), as xHCI98
  builds its x86 drivers (xHCI98 `AGENTS.md`, toolchain table). Plain C.
  Which DDK's `hidclass.lib` to link is settled by the 9x import check:
  the binary must import from `HIDCLASS.SYS` only `HidRegisterMinidriver`.
- **amd64 (XP x64 and Vista x64, Xbox One and Series pads only):** WDK 7.1
  (`tools\WinDDK71`), as the UAC 2 design does, signed as xHCI98 and Stor98
  are.
- Kits are installed into this repository's git-ignored `tools/`, named by
  `build.cmd`; nothing is referenced from the xHCI98 tree at build time
  (this repository's `AGENTS.md`, "Standalone").

## 10. What is adapted, and the NOTICE.md rows it will need

| Planned file | Adapted from | Licence |
|---|---|---|
| `src/hid/xpad98/xusb.c` (360 report parsing, start request, LED, presence, wireless envelope, rumble format) | Linux `drivers/input/joystick/xpad.c`: `xpad360_process_packet`, `xpad360w_process_packet`, `xpad_start_input`, `xpad_inquiry_pad_presence`, `xpad_send_led_command`, `xpad_play_effect` | GPL-2.0-or-later |
| `src/hid/xpad98/gip.c` (GIP init packets and order, input parsing, Guide ack) | same file: `xboxone_*` packets, `xboxone_init_packets[]`, `xpadone_process_packet`, `xpadone_ack_mode_report`, `xpad_start_xbox_one` | GPL-2.0-or-later |
| `src/hid/xpad98/devtab.c` and the id list in `xpad98.inf` | same file: `xpad_device[]`, `xpad_table[]` | GPL-2.0-or-later |

Licences read in the files' SPDX lines on 2026-10-20 (GitHub
`torvalds/linux` master). `xpad.c` keeps its copyright block (Marko
Friedemann and others) in each file that takes code from it.

Not adapted, read only: ReactOS `drivers/hid/hidclass/` ("LICENSE: GPL - See
COPYING in the top level directory"), for how a HID class driver uses
minidriver IOCTLs and builds compatible ids; the in-box `hidclass.sys` is
what runs. The 98 DDK `hidmini` sample is not read (Owner, 2026-10-20);
the interface comes from the DDK documentation and `hidport.h`.

## 11. Test plan

**QEMU has no Xbox pad model.** Its USB device models are `usb-kbd`,
`usb-mouse`, `usb-tablet`, `usb-storage`, `usb-net`, `usb-audio`, `usb-hub`
and others listed in xHCI98 `docs/contributing/build-and-test.md`
("Peripheral (function) drivers"); none is a game controller. The VM path is
**`usb-host` passthrough** of a real pad, bound on the Windows host to WinUSB
with Zadig (`tools\zadig-2.9.exe` on the xHCI98 host) and attached by
`hostbus`/`hostaddr`, the method xHCI98's task 24.1 used to reach a real
Low-Speed mouse in a guest (same file). Whether libusb on the host can claim
a 360 receiver's or GIP pad's every interface is **unverified** (xHCI98 found
libusb claims only some interfaces of an audio composite); the wired 360 pad,
being one non-composite device, is the likeliest to pass through.

Real hardware (Owner, 2026-10-20: one or two mainstream devices per class;
no Linux gadget board):

- A 360-protocol pad outside `x360c`'s list, such as an 8BitDo pad in
  XInput mode, and the 360 Wireless Gaming Receiver `045E:0719` with a pad
  if at hand.
- An Xbox One or Series X|S pad (`045E:02EA`, `045E:0B12`); it is the pad
  for the XP leg too.
- A High Speed (USB 2.0) hub, for the EHCI legs (below).

Clones, 8BitDo pads and third-party GIP pads (PDP, PowerA) need init quirks
from Linux's table; they stay **unverified** until a user's pad reaches
them.

Tested systems (Owner, 2026-10-20; `README.md`, "Tested by the author"):
98 SE, 2000 SP4 and XP SP3 32-bit. ME, XP x64 and Vista x86 get their
release folders from the same source and stay "Built, not tested"; so
does Vista x64 once section 4.5's signing condition lets it be a target
at all. A reading or test leg only they could settle does not block a
release.

Hosts: 98 SE on a UHCI/OHCI machine with Microsoft's stack, on an EHCI
machine with NUSB's stack (fully supported, Owner, 2026-10-20), and on an
xHCI machine with xHCI98; 2000 SP4 in a VM with passthrough and on an EHCI
machine under Microsoft's stack. XP SP3 32-bit on a real machine under
Microsoft's stack and xHCI98, or in a VM with passthrough: the Xbox One
pad, plus a 360-protocol pad to confirm the INF leaves it alone. The
results go into `README.md`'s "Tested by the author" table.

**EHCI legs.** 98 SE (NUSB), 2000 SP4 and XP SP3 each get one. The pads
are Full Speed, so a pad on an EHCI root port, real or in QEMU, is handed
to the companion UHCI/OHCI controller and is not an EHCI leg. The EHCI
leg puts the pad behind the High Speed hub, so the hub's transaction
translator carries it over EHCI, and the record shows that. With no High
Speed model of the path in QEMU, these legs are real hardware (`build.md`
section 8).

**Left unverified on the systems not tested**, and listed in the release
notes of their folders: ME's `hiddev.inf` and `joystick.inf` plumbing, if
M0's reading of them is not done (4.2), and the receiver's and every
pad's behaviour on ME; that Vista's `input.inf` keeps `HID_Inst.NT`
(section 8) and the readings of M6 on Vista x86; the x64 build's load and
readings on XP x64 and Vista x64.

Readings per milestone: `joy.cpl` test page (every axis, button, POV);
a `joyGetPosEx` dump tool (X, Y, Z, R, U, V, buttons, POV, at 100 Hz to
catch dropped reports); DirectInput's `DIJOYSTATE2` from a DirectX 7 test
program on 98 SE; two or three retro games per layout; unplug and replug
while a game holds the joystick open; standby and resume; two pads at once;
four on the receiver.

## 12. Risks

- **`joyhid.vxd` behaviour is undocumented** (axis assignment, Game Pad vs
  Joystick usage, Null-state hats, multi-collection devices). Mitigation:
  M1 measures before anything is built on it, and every choice that depends
  on it is a registry value.
- **INF on 98 SE pulling `hidclass.sys` from the CD** may need the CD at
  install (section 8). Acceptable if documented.
- **Clone diversity.** Third-party 360 and GIP pads need init quirks that
  Linux has accumulated over years; the table is adapted, but each new pad
  may need an entry.
- **Overlap with `x360c`.** A pad on `x360c`'s list is left to it; a user
  who forces this driver onto one by "Have Disk" gets it, and the release
  notes say which driver each pad is for.
- **Power management on 9x**: HID devices on 98 SE do not suspend the way a
  WDM driver may expect; the driver handles `IRP_MN_SET_POWER` minimally
  (D0/D3, re-run init on D0), and standby/resume is a test item.

## 13. Owner decisions and open questions

Owner, 2026-10-20, on the questions this draft raised:

- **Q1. Test hardware**: one or two mainstream devices per class (section
  11).
- **Q2. HID-class pads** (DualShock 3, Switch Pro): not planned.
- **Q3. XP through 8**: the opt-in DirectInput mode is dropped; XBCD
  (GPL-2.0-or-later) serves 360-protocol pads on XP and later. Xbox One
  and Series pads are served on XP and Vista, x86 and, as signing allows,
  x64, because Microsoft has no Xbox One driver there; 7 and 8 have
  Microsoft's own and are not targets (section 4.5).
- **Q4. Default layout**: triggers combined on Z (layout 0); separate
  triggers and trigger buttons as registry options.
- **Q5. Default deadzone**: off.
- **Q6. Folder names**: the one scheme of `README.md`, "Layout" (section 7).
- **Q7. Rumble**: not in v1; later as a plain HID output report only, with
  no DirectInput force-feedback effect driver (section 6.6).
- **Q8. Original Xbox pads**: not planned.
- **Order**: 360-protocol wired pads outside `x360c`'s list, then the 360
  wireless receiver, then Xbox One and Series pads.
- **Scope after the survey of existing drivers** (`existing.md` section
  6): Morii's `x360c` and `x360wc` are carried in `usb-wdm-nonfree`, from
  Morii's own site with provenance recorded. This driver keeps Xbox One and
  Series pads on all its systems, the 360 receiver on 98 SE and ME, and
  360-protocol pads outside `x360c`'s ID list (section 1).
- **Morii's final builds**: `x360c` stays the 360-pad driver in the
  non-free collection, in Morii's final 2010 builds (25 pad IDs), with
  `x360wc` for the 360 receiver on 2000 to 7. This driver drops the
  receiver on 2000 and keeps it on 98 SE and ME; it keeps Xbox One and
  Series pads (98 SE to Vista) and 360-protocol pads outside `x360c`'s 25
  IDs (sections 1 and 5.1).
- **Back-ported EHCI stacks**: NUSB's EHCI stack on 98 SE is fully
  supported and in the test matrix.

Owner, 2026-10-22, on the first implementation's findings: adapt Linux
master, not v6.12 (section 5.4); trigger buttons 11 and 12 when the Guide
button is off (6.2); refuse a configuration with any interface other
than vendor class `0xFF` (5.1).

## 14. Milestones (smallest useful first)

- **M0. Facts.** Descriptors of each held pad (`lsusb-desc.py`): device
  class, interfaces, endpoints, `bInterval`. ME's `hiddev.inf` and
  `joystick.inf`. `xusb*.inf` presence in Vista, 7 (unmodified) and 8
  images. Output: corrections to sections 4 and 5. The ME and Vista
  readings do not block a release (section 11).
- **M1. Wired 360-protocol pad (outside `x360c`'s list) on 98 SE in the
  default layout.** Layout 0: X/Y left
  stick, Z both triggers combined, Rx/Ry right stick, D-pad as hat, A B X Y
  LB RB Back Start LS RS. `xpad98.sys` with the minidriver
  core, `usbio.c`, `xusb.c` (wired, start request, LED 1), the 9x INF, the
  `joyGetPosEx` dump tool. Measures the open 98 SE questions: Game Pad vs
  Joystick usage, which usage lands on which WinMM axis, Null-state hat,
  install of `hidclass.sys` on a fresh system. Tested under Microsoft's
  UHCI/OHCI stack, NUSB's EHCI stack (behind a High Speed hub, section
  11) and xHCI98.
- **M2. Layouts and triggers.** Layouts 0-2, `TriggerButtons`, Guide
  button, `TopUsage`; 6-axis reports; DirectInput test on 98 SE. ME gets
  the same build, not tested.
- **M3. Multiple pads and the wireless receiver.** Player LEDs from the
  driver-wide bitmap, two wired pads at once, the receiver as four
  collections with presence handling (tested on 98 SE; ME is built, not
  tested); optional Guide-hold power-off.
- **M4. 2000.** NT INF, service install, DirectInput and WinMM readings on
  2000 SP4, including its EHCI leg; `xpadcfg.exe`.
- **M5. Xbox One and Series pads (GIP)** on 98 SE, ME and 2000, tested on
  98 SE and 2000: init sequence and per-pad extras, Guide ack, 10-bit
  triggers.
- **M6. Xbox One and Series pads on XP and Vista.** The GIP-only INFs
  (section 8), DirectInput and WinMM readings on XP SP3 32-bit, including
  its EHCI leg. The Vista x86 folder, the x64 build for XP x64, and for
  Vista x64 once signing allows, are built and released "Built, not
  tested" (section 11).

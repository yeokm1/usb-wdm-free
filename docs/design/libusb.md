# libusb-win32 on Windows 98 SE, ME and 2000: findings and recommendation

Status: design draft, 2026-10-20. Build work for M1 (commits 1 to 5 and 7
of section 6.6) done 2026-10-22, host only, no guest run:
[testing/2026-10-22-libusb0.md](../testing/2026-10-22-libusb0.md).

This note answers the question `candidates.md` (candidate 4) left open:
does libusb-win32's GPL kernel driver still run on 98 SE and ME, and if
not, what would it cost to make it run. It is a findings note followed by
a recommendation, then a package outline for the recommended option.

## 1. What libusb-win32 is

libusb-win32 is a Windows port of the libusb-0.1 API. It has two parts:

- **`libusb0.sys`**, a WDM kernel driver. It binds to a USB device (or
  interface) by an INF that names its hardware id. It creates
  `\Device\libusb0-NNNN` with the symbolic link `\DosDevices\libusb0-NNNN`
  and turns `DeviceIoControl` requests from user mode into URBs. Early
  versions could also be installed as a class filter driver over other
  drivers' devices.
- **`libusb0.dll`**, the user-mode library. It exports the libusb-0.1 API
  (`usb_init`, `usb_find_devices`, `usb_open`, `usb_control_msg`,
  `usb_bulk_read` and so on). It finds devices by opening
  `\\.\libusb0-0001` to `\\.\libusb0-0255`, and talks to the driver with
  the `LIBUSB_IOCTL_*` codes in `src/driver/driver_api.h`.

A program written for libusb-0.1 on Linux builds against `usb.h` (later
`lusb0_usb.h`) and links `libusb0.dll`. For it to work, the device must
be bound to `libusb0.sys`.

Upstream today is `github.com/mcuee/libusb-win32`. Its git history is
the imported SourceForge SVN history from 2003-09-26 on. SourceForge
(`sourceforge.net/projects/libusb-win32`) keeps the old release files;
since 2024-02-23 its readme points to GitHub for new releases. Commit
hashes below are from the GitHub repository.

## 2. Findings

### 2.1 Releases and the systems each one claims

The SourceForge folder `libusb-win32-releases/` lists 17 releases:
0.1.8.0 (2004-02-15), 0.1.10.0 (2005-02-14), 0.1.10.1 (2005-03-09),
0.1.12.0 (2007-02-25), 0.1.12.1 (2007-03-20), 0.1.12.2 (2009-07-07),
1.1.14.0, 1.1.14.3, 1.2.0.0 to 1.2.6.0 (2010-07-08 to 2012-01-17),
1.2.7.3 (2023-01-10) and 1.4.0.2 (2026-05-18). GitHub adds snapshots
1.2.7.2 to 1.4.0.1 and release 1.4.0.0.

The claimed systems come from `README.in` (it becomes the binary
package's `README.txt`):

| Period | `README.in` says | Evidence |
|---|---|---|
| 2003-09 to 2005-01 (0.1.7.x, 0.1.8.0) | Win98, WinME, Win2k, WinXP | `README.in` at `241d8b6`; 9x code in `src/service/service_9x.c` |
| 2005-01-08 to 2005-02-14 | Win2k, WinXP | `5b37fa9` "removed services, updated to a new installer, dropped support for Win98" |
| 2005-02-14 to 2010-05-20 (0.1.10.0 to 0.1.12.2) | Win98SE, WinME, Win2k, WinXP | `675cb51`; the released `libusb-win32-device-bin-0.1.12.2/README.txt` says the same |
| 2010-05-20 on (1.1.14 and later) | Win2k, WinXP, Vista, Win7 | `578c7e1` "Updated readme.in to reflect supported operating systems" |
| 2019-09-18 on (1.2.7.x and later) | 2000 builds removed | `7c90762` "Remove deprecated win versions (w2k + IA64)" |
| 1.4.0.2 | Win7, Win8, Win10, Win11 | `README.txt` in `libusb-win32-bin-1.4.0.2.zip` |

The installer's minimum system moved the same way: `7d04e29`
(2010-05-04) changed the Inno Setup `MinVersion` from "Win98SE, Win2k"
(`4.1.2222, 5`) to "Win2k" (`0, 5.0.2195`).

**So the last release that claims 98 SE and ME is 0.1.12.2 (2009-07-07).**
The last release whose driver can load on 2000 is 1.2.6.0 (2012-01-17).
Section 2.3 shows the import tables behind both statements.

### 2.2 The 9x-specific code, and what happened to it

There were three kinds.

1. **A user-mode service, 2003 to 2005.** `src/service/service_9x.c` and
   `service_nt.c` existed up to `5b37fa9` (2005-01-08). That commit
   deleted them and, for five weeks, dropped 98 support. Nothing of that
   design survives.
2. **9x branches in the DLL and installer, 2005 to today.** In 0.1.12.2,
   `src/registry.c` chooses its path with `usb_registry_is_nt()`, which is
   `GetVersion() < 0x80000000`. On 9x it:
   - uses the `SYSTEM\CurrentControlSet\Services\Class\` class keys
     (`CLASS_KEY_PATH_9X`) and the 9x class names (`usb`, `hid`, `net`,
     `image`, `media`, `modem`);
   - writes `LowerFilters`/`UpperFilters` as comma-separated `REG_SZ`
     instead of `REG_MULTI_SZ`;
   - reads device properties through `SetupDiOpenDevRegKey` because
     `SetupDiGetDeviceRegistryProperty` does not serve them on 9x.

   `3a902c7` (2006-07-17) "reversed the link order of cfgmgr32.dll and
   setupapi.dll (win98 support)". This code was **never removed**:
   `registry.c` and `install.c` at today's `HEAD` still carry
   `usb_registry_is_nt()` and the `_9X` paths, now dead.
3. **The INF's 9x install section, 2003 to 2010.** The INF template in
   0.1.12.2's `src/inf_wizard.c` has a plain `[LIBUSB_DEV]` section for 9x
   with `HKR,,DevLoader,,*ntkern` and `HKR,,NTMPDriver,,libusb0.sys`,
   beside the `.NT` and `.NTAMD64` sections. This was removed in 2010:
   `936791a` "Removed dead code in inf-wizard.c" and `3b714b1`
   (1.2.2.0), when INF generation moved to libwdi. INFs that libwdi or
   Zadig generate today have no 9x section.

The 0.1.12.2 **driver** has no 9x branch at all. It is written to the
subset of WDM that 98 has. For example, it carries its own remove lock
(`libusb_remove_lock_t` in `libusb_driver.h`) where a 2000-only driver
would call `IoAcquireRemoveLock`, which the 98 DDK's
`INC_WIN98_WDM.H` does not declare. The 1.x drivers lost this by
starting to call 2000-and-later kernel routines (section 2.3); nothing
was deliberately "removed".

### 2.3 Import tables: what the binaries need from the kernel

The release binaries were parsed as files, never run. Their import names
were compared with three sets:

- the 98 SE DDK's import library `LIBS_DDK\LIB_I386_FREE_WDM.LIB`;
- the names in 98 SE's `ntkern.vxd` (as xHCI98's
  `docs/contributing/lessons.md` did for path roots);
- the Windows 2000 DDK's `libfre\i386\ntoskrnl.lib`.

| Binary | Imports absent on 98 SE | Imports absent on 2000 | Built, PE subsystem |
|---|---|---|---|
| 0.1.12.2 `libusb0.sys` (MinGW build) | none: all 34 `ntoskrnl.exe` imports and `usbd.sys!_USBD_CreateConfigurationRequestEx@8` are in `WDM.LIB` and in `ntkern.vxd`'s names; 98 SE's `usbd.sys` exports the latter | none | 2009-07-07, 4.0 |
| 0.1.12.2 `libusb0.dll` | none known (kernel32, advapi32, setupapi, cfgmgr32, msvcrt, user32; the 98 SE DLL export tables were not checked) | none | 2009-07-07, 4.0 |
| 1.2.6.0 `libusb0.sys` (WDK build) | `ExFreePoolWithTag`, `ObQueryNameString`, `MmMapLockedPagesSpecifyCache` (in neither set); `KeTickCount` (in `WDM.LIB`, not found in `ntkern.vxd`) | none | 2012-01-17, 5.0 |
| 1.2.6.0 `libusb0_x86.dll` | `advapi32!CheckTokenMembership` | none | 2012-01-17, 5.0 |
| 1.2.7.3 `libusb0.sys` | as 1.2.6.0, plus `RtlGetVersion` | `RtlGetVersion` (not in the 2000 DDK's `ntoskrnl.lib`) | 2021-11-13, 5.1 |
| 1.4.0.2 `libusb0.sys` | as above, plus `DbgPrintEx`, `IoSetCompletionRoutineEx` and others | `RtlGetVersion`, `DbgPrintEx`, `IoSetCompletionRoutineEx` | 2026-01-09, 6.1 |

Conclusions:

- 0.1.12.2's driver imports nothing that 98 SE lacks. This matches its
  README's claim. It is not proof that it works: no test has been run
  here (section 6).
- No 1.x driver loads on 98: Windows refuses a driver with an unresolved
  import. No 1.x DLL loads on 98 either, because it statically imports
  `CheckTokenMembership`, which Microsoft documents as Windows 2000 and
  later.
- 1.2.6.0 is the last driver whose imports 2000 satisfies; from 1.2.7.x
  on, `RtlGetVersion` is imported. This agrees with `7c90762` (2019).
- ME's `ntkern.vxd` was not available here. ME is unverified at import
  level, and stays so unless its `ntkern.vxd` is read or the optional ME
  run of section 6.7 is made; ME is not a tested system.

One gap in the method: an absent name in `ntkern.vxd` is weak evidence.
`KeQuerySystemTime` is declared in the 98 DDK's `wdm.h` and is in
`WDM.LIB`, but also does not appear as a string in `ntkern.vxd`. The
`WDM.LIB` column is the stronger test.

### 2.4 Licences

From the file headers of 0.1.12.2 (which match today's `HEAD` in kind)
and the package's `installer_license.txt` ("Driver, Installer: GNU
General Public License (GPL); Library, Test Files: GNU Lesser General
Public License (LGPL)"):

| Part | Files | Licence as stated |
|---|---|---|
| Driver `libusb0.sys` | `src/driver/*.c`, `*.h` (Stephan Meyer, 2002-2005) | GPL, "version 2 of the License, or (at your option) any later version", i.e. GPL-2.0-or-later |
| DLL, Windows parts | `src/windows.c`, `registry.c`, `install.c`, `install_filter.c`, `libusb_dyn.c`, `registry.h` | "GNU Lesser General Public License ... version 2 of the License, or (at your option) any later version" (there is no "Lesser" GPL version 2; the shipped `COPYING_LGPL.txt` is LGPL 2.1); treat as LGPL-2.1-or-later |
| DLL, libusb-0.1 core | `src/usb.c`, `descriptors.c`, `error.c` (Johannes Erdfelt, 2000-2003), and `usb.h`, `usbi.h`, `error.h` (no header) | "This library is covered by the LGPL, read LICENSE for details", no version given |
| INF wizard | `src/inf_wizard.c` | GPL-2.0-or-later |
| Test programs | `tests/testlibusb.c` (no header), `testlibusb_win.c` | LGPL per `installer_license.txt` |
| GCC-only header | `src/driver/usbdlib_gcc.h` | no header; a MinGW-era copy of DDK declarations. Not needed with a DDK build, and not to be copied |

The SourceForge project page's metadata says "GPLv3, LGPLv3". That
disagrees with every file header and is not a licence grant; the headers
govern.

All of this is compatible with this repository. The driver is
GPL-2.0-or-later and is taken in as such. Owner, 2026-10-20: the DLL
stays LGPL-2.1-or-later. Its callers include closed-source programs that
load `libusb0.dll`, and relicensing it to GPL (which LGPL 2.1 section 3
permits) would make shipping it with them questionable. The core files
whose header names no LGPL version are recorded as LGPL-2.1-or-later, the
version shipped in the same package's `COPYING_LGPL.txt` (Owner,
2026-10-20).

### 2.5 The interface between DLL and driver

- **DLL exports.** 0.1.12.2's `libusb0.def` and today's differ by four
  added names: `usb_install_npA`, `usb_install_npW`,
  `usb_install_np_rundll` and `usb_reset_ex`. A program linked against
  the 1.2.x import library that calls none of these loads with a
  0.1.12.2 DLL. One that calls `usb_reset_ex` does not.
- **IOCTL request layout.** `libusb_request` is 24 bytes in both 0.1.12.2
  and 1.2.6.0 `driver_api.h`. Both DLLs require
  `ret >= sizeof(libusb_request)` from `LIBUSB_IOCTL_GET_VERSION` before
  they accept a device (`windows.c`). Both drivers return
  `sizeof(libusb_request)`. So the handshake passes across versions.
  The 1.x IOCTL codes (`0x8FF`, `0x900` to `0x911`, `0x817`) are new
  codes; the old ones kept their numbers.
- **Transfer size.** The 0.1.12.2 driver refuses requests larger than
  `LIBUSB_MAX_READ_WRITE` (64 KB, `driver_api.h`). The DLL splits larger
  bulk transfers into 64 KB pieces (`windows.c`, `_usb_transfer_sync`).

### 2.6 What the driver needs from the USB stack

The 0.1.12.2 driver sends:

- `IOCTL_INTERNAL_USB_SUBMIT_URB` with select configuration/interface,
  control (standard, class, vendor; device, interface, endpoint, other),
  bulk/interrupt, isochronous (`USBD_START_ISO_TRANSFER_ASAP`), abort
  pipe and reset pipe;
- `IOCTL_INTERNAL_USB_RESET_PORT` and `IOCTL_INTERNAL_USB_CYCLE_PORT`
  for `usb_reset`.

All are declared in the 98 DDK's `INC_WIN98_USBDI.H` and
`INC_WIN98_USBIOCTL.H`. It uses no USBD bus interface and no
`IOCTL_INTERNAL_USB_GET_*` queries.

- **Under xHCI98:** the URB set is served
  (`xhci98/docs/contributing/roadmap-hcd.md`, item 26-A.5). `RESET_PORT`
  and `CYCLE_PORT` are served since 2026-10-03 (item 26-A.6,
  `runs/run-26.md`). Isochronous works, and its ring is to be enlarged
  (Owner, 2026-10-20: planned in xHCI98); `libusb0.sys` queues whatever
  the caller asks for, so that is not a design limit here.
- **Under Microsoft's 98 SE/ME/2000 stacks and NUSB's EHCI stack:** these
  are the stacks it was written for in 2005-2009. Whether 98 SE's
  `usbhub.sys` honours `CYCLE_PORT` is unverified.
- **User-mode access on 98.** The DLL opens `\\.\libusb0-NNNN`. On 98 SE
  this resolves through NTKERN to the `\DosDevices\` link. xHCI98 relies
  on the same resolution for its `\\.\HCD<n>` names
  (`xhci98/docs/contributing/design/25-usb-tree-tool.md`, "Not verified"
  list, citing its record 13 section 8.2). For ME it is unverified.

### 2.7 Defects in 0.1.12.2 relevant to a build here

Read from the source; none has been reproduced:

1. **`call_usbd()` race** (`libusb_driver.c`). On a timeout it cancels
   the IRP, then waits on an event that the completion routine signals.
   It completes the IRP built by `IoBuildDeviceIoControlRequest` with
   that IRP's `io_status` on the stack. Upstream fixed exactly this code
   in `f41b963` (2021) "fix race in call_usbd_ex() that can lead to stack
   corruption", then reworked it in `52898f0` (2023). The fix uses only
   `KeClearEvent`, `KeWaitForSingleObject` and `IoCancelIrp`, which are
   all in the 98 `WDM.LIB`.
2. **INF on 98 puts the DLL in the wrong folder.** The template's
   `libusb_files_dll = 10,system32` is used by the 9x section too. On 98,
   DIRID 10 is `C:\WINDOWS`, so the DLL lands in `C:\WINDOWS\SYSTEM32`.
   The 9x loader does not search that folder: its documented order is the
   application folder, the current folder, `SYSTEM`, the Windows folder,
   then `PATH`. Programs that ship their own `libusb0.dll` beside the
   `.exe` hide the problem. An INF here installs the DLL to DIRID 11 on
   9x. The loader behaviour on 98 is unverified until tested.
3. **Filter mode.** `install-filter.exe` and the class-key code in
   `registry.c` attach `libusb0.sys` as an upper or lower filter to whole
   device classes. Upstream later fixed blue screens in that mode:
   - `a44b390` (2010): power policy ownership as a filter;
   - 1.2.2.0: filter over auto-suspended devices.

   It also conflicts with this repository's rule that a device is never
   bound generically (Owner, 2026-10-20). A build here drops filter mode
   entirely.
4. Upstream fixes from 2010 on mostly touch code that 0.1.12.2 does not
   have: transfer splitting (`5beec48` in 2024 removed it as
   "fundamentally broken"), the libusbK IOCTLs, and divide-by-zero in the
   splitting arithmetic (`96044f2`). The 0.1.12.2 `create_urb()` already
   refuses `packet_size <= 0` for isochronous. Two others need a read
   before porting:
   - the `usb_find_devices` descriptor-failure fix (1.2.0.0 changelog,
     bug 2928293);
   - the 8-byte `usb_get_descriptor` buffer fix (bug 2128187).

## 3. Programs that would use it on 98 SE

The libusb-0.1 API is what hobbyist device tools of 2004-2012 used on
Windows. What matters for 98 SE is which builds of them still run there.

| Program | Uses | Current version on 98 |
|---|---|---|
| **avrdude** (AVR programmers) | libusb-0.1 for `usbtiny.c` and `usb_libusb.c` (AVRISP mkII 03EB:2104, JTAGICE mkII 03EB:2103, AVR Dragon 03EB:2107); `usbasp.c` (16C0:05DC) takes either API. Today's `CMakeLists.txt` still has `USE_LIBUSBWIN32` and searches for `libusb0.a`/`usb0` and `lusb0_usb.h` | Unverified. Current releases are built with current MSVC or MinGW-w64, whose runtimes target XP or later. A 98 build would need a 98-capable toolchain and a check of the Win32 calls it makes (`ser_win32.c`, `hid` and `setupapi` use) |
| **avrdude 5.x from WinAVR 20100110** | Linked against libusb-win32 per the WinAVR manual | Unverified. The WinAVR project listing names "95/98/NT/2000/XP/Vista/7" in general terms only |
| **V-USB devices** (obdev's shared 16C0:05DC and similar: USBasp, many DIY gadgets) and their host tools | libusb-0.1, by obdev's design | Per tool; old builds are likely, unverified |
| **micronucleus** (Digispark bootloader) | libusb-0.1 on Windows | Unverified |
| **FX2 tools** (`cycfx2prog`, `fxload` ports), **libftdi 0.x** programs | libusb-0.1 | Unverified; FTDI parts are better served by FTDI's own 98 drivers (non-free pack, `serial.md`) |
| **SDR tools** (rtl-sdr, HackRF, Airspy), OpenOCD, dfu-util, sigrok, fwupd | **libusb-1.0**, not 0.1 | Not served by `libusb0.dll` at all; see section 4 |

The audience is technical and small but real: AVR programmers and V-USB
gadgets are the common case. Most of them have no 98 driver of their own
other than libusb-win32. Running today's avrdude on 98 is a separate
porting question that this package does not answer. What the package
gives is the driver and DLL that 98-era builds of these tools expect.

## 4. Does a libusb-1.0 backend for 98 matter?

Not now.

- **libusb-1.0 refuses old systems.** Its Windows backend
  (`libusb/os/windows_common.c` on `master`) fails `libusb_init` with
  "Windows version is too old" below Vista. Its `ChangeLog` records
  "Windows: Drop support for Windows XP" in v1.0.24 (2020-12-09). It is
  built on I/O completion ports (`CreateIoCompletionPort`) and
  `CancelIoEx`; Microsoft documents `CancelIoEx` as Vista and later.
- **It reaches `libusb0.sys` only through `libusbK.dll`.** It does not
  open `\\.\libusb0-NNNN` itself
  (`libusb/os/windows_winusb.c`: `load_system_library(ctx, "libusbK")`,
  "libusb0/libusbK through libusbk DLL"). That support arrived in
  v1.0.13 (2012). Whether any libusbK release loads on 98 is unverified
  and unlikely.
- **A 98 backend means new code** in libusb-1.0 (LGPL-2.1-or-later): an
  event loop without completion ports, over the `libusb0.sys` IOCTLs.
  Its users would be the tools in the last row of section 3. Their
  current builds would not run on 98 anyway, and an SDR stream at
  2.4 MS/s is beyond the machines 98 SE is usually run on. That is weeks
  of work for few users.
- A lighter variant, if asked for later: start from libusb 1.0.23 (the
  last with an XP-capable backend) and add a `libusb0.sys` backend
  using overlapped I/O and events. This is recorded as a possible later
  candidate, not planned.

## 5. Options

**(a) Package an old release as-is** (0.1.12.2 `libusb0.sys` and
`libusb0.dll` from SourceForge). Cheapest. Against it:

- `AGENTS.md` has `free/` hold files built from this repository's `src/`
  ("A release copies the built files"). A 2009 MinGW binary is not that.
- The 0.1.12.2 binaries carry the `call_usbd()` race and the INF's 98
  DLL-folder problem (section 2.7).
- Nothing could be fixed without rebuilding anyway.

Useful only as M0's test subject (section 6.7), not as the package.

**(b) Build the last 9x-capable source here, fixed.** Take the 0.1.12.2
driver and DLL source into `src/`, build them with the toolchain the
other 98 packages use, and make a small set of changes:

- the `call_usbd()` fix;
- drop filter mode;
- a 98-correct INF;
- the four missing DLL exports;
- the two DLL fixes once read.

The import table is already 98-clean (section 2.3). The driver is 3,515
lines (`src/driver/*.c`, `*.h`, without the GCC header).

A variant of (b) starts from 1.2.6.0 or `HEAD` instead and shims the
missing imports. 1.2.6.0's driver is 6,847 lines. It brings the
transfer-splitting code upstream later removed as broken, the libusbK
IOCTLs, `ObQueryNameString` for `GET_OBJECT_NAME` (no 98 equivalent),
and `IoRegisterDeviceInterface` paths not needed by any 0.1 DLL. `HEAD`
adds XP-and-later calls (`IoSetCompletionRoutineEx`, `RtlGetVersion`,
`MmGetSystemRoutineAddress`, `DbgPrintEx`). That is about twice the code
to port, with more ways to fail on 98. Rejected.

**(c) Write our own.** It would have to reproduce the `libusb0.sys`
IOCTL interface and device naming byte for byte, or no existing program
could use it. That is (b) with more work and less history. Rejected.

**(d) Not worth it.** Against: the cost of (b) is small (below), the
code is GPL-compatible, and the users (AVR programmers, V-USB gadgets)
have no other 98 driver.

## 6. Decision: (b), from 0.1.12.2, for 98 SE and ME

Owner, 2026-10-20: libusb-win32 0.1.12.2's driver and DLL are rebuilt
from source here, with the four fixes of section 2.7, and shipped for
**98 SE and ME**.

For **2000 and later**, users are pointed to upstream (Owner,
2026-10-20):

- 1.2.6.0 is the last release built for 2000 and serves 2000 through 7,
  x86 and x64, with signed x64 drivers.
- 1.4.0.2 serves 7 and later.

Upstream's builds are newer, maintained and signed, and the gap this
package fills is the one Windows and upstream leave: 98 SE and ME. No
`free/` folder for 2000 is planned.

**Effort**, an estimate:

- importing the source, the build, the import-check script and the INF:
  2 to 3 days;
- the fixes in section 2.7: 2 to 3 days;
- testing per section 6.7: about a week, mostly on real devices.

About two to three weeks in all.

### 6.1 Names

- **Package:** `libusb0`, in `src/generic/libusb0/` (Owner, 2026-10-20:
  class folder `generic`).
- **Binary names stay upstream's**, because programs depend on them:
  - programs load `libusb0.dll` by name;
  - the DLL opens `\\.\libusb0-NNNN`;
  - the INF's `NTMPDriver` names `libusb0.sys`.

  These are already 8.3. The class stays upstream's `LibUsbDevices`,
  `{EB781AAF-9C70-4523-A5DF-642A87ECA567}`, so a device installed by an
  upstream INF and one installed by ours share a class.
- **Version:** `0.1.13.N`. That is above every 0.1.x upstream release and
  below every 1.x, so on a system that later gets an upstream 1.x
  package, the newer upstream driver ranks higher. The tag is
  `libusb0-0.1.13.N`.

### 6.2 `free/` folders

`free/generic/libusb0/w98se-me/`:

| File | What |
|---|---|
| `LIBUSB0.SYS` | The driver |
| `LIBUSB0.DLL` | The library |
| `LIBUSB0.INF` | A curated device list (below), 9x section only |
| `TESTLIBU.EXE` | Upstream's `testlibusb` (lists devices and descriptors), console |
| `INFWIZ.EXE` | Upstream's INF wizard, writing 9x-correct INFs (M2) |
| `README.TXT` | How to add a device by hand, which programs to pair it with, the licences |
| `USB.H` | The libusb-0.1 header, for building a 98 tool against this DLL |
| `LIBUSB0.LIB` | Import library for MSVC |
| `LIBUSB0.A` | Import library for MinGW |

Owner, 2026-10-20: the developer files (`USB.H` and the import
libraries) go under `free/` as well as `src/`.

The INF matches hardware ids of listed devices by VID/PID only, plus
`&MI_xx` forms where Windows has split a composite device; it never binds
a device generically (Owner, 2026-10-20). Its first list, from avrdude's
`avrdude.conf.in` and `usbdevs.h`:

- 16C0:05DC (USBasp and other V-USB devices on obdev's shared libusb
  PID);
- 1781:0C9F (USBtinyISP);
- 03EB:2104 (AVRISP mkII);
- 03EB:2103 (JTAGICE mkII);
- 03EB:2107 (AVR Dragon).

Any other device gets its own INF from `INFWIZ.EXE` or a hand-edited
copy (Have Disk). This is the same manual route `usbmidi.md` documents
for unlisted MIDI devices.

### 6.3 INF and install on 98 SE and ME

- `[Version] Signature="$Chicago$"`, the class above, `DriverVer`
  `0.1.13.N`.
- A 9x install section with:
  - `CopyFiles` of `LIBUSB0.SYS` to DIRID 10 `SYSTEM32\DRIVERS` (where
    98 loads `NTMPDriver` from, as 0.1.12.2 did);
  - `LIBUSB0.DLL` to **DIRID 11** (`SYSTEM`), fixing section 2.7 item 2;
  - `AddReg` `DevLoader=*ntkern`, `NTMPDriver=libusb0.sys`.
- No `LowerFilters`/`UpperFilters`, and no class-wide binding.
- Install is the usual route: Add New Hardware Wizard, or Update Driver
  on the device, pointed at the folder.

### 6.4 Build toolchain

- **`LIBUSB0.SYS`:** the Windows 2000 DDK (`tools\ntddk`) with MSVC 6.0
  (`tools\MSVC600`), as the other 98 packages and xHCI98 do. The
  upstream `ddk_make/sources_drv` gives the file list. A script checks
  the import table against the 98 SE set, as `serial.md` section 9
  plans. Today the list is the 35 names in section 2.3.
- **`LIBUSB0.DLL`, `TESTLIBU.EXE`, `INFWIZ.EXE`:** MSVC 6.0 against its
  own headers and `msvcrt.dll`, which 98 ships. The code is C89 and was
  built with the DDK compiler upstream (`ddk_make/sources_dll`).
  `libusb0.def` gains the four 1.x exports:
  - `usb_reset_ex` maps to the existing reset;
  - the `usb_install_np*` entry points return an error on 9x.
- The upstream MinGW `Makefile` and `usbdlib_gcc.h` are not taken.
- **`LIBUSB0.A`:** generated from `libusb0.def` by GNU `dlltool`
  (binutils for i686, installed in the git-ignored `tools/binutils/`)
  in the `win32` build (Owner, 2026-10-22).
- **DLL version resource:** a new LGPL-2.1-or-later `src/dllver.rc`
  with upstream's values, because upstream's `resource.rc` includes the
  GPL-headed `common.rc` (Owner, 2026-10-22: approved).
- Developer files (`USB.H` and the MSVC and MinGW import libraries) live
  in `src/generic/libusb0/` and are released with the package (6.2), for
  anyone building a 98 tool.

### 6.5 What is taken from upstream, and the `NOTICE.md` rows

The source is libusb-win32 0.1.12.2: the SourceForge tarball
`libusb-win32-src-0.1.12.2.tar.gz`, identical to git commit `00eb396`
of `github.com/mcuee/libusb-win32` (compared file by file). Every file
keeps its header, and gets an SPDX tag added beside it (not replacing
it):

- `GPL-2.0-or-later` for the driver and the INF wizard;
- `LGPL-2.1-or-later` for the DLL's Windows files;
- `LGPL-2.1-or-later` for the Erdfelt core files, with a note that their
  header names no version (Owner, 2026-10-20: accepted).

Upstream's own file names and layout are kept under
`src/generic/libusb0/` (Owner, 2026-10-20), so a file there is at the
same relative path as in the tarball and diffs against upstream stay
plain. The names under `src/` need not be 8.3; only `free/` must be.

Planned rows, one per file (each wildcard below expands to one row per
file), origin "libusb-win32 0.1.12.2 (`00eb396`)":

| File (planned, under `src/generic/libusb0/`, upstream's paths) | Licence |
|---|---|
| `src/driver/*.c` (Stephan Meyer's driver sources) | GPL-2.0-or-later |
| `src/driver/*.h`, except the GCC-only `usbdlib_gcc.h` | GPL-2.0-or-later |
| `src/windows.c`, `registry.c`, `install.c`, `registry.h` | LGPL-2.1-or-later (header: "Lesser GPL version 2 or later") |
| `src/usb.c`, `descriptors.c`, `error.c`, `usb.h`, `usbi.h`, `error.h` | LGPL-2.1-or-later (header states no version; Johannes Erdfelt) |
| `src/inf_wizard.c` | GPL-2.0-or-later |
| `tests/testlibusb.c` | LGPL per upstream `installer_license.txt` |

Upstream's `install_filter.c`, `libusb_dyn.c` and the Inno Setup files
are not taken.

### 6.6 Changes against upstream, each a separate commit

1. Import as-is, building with the DDK and MSVC 6.
2. `call_usbd()`: wait for completion before completing the IRP, after
   `f41b963`/`52898f0`.
3. Remove filter mode: `install-filter`, the class-filter registry code,
   and the driver's `is_filter` path stays only as "not a filter".
4. INF: DLL to DIRID 11 on 9x; no `.NT` section.
5. DLL: the four 1.x exports.
6. DLL: the bug 2928293 and 2128187 fixes, if section 2.7 item 4's
   reading confirms them.
7. Version `0.1.13.N` everywhere: resources, `DriverVer`,
   `LIBUSB_IOCTL_GET_VERSION`.

### 6.7 Test plan

98 SE is the one tested system (Owner, 2026-10-20; `README.md`, "Tested
by the author"). The `w98se-me` folder serves ME from the same build and
is "Built, not tested" there; a reading or test only ME could settle does
not block a release, and the release notes list what stays unverified on
ME (section 8). 2000 is not tested either: its users are pointed to
upstream.

Hosts: QEMU guests with 98 SE under each stack:

- the guest's own UHCI/OHCI (`piix3-usb-uhci`, `pci-ohci`);
- NUSB's EHCI stack on 98 SE (fully supported, Owner, 2026-10-20);
- xHCI98 (`nec-usb-xhci`).

Then real 98 SE hardware once, which includes the EHCI leg. An EHCI leg
counts only when EHCI carries the traffic: a High Speed device (an FX2LP
loopback board), or the USBasp or AVRISP mkII behind a High Speed (USB
2.0) hub. A Low or Full Speed device on a QEMU EHCI port is handed to the
companion controller and is a UHCI leg, so the EHCI leg is run on real
hardware with a High Speed hub, and the record shows which path carried
it.

**M0: evidence, before any code.** The upstream 0.1.12.2 binaries are
installed with a hand-written 9x INF, on 98 SE under each stack, with one
device. This tells us whether the 2009 build works at all. It separates
"the old code fails on 98" from "our build fails". Nothing from M0 ships.
The same run on ME is useful but optional; it does not block a release.

**Devices** (Owner, 2026-10-20: real devices, one or two mainstream ones
per class):

- a USBasp (V-USB, low speed, control only);
- an AVRISP mkII or clone (full speed, bulk);
- a full- or high-speed board with bulk and interrupt endpoints and a
  loopback firmware, for transfer sizes and timeouts (an FX2LP or
  RP2040 board; whichever the owner has, section 7).

QEMU has no generic vendor-class device model (`qemu-system-i386 -device
help` lists none), so these are passed through with `usb-host`.

**Checks:**

1. Install from `LIBUSB0.INF`. The DLL lands in `SYSTEM`; a program in
   another folder finds it.
2. `TESTLIBU.EXE` lists the device with its descriptors.
3. avrdude reads the signature and flash of an ATmega328P:
   - through USBasp;
   - through AVRISP mkII;
   - with the 98-era avrdude build (WinAVR 20100110) and, if one can be
     built, a current avrdude built for 98.
4. Loopback: control in and out, 0 to 4 KB; bulk 1 byte to 1 MB (across
   the DLL's 64 KB split); interrupt in. Each with and without timeouts,
   with checksums.
5. A timeout under load (unplug the loopback's sink) does not crash.
   This is the `call_usbd()` case.
6. `usb_reset`: `RESET_PORT` then `CYCLE_PORT`; the device comes back.
7. Surprise removal with the device open, and re-plug, 20 times.
8. Two devices at once (`libusb0-0001` and `-0002`).
9. No isochronous test (Owner, 2026-10-20). No current 98 tool here is
   known to use it.

### 6.8 Milestones

- **M0.** Section 6.7's evidence run with upstream binaries. Answers
  "does 0.1.12.2 run on 98 SE" as a fact.
- **M1.** Commits 1 to 5 and 7. `LIBUSB0.SYS`, `LIBUSB0.DLL`,
  `LIBUSB0.INF`, `TESTLIBU.EXE` and the developer files in
  `free/generic/libusb0/w98se-me/`, tested with USBasp and AVRISP mkII
  on 98 SE under every stack, including the EHCI leg (section 6.7). ME is
  "Built, not tested".
- **M2.** Commit 6, `INFWIZ.EXE` and the loopback suite.

## 7. Owner decisions and open questions

Owner, 2026-10-20, on the questions this draft raised:

- **Route**: libusb-win32 0.1.12.2 rebuilt from source for 98 SE and ME,
  with the four fixes of section 2.7 (section 6).
- **2000 and later**: users are pointed to upstream (1.2.6.0 for 2000 to
  7, 1.4.0.2 for 7 and later); no `w2k` folder.
- **Licences**: the driver GPL-2.0-or-later; the DLL stays
  LGPL-2.1-or-later, so closed-source 98 tools may load it; the core files
  with an unversioned LGPL header are recorded as LGPL-2.1-or-later
  (section 2.4).
- **Developer files**: `USB.H` and the import libraries go under `free/`
  (section 6.2).
- **Class folder**: `src/generic/libusb0/`, `free/generic/libusb0/`.
- **Test hardware**: one or two mainstream devices per class (section 6.7).
  The loopback board is whichever FX2LP or RP2040 board the owner has.
- **Isochronous**: no isochronous test.
- **File names under `src/`**: upstream's own names and layout are kept,
  for easier diffs against upstream (section 6.5).
- **Back-ported EHCI stacks**: NUSB's EHCI stack on 98 SE is fully
  supported and in the test matrix.

Owner, 2026-10-22, on the first build's questions:

- **MinGW import library**: GNU binutils' `dlltool` goes into the
  git-ignored `tools/` and the build generates `LIBUSB0.A` from
  `libusb0.def` (section 6.4).
- **DLL version resource**: the new LGPL `src/dllver.rc` is approved.
- **Export tables**: extracting the 98 SE and ME system DLLs from the
  pristine guest images with 7-Zip and reading their export tables (names
  and ordinals only) is fine under the host rule.

### Open questions

None at present.

## 8. Not verified

- That 0.1.12.2 runs on 98 SE, ME or 2000 at all. Only its README's
  claim and section 2.3's import check support it. M0 settles it for
  98 SE.
- On ME, which is "Built, not tested" (section 6.7), and listed in the
  release notes as unverified there: `ntkern.vxd`'s exports, its `\\.\`
  resolution of `\DosDevices\` links, and every check of section 6.7.
- ~~That the DLL's user-mode imports exist on 98 SE and ME.~~ Settled
  at export-table level for this repository's build, 2026-10-22: all 56
  are in the 98 SE and ME export tables of their modules
  ([testing/2026-10-22-libusb0.md](../testing/2026-10-22-libusb0.md)).
- The 9x loader's refusal of images with PE subsystem version 5.0 or
  later. It is irrelevant once the `CheckTokenMembership` import already
  blocks the 1.x DLL.
- That 98 SE's `usbhub.sys` serves `IOCTL_INTERNAL_USB_CYCLE_PORT`.
- Which avrdude, micronucleus, FX2 and V-USB tool builds run on 98.
- That `libusbK.dll` does not load on 98 (only "unlikely").
- The effort figures, which are estimates.

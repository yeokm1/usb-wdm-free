# Installer (INSTALL.BAT)

Status: deferred, design draft, 2026-10-20. No code yet. Owner,
2026-10-20: no installer for now; users install each driver by hand from
its `free/` folder (`README.md`, "Installing"). This draft is kept for
later. No package depends on it, and the signing policy of section 11 is
also recorded in [build.md](build.md) section 7. Before work resumes,
its layout (`free/INSTALL.BAT`, `free/INSTALL/`, `src/install/`) must be
brought under `AGENTS.md`'s `src/<class>/<package>/` and
`free/<class>/<package>/<systems>/` rules, or the owner must record an
exception there.

## 1. Purpose and scope

Owner, 2026-10-20: "USB free and non free should have their own
installer tool like a simple batch program. Those drivers should come with
their own installer." This repository's installer is a batch program,
`free/INSTALL.BAT`, that installs, updates and removes the drivers released
under `free/` on Windows 98 SE, ME, 2000, XP, Vista, 7 and 8 (x86, and x64
where a package has an x64 build), using only what each of those systems
has in the box, plus one small helper program of this repository's own
where the box has nothing (section 7).

Pointing the Add New Hardware wizard or Update Driver at a `free/` folder
stays a supported way to install every package (`README.md`,
"Installing"). The installer adds three things that route lacks: drivers
staged before their device is plugged in, so the first plug can find its
files locally (Windows may still ask for media or signing consent); a
driver put onto a device that is already present and
bound to something else; and an uninstall that removes what was installed
and nothing more.

Not in scope: a windowed front end; any driver that is not released under
`free/`; changing the system's driver-signing policy.

## 2. Independence

This repository's installer works on its own. A release of usb-wdm-free
carries everything the installer needs at run time, its helper program
included, and nothing from any other repository is needed to build,
release or run it. It may notice files that other installers placed
(section 9), but that detection is optional: with no other installer
present it works the same.

## 3. Decisions in brief

1. **One batch file for both interpreters.** `INSTALL.BAT` runs under
   `COMMAND.COM` on 98 SE and ME and under `cmd.exe` on 2000 to 8. It
   splits into a 9x half and an NT half on its first test (`%OS%`), and
   the 9x half uses nothing `COMMAND.COM` lacks (section 5.1).
2. **A menu, one package at a time.** `CHOICE` on 98 SE and ME, `SET /P`
   on 2000 to 8, because `CHOICE` is not in the box on 2000 and XP
   (section 5.5).
3. **98 SE and ME: pure batch.** A package is staged by running a
   staging INF of this repository's with the in-box
   `rundll.exe setupx.dll,InstallHinfSection`, which copies the package's
   INFs into `%windir%\INF` and its binaries to where its INF sends them,
   never over a file already there (section 6.2). A device already present
   gets printed Device Manager steps.
4. **2000 to 8: a helper is needed, `WDMINST.EXE` (x86) and `WDMIN64.EXE`
   (x64).** Windows 2000 has no in-box way to stage a Plug and Play INF,
   and no system in the range has an in-box command that installs a driver
   on a device already present or one bound to a better-ranked in-box
   driver. The helper is a small GPL-2.0 Win32 console program calling
   documented SetupAPI and newdev routines (section 7). The batch file stays
   the user-facing tool. Owner, 2026-10-20: the helper is accepted, on
   2000 to 8 behind `INSTALL.BAT`; 98 SE and ME stay pure batch.
5. **Ownership is recorded in files, not the registry,** because the 9x
   half cannot read the registry. A file this installer placed (absent
   before) is its own; a file it found present never is. It never
   overwrites a file it did not place and never deletes one (section 8).
6. **x64 signing as xHCI98 and Stor98 do it** (Owner, 2026-10-20): the
   x64 drivers carry the project's embedded test signature. The installer
   reports whether test-signing mode is on and prints the steps. It never
   runs `bcdedit` (section 11).
7. **One claim scheme shared with other installers.** Owner, 2026-10-20:
   the `%windir%\USBCLAIM\` marker scheme of section 8.2 is accepted, for
   usb-wdm-nonfree's installer and any other to use too.
8. **A package installed by someone else is reported and never
   replaced** (Owner, 2026-10-20; section 8.2, rule 5).

## 4. Layout and names

All names under `free/` are 8.3 (`AGENTS.md`, "Names").

| Path | What |
|---|---|
| `free/INSTALL.BAT` | The installer. ASCII, CRLF line endings (section 5.6) |
| `free/INSTALL/WDMINST.EXE` | The helper, x86: 2000, XP, Vista, 7, 8 32-bit. The 9x half never calls it (section 3, item 3) |
| `free/INSTALL/WDMIN64.EXE` | The helper, x64: XP x64, Vista, 7, 8 64-bit |
| `free/INSTALL/ARP.INF` | Writes and removes the Add/Remove Programs entry (section 8.5) |
| `free/INSTALL/P/<ID>.BAT` | One package script per package and systems folder, generated at release (section 4.1) |
| `free/INSTALL/P/<ID>.INF` | The 98 SE and ME staging INF of that package and folder, generated at release |
| `free/INSTALL/README.TXT` | What the installer does, the switches, the steps it may print |
| `src/install/wdminst/` | The helper's source and `build.cmd` |
| `src/install/` | `INSTALL.BAT`'s source, the generators of `P/`, and the batch lint (section 12.1) |

`<ID>` is an 8.3 base name naming the package and its systems folder, for
example `UAC2W9X` for `free/audio/uac2/w98se-me/`, `UAC2NT` for
`w2k-xp`, `UAC2X64` for `wv-8-x64`. The systems folders are those of the
one scheme in `README.md`, "Layout" (Owner, 2026-10-20). The installer's
own source is in `src/install/`, class `install`, its helper in
`src/install/wdminst/` (`README.md`, "Layout").

### 4.1 Package scripts

`COMMAND.COM` has no way to read a list from a file (no `FOR /F`), so the
per-package data is code: a small batch file per package and folder,
generated at release from the package's INF, which `INSTALL.BAT` calls
with a verb. It defines, as `SET` lines and calls back into
`INSTALL.BAT`:

- the package's display name, version (the INF's `DriverVer`), and
  the systems the folder serves, as the OS keys of section 5.2;
- each file the INF copies, with its destination as a Windows directory
  id (`SYSTEM32\DRIVERS`, `SYSTEM`, `IOSUBSYS`, `INF`);
- whether each copy is this package's payload or an external Windows
  dependency reached through `Include`/`Needs` or the system layout.
  Only package payload participates in ownership and conflict checks;
  in-box dependencies are installed and maintained by Windows;
- each INF of the package (a package may have two: the serial package's
  `USBCOM.INF` and `USBCOMP.INF`);
- its install policy for a present device (section 6.5): `NOFORCE`, or
  the names of the in-box drivers it may take a device from when the user
  confirms;
- the Windows CD note, where the INF pulls Microsoft files from the
  Windows media through `LayoutFile` or `Include`/`Needs`;
- the steps text for a present device on 98 SE and ME.

A release gate compares each package script and staging INF with the
package INF's `CopyFiles` and models, so they cannot drift apart.

## 5. The batch program: one file, two interpreters

### 5.1 What each interpreter lacks

The 9x half runs on the `COMMAND.COM` of 98 SE (MS-DOS 7.1) and ME. The
NT half runs on `cmd.exe` with command extensions, on by default from
Windows 2000.

| Feature | 98 SE / ME `COMMAND.COM` | 2000 / XP `cmd.exe` | Vista / 7 / 8 `cmd.exe` |
|---|---|---|---|
| `FOR /F` (read lines, command output) | no | yes | yes |
| `SET /P` (read a line from the user) | no | yes | yes |
| `%~dp0` (the batch file's own folder), `%~f0` | no | yes | yes |
| `%*` (all arguments) | no; `%1` to `%9` and `SHIFT` only | yes | yes |
| `IF /I`, `IF DEFINED`, `IF ... ( ... ) ELSE ( ... )` blocks | no | yes | yes |
| `CALL :label`, `SETLOCAL`, `%ERRORLEVEL%` | no; `IF ERRORLEVEL n` (true when the code is n or more) only | yes | yes |
| `2>` (redirect standard error) | no | yes | yes |
| `IF EXIST` with wildcards, `IF EXIST dir\NUL` for a folder | yes | yes (`dir\` preferred) | yes |
| `CHOICE` | in the box (`%windir%\COMMAND\CHOICE.COM`) | not in the box (Resource Kit only) | in the box (`choice.exe`, other syntax) |
| `FIND`, `FC`, `XCOPY` | in the box (`%windir%\COMMAND`) | in the box | in the box |
| Reading the registry | no tool (only `REGEDIT /E` export, behaviour on a missing key unverified) | `REG.EXE` on XP; on 2000 only in the Support Tools | `REG.EXE` |
| Environment size | 256 bytes by default for a batch's copy; more with `COMMAND /E:n` | bounded, but no DOS-size assumption | bounded, but no DOS-size assumption |

Sources: the MS-DOS 6.22 `HELP` entries for `CHOICE`, `FOR`, `IF`, `SET`;
Microsoft's command-line references for Windows 2000, XP and Server 2003
(`set /p`, `for /f`, `call`, `if /i`, and `choice`, which the Server 2003
reference lists as new there and which Vista and later carry); the 98 SE
in-box INFs for the `rundll.exe setupx.dll` form (section 6.2). None of
these was run on a target for this draft: each row is read again on the M0
legs (section 12) and the table corrected where a reading differs.

What the 9x half does about each gap:

- **No `%~dp0`.** It works relative to the current directory and checks
  it first (section 5.3).
- **No `CALL :label`.** It calls itself with a verb as the first argument,
  `CALL INSTALL.BAT VERB args`, and the first lines dispatch on `%1`.
- **No `FOR /F`.** The data it needs is code (section 4.1). The folder
  checks use `IF EXIST`, and a loop over files uses `FOR %%F IN (...)`,
  which `COMMAND.COM` has, with wildcards.
- **Errorlevels.** Every test is `IF ERRORLEVEL n` in descending order.
  `COPY` and `DEL` do not set a dependable errorlevel, so every copy and
  delete is checked afterwards with `IF EXIST`.
- **Environment space.** On "Out of environment space" the batch file
  restarts itself once under `%COMSPEC% /E:4096 /C INSTALL.BAT ...`, the
  documented `COMMAND` switch.

### 5.2 Which system it is

1. `IF "%OS%"=="Windows_NT"`: the NT half. `%OS%` is set by NT's
   environment; 98 SE and ME do not set it.
2. 9x half: `%windir%` empty means the batch file was run outside Windows
   (MS-DOS mode, or a boot floppy): refused. Then
   `VER | FIND "4.10.2222"` for 98 SE and `VER | FIND "4.90."` for ME,
   tested by `FIND`'s errorlevel (0 found, 1 not found). Any other version
   is refused (98 first edition, 95). If the M0 leg shows that the 98 SE
   `FIND` does not set a useful errorlevel, M0 must establish another
   pure-batch test before that system can ship; v1 keeps the owner's
   pure-batch requirement.
3. NT half: `VER` piped to `FIND` for `5.00.` (2000), `5.1.` (XP),
   `5.2.` (XP x64), `6.0.` (Vista), `6.1.` (7), `6.2.` (8). The version
   numbers are in `VER`'s output on every language. `6.3.` and later
   (8.1 on): a warning, "not a target, not tested", and the user may go on
   (Owner, 2026-10-20: warn and allow). Then the helper's
   `/OS` mode confirms the version and the product type (a server is
   refused, as not a target) by `GetVersionEx`.
4. Architecture on NT: `PROCESSOR_ARCHITECTURE` or
   `PROCESSOR_ARCHITEW6432` equal to `AMD64` is x64. With
   `PROCESSOR_ARCHITEW6432` set, the batch file is running in a 32-bit
   `cmd.exe` under WOW64, where `%windir%\system32` is redirected and
   device installation is refused (section 7.1). It restarts itself in
   the 64-bit `cmd.exe` through `%windir%\Sysnative\cmd.exe` (Vista and
   later). On XP x64, `Sysnative` needs an update not in the box, so it
   stops and asks the user to run it from Explorer or Start, Run, `cmd`,
   both 64-bit.

The resulting OS key (`98SE`, `ME`, `2K`, `XP`, `XP64`, `V86`, `V64`,
`W7`, `W7X64`, `W8`, `W8X64`; final names in M1; 7 and 8 have separate
keys because some packages, `cdcnet` among them, serve 7 and not 8) selects the packages the
menu lists: a package script names the keys its folder serves.

### 5.3 Where it is run from

The batch file may be started by a double-click in Explorer, from a
command prompt in or outside its folder, from a CD or a mounted image, or
from a network share.

- **9x.** `COMMAND.COM` gives no reliable path of the batch file itself
  (`%0` is whatever was typed). Started from Explorer, the current
  directory is the batch file's folder. The first check is
  `IF EXIST INSTALL\P\NUL` and `IF EXIST INSTALL.BAT`. If either fails, the
  batch file says "Change to the folder that holds INSTALL.BAT and run it
  there" and stops.
- **NT.** `PUSHD "%~dp0"` first. That also covers two traps: a UNC path,
  which `cmd.exe` refuses as a current directory and `PUSHD` maps to a
  temporary drive letter, and "Run as administrator" on Vista and later,
  which starts in `%windir%\system32`.
- **Read-only media.** The installer writes nothing next to itself. Its
  first step for a package is a local copy (section 8.1), so a CD, an image
  or a share may go away afterwards.

### 5.4 Rights

- 98 SE and ME have none to check.
- On NT the batch file calls `WDMINST /ADMIN`. It answers by errorlevel:
  administrator or not, and on Vista and later elevated or not. Without
  rights the batch file says "Right-click INSTALL.BAT, Run as
  administrator" (Vista and later) or "Log on as an administrator" (2000,
  XP) and stops. The helper's manifest asks for `asInvoker`, so the
  helper never raises a consent prompt of its own for each call.

### 5.5 The menu

The menu lists the packages whose script names the current OS key, one
line each:

- a letter, the name and the version;
- the state: not installed, installed by this installer and current,
  installed by this installer at an older version, or present but not
  installed by this installer (section 8.2).

Then come Uninstall, Show details and Quit. One action per choice,
then the menu again.

- **98 SE, ME**: `CHOICE /C:ABCDEFGHIJKLMNOPRUQ /N`, then
  `IF ERRORLEVEL n` from the highest down. `CHOICE` returns the 1-based
  position of the key. The letters are fixed by the generated list,
  at most 16 packages on one page; a second page if there are more.
- **2000 to 8**: `SET CH=` then `SET /P CH=Choice: `, compared with
  `IF /I`. `SET /P` keeps the old value on an empty line, hence the
  clear first. `choice.exe` exists on Vista and later but not on 2000
  and XP, so one route serves all of NT.

Switches for scripts on NT: `/I <ID>` install or update one package,
`/U <ID>` uninstall one, `/U ALL`, `/L` list states, and `/Y` to answer
yes. `/Y` accepts an already allowed plan; it never overrides ownership,
device-selection or signing-policy checks. On 9x the menu is the normal
interface; `/U` is a reserved entry into that menu's uninstall-all
confirmation for Add/Remove Programs (section 8.5). It fits in `%1` and
does not require NT's `%*` expansion.

### 5.6 Line endings and text

`INSTALL.BAT` and every `P/*.BAT` are ASCII with CRLF line endings. A
`.gitattributes` rule holds `*.BAT` to CRLF. `COMMAND.COM` finds `GOTO`
labels only by line, and LF-only batch files lose labels on 98 SE (to be
confirmed on the M0 leg, which runs an LF copy on purpose). INFs are ASCII,
CRLF, as the packages' INFs. Messages are at most 79 columns. They are in
English, and they never parse the localised output of a Windows tool:
only errorlevels and `IF EXIST`.

## 6. How a package is installed, per system

### 6.1 What each system has in the box

| Need | 98 SE / ME | 2000 | XP | Vista / 7 / 8 |
|---|---|---|---|---|
| Run an INF's non-device install section | `rundll.exe setupx.dll,InstallHinfSection <sect> <mode> <inf>` (16-bit engine) | `rundll32 setupapi.dll,InstallHinfSection` | same | same |
| Put an OEM device INF where the device installer finds it later | an INF section's `CopyFiles` to LDID 17 (`%windir%\INF`) | none in the box (`SetupCopyOEMInf` is an API, no command) | `CopyINF=` in an INF's `DefaultInstall` (Microsoft documents it from XP); untested here | `pnputil -a <inf>` |
| Install a driver on a present device | none (only the wizards) | none (only the wizards) | none | `pnputil -i -a <inf>`, ranked: it does not displace a better-ranked (for example signed in-box) driver |
| Remove a staged INF | delete the copy | none | none | `pnputil -d oemNN.inf` |

Sources: the 98 SE `msmouse.inf` line 529 runs
`rundll.exe %11%\setupx.dll,InstallHinfSection MsmousePerUser 64 %17%\msmouse.inf`;
the 98 DDK's `setupx.h` (`INC_WIN98_INC16_SETUPX.H`) defines
`LDID_INF 17` (line 139) and its import library `setupx.lib` carries the
`InstallHinfSection` export name; the 98 DDK's NDIS miniport notes
(`HELP_OTHER.CHM`, `ndismini_01tc.htm`) describe an INF's `CopyFiles`
that "will copy ... the INF for the adapter into the Windows\Inf directory
and rebuild the driver database file"; Microsoft Learn, "INF DefaultInstall
Section" (`CopyINF`, Windows XP and later), "PnPUtil" (the Vista to 8
syntax `-a`, `-i -a`, `-d`, `-e`), "SetupCopyOEMInf",
"UpdateDriverForPlugAndPlayDevices", and "How Windows Ranks Driver
Packages" (an unsigned package ranks below a signed one from Vista on).

### 6.2 98 SE and ME

**Staging (the device not yet present).** For a package the user picks:

1. The package's folder is copied to the local copy,
   `%windir%\WDMFREE\PKG\<ID>\`, with `XCOPY`, the staging INF
   `<ID>.INF` beside it, and checked file by file (section 8.1).
2. For each file the package script names, `IF EXIST` at its destination
   decides, before anything is copied, whether this installer will place
   it. A file already there is recorded as found (section 8.2) and is not
   touched.
3. `rundll.exe setupx.dll,InstallHinfSection DefaultInstall 132 <ID>.INF`
   runs the staging INF from the local copy. It is a `$CHICAGO$` INF of
   this repository's whose `DefaultInstall` `CopyFiles` sections send the
   package's binaries where the package INF sends them, and the package's
   INFs to LDID 17 (`%windir%\INF`). It does what NUSB's own installer INF
   does for its drivers (NUSB 3.3 `_NUSB.INF`, `[DestinationDirs]`, the
   INFs to 17 and the drivers to `10,SYSTEM32\DRIVERS`). Every file entry
   carries flag `0x10`. In `setupx.h` that is `VNLP_IFEXISTS`, "File
   skipped if already on machine" (line 737), so the engine too skips a
   file that appeared since step 2. That an INF's copy flags reach the
   engine as these `VNLP_` values is unverified, and M0 reads it.
   Mode `132` (silent, restart if needed) is the value Microsoft's NT
   documentation of `InstallHinfSection` gives. Whether the 16-bit engine
   honours the same bits is read in M0. The 98 SE in-box INF uses `64`.
4. Step 2's list is checked again: each file absent before and present
   now is compared byte for byte with the local source using `FC /B`.
   Only a match is recorded as placed (section 8.2). Missing, different
   or unreadable files leave an incomplete operation with its records.
5. A package INF whose name is already taken in `%windir%\INF` by a file
   this installer did not place stops that package before step 3, with
   the Have Disk steps. Overwriting another INF of the same name is never
   done. The package names in this repository are chosen not to collide
   with in-box INFs (each design names its INF).

**Copying into `INF\OTHER` by hand is not done.** `INF\OTHER` is where the
setup engine keeps the OEM INFs it installed from. It records each copy's
original name under `HKLM\Software\Microsoft\Windows\CurrentVersion\Setup\SetupX\INF\OEM Name`
(`REGSTR_PATH_INF_OEMNAME`, 98 DDK `regstr.h` line 64). Writing either by
hand edits the engine's own bookkeeping. A copy to LDID 17 through the
engine's own `CopyFiles` is the route the DDK describes.

**The driver database.** 98 SE keeps an index of the INF directory
(`DRVIDX.BIN`, `DRVDATA.BIN` in `%windir%\INF`). The DDK sentence above
pairs the copy with a rebuild of that index, but does not say which
component does it. The installer asks for a restart after staging on 98 SE
and ME, as NUSB's installer does. The M0 leg reads three things:

- whether a device plugged before the restart finds the staged INF;
- whether one plugged after the restart does;
- whether a rebuild is needed.

If staged INFs are not found after a restart, the fallback is to delete
`DRVIDX.BIN` and `DRVDATA.BIN`, which Windows rebuilds at the next start
("Building driver information database"). That is a widely given repair,
not a documented interface, so it is taken only if M0 shows it necessary,
and is then said in the release notes.

**Microsoft files from the Windows CD.** Packages whose INF pulls
Microsoft's files through `LayoutFile=layout.inf` or `Include`/`Needs`
(`hidclass.sys` for the gamepad, KS and WDMAUD for the audio packages,
the network components for the Ethernet packages) make Windows ask for
the 98 SE or ME CD at the device's install. The installer says so before
staging and names the folder to type when the offered path is not the
CD's (the CD-ROM drive's `\WIN98` or `\WIN9X`, or wherever its contents
were copied). Nothing of Microsoft's is shipped.

**The device already present** is section 6.5.

### 6.3 2000 and XP

The helper does the work. `INSTALL.BAT` calls
`WDMINST /STAGE <ID> <release folder>`, naming the offered `free/`
folder:

1. The helper allocates a source generation under
   `%windir%\WDMFREE\PKG\<ID>\G0000001\` (section 8.1), copies the
   offered release there and verifies every file before publishing any
   INF. A verified generation is immutable and never renamed: 2000 and
   XP take a device's files, at its later install, from the path recorded
   when the INF was staged. `localCopy` below is that generation's full
   path, not the package's parent directory.
2. `SetupCopyOEMInfA(inf, localCopy, SPOST_PATH,
   SP_COPY_NOOVERWRITE, ...)` for each INF. Success records the returned
   `oemNN.inf` as newly published. `ERROR_FILE_EXISTS` returns an existing
   name: it is reused only if already owned by this package, otherwise
   reported as present and left unclaimed. Other errors stop the step.
   The local source is retained even when staging succeeds.
3. Present devices, by section 6.5.

`rundll32 setupapi.dll,InstallHinfSection` with a `CopyINF=` line would
stage on XP without the helper. It is not used. The helper is needed on
2000 and for present devices anyway, and one route for 2000 to 8 means one
thing to test.

### 6.4 Vista, 7 and 8

As 2000 and XP, through the helper. `pnputil -a` would stage, but
`pnputil` on these systems has no switch that forces a driver over a
better-ranked one. That is the case the audio packages meet: an unsigned
x86 package ranks below Microsoft's signed `wdma_usb.inf` (design
`uac2.md` section 7, `usbmidi.md` section 4.2). Its output is also
localised. `pnputil` remains the documented manual route, and
`README.TXT` gives its command lines for users who want to stage by hand.

**The identical-INF trap.** On Vista and later, when the driver store
already holds a byte-identical INF, Windows installs from the stored
package with its old files. Each release of a package changes its INF's
`DriverVer`, so released packages never meet it. A release gate refuses
a package whose INF is identical to its previous release's while its
binaries differ.

Staging and loading are separate gates. An embedded signature on a SYS
does not sign its INF package. M0 must test `SetupCopyOEMInf` for the
owner's catalogless binary packages, and catalog signing for INF-only
packages, on each target architecture. A staging rejection is a failed
operation, never a reason to copy into the Driver Store by hand. The
build and release contract is in [build.md](build.md).

### 6.5 A device already present

A device plugged in before the installer ran is in one of three states:
no driver (the wizard cancelled, "Other devices"), an older release of
this package, or another driver.

| State | 98 SE / ME | 2000 to 8 |
|---|---|---|
| No driver, wizard cancelled | Printed steps: Device Manager, the device, Remove, then Refresh; the new-hardware search then finds the staged INF. If not, Update Driver pointing at the local copy | `WDMINST` installs it: `UpdateDriverForPlugAndPlayDevicesA(NULL, <device's first hardware id>, <full local-source INF path>, INSTALLFLAG_FORCE, &reboot)` |
| This package, older release | The files this installer placed are replaced (section 8.3). The device runs them after the restart. If the new release changes the INF's registry or file set, printed Update Driver steps | `WDMINST` reinstalls it from the new INF, same call |
| Another driver | Printed Update Driver steps, offered only when the package script names that driver as one it may replace | Only when the package script names the driver (an in-box Microsoft driver the package is meant to replace, for example `usbaudio.sys` for a UAC 2 function on Vista and later) and the user confirms. A vendor or third-party driver is never displaced |

The id rule: the call uses the device's own first (most specific)
hardware id, never a compatible id such as `USB\Class_01`. This API acts
on every present device carrying that id. Immediately before the call,
the helper re-enumerates them and requires each to match the package's
allowed interface/function and replacement policy, and the confirmed
plan. Two identical devices, one on another vendor's driver, stop the
call for that id; the user is told to use Device Manager. Use the INF
in the operation's verified source generation (section 8.1), never
`%windir%\INF\oemNN.inf`, as `FullInfPath`.

`hwndParent` is `NULL`. Windows' own signature dialogs ("has not passed
Windows Logo testing" on XP, "cannot verify the publisher" on Vista and
later) are answered by the user. A refusal comes back as an error the
batch file shows.

A device that is not present is not installed by the helper. It binds at
its next plug from the staged INF. The helper's summary says so.

### 6.6 Is pure batch enough?

No, not for this repository's packages. Pure batch can stage on 98 SE and
ME (section 6.2), on XP through `CopyINF` (untested), and on Vista to 8
through `pnputil -a`. It cannot do these:

- stage at all on Windows 2000;
- install a driver on a present device on any NT system, short of
  printed steps;
- force a driver over a better-ranked in-box one, which the audio
  packages and the RTL8153's move from `cdcnet` to `rtl815x`
  (`ethernet.md` section 6.5) need;
- remove a staged INF on 2000 and XP;
- find which devnodes use a staged INF before removing it.

On 98 SE and ME no Win32 program can install a driver on a device
either: the device installer is the 16-bit `SETUPX`. So the helper is
NT-only for device work. Owner, 2026-10-20: 98 SE and ME get printed
steps for present devices in v1; a 16-bit SETUPX helper comes later, if
needed (section 13, M5).

## 7. The helper

### 7.1 What it is

A Win32 console program in C, GPL-2.0, source in `src/install/wdminst/`.

- **x86** `WDMINST.EXE`: MSVC 6.0 with the C runtime linked statically
  (`LIBC`), so one image starts on 2000, XP, Vista, 7 and 8. It is not
  run on 98 SE or ME, whose half of the installer is pure batch.
- **x64** `WDMIN64.EXE`: WDK 7.1's amd64 compiler, subsystem 5.02, for
  XP x64 to 8 x64. Microsoft documents that a 32-bit process on 64-bit
  Windows cannot run device installation (`SetupDiCallClassInstaller` and
  the routines built on it fail with `ERROR_IN_WOW64`), so the x64 build
  is required. The x86 build under WOW64 (`IsWow64Process`, looked up at
  run time) refuses.
- **Imports** `KERNEL32` and `ADVAPI32` only. `setupapi.dll`, `newdev.dll`
  and `cfgmgr32.dll` are loaded with `LoadLibrary`, and each routine is
  looked up with `GetProcAddress`. The image therefore loads on 2000
  even where a later routine is absent. A routine missing on a system refuses the step that needs it and names
  the routine. A build check holds the import table to those two DLLs.
- **Manifest** `asInvoker`, so Vista and later do not prompt for every
  call (section 5.4).
- **Not signed.** It is a user-mode program, and the x64 test signature
  (section 11) is for drivers.
- The toolchains go in the git-ignored `tools/`, as for the drivers
  (`AGENTS.md`, "Standalone"); [build.md](build.md) defines the profiles
  and import checks.

### 7.2 Modes

| Mode | Systems | What |
|---|---|---|
| `/OS` | NT | errorlevel = an OS key code; confirms section 5.2 step 3 |
| `/ADMIN` | NT | errorlevel 0 administrator (elevated on Vista+), 1 not |
| `/SAME <a> <b>` | NT | errorlevel 0 when the two files are byte-identical, 1 when not, 2 when either is unreadable |
| `/STAGE <ID> <dir>` | NT | section 6.3 step 2 for each INF of the package |
| `/DEVICES <ID>` | NT | lists the present devices the package's INFs match, with their current driver, and the plan of section 6.5 |
| `/APPLY <ID> [/Y]` | NT | carries out the plan, asking per device unless `/Y` |
| `/REMOVE <ID>` | NT | section 8.4's NT steps |
| `/TESTSIGN` | x64 | section 11 |

The mutation modes return 0 done, 1 refused before changes, 2 failed
(possibly after changes; the log says what), 3 done but restart needed,
4 done but steps remain (printed). The query modes `/OS`, `/ADMIN` and
`/SAME` use their own codes in the table. Every chain is tested in
descending order; codes are categories, not severity levels. A failure
with a restart also needed returns 2 and records the restart requirement.
Every NT run appends to `%windir%\WDMFREE\WDMINST.LOG`.

### 7.3 Size

Roughly 1,500 to 2,000 lines of C (an estimate): device enumeration with
`SetupDiGetClassDevs` (`DIGCF_ALLCLASSES`, present and all), INF model
matching through `SetupDiBuildDriverInfoList` with `DI_ENUMSINGLEINF` on
each device, the calls above, and the record files.

### 7.4 Execution and recovery contract

An install is several Windows operations, not one atomic transaction.
Before mutation, the installer validates the selected package id against
the generated list, the OS and architecture, all source files, destination
conflicts, and the present-device plan. IDs and paths read from records
are data, never commands to execute. An id cannot contain separators,
drive prefixes or `..`; a record cannot expand the package's allowed
destinations or service names.

`INST\<ID>\PENDING.TXT` records the requested version and current phase:
prepare, publish, apply, verify, commit; removal records remove and
cleanup. On 9x the same phases are empty `PH_*` markers (section 8.1),
because the batch file can test a file's presence but not read it. Before each destructive step, its old reference and ownership
record must already exist. Re-running an incomplete operation verifies
the observed state and resumes or prints recovery steps. `VERSION.TXT`
is committed only after the intended files and device bindings have
been verified. A cancellation after publishing retains that INF's record
and local source so uninstall can find them.

The installer runs one operation at a time. The 9x path requires other
installers and new-hardware wizards to be closed during file staging;
M0 tests interruption at every phase. NT checks for an existing active
operation and refuses concurrent use. Records left by a crash are shown
as incomplete; their presence alone is not proof that a process is still
running. External Windows installers do not participate in this lock,
so files and devnode bindings are checked again before mutation.

Before an NT apply or update that can copy files, all package-payload
destinations must pass section 8.2 again. External Windows dependencies
are excluded from claims and payload-conflict checks; finding an existing
USBSTOR binary is expected for `usbflop`. An unowned payload file or
changed owned file blocks
the apply, rather than relying on SetupAPI's version rules to preserve
it. The release gate rejects package directives that copy outside the
declared file set. M0 must prove this preflight and Windows' actual copy
behaviour preserve the ownership rules, including after partial failure.

## 8. Records, ownership, updates and uninstall

### 8.1 The installer's home

`%windir%\WDMFREE\` (8.3) holds:

| Path | What |
|---|---|
| `INSTALL.BAT`, `INSTALL\` | A copy of the installer, so the uninstall works without the download |
| `PKG\<ID>\` | 9x: the active local copy, Have Disk source and comparison reference. NT: parent of immutable source generations below |
| `PKG\<ID>\G0000001\` | NT: one verified source generation; the full path used by staging and apply |
| `INST\<ID>\SOURCE.TXT` | NT: the committed active generation name, for example `G0000001`; read as data, never executed |
| `INST\<ID>\GENLAST.TXT` | NT: generation allocation high-water mark, retained even when an unused generation is removed |
| `INST\<ID>\VERSION.TXT` | The package is installed by this installer, at this version |
| `INST\<ID>\PENDING.TXT` | NT: an incomplete operation, its requested version and phase (section 7.4) |
| `INST\<ID>\PH_<phase>` | 9x: an empty marker per reached phase (`PH_PREP`, `PH_PUB`, `PH_APPL`, `PH_VERI`, `PH_REM`, `PH_CLEAN`), tested with `IF EXIST`, since `COMMAND.COM` cannot read a file's contents |
| `INST\<ID>\OEMnn.INF` | Empty marker: the NT INF copy this installer created (section 6.3) |
| `INST\<ID>\OEMnn.SRC` | NT: the source generation of that published INF; retained with the marker until the INF is removed |
| `INST\<ID>\ABSENT.TXT` | NT: payload destinations absent when `/STAGE` ran, for the deferred placement rule of section 8.2 |
| `INST\<ID>\SERVICE.NEW` | The package's service did not exist before this installer's first install of it |
| `INST\<ID>\DEVICES.TXT` | NT: the device instance ids the helper installed. Written and read by the helper only |
| `WDMINST.LOG` | The helper's log |

On NT, generation names are `G` followed by seven decimal digits.
Allocation advances monotonically above the highest existing number and
`GENLAST.TXT`, recording the allocation before use and never reusing an
old name. Exhaustion at `G9999999` refuses a new
operation before publication. A partial preparation can be discarded or
re-copied only before its INFs are published; `PENDING.TXT` distinguishes
preparation from a verified immutable source. `SOURCE.TXT` changes only
at successful commit. Until then the pending operation records its new
generation separately, including the generation used to verify each
file already placed by that operation.

An owned published INF keeps its generation alive even while its devices
are absent or an update has committed a newer generation. The helper
removes a generation only after no published-INF record, current source
pointer, pending operation or outstanding file cleanup references it.
This avoids changing SetupAPI source paths during updates. If
`SP_COPY_NOOVERWRITE` returns an already owned INF, its existing
`OEMnn.SRC` remains authoritative; the unused prepared generation can be
discarded after all other references are checked.

### 8.2 Who owns a file

Every file the installer may place outside its home (a driver binary,
a DLL, a package INF in `%windir%\INF` on 9x) has a claim directory,
`%windir%\USBCLAIM\<file name>\`, holding empty marker files named after
the installers that use it:

- `WDMFREE.PLC`: this installer placed the file. It was absent when
  section 6.2 step 2 looked, and present afterwards.
- `WDMFREE.USE`: this installer found the file present and relies on it.

A claim directory is keyed by file name alone. The packages of this
repository have unique file names, and their destinations are fixed by
their INFs.

On NT, binaries reach `system32\drivers` through Windows' own device
install, from the staged INF. A file absent before the helper's first
install of a package and present after is recorded as `.PLC` the same
way, by the helper checking before and after. A device absent at staging
gets its files later, from Windows, when the helper is not running. So
`/STAGE` records each payload destination absent at that moment in
`INST\<ID>\ABSENT.TXT`. Something else may place identical bytes in
that time, so absence plus byte equality does not prove the placement
was this installer's. A later run records such a file as `.USE`, never
`.PLC`, unless the helper itself observed the placement; the uninstall
then leaves it, which is the safe side.

The rules:

1. **Never overwrite** a file without a `WDMFREE.PLC` marker.
2. **Never delete** a file without a `WDMFREE.PLC` marker.
3. **Never delete a placed file that another installer claims.** If any
   other marker is in its claim directory (`IF EXIST` with a wildcard,
   after this installer's own marker is removed), the file stays. The
   uninstall says which other installer claims it. The file is then
   no installer's to delete; that is the safe side.
4. **Never delete a placed file whose bytes changed.** If it differs from
   the local copy's, someone replaced it, and it stays, with a note.
   NT uses `WDMINST /SAME`; pure-batch 9x uses `FC /B` after M0 confirms
   match, mismatch and unreadable-file errorlevels. An unreadable file is
   left. `IF EXIST` alone proves neither a complete copy nor ownership.
5. **A package present but not installed by this installer** (its binary
   present with no `WDMFREE.PLC`, or its service registered on NT with no
   `INST\<ID>`) shows in the menu as "present, not installed by this
   installer". It is never installed over (Owner, 2026-10-20: reported,
   never replaced). To use this installer for it, the user uninstalls it
   first by whatever installed it. This keeps the "never overwrite" rule
   whole.

The marker scheme is shared (Owner, 2026-10-20). Another installer that
places or uses the same files, usb-wdm-nonfree's among them, writes its
own `<NAME>.PLC` and `<NAME>.USE` markers there and honours the rules
above. Nothing here depends on one doing so.

### 8.3 Update

Running the installer again on a package it installed at an older
version:

- on NT, the offered release is prepared as a new immutable generation
  (section 8.1). The previous generation remains the committed comparison
  source; staging and apply use the new generation's full INF path;
- on 9x, the offered release is checked in `PKG\<ID>.NEW\` while the
  active source stays at `PKG\<ID>\`. Before replacing the active source,
  preserve it as `PKG\<ID>.OLD\` for comparison and interrupted-operation
  recovery. The source move happens only after destination verification;
- each file with a `WDMFREE.PLC` marker whose bytes equal the old local
  copy's is replaced by the new release's. On 9x it is copied with
  `COPY`, since the staging INF's no-overwrite flag would skip it. On NT
  it is replaced by Windows' own install from the new INF (section 6.5,
  second row). A placed file whose bytes changed blocks the update
  before apply, with a note, as in rule 4;
- new files of the new release are placed by section 6.2 or 6.3, with
  their markers. Files the new release no longer has are removed by the
  uninstall rules;
- on NT, a new `oemNN.inf` is staged; its published name is recorded with
  its source generation in `OEMnn.SRC`. The old INF is removed by section
  8.4's NT step 2 once no device names it. Otherwise its original source
  generation stays available without blocking a successful update of the
  present devices;
- NT commits the new `SOURCE.TXT` pointer after destination and device
  verification. 9x promotes the verified `.NEW` source to `PKG\<ID>\`,
  retaining `.OLD` until outstanding cleanup is complete. `PENDING.TXT`
  records this commit sequence so a crash between source and version
  writes can be reconciled;
- `VERSION.TXT` is rewritten last. An old comparison source is deleted
  only after successful commit and any pending cleanup; NT also requires
  every reference listed in section 8.1 to be gone.

A version is the INF's `DriverVer` version. On NT the helper compares
it and says whether the step is an update or a downgrade. A downgrade is
allowed after a confirmation. On 9x the batch file cannot compare two
version strings. It shows the installed and the offered version and asks
before any change.

### 8.4 Uninstall

Per package, from the menu or `/U <ID>`, or all of them through the
Add/Remove Programs entry.

**98 SE and ME:**

1. The batch file prints the devices to remove first: Device Manager,
   each device of this package, Remove. A batch program cannot remove a
   devnode on 9x. Files deleted under a device that is still installed
   leave it failing at the next start, and the text says so.
2. The package's uninstall section runs, if it has one (for example
   `uvc98`'s registration of its codec in `system.ini`). It is run with
   `rundll.exe setupx.dll,InstallHinfSection <ID>.Remove 132 <ID>.INF`
   from the local copy.
3. Each file with a `WDMFREE.PLC` marker is deleted by the rules of
   section 8.2, and checked with `IF EXIST`. A file that will not go is
   left, with a note to restart and run the uninstall again.
4. A failed delete retains the affected marker, original byte reference
   and `PENDING.TXT`, so retrying after restart remains possible. Other
   markers and the package's local source are removed only once all
   required work is complete. If no package or incomplete operation
   remains, the installer removes its home and Add/Remove Programs entry.
5. A restart is asked for.

**2000 to 8**, through `WDMINST /REMOVE <ID>`:

1. Enumerate present and non-present devnodes. Immediately before
   `DIF_REMOVE`, require the current driver key's `InfPath` to name an
   `oemNN.inf` this package owns. `DEVICES.TXT` is a lookup aid, never
   authority to remove a device that was subsequently rebound. A missing
   or unreadable binding is left for explicit recovery.
2. Each `oemNN.inf` this installer created goes. That is
   `SetupUninstallOEMInfA` without `SUOI_FORCEDELETE` on XP and later,
   which refuses an INF a device still uses. On 2000, where that routine
   is not documented, the helper deletes `oemNN.inf` and `oemNN.pnf`
   itself, after the same check over every devnode's `InfPath`. A catalog
   staged with it (`usbflop`'s) stays registered on 2000; the summary
   says so, and M0 reads whether that leaves anything visible.
3. A package's own service is deleted only if `SERVICE.NEW` exists and
   no devnode, present or not, still names it. In-box services and files
   used through `Include`/`Needs` are always external, even if this
   install first created their service registration. In particular,
   `usbflop` never owns or deletes the USBSTOR service or binaries.
4. Owned files go by the rules of section 8.2, and only once steps 1 to
   3 have succeeded. Immediately before deleting a driver binary, the
   helper checks again that no devnode, present or not, and no service
   (this package's or another, through any INF) still names it. A
   remaining or unreadable reference keeps the file, its marker and its
   comparison source, with a note. A file in use is left
   with its marker, comparison source and pending-cleanup record. The
   summary asks for a restart and another uninstall attempt, which must
   recheck the file's current bytes and other claims before deleting it.
   V1 never queues an original shared destination for deletion with
   `MoveFileEx`: another installer could replace that path before reboot.
5. As 9x steps 4 and 5. No file under the Driver Store is removed
   directly by the installer.

Never removed, on any system: a file without this installer's `.PLC`
marker, an `oemNN.inf` or `INF\OTHER` copy it did not create, a device
whose INF copy is not its own, Microsoft's files (the Windows CD's, and
in-box drivers an INF-only package binds), and the user's settings in
the registry beyond what the package's own uninstall section names.

### 8.5 The Add/Remove Programs entry

One entry for the whole repository (Owner, 2026-10-20), written by
`InstallHinfSection DefaultInstall` of `ARP.INF` (the 16-bit engine on
9x, `setupapi` on NT, one INF for both), so no `.reg` quoting is needed:

```
HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\WDMFREE
  DisplayName     = "usb-wdm-free USB drivers"
  UninstallString = 9x: "%10%\COMMAND.COM /C %10%\WDMFREE\INSTALL.BAT /U"
                    NT: "%11%\cmd.exe /C %10%\WDMFREE\INSTALL.BAT /U"
  InstallLocation = %10%\WDMFREE
```

`REGSTR_PATH_UNINSTALL` is in the 98 DDK's `regstr.h` (line 83). Whether
98 SE's Add/Remove Programs runs a `COMMAND.COM /C` line correctly, and
in which directory, is read in M0. The uninstall reached that way changes
to `%windir%\WDMFREE` first: on 9x through the full path in the entry and
a `CD` the batch file does when its first argument is `/U`. `ARP.INF`'s
`Uninstall` section deletes the key.

## 9. Living beside other installers

Other installers may carry some of the same files. That includes vendor
packages, earlier hand installs, and installers of other projects that
ship these GPL drivers. Coexistence rests on section 8.2's rules, which
need no knowledge of the other installer:

- a file present before this installer looked is never overwritten and
  never deleted by it;
- a file this installer placed is not deleted while another installer's
  marker claims it;
- a package present but not installed by this installer is left alone.

Optional detection adds only better messages: the claim directory's
other markers are named in the menu and the uninstall summary. When there
are none, the installer behaves the same.

## 10. Per-package needs

Folders are each design's, in the one scheme of `README.md`, "Layout".

| Package | Systems, folders | Class, binding | What the installer must do beyond section 6 | Present-device policy |
|---|---|---|---|---|
| `uac2` (`uac2wdm.sys`, `uac2.md`) | 98 SE to 8: `w98se-me`, `w2k-xp`, `wv-8-x86`, `wxp-x64`, `wv-8-x64` | `MEDIA`, interface ids `USB\Class_01&SubClass_0x&Prot_20` | Windows CD note on 9x (KS, WDMAUD); on Vista-8 an unsigned package loses to `wdma_usb.inf`, so the user is offered the device | may replace `usbaudio.sys` on a UAC 2 function, user confirms |
| `usbmidi` (`usbmidi.md`) | 98 SE to 8: `w98se`, `wme`, `w2k`, `wxp`, `wv-8-x86`, `wxp-x64`, `wv-8-x64` | `MEDIA`; generic `USB\Class_01&SubClass_03` in `w98se` and `w2k` only, VID/PID lists in the others | Windows CD note on 9x. Unlisted AC+MS devices: the documented Have Disk steps (`usbmidi.md` section 4.3); the installer does not move them | may replace `usbaudio.sys` on a listed device only |
| `uvc98` (`uvc.md`) | 98 SE to 2000: `w98se-me`, `w2k` | `Image` | Its INF registers `uvcdec.dll` as `VIDC.MJPG`/`VIDC.YUY2` (`system.ini` `[drivers32]` on 9x, `Drivers32` on 2000). Owner, 2026-10-20: it never takes over an existing `VIDC.MJPG` registration. Package need: the INF writes each value only if absent, and the uninstall section removes only values naming `uvcdec.dll` | none (no in-box UVC driver on these systems) |
| `usbcom` (`serial.md`) | `w98se-me`: `USBCOM.SYS`, `USBCOM.VXD`, `USBCOM.INF`, `USBCOMP.INF`; `w2k-8`: `USBCDC.INF` only | 9x: `USB` class on the device, `Ports` on the child devnode | 9x: both INFs staged, so the child port installs silently after the parent. NT: INF-only, binds in-box `usbser.sys`; owns no binary, so the uninstall removes the staged INF and devices only; x64 install of an uncatalogued INF-only package is read in its M0 | none, except listed composite boards, bound as a whole device by VID/PID on Microsoft's 98 SE and 2000 stacks (and ME's only if testing shows ME does not split composites; `serial.md` section 5.2) |
| `cdcnet`, `rtl815x`, `ax88179` (`ethernet.md`) | NDIS 5 only. `cdcnet`: 98 SE to 7, `w98se-me`, `w2k-xp`, `wv-7-x86` (x86 NDIS 5); `wxp-x64`, `wv-7-x64` (x64 NDIS 5.1). `rtl815x`, `ax88179`: 98 SE, ME and 2000, `w98se-me`, `w2k` (x86 NDIS 5); on XP to 7 Realtek's and ASIX's own drivers serve those chips. Windows 8 is not a target | `Net` | 9x: network components, Windows CD and a restart at the device's install, said first. NT: `Net` class install through the helper as any other; on XP to 7 `cdcnet` also stages `CDCRNDIS.INF`, which binds Windows' own RNDIS driver and owns no binary (`ethernet.md` section 7.5). `rtl815x` and `ax88179` have no XP to 7 folder, so there is nothing of theirs to install there (`ethernet.md` section 1 item 8). When `rtl815x` is installed on 98 SE, ME or 2000, RTL8153 devices on `cdcnet` are offered the move (`ethernet.md` section 6.5) | `cdcnet` to `rtl815x` for the RTL8153 only, on 98 SE, ME and 2000, user confirms. Owner, 2026-10-20: on XP to 7 `cdcnet` keeps the RTL8153 ids as a free fallback, and never replaces Realtek's own driver on a device that already has it |
| `xpad98` (`gamepad.md`) | 98 SE to 2000: `w98se-me` (with the 360 receiver), `w2k` (without it: Morii's `x360wc` serves it there); XP and Vista, Xbox One and Series pads only: `wxp`, `wv-x86`, `wxp-x64`, `wv-x64` (Vista x64 once signing allows) | `HIDClass` | 9x: `hidclass.sys` from the Windows CD on a machine that never had a HID device. 98 SE to 2000: the INFs leave `x360c`'s 25 pad IDs out (`gamepad.md` section 5.1). XP and Vista: the INF lists only Xbox One and Series (GIP) ids, so 360-protocol pads stay with XBCD | none (no in-box driver for these pads on XP or Vista) |
| `usbflop` (`floppy.md`) | `w98se-me` (`USBFLOP.INF`, 9x); `w2k-8` (`USBFLOP.INF` and `USBFLOP.CAT`, x86 and x64) | INF only, binds `usbstor.sys` by class (the CBI and CB lines): ME's and 2000-8's in-box one; on 98 SE that of a USB storage driver set already installed | Owns no binary. On 98 SE it does nothing without a `usbstor.sys`, which this repository does not ship: the menu checks `IF EXIST %windir%\SYSTEM32\DRIVERS\USBSTOR.SYS` and says so, but stages it anyway on request | none; Microsoft's listed lines keep their drives |
| `usbprn` (`printing.md`) | 98 SE: `w98se` (`w98se-me` only if ME becomes a target) | `USB` class; `USBPRN.SYS`, port monitor `USBPRNMN.DLL` to `SYSTEM` | `USBPRN.INF`'s monitor `AddReg` and `RunOnce` (`AddMonitor`) run at the device's install, not at staging. Package need: an uninstall section that deletes the monitor's registration and port, which the installer runs before deleting `USBPRNMN.DLL` (the spooler holds it loaded, so the delete may need the restart). Low priority: built after the other drivers | none (98 SE has no printer class driver) |
| `libusb0` (`libusb.md`) | 98 SE, ME: `w98se-me` | `LibUsbDevices` class, VID/PID list | `LIBUSB0.DLL` and `LIBUSB0.SYS` keep upstream's names, so other software may have placed the same files: the claim rules of section 8.2 matter most here, and a `libusb0.dll` already present is never replaced. `TESTLIBU.EXE`, `INFWIZ.EXE` and the developer files are not installed | none; a device on another driver is left |
| User-mode tools (`xpadcfg.exe`, M4 of `gamepad.md`) | as the package | not a driver | not installed. They run from the download or the local copy | n/a |

## 11. Signing (x64 Vista, 7, 8)

Owner, 2026-10-20: signing as xHCI98 and Stor98 do it:

- the x64 drivers carry an embedded Authenticode signature by the
  project's own test certificate, applied at release, with no catalog
  and the INFs unchanged;
- the x86 driver images are not signed. INF-only packages may need a
  signed catalog as their package design specifies; there is no image
  of this repository's to embed-sign in that case.

What the installer does:

- **Test-signing mode.** `WDMIN64 /TESTSIGN` reads
  `HKLM\SYSTEM\CurrentControlSet\Control\SystemStartOptions` for the word
  `TESTSIGNING`. Present: "test-signing mode is on". Absent: "could not
  be confirmed", because the value is a reading and not a documented
  contract, and an F8 boot with enforcement disabled does not set it. It
  then points to `README.TXT`'s section: `bcdedit /set testsigning on` and
  a restart, or F8 "Disable Driver Signature Enforcement" at every start.
  On 8 it adds that test-signing mode needs Secure Boot off. Without it
  the device shows Code 52 on 7 and 8 and Code 39 on Vista. The user
  confirms, and the installer goes on. It never runs `bcdedit`.
- **The signature.** The helper may read the security-directory structure
  of this repository's own SYS files. A present certificate table is
  reported as "embedded signature data present, not verified": it does
  not prove a valid signature or identify its signer. The release gate
  verifies the actual signature and records the signer and file hash
  ([build.md](build.md)). A build without signature data is reported as
  unsigned and pointed to the F8 route. This inspection never reads
  Microsoft's or a vendor's driver binary.
- **The certificate** is not imported into any store. It is not needed
  for the embedded driver signature to load in test mode. This does not
  establish package or catalog trust during staging; that separate M0
  gate is in section 6.4, including `usbflop`'s INF-only catalog.
- **XP x64** does not enforce kernel signing. The unsigned-driver dialog
  is Windows' own.

## 12. Testing

### 12.1 Host-side

- **A lint over `INSTALL.BAT`** (`src/install/batlint`). It checks that
  every line the 9x half can reach uses none of the constructs of
  section 5.1's NT-only rows (`%~`, `%*`, `SET /P`, `FOR /F`, `IF /I`,
  `IF DEFINED`, `CALL :`, `SETLOCAL`, `2>`, parenthesised blocks). It also
  checks CRLF line endings, ASCII, lines of at most 79 columns in
  messages, and that every `IF ERRORLEVEL` chain is in descending order.
- **The release gate of section 4.1**: package scripts and staging INFs
  against each package INF.
- **The helper's pure parts** (record parsing, version comparison, plan
  per device), compiled for the host with recorded inputs.

### 12.2 The VM legs

Guests on the bench: QEMU 98 SE, ME, 2000 SP4, XP SP3, XP x64, Vista x86
and x64, 7 x86 and x64, 8 x86 and x64. A test package, an INF-only
package or the first real one, binding a QEMU device (`usb-serial`,
`usb-net`, `usb-audio`, `usb-kbd`), serves before the real packages
exist. Per system:

- **M0 readings** (section 15): `CHOICE`, `FIND`'s and `FC`'s errorlevels,
  the LF batch file, environment space, `InstallHinfSection` mode bits on
  9x, the `VNLP_IFEXISTS` flag, the driver database after staging,
  Add/Remove Programs running the entry, `REGEDIT /E` on a missing key.
- **Where it runs from**: Explorer double-click, a prompt in another
  folder, a CD image, a network share (UNC), a path with spaces (NT), Run
  as administrator (Vista+), a 32-bit `cmd.exe` on x64.
- **Install**:
  - with the device absent, then plugged: files found locally; record
    Windows CD requests and signing-consent prompts separately;
  - with the device present and driverless;
  - with it present on the in-box driver the package may replace;
  - with it present on a driver it may not replace: left.
- **Update**: an older release, then this one, with the device present
  and absent. A placed file changed by hand is left.
- **Uninstall**:
  - after each install;
  - with a second installer's marker present;
  - with a file in use (NT);
  - after a cancelled step.

  Compare the guest's file list and registry before and after. All owned
  payload and records must be cleaned up or explicitly retained for
  checked recovery. Record allowed Windows deltas separately: setup logs,
  caches and first-created in-box dependency services retained under the
  external-ownership rule. No unrelated device or externally owned file
  may be removed or overwritten.
- **Files present first**: a package's binaries placed by hand before the
  installer runs. The package shows as "present, not installed by this
  installer", and nothing is touched.
- **Recovery**: interrupt each phase of section 7.4, then rerun and
  uninstall. Include a locked file, a partial local copy, failure after
  the first INF of a two-INF package, cancellation of a signing dialog,
  and a recorded device rebound to another driver before removal.
  A failed deletion must retain enough records for a checked retry after
  restart; replace the file before retry and confirm the new bytes stay.
- **x64 staging**: test catalogless binary packages and the INF-only
  `usbflop` catalog separately from whether a SYS loads in test mode.

### 12.3 Real devices

Owner, 2026-10-20: one or two mainstream devices per class, on real
machines: one 98 SE and one XP or 7 machine. Each installs, updates and uninstalls on each
host stack the package names:

- Microsoft's in-box stacks;
- NUSB's EHCI stack on 98 SE (fully supported, Owner, 2026-10-20);
- any xHCI host controller driver installed for the bench.

The x64 legs run in test-signing mode and with F8.

## 13. Milestones

- **M0, readings.** A throwaway batch file and test INF settle section
  15's unverified rows on the systems each row names. Exit: the table
  of section 5.1 and the 9x staging route confirmed or corrected.
- **M1, 9x pure batch.** `INSTALL.BAT`'s 9x half, `ARP.INF`, the claim
  directory, the package-script and staging-INF generators with their
  gate, the lint. Stage, update and uninstall one package on 98 SE and
  ME. The first package that ships may be the serial or Ethernet one,
  whichever is first; the test package until then.
- **M2, the helper on 2000 to 8 x86.** `WDMINST.EXE` (`/OS`, `/ADMIN`,
  `/SAME`, `/STAGE`, `/DEVICES`, `/APPLY`, `/REMOVE`) and the
  NT half of `INSTALL.BAT`.
- **M3, x64.** `WDMIN64.EXE`, the WOW64 restart, `/TESTSIGN`. XP x64,
  Vista x64, 7 x64, 8 x64.
- **M4, every released package** in the table of section 10, with its
  real-device legs, as each package ships.
- **M5 (later, if needed), a 16-bit helper for 98 SE and ME** that installs on a
  present device through the 98 DDK's documented `SETUPX` device-installer
  routines (`DiCreateDeviceInfo`, `DiBuildCompatDrvList`,
  `DiCallClassInstaller` with `DIF_INSTALLDEVICE`, `DI_ENUMSINGLEINF`;
  `setupx.h`), replacing the printed steps. Open Watcom's Win16 target.
  Owner, 2026-10-20: printed steps in v1; this helper only if they prove
  not enough.

## 14. Owner decisions and open questions

Owner, 2026-10-20, on the questions this draft raised:

- **The helper**: `WDMINST.EXE` and `WDMIN64.EXE`, GPL-2.0, are accepted
  on 2000 to 8, behind `INSTALL.BAT`; 98 SE and ME stay pure batch
  (sections 3 and 7).
- **Claim directory**: the shared `%windir%\USBCLAIM\` scheme with its
  `.PLC` and `.USE` markers (section 8.2) is accepted, for
  usb-wdm-nonfree's installer and others to use as well.
- **Present devices on 98 SE and ME**: printed steps in v1; a 16-bit
  SETUPX helper later, if needed (section 6.6, M5).
- **Add/Remove Programs**: one entry for the repository (section 8.5).
- **`uvc98`'s codec registration**: the package never takes over an
  existing `VIDC.MJPG` registration (section 10).
- **Windows 8.1 and later**: warn, and let the user go on (section 5.2).
- **A package installed by something else**: reported in the menu and
  never replaced (section 8.2, rule 5).
- **Independence**: this installer stands on its own, apart from
  xHCI98's and from usb-wdm-nonfree's (section 2).
- **Signing**: as xHCI98 and Stor98 (section 11).
- **Folders**: the one scheme of `README.md`, "Layout" (section 10).
- **Test hardware**: one or two mainstream devices per class; NUSB's
  EHCI stack on 98 SE is fully supported and in the test legs (section
  12.3).

### Open questions

None at present. The unverified readings of section 15 are settled in M0.

## 15. Unverified, and what settles it

Each is read in M0 on the systems named.

| Item | Systems | Reading |
|---|---|---|
| `CHOICE` present, `/C:` syntax, errorlevel by position | 98 SE, ME | run it |
| `CHOICE` absent | 2000, XP | `where`/`dir` on a clean install |
| `FIND` and `FC` set errorlevels | 98 SE, ME | run them on a match and a miss |
| `FC /B` distinguishes equal, different and unreadable files | 98 SE, ME | include missing, locked and partially copied files; release blocked if no dependable pure-batch comparison is available |
| LF-only batch files lose `GOTO` labels | 98 SE | run an LF copy |
| `COMMAND /E:4096` re-run on environment exhaustion | 98 SE | fill the environment |
| `InstallHinfSection` mode `132` honoured by the 16-bit engine | 98 SE, ME | a test INF, with and without a needed restart |
| Copy flag `0x10` reaches the engine as `VNLP_IFEXISTS` | 98 SE, ME | a test INF over a present file |
| A device plugged after staging finds the INF before and after a restart; whether `DRVIDX.BIN` needs deleting | 98 SE, ME | the test package |
| No disk prompt at plug-time for files already at their destination | 98 SE, ME | the test package |
| Add/Remove Programs runs a `COMMAND.COM /C` line | 98 SE, ME | the entry |
| `REGEDIT /E` on a missing key: silent, no file | 98 SE, ME | run it |
| `SetupUninstallOEMInfA` exported | 2000 | `GetProcAddress` |
| `CopyINF` in `DefaultInstall` stages on XP (recorded, not used) | XP | a test INF |
| `Sysnative` absent on XP x64 without the update | XP x64 | `dir` |
| Catalogless SYS package staging versus embedded-signature loading; INF-only catalog trust | Vista, 7, 8 x64 | call the staging API and record Windows' prompts/errors, then separately test device install and load under test mode and F8 |
| Immutable source generations survive updates and interrupted commits | 2000, XP | stage old and new generations, leave a device absent through the update, then reattach it; interrupt pointer/version commit and confirm recovery preserves all referenced sources |
| `SetupCopyOEMInf` and `SP_COPY_NOOVERWRITE` behaviour on 2000 | 2000 SP4 | use 2000 kit documentation/header, then exercise new, identical and changed INFs; current Learn minimum is XP |

## 16. Sources

- Windows 98 DDK:
  - `INC_WIN98_INC16_SETUPX.H`: `LDID_*` lines 116-166, the `VNLP_*` copy
    flags lines 733-743, the device-installer prototypes;
  - `LIB_WIN98_SETUPX.LIB`: the `InstallHinfSection` export name;
  - `INC_WIN98_REGSTR.H`: `REGSTR_PATH_INF_OEMNAME` line 64,
    `REGSTR_PATH_UNINSTALL` line 83, `REGSTR_VAL_DEVICEPATH` line 231;
  - `HELP_OTHER.CHM`, `ndismini_01tc.htm`: an INF copy into
    `Windows\Inf` and the driver database.
- Windows 98 SE in-box `msmouse.inf` line 529: the
  `rundll.exe setupx.dll,InstallHinfSection` form.
- NUSB 3.3 `_NUSB.INF` (third-party, read as an example of practice, not
  copied): `[DestinationDirs]` line 19 (INFs to 17); its uninstall entry,
  lines 129-130.
- Microsoft Learn: "SetupCopyOEMInf", "SetupUninstallOEMInf",
  "UpdateDriverForPlugAndPlayDevices", "PnPUtil" (Vista to 8 syntax),
  "INF DefaultInstall Section", "How Windows Ranks Driver Packages",
  "InstallHinfSection", "Device installation on 64-bit systems"
  (`ERROR_IN_WOW64`), "File System Redirector" (`Sysnative`).
- API contracts checked for the execution plan:
  [SetupCopyOEMInfA](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupcopyoeminfa)
  (`SP_COPY_NOOVERWRITE`, source-path records; current page lists XP as
  minimum, so 2000 is a historical-kit and M0 gate),
  [UpdateDriverForPlugAndPlayDevicesA](https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-updatedriverforplugandplaydevicesa)
  (distribution-source INF path and hardware-id-wide scope), and
  [SetupUninstallOEMInfA](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupuninstalloeminfa)
  (includes non-present devices in its usage check).
- [Driver Store](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/driver-store):
  staging verifies the whole package; stored files must not be edited
  directly. [Enable loading of test-signed drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option):
  test-mode kernel loading does not require the signing certificate to
  chain to a trusted root. Package staging is a distinct question.
- MS-DOS 6.22 `HELP` (`CHOICE`, `FIND`, `IF`, `FOR`, `COMMAND /E`);
  Microsoft's command-line references for Windows 2000, XP and Server
  2003.
- This repository's designs: `uac2.md` sections 6-7, `usbmidi.md`
  sections 4 and 8, `uvc.md` section 8, `serial.md` sections 4, 5.1.2 and
  8, `ethernet.md` section 7 (NDIS 5 only), `gamepad.md` sections
  7-8, `floppy.md` sections 4 and 7, `printing.md` section 10,
  `libusb.md` section 6.

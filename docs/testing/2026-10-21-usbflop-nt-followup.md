# USB floppy: reboot, XP and Windows 7 follow-up, 2026-10-21

Development experiments only; no release or media-write claim. The physical
drive is Mitsumi `03EE:6901`, without a medium. QEMU forwards it through
UHCI using `usb-host`; the host has WinUSB from Zadig. All owned guests
are headless, monitors bind localhost, and overlays, virtual floppies,
process TEMP/TMP and QEMU logs are on `E:\usb-wdm-free\wave1`.

## 98 SE clean boot resolves immediate error

The existing `w98flop.qcow2` overlay from
[the earlier run](2026-10-21-usbflop-more.md) was started with the same
Pentium III, 256 MB, standard VGA, default ACPI and project CBI binding.
The earlier unspecified problem occurred immediately after a live
Update Driver operation, without a reboot request.

After this clean boot the **USB UFI floppy (CBI, usb-wdm-free experiment)**
parent reports **This device is working properly**. Its **USB Floppy Disk**
child independently reports the same. Evidence is in ignored
`out/storage/usbflop/98-reboot-status.png` and `98-reboot-child.png`.
This settles the previous immediate-status failure as requiring a boot
before claiming successful operation; no source change was needed.

An idle monitor detach/replug caused new hardware detection and selected
the native **Mitsumi USB Floppy** instead of retaining the manually chosen
class model. That native parent worked properly too. See
`98-replug-tree.png` and `98-replug-native.png`. This is consistent with
the listed VID/PID candidate winning fresh enumeration; an actually
unlisted device remains untested. The guest then shut down cleanly and
its QEMU process exited.

## XP SP3 x86 manual class binding

New overlay `wxpflop.qcow2` backs onto pristine `vm/wxp.qcow2`.
QEMU x86_64 used `-machine pc -accel whpx,kernel-irqchip=off -cpu pentium3
-smp 4 -m 512 -vga std -nic none -usb`. The physical drive initially
installed its native Microsoft Mitsumi entry and reported working.

Update Driver / no Windows Update / specific location / don't search /
Have Disk accepted `A:\USBFLOP.INF` and the sole compatible CBI model.
The ordinary unsigned-package Windows Logo warning was acknowledged with
Continue Anyway. No enforcement configuration was changed. Installation
completed without a source-file or reboot request. The parent reports
working, provider usb-wdm-free, version 0.0.0.1, date 21-Oct-26, unsigned.
The **MITSUMI USB FDD USB Device** child under Floppy disk drives also
reports working. Screenshots: `xp-native.png`, `xp-ours-driver.png`,
`xp-ours-status.png`, `xp-child.png` in ignored `out/storage/usbflop/`.

Exact staged NT INF SHA-256:
`8D4B63240B6CC04FF54BDC982A45300936F06E9FF61928DA700FB49E97EC9D42`.
SetupAPI text was exported through guest Notepad Save As to virtual
`A:\XPSETUP.LOG` (host `wxpflop-a\xpsetup.log`), without inspecting driver
binary contents. It records the class match, actual `UFI_CBI.NT` section,
unsigned rank `0x0000a000`, effective date 10/21/2026, and successful full
device installation. This differs from the older Windows 2000 unsigned
date behavior recorded in its own run log.

Shutdown initially waited for Device Manager's open property dialog;
the shutdown End Program prompt closed MMC and Windows then powered off.
The QEMU process exited; the exported log was preserved on E:.

## Windows 7 x64

New overlay `w7flop.qcow2` backs onto pristine `vm/w764.qcow2`.
QEMU x86_64 used `-machine pc -accel tcg,thread=multi -cpu qemu64
-smp 4 -m 2048 -vga std -nic none -usb`. Its existing boot configuration
always presents Advanced Boot Options. The initial End key did not move
selection and entered Recovery; Recovery was exited without running any
repair. On the next menu, Up wrapped selection to **Start Windows Normally**
and that observed selection was accepted (`w7-normalboot.png`).
Disable Driver Signature Enforcement was not selected.

After XP released the physical drive, the Windows 7 USB forwarding device
was removed/re-added to obtain the actual 12 Mb/s Mitsumi rather than its
unclaimed USB Host Device placeholder. The native Microsoft Mitsumi entry
installed automatically and reported working (`w7-native.png`).

Update Driver / browse / let me pick / Have Disk loaded the same staged NT
INF and compatible CBI model. Windows Security's ordinary unsigned
publisher prompt was acknowledged with **Install this driver software
anyway**. Installation completed successfully without a source-file or
reboot request. Parent status is working; provider usb-wdm-free, version
0.0.0.1, date 21/10/2026, not digitally signed. Its **MITSUMI USB FDD USB
Device** child reports working, with location explicitly on the project's
CBI parent. Evidence: `w7-installed.png`, `w7-ours-driver.png`,
`w7-ours-status.png`, `w7-child.png` in ignored `out/storage/usbflop/`.

An elevated command prompt was used only to read/export `bcdedit /enum
all` and copy SetupAPI's text log. The exported `w7flop-a\bcdall.txt`
contains neither `testsigning` nor `nointegritychecks` in the current
loader or its inherited settings. No BCD value was changed. Thus this
INF-only install succeeds through the ordinary package-warning path
while retaining normal boot policy; it does not establish acceptance of
any unsigned kernel binary.

`w7flop-a\w7setup.log` records architecture amd64, import as `oem2.inf`,
the missing catalog accepted by the user, selection of `Models.NTamd64`
and `UFI_CBI.NT`, and execution of the included system
`USBSTOR_CBI.NT` / `.Services` sections. These observations are installation
log/INF facts, not driver-binary inspection. The guest's RTC displays
2026-10-20 while the host's Singapore run date is 2026-10-21.

A subsequent clean reboot again used the observed **Start Windows
Normally** selection. The project CBI parent and Mitsumi floppy child
both still report working (`w7-reboot-normal.png`, `w7-reboot-status.png`,
`w7-reboot-child.png`). No Code 52 appeared. The existing activation
reminder was dismissed without activation or configuration changes.

All media content operations, unlisted-drive matching, CB transport and
physical controller tests remain outside this run. No driver source or
release files were changed for these follow-up tests. XP was not rebooted
after installation. Windows 7 was then shut down through Windows; owned
QEMU process cleanup is recorded below.

Final inventory confirmed owned PIDs 38032 (98 SE), 41044 (XP) and 33468
(7 x64) had all exited. Other agents' headless guests were left alone.
Physical floppy forwarding is released. Scratch and text evidence remain
on E:; screenshots remain in ignored `out/`. `git diff --check` passed.

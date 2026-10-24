# USB experiment helpers

These are development tools, built into ignored `out/`; they are not a
released driver package. They inspect documented USB/PnP interfaces only.

`hbdesc.cmd` builds `HUBDESC.EXE`, a standard-descriptor reader through USB
hub IOCTLs that works while a device retains its native function driver.
Without arguments its historical AX88179 mode saves AXDEV.BIN/AXCFG.BIN
beside the EXE and shows a result dialog. With one explicit `VID:PID`
argument it runs without a result dialog and writes DEVICE.BIN, CONFIG.BIN
and HUBDESC.TXT beside the EXE. Use a separate output directory per device.
It refuses ambiguous multiple matches and never requests strings, class
or vendor controls, resets or configuration changes. Configuration data
is bounded to4096 bytes. Neither this helper nor native PnP metadata alone
establishes working guest transport or video/audio performance.

`pnp.cmd` builds `PNPSTAT.EXE` with the repository-local MSVC 6 toolchain.
Run it in a guest with a writable A: exchange disk. It writes
`A:\PNPSTAT.TXT` with present synthetic VID FFFF devices' hardware ID,
description, service, class driver key, and Configuration Manager status
and problem number. It does not install, remove or change devices, and
does not inspect any driver binary. The helper intentionally ignores real
devices and does not need administrator rights for this read-only check.

Contracts: [SetupDiGetDeviceRegistryProperty](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdigetdeviceregistrypropertya),
[CM_Get_DevNode_Status](https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_get_devnode_status).

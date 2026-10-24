# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
#
# Host tests of the pad simulator (xpademu.py): builds the driver's own
# portable cores (xusb.c, gip.c, xpcfg.c, devtab.c, report.c) as an
# import-free x64 DLL with WDK 7.1's compiler, so that xpsimtest.py checks
# the simulator's descriptors and packets with the parsers the driver
# runs. No guest, no USB device. Output: out\hid\xpad98\simhost\.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$dest = Join-Path $repo 'out/hid/xpad98/simhost'
New-Item -ItemType Directory -Force $dest | Out-Null
$bin = Join-Path $repo 'tools/WinDDK71/bin/x86/amd64'
if (!(Test-Path -LiteralPath "$bin/cl.exe")) { throw 'Install WDK 7.1 under tools/WinDDK71 first.' }
$exports = 'XpX360Parse','XpX360wParse','XpGipParse','XpGipNextInit','XpParseConfig','XpX360Led',
           'XpX360wLed','XpX360wPresence','XpLedForPlayer','XpFindDevice','XpPadReset'
Push-Location $dest
try {
    $src = 'xusb.c','gip.c','devtab.c','xpcfg.c','report.c' | ForEach-Object { Join-Path $PSScriptRoot $_ }
    & "$bin/cl.exe" /nologo /c /O2 /W3 /WX /GS- /Zl @src > build.log 2>&1
    if ($LASTEXITCODE) { Get-Content build.log; throw 'core compile failed' }
    $objs = 'xusb.obj','gip.obj','devtab.obj','xpcfg.obj','report.obj'
    & "$bin/link.exe" /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /OUT:xpcore.dll ($exports | ForEach-Object { "/EXPORT:$_" }) @objs >> build.log 2>&1
    if ($LASTEXITCODE) { Get-Content build.log; throw 'core DLL link failed' }
} finally { Pop-Location }
$env:XPCORE_DLL = Join-Path $dest 'xpcore.dll'
# unittest reports on stderr; cmd keeps PowerShell 5.1 from turning it into errors.
$log = Join-Path $dest 'test.log'
cmd /c "python -I `"$(Join-Path $PSScriptRoot 'xpsimtest.py')`" > `"$log`" 2>&1"
$rc = $LASTEXITCODE
Get-Content $log
if ($rc) { throw 'pad simulator host tests failed' }

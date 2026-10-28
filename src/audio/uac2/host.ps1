# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Compile the driver's own descriptor core (uacdesc.c) into an x64 DLL for
# the Python host tests of uacemu.py's profiles (uacsimtest.py). No kernel
# I/O; never part of a driver.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$dest = Join-Path $repo 'out/audio/uac2/hostabi'
New-Item -ItemType Directory -Force $dest | Out-Null
$bin = Join-Path $repo 'tools/WinDDK71/bin/x86/amd64'
Push-Location $dest
try {
    & "$bin/cl.exe" /nologo /c /O2 /W3 /WX /GS- /Zl /Fouacdesc.obj "$PSScriptRoot/uacdesc.c"
    if ($LASTEXITCODE) { throw 'uacdesc compile failed' }
    & "$bin/cl.exe" /nologo /c /O2 /W3 /WX /GS- /Zl /Fouacplan.obj "$PSScriptRoot/uacplan.c"
    if ($LASTEXITCODE) { throw 'uacplan compile failed' }
    & "$bin/cl.exe" /nologo /c /O2 /W3 /WX /GS- /Zl /Fouacvol.obj "$PSScriptRoot/uacvol.c"
    if ($LASTEXITCODE) { throw 'uacvol compile failed' }
    & "$bin/cl.exe" /nologo /c /O2 /W3 /WX /GS- /Zl /Fouacctl.obj "$PSScriptRoot/uacctl.c"
    if ($LASTEXITCODE) { throw 'uacctl compile failed' }
    & "$bin/link.exe" /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /OUT:uaccore.dll `
        /EXPORT:UacParse /EXPORT:UacFunction /EXPORT:UacWholeFirst /EXPORT:UacClock /EXPORT:UacClockPath `
        /EXPORT:UacPlanFormats /EXPORT:UacPlanPick /EXPORT:UacPlanTerminal /EXPORT:UacPlanPacketBytes /EXPORT:UacPlanChannelMask /EXPORT:UacVolFind /EXPORT:UacVolRange uacdesc.obj uacplan.obj uacvol.obj uacctl.obj "$repo/tools/WinDDK71/lib/Crt/amd64/msvcrt.lib"
    if ($LASTEXITCODE) { throw 'host DLL link failed' }
} finally { Pop-Location }

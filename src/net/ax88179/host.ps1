# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Host-only ABI for the package's portable control planner.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$dest=Join-Path $repo 'out/net/ax88179/hostabi'
New-Item -ItemType Directory -Force $dest | Out-Null
Push-Location $dest
try {
    & "$repo/tools/WinDDK71/bin/x86/amd64/cl.exe" /nologo /c /O2 /Oi /W3 /WX /GS- /Zl /Foaxctrl.obj "$PSScriptRoot/axctrl.c"
    if($LASTEXITCODE) { throw 'control planner compile failed' }
    & "$repo/tools/WinDDK71/bin/x86/amd64/cl.exe" /nologo /c /O2 /Oi /W3 /WX /GS- /Zl /Foaxframe.obj "$PSScriptRoot/axframe.c"
    if($LASTEXITCODE) { throw 'framing core compile failed' }
    & "$repo/tools/WinDDK71/bin/x86/amd64/link.exe" /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /OUT:axcore.dll /EXPORT:AxCtrlDescribe /EXPORT:AxCtrlReadPlan /EXPORT:AxCtrlResponse /EXPORT:AxCtrlMac /EXPORT:AxCtrlPhy /EXPORT:AxCtrlPowerPrefix /EXPORT:AxCtrlLinkPlan /EXPORT:AxFrameReceive /EXPORT:AxFrameReceiveFcs /EXPORT:AxFrameTransmit axctrl.obj axframe.obj "$repo/tools/WinDDK71/lib/Crt/amd64/msvcrt.lib"
    if($LASTEXITCODE) { throw 'host ABI link failed' }
} finally { Pop-Location }

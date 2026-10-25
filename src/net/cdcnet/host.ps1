# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Compile the driver's own descriptor and NCM cores (cdcdesc.c, cdcncm.c) into an x64 DLL
# for the Python host tests of ncmemu.py (ncmsimtest.py). No kernel I/O;
# never part of a driver.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$dest = Join-Path $repo 'out/net/cdcnet/hostabi'
New-Item -ItemType Directory -Force $dest | Out-Null
$bin = Join-Path $repo 'tools/WinDDK71/bin/x86/amd64'
Push-Location $dest
try {
    foreach ($name in 'cdcdesc', 'cdcncm') {
        & "$bin/cl.exe" /nologo /c /O2 /W3 /WX /GS- /Zl "/Fo$name.obj" "$PSScriptRoot/$name.c"
        if ($LASTEXITCODE) { throw "$name compile failed" }
    }
    & "$bin/link.exe" /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /OUT:cdccore.dll `
        /EXPORT:CdcProbeConfig /EXPORT:CdcParseMacString /EXPORT:CdcNcmParams /EXPORT:CdcNcmRxParse /EXPORT:CdcNcmTxPlan /EXPORT:CdcNcmTxFinish cdcdesc.obj cdcncm.obj "$repo/tools/WinDDK71/lib/Crt/amd64/msvcrt.lib"
    if ($LASTEXITCODE) { throw 'host DLL link failed' }
} finally { Pop-Location }

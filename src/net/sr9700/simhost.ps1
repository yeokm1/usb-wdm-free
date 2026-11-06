# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Compile the driver's own chip core (srchip.c, netchip.c) into an x64 DLL
# for the Python host tests of sremu.py (sr9simtest.py). No kernel
# I/O; never part of a driver.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$dest = Join-Path $repo 'out/net/sr9700/hostabi'
New-Item -ItemType Directory -Force $dest | Out-Null
$bin = Join-Path $repo 'tools/WinDDK71/bin/x86/amd64'
Push-Location $dest
try {
    foreach ($name in 'srchip', 'netchip') {
        & "$bin/cl.exe" /nologo /c /O2 /W3 /WX /GS- /Zl "/Fo$name.obj" "$PSScriptRoot/$name.c"
        if ($LASTEXITCODE) { throw "$name compile failed" }
    }
    & "$bin/link.exe" /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /OUT:srchipcore.dll `
        /EXPORT:NetPackageChip,DATA srchip.obj netchip.obj "$repo/tools/WinDDK71/lib/Crt/amd64/msvcrt.lib"
    if ($LASTEXITCODE) { throw 'host DLL link failed' }
} finally { Pop-Location }

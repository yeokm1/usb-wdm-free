# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Compile the driver's own chip core (ax7chip.c, netchip.c) into an x64 DLL
# for the Python host tests of ax7emu.py (ax7simtest.py), with this
# package's chip state (NET_CHIP_STATE 2048). No kernel I/O; never part of
# a driver.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$dest = Join-Path $repo 'out/net/ax88772/hostabi'
New-Item -ItemType Directory -Force $dest | Out-Null
$bin = Join-Path $repo 'tools/WinDDK71/bin/x86/amd64'
Push-Location $dest
try {
    foreach ($name in 'ax7chip', 'netchip') {
        & "$bin/cl.exe" /nologo /c /O2 /W3 /WX /GS- /Zl /DNET_CHIP_STATE=2048 "/Fo$name.obj" "$PSScriptRoot/$name.c"
        if ($LASTEXITCODE) { throw "$name compile failed" }
    }
    & "$bin/link.exe" /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /OUT:ax7chipcore.dll `
        /EXPORT:NetPackageChip,DATA ax7chip.obj netchip.obj "$repo/tools/WinDDK71/lib/Crt/amd64/msvcrt.lib"
    if ($LASTEXITCODE) { throw 'host DLL link failed' }
} finally { Pop-Location }

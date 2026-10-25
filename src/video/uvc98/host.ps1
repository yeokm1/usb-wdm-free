# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Compile owned portable C for Python host integration tests, no kernel I/O.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$dest = Join-Path $repo 'out/video/uvc98/hostabi'
New-Item -ItemType Directory -Force $dest | Out-Null
$compiler = Join-Path $repo 'tools/WinDDK71/bin/x86/amd64/cl.exe'
Push-Location $dest
try {
    foreach ($name in 'uvcpay','uvcctrl','uvcdesc','uvcneg','hostabi') {
        & $compiler /nologo /c /O2 /Oi /W3 /WX /GS- /Zl "/Fo$name.obj" "$PSScriptRoot/$name.c"
        if ($LASTEXITCODE) { throw "$name compile failed" }
    }
    & "$repo/tools/WinDDK71/bin/x86/amd64/link.exe" /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /OUT:uvccore.dll /EXPORT:UvcPayInit /EXPORT:UvcPayFeed /EXPORT:UvcPayLost /EXPORT:UvcDescParse /EXPORT:UvcNegotiateLimited /EXPORT:UvcTestPaySize /EXPORT:UvcTestDescSize uvcpay.obj uvcctrl.obj uvcdesc.obj uvcneg.obj hostabi.obj "$repo/tools/WinDDK71/lib/Crt/amd64/msvcrt.lib"
    if ($LASTEXITCODE) { throw 'Host DLL link failed' }
} finally { Pop-Location }

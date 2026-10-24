# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Build the original C codec as an import-free x64 host-test DLL.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$dest = Join-Path $repo 'out/audio/usbmidi/host'
New-Item -ItemType Directory -Force $dest | Out-Null
$compiler = Join-Path $repo 'tools/WinDDK71/bin/x86/amd64/cl.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw 'Install WDK 7.1 under tools/WinDDK71 first.' }
Push-Location $dest
try {
    & $compiler /nologo /c /O2 /W3 /WX /GS- /Zl /Foumpkt.obj "$PSScriptRoot/umpkt.c"
    if ($LASTEXITCODE) { throw 'Codec compile failed' }
    & "$repo/tools/WinDDK71/bin/x86/amd64/link.exe" /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /OUT:umcodec.dll /EXPORT:UmEncoderInit /EXPORT:UmEncodeByte /EXPORT:UmDecodePacket umpkt.obj
    if ($LASTEXITCODE) { throw 'Codec DLL link failed' }
    # The driver's descriptor walk (msdesc.c), for midisimtest.py's profile checks.
    & $compiler /nologo /c /O2 /W3 /WX /GS- /Zl "/I$repo/tools/WinDDK71/inc/crt" "/I$repo/tools/WinDDK71/inc/api" /Fomsdesc.obj "$PSScriptRoot/msdesc.c"
    if ($LASTEXITCODE) { throw 'msdesc compile failed' }
    & "$repo/tools/WinDDK71/bin/x86/amd64/link.exe" /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /OUT:umdesc.dll /EXPORT:UmFindInterface /EXPORT:UmMapPorts msdesc.obj "$repo/tools/WinDDK71/lib/Crt/amd64/msvcrt.lib"
    if ($LASTEXITCODE) { throw 'msdesc DLL link failed' }
} finally { Pop-Location }

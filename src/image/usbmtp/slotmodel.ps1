# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Builds sys\slot.c (USBMTP.SYS's slot transitions, the driver's own file)
# with slotmodel.c as an x86 console program using the repository's MSVC
# 6.0, the driver's compiler, and runs the exhaustive model check
# (docs/design/mtp.md 5.4). Output: out\image\usbmtp\model\.
#   -Defs  compile switches for the model (a seeded fault: slotmut.ps1)
#   -Slot  another copy of slot.c to build instead (a broken copy: slotmut.ps1)
#   -Out   the output folder (default out\image\usbmtp\model)
#   -Only  run only the scenarios whose name holds this text
# The program is linked large-address-aware: its state store is allocated
# in blocks as it grows, past 2 GB for the largest scenario.
param([string]$Defs = '', [string]$Slot = '', [string]$Out = '', [string]$Only = '')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$vc = Join-Path $root 'tools\MSVC600\VC98'
foreach ($need in @("$vc\BIN\CL.EXE", "$root\tools\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL")) {
    if (-not (Test-Path -LiteralPath $need)) { throw "MISSING: $need - install MSVC 6.0 as docs\toolchain.md describes" }
}
$out = if ($Out) { $Out } else { Join-Path $root 'out\image\usbmtp\model' }
$slotSrc = if ($Slot) { $Slot } else { "$PSScriptRoot\sys\slot.c" }
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$saved = @{ PATH = $env:PATH; INCLUDE = $env:INCLUDE; LIB = $env:LIB; CL = $env:CL; LINK = $env:LINK }
try {
    $env:PATH = "$vc\BIN;$root\tools\MSVC600\Common\MSDev98\Bin;$env:SystemRoot\system32"
    $env:INCLUDE = "$vc\Include;$PSScriptRoot\sys"; $env:LIB = "$vc\Lib"; $env:CL = $null; $env:LINK = $null
    Push-Location $out
    try {
        # The driver's file is held to /W4; the model to /W3.
        & cl.exe /nologo /c /W4 /WX /O2 /Foslot.obj "$slotSrc" *>> build.log
        if ($LASTEXITCODE) { Get-Content build.log; throw 'slot.c build failed' }
        $d = @(); if ($Defs) { $d = @($Defs -split ' ' | Where-Object { $_ } | ForEach-Object { "/D$_" }) }
        & cl.exe /nologo /W3 /WX /O2 @d /I"$PSScriptRoot" /Feslotmodel.exe "$PSScriptRoot\slotmodel.c" slot.obj /link /LARGEADDRESSAWARE *>> build.log
        if ($LASTEXITCODE) { Get-Content build.log; throw 'slotmodel build failed' }
        if ($Only) { & .\slotmodel.exe $Only | Tee-Object -FilePath result.txt } else { & .\slotmodel.exe | Tee-Object -FilePath result.txt }
        if ($LASTEXITCODE) { throw 'slot model check failed' }
        'PASS usbmtp slot model' | Set-Content -Encoding ASCII PASS.TXT
    } finally { Pop-Location }
} finally { foreach ($k in $saved.Keys) { Set-Item "env:$k" $saved[$k] } }

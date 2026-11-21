# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Builds sys\cfgwalk.c (USBMTP.SYS's configuration descriptor walk, the
# driver's own file) with cfgtest.c as an x86 console program using the
# repository's MSVC 6.0, and runs it (docs/design/mtp.md 3.3). Output:
# out\image\usbmtp\cfgtest\.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$vc = Join-Path $root 'tools\MSVC600\VC98'
foreach ($need in @("$vc\BIN\CL.EXE", "$root\tools\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL")) {
    if (-not (Test-Path -LiteralPath $need)) { throw "MISSING: $need - install MSVC 6.0 as docs\toolchain.md describes" }
}
$out = Join-Path $root 'out\image\usbmtp\cfgtest'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$saved = @{ PATH = $env:PATH; INCLUDE = $env:INCLUDE; LIB = $env:LIB; CL = $env:CL; LINK = $env:LINK }
try {
    $env:PATH = "$vc\BIN;$root\tools\MSVC600\Common\MSDev98\Bin;$env:SystemRoot\system32"
    $env:INCLUDE = "$vc\Include"; $env:LIB = "$vc\Lib"; $env:CL = $null; $env:LINK = $null
    Push-Location $out
    try {
        # The driver's file on its own, at /W4; the test compiles it in, with
        # its byte accessor bounds-checked.
        & cl.exe /nologo /c /W4 /WX /O2 /Focfgwalk.obj "$PSScriptRoot\sys\cfgwalk.c" *>> build.log
        if ($LASTEXITCODE) { Get-Content build.log; throw 'cfgwalk.c build failed' }
        & cl.exe /nologo /W3 /WX /O2 /I"$PSScriptRoot" /Fecfgtest.exe "$PSScriptRoot\cfgtest.c" *>> build.log
        if ($LASTEXITCODE) { Get-Content build.log; throw 'cfgtest build failed' }
        & .\cfgtest.exe | Tee-Object -FilePath result.txt
        if ($LASTEXITCODE) { throw 'cfgwalk host test failed' }
    } finally { Pop-Location }
} finally { foreach ($k in $saved.Keys) { Set-Item "env:$k" $saved[$k] } }

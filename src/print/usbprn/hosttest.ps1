# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Builds prncore.c with hosttest.c as an x86 console test using the
# repository's MSVC 6.0 (the compiler of USBPRN.SYS and USBPRNMN.DLL) and
# runs it. Output: out\print\usbprn\host\.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$vc = Join-Path $root 'tools\MSVC600\VC98'
foreach ($need in @("$vc\BIN\CL.EXE", "$root\tools\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL")) {
    if (-not (Test-Path -LiteralPath $need)) { throw "MISSING: $need - install MSVC 6.0 as docs\toolchain.md describes" }
}
$out = Join-Path $root 'out\print\usbprn\host'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$saved = @{ PATH = $env:PATH; INCLUDE = $env:INCLUDE; LIB = $env:LIB; CL = $env:CL; LINK = $env:LINK }
try {
    $env:PATH = "$vc\BIN;$root\tools\MSVC600\Common\MSDev98\Bin;$env:SystemRoot\system32"
    $env:INCLUDE = "$vc\Include"; $env:LIB = "$vc\Lib"; $env:CL = $null; $env:LINK = $null
    Push-Location $out
    try {
        # The core is held to /W4; the harness to /W3.
        & cl.exe /nologo /c /W4 /WX /O2 /Foprncore.obj "$PSScriptRoot\prncore.c" *>> build.log
        if ($LASTEXITCODE) { Get-Content build.log; throw 'prncore build failed' }
        & cl.exe /nologo /W3 /WX /O2 /Fehosttest.exe "$PSScriptRoot\hosttest.c" prncore.obj *>> build.log
        if ($LASTEXITCODE) { Get-Content build.log; throw 'host test build failed' }
        & .\hosttest.exe | Tee-Object -FilePath result.txt
        if ($LASTEXITCODE) { throw 'host tests failed' }
        'PASS usbprn host tests' | Set-Content -Encoding ASCII PASS.TXT
    } finally { Pop-Location }
} finally { foreach ($k in $saved.Keys) { Set-Item "env:$k" $saved[$k] } }

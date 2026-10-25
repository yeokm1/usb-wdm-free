# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Builds tests\lutest.c with the DLL's own src\descriptors.c and
# src\error.c as an x86 console test, using the repository's MSVC 6.0
# (the DLL's compiler and CRT), and runs it. It touches no device, driver
# or USB stack. Output: out\generic\libusb0\host\.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$vc = Join-Path $root 'tools\MSVC600\VC98'
foreach ($need in @("$vc\BIN\CL.EXE", "$root\tools\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL")) {
    if (-not (Test-Path -LiteralPath $need)) { throw "MISSING: $need - install MSVC 6.0 as docs\toolchain.md describes" }
}
$out = Join-Path $root 'out\generic\libusb0\host'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$src = Join-Path $PSScriptRoot 'src'
$saved = @{ PATH = $env:PATH; INCLUDE = $env:INCLUDE; LIB = $env:LIB; CL = $env:CL; LINK = $env:LINK }
try {
    $env:PATH = "$vc\BIN;$root\tools\MSVC600\Common\MSDev98\Bin;$env:SystemRoot\system32"
    $env:INCLUDE = "$vc\Include"; $env:LIB = "$vc\Lib"; $env:CL = $null; $env:LINK = $null
    Push-Location $out
    try {
        # The harness is held to /WX; upstream's files are built as the DLL builds them.
        & cl.exe /nologo /c /W3 /WX /O2 /MD /DWIN32 "/I$src" "/I$src\driver" "$PSScriptRoot\tests\lutest.c" *> build.log
        if ($LASTEXITCODE) { Get-Content build.log; throw 'host test build failed' }
        & cl.exe /nologo /c /W3 /O2 /MD /DWIN32 "/I$src" "/I$src\driver" "$src\descriptors.c" "$src\error.c" *>> build.log
        if ($LASTEXITCODE) { Get-Content build.log; throw 'host test build failed' }
        & link.exe /nologo /SUBSYSTEM:CONSOLE /OUT:lutest.exe lutest.obj descriptors.obj error.obj kernel32.lib *>> build.log
        if ($LASTEXITCODE) { Get-Content build.log; throw 'host test link failed' }
        & .\lutest.exe | Tee-Object -FilePath result.txt
        if ($LASTEXITCODE) { throw 'host tests failed' }
        'PASS libusb0 host tests' | Set-Content -Encoding ASCII PASS.TXT
    } finally { Pop-Location }
} finally { foreach ($k in $saved.Keys) { Set-Item "env:$k" $saved[$k] } }

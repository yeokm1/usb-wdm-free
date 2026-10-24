# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
#
# Host tests of the xpad98 portable cores with the repository's MSVC 6.0,
# then the INF id check (infchk.ps1). No driver is installed; nothing is
# downloaded. Output: out\hid\xpad98\host\ (xptest.exe, test.log).
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$dest = Join-Path $repo 'out/hid/xpad98/host'
New-Item -ItemType Directory -Force $dest | Out-Null
$vc = Join-Path $repo 'tools/MSVC600/VC98'
$compiler = Join-Path $vc 'BIN/CL.EXE'
if (!(Test-Path -LiteralPath $compiler)) { throw "MISSING: $compiler - install MSVC 6.0 as docs\toolchain.md describes" }
$saved = @{ PATH = $env:PATH; INCLUDE = $env:INCLUDE; LIB = $env:LIB; CL = $env:CL }
try {
    $env:PATH = "$vc\BIN;$repo\tools\MSVC600\Common\MSDev98\Bin;$env:SystemRoot\system32"
    $env:INCLUDE = "$vc\Include"
    $env:LIB = "$vc\Lib"
    $env:CL = $null
    Push-Location $dest
    try {
        $src = 'xptest.c','xusb.c','gip.c','devtab.c','xpcfg.c','report.c' | ForEach-Object { Join-Path $PSScriptRoot $_ }
        & $compiler /nologo /W4 /WX /O2 /Fexptest.exe @src > build.log 2>&1
        if ($LASTEXITCODE) { Get-Content build.log; throw 'xpad98 host-test build failed' }
        & ./xptest.exe | Tee-Object -FilePath test.log
        if ($LASTEXITCODE) { throw 'xpad98 host tests failed' }
    } finally { Pop-Location }
} finally {
    $env:PATH = $saved.PATH; $env:INCLUDE = $saved.INCLUDE; $env:LIB = $saved.LIB; $env:CL = $saved.CL
}
& (Join-Path $PSScriptRoot 'infchk.ps1') -Test (Join-Path $dest 'xptest.exe')
if ($LASTEXITCODE) { throw 'xpad98 INF check failed' }

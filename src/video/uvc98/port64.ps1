# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Compile-only check of actual adapter/hooks/core against WDK7.1 amd64 ABI.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../../..").Path
$out = Join-Path $root 'out/video/uvc98/port/x64'
New-Item -ItemType Directory -Force $out | Out-Null
$oldInclude = $env:INCLUDE
Push-Location $out
try {
    $env:INCLUDE = "$root/tools/WinDDK71/inc/ddk;$root/tools/WinDDK71/inc/api;$root/tools/WinDDK71/inc/crt"
    $sources = 'uvcport','uvcportn','uvcflow','uvcown','uvcgen','uvcwork','uvcm1' | ForEach-Object { "$PSScriptRoot/$_.c" }
    & "$root/tools/WinDDK71/bin/x86/amd64/cl.exe" /nologo /c /W3 /WX /O1 /GS- /Zl /D_AMD64_=1 /DAMD64=1 /D_WIN64=1 /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 @sources *> build.log
    if ($LASTEXITCODE -ne 0) { throw "amd64 compile failed: $LASTEXITCODE" }
    Write-Output 'PASS: amd64 adapter/hooks/core/worker/M1 compile only; no driver link or runtime claim'
} finally {
    $env:INCLUDE = $oldInclude
    Pop-Location
}

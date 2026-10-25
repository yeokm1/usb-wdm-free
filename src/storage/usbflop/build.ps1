# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
param([ValidateSet('free','checked')][string]$Flavour = 'free')
$ErrorActionPreference = 'Stop'
function Hash([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','') }
    finally { $stream.Dispose(); $sha.Dispose() }
}
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
. (Join-Path $PSScriptRoot 'validate.ps1')
$out = Join-Path $root "out\storage\usbflop\inf\$Flavour"
New-Item -ItemType Directory -Force $out | Out-Null
# Invalidate a previous pass before any work. Only these build-owned files.
Remove-Item -LiteralPath (Join-Path $out 'PASS.TXT') -Force -ErrorAction SilentlyContinue
$records = @('package storage/usbflop', ('profile inf ' + $Flavour),
    'stage release 0.1.0, unsigned, no catalog',
    ('commit ' + (git -C $root rev-parse HEAD)),
    ('tree ' + $(if (git -C $root status --porcelain) {'dirty'} else {'clean'})))
foreach ($variant in @('nt','9x','me')) {
    $source = Join-Path $PSScriptRoot "$variant\USBFLOP.INF"
    $text = [IO.File]::ReadAllText($source)
    Assert-FloppyInf $text $variant
    $folder = Join-Path $out $variant
    New-Item -ItemType Directory -Force $folder | Out-Null
    $dest = Join-Path $folder 'USBFLOP.INF'
    [IO.File]::WriteAllText($dest, ($text -replace "`r?`n", "`r`n"), [Text.Encoding]::ASCII)
    $records += (Hash $source) + "  source/$variant/USBFLOP.INF"
    $records += (Hash $dest) + "  $variant/USBFLOP.INF"
}
foreach ($file in @('build.cmd','build.ps1','validate.ps1')) {
    $records += (Hash (Join-Path $PSScriptRoot $file)) + "  source/$file"
}
$records | Set-Content -Encoding ASCII (Join-Path $out 'record.txt')
'PASS: scoped CB/CBI models, transport/loader/filter/flags, ME system-media copy, no local payload, ASCII/CRLF' |
    Set-Content -Encoding ASCII (Join-Path $out 'gate.txt')
'Static gate only; Windows Setup and device tests pending.' |
    Set-Content -Encoding ASCII (Join-Path $out 'build.log')
'PASS usbflop inf' | Set-Content -Encoding ASCII (Join-Path $out 'PASS.TXT')
Write-Output "PASS: $out"

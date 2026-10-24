# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
param([ValidateSet('free','checked')][string]$Flavour = 'free')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$out = Join-Path $root "out\serial\usbcom\inf\$Flavour"
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item -LiteralPath (Join-Path $out 'PASS.TXT') -Force -ErrorAction SilentlyContinue
$source = Join-Path $PSScriptRoot 'USBCDC.INF'
$text = [IO.File]::ReadAllText($source)
. (Join-Path $PSScriptRoot 'validate.ps1')
Assert-CdcInf $text
$dest = Join-Path $out 'USBCDC.INF'
[IO.File]::WriteAllText($dest, ($text -replace "`r?`n", "`r`n"), [Text.Encoding]::ASCII)
$records = @('package serial/usbcom', ('profile inf ' + $Flavour),
    'stage unsigned INF-only package; real-board tests pending (serial.md 12, M0)',
    ('commit ' + (git -C $root rev-parse HEAD)),
    ('tree ' + $(if (git -C $root status --porcelain) {'dirty'} else {'clean'})))
foreach ($path in @($source,$dest,(Join-Path $PSScriptRoot 'build.cmd'),
    (Join-Path $PSScriptRoot 'build.ps1'),(Join-Path $PSScriptRoot 'validate.ps1'),
    (Join-Path $PSScriptRoot 'boards.ps1'),(Join-Path $PSScriptRoot 'boardids.txt'))) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try { $records += ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','') + '  ' + $path }
    finally { $stream.Dispose(); $sha.Dispose() }
}
$records | Set-Content -Encoding ASCII (Join-Path $out 'record.txt')
'PASS: scoped board/interface IDs, section graph, associated usbser service, in-box-only payload, ASCII/CRLF' |
    Set-Content -Encoding ASCII (Join-Path $out 'gate.txt')
'Static gate only; Windows Setup, Ports assignment and serial I/O tests pending.' |
    Set-Content -Encoding ASCII (Join-Path $out 'build.log')
'PASS usbcom inf' | Set-Content -Encoding ASCII (Join-Path $out 'PASS.TXT')
Write-Output "PASS: $out"

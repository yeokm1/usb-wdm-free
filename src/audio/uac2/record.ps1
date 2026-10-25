# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory)][string]$Output)
$ErrorActionPreference = 'Stop'
$sources = @(Get-ChildItem -LiteralPath $PSScriptRoot -File |
    Where-Object { $_.Extension -in '.cmd','.ps1','.cpp','.c','.h','.inf','.rc','.txt','.manifest' })
$sources += Get-Item (Join-Path $PSScriptRoot '..\common\punknown.h')
$sources += Get-Item (Join-Path $Output 'uac2wdm.sys'),(Join-Path $Output 'uac2wdm.inf')
foreach ($file in $sources) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($file.FullName)
    try { ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','') + '  ' + $file.FullName }
    finally { $stream.Dispose(); $sha.Dispose() }
}

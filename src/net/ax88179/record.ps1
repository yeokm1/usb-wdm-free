# SPDX-License-Identifier: GPL-2.0-only
# Hashes of the package sources and of the built files, for record.txt.
param([Parameter(Mandatory)][string]$Output)
$ErrorActionPreference = 'Stop'
$files = @(Get-ChildItem -LiteralPath $PSScriptRoot -File)
$files += @(Get-ChildItem -LiteralPath $Output -File | Where-Object { $_.Extension -in '.sys', '.INF', '.inf' })
foreach ($file in $files) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($file.FullName)
    try { ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','') + '  ' + $file.FullName }
    finally { $stream.Dispose(); $sha.Dispose() }
}

# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Host-only native ABI for this package's actual original packet planner.
param([Parameter(Mandatory=$true)][string]$Compiler,
      [string]$Output='E:\usb-wdm-free\wave2\uac-native')
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$env:TEMP=$Output
$env:TMP=$Output
$tool=Get-Item -LiteralPath $Compiler
$env:PATH=$tool.DirectoryName+';'+$env:PATH
& $tool.FullName -shared -O2 -Wall -Wextra -Werror '-Wl,--no-insert-timestamp' ('-Wl,--out-implib,'+(Join-Path $Output 'UACRATE.A')) `
    (Join-Path $PSScriptRoot 'uacrate.c') -o (Join-Path $Output 'UACRATE.DLL')
if($LASTEXITCODE){throw 'Rate DLL build failed'}
Get-FileHash -LiteralPath (Join-Path $Output 'UACRATE.DLL')

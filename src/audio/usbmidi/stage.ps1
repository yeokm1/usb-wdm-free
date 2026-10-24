# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Preserve INF semantics while producing deterministic ANSI/CRLF media.
param([Parameter(Mandatory)][string]$Source,[Parameter(Mandatory)][string]$Destination)
$ErrorActionPreference='Stop'
$content=[IO.File]::ReadAllText((Resolve-Path -LiteralPath $Source).Path)
if($content.ToCharArray() | Where-Object {[int]$_ -gt 127}) {throw 'MIDI test INF must remain ASCII'}
$content=$content -replace '\r?\n',"`r`n"
[IO.File]::WriteAllText($Destination,$content,[Text.Encoding]::ASCII)

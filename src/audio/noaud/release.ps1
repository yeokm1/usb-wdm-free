# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Release noaud: copy NOAUD98.INF and README.TXT into free/audio/noaud/w98se/
# (ASCII, CRLF) and rewrite SHA256.TXT over every file under free/.
# An INF only: nothing to build. Run from any directory:
#   powershell -NoProfile -File src\audio\noaud\release.ps1
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$top = (Resolve-Path (Join-Path $here '..\..\..')).Path
$dest = Join-Path $top 'free\audio\noaud\w98se'
New-Item -ItemType Directory -Force $dest | Out-Null
foreach ($name in 'NOAUD98.INF', 'README.TXT') {
    $text = [IO.File]::ReadAllText((Join-Path $here $name))
    if ($text.ToCharArray() | Where-Object { [int]$_ -gt 127 }) { throw "$name must be ASCII" }
    $text = $text -replace '\r?\n', "`r`n"
    [IO.File]::WriteAllText((Join-Path $dest $name), $text, [Text.Encoding]::ASCII)
}
# SHA256.TXT: every file under free/, sorted by path, "<hash>  <path>" with '/' separators, LF.
$free = Join-Path $top 'free'
$lines = Get-ChildItem -LiteralPath $free -Recurse -File | ForEach-Object {
    $rel = 'free/' + $_.FullName.Substring($free.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
    "$hash  $rel"
} | Sort-Object { $_.Substring(66) } -CaseSensitive
[IO.File]::WriteAllText((Join-Path $top 'SHA256.TXT'), (($lines -join "`n") + "`n"), [Text.Encoding]::ASCII)
"released to $dest; SHA256.TXT lists $(@($lines).Count) files"

# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Rewrites the model sections of USBCOM.INF and USBCDC.INF from
# boardids.txt (boards.ps1). Run after editing boardids.txt; the gates then
# check the INFs against the same derivation. Touches nothing else.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'boards.ps1')
$ids = Join-Path $PSScriptRoot 'boardids.txt'
function Set-Section([string]$text, [string]$name, [string[]]$lines) {
    $nl = "`r`n"
    $t = $text -replace "`r?`n", $nl
    $start = $t.IndexOf("[$name]$nl")
    if ($start -lt 0) { throw "No section [$name]" }
    $body = $start + "[$name]$nl".Length
    $next = $t.IndexOf("$nl[", $body)
    $blank = $t.LastIndexOf("$nl$nl", $next)
    $end = if ($blank -ge $body) { $blank + 2 * $nl.Length } else { $next + $nl.Length }
    return $t.Substring(0, $body) + (($lines -join $nl) + $nl + $nl) + $t.Substring($end)
}
$p = Join-Path $PSScriptRoot 'USBCOM.INF'
$t = [IO.File]::ReadAllText($p)
$t = Set-Section $t 'Models' @(Get-UsbcomModels 'USBCOM' 'Models' $ids | ForEach-Object { "%$($_[0])%=UsbCom,$($_[1])" })
[IO.File]::WriteAllText($p, $t, [Text.Encoding]::ASCII)
$p = Join-Path $PSScriptRoot 'USBCDC.INF'
$t = [IO.File]::ReadAllText($p)
foreach ($s in 'Models', 'Models.NTx86', 'Models.NTamd64') {
    $t = Set-Section $t $s @(Get-UsbcomModels 'USBCDC' $s $ids | ForEach-Object { "%$($_[0])%=ACM,$($_[1])" })
}
[IO.File]::WriteAllText($p, $t, [Text.Encoding]::ASCII)
Write-Output 'Model sections written.'

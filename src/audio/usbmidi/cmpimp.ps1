# SPDX-License-Identifier: GPL-2.0-only
#
# Compares two DUMPBIN /IMPORTS reports of binaries built here: the driver
# as linked with the 2000 DDK libraries and the 98 SE DDK link probe. Every
# MODULE!name must be the same in both (build.md 5.1: the 98 SE import
# libraries name the module each import resolves to).
#
# Copyright (C) 2026 Yeo Kheng Meng

param([Parameter(Mandatory)] [string] $Built, [Parameter(Mandatory)] [string] $Probe,
      [Parameter(Mandatory)] [string] $Report)

function Read-Imports($path) {
    $set = New-Object System.Collections.Generic.SortedSet[string]
    $module = $null
    foreach ($l in Get-Content $path) {
        if ($l -match '^\s*Summary\s*$') { break }
        if ($l -match '^\s{4}(\S+\.(exe|sys|dll))\s*$') { $module = $Matches[1].ToUpperInvariant(); continue }
        if ($module -and $l -match '^\s+[0-9A-Fa-f]+\s+([A-Za-z_@?$][\w@?$]*)\s*$') { [void]$set.Add("$module!$($Matches[1])") }
    }
    return ,$set
}
$a = Read-Imports $Built
$b = Read-Imports $Probe
$onlyA = @($a | Where-Object { -not $b.Contains($_) })
$onlyB = @($b | Where-Object { -not $a.Contains($_) })
$lines = @("built: $($a.Count) imports; 98 SE probe: $($b.Count) imports")
$onlyA | ForEach-Object { $lines += "  only in built: $_" }
$onlyB | ForEach-Object { $lines += "  only in probe: $_" }
$ok = ($a.Count -gt 0) -and ($onlyA.Count -eq 0) -and ($onlyB.Count -eq 0)
$lines += $(if ($ok) { 'result: PASS' } else { 'result: FAIL' })
$lines | Set-Content -Encoding ASCII $Report
$lines | ForEach-Object { Write-Output $_ }
if (-not $ok) { exit 1 }
exit 0

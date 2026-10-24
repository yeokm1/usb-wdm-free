# SPDX-License-Identifier: GPL-2.0-only
#
# Import and PE gate for a binary built here (build.md section 5.2).
# Reads DUMPBIN /HEADERS and /IMPORTS output of OUR binary only. Fails
# on a wrong machine or subsystem version, on any delay-load or ordinal
# import, and on any MODULE!name not in the allowed list.
#
# Copyright (C) 2026 Yeo Kheng Meng

param(
    [Parameter(Mandatory)] [string] $Headers,
    [Parameter(Mandatory)] [string] $Imports,
    [Parameter(Mandatory)] [string] $Allowed,
    [Parameter(Mandatory)] [string] $Machine,
    [Parameter(Mandatory)] [string] $Subsystem,
    [Parameter(Mandatory)] [string] $Report
)

$ErrorActionPreference = 'Stop'
$fail = @()
$out = @()

$h = Get-Content $Headers
if (-not ($h | Where-Object { $_ -match "^\s*$Machine machine" })) {
    $fail += "machine is not $Machine"
}
if (-not ($h | Where-Object { $_ -match "^\s*$([regex]::Escape($Subsystem)) subsystem version" })) {
    $fail += "subsystem version is not $Subsystem"
}
if (-not ($h | Where-Object { $_ -match '^\s*1 subsystem \(Native\)' })) {
    $fail += "subsystem is not Native"
}
foreach ($l in $h) {
    if ($l -match '^\s*([0-9A-Fa-f]+)\s+\[\s*([0-9A-Fa-f]+)\]\s+RVA \[size\] of Delay Import Directory\s*$') {
        if ([Convert]::ToUInt64($Matches[1], 16) -ne 0 -or
            [Convert]::ToUInt64($Matches[2], 16) -ne 0) {
            $fail += 'nonempty PE delay-import directory'
        }
    }
}

$allow = @{}
foreach ($l in Get-Content $Allowed) {
    $t = $l.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    $allow[$t.ToUpperInvariant()] = $true
}

$module = $null
$moduleImports = 0
$modules = 0
$section = $false
$summary = $false
foreach ($l in Get-Content $Imports) {
    if ($l -match 'delay[- ]load imports') {
        $fail += 'delay-load import section'
        $section = $true
        continue
    }
    if ($l -match '^\s*Section contains the following imports:\s*$') {
        if ($section) { $fail += 'duplicate import section' }
        $section = $true
        continue
    }
    if (-not $section) { continue }
    if ($l -match '^\s*Summary\s*$') { $summary = $true; break }
    if ($l -match '^\s*$') { continue }
    if ($l -match '^\s{4}(\S+\.(exe|sys|dll))\s*$') {
        if ($module -and $moduleImports -eq 0) { $fail += "no named imports parsed for $module" }
        $module = $Matches[1].ToUpperInvariant()
        $moduleImports = 0
        $modules++
        continue
    }
    if ($null -eq $module) {
        $fail += "unrecognized import section row: $($l.Trim())"
        continue
    }
    # Both kit dumpers print these four descriptor fields before the names.
    if ($l -match '^\s*[0-9A-Fa-f]+\s+(Import Address Table|Import Name Table|time date stamp|Index of first forwarder reference)\s*$') {
        continue
    }
    # DUMPBIN versions differ in whether an ordinal row has a thunk prefix.
    if ($l -match '^\s*(?:[0-9A-Fa-f]+\s+)?Ordinal\s+(?:0x)?([0-9A-Fa-f]+)\s*$') {
        $fail += "ordinal import $module!#$($Matches[1])"
        continue
    }
    if ($l -match '^\s+[0-9A-Fa-f]+\s+([A-Za-z_@?$][A-Za-z0-9_@?$]*)\s*$') {
        $key = "$module!$($Matches[1])".ToUpperInvariant()
        $out += "$module!$($Matches[1])"
        $moduleImports++
        if (-not $allow.ContainsKey($key)) { $fail += "not allowed: $module!$($Matches[1])" }
        continue
    }
    $fail += "unrecognized import row in ${module}: $($l.Trim())"
}
if ($module -and $moduleImports -eq 0) { $fail += "no named imports parsed for $module" }
if (-not $section) { $fail += 'no import section found' }
if (-not $summary) { $fail += 'missing import report summary; report may be truncated' }
if ($modules -eq 0 -or $out.Count -eq 0) { $fail += 'no named imports parsed' }

$lines = @("imports:") + ($out | ForEach-Object { "  $_" })
if ($fail.Count -eq 0) {
    $lines += "result: PASS"
} else {
    $lines += "result: FAIL"
    $lines += ($fail | ForEach-Object { "  $_" })
}
$lines | Set-Content -Encoding ASCII $Report
$lines | ForEach-Object { Write-Output $_ }
if ($fail.Count -ne 0) { exit 1 }
exit 0

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

$allow = @{}
foreach ($l in Get-Content $Allowed) {
    $t = $l.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    $allow[$t.ToUpperInvariant()] = $true
}

$module = $null
$delay = $false
foreach ($l in Get-Content $Imports) {
    if ($l -match '^\s*Summary\s*$') { break }
    if ($l -match 'delay load imports') { $delay = $true }
    if ($l -match '^\s{4}(\S+\.(exe|sys|dll))\s*$') {
        $module = $Matches[1].ToUpperInvariant()
        if ($delay) { $fail += "delay-load import from $module" }
        continue
    }
    if ($null -eq $module) { continue }
    if ($l -match '^\s+Ordinal\s+(\d+)') {
        $fail += "ordinal import $module!#$($Matches[1])"
        continue
    }
    if ($l -match '^\s+[0-9A-Fa-f]+\s+([A-Za-z_@?$][\w@?$]*)\s*$') {
        $key = "$module!$($Matches[1])".ToUpperInvariant()
        $out += "$module!$($Matches[1])"
        if (-not $allow.ContainsKey($key)) { $fail += "not allowed: $module!$($Matches[1])" }
    }
}

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

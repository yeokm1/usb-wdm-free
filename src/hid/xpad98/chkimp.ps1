# SPDX-License-Identifier: GPL-2.0-only
#
# Import and PE gate for xpad98.sys (build.md section 5.2). Reads DUMPBIN
# /HEADERS and /IMPORTS output of OUR binary only. Fails on a wrong
# machine or subsystem version, on any delay-load or ordinal import, and
# on any MODULE!name not in the package's allowed list. With -Win98 (the
# x86 build), every import must also be named by one of the Windows 98 SE
# DDK's import libraries (WDM.LIB, HAL.LIB, HIDCLASS.LIB, read with
# DUMPBIN /EXPORTS): a kit declaration for 98, not proof that NTKERN
# exports it (build.md 5.1; the guest load test is that proof).
#
# Copyright (C) 2026 Yeo Kheng Meng

param(
    [Parameter(Mandatory)] [string] $Headers,
    [Parameter(Mandatory)] [string] $Imports,
    [Parameter(Mandatory)] [string] $Allowed,
    [Parameter(Mandatory)] [string] $Machine,
    [Parameter(Mandatory)] [string] $Subsystem,
    [Parameter(Mandatory)] [string] $Report,
    [string] $Win98     # comma-separated DUMPBIN /EXPORTS listings
)

$fail = @()
$out = @()

$h = Get-Content $Headers
if (-not ($h | Where-Object { $_ -match "^\s*$Machine machine" })) { $fail += "machine is not $Machine" }
if (-not ($h | Where-Object { $_ -match "^\s*$([regex]::Escape($Subsystem)) subsystem version" })) { $fail += "subsystem version is not $Subsystem" }
if (-not ($h | Where-Object { $_ -match '^\s*1 subsystem \(Native\)' })) { $fail += "subsystem is not Native" }
foreach ($l in $h) {
    if ($l -match '^\s*([0-9A-Fa-f]+)\s+\[\s*([0-9A-Fa-f]+)\]\s+RVA \[size\] of Delay Import Directory') {
        if ([Convert]::ToUInt64($Matches[1], 16) -or [Convert]::ToUInt64($Matches[2], 16)) { $fail += 'delay import directory present' }
    }
}

$allow = @{}
foreach ($l in Get-Content $Allowed) {
    $t = $l.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    $allow[$t.ToUpperInvariant()] = $true
}

$w98 = @{}
if ($Win98) {
    foreach ($f in ($Win98 -split ',' | ForEach-Object { $_.Trim('" ') } | Where-Object { $_ })) {
        $in = $false
        foreach ($l in Get-Content $f) {
            if ($l -match '^\s*Exports\s*$') { $in = $true; continue }
            if ($l -match '^\s*Summary\s*$') { break }
            if (-not $in) { continue }
            $t = $l.Trim()
            if ($t -eq '' -or $t -match '^ordinal') { continue }
            $n = ($t -split '\s+')[-1]
            $n = $n -replace '^[_@]', '' -replace '@\d+$', ''
            $w98[$n.ToUpperInvariant()] = $true
        }
    }
    if ($w98.Count -lt 100) { $fail += "98 DDK library export lists look empty ($($w98.Count) names)" }
}

$module = $null
$count = 0
foreach ($l in Get-Content $Imports) {
    if ($l -match '^\s*Summary\s*$') { break }
    if ($l -match 'delay[- ]load imports') { $fail += 'delay-load imports' }
    if ($l -match '^\s{4}(\S+\.(exe|sys|dll))\s*$') { $module = $Matches[1].ToUpperInvariant(); continue }
    if ($null -eq $module) { continue }
    if ($l -match '^\s+Ordinal\s+(\d+)') { $fail += "ordinal import $module!#$($Matches[1])"; continue }
    if ($l -match '^\s+[0-9A-Fa-f]+\s+([A-Za-z_@?$][\w@?$]*)\s*$') {
        $name = $Matches[1]
        $key = "$module!$name".ToUpperInvariant()
        $count++
        $note = ''
        if (-not $allow.ContainsKey($key)) { $fail += "not allowed: $module!$name" }
        if ($Win98) {
            if ($w98.ContainsKey($name.ToUpperInvariant())) { $note = '  [98 DDK lib]' }
            else { $fail += "not declared by the 98 DDK libraries: $module!$name" }
        }
        $out += "$module!$name$note"
    }
}
if ($count -lt 10) { $fail += "only $count imports parsed" }

$lines = @("imports ($count):") + ($out | ForEach-Object { "  $_" })
if ($fail.Count -eq 0) { $lines += 'result: PASS' }
else { $lines += 'result: FAIL'; $lines += ($fail | ForEach-Object { "  $_" }) }
$lines | Set-Content -Encoding ASCII $Report
$lines | ForEach-Object { Write-Output $_ }
if ($fail.Count -ne 0) { exit 1 }
exit 0

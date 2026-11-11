# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Host check (test.cmd): every install section of both uac2 INFs writes
# Uac2QueueMs at the driver's default, without overwriting a value already
# there (docs/design/uac2.md 19.3): the REG_DWORD form in the NT sections,
# the 4-byte binary form in 98 SE / ME's [UAC2]. Mutations of the INF text
# must be refused. Reads text only; installs nothing.
$ErrorActionPreference = 'Stop'
$plan = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '../common/uacplan.h'))
if ($plan -notmatch '#define UAC_QUEUE_DEFAULT_MS (\d+)U') { throw 'UAC_QUEUE_DEFAULT_MS not found in uacplan.h' }
$default = [int]$Matches[1]
$bytes = (0..3 | ForEach-Object { '{0:x2}' -f (($default -shr (8 * $_)) -band 255) }) -join ','

function Sections([string]$Text) {
    $map = @{}; $name = $null
    foreach ($raw in ($Text -split "`r?`n")) {
        $line = ($raw -replace ';.*$', '').Trim()
        if (-not $line) { continue }
        if ($line -match '^\[(.+)\]$') {
            $name = $Matches[1].Trim().ToLowerInvariant()
            if ($map.ContainsKey($name)) { throw "duplicate section [$name]" }
            $map[$name] = New-Object System.Collections.ArrayList
            continue
        }
        if ($name) { [void]$map[$name].Add($line) }
    }
    $map
}

function AddRegOf($map, [string]$section) {
    $s = $map[$section.ToLowerInvariant()]
    if (-not $s) { throw "no section [$section]" }
    $names = @()
    foreach ($l in $s) {
        if ($l -match '^addreg\s*=\s*(.+)$') { $names += ($Matches[1] -split ',' | ForEach-Object { $_.Trim().ToLowerInvariant() }) }
    }
    $names
}

function Assert-Queue([string]$Text) {
    Assert-Value $Text 'uac2queuems' "$default" $bytes
    Assert-Value $Text 'uac2volabovezero' '0' '00,00,00,00'
}

# One value, written once in every install section: REG_DWORD no-clobber
# $dword in the NT sections, binary no-clobber $binary in [UAC2].
function Assert-Value([string]$Text, [string]$value, [string]$dword, [string]$binary) {
    $map = Sections $Text
    $want = @{ 'uac2' = 'binary'; 'uac2.nt' = 'dword'; 'uac2xp.nt' = 'dword'; 'uac2.ntamd64' = 'dword' }
    foreach ($section in $want.Keys) {
        $lines = @()
        foreach ($reg in (AddRegOf $map $section)) {
            $r = $map[$reg]
            if (-not $r) { throw "[$section] names a missing AddReg section [$reg]" }
            $lines += @($r | Where-Object { $_ -match "^hkr\s*,\s*,\s*$value\s*," })
        }
        if ($lines.Count -ne 1) { throw "[$section] writes $value $($lines.Count) times" }
        $f = ($lines[0] -split ',' | ForEach-Object { $_.Trim() })
        if ($want[$section] -eq 'dword') {
            if ($f.Count -ne 5 -or $f[3] -ne '0x00010003' -or $f[4] -ne $dword) {
                throw "[$section]: $value is not REG_DWORD no-clobber $dword ($($lines[0]))"
            }
        } else {
            if ($f.Count -ne 8 -or $f[3] -ne '0x00000003' -or (($f[4..7] -join ',').ToLowerInvariant()) -ne $binary) {
                throw "[$section]: $value is not binary no-clobber $binary ($($lines[0]))"
            }
        }
    }
}

# Every install section makes the device the preferred one at install, once:
# binary no-clobber 02,00,00,00 (design 22.3).
function Assert-Preferred([string]$Text) {
    $map = Sections $Text
    foreach ($section in @('uac2', 'uac2.nt', 'uac2xp.nt', 'uac2.ntamd64')) {
        $lines = @()
        foreach ($reg in (AddRegOf $map $section)) {
            $r = $map[$reg]
            if (-not $r) { throw "[$section] names a missing AddReg section [$reg]" }
            $lines += @($r | Where-Object { $_ -match '^hkr\s*,\s*,\s*setuppreferredaudiodevices\s*,' })
        }
        if ($lines.Count -ne 1) { throw "[$section] writes SetupPreferredAudioDevices $($lines.Count) times" }
        $f = ($lines[0] -split ',' | ForEach-Object { $_.Trim() })
        if ($f.Count -ne 8 -or $f[3] -ne '3' -or (($f[4..7] -join ',').ToLowerInvariant()) -ne '02,00,00,00') {
            throw "[$section]: SetupPreferredAudioDevices is not binary no-clobber 02,00,00,00 ($($lines[0]))"
        }
    }
}

$n = 0
foreach ($inf in @('uac2wdm.inf', 'x64\uac2wdm.inf')) {
    $t = [IO.File]::ReadAllText((Join-Path $PSScriptRoot $inf))
    Assert-Queue $t
    Assert-Preferred $t
    $pl = 'HKR,,SetupPreferredAudioDevices,3,02,00,00,00'
    $badP = [ordered]@{
        'preferred missing' = $t.Replace("$pl`r`n", '')
        'preferred off'     = $t.Replace($pl, 'HKR,,SetupPreferredAudioDevices,3,00,00,00,00')
        'preferred odd'     = $t.Replace($pl, 'HKR,,SetupPreferredAudioDevices,3,01,00,00,00')
        'preferred clobber' = $t.Replace($pl, 'HKR,,SetupPreferredAudioDevices,1,02,00,00,00')
        'preferred twice'   = $t.Replace('[QueueNT]', "[QueueNT]`r`n$pl")
    }
    foreach ($k in $badP.Keys) {
        if ($badP[$k] -ceq $t) { throw "${inf}: mutation did not change the text: $k" }
        $failed = $false
        try { Assert-Preferred $badP[$k] } catch { $failed = $true }
        if (-not $failed) { throw "${inf}: check accepted: $k" }
        $n++
    }
    $bad = [ordered]@{
        'NT clobbers'     = $t.Replace('HKR,,Uac2QueueMs,0x00010003,32', 'HKR,,Uac2QueueMs,0x00010001,32')
        'NT default'      = $t.Replace('HKR,,Uac2QueueMs,0x00010003,32', 'HKR,,Uac2QueueMs,0x00010003,16')
        '9x clobbers'     = $t.Replace('HKR,,Uac2QueueMs,0x00000003,20,00,00,00', 'HKR,,Uac2QueueMs,0x00000001,20,00,00,00')
        '9x as a number'  = $t.Replace('HKR,,Uac2QueueMs,0x00000003,20,00,00,00', 'HKR,,Uac2QueueMs,0x00010003,32')
        '9x missing'      = $t.Replace('AddReg=Loader,AudioSupport,Queue9x', 'AddReg=Loader,AudioSupport')
        'XP missing'      = $t.Replace("[UAC2XP.NT]`r`nInclude=ks.inf,wdmaudio.inf`r`nNeeds=KS.Registration,WDMAUDIO.Registration.NT`r`nCopyFiles=Driver.Copy.NT`r`nAddReg=AudioSupport,QueueNT", "[UAC2XP.NT]`r`nInclude=ks.inf,wdmaudio.inf`r`nNeeds=KS.Registration,WDMAUDIO.Registration.NT`r`nCopyFiles=Driver.Copy.NT`r`nAddReg=AudioSupport")
        'twice'           = $t.Replace('[QueueNT]', "[QueueNT]`r`nHKR,,Uac2QueueMs,0x00010003,32")
        'AZ NT clobbers'  = $t.Replace('HKR,,Uac2VolAboveZero,0x00010003,0', 'HKR,,Uac2VolAboveZero,0x00010001,0')
        'AZ NT on'        = $t.Replace('HKR,,Uac2VolAboveZero,0x00010003,0', 'HKR,,Uac2VolAboveZero,0x00010003,1')
        'AZ 9x clobbers'  = $t.Replace('HKR,,Uac2VolAboveZero,0x00000003,00,00,00,00', 'HKR,,Uac2VolAboveZero,0x00000001,00,00,00,00')
        'AZ 9x on'        = $t.Replace('HKR,,Uac2VolAboveZero,0x00000003,00,00,00,00', 'HKR,,Uac2VolAboveZero,0x00000003,01,00,00,00')
        'AZ NT missing'   = $t.Replace("HKR,,Uac2VolAboveZero,0x00010003,0`r`n", '')
        'AZ twice'        = $t.Replace('[Queue9x]', "[Queue9x]`r`nHKR,,Uac2VolAboveZero,0x00000003,00,00,00,00")
    }
    foreach ($k in $bad.Keys) {
        if ($bad[$k] -ceq $t) { throw "${inf}: mutation did not change the text: $k" }
        $failed = $false
        try { Assert-Queue $bad[$k] } catch { $failed = $true }
        if (-not $failed) { throw "${inf}: check accepted: $k" }
        $n++
    }
}
# The driver's fallback name (uac2wdm.cpp NameFallback, docs/design/uac2.md 21)
# is each INF's Device string.
$cpp = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '../common/uacwdm.cpp'))
# uacwdm.cpp is shared with uac1 (UAC_V1): uac2's name is the #else branch's.
if ($cpp -notmatch '#else\r?\nstatic const char NameFallback\[\] = "([^"]*)";') { throw 'NameFallback not found in uac2wdm.cpp' }
$fallback = $Matches[1]
foreach ($inf in @('uac2wdm.inf', 'x64\uac2wdm.inf')) {
    $t = [IO.File]::ReadAllText((Join-Path $PSScriptRoot $inf))
    if ($t -notmatch '(?m)^Device="([^"]*)"\s*$') { throw "${inf}: no Device string" }
    if ($Matches[1] -cne $fallback) { throw "${inf}: Device string '$($Matches[1])' is not the driver's fallback '$fallback'" }
}
# uac1 (the UAC_V1 branch) names its devices from uac1's INF the same way.
if ($cpp -notmatch '#ifdef UAC_V1\r?\nstatic const char NameFallback\[\] = "([^"]*)";') { throw 'uac1 NameFallback not found in uacwdm.cpp' }
$fallback1 = $Matches[1]
$t = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\uac1\uac1wdm.inf'))
if ($t -notmatch '(?m)^Device="([^"]*)"\s*$') { throw 'uac1wdm.inf: no Device string' }
if ($Matches[1] -cne $fallback1) { throw "uac1wdm.inf: Device string '$($Matches[1])' is not the driver's fallback '$fallback1'" }
Write-Output "PASS infchk: Uac2QueueMs at $default, Uac2VolAboveZero at 0 and SetupPreferredAudioDevices 2 (no-clobber) in every install section of both INFs, Device the driver fallback name (uac2 and uac1); $n mutations refused"

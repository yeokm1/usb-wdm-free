# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Host check (test.cmd): every install section of both uac2 INFs writes
# Uac2QueueMs at the driver's default, without overwriting a value already
# there (docs/design/uac2.md 19.3): the REG_DWORD form in the NT sections,
# the 4-byte binary form in 98 SE / ME's [UAC2]. Mutations of the INF text
# must be refused. Reads text only; installs nothing.
$ErrorActionPreference = 'Stop'
$plan = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'uacplan.h'))
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
    $map = Sections $Text
    $want = @{ 'uac2' = 'binary'; 'uac2.nt' = 'dword'; 'uac2xp.nt' = 'dword'; 'uac2.ntamd64' = 'dword' }
    foreach ($section in $want.Keys) {
        $lines = @()
        foreach ($reg in (AddRegOf $map $section)) {
            $r = $map[$reg]
            if (-not $r) { throw "[$section] names a missing AddReg section [$reg]" }
            $lines += @($r | Where-Object { $_ -match '^hkr\s*,\s*,\s*uac2queuems\s*,' })
        }
        if ($lines.Count -ne 1) { throw "[$section] writes Uac2QueueMs $($lines.Count) times" }
        $f = ($lines[0] -split ',' | ForEach-Object { $_.Trim() })
        if ($want[$section] -eq 'dword') {
            if ($f.Count -ne 5 -or $f[3] -ne '0x00010003' -or $f[4] -ne "$default") {
                throw "[$section]: Uac2QueueMs is not REG_DWORD no-clobber $default ($($lines[0]))"
            }
        } else {
            if ($f.Count -ne 8 -or $f[3] -ne '0x00000003' -or (($f[4..7] -join ',').ToLowerInvariant()) -ne $bytes) {
                throw "[$section]: Uac2QueueMs is not binary no-clobber $bytes ($($lines[0]))"
            }
        }
    }
}

$n = 0
foreach ($inf in @('uac2wdm.inf', 'x64\uac2wdm.inf')) {
    $t = [IO.File]::ReadAllText((Join-Path $PSScriptRoot $inf))
    Assert-Queue $t
    $bad = [ordered]@{
        'NT clobbers'     = $t.Replace('HKR,,Uac2QueueMs,0x00010003,32', 'HKR,,Uac2QueueMs,0x00010001,32')
        'NT default'      = $t.Replace('HKR,,Uac2QueueMs,0x00010003,32', 'HKR,,Uac2QueueMs,0x00010003,16')
        '9x clobbers'     = $t.Replace('HKR,,Uac2QueueMs,0x00000003,20,00,00,00', 'HKR,,Uac2QueueMs,0x00000001,20,00,00,00')
        '9x as a number'  = $t.Replace('HKR,,Uac2QueueMs,0x00000003,20,00,00,00', 'HKR,,Uac2QueueMs,0x00010003,32')
        '9x missing'      = $t.Replace('AddReg=Loader,AudioSupport,Queue9x', 'AddReg=Loader,AudioSupport')
        'XP missing'      = $t.Replace("[UAC2XP.NT]`r`nInclude=ks.inf,wdmaudio.inf`r`nNeeds=KS.Registration,WDMAUDIO.Registration.NT`r`nCopyFiles=Driver.Copy.NT`r`nAddReg=AudioSupport,QueueNT", "[UAC2XP.NT]`r`nInclude=ks.inf,wdmaudio.inf`r`nNeeds=KS.Registration,WDMAUDIO.Registration.NT`r`nCopyFiles=Driver.Copy.NT`r`nAddReg=AudioSupport")
        'twice'           = $t.Replace('[QueueNT]', "[QueueNT]`r`nHKR,,Uac2QueueMs,0x00010003,32")
    }
    foreach ($k in $bad.Keys) {
        if ($bad[$k] -ceq $t) { throw "${inf}: mutation did not change the text: $k" }
        $failed = $false
        try { Assert-Queue $bad[$k] } catch { $failed = $true }
        if (-not $failed) { throw "${inf}: check accepted: $k" }
        $n++
    }
}
Write-Output "PASS infchk: Uac2QueueMs at $default in every install section of both INFs; $n mutations refused"

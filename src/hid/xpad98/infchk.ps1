# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
#
# Checks the three xpad98 INFs against devtab.c (through xptest.exe ids):
# each model section lists exactly the ids its folder is meant to serve
# (gamepad.md sections 1, 5.1, 7 and 8), no x360c id appears anywhere, no
# id repeats, the files are ASCII and the install sections exist.
param([Parameter(Mandatory)][string]$Test)
$ErrorActionPreference = 'Stop'
$fail = @()

function Ids([string]$set) {
    $r = & $Test ids $set
    if ($LASTEXITCODE) { throw "xptest ids $set failed" }
    return @($r | ForEach-Object { $_.Trim().ToUpperInvariant() } | Where-Object { $_ })
}
$want = @{ X360 = Ids 'x360'; X360W = Ids 'x360w'; GIP = Ids 'gip' }

# The 25 x360c ids, as devtab.c lists them (design 5.1).
$x360c = @('045E:028E','046D:C242','0738:CB02','0738:4716','0738:4718','0738:4726',
    '0738:4728','0738:4736','0738:4738','0738:A728','1BAD:F028','1BAD:F029','1BAD:F039',
    '0F0D:000A','0F0D:000B','0F0D:000C','0F0D:000D','0F0D:000F','0F0D:0016','0F0D:0031',
    '1BAD:F500','1BAD:F501','1BAD:F502','1BAD:F504','1BAD:F506') |
    ForEach-Object { $p = $_.Split(':'); "USB\VID_$($p[0])&PID_$($p[1])" }
if ($x360c.Count -ne 25) { $fail += 'x360c list is not 25 ids' }

function Sections([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes | Where-Object { $_ -gt 0x7E -or ($_ -lt 0x20 -and $_ -ne 9 -and $_ -ne 10 -and $_ -ne 13) }) {
        $script:fail += "$path is not plain ASCII"
    }
    $s = @{}; $cur = $null
    foreach ($l in [IO.File]::ReadAllLines($path)) {
        $t = $l.Trim()
        if ($t -eq '' -or $t.StartsWith(';')) { continue }
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1]; $s[$cur] = @(); continue }
        if ($null -ne $cur) { $s[$cur] += $t }
    }
    return $s
}

function Models($lines, [string]$path, [string]$section) {
    $m = @{}
    foreach ($l in $lines) {
        if ($l -notmatch '^%(\w+)%\s*=\s*XPAD\s*,\s*(USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4})$') {
            $script:fail += "${path} [$section]: unexpected line $l"; continue
        }
        $k = $Matches[1]; $id = $Matches[2].ToUpperInvariant()
        if (-not $m.ContainsKey($k)) { $m[$k] = @() }
        if ($m.Values | ForEach-Object { $_ } | Where-Object { $_ -eq $id }) { $script:fail += "${path} [$section]: $id repeats" }
        if ($x360c -contains $id) { $script:fail += "${path} [$section]: $id is an x360c id" }
        $m[$k] += $id
    }
    return $m
}

function Same($got, $exp, [string]$what) {
    $g = @($got | Sort-Object); $e = @($exp | Sort-Object)
    if (($g -join ',') -ne ($e -join ',')) {
        $missing = $e | Where-Object { $g -notcontains $_ }
        $extra = $g | Where-Object { $e -notcontains $_ }
        $script:fail += "${what}: missing [$($missing -join ' ')] extra [$($extra -join ' ')]"
    }
}

function Expect([string]$file, [string[]]$modelSections, [string[]]$families, [string[]]$installs) {
    $path = Join-Path $PSScriptRoot $file
    $s = Sections $path
    foreach ($ms in $modelSections) {
        if (-not $s.ContainsKey($ms)) { $script:fail += "${file}: no [$ms]"; continue }
        $m = Models $s[$ms] $file $ms
        foreach ($k in $m.Keys) { if ($families -notcontains $k) { $script:fail += "${file} [$ms]: family $k not served here" } }
        foreach ($f in $families) { Same $m[$f] $want[$f] "$file [$ms] $f" }
    }
    foreach ($i in $installs) { if (-not $s.ContainsKey($i)) { $script:fail += "${file}: no [$i]" } }
    if (-not (($s['SourceDisksFiles'] -join ' ') -match '^xpad98\.sys=1$')) { $script:fail += "${file}: SourceDisksFiles" }
}

Expect 'xp98me.inf' @('Models') @('X360','X360W','GIP') @('XPAD','XPAD.Copy','XPAD.AddReg')
Expect 'xp2k.inf' @('Models') @('X360','GIP') @('XPAD.NT','XPAD.NT.Services','XPAD.Service','XPAD.Copy')
Expect 'xpxpv.inf' @('Models.NTx86','Models.NTamd64') @('GIP') @('XPAD.NT','XPAD.NT.Services','XPAD.Service','XPAD.Copy')

if ($fail.Count) {
    $fail | ForEach-Object { Write-Output "FAIL $_" }
    exit 1
}
Write-Output "PASS infchk: xp98me $($want.X360.Count)+$($want.X360W.Count)+$($want.GIP.Count), xp2k $($want.X360.Count)+$($want.GIP.Count), xpxpv $($want.GIP.Count) x2 ids; no x360c id"
exit 0

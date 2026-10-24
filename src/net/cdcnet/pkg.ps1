# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
#
# pkg.ps1 - stage CDCNET's release folders under out\net\cdcnet\pkg\ and run
# the package gates of build.md section 7 (steps 1-4 and 7) on them. It
# writes only under out\; nothing goes to free\ (a release is a separate,
# owner-requested step). Files only: no program of this project runs.
#
#   pkg.ps1 [-Pfx <file> -PfxPassword <text>]
#
# Folders (ethernet.md section 7.4):
#   w98se-me   x86 NDIS 5.0 CDCNET.SYS, CDCNET.INF (from CDC98.INF)
#   w2k-xp     x86 CDCNET.SYS, CDCNET.INF, CDCRNDIS.INF
#   wv-7-x86   x86 CDCNET.SYS, CDCNET.INF, CDCRNDIS.INF
#   wxp-x64    x64 NDIS 5.1 CDCNET.SYS, CDCNET.INF, CDCRNDIS.INF
#   wv-7-x64   x64 CDCNET.SYS, CDCNET.INF, CDCRNDIS.INF
# Without -Pfx the x64 binary stays an unsigned development artifact and
# the report says so (build.md section 7, step 4).
param([string]$Pfx, [string]$PfxPassword)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$src = $PSScriptRoot
$out = Join-Path $root 'out\net\cdcnet'
$pkg = Join-Path $out 'pkg'
$fail = New-Object System.Collections.Generic.List[string]
$log = New-Object System.Collections.Generic.List[string]

foreach ($p in 'ndis5\free', 'ndis51\free') {
    if (-not (Test-Path (Join-Path $out "$p\PASS.TXT"))) { throw "build $p first (no PASS.TXT)" }
}
$sys86 = Join-Path $out 'ndis5\free\cdcnet.sys'
$sys64 = Join-Path $out 'ndis51\free\cdcnet.sys'

# Version: the resource and every INF's DriverVer agree.
$rc = Get-Content (Join-Path $src 'cdcnet.rc') -Raw
if ($rc -notmatch 'FILEVERSION (\d+),(\d+),(\d+),(\d+)') { throw 'no FILEVERSION' }
$version = "$($Matches[1]).$($Matches[2]).$($Matches[3]).$($Matches[4])"
$log.Add("version $version")

$folders = [ordered]@{
    'w98se-me' = @{ Sys = $sys86; Infs = @{ 'CDCNET.INF' = 'CDC98.INF' }; Machine = 0x14c }
    'w2k-xp'   = @{ Sys = $sys86; Infs = @{ 'CDCNET.INF' = 'CDCNET.INF'; 'CDCRNDIS.INF' = 'CDCRNDIS.INF' }; Machine = 0x14c }
    'wv-7-x86' = @{ Sys = $sys86; Infs = @{ 'CDCNET.INF' = 'CDCNET.INF'; 'CDCRNDIS.INF' = 'CDCRNDIS.INF' }; Machine = 0x14c }
    'wxp-x64'  = @{ Sys = $sys64; Infs = @{ 'CDCNET.INF' = 'CDCNET.INF'; 'CDCRNDIS.INF' = 'CDCRNDIS.INF' }; Machine = 0x8664 }
    'wv-7-x64' = @{ Sys = $sys64; Infs = @{ 'CDCNET.INF' = 'CDCNET.INF'; 'CDCRNDIS.INF' = 'CDCRNDIS.INF' }; Machine = 0x8664 }
}

function Is83([string]$name) {
    return $name -cmatch '^[A-Z0-9_\-]{1,8}(\.[A-Z0-9_\-]{1,3})?$' -and
        $name -notmatch '^(CON|PRN|AUX|NUL|COM\d|LPT\d)(\.|$)'
}

function Machine([string]$file) {
    $b = [IO.File]::ReadAllBytes($file)
    $pe = [BitConverter]::ToInt32($b, 0x3c)
    return [BitConverter]::ToUInt16($b, $pe + 4)
}

if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
New-Item -ItemType Directory $pkg | Out-Null
$signed = $false
foreach ($name in $folders.Keys) {
    $f = $folders[$name]
    $dir = Join-Path $pkg $name
    New-Item -ItemType Directory $dir | Out-Null
    Copy-Item $f.Sys (Join-Path $dir 'CDCNET.SYS')
    foreach ($dest in $f.Infs.Keys) {
        $text = [IO.File]::ReadAllText((Join-Path $src $f.Infs[$dest])) -replace "`r?`n", "`r`n"
        [IO.File]::WriteAllText((Join-Path $dir $dest), $text, [Text.Encoding]::ASCII)
    }
    if ((Machine (Join-Path $dir 'CDCNET.SYS')) -ne $f.Machine) { $fail.Add("${name}: CDCNET.SYS machine is not $('{0:x}' -f $f.Machine)") }
    if ($f.Machine -eq 0x8664 -and $Pfx) {
        $signtool = Join-Path $root 'tools\WinDDK71\bin\x86\signtool.exe'
        & $signtool sign /f $Pfx /p $PfxPassword (Join-Path $dir 'CDCNET.SYS') | Out-Null
        if ($LASTEXITCODE) { $fail.Add("${name}: signing failed") } else { $signed = $true }
    }

    # Step 2: 8.3 names, no case-insensitive collisions.
    $names = @(Get-ChildItem $dir -File | ForEach-Object Name)
    foreach ($n in $names) { if (-not (Is83 $n)) { $fail.Add("${name}: not an 8.3 name: $n") } }
    if (-not (Is83 $name.ToUpper()) -and $name.Length -gt 8) { $fail.Add("folder name not 8.3: $name") }

    foreach ($inf in $f.Infs.Keys) {
        $text = Get-Content (Join-Path $dir $inf)
        $all = $text -join "`n"
        # Step 1: version, interface-level ids, no test ids.
        if ($all -notmatch "DriverVer=\d\d/\d\d/\d{4},$([regex]::Escape($version))") { $fail.Add("${name}\${inf}: DriverVer is not $version") }
        foreach ($m in [regex]::Matches($all, 'USB\\[A-Za-z0-9_&]+')) {
            $id = $m.Value
            if ($id -match '^USB\\Class_') { continue }
            if ($id -eq 'USB\CDCNET') { continue }              # the 9x Ndi\DeviceID value, not a match
            $fail.Add("${name}\${inf}: whole-device id not in the design: $id")
        }
        if ($all -match 'VID_0525') { $fail.Add("${name}\${inf}: test id 0525 in a release INF") }
        # Step 3: every file the INF copies is in the folder.
        $copy = $false
        foreach ($line in $text) {
            if ($line -match '^\[(.+)\]') { $copy = $Matches[1] -match '^Copy' ; continue }
            if ($copy -and $line -match '^([^;=\s]+\.(sys|dll))') {
                if (-not (Test-Path (Join-Path $dir $Matches[1]))) { $fail.Add("${name}\${inf}: copies missing $($Matches[1])") }
            }
        }
    }
    $log.Add("$name : " + ($names -join ' '))
}
if (-not $signed) { $log.Add('x64 CDCNET.SYS: UNSIGNED development artifact (no -Pfx); Vista/7 x64 load needs test mode or enforcement off') }

# Step 7: the manifest, in stable path order.
$manifest = Get-ChildItem $pkg -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($pkg.Length + 1).Replace('\', '/')
    (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLower() + '  ' + $rel
}
[IO.File]::WriteAllLines((Join-Path $out 'pkg-SHA256.TXT'), [string[]]$manifest)

$report = Join-Path $out 'pkg-gate.txt'
$lines = @($log) + @($fail | ForEach-Object { "FAIL: $_" }) + @(if ($fail.Count) { 'result: FAIL' } else { 'result: PASS' })
[IO.File]::WriteAllLines($report, [string[]]$lines)
$lines | ForEach-Object { $_ }
if ($fail.Count) { exit 1 }

# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
#
# Gates for libusb0's built files (build.md section 5.2). They read only
# DUMPBIN output of this package's own binaries, DUMPBIN /EXPORTS listings
# of the Windows 98 SE DDK's import libraries (a kit declaration for 98,
# not proof that the 98 module exports the name: build.md 5.1), and, when
# present, the export-table readings recorded in exports98/.

# Names an import library declares, from DUMPBIN /EXPORTS output:
# decoration (_name@n, __imp_) stripped.
function Read-LibExports([string]$Listing) {
    $names = @{}
    $in = $false
    foreach ($l in Get-Content -LiteralPath $Listing) {
        if ($l -match '^\s*Exports\s*$') { $in = $true; continue }
        if ($l -match '^\s*Summary\s*$') { break }
        if (-not $in) { continue }
        $t = $l.Trim()
        if ($t -eq '' -or $t -match '^ordinal') { continue }
        $n = ($t -split '\s+')[-1] -replace '^__imp_', ''
        # The name as listed, without one C-decoration underscore, and
        # without a stdcall @n suffix (fastcall: @name@n): _strlwr is __strlwr in
        # the library, ZwClose is _ZwClose@4.
        $u = $n -replace '^[_@]', ''
        foreach ($v in @($n, $u, ($n -replace '@\d+$', ''), ($u -replace '@\d+$', ''))) { $names[$v.ToUpperInvariant()] = $true }
    }
    if ($names.Count -lt 10) { throw "Library export listing looks empty: $Listing" }
    return $names
}

# Names a recorded export table lists ("ordinal name" per line, '#' comments).
function Read-ExportTable([string]$Path) {
    $names = @{}
    foreach ($l in Get-Content -LiteralPath $Path) {
        $t = $l.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $f = $t -split '\s+'
        if ($f.Count -lt 2) { continue }
        $names[$f[-1].ToUpperInvariant()] = $true
    }
    return $names
}

# PE contract: machine, subsystem kind and version, no delay imports.
function Assert-Pe([string]$Headers, [string]$Subsystem, [string]$Version, [bool]$Dll) {
    $h = Get-Content -LiteralPath $Headers
    if (-not ($h -match '^\s*14C machine \(i386\)')) { throw "${Headers}: machine is not i386" }
    if (-not ($h -match ('^\s*' + [regex]::Escape($Version) + ' subsystem version'))) { throw "${Headers}: subsystem version is not $Version" }
    if (-not ($h -match ('^\s*\d+ subsystem \(' + $Subsystem + '\)'))) { throw "${Headers}: subsystem is not $Subsystem" }
    $isDll = [bool]($h -match '^\s*DLL\s*$')
    if ($isDll -ne $Dll) { throw "${Headers}: DLL flag is $isDll, expected $Dll" }
    foreach ($line in $h) {
        if ($line -match '^\s*([0-9A-Fa-f]+)\s+\[\s*([0-9A-Fa-f]+)\]\s+RVA \[size\] of Delay Import Directory') {
            if ([Convert]::ToUInt64($Matches[1], 16) -or [Convert]::ToUInt64($Matches[2], 16)) { throw "${Headers}: delay imports" }
        }
    }
}

# Every import: named (no ordinal), MODULE!name in the package allowlist,
# and declared by the 98 DDK import library mapped to that module. With
# $Tables (module -> export-table file), also present in the recorded 98
# SE / ME export table of that module (exports98/, read from the target
# systems' own files: names and ordinals only).
$script:tableHits = 0

# Export tables recorded in exports98/<module>-<98|me>.txt, by system.
function Get-Tables98([string]$Dir) {
    $t = @{ '98 SE' = @{}; 'ME' = @{} }
    foreach ($f in Get-ChildItem -LiteralPath $Dir -Filter '*.txt') {
        if ($f.BaseName -notmatch '^(.+)-(98|me)$') { continue }
        $sys = if ($Matches[2] -eq '98') { '98 SE' } else { 'ME' }
        $mod = $Matches[1].ToUpperInvariant() + $(if ($Matches[1] -eq 'usbd') { '.SYS' } else { '.DLL' })
        $t[$sys][$mod] = Read-ExportTable $f.FullName
    }
    if ($t['98 SE'].Count -lt 5 -or $t['ME'].Count -lt 6) { throw "Export tables missing from $Dir" }
    return $t
}

function Assert-Imports([string]$Imports, [string]$Allowed, [hashtable]$Libs, [hashtable]$Tables) {
    $allow = @{}
    foreach ($l in Get-Content -LiteralPath $Allowed) {
        $t = $l.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        if ($t -notmatch '^([A-Za-z0-9_.]+)!([A-Za-z_][A-Za-z0-9_]*(@\d+)?)$') { throw "Bad allowlist line: $t" }
        $allow[$Matches[1].ToUpperInvariant() + '!' + $Matches[2].ToUpperInvariant()] = $true
    }
    $module = $null; $active = $false; $done = $false; $found = @()
    foreach ($line in Get-Content -LiteralPath $Imports) {
        if ($line -match 'delay[- ]load') { throw 'Delay import section' }
        if ($line -match '^\s*Section contains the following imports:') { $active = $true; continue }
        if (-not $active) { continue }
        if ($line -match '^\s*Summary') { $done = $true; break }
        if (-not $line.Trim()) { continue }
        if ($line -match '^\s{4}(\S+\.(exe|dll|sys))\s*$') { $module = $Matches[1].ToUpperInvariant(); continue }
        if ($line -match '^\s*[0-9A-Fa-f]+\s+(Import Address Table|Import Name Table|time date stamp|Index of first forwarder reference)\s*$') { continue }
        if ($line -match '^\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_]*(@\d+)?)\s*$') {
            $name = $Matches[1]
            $key = "$module!$($name.ToUpperInvariant())"
            if (-not $allow.ContainsKey($key)) { throw "Import not in allowlist: $module!$name" }
            if (-not $Libs.ContainsKey($module)) { throw "No 98 DDK library mapped for module $module" }
            if (-not ($Libs[$module].ContainsKey($name.ToUpperInvariant()) -or $Libs[$module].ContainsKey(($name -replace '@\d+$', '').ToUpperInvariant()))) { throw "Import not declared by the 98 DDK library for ${module}: $name" }
            if ($Tables) {
                foreach ($sys in $Tables.Keys) {
                    $t = $Tables[$sys]
                    # Modules with no PE export table to read (NTOSKRNL and HAL
                    # are NTKERN.VXD on 9x) are left to the DDK library check.
                    if (-not $t.ContainsKey($module)) { continue }
                    if (-not $t[$module].ContainsKey($name.ToUpperInvariant())) { throw "Import absent from the $sys export table of ${module}: $name" }
                    $script:tableHits++
                }
            }
            $found += "$module!$name"
            continue
        }
        throw "Unparsed import row (ordinal?): $line"
    }
    if (-not $done -or $found.Count -lt 5) { throw "Incomplete import report: $Imports" }
    return ,$found
}

# The DLL's export names are exactly the expected set (from the .def).
function Assert-Exports([string]$Exports, [string]$Def) {
    $want = @()
    $in = $false
    foreach ($l in Get-Content -LiteralPath $Def) {
        $t = ($l -replace ';.*$', '').Trim()
        if (-not $t) { continue }
        if ($t -eq 'EXPORTS') { $in = $true; continue }
        if ($in) { $want += ($t -split '\s+')[0] }
    }
    $got = @()
    $in = $false
    foreach ($l in Get-Content -LiteralPath $Exports) {
        if ($l -match '^\s*ordinal\s+hint\s+RVA\s+name') { $in = $true; continue }
        if ($l -match '^\s*Summary') { break }
        if (-not $in) { continue }
        if ($l -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $got += $Matches[1] }
    }
    $d = @(Compare-Object ($want | Sort-Object) ($got | Sort-Object) -CaseSensitive)
    if ($d.Count) { throw ('DLL exports differ from the .def: ' + (($d | ForEach-Object { "$($_.SideIndicator)$($_.InputObject)" }) -join ' ')) }
    return $got.Count
}

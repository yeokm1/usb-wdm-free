# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Gates for usbmtp's built files and INF (docs/design/build.md 5.2,
# docs/design/mtp.md 5.5). They read only this package's own files, DUMPBIN
# output of its own binaries, the 98 DDK's WDM.H and USBDLIB.H, and the
# export table recorded in exports98/ (ME's USBD.SYS, read as an export
# table: names and ordinals only). The import checks follow usbprn's gate
# (src/print/usbprn/gate.ps1).

# ---- USBMTP.SYS --------------------------------------------------------------

# x86 native 4.00 PE; no delay imports or ordinals; imports only from
# NTOSKRNL.EXE, HAL.DLL and USBD.SYS; every import in the package allowlist
# AND declared in the 98 DDK's WDM.H (USBD.SYS's in its USBDLIB.H, by the
# name without its stdcall decoration); a USBD.SYS import also in ME's
# recorded USBD.SYS export table, decorated as imported. 98 SE's USBD.SYS
# table is not recorded: there the guest load test decides (build.md 5.1).
function Assert-SysImports([string]$Headers, [string]$Imports, [string]$Allowed, [string]$Wdm98, [string]$Usbd98, [string]$UsbdMe) {
    $h = Get-Content -LiteralPath $Headers
    foreach ($re in @('^\s*14C machine', '^\s*4\.00 subsystem version', '^\s*1 subsystem \(Native\)')) {
        if (-not ($h -match $re)) { throw "SYS PE contract missing: $re" }
    }
    foreach ($line in $h) {
        if ($line -match '^\s*([0-9A-Fa-f]+)\s+\[\s*([0-9A-Fa-f]+)\]\s+RVA \[size\] of Delay Import Directory') {
            if ([Convert]::ToUInt64($Matches[1], 16) -or [Convert]::ToUInt64($Matches[2], 16)) { throw 'Delay imports forbidden' }
        }
    }
    $allow = Read-Allow $Allowed
    $decl = @{ 'NTOSKRNL.EXE' = [IO.File]::ReadAllText($Wdm98); 'HAL.DLL' = [IO.File]::ReadAllText($Wdm98);
               'USBD.SYS' = [IO.File]::ReadAllText($Usbd98) }
    $me = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($l in Get-Content -LiteralPath $UsbdMe) {
        $t = $l.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $f = $t -split '\s+'
        if ($f.Count -ge 2) { [void]$me.Add($f[-1]) }
    }
    if ($me.Count -lt 10) { throw "Export table looks empty: $UsbdMe" }
    $found = @(Read-Imports $Imports)
    $usbd = 0
    foreach ($key in $found) {
        $m, $name = $key.Split('!')
        if (-not $decl.ContainsKey($m)) { throw "SYS imports a module outside NTOSKRNL.EXE, HAL.DLL and USBD.SYS: $m" }
        if (-not $allow.Contains($key)) { throw "Import not in allowlist: $key" }
        $plain = $name
        if ($m -eq 'USBD.SYS') {
            if ($name -notmatch '^_([A-Za-z][A-Za-z0-9_]*)@\d+$') { throw "USBD.SYS import not in stdcall form: $name" }
            $plain = $Matches[1]
            if (-not $me.Contains($name)) { throw "Import absent from the ME export table of USBD.SYS: $name" }
            $usbd++
        } elseif ($name -match '@') { throw "Decorated import from ${m}: $name" }
        if ($decl[$m] -notmatch ('\b' + [regex]::Escape($plain) + '\b')) { throw "Import not declared in the 98 DDK headers: $key" }
    }
    if ($found.Count -lt 20) { throw 'Incomplete import report' }
    return "PASS: x86 native 4.00; $($found.Count) named imports, all allowlisted and declared in the 98 DDK WDM.H (USBD.SYS: USBDLIB.H), the $usbd from USBD.SYS also in ME's USBD.SYS export table; no ordinals or delay loads"
}

# MODULE!Name keys: the module upper-cased, the symbol as written (symbol
# names are compared case-sensitively, as the loader does).
function Read-Allow([string]$Allowed) {
    $allow = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($l in Get-Content -LiteralPath $Allowed) {
        $t = $l.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        if ($t -notmatch '^([A-Za-z0-9_.]+)!([A-Za-z_][A-Za-z0-9_]*(@\d+)?)$') { throw "Bad allowlist line: $t" }
        [void]$allow.Add($Matches[1].ToUpperInvariant() + '!' + $Matches[2])
    }
    return ,$allow
}

# MODULE!name of every import in a DUMPBIN /IMPORTS report; throws on an
# ordinal import, a delay load or an unparsed row.
function Read-Imports([string]$Imports) {
    $module = $null; $active = $false; $done = $false; $found = @()
    foreach ($line in Get-Content -LiteralPath $Imports) {
        if ($line -match 'delay[- ]load') { throw 'Delay import section forbidden' }
        if ($line -match '^\s*Section contains the following imports:') { $active = $true; continue }
        if (-not $active) { continue }
        if ($line -match '^\s*Summary') { $done = $true; break }
        if (-not $line.Trim()) { continue }
        if ($line -match '^\s{4}(\S+\.(exe|dll|sys|drv))\s*$') { $module = $Matches[1].ToUpperInvariant(); continue }
        if ($line -match '^\s*[0-9A-Fa-f]+\s+(Import Address Table|Import Name Table|time date stamp|Index of first forwarder reference)\s*$') { continue }
        if ($line -match '^\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_]*(@\d+)?)\s*$') { $found += "$module!$($Matches[1])"; continue }
        throw "Unparsed import row (ordinal?): $line"
    }
    if (-not $done) { throw "Incomplete import report: $Imports" }
    return $found
}

# ---- USBMTP.INF ----------------------------------------------------------------

function Read-Inf([string]$Text) {
    if ($Text -match '[^\x09\x0a\x0d\x20-\x7e]') { throw 'INF must be ASCII text' }
    $sections = [ordered]@{}; $cur = $null
    foreach ($raw in ($Text -split "`r?`n")) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith(';')) { continue }
        if ($line -match '^\[([^\]]+)\]$') {
            $cur = $Matches[1].ToUpperInvariant()
            if ($sections.Contains($cur)) { throw "Duplicate section $cur" }
            $sections[$cur] = New-Object System.Collections.ArrayList
        } elseif ($cur) { [void]$sections[$cur].Add($line) }
        else { throw "Line outside a section: $line" }
    }
    return $sections
}

function Get-Strings($s) {
    $str = @{}
    foreach ($l in $s['STRINGS']) {
        if ($l -notmatch '^([A-Za-z][A-Za-z0-9]*)\s*=\s*"([^"%]+)"$') { throw "Bad string line: $l" }
        if ($str.ContainsKey($Matches[1])) { throw "Duplicate string $($Matches[1])" }
        $str[$Matches[1]] = $Matches[2]
    }
    return $str
}

function Assert-Exact($s, [string]$section, [string[]]$expected) {
    $got = @($s[$section] | ForEach-Object { ($_ -replace '\s*=\s*', '=') -replace '\s*,\s*', ',' })
    $exp = @($expected | ForEach-Object { ($_ -replace '\s*=\s*', '=') -replace '\s*,\s*', ',' })
    if ($got.Count -ne $exp.Count -or @(Compare-Object $exp $got -CaseSensitive:$false -SyncWindow 0).Count) {
        throw "Section [$section] differs from the package contract"
    }
}

function Assert-StringRefs($s, $str) {
    foreach ($k in $s.Keys) {
        if ($k -eq 'STRINGS') { continue }
        foreach ($l in $s[$k]) {
            foreach ($m in [regex]::Matches($l, '%([^%]+)%')) {
                if (-not $str.ContainsKey($m.Groups[1].Value)) { throw "Undefined string $($m.Groups[1].Value)" }
            }
        }
    }
}

# USBMTP.INF (98 SE and ME): the exact sections; its own class and GUID;
# the class match 06/01/01 only, never a whole class (USB\Class_06 or
# USB\Class_FF, 3.2); the NTKERN loader values; the one file.
function Assert-UsbmtpInf([string]$Text) {
    $s = Read-Inf $Text
    $want = @('VERSION','CLASSINSTALL','USBMTP.CLASSREG','MANUFACTURER','MODELS','USBMTP','USBMTP.ADDREG','USBMTP.SYS',
              'DESTINATIONDIRS','SOURCEDISKSNAMES','SOURCEDISKSFILES','STRINGS')
    if (@(Compare-Object $want @($s.Keys)).Count) { throw 'USBMTP.INF: unexpected or missing section' }
    Assert-Exact $s 'VERSION' @('Signature="$CHICAGO$"', 'Class=UsbMtpDevices', 'ClassGUID={2EFBDFB0-C5E2-4591-8903-72D29C3A8A4B}',
        'Provider=%Provider%', ($s['VERSION'] | Where-Object { $_ -match '^DriverVer\s*=' }))
    $dv = @($s['VERSION'] | Where-Object { $_ -match '^DriverVer\s*=\s*(\d{2}/\d{2}/\d{4}),(\d+\.\d+\.\d+\.\d+)$' })
    if ($dv.Count -ne 1) { throw 'Missing or invalid DriverVer' }
    $date = [datetime]::MinValue
    if (-not [datetime]::TryParseExact(($dv[0] -replace '^DriverVer\s*=\s*', '').Split(',')[0], 'MM/dd/yyyy',
        [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$date)) {
        throw 'Invalid DriverVer date'
    }
    Assert-Exact $s 'CLASSINSTALL' @('AddReg=UsbMtp.ClassReg')
    Assert-Exact $s 'USBMTP.CLASSREG' @('HKR,,,,%ClassName%', 'HKR,,Icon,,"-20"')
    Assert-Exact $s 'MANUFACTURER' @('%Provider%=Models')
    Assert-Exact $s 'MODELS' @('%Device%=UsbMtp,USB\Class_06&SubClass_01&Prot_01')
    Assert-Exact $s 'USBMTP' @('CopyFiles=UsbMtp.Sys', 'AddReg=UsbMtp.AddReg')
    Assert-Exact $s 'USBMTP.ADDREG' @('HKR,,DevLoader,,*ntkern', 'HKR,,NTMPDriver,,USBMTP.SYS')
    Assert-Exact $s 'USBMTP.SYS' @('USBMTP.SYS')
    Assert-Exact $s 'DESTINATIONDIRS' @('UsbMtp.Sys=10,System32\Drivers')
    Assert-Exact $s 'SOURCEDISKSNAMES' @('1=%Disk%,,,')
    Assert-Exact $s 'SOURCEDISKSFILES' @('USBMTP.SYS=1')
    Assert-StringRefs $s (Get-Strings $s)
}

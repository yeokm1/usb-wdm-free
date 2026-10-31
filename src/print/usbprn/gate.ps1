# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Gates for usbprn's built files and INFs (build.md section 5.2). They read
# only this package's own files, DUMPBIN output of its own binaries, the 98
# DDK's WDM.H, and the export tables recorded in exports98/ (98 SE and ME
# system files read as export tables only: names and ordinals).

# ---- USBPRN.SYS --------------------------------------------------------------

# x86 native 4.00 PE; no delay imports or ordinals; every import in the
# package allowlist AND declared in the 98 DDK's WDM.H.
function Assert-SysImports([string]$Headers, [string]$Imports, [string]$Allowed, [string]$Wdm98) {
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
    $wdm = [IO.File]::ReadAllText($Wdm98)
    $found = @(Read-Imports $Imports)
    foreach ($key in $found) {
        $name = $key.Split('!')[1]
        if (-not $allow.Contains($key)) { throw "Import not in allowlist: $key" }
        if ($wdm -notmatch ('\b' + [regex]::Escape($name) + '\b')) { throw "Import not declared in 98 WDM.H: $key" }
    }
    if ($found.Count -lt 20) { throw 'Incomplete import report' }
    return "PASS: x86 native 4.00; $($found.Count) named imports, all allowlisted and declared in the 98 DDK WDM.H; no ordinals or delay loads"
}

# MODULE!Name keys: the module upper-cased, the symbol as written (symbol
# names are compared case-sensitively, as the loader does).
function Read-Allow([string]$Allowed) {
    $allow = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($l in Get-Content -LiteralPath $Allowed) {
        $t = $l.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        if ($t -notmatch '^([A-Za-z0-9_.]+)!([A-Za-z_][A-Za-z0-9_]*)$') { throw "Bad allowlist line: $t" }
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
        if ($line -match '^\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*$') { $found += "$module!$($Matches[1])"; continue }
        throw "Unparsed import row (ordinal?): $line"
    }
    if (-not $done) { throw "Incomplete import report: $Imports" }
    return $found
}

# ---- USBPRNMN.DLL ------------------------------------------------------------

function Read-ExportTable([string]$Path) {
    $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($l in Get-Content -LiteralPath $Path) {
        $t = $l.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $f = $t -split '\s+'
        if ($f.Count -lt 2) { continue }
        [void]$names.Add($f[-1])
    }
    if ($names.Count -lt 10) { throw "Export table looks empty: $Path" }
    return ,$names
}

# Win32 x86 DLL, subsystem 4.00; every import allowlisted and present in
# the recorded 98 SE AND ME export tables of its module; no other modules.
function Assert-DllImports([string]$Headers, [string]$Imports, [string]$Allowed, [string]$TableDir) {
    $h = Get-Content -LiteralPath $Headers
    if (-not ($h -match '^\s*14C machine \(i386\)')) { throw 'DLL: machine is not i386' }
    if (-not ($h -match '^\s*4\.00 subsystem version')) { throw 'DLL: subsystem version is not 4.00' }
    if (-not ($h -match '^\s*2 subsystem \(Windows GUI\)')) { throw 'DLL: subsystem is not Windows GUI' }
    if (-not ($h -match '^\s*DLL\s*$')) { throw 'DLL: the DLL flag is not set' }
    foreach ($line in $h) {
        if ($line -match '^\s*([0-9A-Fa-f]+)\s+\[\s*([0-9A-Fa-f]+)\]\s+RVA \[size\] of Delay Import Directory') {
            if ([Convert]::ToUInt64($Matches[1], 16) -or [Convert]::ToUInt64($Matches[2], 16)) { throw 'DLL: delay imports' }
        }
    }
    $files = @{ 'KERNEL32.DLL' = 'kernel32'; 'USER32.DLL' = 'user32'; 'ADVAPI32.DLL' = 'advapi32'; 'WINSPOOL.DRV' = 'winspool' }
    $tables = @{}
    foreach ($m in $files.Keys) {
        foreach ($sys in '98', 'me') {
            $tables["$m|$sys"] = Read-ExportTable (Join-Path $TableDir "$($files[$m])-$sys.txt")
        }
    }
    $allow = Read-Allow $Allowed
    $found = @(Read-Imports $Imports)
    foreach ($key in $found) {
        $m, $name = $key.Split('!')
        if (-not $files.ContainsKey($m)) { throw "DLL imports a module outside kernel32, user32, advapi32 and winspool.drv: $m" }
        if (-not $allow.Contains($key)) { throw "Import not in allowlist: $key" }
        foreach ($sys in '98', 'me') {
            if (-not $tables["$m|$sys"].Contains($name)) { throw "Import absent from the $sys export table of ${m}: $name" }
        }
    }
    if ($found.Count -lt 10) { throw 'DLL: incomplete import report' }
    return "PASS: Win32 x86 DLL 4.00; $($found.Count) named imports, all allowlisted and in the 98 SE and ME export tables of kernel32, user32, advapi32, winspool.drv"
}

# The DLL's exports are exactly the .def's.
function Assert-Exports([string]$Exports, [string]$Def) {
    $want = @(); $in = $false
    foreach ($l in Get-Content -LiteralPath $Def) {
        $t = ($l -replace ';.*$', '').Trim()
        if (-not $t) { continue }
        if ($t -eq 'EXPORTS') { $in = $true; continue }
        if ($in) { $want += ($t -split '\s+')[0] }
    }
    $got = @(); $in = $false
    foreach ($l in Get-Content -LiteralPath $Exports) {
        if ($l -match '^\s*ordinal\s+hint\s+RVA\s+name') { $in = $true; continue }
        if ($l -match '^\s*Summary') { break }
        if (-not $in) { continue }
        if ($l -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $got += $Matches[1] }
    }
    $d = @(Compare-Object ($want | Sort-Object) ($got | Sort-Object) -CaseSensitive)
    if ($d.Count -or $want.Count -eq 0) { throw ('DLL exports differ from the .def: ' + (($d | ForEach-Object { "$($_.SideIndicator)$($_.InputObject)" }) -join ' ')) }
    return "PASS: exports exactly $($want -join ', ')"
}

# ---- INFs --------------------------------------------------------------------

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

function Assert-Version($s) {
    Assert-Exact $s 'VERSION' @('Signature="$CHICAGO$"', 'Class=USB', 'ClassGUID={36FC9E60-C465-11CF-8056-444553540000}',
        'Provider=%Provider%', ($s['VERSION'] | Where-Object { $_ -match '^DriverVer\s*=' }))
    $dv = @($s['VERSION'] | Where-Object { $_ -match '^DriverVer\s*=\s*(\d{2}/\d{2}/\d{4}),(\d+\.\d+\.\d+\.\d+)$' })
    if ($dv.Count -ne 1) { throw 'Missing or invalid DriverVer' }
    $date = [datetime]::MinValue
    if (-not [datetime]::TryParseExact(($dv[0] -replace '^DriverVer\s*=\s*', '').Split(',')[0], 'MM/dd/yyyy',
        [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$date)) {
        throw 'Invalid DriverVer date'
    }
}

# The registry values USBPRNMN.DLL reads (UmReadSetting("Name", default)),
# from its source.
function Get-MonitorSettings([string]$Source) {
    $t = [IO.File]::ReadAllText($Source)
    $r = @{}
    # Every call must be the plain form UmReadSetting("Name", <decimal>);
    # the definition is the one other use of the name.
    $all = [regex]::Matches($t, '\bUmReadSetting\s*\(').Count
    $calls = [regex]::Matches($t, '\bUmReadSetting\s*\(\s*"([A-Za-z0-9]+)"\s*,\s*(\d+)\s*\)')
    $defs = [regex]::Matches($t, 'static\s+DWORD\s+UmReadSetting\s*\(').Count
    if ($defs -ne 1 -or $calls.Count + $defs -ne $all) { throw 'A monitor setting read is not of the form UmReadSetting("Name", default)' }
    foreach ($m in $calls) {
        $k = $m.Groups[1].Value; $v = [int]$m.Groups[2].Value
        if ($r.ContainsKey($k) -and $r[$k] -ne $v) { throw "Monitor setting $k read with two defaults" }
        $r[$k] = $v
    }
    if ($r.Count -eq 0) { throw 'No monitor settings found in the source' }
    return $r
}

# USBPRN.INF (98 SE): the exact sections; interface-level class ids only;
# the monitor's RunOnce; every value the monitor reads written at its
# default, no-clobber (owner rule, 2026-10-30).
function Assert-UsbprnInf([string]$Text, [string]$MonitorSource) {
    $s = Read-Inf $Text
    $want = @('VERSION','MANUFACTURER','MODELS','USBPRN','USBPRN.ADDREG','USBPRN.SETTINGS','USBPRN.SYS','USBPRN.DLL',
              'DESTINATIONDIRS','SOURCEDISKSNAMES','SOURCEDISKSFILES','STRINGS')
    if (@(Compare-Object $want @($s.Keys)).Count) { throw 'USBPRN.INF: unexpected or missing section' }
    Assert-Version $s
    Assert-Exact $s 'MANUFACTURER' @('%Provider%=Models')
    Assert-Exact $s 'MODELS' @('%Bidi%=UsbPrn,USB\Class_07&SubClass_01&Prot_02',
        '%Uni%=UsbPrn,USB\Class_07&SubClass_01&Prot_01', '%P1284%=UsbPrn,USB\Class_07&SubClass_01&Prot_03')
    Assert-Exact $s 'USBPRN' @('CopyFiles=UsbPrn.Sys,UsbPrn.Dll', 'AddReg=UsbPrn.AddReg,UsbPrn.Settings')
    Assert-Exact $s 'USBPRN.ADDREG' @('HKR,,DevLoader,,*ntkern', 'HKR,,NTMPDriver,,USBPRN.SYS',
        'HKLM,"Software\Microsoft\Windows\CurrentVersion\RunOnce","UsbPrnMonitor",,"RUNDLL32.EXE USBPRNMN.DLL,InstallMonitor"')
    $settings = Get-MonitorSettings $MonitorSource
    $lines = @($settings.Keys | Sort-Object | ForEach-Object { "HKLM,""Software\usb-wdm-free\usbprn"",""$_"",0x00010003,$($settings[$_])" })
    Assert-Exact $s 'USBPRN.SETTINGS' $lines
    Assert-Exact $s 'USBPRN.SYS' @('USBPRN.SYS')
    Assert-Exact $s 'USBPRN.DLL' @('USBPRNMN.DLL')
    Assert-Exact $s 'DESTINATIONDIRS' @('UsbPrn.Sys=10,System32\Drivers', 'UsbPrn.Dll=11')
    Assert-Exact $s 'SOURCEDISKSNAMES' @('1=%Disk%,,,')
    Assert-Exact $s 'SOURCEDISKSFILES' @('USBPRN.SYS=1', 'USBPRNMN.DLL=1')
    Assert-StringRefs $s (Get-Strings $s)
}

# USBPRNME.INF (ME): binds ME's own usbprint.sys, copies nothing.
function Assert-UsbprnMeInf([string]$Text) {
    $s = Read-Inf $Text
    $want = @('VERSION','MANUFACTURER','MODELS','USBPRINT','USBPRINT.ADDREG','STRINGS')
    if (@(Compare-Object $want @($s.Keys)).Count) { throw 'USBPRNME.INF: unexpected or missing section' }
    Assert-Version $s
    Assert-Exact $s 'MANUFACTURER' @('%Provider%=Models')
    Assert-Exact $s 'MODELS' @('%Bidi%=UsbPrint,USB\Class_07&SubClass_01&Prot_02',
        '%Uni%=UsbPrint,USB\Class_07&SubClass_01&Prot_01')
    Assert-Exact $s 'USBPRINT' @('AddReg=UsbPrint.AddReg')
    Assert-Exact $s 'USBPRINT.ADDREG' @('HKR,,DevLoader,,*ntkern', 'HKR,,NTMPDriver,,usbprint.sys')
    Assert-StringRefs $s (Get-Strings $s)
}

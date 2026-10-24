# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
#
# The LIBUSB0.INF gate (docs/design/libusb.md 6.2, 6.3): fixed sections,
# the exact VID/PID model list, the 9x loader values, the DLL to DIRID 11,
# every copied file in the package, DriverVer equal to the build's version;
# no NT sections, no filters, no class or generic match.

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

function Assert-Exact($s, [string]$section, [string[]]$expected) {
    $norm = { param($x) (($x -replace '\s*=\s*', '=') -replace '\s*,\s*', ',') }
    $got = @($s[$section] | ForEach-Object { & $norm $_ })
    $exp = @($expected | ForEach-Object { & $norm $_ })
    if ($got.Count -ne $exp.Count -or @(Compare-Object $exp $got -CaseSensitive:$false).Count) {
        throw "Section [$section] differs from the package contract"
    }
}

function Assert-LibusbInf([string]$Text, [string]$Version) {
    $s = Read-Inf $Text
    $want = @('VERSION', 'CLASSINSTALL', 'LIBUSB.CLASSREG', 'MANUFACTURER', 'DEVICES', 'LIBUSB', 'LIBUSB.HW',
              'LIBUSB.ADDREG', 'LIBUSB.HWREG', 'LIBUSB.SYS', 'LIBUSB.DLL', 'DESTINATIONDIRS', 'SOURCEDISKSNAMES',
              'SOURCEDISKSFILES', 'STRINGS')
    if (@(Compare-Object $want @($s.Keys)).Count) { throw 'LIBUSB0.INF: unexpected or missing section' }
    $dv = @($s['VERSION'] | Where-Object { $_ -match '^DriverVer\s*=\s*\d{2}/\d{2}/\d{4}\s*,\s*\d+\.\d+\.\d+\.\d+$' })
    if ($dv.Count -ne 1) { throw 'LIBUSB0.INF: missing or invalid DriverVer' }
    $dvm = [regex]::Match($dv[0], '(\d{2}/\d{2}/\d{4})\s*,\s*(\S+)$')
    $date = [datetime]::MinValue
    if (-not [datetime]::TryParseExact($dvm.Groups[1].Value, 'MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::None, [ref]$date)) { throw 'LIBUSB0.INF: invalid DriverVer date' }
    if ($dvm.Groups[2].Value -ne $Version) { throw "LIBUSB0.INF: DriverVer is $($dvm.Groups[2].Value), the build is $Version" }
    Assert-Exact $s 'VERSION' @('Signature="$CHICAGO$"', 'Class=LibUsbDevices',
        'ClassGUID={EB781AAF-9C70-4523-A5DF-642A87ECA567}', 'Provider=%Provider%', $dv[0])
    Assert-Exact $s 'CLASSINSTALL' @('AddReg=LibUsb.ClassReg')
    Assert-Exact $s 'LIBUSB.CLASSREG' @('HKR,,,,%ClassName%', 'HKR,,Icon,,"-20"')
    Assert-Exact $s 'MANUFACTURER' @('%Provider%=Devices')
    Assert-Exact $s 'DEVICES' @('%USBasp%=LibUsb,USB\VID_16C0&PID_05DC', '%USBtiny%=LibUsb,USB\VID_1781&PID_0C9F',
        '%AVRISP2%=LibUsb,USB\VID_03EB&PID_2104', '%JTAGICE2%=LibUsb,USB\VID_03EB&PID_2103',
        '%Dragon%=LibUsb,USB\VID_03EB&PID_2107')
    Assert-Exact $s 'LIBUSB' @('CopyFiles=LibUsb.Sys,LibUsb.Dll', 'AddReg=LibUsb.AddReg')
    Assert-Exact $s 'LIBUSB.HW' @('AddReg=LibUsb.HwReg')
    Assert-Exact $s 'LIBUSB.ADDREG' @('HKR,,DevLoader,,*ntkern', 'HKR,,NTMPDriver,,LIBUSB0.SYS')
    Assert-Exact $s 'LIBUSB.HWREG' @('HKR,,SurpriseRemovalOK,0x00010001,1')
    Assert-Exact $s 'LIBUSB.SYS' @('LIBUSB0.SYS')
    Assert-Exact $s 'LIBUSB.DLL' @('LIBUSB0.DLL')
    Assert-Exact $s 'DESTINATIONDIRS' @('LibUsb.Sys=10,System32\Drivers', 'LibUsb.Dll=11')
    Assert-Exact $s 'SOURCEDISKSNAMES' @('1=%Disk%,,,')
    Assert-Exact $s 'SOURCEDISKSFILES' @('LIBUSB0.SYS=1', 'LIBUSB0.DLL=1')
    $str = @{}
    foreach ($l in $s['STRINGS']) {
        if ($l -notmatch '^([A-Za-z][A-Za-z0-9]*)\s*=\s*"([^"%]+)"$') { throw "LIBUSB0.INF: bad string line: $l" }
        if ($str.ContainsKey($Matches[1])) { throw "LIBUSB0.INF: duplicate string $($Matches[1])" }
        $str[$Matches[1]] = $true
    }
    foreach ($k in $s.Keys) {
        if ($k -eq 'STRINGS') { continue }
        foreach ($l in $s[$k]) {
            foreach ($m in [regex]::Matches($l, '%([^%]+)%')) {
                if (-not $str.ContainsKey($m.Groups[1].Value)) { throw "LIBUSB0.INF: undefined string $($m.Groups[1].Value)" }
            }
        }
    }
}

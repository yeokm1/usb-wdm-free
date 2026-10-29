# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# The model lines of USBCOM.INF (98 SE/ME) and USBCDC.INF (2000 to 8), derived
# from boardids.txt (owner decisions in docs/design/serial.md 13). Used by the
# gates (validate.ps1, gate98.ps1) and by geninf.ps1, which writes them.

# The id that no device reports: the generic model, for manual "Have Disk"
# selection only (Owner, 2026-10-22).
$UsbcomManualId = 'USBCOM\MANUAL_CDC_ACM'

function Read-BoardIds([string]$Path) {
    $rows = @()
    foreach ($l in Get-Content -LiteralPath $Path) {
        if (-not $l.Trim() -or $l.StartsWith('#')) { continue }
        if ($l -notmatch '^([0-9A-F]{4}):([0-9A-F]{4}) (single|composite)\s+([a-z0-9.-]+)\s+(.+)$') {
            throw "Bad boardids line: $l"
        }
        $rows += [pscustomobject]@{ Vid = $Matches[1]; Pid = $Matches[2]; Kind = $Matches[3]; Source = $Matches[4]; Board = $Matches[5] }
    }
    $keys = @($rows | ForEach-Object { "$($_.Vid):$($_.Pid)" })
    if (@($keys | Sort-Object -Unique).Count -ne $keys.Count) { throw 'Duplicate id in boardids.txt' }
    return $rows
}

function Get-BoardString($r) {
    switch ($r.Vid) {
        { $_ -in '2341', '2A03' } { if ($r.Kind -eq 'single') { return 'ArduinoBoot' } else { return 'Arduino' } }
        '1B4F' { if ($r.Kind -eq 'single') { return 'SparkFunBoot' } else { return 'SparkFun' } }
        '239A' { return 'Adafruit' }
        '303A' { return 'Espressif' }
        '0483' { return 'Stm32' }
        '1A86' { return 'Ch9102' }
        default { throw "No string for VID $($r.Vid)" }
    }
}

# The USB serial bridges of USBCOM.INF (design 6.3): string, VID:PID, ports.
# A part with more than one port is listed as a whole device (98 SE's own
# stack and NUSB's EHCI leave the split to Windows' composite parent, and a
# hardware id wins over USB\COMPOSITE) and per function (xHCI98 splits it).
# The gate checks this list against chipcore.c's table, whose "(INF)" rows
# are exactly these ids.
$UsbcomChips = @(
    @('FtdiX', '0403:6015', 1), @('Ft232h', '0403:6014', 1), @('Ft4232h', '0403:6011', 4),
    @('Pl2303', '067B:2303', 1), @('Pl2303', '067B:2304', 1),
    @('Pl2303g', '067B:23A3', 1), @('Pl2303g', '067B:23B3', 1), @('Pl2303g', '067B:23C3', 1),
    @('Pl2303g', '067B:23D3', 1), @('Pl2303g', '067B:23E3', 1), @('Pl2303g', '067B:23F3', 1),
    @('Cp2105', '10C4:EA70', 2), @('Cp2108', '10C4:EA71', 4))

# The ids chipcore.c's family table marks "(INF)" and "(manual only)".
function Get-ChipTableIds([string]$ChipCore, [string]$Mark) {
    $ids = @()
    foreach ($l in Get-Content -LiteralPath $ChipCore) {
        if ($l -match '^\s*\{ 0x([0-9A-Fa-f]{4}), 0x([0-9A-Fa-f]{4}), AC_FAM_[A-Z0-9]+ \},\s*/\*.*\((INF|manual only)\) \*/') {
            if ($Matches[3] -eq $Mark) { $ids += ($Matches[1] + ':' + $Matches[2]).ToUpperInvariant() }
        }
    }
    return $ids
}

# Returns an ordered list of "%Str%,ID" pairs (install section added by the caller).
function Get-UsbcomModels([string]$Inf, [string]$Section, [string]$BoardIds) {
    $rows = Read-BoardIds $BoardIds
    $m = New-Object System.Collections.ArrayList
    $id = { param($r) "USB\VID_$($r.Vid)&PID_$($r.Pid)" }
    if ($Inf -eq 'USBCOM') {
        [void]$m.Add(@('Uno', 'USB\VID_2341&PID_0043'))
        [void]$m.Add(@('Uno', 'USB\VID_2A03&PID_0043'))
        [void]$m.Add(@('PicoWhole', 'USB\VID_2E8A&PID_000A'))
        [void]$m.Add(@('Pico', 'USB\VID_2E8A&PID_000A&MI_00'))
        foreach ($r in $rows) {
            $s = Get-BoardString $r
            [void]$m.Add(@($s, (& $id $r)))
            if ($r.Kind -eq 'composite') { [void]$m.Add(@($s, ((& $id $r) + '&MI_00'))) }
        }
        foreach ($c in $UsbcomChips) {
            $v, $p = $c[1].Split(':')
            [void]$m.Add(@($c[0], "USB\VID_$v&PID_$p"))
            if ($c[2] -gt 1) {
                for ($i = 0; $i -lt $c[2]; $i++) { [void]$m.Add(@($c[0], ("USB\VID_$v&PID_$p&MI_{0:D2}" -f $i))) }
            }
        }
        # Owner, 2026-10-26 (xHCI98 roadmap 44.44): 98 SE and ME have no
        # CDC-ACM COM-port driver of their own (serial.md 2.5, 5.4).
        [void]$m.Add(@('Generic', 'USB\Class_02&SubClass_02'))
        [void]$m.Add(@('Manual', $UsbcomManualId))
    } elseif ($Section -eq 'Models') {                 # USBCDC, Windows 2000
        [void]$m.Add(@('Uno', 'USB\VID_2341&PID_0043'))
        [void]$m.Add(@('Uno', 'USB\VID_2A03&PID_0043'))
        [void]$m.Add(@('PicoWhole', 'USB\VID_2E8A&PID_000A'))
        foreach ($r in $rows) { [void]$m.Add(@((Get-BoardString $r), (& $id $r))) }
        # As on XP: no in-box INF binds a CDC-ACM function (serial.md 2.5).
        [void]$m.Add(@('Generic', 'USB\Class_02&SubClass_02'))
        [void]$m.Add(@('Manual', $UsbcomManualId))
    } else {                                            # USBCDC, XP and later
        [void]$m.Add(@('Uno', 'USB\VID_2341&PID_0043'))
        [void]$m.Add(@('Uno', 'USB\VID_2A03&PID_0043'))
        [void]$m.Add(@('Pico', 'USB\VID_2E8A&PID_000A&MI_00'))
        foreach ($r in $rows) {
            if ($r.Kind -eq 'single') { [void]$m.Add(@((Get-BoardString $r), (& $id $r))) }
        }
        [void]$m.Add(@('Generic', 'USB\Class_02&SubClass_02'))
        [void]$m.Add(@('Manual', $UsbcomManualId))
    }
    return $m
}

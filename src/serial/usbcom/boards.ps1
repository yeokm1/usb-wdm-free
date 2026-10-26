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

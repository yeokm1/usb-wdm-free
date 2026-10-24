# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Small package policy gate; not a general INF parser or Setup substitute.
function Assert-FloppyInf([string]$Text, [ValidateSet('nt','9x','me')][string]$Variant) {
    if ($Text -match '[^\x09\x0a\x0d\x20-\x7e]') { throw 'INF must be ASCII text' }
    $sections = @{}
    $current = $null
    foreach ($raw in ($Text -split "`r?`n")) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith(';')) { continue }
        if ($line -match '^\[([^\]]+)\]$') {
            $current = $Matches[1]
            if ($sections.ContainsKey($current)) { throw "Duplicate section $current" }
            $sections[$current] = @()
        } elseif ($current) { $sections[$current] += $line }
        else { throw 'Directive outside an INF section' }
    }
    $models = @('%CBI%=UFI_CBI,USB\Class_08&SubClass_04&Prot_00',
                '%CB%=UFI_CB,USB\Class_08&SubClass_04&Prot_01')
    $expected = @{
        Version = @('Class=USB','ClassGuid={36FC9E60-C465-11CF-8056-444553540000}',
                    'Provider=%Provider%')
        Models = $models
    }
    if ($Variant -eq 'nt') {
        $expected.Version += 'Signature="$Windows NT$"'
        $expected.Manufacturer = @('%Provider%=Models,NTx86,NTamd64')
        $expected['Models.NTx86'] = $models
        $expected['Models.NTamd64'] = $models
        foreach ($transport in @('CBI','CB')) {
            foreach ($suffix in @('NT','NT.Services')) {
                $expected["UFI_$transport.$suffix"] = @('Include=usbstor.inf',"Needs=USBSTOR_$transport.$suffix")
            }
        }
    } else {
        $expected.Version += 'Signature="$CHICAGO$"'
        $expected.Manufacturer = @('%Provider%=Models')
        $expected.UFI_CBI = @('AddReg=Loader,CBI.Flags')
        $expected.UFI_CB = @('AddReg=Loader,CB.Flags')
        $expected['UFI_CBI.HW'] = @('AddReg=Mapper')
        $expected['UFI_CB.HW'] = @('AddReg=Mapper')
        $expected.Loader = @('HKR,,DevLoader,,*NTKERN','HKR,,NTMPDriver,,USBSTOR.SYS')
        $expected.Mapper = @('HKR,,upperfilters,0,"USBNTMAP.SYS"')
        $expected['CBI.Flags'] = @('HKR,,DriverFlags,0x00010001,2')
        $expected['CB.Flags'] = @('HKR,,DriverFlags,0x00010001,3')
        if ($Variant -eq 'me') {
            $expected.Version += 'LayoutFile=layout.inf,layout1.inf,layout2.inf'
            $expected.UFI_CBI += 'CopyFiles=Storage.Copy'
            $expected.UFI_CB += 'CopyFiles=Storage.Copy'
            $expected.DestinationDirs = @('Storage.Copy=10,system32\drivers')
            $expected['Storage.Copy'] = @('USBSTOR.SYS','USBNTMAP.SYS')
        }
    }
    $required = @($expected.Keys) + 'Strings'
    if (@(Compare-Object $required @($sections.Keys)).Count) { throw 'Unexpected or missing INF section' }
    foreach ($section in $expected.Keys) {
        $actual = @($sections[$section] | ForEach-Object {
            $_ -replace '\s*=\s*','=' -replace '\s*,\s*',','
        })
        if ($section -eq 'Version') {
            $versions = @($actual | Where-Object { $_ -match '^DriverVer=' })
            if ($versions.Count -ne 1 -or $versions[0] -notmatch
                '^DriverVer=(\d{2}/\d{2}/\d{4}),([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)$') { throw 'Invalid DriverVer' }
            $date = [datetime]::MinValue
            if (-not [datetime]::TryParseExact($Matches[1], 'MM/dd/yyyy',
                [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::None, [ref]$date)) { throw 'Invalid DriverVer date' }
            foreach ($part in ($Matches[2] -split '\.')) {
                if ([uint64]$part -gt 65535) { throw 'DriverVer component exceeds 16 bits' }
            }
            $actual = @($actual | Where-Object { $_ -notmatch '^DriverVer=' })
        }
        $canonical = @($expected[$section])
        if ($actual.Count -ne $canonical.Count -or @(Compare-Object $canonical $actual).Count) {
            throw "Unsafe model, dependency or registry setting in $section"
        }
    }
    $strings = @{}
    foreach ($line in $sections.Strings) {
        if ($line -notmatch '^([A-Za-z][A-Za-z0-9]*)\s*=\s*"([^"%]+)"$' -or
            $strings.ContainsKey($Matches[1])) { throw 'Invalid or duplicate string' }
        $strings[$Matches[1]] = $Matches[2]
    }
    foreach ($key in @('Provider','CBI','CB')) {
        if (-not $strings.ContainsKey($key)) { throw "Undefined string $key" }
    }
}

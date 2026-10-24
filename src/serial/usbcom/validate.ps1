# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Policy gate for this deliberately small, INF-only experimental package.
# This is not a general INF parser and does not replace Windows Setup.
function Assert-CdcInf([string]$Text) {
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
        } elseif ($current -and $line -match '^([^=]+)=(.*)$') {
            $sections[$current] += ,@($Matches[1].Trim(), $Matches[2].Trim())
        } else { throw "Unsupported INF syntax: $line" }
    }
    $required = @('Version','Manufacturer','Models','Models.NTx86','Models.NTamd64',
        'ACM.NT','ACM.NT.Services','SerialService','Strings')
    if (@(Compare-Object $required @($sections.Keys)).Count) { throw 'Unexpected or missing INF section' }

    $fixed = @{
        Version = @{
            Signature = '"$Windows NT$"'; Class = 'Ports'
            ClassGuid = '{4D36E978-E325-11CE-BFC1-08002BE10318}'
            Provider = '%Provider%'; DriverVer = $null
        }
        Manufacturer = @{ '%Provider%' = 'Models,NTx86,NTamd64' }
        'ACM.NT' = @{ Include = 'mdmcpq.inf'; CopyFiles = 'FakeModemCopyFileSection' }
        'ACM.NT.Services' = @{ AddService = 'usbser,0x00000002,SerialService' }
        SerialService = @{
            DisplayName = '%Service%'; ServiceType = '1'; StartType = '3'
            ErrorControl = '1'; ServiceBinary = '%12%\usbser.sys'
        }
    }
    foreach ($section in $fixed.Keys) {
        $keys = @{}
        foreach ($pair in $sections[$section]) {
            $key, $value = $pair
            if ($keys.ContainsKey($key) -or -not $fixed[$section].ContainsKey($key)) {
                throw "Unexpected or duplicate directive $section/$key"
            }
            $keys[$key] = $true
            if ($key -eq 'DriverVer') {
                if ($value -notmatch '^(\d{2}/\d{2}/\d{4}),([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)$') {
                    throw 'Invalid DriverVer'
                }
                $date = [datetime]::MinValue
                if (-not [datetime]::TryParseExact($Matches[1], 'MM/dd/yyyy',
                    [Globalization.CultureInfo]::InvariantCulture,
                    [Globalization.DateTimeStyles]::None, [ref]$date)) { throw 'Invalid DriverVer date' }
                foreach ($part in ($Matches[2] -split '\.')) {
                    if ([uint64]$part -gt 65535) { throw 'DriverVer component exceeds 16 bits' }
                }
            } elseif (($value -replace '\s','') -ine ($fixed[$section][$key] -replace '\s','')) {
                throw "Unsafe dependency or setting $section/$key"
            }
        }
        if (@(Compare-Object @($fixed[$section].Keys) @($keys.Keys)).Count) {
            throw "Missing directive in $section"
        }
    }

    # The exact model lines (display string and id, in order), derived from
    # boardids.txt by boards.ps1.
    . (Join-Path $PSScriptRoot 'boards.ps1')
    foreach ($model in @('Models','Models.NTx86','Models.NTamd64')) {
        $expected = @(Get-UsbcomModels 'USBCDC' $model (Join-Path $PSScriptRoot 'boardids.txt') |
            ForEach-Object { ('%' + $_[0] + '%|' + $_[1]).ToUpperInvariant() })
        $got = @()
        foreach ($pair in $sections[$model]) {
            $token, $value = $pair
            if ($token -notmatch '^%[A-Za-z][A-Za-z0-9]*%$' -or
                $value -notmatch '^ACM\s*,\s*((USB|USBCOM)\\[^,\s]+)$') { throw "Unsafe model line in $model" }
            $got += ($token + '|' + $Matches[1]).ToUpperInvariant()
        }
        if ($got.Count -ne $expected.Count -or (Compare-Object $expected $got -SyncWindow 0)) {
            throw "Unsafe model IDs in $model"
        }
    }

    $strings = @{}
    foreach ($pair in $sections['Strings']) {
        $key, $value = $pair
        if ($strings.ContainsKey($key) -or $value -notmatch '^"[^"%]+"$') {
            throw "Invalid or duplicate string $key"
        }
        $strings[$key] = $value
    }
    foreach ($section in $sections.Keys) {
        if ($section -eq 'Strings') { continue }
        foreach ($pair in $sections[$section]) {
            foreach ($token in [regex]::Matches(($pair -join '='), '%([^%]+)%')) {
                $key = $token.Groups[1].Value
                if ($key -ne '12' -and -not $strings.ContainsKey($key)) { throw "Undefined string $key" }
            }
        }
    }
}

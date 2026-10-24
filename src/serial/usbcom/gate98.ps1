# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Gates for the Windows 98 SE / ME half of usbcom (build.md section 5.2).
# They inspect only this package's own built files and its own INFs.

# USBCOM.SYS: x86 native 4.00 PE; no delay imports or ordinals; every
# import is in the package allowlist AND declared in the 98 DDK's WDM.H.
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
    $allow = @{}
    foreach ($l in Get-Content -LiteralPath $Allowed) {
        $t = $l.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        if ($t -notmatch '^([A-Za-z0-9_.]+)!([A-Za-z_][A-Za-z0-9_]*)$') { throw "Bad allowlist line: $t" }
        $allow[$Matches[1].ToUpperInvariant() + '!' + $Matches[2]] = $true
    }
    $wdm = [IO.File]::ReadAllText($Wdm98)
    $module = $null; $active = $false; $done = $false; $found = @()
    foreach ($line in Get-Content -LiteralPath $Imports) {
        if ($line -match 'delay[- ]load') { throw 'Delay import section forbidden' }
        if ($line -match '^\s*Section contains the following imports:') { $active = $true; continue }
        if (-not $active) { continue }
        if ($line -match '^\s*Summary') { $done = $true; break }
        if (-not $line.Trim()) { continue }
        if ($line -match '^\s{4}(\S+\.(exe|dll|sys))\s*$') { $module = $Matches[1].ToUpperInvariant(); continue }
        if ($line -match '^\s*[0-9A-Fa-f]+\s+(Import Address Table|Import Name Table|time date stamp|Index of first forwarder reference)\s*$') { continue }
        if ($line -match '^\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*$') {
            $key = "$module!$($Matches[1])"
            if (-not $allow.ContainsKey($key)) { throw "Import not in allowlist: $key" }
            if ($wdm -notmatch ('\b' + [regex]::Escape($Matches[1]) + '\b')) { throw "Import not declared in 98 WDM.H: $key" }
            $found += $key
            continue
        }
        throw "Unparsed import row (ordinal?): $line"
    }
    if (-not $done -or $found.Count -lt 20) { throw 'Incomplete import report' }
    return "PASS: x86 native 4.00; $($found.Count) named imports, all allowlisted and declared in the 98 DDK WDM.H; no ordinals or delay loads"
}

# USBCOMV.VXD: an LE image, VxD module type, no imported modules, export
# ordinal 1 present and pointing at a DDB named USBCOM with DDB_Size 0x50
# and a nonzero control procedure.
function Assert-Vxd([string]$Path) {
    $d = [IO.File]::ReadAllBytes($Path)
    if ($d.Length -lt 0x200 -or $d[0] -ne 0x4D -or $d[1] -ne 0x5A) { throw 'VXD: no MZ header' }
    $le = [BitConverter]::ToUInt32($d, 0x3C)
    if ($le + 0xC4 -gt $d.Length -or $d[$le] -ne 0x4C -or $d[$le + 1] -ne 0x45) { throw 'VXD: no LE header' }
    $u32 = { param($o) [BitConverter]::ToUInt32($d, [int]($le + $o)) }
    if ($d[$le + 2] -ne 0 -or $d[$le + 3] -ne 0) { throw 'VXD: not little-endian' }
    if ([BitConverter]::ToUInt16($d, $le + 0x08) -ne 2) { throw 'VXD: CPU type is not 386' }
    if ([BitConverter]::ToUInt16($d, $le + 0x0A) -ne 4) { throw 'VXD: OS type is not Windows 386' }
    if (((& $u32 0x10) -band 0x38000) -ne 0x38000) { throw 'VXD: module type is not a VxD' }
    if ((& $u32 0x74) -ne 0) { throw 'VXD: imports modules (VxDs link through int 20h only)' }
    $pageSize = & $u32 0x28
    $objTab = $le + (& $u32 0x40)
    $objCount = & $u32 0x44
    $pageMap = $le + (& $u32 0x48)
    $entry = $le + (& $u32 0x5C)
    $dataPages = & $u32 0x80
    # Entry table: first bundle must hold ordinal 1 as a 32-bit entry.
    $count = $d[$entry]; $type = $d[$entry + 1]
    if ($count -lt 1 -or $type -ne 3) { throw 'VXD: ordinal 1 is not a 32-bit entry' }
    $obj = [BitConverter]::ToUInt16($d, $entry + 2)
    $off = [BitConverter]::ToUInt32($d, $entry + 5)
    if ($obj -lt 1 -or $obj -gt $objCount) { throw 'VXD: bad DDB object' }
    $o = $objTab + 24 * ($obj - 1)
    $vsize = [BitConverter]::ToUInt32($d, $o)
    $pti = [BitConverter]::ToUInt32($d, $o + 12)
    if ($off + 0x50 -gt $vsize) { throw 'VXD: DDB outside its object' }
    $pm = $pageMap + 4 * ($pti - 1 + [Math]::Floor($off / $pageSize))
    $page = ($d[$pm] -shl 16) -bor ($d[$pm + 1] -shl 8) -bor $d[$pm + 2]
    $fo = $dataPages + ($page - 1) * $pageSize + ($off % $pageSize)
    if ($fo + 0x50 -gt $d.Length) { throw 'VXD: DDB outside the file' }
    $name = [Text.Encoding]::ASCII.GetString($d, $fo + 0x0C, 8)
    if ($name -ne 'USBCOMV ') { throw "VXD: DDB name is '$name'" }
    if ([BitConverter]::ToUInt32($d, $fo + 0x40) -ne 0x50) { throw 'VXD: DDB_Size is not 0x50' }
    # DDB_Control_Proc is filled by a load-time fixup: find a 32-bit offset
    # fixup at that field to a nonzero internal target.
    # The field's own page: the DDB may straddle a page boundary.
    $pageIdx = $pti - 1 + [Math]::Floor(($off + 0x18) / $pageSize)
    $fpt = $le + (& $u32 0x68)
    $frt = $le + (& $u32 0x6C)
    $start = $frt + [BitConverter]::ToUInt32($d, $fpt + 4 * $pageIdx)
    $end = $frt + [BitConverter]::ToUInt32($d, $fpt + 4 * ($pageIdx + 1))
    $want = ($off + 0x18) % $pageSize
    $ctl = $false
    $r = $start
    while ($r -lt $end) {
        # LE fixup record: source type, target flags, then either one source
        # offset or (source type bit 20h) a count, the target, an optional
        # additive value (flag 04h; 32-bit with flag 20h) and that many source
        # offsets.
        $st = $d[$r]; $fl = $d[$r + 1]; $r += 2
        $list = ($st -band 0x20) -ne 0
        if ($list) { $cnt = $d[$r]; $r += 1 } else { $so = [BitConverter]::ToInt16($d, $r); $r += 2 }
        if (($fl -band 3) -ne 0) { throw 'VXD: non-internal fixup (import)' }
        if ($fl -band 0x40) { $tobj = [BitConverter]::ToUInt16($d, $r); $r += 2 } else { $tobj = $d[$r]; $r += 1 }
        $toff = 0
        if (($st -band 0x0F) -ne 2) {
            if ($fl -band 0x10) { $toff = [BitConverter]::ToUInt32($d, $r); $r += 4 } else { $toff = [BitConverter]::ToUInt16($d, $r); $r += 2 }
        }
        if ($fl -band 0x04) { if ($fl -band 0x20) { $r += 4 } else { $r += 2 } }
        $sources = @()
        if ($list) { for ($i = 0; $i -lt $cnt; $i++) { $sources += [BitConverter]::ToInt16($d, $r); $r += 2 } } else { $sources = @($so) }
        if (($sources -contains $want) -and ($st -band 0x0F) -eq 7 -and $tobj -ge 1 -and $toff -ne 0) { $ctl = $true }
    }
    if (-not $ctl) { throw 'VXD: no fixup sets the control procedure' }
    if ([BitConverter]::ToUInt16($d, $fo + 0x06) -ne 0) { throw 'VXD: device id must be Undefined_Device_ID' }
    return "PASS: LE VxD (module flags 38000h), no imported modules, ordinal 1 = DDB 'USBCOMV', DDB_Size 50h, control procedure set"
}

# The two 98 INFs: fixed sections, exact model ids, exact loader values,
# every copied file in the package list.
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
    if ($got.Count -ne $exp.Count -or @(Compare-Object $exp $got -CaseSensitive:$false).Count) {
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

function Assert-Version($s, [string]$class, [string]$guid) {
    Assert-Exact $s 'VERSION' @('Signature="$CHICAGO$"', "Class=$class", "ClassGUID=$guid",
        'Provider=%Provider%', ($s['VERSION'] | Where-Object { $_ -match '^DriverVer\s*=' }))
    $dv = @($s['VERSION'] | Where-Object { $_ -match '^DriverVer\s*=\s*(\d{2}/\d{2}/\d{4}),(\d+\.\d+\.\d+\.\d+)$' })
    if ($dv.Count -ne 1) { throw 'Missing or invalid DriverVer' }
    $date = [datetime]::MinValue
    if (-not [datetime]::TryParseExact(($dv[0] -replace '^DriverVer\s*=\s*', '').Split(',')[0], 'MM/dd/yyyy',
        [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$date)) {
        throw 'Invalid DriverVer date'
    }
}

function Assert-Usbcom98Inf([string]$Text) {
    $s = Read-Inf $Text
    $want = @('VERSION','MANUFACTURER','MODELS','USBCOM','USBCOM.ADDREG','USBCOM.COPY',
              'DESTINATIONDIRS','SOURCEDISKSNAMES','SOURCEDISKSFILES','STRINGS')
    if (@(Compare-Object $want @($s.Keys)).Count) { throw 'USBCOM.INF: unexpected or missing section' }
    Assert-Version $s 'USB' '{36FC9E60-C465-11CF-8056-444553540000}'
    Assert-Exact $s 'MANUFACTURER' @('%Provider%=Models')
    # The exact model lines, in order, derived from boardids.txt (boards.ps1).
    . (Join-Path $PSScriptRoot 'boards.ps1')
    $models = @(Get-UsbcomModels 'USBCOM' 'Models' (Join-Path $PSScriptRoot 'boardids.txt') |
        ForEach-Object { ("%$($_[0])%=UsbCom,$($_[1])").ToUpperInvariant() })
    $got = @($s['MODELS'] | ForEach-Object { (($_ -replace '\s*=\s*', '=') -replace '\s*,\s*', ',').ToUpperInvariant() })
    if ($got.Count -ne $models.Count -or (Compare-Object $models $got -SyncWindow 0)) {
        throw 'USBCOM.INF: model lines differ from boardids.txt'
    }
    Assert-Exact $s 'USBCOM' @('CopyFiles=UsbCom.Copy', 'AddReg=UsbCom.AddReg')
    Assert-Exact $s 'USBCOM.ADDREG' @('HKR,,DevLoader,,*ntkern', 'HKR,,NTMPDriver,,USBCOM.SYS')
    Assert-Exact $s 'USBCOM.COPY' @('USBCOM.SYS')
    Assert-Exact $s 'DESTINATIONDIRS' @('UsbCom.Copy=10,System32\Drivers')
    Assert-Exact $s 'SOURCEDISKSNAMES' @('1=%Disk%,,,')
    Assert-Exact $s 'SOURCEDISKSFILES' @('USBCOM.SYS=1')
    Assert-StringRefs $s (Get-Strings $s)
}

function Assert-Usbcomp98Inf([string]$Text) {
    $s = Read-Inf $Text
    $want = @('VERSION','MANUFACTURER','MODELS','USBCOMPORT','USBCOMPORT.ADDREG','USBCOMPORT.COPY',
              'DESTINATIONDIRS','SOURCEDISKSNAMES','SOURCEDISKSFILES','STRINGS')
    if (@(Compare-Object $want @($s.Keys)).Count) { throw 'USBCOMP.INF: unexpected or missing section' }
    Assert-Version $s 'Ports' '{4D36E978-E325-11CE-BFC1-08002BE10318}'
    Assert-Exact $s 'MANUFACTURER' @('%Provider%=Models')
    Assert-Exact $s 'MODELS' @('%Port%=UsbComPort,USBCOM\COMPORT')
    Assert-Exact $s 'USBCOMPORT' @('CopyFiles=UsbComPort.Copy', 'AddReg=UsbComPort.AddReg')
    Assert-Exact $s 'USBCOMPORT.ADDREG' @('HKR,,DevLoader,,*vcomm', 'HKR,,PortDriver,,USBCOMV.VXD',
        'HKR,,Contention,,*vcd', 'HKR,,PortSubClass,1,01', 'HKR,,ConfigDialog,,serialui.dll',
        'HKR,,EnumPropPages,,"serialui.dll,EnumPropPages"',
        'HKR,,DCB,3,1C,00,00,00,80,25,00,00,11,10,00,00,00,00,00,01,00,01,08,00,00,11,13,00,00,00,00,00')
    Assert-Exact $s 'USBCOMPORT.COPY' @('USBCOMV.VXD')
    Assert-Exact $s 'DESTINATIONDIRS' @('UsbComPort.Copy=11')
    Assert-Exact $s 'SOURCEDISKSNAMES' @('1=%Disk%,,,')
    Assert-Exact $s 'SOURCEDISKSFILES' @('USBCOMV.VXD=1')
    Assert-StringRefs $s (Get-Strings $s)
    # The DCB value must be the 28-byte Win32 DCB it claims to be.
    $dcb = ($s['USBCOMPORT.ADDREG'] | Where-Object { $_ -match '^HKR,,DCB,3,' }) -replace '^HKR,,DCB,3,', ''
    $bytes = @($dcb.Split(',') | ForEach-Object { [Convert]::ToByte($_, 16) })
    if ($bytes.Count -ne 28 -or $bytes[0] -ne 28 -or [BitConverter]::ToUInt32([byte[]]$bytes, 4) -ne 9600 -or $bytes[18] -ne 8) {
        throw 'USBCOMP.INF: DCB is not a 28-byte 9600 8N1 Win32 DCB'
    }
}

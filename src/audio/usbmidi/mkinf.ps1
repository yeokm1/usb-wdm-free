# SPDX-License-Identifier: GPL-2.0-only
#
# Writes one usbmidi.inf per release folder from the usbmidi.inf template
# (design 8, 4.2) and checks each: ASCII, CRLF, every %string% defined,
# every referenced section present, the generic MIDIStreaming match only
# in the 98 SE and 2000 folders, no whole-device or class-01-wide ids.
# A test folder adds the simulator's FFFF ids; it is never released.
#
# Copyright (C) 2026 Yeo Kheng Meng

param(
    [Parameter(Mandatory)] [string] $Template,
    [Parameter(Mandatory)] [string] $Destination,
    [Parameter(Mandatory)] [ValidateSet('wdmx86', 'wdm64')] [string] $Profile
)
$ErrorActionPreference = 'Stop'

$generic = '%Generic%=UM,USB\Class_01&SubClass_03&Prot_00'
$manual  = '%Manual%=UM,USBMIDI_BY_HAND'
# The device list (design 4.2): AC+MS devices, by their grouped function id.
# On ME and XP to 8 Windows' own driver serves these, so it stays empty there.
$list    = @('%UmOneTab%=UM,USB\VID_0582&PID_0168&MI_00')
$sim     = @('%Simulation1%=UM,USB\VID_FFFF&PID_0001&MI_00',
             '%Simulation2%=UM,USB\VID_FFFF&PID_0002&MI_00')

if ($Profile -eq 'wdmx86') {
    $folders = [ordered]@{
        'w98se'    = @{ Tags = @('X86', 'W9X'); Models = @($generic) + $list + @($manual) }
        'wme'      = @{ Tags = @('X86', 'W9X'); Models = @($manual) }
        'w2k'      = @{ Tags = @('X86');        Models = @($generic) + $list + @($manual) }
        'wxp'      = @{ Tags = @('X86');        Models = @($manual) }
        'wv-8-x86' = @{ Tags = @('X86');        Models = @($manual) }
        'test'     = @{ Tags = @('X86', 'W9X'); Models = @($generic) + $list + $sim + @($manual) }
    }
} else {
    $folders = [ordered]@{
        'wxp-x64'  = @{ Tags = @('X64'); Models = @($manual) }
        'wv-8-x64' = @{ Tags = @('X64'); Models = @($manual) }
        'test-x64' = @{ Tags = @('X64'); Models = @($generic) + $list + $sim + @($manual) }
    }
}

$text = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $Template).Path)
if ($text.ToCharArray() | Where-Object { [int]$_ -gt 127 }) { throw 'template is not ASCII' }
$lines = $text -split '\r?\n'

foreach ($name in $folders.Keys) {
    $f = $folders[$name]
    $out = New-Object System.Collections.Generic.List[string]
    $keep = $true
    foreach ($l in $lines) {
        if ($l -match '^;@IF (\w+)\s*$') { $keep = $f.Tags -contains $Matches[1]; continue }
        if ($l -match '^;@END\s*$') { $keep = $true; continue }
        if (-not $keep) { continue }
        if ($l -match '^;@MODELS\s*$') {
            $out.Add($(if ($f.Tags -contains 'X64') { '[Models.NTamd64]' } else { '[Models]' }))
            foreach ($m in $f.Models) { $out.Add($m) }
            continue
        }
        $out.Add($l)
    }
    $inf = ($out -join "`r`n").TrimEnd() + "`r`n"

    # Checks.
    $strings = @{}
    $sections = @{}
    $section = $null
    foreach ($l in $out) {
        if ($l -match '^\[([^\]]+)\]') { $section = $Matches[1].ToUpperInvariant(); $sections[$section] = $true; continue }
        if ($section -eq 'STRINGS' -and $l -match '^\s*([^=;\s]+)\s*=') { $strings[$Matches[1].ToUpperInvariant()] = $true }
    }
    foreach ($m in [regex]::Matches($inf, '%([A-Za-z_][A-Za-z0-9_]*)%')) {
        $k = $m.Groups[1].Value.ToUpperInvariant()
        if (-not $strings.ContainsKey($k)) { throw "${name}: %$k% is not in [Strings]" }
    }
    $install = if ($f.Tags -contains 'X64') { 'UM.NTAMD64' } else { 'UM.NT' }
    foreach ($s in @('VERSION', 'MANUFACTURER', 'SOURCEDISKSNAMES', 'SOURCEDISKSFILES', 'DESTINATIONDIRS',
                     'DRIVER.COPY', 'MIDISUPPORT', 'SERVICE', 'STRINGS', $install, "$install.SERVICES")) {
        if (-not $sections.ContainsKey($s)) { throw "${name}: section [$s] missing" }
    }
    if ($f.Tags -contains 'W9X' -and -not $sections.ContainsKey('UM')) { throw "${name}: 9x section [UM] missing" }
    $ids = $f.Models | ForEach-Object { ($_ -split ',', 2)[1] }
    foreach ($id in $ids) {
        if ($id -match '^USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}$') { throw "${name}: whole-device id $id" }
        if ($id -match '^USB\\Class_01(&SubClass_0[0-2])?$' -or $id -match 'SubClass_0[012]') { throw "${name}: audio-wide id $id" }
        if ($id -match 'Class_01&SubClass_03' -and $name -notin @('w98se', 'w2k', 'test', 'test-x64')) {
            throw "${name}: generic MIDIStreaming match outside 98 SE/2000"
        }
    }

    $dir = Join-Path $Destination $name
    New-Item -ItemType Directory -Force $dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir 'usbmidi.inf'), $inf, [Text.Encoding]::ASCII)
    "inf ${name}: $($f.Models.Count) models, tags $($f.Tags -join ',')"
}

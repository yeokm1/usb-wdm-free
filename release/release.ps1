# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
#
# release.ps1 - the release operation of docs/design/build.md section 7.
#
#   release.ps1 -Definition release\0.1.0.psd1 [-Part x86|all] [-X64Dir <signed x64 drivers>]
#               [-NonFree <usb-wdm-nonfree checkout>] [-Certificate <public .cer>]
#               [-Thumbprint <signer SHA-1>] [-OutRoot <checkout holding out\>] [-Check]
#
# Copies validated build results (out\...) and INFs into free\<class>\<pkg>\<folder>\,
# as the definition lists them, and each tool (Tools) into free\tools\<name>\, then:
#   1. checks each INF: DriverVer, Signature, AddService names against the package's,
#      every file it copies present in its folder (or from Windows through LayoutFile,
#      Include/Needs, reported), whole-device hardware ids only where the definition
#      records them, no bare USB\Class_xx id; each image's architecture (x86 unsigned,
#      x64 with an embedded signature by -Thumbprint whose SHA-1 image digest matches);
#   2. checks every file and folder name under free\ for 8.3, reserved DOS names and
#      case-insensitive collisions;
#   3. checks each folder holds exactly its files (README.TXT and declared extras aside);
#   4. checks each tool: no INF (a tool is not a driver package and has no STAGE.TXT
#      line), each image x86, unsigned and importing only the DLLs its entry lists,
#      each text file plain ASCII, CRLF, at most 78 columns;
#   7. writes STAGE.TXT (STAGE/COPY lines, PACKAGE lines, relations) and checks every
#      INF under free\ is on exactly one line and no id is staged twice on one system
#      (with -NonFree, over both sets), then rewrites SHA256.TXT over free\ and checks it.
# -Part x86 leaves the x64 folders out (before the owner signs). -Check builds the tree
# in a scratch folder and touches nothing in the repository. -OutRoot takes the packages'
# build results (out\..., with their records) from another checkout of this repository,
# such as the worktree a release was first made in; a tool is built in this checkout by
# its own build script. Never signs anything and never reads a private key: the x64
# images arrive signed in -X64Dir.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Definition,
    [ValidateSet('x86', 'all')][string]$Part = 'all',
    [string]$X64Dir = '',
    [string]$NonFree = '',
    [string]$Certificate = '',
    [string]$Thumbprint = '814CF778B45159EC2659C39C16C7E75EB3C37145',
    [string]$OutRoot = '',
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildRoot = if ($OutRoot -ne '') { [IO.Path]::GetFullPath($OutRoot).TrimEnd('\') } else { $Root }
$script:Errors = New-Object System.Collections.Generic.List[string]
$script:Notes = New-Object System.Collections.Generic.List[string]
function Fail([string]$m) { $script:Errors.Add($m) }
function Note([string]$m) { $script:Notes.Add($m) }

$Systems = @('W98SE', 'WME', 'W2K', 'WXP', 'WXP64', 'WVISTA', 'WVISTA64', 'W7', 'W7X64', 'W8', 'W8X64', 'W10', 'W10X64')
# NT version and architecture per token (9x: none).
$NtVer = @{ W2K = '5.0'; WXP = '5.1'; WXP64 = '5.2'; WVISTA = '6.0'; WVISTA64 = '6.0'; W7 = '6.1'; W7X64 = '6.1'; W8 = '6.2'; W8X64 = '6.2'; W10 = '10.0'; W10X64 = '10.0' }
$X64Sys = @('WXP64', 'WVISTA64', 'W7X64', 'W8X64', 'W10X64')

# ---------------------------------------------------------------- INF reading
function Read-Inf([string]$Path) {
    # Sections (lower-case name) to a list of raw entries: comments after ';'
    # outside quotes removed, '\' continuations joined, %strings% substituted.
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) { $text = [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2) }
    else { $text = [Text.Encoding]::GetEncoding(1252).GetString($bytes) }
    $sections = [ordered]@{}
    $cur = $null; $carry = ''
    foreach ($raw in ($text -split "`r?`n")) {
        $line = $carry + $raw; $carry = ''
        $sb = New-Object Text.StringBuilder; $q = $false
        foreach ($ch in $line.ToCharArray()) {
            if ($ch -eq '"') { $q = -not $q }
            if ($ch -eq ';' -and -not $q) { break }
            [void]$sb.Append($ch)
        }
        $line = $sb.ToString().TrimEnd()
        if ($line.EndsWith('\')) { $carry = $line.Substring(0, $line.Length - 1); continue }
        $t = $line.Trim()
        if ($t -eq '') { continue }
        if ($t -match '^\[(.+)\]$') { $cur = $Matches[1].Trim().ToLowerInvariant(); if (-not $sections.Contains($cur)) { $sections[$cur] = New-Object System.Collections.Generic.List[string] }; continue }
        if ($null -ne $cur) { $sections[$cur].Add($t) }
    }
    $strings = @{}
    foreach ($k in @($sections.Keys)) {
        if ($k -like 'strings*') { foreach ($e in $sections[$k]) { if ($e -match '^\s*([^=]+?)\s*=\s*(.*)$') { $strings[$Matches[1].ToLowerInvariant()] = $Matches[2].Trim().Trim('"') } } }
    }
    $sub = { param($s) [regex]::Replace($s, '%([^%]+)%', { param($m) $k = $m.Groups[1].Value.ToLowerInvariant(); if ($strings.ContainsKey($k)) { $strings[$k] } else { $m.Value } }) }
    $out = [ordered]@{}
    foreach ($k in $sections.Keys) { $out[$k] = @($sections[$k] | ForEach-Object { & $sub $_ }) }
    return $out
}
function Split-Fields([string]$s) { @($s -split ',' | ForEach-Object { $_.Trim().Trim('"') }) }
function Get-Key([string]$e) { if ($e -match '^\s*([^=]+?)\s*=\s*(.*)$') { return @($Matches[1].Trim(), $Matches[2].Trim()) } return @($null, $e.Trim()) }
function Get-Values($inf, [string]$sec, [string]$key) {
    $out = @()
    if (-not $inf.Contains($sec)) { return $out }
    foreach ($e in $inf[$sec]) { $kv = Get-Key $e; if ($null -ne $kv[0] -and $kv[0] -ieq $key) { $out += $kv[1] } }
    return $out
}

function Get-ModelSections($inf) {
    # Manufacturer entries: models section name and its decorations.
    $out = @()
    if (-not $inf.Contains('manufacturer')) { return $out }
    foreach ($e in $inf['manufacturer']) {
        $kv = Get-Key $e
        $f = @(Split-Fields $kv[1])
        if ($f.Count -eq 0 -or $f[0] -eq '') { continue }
        $out += [pscustomobject]@{ Name = $f[0]; Decorations = @($f | Select-Object -Skip 1 | Where-Object { $_ -ne '' }) }
    }
    return $out
}
function Get-ModelIds($inf, [string]$section) {
    $ids = @()
    $s = $section.ToLowerInvariant()
    if (-not $inf.Contains($s)) { return $ids }
    foreach ($e in $inf[$s]) {
        $kv = Get-Key $e
        $f = @(Split-Fields $kv[1])
        $ids += @($f | Select-Object -Skip 1 | Where-Object { $_ -ne '' } | ForEach-Object { $_.ToUpperInvariant() })
    }
    return $ids
}
function Get-AllModelIds($inf) {
    $ids = @()
    foreach ($m in (Get-ModelSections $inf)) {
        $ids += Get-ModelIds $inf $m.Name
        foreach ($d in $m.Decorations) { $ids += Get-ModelIds $inf ($m.Name + '.' + $d) }
    }
    return @($ids | Sort-Object -Unique)
}
function Get-SystemModelIds($inf, [string]$sys) {
    # The ids an INF matches on one system: 9x and 2000 the undecorated models;
    # XP and later the best decoration for the architecture whose version is not
    # above the system's, else the undecorated models on x86 only.
    $ids = @()
    foreach ($m in (Get-ModelSections $inf)) {
        if ($sys -eq 'W98SE' -or $sys -eq 'WME' -or $sys -eq 'W2K') { $ids += Get-ModelIds $inf $m.Name; continue }
        $arch = if ($X64Sys -contains $sys) { 'ntamd64' } else { 'ntx86' }
        $v = [version]$NtVer[$sys]
        $best = $null; $bestV = $null
        foreach ($d in $m.Decorations) {
            $p = $d.ToLowerInvariant().Split('.')
            if ($p[0] -ne $arch -and $p[0] -ne 'nt') { continue }
            $dv = if ($p.Count -ge 3) { [version]("$($p[1]).$($p[2])") } elseif ($p.Count -eq 2) { [version]("$($p[1]).0") } else { [version]'0.0' }
            if ($dv -gt $v) { continue }
            if ($p.Count -gt 3) { Fail "model decoration '$d' (product or suite qualifier) is not read by this script"; continue }
            # Version first; at one version the architecture-specific decoration wins.
            $rank = [version]("{0}.{1}.{2}" -f $dv.Major, $dv.Minor, $(if ($p[0] -eq 'nt') { 0 } else { 1 }))
            if ($null -eq $best -or $rank -gt $bestV) { $best = $d; $bestV = $rank }
        }
        if ($null -ne $best) { $ids += Get-ModelIds $inf ($m.Name + '.' + $best) }
        elseif ($arch -eq 'ntx86') { $ids += Get-ModelIds $inf $m.Name }
    }
    return @($ids | Sort-Object -Unique)
}

function Test-Inf([string]$Path, [string]$Folder, $Pkg, [string[]]$FolderFiles, [string[]]$Sys) {
    # Returns the files this INF takes from its folder.
    $inf = Read-Inf $Path
    $name = Split-Path $Path -Leaf
    $label = "$($Pkg.Id)/$Folder/$name"
    $sig = @(Get-Values $inf 'version' 'Signature')
    if ($sig.Count -ne 1 -or $sig[0].Trim('"') -notmatch '^\$(Chicago|Windows NT)\$$') { Fail "$label`: Signature is not `$Chicago`$ or `$Windows NT`$" }
    elseif ($sig[0].Trim('"') -match '^\$Chicago\$$' -and @($Sys | Where-Object { $X64Sys -contains $_ }).Count) {
        # 64-bit Windows refuses a $CHICAGO$ INF (7 x64 pnputil, 2026-10-24).
        Fail "$label`: a `$CHICAGO`$ INF for x64 systems ($($Sys -join ' '))"
    }
    $dv = @(Get-Values $inf 'version' 'DriverVer')
    if ($dv.Count -ne 1 -or $dv[0] -notmatch '^\d{2}/\d{2}/\d{4}\s*,\s*\d+\.\d+\.\d+\.\d+$') { Fail "$label`: no valid DriverVer ('$($dv -join ';')')" }
    else {
        $dd = ($dv[0] -split ',')[0].Trim(); $vv = ($dv[0] -split ',')[1].Trim()
        $date = [datetime]::MinValue
        if (-not [datetime]::TryParseExact($dd, 'MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture, 'None', [ref]$date)) { Fail "$label`: DriverVer date '$dd' is not a date" }
        if (@($vv.Split('.') | Where-Object { [int64]$_ -ge 65535 }).Count) { Fail "$label`: DriverVer version '$vv' has a part of 65535 or more" }
        if (@($vv.Split('.') | Where-Object { [int64]$_ -ne 0 }).Count -eq 0) { Fail "$label`: DriverVer version is 0.0.0.0" }
        Note "$label DriverVer $dd,$vv"
    }
    # Services and loader references.
    $binaries = @()
    foreach ($k in $inf.Keys) {
        foreach ($v in (Get-Values $inf $k 'AddService')) {
            $af = @(Split-Fields $v)
            $svc = $af[0]
            if ($svc -eq '') { continue }
            if ($Pkg.Services -notcontains $svc) { Fail "$label`: AddService '$svc' is not one of the package's services ($($Pkg.Services -join ', '))" }
            if ($af.Count -lt 3 -or $af[2] -eq '' -or -not $inf.Contains($af[2].ToLowerInvariant())) { Fail "$label`: AddService '$svc' names no service-install section of this INF"; continue }
            $bin = @(Get-Values $inf $af[2].ToLowerInvariant() 'ServiceBinary')
            if ($bin.Count -ne 1) { Fail "$label`: service '$svc' has no ServiceBinary" } else { $binaries += (Split-Path ($bin[0].Trim('"')) -Leaf) }
        }
        foreach ($e in $inf[$k]) {
            if ($e -match '^\s*HKR\s*,[^,]*,\s*(NTMPDriver|DeviceVxDs|PortDriver)\s*,[^,]*,\s*(.+)$') {
                foreach ($x in ($Matches[2] -split ',')) { $x = $x.Trim().Trim('"'); if ($x -ne '' -and -not $x.StartsWith('*')) { $binaries += $x } }
            }
        }
    }
    foreach ($x in ($binaries | Sort-Object -Unique)) {
        if ($FolderFiles -contains $x.ToLowerInvariant()) { continue }
        if ($Pkg.WindowsFiles -contains $x.ToUpperInvariant()) { Note "$label`: $x is Windows' own (declared)"; continue }
        Fail "$label`: loads '$x', which is neither in its folder nor a declared Windows file"
    }
    # Files copied.
    $layout = (@(Get-Values $inf 'version' 'LayoutFile').Count -gt 0)
    $include = $false
    foreach ($k in $inf.Keys) { if (@(Get-Values $inf $k 'Include').Count -gt 0) { $include = $true } }
    $need = @{}
    foreach ($k in $inf.Keys) {
        if ($k -like 'sourcedisksfiles*') { foreach ($e in $inf[$k]) { $n = (Get-Key $e)[0]; if ($null -eq $n) { $n = $e }; $need[$n.ToLowerInvariant()] = 'SourceDisksFiles' } }
    }
    $fromWin = @()
    foreach ($k in $inf.Keys) {
        foreach ($v in (Get-Values $inf $k 'CopyFiles')) {
            foreach ($c in (Split-Fields $v)) {
                if ($c -eq '') { continue }
                if ($c.StartsWith('@')) { $files = @($c.Substring(1)) }
                elseif ($inf.Contains($c.ToLowerInvariant())) { $files = @($inf[$c.ToLowerInvariant()] | ForEach-Object { $f = @(Split-Fields $_); if ($f.Count -ge 2 -and $f[1] -ne '') { $f[1] } else { $f[0] } }) }
                elseif ($include -and $Pkg.WindowsSections -contains $c.ToUpperInvariant()) { Note "$label`: CopyFiles section '$c' comes from an included Windows INF (declared)"; continue }
                else { Fail "$label`: CopyFiles names '$c', no section of this INF, and it includes no Windows INF"; continue }
                foreach ($f in $files) {
                    $lf = $f.ToLowerInvariant()
                    if ($need.ContainsKey($lf)) { continue }
                    if ($FolderFiles -contains $lf) { $need[$lf] = 'CopyFiles'; continue }
                    if ($layout -and $Pkg.WindowsFiles -contains $f.ToUpperInvariant()) { $fromWin += $f }
                    else { Fail "$label`: copies '$f', which is neither in its folder nor a declared Windows file it takes through LayoutFile" }
                }
            }
        }
    }
    if ($fromWin.Count -gt 0) { Note "$label takes from Windows (LayoutFile/Include): $(($fromWin | Sort-Object -Unique) -join ', ')" }
    foreach ($k in @(Get-Values $inf 'version' 'LayoutFile')) { Note "$label LayoutFile=$k" }
    foreach ($n in $need.Keys) { if ($FolderFiles -notcontains $n) { Fail "$label`: needs '$n' ($($need[$n])), not in its folder" } }
    # Hardware ids.
    foreach ($id in (Get-AllModelIds $inf)) {
        if ($id -match '^USB\\CLASS_[0-9A-F]{2}$') { Fail "$label`: bare class id $id (not interface-level)" }
        if ($id -match '^USB\\DEVCLASS_') { Fail "$label`: device-class id $id (a whole-device class match)" }
        if ($id -match '^USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}(&REV_[0-9A-F]{4})?$') {
            if ($Pkg.DeviceIds -notcontains '*' -and $Pkg.DeviceIds -notcontains $id) { Fail "$label`: whole-device id $id not recorded as allowed for $($Pkg.Id)" }
        }
    }
    foreach ($x in $Sys) { if (@(Get-SystemModelIds $inf $x).Count -eq 0) { Fail "$label`: matches nothing on $x, a system its STAGE.TXT line names" } }
    return @($need.Keys)
}

# ---------------------------------------------------------------- PE images
function Get-Pe([byte[]]$b) {
    if ($b.Length -lt 64 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return $null }
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ($pe -lt 0 -or $pe + 24 -gt $b.Length -or [BitConverter]::ToUInt32($b, $pe) -ne 0x4550) { return $null }
    $machine = [BitConverter]::ToUInt16($b, $pe + 4)
    $opt = $pe + 24
    $magic = [BitConverter]::ToUInt16($b, $opt)
    $dd = if ($magic -eq 0x20B) { $opt + 112 } else { $opt + 96 }
    $sec = $dd + 4 * 8
    if ($magic -ne 0x10B -and $magic -ne 0x20B) { return $null }
    if ($sec + 8 -gt $b.Length) { return $null }
    return [pscustomobject]@{ Machine = $machine; Magic = $magic; CheckSum = $opt + 64; SecDir = $sec
        CertOff = [BitConverter]::ToUInt32($b, $sec); CertLen = [BitConverter]::ToUInt32($b, $sec + 4) }
}
function Get-Imports([byte[]]$b) {
    # The DLL names in a PE image's import and delay-import directories (upper
    # case), or $null when either cannot be read.
    try {
        $pe = [BitConverter]::ToInt32($b, 0x3C)
        $nsec = [BitConverter]::ToUInt16($b, $pe + 6); $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
        $opt = $pe + 24
        $plus = ([BitConverter]::ToUInt16($b, $opt) -eq 0x20B)
        $dd = if ($plus) { $opt + 112 } else { $opt + 96 }
        $ndd = [BitConverter]::ToUInt32($b, $dd - 4)
        $base = if ($plus) { [BitConverter]::ToUInt64($b, $opt + 24) } else { [uint64][BitConverter]::ToUInt32($b, $opt + 28) }
        $secs = @(for ($i = 0; $i -lt $nsec; $i++) {
            $h = $opt + $optSize + 40 * $i
            $vs = [BitConverter]::ToUInt32($b, $h + 8); $rs = [BitConverter]::ToUInt32($b, $h + 16)
            [pscustomobject]@{ Va = [BitConverter]::ToUInt32($b, $h + 12); Size = [Math]::Max($vs, $rs); Raw = [BitConverter]::ToUInt32($b, $h + 20) }
        })
        $off = { param($rva) foreach ($s in $secs) { if ($rva -ge $s.Va -and $rva -lt $s.Va + $s.Size) { return [int]($rva - $s.Va + $s.Raw) } }; throw "RVA $rva in no section" }
        $name = { param($rva) $o = & $off $rva; $e = $o; while ($b[$e] -ne 0) { $e++ }; [Text.Encoding]::ASCII.GetString($b, $o, $e - $o).ToUpperInvariant() }
        $out = @()
        # Import directory (entry 1): 20-byte descriptors, the name RVA at +12.
        $rva = if ($ndd -gt 1) { [BitConverter]::ToUInt32($b, $dd + 8) } else { 0 }
        if ($rva -ne 0) {
            $at = & $off $rva
            while ($true) {
                $nameRva = [BitConverter]::ToUInt32($b, $at + 12); $thunk = [BitConverter]::ToUInt32($b, $at + 16)
                if ($nameRva -eq 0 -and $thunk -eq 0) { break }
                $out += & $name $nameRva
                $at += 20
            }
        }
        # Delay-import directory (entry 13): 32-byte descriptors, the name at
        # +4, an RVA when attribute bit 0 is set and else a VA (MSVC 6.0).
        $rva = if ($ndd -gt 13) { [BitConverter]::ToUInt32($b, $dd + 13 * 8) } else { 0 }
        if ($rva -ne 0) {
            $at = & $off $rva
            while ($true) {
                $attr = [BitConverter]::ToUInt32($b, $at); $nameRef = [BitConverter]::ToUInt32($b, $at + 4)
                if ($nameRef -eq 0) { break }
                $nameRva = if ($attr -band 1) { $nameRef } else { [uint32]([uint64]$nameRef - $base) }
                $out += & $name $nameRva
                $at += 32
            }
        }
        return ,$out
    } catch { return $null }
}
function Read-DerHeader([byte[]]$d, [int]$at) {
    # Tag, header length and content length of the DER element at $at, or $null.
    if ($at + 2 -gt $d.Length) { return $null }
    $tag = $d[$at]; $l = [int]$d[$at + 1]; $h = 2
    if ($l -band 0x80) {
        $n = $l -band 0x7F
        if ($n -lt 1 -or $n -gt 3 -or $at + 2 + $n -gt $d.Length) { return $null }
        $l = 0; for ($i = 0; $i -lt $n; $i++) { $l = ($l -shl 8) -bor $d[$at + 2 + $i] }
        $h = 2 + $n
    }
    if ($at + $h + $l -gt $d.Length) { return $null }
    return [pscustomobject]@{ Tag = $tag; Head = $h; Len = $l; End = $at + $h + $l }
}
function Test-Image([string]$Path, [string]$Arch, [string]$Label) {
    $b = [IO.File]::ReadAllBytes($Path)
    $pe = Get-Pe $b
    if ($null -eq $pe) {
        if ($Path -match '\.vxd$' -and $Arch -eq 'x86' -and $b.Length -gt 64 -and $b[0] -eq 0x4D -and $b[1] -eq 0x5A) {
            $le = [BitConverter]::ToInt32($b, 0x3C)
            if ($le -gt 0 -and $le + 2 -le $b.Length -and $b[$le] -eq 0x4C -and $b[$le + 1] -eq 0x45) { Note "$Label`: VxD (LE header)"; return }
        }
        Fail "$Label`: not a PE image"; return
    }
    $want = if ($Arch -eq 'x64') { 0x8664 } else { 0x14C }
    $wantMagic = if ($Arch -eq 'x64') { 0x20B } else { 0x10B }
    if ($pe.Machine -ne $want -or $pe.Magic -ne $wantMagic) { Fail ("$Label`: machine 0x{0:X}, magic 0x{1:X} in a {2} folder" -f $pe.Machine, $pe.Magic, $Arch); return }
    if ($Arch -ne 'x64') { if ($pe.CertLen -ne 0) { Fail "$Label`: an x86 image carries a certificate table (policy: unsigned)" }; return }
    if ($Path -notmatch '\.sys$') { return }
    if ($pe.CertLen -eq 0) { Fail "$Label`: x64 driver is not signed"; return }
    $len = [BitConverter]::ToUInt32($b, $pe.CertOff)
    $rev = [BitConverter]::ToUInt16($b, $pe.CertOff + 4)
    $type = [BitConverter]::ToUInt16($b, $pe.CertOff + 6)
    if ($pe.CertOff % 8 -ne 0 -or $rev -ne 0x200 -or $type -ne 2 -or $len -lt 8 -or $pe.CertOff + $len -gt $b.Length -or (($len + 7) -band -8) -ne $pe.CertLen) { Fail "$Label`: certificate table is not one aligned WIN_CERTIFICATE (revision 2.0, PKCS#7)"; return }
    $outer = Read-DerHeader $b ($pe.CertOff + 8)
    if ($null -eq $outer -or $outer.Tag -ne 0x30 -or $outer.End -gt $pe.CertOff + $len) { Fail "$Label`: certificate table holds no DER PKCS#7"; return }
    # dwLength may include up to 7 zero bytes after the DER; the table ends 8-aligned.
    for ($i = $outer.End; $i -lt $pe.CertOff + $pe.CertLen; $i++) { if ($b[$i] -ne 0) { Fail "$Label`: non-zero padding in the certificate table"; return } }
    if ($pe.CertOff + $pe.CertLen - $outer.End -gt 7) { Fail "$Label`: more than 7 bytes after the certificate"; return }
    $pkcs = New-Object byte[] ($outer.End - $pe.CertOff - 8); [Array]::Copy($b, $pe.CertOff + 8, $pkcs, 0, $pkcs.Length)
    Add-Type -AssemblyName System.Security
    $cms = New-Object System.Security.Cryptography.Pkcs.SignedCms
    try { $cms.Decode($pkcs); $cms.CheckSignature($true) } catch { Fail "$Label`: signature does not verify: $($_.Exception.Message)"; return }
    if ($cms.SignerInfos.Count -ne 1) { Fail "$Label`: $($cms.SignerInfos.Count) signers" ; return }
    $si = $cms.SignerInfos[0]
    if ($si.Certificate.Thumbprint -ne $Thumbprint) { Fail "$Label`: signer $($si.Certificate.Thumbprint), not $Thumbprint"; return }
    if ($si.DigestAlgorithm.Value -ne '1.3.14.3.2.26') { Fail "$Label`: digest algorithm $($si.DigestAlgorithm.FriendlyName), not SHA-1"; return }
    # The Authenticode SHA-1 image digest (checksum, security entry and table skipped).
    $sha = [Security.Cryptography.SHA1]::Create()
    $ranges = @(@(0, $pe.CheckSum), @(($pe.CheckSum + 4), $pe.SecDir), @(($pe.SecDir + 8), $pe.CertOff), @(($pe.CertOff + $pe.CertLen), $b.Length))
    foreach ($r in $ranges) { if ($r[1] -gt $r[0]) { [void]$sha.TransformBlock($b, $r[0], $r[1] - $r[0], $null, 0) } }
    [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
    if ($cms.ContentInfo.ContentType.Value -ne '1.3.6.1.4.1.311.2.1.4') { Fail "$Label`: signed content is not SpcIndirectDataContent"; return }
    $content = $cms.ContentInfo.Content
    # SpcIndirectDataContent ::= SEQUENCE { data SpcAttributeTypeAndOptionalValue
    # (SEQUENCE, type SPC_PE_IMAGE_DATA), messageDigest DigestInfo }, and
    # DigestInfo ::= SEQUENCE { SEQUENCE { OID sha1, NULL }, OCTET STRING (20) }.
    $seq = Read-DerHeader $content 0
    $attr = if ($null -ne $seq -and $seq.Tag -eq 0x30 -and $seq.End -eq $content.Length) { Read-DerHeader $content $seq.Head } else { $null }
    $pe2 = [byte[]](0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x0F)
    $okAttr = ($null -ne $attr -and $attr.Tag -eq 0x30 -and [BitConverter]::ToString($content, $seq.Head + $attr.Head, $pe2.Length) -eq [BitConverter]::ToString($pe2))
    $dinfo = if ($okAttr) { Read-DerHeader $content $attr.End } else { $null }
    $di = [byte[]](0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E, 0x03, 0x02, 0x1A, 0x05, 0x00, 0x04, 0x14)
    if ($null -eq $dinfo -or $dinfo.End -ne $content.Length -or $dinfo.Len -ne 0x21 -or $attr.End + $di.Length + 20 -ne $content.Length -or
        [BitConverter]::ToString($content, $attr.End, $di.Length) -ne [BitConverter]::ToString($di)) { Fail "$Label`: SpcIndirectDataContent is not a PE image's SHA-1 digest"; return }
    $tail = New-Object byte[] 20; [Array]::Copy($content, $content.Length - 20, $tail, 0, 20)
    if ([BitConverter]::ToString($tail) -ne [BitConverter]::ToString($sha.Hash)) { Fail "$Label`: image digest does not match the signature (file changed after signing)"; return }
    $ts = @($si.UnsignedAttributes | Where-Object { $_.Oid.Value -eq '1.2.840.113549.1.9.6' -or $_.Oid.Value -eq '1.3.6.1.4.1.311.3.3.1' }).Count
    Note ("$Label`: signed by {0}, SHA-1 image digest {1}, timestamp {2}" -f $si.Certificate.Thumbprint, ([BitConverter]::ToString($sha.Hash) -replace '-', ''), $(if ($ts) { 'yes' } else { 'none' }))
    return $pe
}
function Test-SignedAgainst([string]$Signed, [string]$Unsigned, [string]$Label) {
    # Only the checksum, the security directory entry and the appended table
    # (with its 8-byte alignment padding) may differ from the build.
    $s = [IO.File]::ReadAllBytes($Signed); $u = [IO.File]::ReadAllBytes($Unsigned)
    $ps = Get-Pe $s; $pu = Get-Pe $u
    if ($null -eq $ps -or $null -eq $pu -or $ps.CertOff -lt $u.Length -or $pu.CertLen -ne 0) { Fail "$Label`: signed and unsigned images do not line up"; return }
    for ($i = 0; $i -lt $u.Length; $i++) {
        if ($i -ge $ps.CheckSum -and $i -lt $ps.CheckSum + 4) { continue }
        if ($i -ge $ps.SecDir -and $i -lt $ps.SecDir + 8) { continue }
        if ($s[$i] -ne $u[$i]) { Fail ("$Label`: byte 0x{0:X} differs from the build" -f $i); return }
    }
    if ($ps.CertOff -ne (($u.Length + 7) -band -8)) { Fail "$Label`: the certificate table is not at the build's length rounded up to 8"; return }
    for ($i = $u.Length; $i -lt $ps.CertOff; $i++) { if ($s[$i] -ne 0) { Fail "$Label`: non-zero padding before the certificate table"; return } }
    if ($ps.CertOff + $ps.CertLen -ne $s.Length) { Fail "$Label`: data after the certificate table" ; return }
    Note "$Label`: matches the build $(Split-Path $Unsigned -Leaf) outside checksum and certificate table"
}

function Test-Fresh([string]$Commit, [string]$PkgId, [string]$Src) {
    # A result built at an earlier commit still stands for HEAD when nothing
    # it is made from changed since: for an image, no source of the package
    # or its class's common code other than INFs and documents; for anything
    # else (an INF, a header), nothing in the package at all. Signed x64
    # images are bound to their build's bytes, so an unchanged package is
    # not rebuilt (MSVC 6.0 and WDK 7.1 links stamp the time).
    $cls = $PkgId.Split('/')[0]
    $paths = @(& git -C $Root diff --name-only $Commit $head -- ("src/" + $PkgId) ("src/$cls/common"))
    if ($LASTEXITCODE) { Fail "$Src`: cannot compare its build commit $Commit with HEAD"; return }
    if ($Src -match '\.(sys|dll|vxd|exe|lib|a)$') { $paths = @($paths | Where-Object { $_ -notmatch '\.(inf|md)$' -and $_ -notmatch '/(pkg\.ps1|README\.TXT)$' }) }
    else {
        # A copied or generated file: its own source (the same file name) and
        # the build scripts that make it count; other INFs and documents not.
        $leaf = Split-Path $Src -Leaf
        $paths = @($paths | Where-Object { $_ -ieq "src/$PkgId/$leaf" -or $_ -match '\.(cmd|ps1|py)$' })
    }
    if ($paths.Count) { Fail "$Src`: built at $Commit; since then $($paths -join ', ') changed" }
    else { Note "$Src`: built at $($Commit.Substring(0, 7)), its sources unchanged to HEAD" }
}

# ---------------------------------------------------------------- names
$Reserved = @('CON', 'PRN', 'AUX', 'NUL', 'CLOCK$') + (1..9 | ForEach-Object { "COM$_"; "LPT$_" })
function Test-Name83([string]$n, [string]$where) {
    if ($n -notmatch '^[A-Za-z0-9_\-!#$%&''(){}~^@`]{1,8}(\.[A-Za-z0-9_\-!#$%&''(){}~^@`]{1,3})?$') { Fail "$where`: '$n' is not an 8.3 name" }
    if ($Reserved -contains ($n.Split('.')[0].ToUpperInvariant())) { Fail "$where`: '$n' is a reserved DOS name" }
}

# ---------------------------------------------------------------- build the tree
$def = Import-PowerShellDataFile -Path (Resolve-Path $Definition)
$head = (& git -C $Root rev-parse HEAD).Trim()
$dirty = @(& git -C $Root status --porcelain -- src).Count
if ($dirty) { Fail "src\ has uncommitted changes: the out\ results would not be the commit's" }
if ($def.ContainsKey('Agree')) {
    foreach ($copy in $def.Agree.Keys) {
        $norm = { param($p) @([IO.File]::ReadAllLines((Join-Path $Root $p)) | Where-Object { $_.Trim() -ne '' -and -not $_.TrimStart().StartsWith(';') -and $_ -notmatch '^\s*Signature\s*=' }) -join "`n" }
        if ((& $norm $copy) -cne (& $norm $def.Agree[$copy])) { Fail "$copy`: differs from $($def.Agree[$copy]) beyond its Signature and comments" }
        else { Note "$copy`: agrees with $($def.Agree[$copy]) but for its Signature and comments" }
    }
}
$free = Join-Path $Root 'free'
$stage = Join-Path ([IO.Path]::GetTempPath()) ("wdmfree-release-" + [guid]::NewGuid().ToString('N'))
$newFree = Join-Path $stage 'free'
New-Item -ItemType Directory -Path $newFree | Out-Null
$stageLines = @(); $pkgLines = @(); $infsAll = @(); $tokens = [ordered]@{}; $destSeen = @{}
foreach ($p in $def.Packages) {
    $wf = @(); if ($p.ContainsKey('WindowsFiles')) { $wf = @($p.WindowsFiles | ForEach-Object { $_.ToUpperInvariant() }) }
    $ws = @(); if ($p.ContainsKey('WindowsSections')) { $ws = @($p.WindowsSections | ForEach-Object { $_.ToUpperInvariant() }) }
    $pkg = [pscustomobject]@{ Id = $p.Id; Kind = $p.Kind; Desc = $p.Desc; DeviceIds = @($p.DeviceIds); Services = @($p.Services); WindowsFiles = $wf; WindowsSections = $ws }
    if ($p.Desc.Length -gt 80 -or $p.Desc.Contains('|')) { Fail "$($p.Id): PACKAGE words over 80 characters or with '|'" }
    $pkgLines += ("PACKAGE  | {0} | {1} | {2}" -f $p.Id, $p.Kind, $p.Desc)
    $tok = @()
    foreach ($f in $p.Folders) {
        if ($Part -eq 'x86' -and $f.Arch -eq 'x64') { Note "$($p.Id)/$($f.Name): x64, left out (-Part x86)"; continue }
        # The folder's systems must fit its architecture.
        foreach ($st in $f.Stage.Values) {
            foreach ($t in ($st[1] -split '\s+')) {
                if ($f.Arch -eq 'x64' -and $X64Sys -notcontains $t) { Fail "$($p.Id)/$($f.Name): x64 folder names $t" }
                if ($f.Arch -eq 'x86' -and $X64Sys -contains $t) { Fail "$($p.Id)/$($f.Name): x86 folder names $t" }
            }
        }
        $dest = Join-Path $newFree (Join-Path ($p.Id.Replace('/', '\')) $f.Name)
        New-Item -ItemType Directory -Path $dest -Force | Out-Null
        $names = @($f.Files.Keys)
        foreach ($n in $names) {
            $key = (Join-Path $dest $n).ToUpperInvariant()
            if ($destSeen.ContainsKey($key)) { Fail "$($p.Id)/$($f.Name)/$n`: a second definition of the same published path"; continue }
            $destSeen[$key] = $true
            $src = $f.Files[$n]
            if ($src.StartsWith('tree:')) { $srcPath = Join-Path $Root $src.Substring(5) }
            elseif ($src.StartsWith('x64:')) {
                if ($X64Dir -eq '') { Fail "$($p.Id)/$($f.Name)/$n`: no -X64Dir for the signed x64 image"; continue }
                $srcPath = Join-Path $X64Dir $src.Substring(4)
            } else {
                $srcPath = Join-Path $BuildRoot $src
                $d = Split-Path $srcPath -Parent; $pass = $null
                while ($d.Length -gt $BuildRoot.Length) { if (Test-Path (Join-Path $d 'PASS.TXT')) { $pass = $d; break }; $d = Split-Path $d -Parent }
                if ($null -eq $pass) { Fail "$src`: no PASS.TXT in its build result" }
                else {
                    $rec = Join-Path $pass 'record.txt'
                    if (Test-Path $rec) {
                        $rt = [IO.File]::ReadAllText($rec)
                        if ($rt -notmatch 'tree\s+clean') { Fail "$src`: built from a dirty tree" }
                        if ($rt -notmatch 'commit\s+([0-9a-f]{40})') { Fail "$src`: its build record names no commit" }
                        elseif ($Matches[1] -ne $head) { Test-Fresh $Matches[1] $p.Id $src }
                    } else { Note "$src`: build result has PASS.TXT but no record.txt" }
                }
            }
            if (-not (Test-Path -LiteralPath $srcPath -PathType Leaf)) { Fail "$($p.Id)/$($f.Name)/$n`: source '$srcPath' missing"; continue }
            Copy-Item -LiteralPath $srcPath -Destination (Join-Path $dest $n)
            if ($src.StartsWith('x64:')) {
                $build = $def.X64Builds[$src.Substring(4)]
                if ($null -eq $build) { Fail "$src`: no X64Builds entry naming its unsigned build"; continue }
                $unsigned = Join-Path $BuildRoot $build
                $rec = Join-Path (Split-Path $unsigned -Parent) 'record.txt'
                if (-not (Test-Path (Join-Path (Split-Path $unsigned -Parent) 'PASS.TXT')) -or -not (Test-Path $rec) -or ([IO.File]::ReadAllText($rec) -notmatch 'commit\s+([0-9a-f]{40})')) { Fail "$build`: no passing build record" }
                elseif ($Matches[1] -ne $head) { Test-Fresh $Matches[1] $p.Id $build }
                Test-SignedAgainst (Join-Path $dest $n) $unsigned "$($p.Id)/$($f.Name)/$n"
            }
        }
        $folderFiles = @(Get-ChildItem -LiteralPath $dest -File | ForEach-Object { $_.Name.ToLowerInvariant() })
        # 1: INFs and images.
        $used = @{}
        foreach ($n in $names) {
            $full = Join-Path $dest $n
            if ($n -match '\.inf$') {
                $sysOf = @(); if ($f.Stage.ContainsKey($n)) { $sysOf = @($f.Stage[$n][1] -split '\s+') }
                foreach ($u in (Test-Inf $full $f.Name $pkg $folderFiles $sysOf)) { $used[$u] = $true }
                $used[$n.ToLowerInvariant()] = $true
                $rel = 'free/' + $p.Id + '/' + $f.Name + '/' + $n
                $infsAll += $rel
                if (-not $f.Stage.ContainsKey($n)) { Fail "$rel`: no STAGE.TXT line in the definition"; continue }
                $s = $f.Stage[$n]
                foreach ($t in ($s[1] -split '\s+')) { if ($Systems -notcontains $t) { Fail "$rel`: unknown system token $t" } }
                $stageLines += [pscustomobject]@{ Pkg = $p.Id; Action = $s[0]; Systems = @($s[1] -split '\s+'); Inf = $rel; Note = $s[2]; Path = $full }
                if ($s[0] -eq 'STAGE') { $tok += ($s[1] -split '\s+') }
            } elseif ($n -match '\.(sys|dll|vxd|exe)$') { Test-Image $full $f.Arch "$($p.Id)/$($f.Name)/$n" | Out-Null }
        }
        # 3: nothing extra.
        $extra = @($f['Extra']) | Where-Object { $_ } | ForEach-Object { $_.ToLowerInvariant() }
        foreach ($x in $folderFiles) {
            if ($used.ContainsKey($x) -or $extra -contains $x -or $x -eq 'readme.txt') { continue }
            Fail "$($p.Id)/$($f.Name)/$x`: in the folder but copied by no INF and not declared"
        }
    }
    $tokens[$p.Id] = @($tok | Sort-Object -Unique { [array]::IndexOf($Systems, $_) })
}

# 4: tools (free\tools\<name>\): not driver packages, so no INF and no STAGE.TXT line.
if ($def.ContainsKey('Tools')) {
    foreach ($t in $def.Tools) {
        $dest = Join-Path $newFree (Join-Path 'tools' $t.Name)
        New-Item -ItemType Directory -Path $dest -Force | Out-Null
        $want = @($t.Imports | ForEach-Object { $_.ToUpperInvariant() })
        foreach ($n in @($t.Files.Keys)) {
            $label = "tools/$($t.Name)/$n"
            $key = (Join-Path $dest $n).ToUpperInvariant()
            if ($destSeen.ContainsKey($key)) { Fail "$label`: a second definition of the same published path"; continue }
            $destSeen[$key] = $true
            if ($n -match '\.inf$') { Fail "$label`: a tool folder holds no INF (a tool is not a driver package)"; continue }
            $src = $t.Files[$n]
            $srcPath = if ($src.StartsWith('tree:')) { Join-Path $Root $src.Substring(5) } else { Join-Path $Root $src }
            if (-not (Test-Path -LiteralPath $srcPath -PathType Leaf)) { Fail "$label`: source '$srcPath' missing"; continue }
            $full = Join-Path $dest $n
            Copy-Item -LiteralPath $srcPath -Destination $full
            if ($n -match '\.(exe|dll)$') {
                Test-Image $full 'x86' $label | Out-Null
                $imp = Get-Imports ([IO.File]::ReadAllBytes($full))
                if ($null -eq $imp) { Fail "$label`: cannot read its import table"; continue }
                foreach ($d in $imp) { if ($want -notcontains $d) { Fail "$label`: imports $d, not in its Imports list" } }
                Note ("$label`: SHA-256 {0}, imports {1}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $full).Hash.ToLowerInvariant(), ($imp -join ' '))
            } elseif ($n -match '\.txt$') {
                $tb = [IO.File]::ReadAllBytes($full)
                if (@($tb | Where-Object { $_ -gt 0x7E -or ($_ -lt 0x20 -and $_ -ne 0x0D -and $_ -ne 0x0A) }).Count) { Fail "$label`: not plain ASCII" }
                $tl = [Text.Encoding]::ASCII.GetString($tb) -split "`r`n"
                if (@($tl | Where-Object { $_.Contains("`n") -or $_.Contains("`r") -or $_.Length -gt 78 }).Count) { Fail "$label`: a line over 78 columns or not CRLF" }
            }
        }
    }
}

# 2: names.
foreach ($d in @(Get-ChildItem -LiteralPath $newFree -Recurse -Directory) + @(Get-Item $newFree)) {
    $kids = @(Get-ChildItem -LiteralPath $d.FullName)
    $seen = @{}
    foreach ($k in $kids) {
        $rel = $k.FullName.Substring($stage.Length + 1).Replace('\', '/')
        Test-Name83 $k.Name $rel
        $u = $k.Name.ToUpperInvariant()
        if ($seen.ContainsKey($u)) { Fail "$rel`: collides with $($seen[$u])" } else { $seen[$u] = $k.Name }
    }
}

# 7: STAGE.TXT.
$known = @{}
foreach ($l in $stageLines) { $k = $l.Inf.ToLowerInvariant(); if ($known.ContainsKey($k)) { Fail "$($l.Inf): on two lines" }; $known[$k] = $true }
foreach ($i in @(Get-ChildItem -LiteralPath $newFree -Recurse -File -Filter *.inf)) {
    $rel = $i.FullName.Substring($stage.Length + 1).Replace('\', '/')
    if (-not $known.ContainsKey($rel.ToLowerInvariant())) { Fail "$rel`: on no STAGE.TXT line" }
}
$sets = @(@{ Name = 'free'; Lines = @($stageLines | ForEach-Object { [pscustomobject]@{ Action = $_.Action; Systems = $_.Systems; Inf = $_.Inf; Path = $_.Path } }) })
if ($NonFree -ne '') {
    $nl = @()
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $NonFree 'STAGE.TXT'))) {
        $t = $line.Trim(); if ($t -eq '' -or $t.StartsWith('#')) { continue }
        $f = @($line -split '\|' | ForEach-Object { $_.Trim() })
        if ($f[0] -ne 'STAGE' -and $f[0] -ne 'COPY') { continue }
        $nl += [pscustomobject]@{ Action = $f[0]; Systems = @($f[1] -split '\s+'); Inf = $f[2]; Path = (Join-Path $NonFree $f[2].Replace('/', '\')) }
    }
    $sets += @{ Name = 'nonfree'; Lines = $nl }
}
$owner = @{}
foreach ($s in $sets) {
    foreach ($l in @($s.Lines | Where-Object { $_.Action -eq 'STAGE' })) {
        $inf = Read-Inf $l.Path
        foreach ($sys in $l.Systems) {
            foreach ($id in (Get-SystemModelIds $inf $sys)) {
                $k = "$sys|$id"; $who = "$($s.Name):$($l.Inf)"
                if ($owner.ContainsKey($k) -and $owner[$k] -ne $who) { Fail "$sys`: $id staged by $($owner[$k]) and $who" } else { $owner[$k] = $who }
            }
        }
    }
}
$hdr = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'stage-header.txt'))
$sb = New-Object Text.StringBuilder
[void]$sb.Append($hdr.Replace("`r`n", "`n"))
foreach ($p in $def.Packages) {
    [void]$sb.Append("`n# ---- $($p.Id)`n")
    [void]$sb.Append((@($pkgLines | Where-Object { $_ -match ("\| " + [regex]::Escape($p.Id) + " \|") })[0]) + "`n")
    foreach ($l in @($stageLines | Where-Object { $_.Pkg -eq $p.Id })) {
        [void]$sb.Append(("{0,-5} | {1} | {2} | {3}`n" -f $l.Action, ($l.Systems -join ' '), $l.Inf, $l.Note))
    }
}
[void]$sb.Append("`n# ---- Relations (set-qualified)`n")
foreach ($r in $def.Relations) { [void]$sb.Append($r + "`n") }
$stageText = $sb.ToString().Replace("`n", "`r`n")
[IO.File]::WriteAllText((Join-Path $stage 'STAGE.TXT'), $stageText, (New-Object Text.ASCIIEncoding))

# SHA256.TXT over free\.
$files = @(Get-ChildItem -LiteralPath $newFree -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1).Replace('\', '/') })
[Array]::Sort($files, [StringComparer]::Ordinal)
$sums = @($files | ForEach-Object { "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage $_.Replace('/', '\'))).Hash.ToLowerInvariant(), $_ })
[IO.File]::WriteAllText((Join-Path $stage 'SHA256.TXT'), (($sums -join "`n") + "`n"), (New-Object Text.ASCIIEncoding))
foreach ($l in [IO.File]::ReadAllLines((Join-Path $stage 'SHA256.TXT'))) {
    $h, $pth = $l -split '  ', 2
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage $pth.Replace('/', '\'))).Hash.ToLowerInvariant() -ne $h) { Fail "SHA256.TXT: $pth does not verify" }
}

# Certificate (public half).
if ($Certificate -ne '') {
    $cb = [IO.File]::ReadAllBytes($Certificate)
    $c = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 (, $cb)
    if ($c.HasPrivateKey) { Fail "certificate: carries a private key" }
    if ($c.Thumbprint -ne $Thumbprint) { Fail "certificate: thumbprint $($c.Thumbprint), not $Thumbprint" }
    if ($cb[0] -ne 0x30) { Fail "certificate: not DER" }
    Copy-Item -LiteralPath $Certificate -Destination (Join-Path $stage 'usb-wdm-free-test.cer')
}

# ---------------------------------------------------------------- report and publish
$script:Notes | ForEach-Object { "note: $_" }
"tokens:"; foreach ($k in $tokens.Keys) { "  $k | $($tokens[$k] -join ' ')" }
if ($script:Errors.Count) { $script:Errors | ForEach-Object { "FAIL: $_" }; "release: $($script:Errors.Count) failures; nothing published (scratch $stage)"; exit 1 }
if ($Check) { "release: checks PASS; -Check, nothing published (scratch $stage)"; exit 0 }
if (Test-Path $free) { Remove-Item -LiteralPath $free -Recurse -Force }
Move-Item -LiteralPath $newFree -Destination $free
Copy-Item -LiteralPath (Join-Path $stage 'STAGE.TXT') -Destination (Join-Path $Root 'STAGE.TXT') -Force
Copy-Item -LiteralPath (Join-Path $stage 'SHA256.TXT') -Destination (Join-Path $Root 'SHA256.TXT') -Force
if ($Certificate -ne '') { Copy-Item -LiteralPath (Join-Path $stage 'usb-wdm-free-test.cer') -Destination (Join-Path $Root 'usb-wdm-free-test.cer') -Force }
Remove-Item -LiteralPath $stage -Recurse -Force
"release: PASS; free\, STAGE.TXT and SHA256.TXT written ($($files.Count) files)"

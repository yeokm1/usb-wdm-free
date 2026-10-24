# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
#
# build.ps1 -Profile icmx86 -Flavour <free|checked> [-Iterations n]
#   icmx86  UVCDEC.DLL, the VfW ICM MJPEG codec for 98 SE, ME and 2000
#           (MSVC 6.0, static C runtime; libjpeg-turbo's x86 SIMD
#           assembled by NASM), its gates, UVCDEC.INF, and the host test
#           JDTEST.EXE run on vectors/ (decode checks, SIMD against plain C,
#           and the 1080p decode timing; -Iterations 0 skips the timing).
# Output: out\video\uvcdec\icmx86\<flavour>\, PASS.TXT written last.
param(
    [Parameter(Mandatory)][ValidateSet('icmx86')][string]$Profile,
    [Parameter(Mandatory)][ValidateSet('free', 'checked')][string]$Flavour,
    [int]$Iterations = 20
)
$ErrorActionPreference = 'Stop'
$pkg = $PSScriptRoot
$root = (Resolve-Path (Join-Path $pkg '..\..\..')).Path
$vc = Join-Path $root 'tools\MSVC600\VC98'
$msdev = Join-Path $root 'tools\MSVC600\Common\MSDev98\Bin'
$ddk = Join-Path $root 'tools\ntddk'
$nasm = Join-Path $root 'tools\nasm\nasm.exe'
$tj = Join-Path $pkg 'turbo'
$out = Join-Path $root "out\video\uvcdec\$Profile\$Flavour"
$obj = Join-Path $out 'obj'
$host_ = Join-Path $out 'host'

function Need([string]$Path, [string]$Kit) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "Missing $Kit`: $Path" }
}
Need "$vc\BIN\CL.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$vc\BIN\LINK.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$msdev\MSPDB60.DLL" 'MSVC 6.0 (tools\MSVC600)'
Need "$vc\Include\VFW.H" 'MSVC 6.0 (tools\MSVC600)'
Need "$vc\Lib\LIBCMT.LIB" 'MSVC 6.0 (tools\MSVC600)'
Need "$ddk\bin\rc.exe" 'Windows 2000 DDK (tools\ntddk)'
Need $nasm 'NASM 3.02 (tools\nasm)'

foreach ($d in @($out, $obj, $host_)) { if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d | Out-Null } }
Remove-Item -LiteralPath (Join-Path $out 'PASS.TXT') -ErrorAction SilentlyContinue
$log = Join-Path $out 'build.log'
Set-Content -LiteralPath $log -Value "uvcdec $Profile $Flavour" -Encoding ASCII

$git = (Get-Command git -ErrorAction SilentlyContinue | Select-Object -First 1).Source
$env:PATH = "$vc\BIN;$msdev;$env:SystemRoot\System32"
$env:INCLUDE = "$vc\Include"
$env:LIB = "$vc\Lib"
foreach ($v in 'CL', '_CL_', 'LINK', '_LINK_') { Remove-Item "env:$v" -ErrorAction SilentlyContinue }

function Run([string]$Exe, [string[]]$ArgList) {
    Add-Content -LiteralPath $log -Value ("> $Exe " + ($ArgList -join ' '))
    $o = & $Exe @ArgList 2>&1
    $code = $LASTEXITCODE
    if ($o) { Add-Content -LiteralPath $log -Value ($o | Out-String) }
    if ($code -ne 0) { throw "$Exe failed ($code); see $log" }
    return $o
}

# The libjpeg-turbo files the DLL links (turbo/, 3.2.0 with the MSVC 6.0
# patches README.md lists), as its CMakeLists.txt JPEG_SOURCES names them:
# a -8, -12 or -16 wrapper compiles one file for that sample precision.
$tjDec = @('src\jaricom', 'src\jcomapi', 'src\jdapimin', 'src\jdarith', 'src\jdatasrc',
    'src\jdhuff', 'src\jdicc', 'src\jdinput', 'src\jdlhuff', 'src\jdmarker', 'src\jdmaster',
    'src\jdphuff', 'src\jdtrans', 'src\jerror', 'src\jmemmgr', 'src\jmemnobs', 'src\jpeg_nbits')
foreach ($b in @('jdapistd', 'jdcolor', 'jddiffct', 'jdlossls', 'jdmainct', 'jdpostct', 'jdsample', 'jutils')) {
    $tjDec += @("src\wrapper\$b-8", "src\wrapper\$b-12", "src\wrapper\$b-16")
}
foreach ($b in @('jdcoefct', 'jddctmgr', 'jdmerge', 'jidctflt', 'jidctfst', 'jidctint', 'jidctred', 'jquant1', 'jquant2')) {
    $tjDec += @("src\wrapper\$b-8", "src\wrapper\$b-12")
}
$tjDec += 'simd\jsimd'
# and the compressor files only the host test links, to make its own frames
$tjEnc = @('src\jcapimin', 'src\jcarith', 'src\jchuff', 'src\jcicc', 'src\jcinit', 'src\jclhuff',
    'src\jcmarker', 'src\jcmaster', 'src\jcparam', 'src\jcphuff', 'src\jctrans', 'src\jdatadst',
    'src\jfdctflt')
foreach ($b in @('jcapistd', 'jccolor', 'jcdiffct', 'jclossls', 'jcmainct', 'jcprepct', 'jcsample')) {
    $tjEnc += @("src\wrapper\$b-8", "src\wrapper\$b-12", "src\wrapper\$b-16")
}
foreach ($b in @('jccoefct', 'jcdctmgr', 'jfdctfst', 'jfdctint')) {
    $tjEnc += @("src\wrapper\$b-8", "src\wrapper\$b-12")
}
# the x86 SIMD, as simd/CMakeLists.txt's i386 SIMD_SOURCES (the *ext* files
# are included by these)
$tjAsm = @('jsimdcpu', 'jfdctflt-3dn', 'jidctflt-3dn', 'jquantf-3dn',
    'jccolor-mmx', 'jcgray-mmx', 'jcsample-mmx', 'jdcolor-mmx', 'jdmerge-mmx', 'jdsample-mmx',
    'jfdctfst-mmx', 'jfdctint-mmx', 'jidctfst-mmx', 'jidctint-mmx', 'jidctred-mmx', 'jquanti-mmx',
    'jfdctflt-sse', 'jidctflt-sse', 'jquantf-sse',
    'jccolor-sse2', 'jcgray-sse2', 'jchuff-sse2', 'jcphuff-sse2', 'jcsample-sse2', 'jdcolor-sse2',
    'jdmerge-sse2', 'jdsample-sse2', 'jfdctfst-sse2', 'jfdctint-sse2', 'jidctflt-sse2', 'jidctfst-sse2',
    'jidctint-sse2', 'jidctred-sse2', 'jquantf-sse2', 'jquanti-sse2',
    'jccolor-avx2', 'jcgray-avx2', 'jcsample-avx2', 'jdcolor-avx2', 'jdmerge-avx2', 'jdsample-avx2',
    'jfdctint-avx2', 'jidctint-avx2', 'jquanti-avx2')

function ObjName([string]$f) { return ($f -replace '\\', '_') }

$opt = if ($Flavour -eq 'free') { @('/O2', '/DNDEBUG') } else { @('/Od', '/Zi', '/D_DEBUG') }
$common = @('/nologo', '/c', '/Gd', '/MT', '/GF', '/DWIN32', '/D_WINDOWS', '/D_X86_=1',
    "/I$pkg", "/I$tj\src", "/I$tj\simd") + $opt
# libjpeg-turbo: function-level linking, so /OPT:REF drops what the codec never calls
$lib = @('/W3', '/Gy', '/DNO_GETENV')

# ---- the SIMD objects (shared by the DLL and the host test) ----
$asmObj = Join-Path $out 'asm'
if (-not (Test-Path $asmObj)) { New-Item -ItemType Directory -Path $asmObj | Out-Null }
$asms = @()
foreach ($f in $tjAsm) {
    Run $nasm @('-fwin32', '-DWIN32', "-I$tj\simd\nasm\", "-I$tj\simd\i386\", "-o$asmObj\$f.obj", "$tj\simd\i386\$f.asm") | Out-Null
    $asms += "$asmObj\$f.obj"
}

# ---- UVCDEC.DLL ----
$objs = @()
foreach ($f in $tjDec) {
    $o = ObjName $f
    Run "$vc\BIN\CL.EXE" ($common + $lib + @("/Fo$obj\$o.obj", "$tj\$f.c")) | Out-Null
    $objs += "$obj\$o.obj"
}
foreach ($f in @('uvcjpeg', 'uvcdec')) {
    Run "$vc\BIN\CL.EXE" ($common + @('/W3', '/WX', "/Fo$obj\$f.obj", "$pkg\$f.c")) | Out-Null
    $objs += "$obj\$f.obj"
}
Run "$ddk\bin\rc.exe" @("/I$vc\Include", "/fo$obj\uvcdec.res", "$pkg\uvcdec.rc") | Out-Null
$dll = Join-Path $out 'UVCDEC.DLL'
$lflags = @('/nologo', '/DLL', "/DEF:$pkg\uvcdec.def", "/OUT:$dll", '/SUBSYSTEM:WINDOWS,4.00',
    '/MACHINE:IX86', '/INCREMENTAL:NO', '/OPT:REF', '/RELEASE', "/MAP:$out\uvcdec.map",
    "/IMPLIB:$obj\uvcdec.lib")
if ($Flavour -eq 'checked') { $lflags += @('/DEBUG', "/PDB:$out\uvcdec.pdb") }
Run "$vc\BIN\LINK.EXE" ($lflags + $objs + $asms + @("$obj\uvcdec.res", 'kernel32.lib')) | Out-Null

Run "$vc\BIN\DUMPBIN.EXE" @('/headers', $dll) | Set-Content -LiteralPath "$out\headers.txt"
Run "$vc\BIN\DUMPBIN.EXE" @('/imports', $dll) | Set-Content -LiteralPath "$out\imports.txt"
Run "$vc\BIN\DUMPBIN.EXE" @('/exports', $dll) | Set-Content -LiteralPath "$out\exports.txt"

# ---- gates ----
$gate = Join-Path $out 'gate.txt'
Set-Content -LiteralPath $gate -Value "uvcdec $Profile $Flavour gates" -Encoding ASCII
$h = Get-Content -LiteralPath "$out\headers.txt"
if (-not ($h -match '^\s*14C machine \(i386\)')) { throw 'UVCDEC.DLL: machine is not i386' }
if (-not ($h -match '^\s*4\.00 subsystem version')) { throw 'UVCDEC.DLL: subsystem version is not 4.00' }
if (-not ($h -match '^\s*4\.00 operating system version')) { throw 'UVCDEC.DLL: OS version is not 4.00' }
if (-not ($h -match '^\s*2 subsystem \(Windows GUI\)')) { throw 'UVCDEC.DLL: subsystem is not Windows GUI' }
if (-not ($h -match '^\s*DLL\s*$')) { throw 'UVCDEC.DLL: not a DLL' }
foreach ($line in $h) {
    if ($line -match '^\s*([0-9A-Fa-f]+)\s+\[\s*([0-9A-Fa-f]+)\]\s+RVA \[size\] of Delay Import Directory') {
        if ([Convert]::ToUInt64($Matches[1], 16) -or [Convert]::ToUInt64($Matches[2], 16)) { throw 'UVCDEC.DLL: delay imports' }
    }
}
# exports: exactly DriverProc
$exp = @()
$in = $false
foreach ($l in Get-Content -LiteralPath "$out\exports.txt") {
    if ($l -match '^\s*ordinal\s+hint\s+RVA\s+name') { $in = $true; continue }
    if ($l -match '^\s*Summary') { break }
    if ($in -and $l -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $exp += $Matches[1] }
}
if (($exp -join ',') -ne 'DriverProc') { throw "UVCDEC.DLL exports '$($exp -join ',')', expected DriverProc" }
# imports: KERNEL32 only, each allowlisted and in 98 SE's and ME's KERNEL32 export tables
function Read-Table([string]$Path) {
    $n = @{}
    foreach ($l in Get-Content -LiteralPath $Path) {
        $t = $l.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $n[($t -split '\s+')[-1].ToUpperInvariant()] = $true
    }
    if ($n.Count -lt 100) { throw "Export table looks empty: $Path" }
    return $n
}
$t98 = Read-Table "$pkg\exports98\kernel32-98.txt"
$tme = Read-Table "$pkg\exports98\kernel32-me.txt"
$allow = @{}
foreach ($l in Get-Content -LiteralPath "$pkg\imports-dll.txt") {
    $t = $l.Trim()
    if (-not $t -or $t.StartsWith('#')) { continue }
    if ($t -notmatch '^KERNEL32\.DLL!([A-Za-z_][A-Za-z0-9_]*)$') { throw "Bad allowlist line: $t" }
    $allow[$Matches[1].ToUpperInvariant()] = $true
}
$module = $null; $active = $false; $done = $false; $found = @()
foreach ($line in Get-Content -LiteralPath "$out\imports.txt") {
    if ($line -match '^\s*Section contains the following imports:') { $active = $true; continue }
    if (-not $active) { continue }
    if ($line -match '^\s*Summary') { $done = $true; break }
    if (-not $line.Trim()) { continue }
    if ($line -match '^\s{4}(\S+\.(dll|DLL))\s*$') {
        $module = $Matches[1].ToUpperInvariant()
        if ($module -ne 'KERNEL32.DLL') { throw "UVCDEC.DLL imports from $module" }
        continue
    }
    if ($line -match '^\s*[0-9A-Fa-f]+\s+(Import Address Table|Import Name Table|time date stamp|Index of first forwarder reference)\s*$') { continue }
    if ($line -match '^\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*$') {
        $n = $Matches[1]; $u = $n.ToUpperInvariant()
        if (-not $allow.ContainsKey($u)) { throw "Import not in imports-dll.txt: KERNEL32.DLL!$n" }
        if (-not $t98.ContainsKey($u)) { throw "Import absent from 98 SE KERNEL32 export table: $n" }
        if (-not $tme.ContainsKey($u)) { throw "Import absent from ME KERNEL32 export table: $n" }
        $found += $n
        continue
    }
    throw "Unparsed import row (ordinal?): $line"
}
if (-not $done -or $found.Count -lt 5) { throw 'Incomplete import report' }
Add-Content -LiteralPath $gate -Value ("UVCDEC.DLL PASS: i386 GUI 4.00 DLL (OS 4.00), no delay imports or ordinals; exports DriverProc only; " +
    "$($found.Count) imports, KERNEL32.DLL only, each in imports-dll.txt and in the 98 SE and ME KERNEL32 export tables (exports98/)")

# ---- UVCDEC.INF, CRLF ASCII ----
$inf = [IO.File]::ReadAllText("$pkg\UVCDEC.INF") -replace "`r?`n", "`r`n"
if ($inf -match '[^\x09\x0A\x0D\x20-\x7E]') { throw 'UVCDEC.INF: not ASCII' }
if ($inf -notmatch '(?im)^\s*Signature\s*=\s*"\$Windows NT\$"' -and $inf -notmatch '(?im)^\s*Signature\s*=\s*"\$CHICAGO\$"') { throw 'UVCDEC.INF: no signature' }
if ($inf -notmatch '(?i)VIDC\.MJPG') { throw 'UVCDEC.INF: no VIDC.MJPG entry' }
[IO.File]::WriteAllText("$out\UVCDEC.INF", $inf, [Text.Encoding]::ASCII)
Add-Content -LiteralPath $gate -Value 'UVCDEC.INF PASS: ASCII, CRLF, VIDC.MJPG registration present'

# ---- host test ----
$hc = @('/nologo', '/c', '/MT', '/O2', '/DWIN32', '/D_CONSOLE', '/D_X86_=1', "/I$pkg", "/I$tj\src", "/I$tj\simd")
$hobjs = @()
foreach ($f in ($tjDec + $tjEnc)) {
    $o = ObjName $f
    Run "$vc\BIN\CL.EXE" ($hc + $lib + @("/Fo$host_\$o.obj", "$tj\$f.c")) | Out-Null
    $hobjs += "$host_\$o.obj"
}
foreach ($f in @('uvcjpeg', 'uvcdec', 'jdtest')) {
    Run "$vc\BIN\CL.EXE" ($hc + @('/W3', '/WX', "/Fo$host_\$f.obj", "$pkg\$f.c")) | Out-Null
    $hobjs += "$host_\$f.obj"
}
$exe = Join-Path $host_ 'JDTEST.EXE'
Run "$vc\BIN\LINK.EXE" (@('/nologo', "/OUT:$exe", '/SUBSYSTEM:CONSOLE', '/INCREMENTAL:NO') + $hobjs + $asms + @('kernel32.lib')) | Out-Null
$tlog = Join-Path $out 'test.log'
$o = & $exe "$pkg\vectors" $Iterations 2>&1
$code = $LASTEXITCODE
$o | Set-Content -LiteralPath $tlog -Encoding ASCII
if ($code -ne 0) { $o | Write-Output; throw "JDTEST.EXE failed ($code); see $tlog" }
$sum = ($o | Select-Object -Last 2) -join ' '
Add-Content -LiteralPath $gate -Value "JDTEST.EXE PASS: $sum"

# ---- record ----
$head = 'unknown (git not found)'; $dirty = 'unknown'
if ($git) {
    $head = (& $git -C $root rev-parse HEAD 2>$null)
    $dirty = if (& $git -C $root status --porcelain -- 'src/video/uvcdec' 2>$null) { 'modified' } else { 'clean' }
}
$record = @("package  video\uvcdec", "profile  $Profile $Flavour", "commit   $head", "tree     $dirty (src\video\uvcdec)",
    "kit      MSVC 6.0 CL/LINK (tools\MSVC600), static CRT LIBCMT; rc.exe from tools\ntddk\bin; $((& $nasm -v) -join ' ') (tools\nasm)",
    "library  libjpeg-turbo 3.2.0, src\video\uvcdec\turbo (MSVC 6.0 patches as README.md lists), jconfig.h, jconfigint.h, jversion.h as README.md says",
    "cflags   $($common -join ' ') /W3; libjpeg-turbo files also $($lib -join ' ')",
    "asflags  -fwin32 -DWIN32 -I turbo\simd\nasm\ -I turbo\simd\i386\",
    "lflags   $($lflags -join ' ')")
$files = @(Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Name -in @('UVCDEC.DLL', 'UVCDEC.INF') })
foreach ($f in $files) { $record += "sha256   $((Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash)  $($f.Name)" }
$record | Set-Content -Encoding ASCII -LiteralPath (Join-Path $out 'record.txt')
$sha = foreach ($f in $files) { "$((Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash.ToLowerInvariant())  $($f.Name)" }
$sha | Set-Content -Encoding ASCII -LiteralPath (Join-Path $out 'SHA256.TXT')
"PASS uvcdec $Profile $Flavour (build gates and host test; no guest has loaded it)" | Set-Content -Encoding ASCII -LiteralPath (Join-Path $out 'PASS.TXT')
Write-Output (Get-Content -LiteralPath $tlog)
Write-Output "PASS: $out"

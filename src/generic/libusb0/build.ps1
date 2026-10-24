# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
#
# build.ps1 -Profile <wdmx86|win32|w98> -Flavour <free|checked>
#
# libusb0 for Windows 98 SE / ME (docs/design/libusb.md section 6.4,
# build.md sections 3 to 5):
#   wdmx86  LIBUSB0.SYS: MSVC 6.0 + the Windows 2000 DDK, plus a second
#           compile against the Windows 98 DDK's own WDM/USB headers, and
#           the import gate (allowlist + the 98 DDK's WDM/USBD libraries).
#   win32   LIBUSB0.DLL, LIBUSB0.LIB, LIBUSB0.A (GNU dlltool), TESTLIBU.EXE: MSVC 6.0 against its own
#           headers and MSVCRT.DLL (cfgmgr32/setupapi declarations from the
#           98 DDK); import gate against the 98 DDK's user-mode libraries;
#           the DLL's export set equals libusb0.def; the host tests.
#   w98     both, LIBUSB0.INF with its gate and mutation tests, negative
#           import/export gate checks, staged as one 98 SE / ME folder.
# Output: out\generic\libusb0\<profile>\<flavour>\, PASS.TXT written last.
# Nothing is written under free/.
param(
    [Parameter(Mandatory)][ValidateSet('wdmx86', 'win32', 'w98')][string]$Profile,
    [Parameter(Mandatory)][ValidateSet('free', 'checked')][string]$Flavour
)
$ErrorActionPreference = 'Stop'
$pkg = $PSScriptRoot
$root = [IO.Path]::GetFullPath((Join-Path $pkg '..\..\..'))
$tools = Join-Path $root 'tools'
$vc = Join-Path $tools 'MSVC600\VC98'
$ddk = Join-Path $tools 'ntddk'
$ddk98 = Join-Path $tools 'win98ddk-extracted\all'
$bu = Join-Path $tools 'binutils\mingw32\bin'
. (Join-Path $pkg 'gate.ps1')
. (Join-Path $pkg 'infgate.ps1')

# Version 0.1.13.N (design 6.1): above every upstream 0.1.x, below every
# 1.x, so an upstream 1.x package installed later ranks higher. It reaches
# the resources, LIBUSB_IOCTL_GET_VERSION (VERSION_*), the DLL's
# usb_get_version() and, checked by the INF gate, DriverVer.
$ver = @(0, 1, 13, 0)
$verDefs = @("/DVERSION_MAJOR=$($ver[0])", "/DVERSION_MINOR=$($ver[1])", "/DVERSION_MICRO=$($ver[2])",
             "/DVERSION_NANO=$($ver[3])")

function Need([string]$path, [string]$kit) {
    if (-not (Test-Path -LiteralPath $path)) { throw "MISSING: $path - install $kit as docs\toolchain.md describes" }
}
Need "$vc\BIN\CL.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$vc\BIN\LINK.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$vc\BIN\DUMPBIN.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$tools\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL" 'MSVC 6.0 (tools\MSVC600)'
Need "$tools\MSVC600\Common\MSDev98\Bin\RC.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$vc\Lib\MSVCRT.LIB" 'MSVC 6.0 (tools\MSVC600)'
Need "$ddk\inc\wdm.h" 'Windows 2000 DDK (tools\ntddk)'
Need "$ddk\inc\ddk\wdm\usbdlib.h" 'Windows 2000 DDK (tools\ntddk)'
Need "$ddk\libfre\i386\wdm.lib" 'Windows 2000 DDK (tools\ntddk)'
Need "$ddk\libfre\i386\usbd.lib" 'Windows 2000 DDK (tools\ntddk)'
Need "$ddk98\INCS_DDK\INC_WIN98_WDM.H" 'Windows 98 SE DDK (tools\win98ddk-extracted)'
Need "$ddk98\INCS_DDK\INC_WIN98_CFGMGR32.H" 'Windows 98 SE DDK (tools\win98ddk-extracted)'
Need "$ddk98\LIBS_DDK\LIB_I386_FREE_WDM.LIB" 'Windows 98 SE DDK (tools\win98ddk-extracted)'
Need "$ddk98\LIBS_DDK\LIB_I386_FREE_CFGMGR32.LIB" 'Windows 98 SE DDK (tools\win98ddk-extracted)'
if ($Profile -ne 'wdmx86') {
    Need "$bu\dlltool.exe" 'GNU binutils for i686 (tools\binutils)'
    Need "$bu\as.exe" 'GNU binutils for i686 (tools\binutils)'
    Need "$bu\nm.exe" 'GNU binutils for i686 (tools\binutils)'
}

$out = Join-Path $root "out\generic\libusb0\$Profile\$Flavour"
if (Test-Path -LiteralPath $out) { Remove-Item -Recurse -Force -LiteralPath $out }
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out 'build.log'
$gate = Join-Path $out 'gate.txt'
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null

$head = (git -C $root rev-parse HEAD) 2>$null
$dirty = if (git -C $root status --porcelain -- src/generic/libusb0) { 'dirty' } else { 'clean' }

# A fresh child environment: nothing from the caller's compiler setup.
$saved = @{}
foreach ($k in 'PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'CL', '_CL_', 'LINK', '_LINK_') {
    $saved[$k] = [Environment]::GetEnvironmentVariable($k)
    [Environment]::SetEnvironmentVariable($k, $null)
}
$psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$env:PATH = "$vc\BIN;$tools\MSVC600\Common\MSDev98\Bin;$env:SystemRoot\system32"

function Run([string]$exe, [string[]]$a) {
    Add-Content -LiteralPath $log -Value ("> $exe " + ($a -join ' '))
    $o = & $exe @a 2>&1
    $o | Add-Content -LiteralPath $log
    if ($LASTEXITCODE) { $o | Write-Host; throw "$exe failed ($LASTEXITCODE)" }
    return $o
}

function Copy-Headers([string]$dir, [string]$from, [string[]]$names) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($n in $names) {
        Copy-Item -LiteralPath (Join-Path $from "INC_WIN98_$n.H") -Destination (Join-Path $dir ($n.ToLowerInvariant() + '.h'))
    }
}

# 98 DDK import library declarations, by module (build.md 5.1).
function Get-Libs98([hashtable]$map) {
    $libs = @{}
    foreach ($m in $map.Keys) {
        $listing = Join-Path $obj ("w98-" + $map[$m] + '.txt')
        Run "$vc\BIN\DUMPBIN.EXE" @('/nologo', '/exports', "$ddk98\LIBS_DDK\LIB_I386_FREE_$($map[$m]).LIB") | Set-Content -LiteralPath $listing
        $libs[$m] = Read-LibExports $listing
    }
    return $libs
}

# RC_VERSION and RC_VERSION_STR (upstream's ddk_make C_DEFINES) reach the
# resource script through a generated wrapper, not through quoted /d options.
function Build-Res([string]$rc, [string]$res) {
    $w = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($rc) + '.wrap.rc')
    @("#define RC_VERSION $($ver -join ',')", "#define RC_VERSION_STR ""$($ver -join '.')""",
      "#include ""$rc""") | Set-Content -Encoding ASCII -LiteralPath $w
    Run "$tools\MSVC600\Common\MSDev98\Bin\RC.EXE" @('/l', '0x409', "/I$vc\Include", "/I$pkg\src", "/fo$res", $w) | Out-Null
}

# The version resource carries the build's version (the version string in
# UTF-16, and the fixed FILEVERSION words).
function Assert-VersionResource([string]$file) {
    $b = [IO.File]::ReadAllBytes($file)
    $str = [Text.Encoding]::Unicode.GetBytes(($ver -join '.') + "`0")
    $hex = [BitConverter]::ToString($b)
    $want = [BitConverter]::ToString($str)
    if ($hex.IndexOf($want) -lt 0) { throw "${file}: no version string $($ver -join '.') in the version resource" }
    $ms = [BitConverter]::GetBytes([uint32](($ver[0] -shl 16) -bor $ver[1]))
    $ls = [BitConverter]::GetBytes([uint32](($ver[2] -shl 16) -bor $ver[3]))
    $sig = [BitConverter]::ToString([byte[]](@(0xBD, 0x04, 0xEF, 0xFE, 0x00, 0x00, 0x01, 0x00) + $ms + $ls))
    if ($hex.IndexOf($sig) -lt 0) { throw "${file}: VS_FIXEDFILEINFO FILEVERSION is not $($ver -join '.')" }
}

$record = @("package  generic\libusb0", "profile  $Profile $Flavour", "commit   $head", "tree     $dirty (src\generic\libusb0)",
            "version  $($ver -join '.')",
            "kit      MSVC 6.0 (cl 12.00.8804, link 6.00.8447); Windows 2000 DDK; Windows 98 SE DDK headers and libraries; GNU binutils (MSYS2 mingw-w64-i686-binutils 2.47-4) dlltool for LIBUSB0.A",
            "date     $(Get-Date -Format s)")
$warnings = 0

$drvSources = @('abort_endpoint', 'claim_interface', 'clear_feature', 'dispatch', 'driver_registry', 'get_configuration',
                'get_descriptor', 'get_interface', 'get_status', 'ioctl', 'libusb_driver', 'pnp', 'power',
                'release_interface', 'reset_device', 'reset_endpoint', 'set_configuration', 'set_descriptor',
                'set_feature', 'set_interface', 'transfer', 'vendor_request', 'driver_debug')

function Build-Sys {
    $drv = Join-Path $pkg 'src\driver'
    # Upstream code is built at /W3 as it is; warnings are logged and counted, not fatal.
    $c = @('/nologo', '/c', '/W3', '/Gz', '/Gy', '/GF', '/Zp8', '/Zl', '/D_X86_=1', '/Di386=1', '/DSTD_CALL',
           '/DCONDITION_HANDLING=1', '/DNT_UP=1', '/DNT_INST=0', '/DWIN32=100', '/D_NT1X_=100', '/DWINNT=1',
           '/D_WIN32_WINNT=0x0500', '/DWIN32_LEAN_AND_MEAN=1', '/DDEVL=1') + $verDefs
    $c += if ($Flavour -eq 'free') { @('/Oxs', '/DDBG=0') } else { @('/Od', '/Oi', '/Zi', "/Fd$obj\libusb0-cl.pdb", '/DDBG=1') }
    $inc = @("/I$ddk\inc\ddk\wdm", "/I$ddk\inc\ddk", "/I$ddk\inc", "/I$vc\Include")
    $objs = @()
    foreach ($f in $drvSources) {
        $o = Run "$vc\BIN\CL.EXE" ($c + $inc + @("/Fo$obj\$f.obj", "$drv\$f.c"))
        $script:warnings += @($o | Where-Object { $_ -match ': warning C\d+' }).Count
        $objs += "$obj\$f.obj"
    }
    # Second compile against the 98 DDK's own WDM/USB headers: anything the
    # 98 headers do not declare fails here.
    $inc98 = Join-Path $obj 'inc98'
    Copy-Headers $inc98 "$ddk98\INCS_DDK" @('WDM', 'NTDEF', 'NTSTATUS', 'BUGCODES', 'NTIOLOGC', 'BASETSD',
        'USB100', 'USBDI', 'USBDLIB', 'USBIOCTL', 'DEVIOCTL')
    $c98 = @('/nologo', '/c', '/W3', '/Gz', '/Zp8', '/Zl', '/D_X86_=1', '/Di386=1', '/DSTD_CALL', '/DNT_UP=1',
             '/DWIN32=100', '/DWINNT=1', '/DDBG=0', "/I$inc98", "/I$vc\Include") + $verDefs
    foreach ($f in $drvSources) {
        Run "$vc\BIN\CL.EXE" ($c98 + @("/Fo$obj\$f.98.obj", "$drv\$f.c")) | Out-Null
    }
    Build-Res "$drv\libusb_driver_rc.rc" "$obj\libusb0.res"
    $l = @('/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,4.00', '/ENTRY:DriverEntry@8', '/BASE:0x10000', '/ALIGN:0x20',
           '/NODEFAULTLIB', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF', '/MERGE:_PAGE=PAGE', '/MERGE:_TEXT=.text',
           '/MACHINE:IX86', '/RELEASE', "/MAP:$out\libusb0.map", "/OUT:$out\LIBUSB0.SYS") + $objs +
         @("$obj\libusb0.res", "$ddk\libfre\i386\wdm.lib", "$ddk\libfre\i386\usbd.lib")
    if ($Flavour -eq 'checked') { $l += @('/DEBUG', "/PDB:$out\libusb0.pdb") }
    Run "$vc\BIN\LINK.EXE" $l | Out-Null
    Run "$vc\BIN\DUMPBIN.EXE" @('/nologo', '/headers', "$out\LIBUSB0.SYS") | Set-Content -LiteralPath "$out\sys-headers.txt"
    Run "$vc\BIN\DUMPBIN.EXE" @('/nologo', '/imports', "$out\LIBUSB0.SYS") | Set-Content -LiteralPath "$out\sys-imports.txt"
    Assert-Pe "$out\sys-headers.txt" 'Native' '4.00' $false
    Assert-VersionResource "$out\LIBUSB0.SYS"
    $libs = Get-Libs98 @{ 'NTOSKRNL.EXE' = 'WDM'; 'USBD.SYS' = 'USBD'; 'HAL.DLL' = 'HAL' }
    $tables = Get-Tables98 "$pkg\exports98"
    $script:tableHits = 0
    $found = Assert-Imports "$out\sys-imports.txt" "$pkg\imports-sys.txt" $libs $tables
    Add-Content -LiteralPath $gate -Value ("LIBUSB0.SYS PASS: i386 native 4.00, no delay imports or ordinals; $($found.Count) imports, " +
        "all allowlisted and declared by the 98 DDK's WDM.LIB/USBD.LIB/HAL.LIB; also compiles against the 98 DDK WDM/USB headers; " +
        "$($script:tableHits) import(s) also in the ME USBD.SYS export table (NTOSKRNL is NTKERN.VXD on 9x: no PE table; 98 SE USBD.SYS not on the image); " +
        "version resource $($ver -join '.')")
    $found | ForEach-Object { Add-Content -LiteralPath $gate -Value "  $_" }
    $script:record += "sys      cflags $($c -join ' ')"
}

function Build-Win32 {
    $src = Join-Path $pkg 'src'
    # cfgmgr32.h and its cfg.h, setupapi.h and regstr.h: the 98 DDK's.
    $incu = Join-Path $obj 'inc98u'
    Copy-Headers $incu "$ddk98\INCS_DDK" @('CFGMGR32', 'CFG', 'SETUPAPI', 'REGSTR')
    Copy-Item -LiteralPath "$ddk98\LIBS_DDK\LIB_I386_FREE_CFGMGR32.LIB" -Destination "$obj\cfgmgr32.lib"
    $c = @('/nologo', '/c', '/W3', '/Gd', '/MD', '/DWIN32', '/D_WINDOWS', '/D_X86_=1') + $verDefs
    $c += if ($Flavour -eq 'free') { @('/O2') } else { @('/Od', '/Zi', "/Fd$obj\libusb0dll-cl.pdb") }
    $inc = @("/I$incu", "/I$src", "/I$pkg\src\driver", "/I$vc\Include")
    $objs = @()
    foreach ($f in 'windows', 'usb', 'error', 'install', 'descriptors', 'registry') {
        $o = Run "$vc\BIN\CL.EXE" ($c + $inc + @("/Fo$obj\$f.obj", "$src\$f.c"))
        $script:warnings += @($o | Where-Object { $_ -match ': warning C\d+' }).Count
        $objs += "$obj\$f.obj"
    }
    Build-Res "$src\dllver.rc" "$obj\libusb0dll.res"
    # cfgmgr32.lib before setupapi.lib: on 98 the CM_* routines live in
    # CFGMGR32.DLL only (upstream 3a902c7, "win98 support").
    $l = @('/nologo', '/DLL', '/SUBSYSTEM:WINDOWS,4.00', '/MACHINE:IX86', '/INCREMENTAL:NO', '/OPT:REF', '/RELEASE',
           "/DEF:$pkg\libusb0.def", "/IMPLIB:$out\LIBUSB0.LIB", "/MAP:$out\libusb0dll.map", "/OUT:$out\LIBUSB0.DLL",
           "/LIBPATH:$vc\Lib") + $objs + @("$obj\libusb0dll.res", "$obj\cfgmgr32.lib", 'setupapi.lib', 'kernel32.lib',
           'advapi32.lib')
    if ($Flavour -eq 'checked') { $l += @('/DEBUG', "/PDB:$out\libusb0dll.pdb") }
    Run "$vc\BIN\LINK.EXE" $l | Out-Null
    Remove-Item -LiteralPath "$out\LIBUSB0.exp" -ErrorAction SilentlyContinue

    # LIBUSB0.A, the MinGW import library (design 6.2, Owner, 2026-10-22):
    # GNU dlltool from tools\binutils, generated from libusb0.def. The i386
    # cdecl names get their leading underscore, as MinGW's compiler emits.
    Push-Location $obj
    try {
        Run "$bu\dlltool.exe" @('-m', 'i386', '--as', "$bu\as.exe", '-t', 'lu0', '-D', 'LIBUSB0.DLL',
            '-d', "$pkg\libusb0.def", '-l', "$out\LIBUSB0.A") | Out-Null
    } finally { Pop-Location }
    $nm = Run "$bu\nm.exe" @("$out\LIBUSB0.A")
    $defNames = @()
    $inDef = $false
    foreach ($l in Get-Content -LiteralPath "$pkg\libusb0.def") {
        $t = ($l -replace ';.*$', '').Trim()
        if (-not $t) { continue }
        if ($t -eq 'EXPORTS') { $inDef = $true; continue }
        if ($inDef) { $defNames += ($t -split '\s+')[0] }
    }
    $code = @{}; $imp = @{}
    foreach ($l in $nm) {
        if ("$l" -match '^[0-9a-f]+ T _(\S+)$') { $code[$Matches[1]] = $true }
        if ("$l" -match '^[0-9a-f]+ I __imp__(\S+)$') { $imp[$Matches[1]] = $true }
    }
    foreach ($n in $defNames) {
        if (-not $code.ContainsKey($n) -or -not $imp.ContainsKey($n)) { throw "LIBUSB0.A lacks _$n or __imp__$n" }
    }
    if ($code.Count -ne $defNames.Count -or $imp.Count -ne $defNames.Count) { throw "LIBUSB0.A has $($code.Count) stubs, the .def $($defNames.Count) names" }
    if ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes("$out\LIBUSB0.A")).IndexOf('LIBUSB0.DLL') -lt 0) { throw 'LIBUSB0.A does not name LIBUSB0.DLL' }
    $script:libaNote = "LIBUSB0.A PASS: GNU dlltool import library for LIBUSB0.DLL; _name and __imp__name for each of the $($defNames.Count) .def names, nothing else"

    # TESTLIBU.EXE: upstream's testlibusb, console, against the import library.
    $ct = @('/nologo', '/c', '/W3', '/Gd', '/MD', '/DWIN32', '/D_CONSOLE', '/D_X86_=1', '/Dsnprintf=_snprintf') + $verDefs
    $ct += if ($Flavour -eq 'free') { @('/O2') } else { @('/Od', '/Zi', "/Fd$obj\testlibu-cl.pdb") }
    $o = Run "$vc\BIN\CL.EXE" ($ct + @("/I$src", "/I$vc\Include", "/Fo$obj\testlibusb.obj", "$pkg\tests\testlibusb.c"))
    $script:warnings += @($o | Where-Object { $_ -match ': warning C\d+' }).Count
    $lt = @('/nologo', '/SUBSYSTEM:CONSOLE,4.00', '/MACHINE:IX86', '/INCREMENTAL:NO', '/OPT:REF', '/RELEASE',
            "/MAP:$out\testlibu.map", "/OUT:$out\TESTLIBU.EXE", "/LIBPATH:$vc\Lib", "$obj\testlibusb.obj",
            "$out\LIBUSB0.LIB", 'kernel32.lib')
    if ($Flavour -eq 'checked') { $lt += @('/DEBUG', "/PDB:$out\testlibu.pdb") }
    Run "$vc\BIN\LINK.EXE" $lt | Out-Null

    $libs = Get-Libs98 @{ 'KERNEL32.DLL' = 'KERNEL32'; 'ADVAPI32.DLL' = 'ADVAPI32'; 'USER32.DLL' = 'USER32';
                          'SETUPAPI.DLL' = 'SETUPAPI'; 'CFGMGR32.DLL' = 'CFGMGR32'; 'MSVCRT.DLL' = 'MSVCRT' }
    $libs['LIBUSB0.DLL'] = @{}
    foreach ($b in @(@('LIBUSB0.DLL', $true, 'imports-dll.txt'), @('TESTLIBU.EXE', $false, 'imports-exe.txt'))) {
        $n = $b[0]; $base = [IO.Path]::GetFileNameWithoutExtension($n).ToLowerInvariant()
        Run "$vc\BIN\DUMPBIN.EXE" @('/nologo', '/headers', "$out\$n") | Set-Content -LiteralPath "$out\$base-headers.txt"
        Run "$vc\BIN\DUMPBIN.EXE" @('/nologo', '/imports', "$out\$n") | Set-Content -LiteralPath "$out\$base-imports.txt"
        Assert-Pe "$out\$base-headers.txt" $(if ($b[1]) { 'Windows GUI' } else { 'Windows CUI' }) '4.00' $b[1]
    }
    Assert-VersionResource "$out\LIBUSB0.DLL"
    $tables = Get-Tables98 "$pkg\exports98"
    $script:tableHits = 0
    $found = Assert-Imports "$out\libusb0-imports.txt" "$pkg\imports-dll.txt" $libs $tables
    $dllHits = $script:tableHits
    Run "$vc\BIN\DUMPBIN.EXE" @('/nologo', '/exports', "$out\LIBUSB0.DLL") | Set-Content -LiteralPath "$out\libusb0-exports.txt"
    $nexp = Assert-Exports "$out\libusb0-exports.txt" "$pkg\libusb0.def"
    Add-Content -LiteralPath $gate -Value ("LIBUSB0.DLL PASS: i386 GUI 4.00 DLL, no delay imports or ordinals; $($found.Count) imports, " +
        "all allowlisted, declared by the 98 DDK's import library of their module and present in the 98 SE and ME export tables of " +
        "that module ($dllHits checks, exports98/); exports equal libusb0.def ($nexp names); version resource $($ver -join '.')")
    $found | ForEach-Object { Add-Content -LiteralPath $gate -Value "  $_" }
    # TESTLIBU.EXE imports libusb0 names: those must be the DLL's own exports.
    $exp = @{}
    foreach ($l in Get-Content -LiteralPath "$out\libusb0-exports.txt") {
        if ($l -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $exp[$Matches[1].ToUpperInvariant()] = $true }
    }
    $libs['LIBUSB0.DLL'] = $exp
    $tables['98 SE']['LIBUSB0.DLL'] = $exp
    $tables['ME']['LIBUSB0.DLL'] = $exp
    $found = Assert-Imports "$out\testlibu-imports.txt" "$pkg\imports-exe.txt" $libs $tables
    Add-Content -LiteralPath $gate -Value "TESTLIBU.EXE PASS: i386 console 4.00; $($found.Count) imports, allowlisted, declared by the 98 DDK libraries and in the 98 SE / ME export tables, or exported by LIBUSB0.DLL"
    $found | ForEach-Object { Add-Content -LiteralPath $gate -Value "  $_" }
    Copy-Item -LiteralPath "$src\usb.h" -Destination "$out\USB.H"
    Add-Content -LiteralPath $gate -Value $script:libaNote

    $t = & $psExe -NoProfile -ExecutionPolicy Bypass -File "$pkg\hosttest.ps1" 2>&1
    if ($LASTEXITCODE) { throw "host tests failed: $t" }
    Add-Content -LiteralPath $gate -Value "host tests $($t | Select-Object -Last 1)"
    $script:record += "dll      cflags $($c -join ' ')"
}

# w98: everything, the INF, the negative gate checks and the file list,
# staged as one 98 SE / ME install candidate (design 6.2). Not a release.
function Stage-W98 {
    Build-Sys
    Build-Win32
    $v = $ver -join '.'
    $t = [IO.File]::ReadAllText((Join-Path $pkg 'LIBUSB0.INF'))
    Assert-LibusbInf $t $v
    [IO.File]::WriteAllText((Join-Path $out 'LIBUSB0.INF'), ($t -replace "`r?`n", "`r`n"), [Text.Encoding]::ASCII)
    Add-Content -LiteralPath $gate -Value "LIBUSB0.INF PASS: package contract (sections, VID/PID models, 9x loader values, DLL to DIRID 11, files, DriverVer $v), ASCII/CRLF"
    $r = & $psExe -NoProfile -ExecutionPolicy Bypass -File "$pkg\inftest.ps1" -Version $v 2>&1
    if ($LASTEXITCODE) { throw "INF mutation tests failed: $r" }
    Add-Content -LiteralPath $gate -Value "INF mutation tests $r"

    # Negative gate checks on scratch copies of this build's own reports.
    $neg = Join-Path $obj 'neg'
    New-Item -ItemType Directory -Force $neg | Out-Null
    function Insert-Row([string]$file, [string]$before, [string]$row, [string]$dest) {
        $imp = [System.Collections.ArrayList]@(Get-Content -LiteralPath $file)
        $at = -1
        for ($i = 0; $i -lt $imp.Count; $i++) { if ($imp[$i] -match ('^\s+[0-9A-Fa-f]+\s+' + $before + '\s*$')) { $at = $i; break } }
        if ($at -lt 0) { throw "negative check: no $before row in $file" }
        $imp.Insert($at, $row)
        $imp | Set-Content -LiteralPath $dest
    }
    function Expect-Fail([scriptblock]$b, [string]$what, [string]$why) {
        $msg = $null
        try { & $b | Out-Null } catch { $msg = $_.Exception.Message }
        if (-not $msg) { throw "gate accepted $what" }
        if ($why -and $msg -notmatch $why) { throw "gate rejected $what for another reason: $msg" }
    }
    $sysLibs = Get-Libs98 @{ 'NTOSKRNL.EXE' = 'WDM'; 'USBD.SYS' = 'USBD'; 'HAL.DLL' = 'HAL' }
    $dllLibs = Get-Libs98 @{ 'KERNEL32.DLL' = 'KERNEL32'; 'ADVAPI32.DLL' = 'ADVAPI32'; 'SETUPAPI.DLL' = 'SETUPAPI';
                             'CFGMGR32.DLL' = 'CFGMGR32'; 'MSVCRT.DLL' = 'MSVCRT' }
    Insert-Row "$out\sys-imports.txt" 'KeSetEvent' '                  99  IoAllocateWorkItem' "$neg\a.txt"
    Expect-Fail { Assert-Imports "$neg\a.txt" "$pkg\imports-sys.txt" $sysLibs $null } 'an unlisted SYS import' 'not in allowlist'
    # 1.2.6.0's driver imports that 98 lacks (design 2.3), even when allowlisted.
    (Get-Content "$pkg\imports-sys.txt") + 'NTOSKRNL.EXE!ExFreePoolWithTag' | Set-Content -LiteralPath "$neg\allow-sys.txt"
    Insert-Row "$out\sys-imports.txt" 'KeSetEvent' '                  99  ExFreePoolWithTag' "$neg\b.txt"
    Expect-Fail { Assert-Imports "$neg\b.txt" "$neg\allow-sys.txt" $sysLibs $null } 'ExFreePoolWithTag' 'not declared by the 98 DDK'
    # 1.2.6.0's DLL import that blocks it on 98 (design 2.3).
    (Get-Content "$pkg\imports-dll.txt") + 'ADVAPI32.DLL!CheckTokenMembership' | Set-Content -LiteralPath "$neg\allow-dll.txt"
    Insert-Row "$out\libusb0-imports.txt" 'RegCloseKey' '                  99  CheckTokenMembership' "$neg\c.txt"
    Expect-Fail { Assert-Imports "$neg\c.txt" "$neg\allow-dll.txt" $dllLibs $null } 'CheckTokenMembership' 'not declared by the 98 DDK'
    # An import the ME table lacks (here: removed from a scratch copy).
    $tb = Get-Tables98 "$pkg\exports98"
    $tb['ME']['CFGMGR32.DLL'].Remove('CM_GET_DEVNODE_STATUS')
    Expect-Fail { Assert-Imports "$out\libusb0-imports.txt" "$pkg\imports-dll.txt" $dllLibs $tb } 'an import absent from an export table' 'export table'
    Insert-Row "$out\libusb0-imports.txt" 'RegCloseKey' '                  Ordinal    12' "$neg\d.txt"
    Expect-Fail { Assert-Imports "$neg\d.txt" "$pkg\imports-dll.txt" $dllLibs $null } 'an ordinal import' 'ordinal'
    (Get-Content "$pkg\libusb0.def") + '    usb_not_built' | Set-Content -LiteralPath "$neg\x.def"
    Expect-Fail { Assert-Exports "$out\libusb0-exports.txt" "$neg\x.def" } 'a missing export' 'differ'
    Add-Content -LiteralPath $gate -Value ('Negative gate checks PASS: an unlisted SYS import, ExFreePoolWithTag and ' +
        'CheckTokenMembership (allowlisted but not declared for 98), an import absent from the ME export table, ' +
        'an ordinal import and a missing DLL export are all rejected')

    $want = @('LIBUSB0.SYS', 'LIBUSB0.DLL', 'LIBUSB0.LIB', 'LIBUSB0.A', 'TESTLIBU.EXE', 'LIBUSB0.INF', 'USB.H')
    foreach ($n in $want) { if (-not (Test-Path -LiteralPath (Join-Path $out $n))) { throw "staged folder lacks $n" } }
    foreach ($f in Get-ChildItem -LiteralPath $out -File) {
        if ($f.Name -notin $want -and $f.Name -notmatch '^(build\.log|gate\.txt|record\.txt|.*-(headers|imports|exports)\.txt|.*\.map|.*\.pdb)$') {
            throw "Unexpected file in the staged folder: $($f.Name)"
        }
    }
    Add-Content -LiteralPath $gate -Value ("Staged install candidate: " + ($want -join ', '))
}

try {
    switch ($Profile) {
        'wdmx86' { Build-Sys }
        'win32'  { Build-Win32 }
        'w98'    { Stage-W98 }
    }
    Add-Content -LiteralPath $gate -Value "compiler warnings in upstream code (logged, not fatal): $warnings"
    $files = @(Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Extension -in '.SYS', '.DLL', '.EXE', '.LIB', '.A', '.INF', '.H' })
    $files += Get-ChildItem -LiteralPath $pkg -Recurse -File | Where-Object { $_.Extension -in '.c', '.h', '.def', '.rc', '.ps1', '.txt', '.inf', '.cmd' }
    foreach ($f in $files) { $record += "sha256   $((Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash)  $($f.FullName)" }
    $record | Set-Content -Encoding ASCII -LiteralPath (Join-Path $out 'record.txt')
    Remove-Item -Recurse -Force -LiteralPath $obj
    "PASS libusb0 $Profile $Flavour (build gates only; no guest has loaded it)" | Set-Content -Encoding ASCII -LiteralPath (Join-Path $out 'PASS.TXT')
    Write-Output "PASS: $out"
} catch {
    Write-Output "FAILED: libusb0 $Profile $Flavour - $($_.Exception.Message) (see $log)"
    exit 1
} finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
}

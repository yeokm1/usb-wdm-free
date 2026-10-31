# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
#
# build.ps1 -Flavour <free|trace>
#   free   the release candidate
#   trace  USBPRN.SYS writes a line per event to QEMU's debug console port
#          0xE9 (UP_TRACE_E9), USBPRNMN.DLL appends to C:\USBPRNMN.LOG
#          (UM_TRACE). Test-only, never released.
#
# usbprn for Windows 98 SE and ME (docs/design/printing.md 10 to 12):
#   USBPRN.SYS    MSVC 6.0 + the Windows 2000 DDK, plus a second compile
#                 against the Windows 98 DDK's own WDM/USB headers; import
#                 gate (allowlist + declared in the 98 WDM.H).
#   USBPRNMN.DLL  MSVC 6.0, the 98 DDK's WINSPLP.H for the MONITOR table, no
#                 C run-time; import gate against the 98 SE and ME export
#                 tables of its four modules (exports98/); export gate.
#   USBPRN.INF, USBPRNME.INF   INF gates and their mutation tests.
#   host tests of prncore.c (hosttest.ps1).
# Output: out\print\usbprn\<flavour>\w98se\ (USBPRN.SYS, USBPRNMN.DLL,
# USBPRN.INF) and ...\wme\ (USBPRNME.INF), PASS.TXT written last. Not a
# release: nothing is written under free/.
param([Parameter(Mandatory)][ValidateSet('free', 'trace')][string]$Flavour)
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$root = [IO.Path]::GetFullPath((Join-Path $src '..\..\..'))
$tools = Join-Path $root 'tools'
$vc = Join-Path $tools 'MSVC600\VC98'
$ddk = Join-Path $tools 'ntddk'
$ddk98 = Join-Path $tools 'win98ddk-extracted'
. (Join-Path $src 'gate.ps1')

# A failed run leaves no earlier PASS.TXT or binaries behind.
$top = Join-Path $root "out\print\usbprn\$Flavour"
if (Test-Path -LiteralPath $top) { Remove-Item -Recurse -Force -LiteralPath $top }

function Need([string]$path, [string]$kit) {
    if (-not (Test-Path -LiteralPath $path)) { throw "MISSING: $path - install $kit as docs\toolchain.md describes" }
}
Need "$vc\BIN\CL.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$vc\BIN\LINK.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$tools\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL" 'MSVC 6.0 (tools\MSVC600)'
Need "$ddk\inc\wdm.h" 'Windows 2000 DDK (tools\ntddk)'
Need "$ddk\libfre\i386\wdm.lib" 'Windows 2000 DDK (tools\ntddk)'
Need "$ddk98\all\INCS_DDK\INC_WIN98_WDM.H" 'Windows 98 SE DDK (tools\win98ddk-extracted)'
Need "$ddk98\incs\INC_WIN98_WINSPLP.H" 'Windows 98 SE DDK (tools\win98ddk-extracted)'

$w98 = Join-Path $top 'w98se'
$wme = Join-Path $top 'wme'
$obj = Join-Path $top 'obj'
New-Item -ItemType Directory -Force $w98, $wme, $obj | Out-Null
$log = Join-Path $top 'build.log'
$gate = Join-Path $top 'gate.txt'

$head = (git -C $root rev-parse HEAD) 2>$null
$dirty = if (git -C $root status --porcelain -- src/print/usbprn) { 'dirty' } else { 'clean' }

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

$record = @("package  print\usbprn", "flavour  $Flavour", "commit   $head", "tree     $dirty (src\print\usbprn)",
            "kit      MSVC 6.0 (cl 12.00.8804, link 6.00.8447); Windows 2000 DDK; Windows 98 SE DDK headers",
            "date     $(Get-Date -Format s)")

function Build-Sys {
    $c = @('/nologo', '/c', '/W3', '/WX', '/Gz', '/Gy', '/GF', '/Zp8', '/Zl', '/D_X86_=1', '/Di386=1', '/DSTD_CALL',
           '/DCONDITION_HANDLING=1', '/DNT_UP=1', '/DNT_INST=0', '/DWIN32=100', '/D_NT1X_=100', '/DWINNT=1',
           '/D_WIN32_WINNT=0x0500', '/DWIN32_LEAN_AND_MEAN=1', '/DDEVL=1', '/Oxs', '/DDBG=0')
    if ($Flavour -eq 'trace') { $c += '/DUP_TRACE_E9=1' }
    $inc = @("/I$ddk\inc\ddk\wdm", "/I$ddk\inc\ddk", "/I$ddk\inc", "/I$vc\Include")
    foreach ($f in 'usbprn', 'prncore') {
        Run "$vc\BIN\CL.EXE" ($c + $inc + @("/Fo$obj\$f.obj", "$src\$f.c")) | Out-Null
    }
    # Second compile against the 98 DDK's own WDM/USB headers.
    $inc98 = Join-Path $obj 'inc98'
    Copy-Headers $inc98 "$ddk98\all\INCS_DDK" @('WDM', 'NTDEF', 'NTSTATUS', 'BUGCODES', 'NTIOLOGC', 'BASETSD',
        'USB100', 'USBDI', 'USBDLIB', 'USBIOCTL', 'DEVIOCTL')
    $c98 = @('/nologo', '/c', '/W3', '/WX', '/Gz', '/Zp8', '/Zl', '/D_X86_=1', '/Di386=1', '/DSTD_CALL', '/DNT_UP=1',
             '/DWIN32=100', '/DWINNT=1', '/DDBG=0', "/I$inc98", "/I$vc\Include")
    Run "$vc\BIN\CL.EXE" ($c98 + @("/Fo$obj\usbprn.98.obj", "$src\usbprn.c")) | Out-Null
    Run "$tools\MSVC600\Common\MSDev98\Bin\RC.EXE" @('/l', '0x409', "/I$vc\Include", "/fo$obj\usbprn.res", "$src\usbprn.rc") | Out-Null
    $l = @('/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,4.00', '/ENTRY:DriverEntry@8', '/BASE:0x10000', '/ALIGN:0x20',
           '/NODEFAULTLIB', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF', '/MACHINE:IX86', '/RELEASE', "/MAP:$obj\usbprn.map",
           "/OUT:$w98\USBPRN.SYS", "$obj\usbprn.obj", "$obj\prncore.obj", "$obj\usbprn.res",
           "$ddk\libfre\i386\wdm.lib")
    Run "$vc\BIN\LINK.EXE" $l | Out-Null
    Run "$vc\BIN\DUMPBIN.EXE" @('/headers', "$w98\USBPRN.SYS") | Set-Content -LiteralPath "$top\sys-headers.txt"
    Run "$vc\BIN\DUMPBIN.EXE" @('/imports', "$w98\USBPRN.SYS") | Set-Content -LiteralPath "$top\sys-imports.txt"
    $r = Assert-SysImports "$top\sys-headers.txt" "$top\sys-imports.txt" "$src\imports-sys.txt" "$ddk98\all\INCS_DDK\INC_WIN98_WDM.H"
    Add-Content -LiteralPath $gate -Value "USBPRN.SYS $r; also compiles /W3 /WX against the 98 DDK WDM headers"
    $script:record += "sys      cflags $($c -join ' ')"
}

function Build-Dll {
    $incm = Join-Path $obj 'incmon'
    New-Item -ItemType Directory -Force $incm | Out-Null
    Copy-Item -LiteralPath "$ddk98\incs\INC_WIN98_WINSPLP.H" -Destination (Join-Path $incm 'winsplp.h')
    $c = @('/nologo', '/c', '/W3', '/WX', '/O1', '/Gy', '/GF', '/Zl', '/DWIN32', '/D_WINDOWS', '/DWINVER=0x0400',
           '/D_WIN32_WINDOWS=0x0410', "/I$incm", "/I$vc\Include")
    if ($Flavour -eq 'trace') { $c += '/DUM_TRACE=1' }
    Run "$vc\BIN\CL.EXE" ($c + @("/Fo$obj\usbprnmn.obj", "$src\usbprnmn.c")) | Out-Null
    Run "$vc\BIN\CL.EXE" ($c + @("/Fo$obj\prncore.u.obj", "$src\prncore.c")) | Out-Null
    Run "$tools\MSVC600\Common\MSDev98\Bin\RC.EXE" @('/l', '0x409', "/I$vc\Include", "/fo$obj\usbprnmn.res", "$src\usbprnmn.rc") | Out-Null
    $l = @('/nologo', '/DLL', '/SUBSYSTEM:WINDOWS,4.00', '/ENTRY:DllEntryPoint@12', '/NODEFAULTLIB', '/INCREMENTAL:NO',
           '/OPT:REF', '/OPT:ICF', '/MACHINE:IX86', '/RELEASE', "/DEF:$src\usbprnmn.def", "/MAP:$obj\usbprnmn.map",
           "/IMPLIB:$obj\usbprnmn.lib", "/OUT:$w98\USBPRNMN.DLL", "$obj\usbprnmn.obj", "$obj\prncore.u.obj",
           "$obj\usbprnmn.res", "$vc\Lib\KERNEL32.LIB", "$vc\Lib\USER32.LIB", "$vc\Lib\ADVAPI32.LIB",
           "$vc\Lib\WINSPOOL.LIB")
    Run "$vc\BIN\LINK.EXE" $l | Out-Null
    Run "$vc\BIN\DUMPBIN.EXE" @('/headers', "$w98\USBPRNMN.DLL") | Set-Content -LiteralPath "$top\dll-headers.txt"
    Run "$vc\BIN\DUMPBIN.EXE" @('/imports', "$w98\USBPRNMN.DLL") | Set-Content -LiteralPath "$top\dll-imports.txt"
    Run "$vc\BIN\DUMPBIN.EXE" @('/exports', "$w98\USBPRNMN.DLL") | Set-Content -LiteralPath "$top\dll-exports.txt"
    $r = Assert-DllImports "$top\dll-headers.txt" "$top\dll-imports.txt" "$src\imports-dll.txt" "$src\exports98"
    Add-Content -LiteralPath $gate -Value "USBPRNMN.DLL $r"
    $r = Assert-Exports "$top\dll-exports.txt" "$src\usbprnmn.def"
    Add-Content -LiteralPath $gate -Value "USBPRNMN.DLL $r"
    $script:record += "dll      cflags $($c -join ' ')"
}

function Stage-Inf([string]$name, [string]$dir) {
    $t = [IO.File]::ReadAllText((Join-Path $src $name)) -replace "`r?`n", "`r`n"
    if ($name -eq 'USBPRN.INF') { Assert-UsbprnInf $t (Join-Path $src 'usbprnmn.c') } else { Assert-UsbprnMeInf $t }
    [IO.File]::WriteAllText((Join-Path $dir $name), $t, [Text.Encoding]::ASCII)
    Add-Content -LiteralPath $gate -Value "$name PASS: package contract (sections, model ids, loader values, files, settings), ASCII/CRLF"
}

try {
    Build-Sys
    Build-Dll
    # Negative gate checks on scratch copies of this build's own reports.
    $neg = Join-Path $obj 'neg'
    New-Item -ItemType Directory -Force $neg | Out-Null
    $imp = Get-Content -LiteralPath "$top\sys-imports.txt"
    $at = [Array]::FindIndex([string[]]$imp, [Predicate[string]]{ param($x) $x -match '^\s+[0-9A-Fa-f]+\s+KeSetEvent\s*$' })
    foreach ($name in 'IoAllocateWorkItem', 'KeSetEventX') {
        $m = [System.Collections.ArrayList]@($imp); $m.Insert($at, "                  99  $name")
        $m | Set-Content -LiteralPath "$neg\imports.txt"
        $failed = $false
        try { Assert-SysImports "$top\sys-headers.txt" "$neg\imports.txt" "$src\imports-sys.txt" "$ddk98\all\INCS_DDK\INC_WIN98_WDM.H" | Out-Null } catch { $failed = $true }
        if (-not $failed) { throw "SYS import gate accepted $name" }
    }
    $dimp = Get-Content -LiteralPath "$top\dll-imports.txt"
    $at = [Array]::FindIndex([string[]]$dimp, [Predicate[string]]{ param($x) $x -match '^\s+[0-9A-Fa-f]+\s+CloseHandle\s*$' })
    Get-Content "$src\imports-dll.txt" | Set-Content -LiteralPath "$neg\allow.txt"
    'KERNEL32.DLL!GetFileSizeEx' | Add-Content -LiteralPath "$neg\allow.txt"
    $m = [System.Collections.ArrayList]@($dimp); $m.Insert($at, "                  99  GetFileSizeEx"); $m | Set-Content -LiteralPath "$neg\dimports.txt"
    $failed = $false
    try { Assert-DllImports "$top\dll-headers.txt" "$neg\dimports.txt" "$neg\allow.txt" "$src\exports98" | Out-Null } catch { $failed = $_.Exception.Message -match 'absent from the 98 export table' }
    if (-not $failed) { throw 'DLL import gate accepted an import 98 SE KERNEL32 does not export' }
    $m = [System.Collections.ArrayList]@($dimp); $m[$at] = $m[$at] -replace 'CloseHandle', 'closehandle'; $m | Set-Content -LiteralPath "$neg\dimports.txt"
    $failed = $false
    try { Assert-DllImports "$top\dll-headers.txt" "$neg\dimports.txt" "$src\imports-dll.txt" "$src\exports98" | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw 'DLL import gate accepted a wrongly cased import name' }
    Remove-Item -Recurse -Force -LiteralPath $neg
    Add-Content -LiteralPath $gate -Value 'Negative gate checks PASS: two unlisted SYS imports, an allowlisted DLL import absent from 98 SE KERNEL32 (GetFileSizeEx) and a wrongly cased DLL import are rejected'
    Stage-Inf 'USBPRN.INF' $w98
    Stage-Inf 'USBPRNME.INF' $wme
    $t = & $psExe -NoProfile -ExecutionPolicy Bypass -File "$src\inftest.ps1" 2>&1
    if ($LASTEXITCODE) { throw "INF mutation tests failed: $t" }
    Add-Content -LiteralPath $gate -Value "INF mutation tests $t"
    $t = & $psExe -NoProfile -ExecutionPolicy Bypass -File "$src\hosttest.ps1" 2>&1
    if ($LASTEXITCODE) { throw "host tests failed: $t" }
    Add-Content -LiteralPath $gate -Value "prncore host tests $(($t | Select-Object -Last 1))"
    foreach ($d in @(@($w98, @('USBPRN.SYS', 'USBPRNMN.DLL', 'USBPRN.INF')), @($wme, @('USBPRNME.INF')))) {
        $names = @(Get-ChildItem -LiteralPath $d[0] -File | ForEach-Object { $_.Name })
        if (@(Compare-Object $d[1] $names).Count) { throw "Unexpected files in $($d[0]): $($names -join ', ')" }
    }
    $files = @(Get-ChildItem -LiteralPath $w98, $wme -File)
    $files += Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Extension -in '.c', '.h', '.def', '.rc', '.ps1', '.txt', '.INF', '.cmd' }
    foreach ($f in $files) { $record += "sha256   $((Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash)  $($f.FullName)" }
    $record | Set-Content -Encoding ASCII -LiteralPath (Join-Path $top 'record.txt')
    "PASS usbprn $Flavour (build gates and host tests)" | Set-Content -Encoding ASCII -LiteralPath (Join-Path $top 'PASS.TXT')
    Write-Output "PASS: $top"
} catch {
    Write-Output "FAILED: usbprn $Flavour - $($_.Exception.Message) (see $log)"
    exit 1
} finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
}

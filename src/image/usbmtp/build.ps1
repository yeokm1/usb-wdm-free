# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
#
# build.ps1 -Flavour <free|trace|standbytest>
#   free         the release candidate
#   trace        USBMTP.SYS writes a line per lower completion to QEMU's
#                debug console port 0xE9 (USBMTP_TRACE). Test-only, never
#                released.
#   standbytest  trace, with the standby's waits for quiet bounded at 0 ms
#                (USBMTP_STANDBY_TEST; mtp.md 5.4, "Standby"), for the
#                standby legs under an emulator that retires every cancel
#                at once. Test-only, never released.
#
# usbmtp for Windows 98 SE and ME (docs/design/mtp.md 5, 9), the wdmx86
# profile:
#   USBMTP.SYS   MSVC 6.0 + the Windows 2000 DDK, plus a second compile of
#                each source against the Windows 98 DDK's own WDM/USB
#                headers; import gate (allowlist + declared in the 98 DDK
#                headers); the interface's layouts checked at compile time;
#                the configuration walk's host test (cfgtest.ps1).
#   USBMTP.INF   INF gate and its mutation tests.
# Output: out\image\usbmtp\<flavour>\w98se-me\ (USBMTP.SYS, USBMTP.INF),
# PASS.TXT written last. Not a release: nothing is written under free/.
# The slot model is run by slotmodel.ps1.
param([Parameter(Mandatory)][ValidateSet('free', 'trace', 'standbytest')][string]$Flavour)
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$sys = Join-Path $src 'sys'
$root = [IO.Path]::GetFullPath((Join-Path $src '..\..\..'))
$tools = Join-Path $root 'tools'
$vc = Join-Path $tools 'MSVC600\VC98'
$ddk = Join-Path $tools 'ntddk'
$ddk98 = Join-Path $tools 'win98ddk-extracted'
. (Join-Path $src 'gate.ps1')

# A failed run leaves no earlier PASS.TXT or binaries behind.
$top = Join-Path $root "out\image\usbmtp\$Flavour"
if (Test-Path -LiteralPath $top) { Remove-Item -Recurse -Force -LiteralPath $top }

function Need([string]$path, [string]$kit) {
    if (-not (Test-Path -LiteralPath $path)) { throw "MISSING: $path - install $kit as docs\toolchain.md describes" }
}
Need "$vc\BIN\CL.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$vc\BIN\LINK.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$tools\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL" 'MSVC 6.0 (tools\MSVC600)'
Need "$ddk\inc\wdm.h" 'Windows 2000 DDK (tools\ntddk)'
Need "$ddk\libfre\i386\wdm.lib" 'Windows 2000 DDK (tools\ntddk)'
Need "$ddk\libfre\i386\usbd.lib" 'Windows 2000 DDK (tools\ntddk)'
Need "$ddk98\all\INCS_DDK\INC_WIN98_WDM.H" 'Windows 98 SE DDK (tools\win98ddk-extracted)'
Need "$ddk98\all\INCS_DDK\INC_WIN98_USBDLIB.H" 'Windows 98 SE DDK (tools\win98ddk-extracted)'

$out = Join-Path $top 'w98se-me'
$obj = Join-Path $top 'obj'
New-Item -ItemType Directory -Force $out, $obj | Out-Null
$log = Join-Path $top 'build.log'
$gate = Join-Path $top 'gate.txt'

$head = (git -C $root rev-parse HEAD) 2>$null
$dirty = if (git -C $root status --porcelain -- src/image/usbmtp) { 'dirty' } else { 'clean' }

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

$record = @("package  image\usbmtp", "flavour  $Flavour", "commit   $head", "tree     $dirty (src\image\usbmtp)",
            "kit      MSVC 6.0 (cl 12.00.8804, link 6.00.8447); Windows 2000 DDK; Windows 98 SE DDK headers",
            "date     $(Get-Date -Format s)")

$wdm98 = "$ddk98\all\INCS_DDK\INC_WIN98_WDM.H"
$usbd98 = "$ddk98\all\INCS_DDK\INC_WIN98_USBDLIB.H"
$usbdMe = Join-Path $src 'exports98\usbd-me.txt'

function Build-Sys {
    $c = @('/nologo', '/c', '/W3', '/WX', '/Gz', '/Gy', '/GF', '/Zp8', '/Zl', '/D_X86_=1', '/Di386=1', '/DSTD_CALL',
           '/DCONDITION_HANDLING=1', '/DNT_UP=1', '/DNT_INST=0', '/DWIN32=100', '/D_NT1X_=100', '/DWINNT=1',
           '/D_WIN32_WINNT=0x0500', '/DWIN32_LEAN_AND_MEAN=1', '/DDEVL=1', '/Oxs', '/DDBG=0')
    if ($Flavour -ne 'free') { $c += '/DUSBMTP_TRACE=1' }
    if ($Flavour -eq 'standbytest') {
        # a source without the switch would build a plain trace driver under this flavour's name
        if (-not (Select-String -Quiet -SimpleMatch 'USBMTP_STANDBY_TEST' -LiteralPath "$sys\usbmtp.c")) {
            throw 'usbmtp.c has no standby test build (USBMTP_STANDBY_TEST)'
        }
        $c += '/DUSBMTP_STANDBY_TEST=1'
    }
    $inc = @("/I$ddk\inc\ddk\wdm", "/I$ddk\inc\ddk", "/I$ddk\inc", "/I$vc\Include")
    Run "$vc\BIN\CL.EXE" ($c + $inc + @("/Fo$obj\usbmtp.obj", "$sys\usbmtp.c")) | Out-Null
    # slot.c and cfgwalk.c have no kernel headers: held to /W4, as
    # slotmodel.ps1 and cfgtest.ps1 build them.
    foreach ($f in 'slot', 'cfgwalk') {
        Run "$vc\BIN\CL.EXE" (($c | Where-Object { $_ -ne '/W3' }) + @('/W4', "/Fo$obj\$f.obj", "$sys\$f.c")) | Out-Null
    }
    # Second compile against the 98 DDK's own WDM/USB headers.
    $inc98 = Join-Path $obj 'inc98'
    Copy-Headers $inc98 "$ddk98\all\INCS_DDK" @('WDM', 'NTDEF', 'NTSTATUS', 'BUGCODES', 'NTIOLOGC', 'BASETSD',
        'USB100', 'USBDI', 'USBDLIB', 'USBIOCTL', 'DEVIOCTL')
    $c98 = @('/nologo', '/c', '/W3', '/WX', '/Gz', '/Zp8', '/Zl', '/D_X86_=1', '/Di386=1', '/DSTD_CALL', '/DNT_UP=1',
             '/DWIN32=100', '/DWINNT=1', '/DDBG=0', "/I$inc98", "/I$vc\Include")
    Run "$vc\BIN\CL.EXE" ($c98 + @("/Fo$obj\usbmtp.98.obj", "$sys\usbmtp.c")) | Out-Null
    Run "$tools\MSVC600\Common\MSDev98\Bin\RC.EXE" @('/l', '0x409', "/I$vc\Include", "/fo$obj\usbmtp.res", "$sys\usbmtp.rc") | Out-Null
    $l = @('/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,4.00', '/ENTRY:DriverEntry@8', '/BASE:0x10000', '/ALIGN:0x20',
           '/NODEFAULTLIB', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF', '/MACHINE:IX86', '/RELEASE', "/MAP:$obj\usbmtp.map",
           "/OUT:$out\USBMTP.SYS", "$obj\usbmtp.obj", "$obj\slot.obj", "$obj\cfgwalk.obj", "$obj\usbmtp.res",
           "$ddk\libfre\i386\wdm.lib", "$ddk\libfre\i386\usbd.lib")
    Run "$vc\BIN\LINK.EXE" $l | Out-Null
    Run "$vc\BIN\DUMPBIN.EXE" @('/headers', "$out\USBMTP.SYS") | Set-Content -LiteralPath "$top\sys-headers.txt"
    Run "$vc\BIN\DUMPBIN.EXE" @('/imports', "$out\USBMTP.SYS") | Set-Content -LiteralPath "$top\sys-imports.txt"
    $r = Assert-SysImports "$top\sys-headers.txt" "$top\sys-imports.txt" "$src\imports-sys.txt" $wdm98 $usbd98 $usbdMe
    Add-Content -LiteralPath $gate -Value "USBMTP.SYS $r; also compiles /W3 /WX against the 98 DDK WDM headers; slot.c and cfgwalk.c /W4 /WX"
    $script:record += "sys      cflags $($c -join ' ')"
}

function Stage-Inf([string]$name, [string]$dir) {
    $t = [IO.File]::ReadAllText((Join-Path $src $name)) -replace "`r?`n", "`r`n"
    Assert-UsbmtpInf $t
    [IO.File]::WriteAllText((Join-Path $dir $name), $t, [Text.Encoding]::ASCII)
    Add-Content -LiteralPath $gate -Value "$name PASS: package contract (sections, class and GUID, model ids, loader values, files), ASCII/CRLF"
}

try {
    Build-Sys
    # Negative gate checks on scratch copies of this build's own reports.
    $neg = Join-Path $obj 'neg'
    New-Item -ItemType Directory -Force $neg | Out-Null
    $imp = Get-Content -LiteralPath "$top\sys-imports.txt"
    $at = [Array]::FindIndex([string[]]$imp, [Predicate[string]]{ param($x) $x -match '^\s+[0-9A-Fa-f]+\s+KeSetEvent\s*$' })
    if ($at -lt 0) { throw 'negative checks: KeSetEvent row not found' }
    foreach ($name in 'IoAllocateWorkItem', 'KeSetEventX') {
        $m = [System.Collections.ArrayList]@($imp); $m.Insert($at, "                  99  $name")
        $m | Set-Content -LiteralPath "$neg\imports.txt"
        $failed = $false
        try { Assert-SysImports "$top\sys-headers.txt" "$neg\imports.txt" "$src\imports-sys.txt" $wdm98 $usbd98 $usbdMe | Out-Null } catch { $failed = $true }
        if (-not $failed) { throw "SYS import gate accepted $name" }
    }
    # An allowlisted name the 98 headers do not declare.
    Get-Content "$src\imports-sys.txt" | Set-Content -LiteralPath "$neg\allow.txt"
    'NTOSKRNL.EXE!IoAllocateWorkItem' | Add-Content -LiteralPath "$neg\allow.txt"
    $m = [System.Collections.ArrayList]@($imp); $m.Insert($at, "                  99  IoAllocateWorkItem"); $m | Set-Content -LiteralPath "$neg\imports.txt"
    $failed = $false
    try { Assert-SysImports "$top\sys-headers.txt" "$neg\imports.txt" "$neg\allow.txt" $wdm98 $usbd98 $usbdMe | Out-Null } catch { $failed = $_.Exception.Message -match 'not declared in the 98 DDK' }
    if (-not $failed) { throw 'SYS import gate accepted an import the 98 DDK headers do not declare' }
    Remove-Item -Recurse -Force -LiteralPath $neg
    Add-Content -LiteralPath $gate -Value 'Negative gate checks PASS: two unlisted SYS imports and an allowlisted import the 98 DDK headers do not declare (IoAllocateWorkItem) are rejected'
    Stage-Inf 'USBMTP.INF' $out
    $t = & $psExe -NoProfile -ExecutionPolicy Bypass -File "$src\inftest.ps1" 2>&1
    if ($LASTEXITCODE) { throw "INF mutation tests failed: $t" }
    Add-Content -LiteralPath $gate -Value "INF mutation tests $t"
    $t = & $psExe -NoProfile -ExecutionPolicy Bypass -File "$src\cfgtest.ps1" 2>&1
    if ($LASTEXITCODE) { throw "configuration walk host test failed: $t" }
    Add-Content -LiteralPath $gate -Value "cfgwalk host test $(($t | Select-Object -Last 2) -join ' ')"
    $names = @(Get-ChildItem -LiteralPath $out -File | ForEach-Object { $_.Name })
    if (@(Compare-Object @('USBMTP.SYS', 'USBMTP.INF') $names).Count) { throw "Unexpected files in ${out}: $($names -join ', ')" }
    $files = @(Get-ChildItem -LiteralPath $out -File)
    $files += Get-ChildItem -LiteralPath $src, $sys -File | Where-Object { $_.Extension -in '.c', '.h', '.rc', '.ps1', '.txt', '.INF', '.cmd' }
    foreach ($f in $files) { $record += "sha256   $((Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash)  $($f.FullName)" }
    $record | Set-Content -Encoding ASCII -LiteralPath (Join-Path $top 'record.txt')
    "PASS usbmtp $Flavour (build gates)" | Set-Content -Encoding ASCII -LiteralPath (Join-Path $top 'PASS.TXT')
    Write-Output "PASS: $top"
} catch {
    Write-Output "FAILED: usbmtp $Flavour - $($_.Exception.Message) (see $log)"
    exit 1
} finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
}

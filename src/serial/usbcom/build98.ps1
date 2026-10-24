# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
#
# build98.ps1 -Profile <wdmx86|vxd9x|w98> -Flavour <free|checked|trace>
#   trace = free with UC_TRACE_E9: both binaries write a line per event to
#           QEMU's debug console port 0xE9. Test-only, never released.
#   split = trace with UC_TEST_SPLIT_CANCEL: a close cancels the newest bulk
#           IN alone first (ucio.c UcSplitCancel). Test-only, never released.
#
# The Windows 98 SE / ME half of usbcom (build.md sections 3 to 5):
#   wdmx86  USBCOM.SYS with MSVC 6.0 + the Windows 2000 DDK, plus a second
#           compile against the Windows 98 DDK's own WDM headers, and the
#           import gate (allowlist + 98 WDM.H declarations).
#   vxd9x   USBCOMV.VXD with MSVC 6.0 + the 98 DDK VxD headers, linked
#           /VXD by MSVC 6.0's linker; LE/DDB gate.
#   w98     both, the host tests and the 98 INF gates, staged together as
#           one install candidate (USBCOM.SYS, USBCOMV.VXD, USBCOM.INF,
#           USBCOMP.INF). Not a release: nothing is written under free/.
# Output: out\serial\usbcom\<profile>\<flavour>\, PASS.TXT written last.
param(
    [Parameter(Mandatory)][ValidateSet('wdmx86', 'vxd9x', 'w98')][string]$Profile,
    [Parameter(Mandatory)][ValidateSet('free', 'checked', 'trace', 'split')][string]$Flavour
)
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$root = [IO.Path]::GetFullPath((Join-Path $src '..\..\..'))
$tools = Join-Path $root 'tools'
$vc = Join-Path $tools 'MSVC600\VC98'
$ddk = Join-Path $tools 'ntddk'
$ddk98 = Join-Path $tools 'win98ddk-extracted'
. (Join-Path $src 'gate98.ps1')

function Need([string]$path, [string]$kit) {
    if (-not (Test-Path -LiteralPath $path)) { throw "MISSING: $path - install $kit as docs\toolchain.md describes" }
}
Need "$vc\BIN\CL.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$vc\BIN\LINK.EXE" 'MSVC 6.0 (tools\MSVC600)'
Need "$tools\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL" 'MSVC 6.0 (tools\MSVC600)'
Need "$ddk\inc\wdm.h" 'Windows 2000 DDK (tools\ntddk)'
Need "$ddk\libfre\i386\wdm.lib" 'Windows 2000 DDK (tools\ntddk)'
Need "$ddk98\all\INCS_DDK\INC_WIN98_WDM.H" 'Windows 98 SE DDK (tools\win98ddk-extracted)'
Need "$ddk98\incs\INC_WIN98_VCOMM.H" 'Windows 98 SE DDK (tools\win98ddk-extracted)'

$out = Join-Path $root "out\serial\usbcom\$Profile\$Flavour"
if (Test-Path -LiteralPath $out) { Remove-Item -Recurse -Force -LiteralPath $out }
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out 'build.log'
$gate = Join-Path $out 'gate.txt'

$head = (git -C $root rev-parse HEAD) 2>$null
$dirty = if (git -C $root status --porcelain -- src/serial/usbcom) { 'dirty' } else { 'clean' }

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

$record = @("package  serial\usbcom", "profile  $Profile $Flavour", "commit   $head", "tree     $dirty (src\serial\usbcom)",
            "kit      MSVC 6.0 (cl 12.00.8804, link 6.00.8447); Windows 2000 DDK; Windows 98 SE DDK headers",
            "date     $(Get-Date -Format s)")

function Build-Sys([string]$dir) {
    $obj = Join-Path $dir 'obj'
    New-Item -ItemType Directory -Force $obj | Out-Null
    $c = @('/nologo', '/c', '/W3', '/WX', '/Gz', '/Gy', '/GF', '/Zp8', '/Zl', '/D_X86_=1', '/Di386=1', '/DSTD_CALL',
           '/DCONDITION_HANDLING=1', '/DNT_UP=1', '/DNT_INST=0', '/DWIN32=100', '/D_NT1X_=100', '/DWINNT=1',
           '/D_WIN32_WINNT=0x0500', '/DWIN32_LEAN_AND_MEAN=1', '/DDEVL=1')
    $c += if ($Flavour -eq 'free') { @('/Oxs', '/DDBG=0') } elseif ($Flavour -eq 'trace') { @('/Oxs', '/DDBG=0', '/DUC_TRACE_E9=1') } elseif ($Flavour -eq 'split') { @('/Oxs', '/DDBG=0', '/DUC_TRACE_E9=1', '/DUC_TEST_SPLIT_CANCEL=1') } else { @('/Od', '/Oi', '/Zi', "/Fd$obj\usbcom.pdb", '/DDBG=1') }
    $inc = @("/I$ddk\inc\ddk\wdm", "/I$ddk\inc\ddk", "/I$ddk\inc", "/I$vc\Include")
    foreach ($f in 'usbcom', 'ucio', 'acmcore') {
        Run "$vc\BIN\CL.EXE" ($c + $inc + @("/Fo$obj\$f.obj", "$src\$f.c")) | Out-Null
    }
    # Second compile against the 98 DDK's own WDM/USB headers: anything the
    # 98 headers do not declare fails here.
    $inc98 = Join-Path $dir 'inc98'
    Copy-Headers $inc98 "$ddk98\all\INCS_DDK" @('WDM', 'NTDEF', 'NTSTATUS', 'BUGCODES', 'NTIOLOGC', 'BASETSD',
        'USB100', 'USBDI', 'USBDLIB', 'USBIOCTL', 'DEVIOCTL')
    $c98 = @('/nologo', '/c', '/W3', '/WX', '/Gz', '/Zp8', '/Zl', '/D_X86_=1', '/Di386=1', '/DSTD_CALL', '/DNT_UP=1',
             '/DWIN32=100', '/DWINNT=1', '/DDBG=0', "/I$inc98", "/I$vc\Include")
    foreach ($f in 'usbcom', 'ucio') {
        Run "$vc\BIN\CL.EXE" ($c98 + @("/Fo$obj\$f.98.obj", "$src\$f.c")) | Out-Null
    }
    Run "$tools\MSVC600\Common\MSDev98\Bin\RC.EXE" @('/l', '0x409', "/I$vc\Include", "/fo$obj\usbcom.res", "$src\usbcom.rc") | Out-Null
    $l = @('/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,4.00', '/ENTRY:DriverEntry@8', '/BASE:0x10000', '/ALIGN:0x20',
           '/NODEFAULTLIB', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF', '/MACHINE:IX86', '/RELEASE', "/MAP:$dir\usbcom.map",
           "/OUT:$dir\USBCOM.SYS", "$obj\usbcom.obj", "$obj\ucio.obj", "$obj\acmcore.obj", "$obj\usbcom.res",
           "$ddk\libfre\i386\wdm.lib")
    if ($Flavour -eq 'checked') { $l += @('/DEBUG', "/PDB:$dir\usbcom.pdb") }
    Run "$vc\BIN\LINK.EXE" $l | Out-Null
    Run "$vc\BIN\DUMPBIN.EXE" @('/headers', "$dir\USBCOM.SYS") | Set-Content -LiteralPath "$dir\headers.txt"
    Run "$vc\BIN\DUMPBIN.EXE" @('/imports', "$dir\USBCOM.SYS") | Set-Content -LiteralPath "$dir\imports.txt"
    $r = Assert-SysImports "$dir\headers.txt" "$dir\imports.txt" "$src\imports-wdmx86.txt" "$ddk98\all\INCS_DDK\INC_WIN98_WDM.H"
    Add-Content -LiteralPath $gate -Value "USBCOM.SYS $r; also compiles /W3 /WX against the 98 DDK WDM headers"
    $script:record += "sys      cflags $($c -join ' ')"
}

function Build-Vxd([string]$dir) {
    $obj = Join-Path $dir 'vobj'
    New-Item -ItemType Directory -Force $obj | Out-Null
    $incv = Join-Path $dir 'incvxd'
    Copy-Headers $incv "$ddk98\incs" @('BASEDEF', 'VMM', 'VCOMM', 'NTKERN')
    $c = @('/nologo', '/c', '/W3', '/WX', '/Gd', '/Gs', '/Oy-', '/Zl', '/GF', '/DIS_32', '/D_X86_', '/DWIN32')
    $c += if ($Flavour -eq 'free') { @('/O1') } elseif ($Flavour -in 'trace', 'split') { @('/O1', '/DUC_TRACE_E9=1') } else { @('/Od') }
    Run "$vc\BIN\CL.EXE" ($c + @("/I$incv", "/Fo$obj\usbcomv.obj", "$src\usbcomv.c")) | Out-Null
    Run "$vc\BIN\CL.EXE" ($c + @("/Fo$obj\acmcore.obj", "$src\acmcore.c")) | Out-Null
    # LNK4078 (code and data merged into the locked LCODE class) is expected.
    Run "$vc\BIN\LINK.EXE" @('/nologo', '/VXD', '/NODEFAULTLIB', "/DEF:$src\usbcomv.def", "/MAP:$dir\usbcomv.map",
        "/OUT:$dir\USBCOMV.VXD", "$obj\usbcomv.obj", "$obj\acmcore.obj") | Out-Null
    foreach ($junk in 'USBCOMV.lib', 'USBCOMV.exp') { Remove-Item -LiteralPath (Join-Path $dir $junk) -ErrorAction SilentlyContinue }
    $r = Assert-Vxd "$dir\USBCOMV.VXD"
    Add-Content -LiteralPath $gate -Value "USBCOMV.VXD $r"
    $script:record += "vxd      cflags $($c -join ' ')"
}

function Stage-Inf([string]$name, [string]$dir) {
    $t = [IO.File]::ReadAllText((Join-Path $src $name))
    if ($name -eq 'USBCOM.INF') { Assert-Usbcom98Inf $t } else { Assert-Usbcomp98Inf $t }
    [IO.File]::WriteAllText((Join-Path $dir $name), ($t -replace "`r?`n", "`r`n"), [Text.Encoding]::ASCII)
    Add-Content -LiteralPath $gate -Value "$name PASS: package contract (sections, model ids, loader values, files), ASCII/CRLF"
}

try {
    switch ($Profile) {
        'wdmx86' { Build-Sys $out }
        'vxd9x'  { Build-Vxd $out }
        'w98' {
            Build-Sys $out
            Build-Vxd $out
            # Negative gate checks on scratch copies of this build's own reports.
            $neg = Join-Path $out 'neg'
            New-Item -ItemType Directory -Force $neg | Out-Null
            $imp = Get-Content -LiteralPath "$out\imports.txt"
            $at = [Array]::FindIndex([string[]]$imp, [Predicate[string]]{ param($x) $x -match '^\s+[0-9A-Fa-f]+\s+KeSetEvent\s*$' })
            $cases = @{ 'IoAllocateWorkItem' = 'not in allowlist'; 'KeSetEventX' = 'not in allowlist' }
            foreach ($name in $cases.Keys) {
                $m = [System.Collections.ArrayList]@($imp); $m.Insert($at, "                  99  $name")
                $m | Set-Content -LiteralPath "$neg\imports.txt"
                $failed = $false
                try { Assert-SysImports "$out\headers.txt" "$neg\imports.txt" "$src\imports-wdmx86.txt" "$ddk98\all\INCS_DDK\INC_WIN98_WDM.H" | Out-Null } catch { $failed = $true }
                if (-not $failed) { throw "SYS import gate accepted $name" }
            }
            Get-Content "$src\imports-wdmx86.txt" | Set-Content -LiteralPath "$neg\allow.txt"
            "NTOSKRNL.EXE!IoAllocateWorkItem" | Add-Content -LiteralPath "$neg\allow.txt"
            $m = [System.Collections.ArrayList]@($imp); $m.Insert($at, "                  99  IoAllocateWorkItem"); $m | Set-Content -LiteralPath "$neg\imports.txt"
            $failed = $false
            try { Assert-SysImports "$out\headers.txt" "$neg\imports.txt" "$neg\allow.txt" "$ddk98\all\INCS_DDK\INC_WIN98_WDM.H" | Out-Null } catch { $failed = $_.Exception.Message -match 'not declared in 98 WDM' }
            if (-not $failed) { throw 'SYS import gate accepted an import the 98 WDM.H does not declare' }
            $v = [IO.File]::ReadAllBytes("$out\USBCOMV.VXD")
            $i = [Text.Encoding]::ASCII.GetString($v).IndexOf('USBCOMV ')
            $v[$i] = [byte][char]'X'
            [IO.File]::WriteAllBytes("$neg\BAD.VXD", $v)
            $failed = $false
            try { Assert-Vxd "$neg\BAD.VXD" | Out-Null } catch { $failed = $true }
            if (-not $failed) { throw 'VXD gate accepted a renamed DDB' }
            Remove-Item -Recurse -Force -LiteralPath $neg
            Add-Content -LiteralPath $gate -Value 'Negative gate checks PASS: two unlisted imports, one allowlisted import the 98 WDM.H lacks (IoAllocateWorkItem), and a renamed DDB are all rejected'
            Stage-Inf 'USBCOM.INF' $out
            Stage-Inf 'USBCOMP.INF' $out
            $t = & $psExe -NoProfile -ExecutionPolicy Bypass -File "$src\test98.ps1" 2>&1
            if ($LASTEXITCODE) { throw "98 INF mutation tests failed: $t" }
            Add-Content -LiteralPath $gate -Value "INF mutation tests $t"
            $t = & $psExe -NoProfile -ExecutionPolicy Bypass -File "$src\hosttest.ps1" 2>&1
            if ($LASTEXITCODE) { throw "host tests failed: $t" }
            Add-Content -LiteralPath $gate -Value "acmcore host tests $t"
            foreach ($f in Get-ChildItem -LiteralPath $out -File) {
                if ($f.Name -notin @('USBCOM.SYS', 'USBCOMV.VXD', 'USBCOM.INF', 'USBCOMP.INF', 'build.log', 'gate.txt',
                                     'record.txt', 'headers.txt', 'imports.txt', 'usbcom.map', 'usbcomv.map', 'usbcom.pdb')) {
                    throw "Unexpected file in the staged folder: $($f.Name)"
                }
            }
        }
    }
    $files = @(Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Extension -in '.SYS', '.VXD', '.INF' })
    $files += Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Extension -in '.c', '.h', '.def', '.rc', '.ps1', '.txt', '.INF' }
    foreach ($f in $files) { $record += "sha256   $((Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash)  $($f.FullName)" }
    $record | Set-Content -Encoding ASCII -LiteralPath (Join-Path $out 'record.txt')
    "PASS usbcom $Profile $Flavour (build gates only; no guest has loaded it)" | Set-Content -Encoding ASCII -LiteralPath (Join-Path $out 'PASS.TXT')
    Write-Output "PASS: $out"
} catch {
    Write-Output "FAILED: usbcom $Profile $Flavour - $($_.Exception.Message) (see $log)"
    exit 1
} finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
}

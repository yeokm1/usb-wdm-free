# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Builds MTPPRB.SYS, the M0 probe driver (docs/design/mtp.md 5.6), with the
# wdmx86 profile (Windows 2000 DDK, MSVC 6.0) into out\image\usbmtp\probe\.
# Not shipped. The imports are listed in out\...\probe\sys-imports.txt.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$vc = Join-Path $root 'tools\MSVC600\VC98'
$ddk = Join-Path $root 'tools\ntddk'
foreach ($need in @("$vc\BIN\CL.EXE", "$ddk\inc\wdm.h", "$ddk\libfre\i386\wdm.lib", "$ddk\libfre\i386\usbd.lib")) {
    if (-not (Test-Path -LiteralPath $need)) { throw "MISSING: $need - see docs\toolchain.md" }
}
$out = Join-Path $root 'out\image\usbmtp\probe'
New-Item -ItemType Directory -Force $out | Out-Null
$env:PATH = "$vc\BIN;$root\tools\MSVC600\Common\MSDev98\Bin;$env:SystemRoot\system32"
$c = @('/nologo', '/c', '/W3', '/WX', '/Gz', '/Gy', '/GF', '/Zp8', '/Zl', '/D_X86_=1', '/Di386=1', '/DSTD_CALL',
       '/DCONDITION_HANDLING=1', '/DNT_UP=1', '/DNT_INST=0', '/DWIN32=100', '/D_NT1X_=100', '/DWINNT=1',
       '/D_WIN32_WINNT=0x0500', '/DWIN32_LEAN_AND_MEAN=1', '/DDEVL=1', '/Oxs', '/DDBG=0',
       "/I$ddk\inc\ddk\wdm", "/I$ddk\inc\ddk", "/I$ddk\inc", "/I$vc\Include", "/Fo$out\mtpprb.obj", "$PSScriptRoot\mtpprb.c")
& "$vc\BIN\CL.EXE" @c 2>&1 | Set-Content "$out\drv-build.log"
if ($LASTEXITCODE) { Get-Content "$out\drv-build.log"; throw 'compile failed' }
# The slot contract the probe is built on (slotm0.c: the driver's slot.c before the fallback abort was removed), held to /W4.
$c2 = ($c | Where-Object { $_ -notlike '/Fo*' -and $_ -notlike '*.c' -and $_ -ne '/W3' }) + @('/W4', "/Fo$out\slot.obj", "$PSScriptRoot\slotm0.c")
& "$vc\BIN\CL.EXE" @c2 2>&1 | Add-Content "$out\drv-build.log"
if ($LASTEXITCODE) { Get-Content "$out\drv-build.log"; throw 'slot.c compile failed' }
$l = @('/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,4.00', '/ENTRY:DriverEntry@8', '/BASE:0x10000', '/ALIGN:0x20',
       '/NODEFAULTLIB', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF', '/MACHINE:IX86', '/RELEASE',
       "/OUT:$out\MTPPRB.SYS", "$out\mtpprb.obj", "$out\slot.obj", "$ddk\libfre\i386\wdm.lib", "$ddk\libfre\i386\usbd.lib")
& "$vc\BIN\LINK.EXE" @l 2>&1 | Add-Content "$out\drv-build.log"
if ($LASTEXITCODE) { Get-Content "$out\drv-build.log"; throw 'link failed' }
& "$vc\BIN\DUMPBIN.EXE" /imports "$out\MTPPRB.SYS" | Set-Content "$out\sys-imports.txt"
'MTPPRB.SYS built'

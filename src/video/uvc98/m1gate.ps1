# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
param([string]$Output,[string]$Repository,[switch]$VMTrace)
$ErrorActionPreference='Stop'
$flavour=Split-Path $Output -Leaf
if($flavour -notin @('free','checked')) { throw 'Unknown build flavour' }
$headers=[IO.File]::ReadAllText((Join-Path $Output 'headers.txt'))
$imports=[IO.File]::ReadAllText((Join-Path $Output 'imports.txt'))
if($headers -notmatch '(?m)^\s+14C machine \(i386\)' -or
   $headers -notmatch '(?m)^\s+4\.00 subsystem version' -or
   $headers -notmatch '(?m)^\s+1 subsystem \(Native\)') { throw 'Unexpected PE target' }
$modules=@([regex]::Matches($imports,'(?mi)^    ([A-Za-z0-9_.]+\.(?:sys|dll|exe))\s*$') | ForEach-Object {$_.Groups[1].Value.ToLowerInvariant()})
$expected=if($flavour -eq 'checked') {'hal.dll,ntoskrnl.exe,stream.sys'} else {'ntoskrnl.exe,stream.sys'}
if(($modules | Sort-Object) -join ',' -ne $expected) { throw 'Unexpected dependency modules' }
$block=($imports -split '(?m)^\s*Summary\s*$')[0]
$names=@([regex]::Matches($block,'(?m)^\s+[0-9A-F]+\s+([A-Za-z_?][\w@$?]*)\s*$') | ForEach-Object {$_.Groups[1].Value})
$allowed=@('InterlockedCompareExchange','PsTerminateSystemThread','KeWaitForSingleObject',
 'KeSetEvent','RtlExtendedLargeIntegerDivide','KeQueryInterruptTime','PsCreateSystemThread',
 'ZwClose','ObfDereferenceObject',
 'MmMapLockedPages','InterlockedExchange','ExAllocatePoolWithTag',
 'ExFreePool','ObfReferenceObject','ExQueueWorkItem','KeInitializeEvent',
 # 98 SE join (uvcwork.c): Exited event, thread object only on WDM 1.10+
 'IoIsWdmVersionAvailable','KeDelayExecutionThread','ObReferenceObjectByHandle',
 'StreamClassScheduleTimer','StreamClassCallAtNewPriority','StreamClassDeviceNotification',
 'StreamClassRegisterAdapter','StreamClassCompleteRequestAndMarkQueueReady')
if($flavour -eq 'checked') { $allowed+=@('RtlAssert','KeGetCurrentIrql') }
if($VMTrace) {
    if($flavour -ne 'checked') { throw 'VM trace requires checked build' }
    $allowed+='WRITE_PORT_UCHAR'
}
if($names.Count -ne $allowed.Count) { throw 'Unexpected import count' }
foreach($name in $names) { if($allowed -notcontains $name) { throw "Unexpected import $name" } }
$inf=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'UVCSIM1.INF'))
if($inf -notmatch '(?m)^Signature="\$Windows NT\$"' -or
   $inf -notmatch '(?m)^%Device%=Capture,ROOT\\UVCSIM1\s*$' -or
   $inf -match '(?mi)USB\\|\[Capture\]|DevLoader|NTMPDriver') { throw 'INF target escaped NT-only ROOT experiment' }
'PASS x86 native4.00, exact NT2000+ prototype import allowlist, NT-only ROOT\UVCSIM1 INF; no target runtime claim'

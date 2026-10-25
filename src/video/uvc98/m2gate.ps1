# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# M2 gate: x86 native 4.00 PE, kernel-only dependencies (no usbd.sys: URBs are
# built by hand), an exact import allowlist and the class-matched INFs.
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
if(($modules | Sort-Object) -join ',' -ne 'hal.dll,ntoskrnl.exe,stream.sys') { throw "Unexpected dependency modules: $($modules -join ',')" }
$block=($imports -split '(?m)^\s*Summary\s*$')[0]
$names=@([regex]::Matches($block,'(?m)^\s+[0-9A-F]+\s+([A-Za-z_?][\w@$?]*)\s*$') | ForEach-Object {$_.Groups[1].Value})
$allowed=@('InterlockedCompareExchange','PsTerminateSystemThread','KeWaitForSingleObject',
 'KeSetEvent','RtlExtendedLargeIntegerDivide','KeQueryInterruptTime','PsCreateSystemThread',
 'ZwClose','ObfDereferenceObject',
 'MmMapLockedPages','InterlockedExchange','ExAllocatePoolWithTag',
 'ExFreePool','ObfReferenceObject','ExQueueWorkItem','KeInitializeEvent',
 # 98 SE join (uvcwork.c): Exited event, thread object only on WDM 1.10+
 'IoIsWdmVersionAvailable','KeDelayExecutionThread','ObReferenceObjectByHandle',
 # StreamClassDeviceNotification: none since UNINITIALIZE reopens the device
 # queue too (uvcportn.c; docs/testing/2026-10-24-uvc98-vfw.md)
 'StreamClassScheduleTimer','StreamClassCallAtNewPriority',
 'StreamClassRegisterAdapter','StreamClassCompleteRequestAndMarkQueueReady',
 # M2 USB transport (uvcusb.c)
 'IoAllocateIrp','IoFreeIrp','IofCallDriver','IoCancelIrp','ObReferenceObjectByHandle',
 'KeDelayExecutionThread','KfAcquireSpinLock','KfReleaseSpinLock','KeInitializeSpinLock',
 'ExAcquireFastMutex','ExReleaseFastMutex','KeClearEvent')
if($flavour -eq 'checked') { $allowed+=@('RtlAssert','KeGetCurrentIrql') }
if($VMTrace) {
    if($flavour -ne 'checked') { throw 'VM trace requires checked build' }
    $allowed+='WRITE_PORT_UCHAR'
}
$extra=@($names | Where-Object { $allowed -notcontains $_ })
$missing=@($allowed | Where-Object { $names -notcontains $_ })
if($extra.Count -or $missing.Count) { throw "Import mismatch; unexpected: $($extra -join ',') missing: $($missing -join ',')" }
# The INFs (design 4; owner 2026-10-20 and 2026-10-24): the video function's
# interface-level class ids, exactly these three; no whole-device class match
# (no DevClass, no Class_EF, no bare USB\Class_0E, no VideoStreaming 0E/02);
# whole-device lines only for the listed per-camera VID/PIDs.
$classIds=@('USB\Class_0E&SubClass_03&Prot_00','USB\Class_0E&SubClass_01&Prot_00','USB\Class_0E&SubClass_01&Prot_01')
$cameraIds=@('USB\VID_046D&PID_094C')
function Test-UvcModels([string]$text,[string]$name) {
    # Every entry of every Models section the [Manufacturer] line names (any
    # decoration), whitespace and case normalised, trailing comments removed;
    # each id after the install section is checked (Codex rel-fixes round 1).
    $lines=@($text -split "`r?`n" | ForEach-Object { ($_ -split ';',2)[0].Trim() })
    $manu=@(); $section=''
    foreach($l in $lines) {
        if($l -match '^\[(.+)\]$') { $section=$Matches[1].Trim().ToLowerInvariant(); continue }
        if($section -eq 'manufacturer' -and $l -match '=') {
            $parts=@(($l -split '=',2)[1] -split ',' | ForEach-Object { $_.Trim() })
            $manu+=$parts[0].ToLowerInvariant()
            foreach($d in $parts[1..($parts.Count-1)]) { if($d) { $manu+=($parts[0]+'.'+$d).ToLowerInvariant() } }
        }
    }
    if(-not $manu.Count) { throw "${name}: no [Manufacturer] models" }
    $ids=@(); $section=''
    foreach($l in $lines) {
        if($l -match '^\[(.+)\]$') { $section=$Matches[1].Trim().ToLowerInvariant(); continue }
        if($manu -contains $section -and $l -match '=') {
            $parts=@(($l -split '=',2)[1] -split ',' | ForEach-Object { $_.Trim() })
            if($parts.Count -lt 2) { throw "${name}: model line without an id: $l" }
            $ids+=@($parts[1..($parts.Count-1)] | Where-Object { $_ })
        }
    }
    $allow=@($classIds+$cameraIds | ForEach-Object { $_.ToUpperInvariant() })
    $have=@($ids | ForEach-Object { $_.ToUpperInvariant() })
    $bad=@($have | Where-Object { $allow -notcontains $_ })
    $miss=@($allow | Where-Object { $have -notcontains $_ })
    if($bad.Count -or $miss.Count) { throw "${name}: models not the interface-level set; unexpected: $($bad -join ',') missing: $($miss -join ',')" }
}
$inf=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'UVCM2.INF'))
if($inf -notmatch '(?m)^Signature="\$Windows NT\$"' -or
   $inf -match '(?mi)\[Capture\]|DevLoader|NTMPDriver') { throw 'UVCM2.INF is not the NT form' }
Test-UvcModels $inf 'UVCM2.INF'
# The 98 SE / ME form: the same models, NTKERN loader, no NT sections.
$inf98=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'UVCM298.INF'))
if($inf98 -notmatch '(?m)^Signature="\$CHICAGO\$"' -or
   $inf98 -notmatch '(?m)^HKR,,NTMPDriver,,uvcm2\.sys\s*$' -or
   $inf98 -match '(?mi)\.NT\]') { throw 'UVCM298.INF is not the 98 SE / ME form' }
Test-UvcModels $inf98 'UVCM298.INF'
'PASS x86 native4.00, exact M2 import allowlist, interface-level class INFs (NT and 98 SE/ME); no target runtime claim'

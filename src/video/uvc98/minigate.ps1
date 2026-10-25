# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
param([string]$Output,[string]$Repository)
$ErrorActionPreference='Stop'
$headers=[IO.File]::ReadAllText((Join-Path $Output 'headers.txt'))
$imports=[IO.File]::ReadAllText((Join-Path $Output 'imports.txt'))
if($headers -notmatch '(?m)^\s+14C machine \(i386\)' -or
   $headers -notmatch '(?m)^\s+4\.00 subsystem version' -or
   $headers -notmatch '(?m)^\s+1 subsystem \(Native\)') { throw 'Unexpected PE target' }
$modules=@([regex]::Matches($imports,'(?m)^    ([A-Za-z0-9_.]+\.(?:SYS|DLL))\s*$') | ForEach-Object {$_.Groups[1].Value})
if($modules.Count -ne 1 -or $modules[0] -ine 'STREAM.SYS') { throw 'Unexpected dependency module' }
$importBlock=($imports -split '(?m)^\s*Summary\s*$')[0]
$names=@([regex]::Matches($importBlock,'(?m)^\s+[0-9A-F]+\s+([A-Za-z_?][\w@$?]*)\s*$') | ForEach-Object {$_.Groups[1].Value})
$allowed=@('StreamClassDeviceNotification','StreamClassStreamNotification','StreamClassRegisterAdapter')
if($names.Count -ne 3) { throw 'Unexpected import count' }
foreach($name in $names) {
    if($allowed -notcontains $name) { throw "Unexpected import $name" }
    foreach($fixture in 'w98strm.txt','wmestrm.txt') {
        $path=Join-Path $Repository ('docs/testing/exports/'+$fixture)
        $text=[IO.File]::ReadAllText($path)
        if($text -notmatch ('(?m)^\d+ '+[regex]::Escape($name)+'\s*$')) { throw "$name absent from $fixture" }
    }
}
'PASS x86 native4.00, three STREAM.SYS imports present in recorded98SE/ME export tables'

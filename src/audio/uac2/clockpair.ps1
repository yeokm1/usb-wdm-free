# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Diagnostic pair: exact retained main/descriptor/rate/resource objects;
# only uacusb.c is compiled twice. Does not overwrite/stage any package SYS.
param([string]$Scratch='E:/usb-wdm-free/wave2/uac-clock-pair')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$dest=[IO.Path]::GetFullPath($Scratch)
$oldSource='E:/usb-wdm-free/wave2/uac-watchdog-baseline/uacusb.c'
New-Item -ItemType Directory -Force $dest | Out-Null
Get-ChildItem -LiteralPath $dest -Filter PASS.TXT -Recurse | ForEach-Object {Remove-Item -LiteralPath $_.FullName}
if((Get-FileHash $oldSource).Hash -ne '2E321F864D6F133141B5113FC42E248609542F84576AF786902D3137415536FE') {throw 'Baseline source identity changed'}
if((Get-FileHash (Join-Path (Split-Path $oldSource -Parent) uacusb.h)).Hash -ne (Get-FileHash "$PSScriptRoot/uacusb.h").Hash) {throw 'Transport header changed'}
New-Item -ItemType Directory -Force $dest | Out-Null
if(Test-Path "$dest/PASS.TXT") {Remove-Item -LiteralPath "$dest/PASS.TXT"}
$sourceInputs=@($oldSource,(Join-Path (Split-Path $oldSource -Parent) uacusb.h),(Join-Path (Split-Path $oldSource -Parent) imports-wdmx86.txt),(Join-Path (Split-Path $oldSource -Parent) imports-wdm64.txt),"$PSScriptRoot/imports-wdmx86.txt","$PSScriptRoot/imports-wdm64.txt","$PSScriptRoot/uacusb.c","$PSScriptRoot/uacusb.h","$PSScriptRoot/uacrate.h","$PSScriptRoot/uacwatch.h",$PSCommandPath) | ForEach-Object {Get-FileHash -LiteralPath $_}
$sourceInputs | ForEach-Object {"$($_.Hash)  $($_.Path)"} | Set-Content "$dest/source-inputs.sha256"
$saved=$env:PATH
try {
 foreach($profile in @('wdmx86','wdm64')) {foreach($flavour in @('free','checked')) {
  $base=Join-Path $repo "out/audio/uac2/$profile/$flavour"
  if(!(Test-Path "$base/PASS.TXT")) {throw "Missing retained successful build: $base"}
  $retained=@('uac2wdm.obj','uacdesc.obj','uacrate.obj','uac2wdm.res','record.txt','uac2wdm.sys') | ForEach-Object {Get-FileHash -LiteralPath "$base/$_"}
  $record=Get-Content "$base/record.txt"
  $c=($record | Where-Object {$_ -match '^cflags\s+'}) -replace '^cflags\s+',''
  $c=$c -replace '/Fd"[^"]+"',''
  $cflags=@($c -split '\s+' | Where-Object {$_})
  $includes=@([regex]::Matches(($record | Where-Object {$_ -match '^include\s+'}),'/I"([^"]+)"') | ForEach-Object {'/I'+$_.Groups[1].Value})
  $includes+=('/I'+$PSScriptRoot)
  $l=($record | Where-Object {$_ -match '^lflags\s+'}) -replace '^lflags\s+',''
  $l=$l -replace '/PDB:"[^"]+"',''
  $lflags=@($l -split '\s+' | Where-Object {$_})
  $libs=@([regex]::Matches(($record | Where-Object {$_ -match '^libs\s+'}),'"([^"]+)"') | ForEach-Object {$_.Groups[1].Value})
  if($profile -eq 'wdmx86') {
   $bin="$repo/tools/MSVC600/VC98/BIN";$dump="$bin/dumpbin.exe";$dumpArgs=@('/nologo')
   $env:PATH="$bin;$repo/tools/MSVC600/Common/MSDev98/Bin;$saved";$machine='14C';$subsystem='4.00'
  } else {
   $bin="$repo/tools/WinDDK71/bin/x86/amd64";$dump="$bin/link.exe";$dumpArgs=@('/dump')
   $env:PATH="$bin;$repo/tools/WinDDK71/bin/x86;$saved";$machine='8664';$subsystem='5.02'
  }
  foreach($variant in @('baseline','monotonic')) {
   $out=Join-Path $dest "$profile/$flavour/$variant"
   New-Item -ItemType Directory -Force $out | Out-Null
   if(Test-Path "$out/PASS.TXT") {Remove-Item -LiteralPath "$out/PASS.TXT"}
   $retained | ForEach-Object {"$($_.Hash)  $($_.Path)"} | Set-Content "$out/retained-inputs.sha256"
   Copy-Item -LiteralPath "$base/record.txt" -Destination "$out/retained-record.txt"
   $src=if($variant -eq 'baseline') {$oldSource} else {"$PSScriptRoot/uacusb.c"}
   $cf=@($cflags);$lf=@($lflags)
   if($flavour -eq 'checked') {$cf+="/Fd$out/transport.cpdb";$lf+="/PDB:$out/transport.pdb"}
   & "$bin/cl.exe" /nologo /c @cf @includes "/Fo$out/uacusb.obj" $src > "$out/build.log" 2>&1
   if($LASTEXITCODE) {throw "Compile failed: $profile/$flavour/$variant"}
   $objects=@("$base/uac2wdm.obj","$base/uacdesc.obj","$base/uacrate.obj","$out/uacusb.obj","$base/uac2wdm.res")
   & "$bin/link.exe" /nologo @lf "/OUT:$out/uac2wdm.sys" "/MAP:$out/uac2wdm.map" @objects @libs >> "$out/build.log" 2>&1
   if($LASTEXITCODE) {throw "Relink failed: $profile/$flavour/$variant"}
   & $dump @dumpArgs /headers "$out/uac2wdm.sys" > "$out/headers.txt"
   if($LASTEXITCODE) {throw 'Own PE header read failed'}
   & $dump @dumpArgs /imports "$out/uac2wdm.sys" > "$out/imports.txt"
   if($LASTEXITCODE) {throw 'Own PE import read failed'}
   $allow=if($variant -eq 'baseline') {Join-Path (Split-Path $oldSource -Parent) "imports-$profile.txt"} else {"$PSScriptRoot/imports-$profile.txt"}
   & "$PSScriptRoot/chkimp.ps1" -Headers "$out/headers.txt" -Imports "$out/imports.txt" -Allowed $allow -Machine $machine -Subsystem $subsystem -Report "$out/gate.txt" > "$out/gate.log"
   if(!$?) {throw "Import gate failed: $profile/$flavour/$variant"}
   Get-FileHash "$out/uacusb.obj","$out/uac2wdm.sys" | ForEach-Object {"$($_.Hash)  $($_.Path)"} | Set-Content "$out/outputs.sha256"
   foreach($inputHash in @($sourceInputs)+@($retained)) {if((Get-FileHash -LiteralPath $inputHash.Path).Hash -ne $inputHash.Hash) {throw "Input changed: $($inputHash.Path)"}}
   'PASS retained-object diagnostic pair; NOT installed; NOT literal C7 relink' | Set-Content "$out/PASS.TXT"
   Write-Output "PASS $profile/$flavour/$variant"
  }
 }}
 'PASS eight diagnostic links/import gates; no guest/runtime/release claim' | Set-Content "$dest/PASS.TXT"
} finally {$env:PATH=$saved}




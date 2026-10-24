# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Isolated guest safety check: recompiles all four native source units with
# watchdog/cleanup changes and never overwrites ordinary package outputs.
# Requires prior normal successful builds in all four named profiles.
param([string]$Scratch='E:/usb-wdm-free/wave2/uac-watchdog-check')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$dest=[IO.Path]::GetFullPath($Scratch)
New-Item -ItemType Directory -Force $dest | Out-Null
# Invalidate every old success marker before any operation that can fail.
foreach($marker in @("$dest/PASS.TXT","$dest/wdmx86/free/PASS.TXT","$dest/wdmx86/checked/PASS.TXT",
    "$dest/wdm64/free/PASS.TXT","$dest/wdm64/checked/PASS.TXT")) {
    if(Test-Path -LiteralPath $marker) {Remove-Item -LiteralPath $marker}
}
$sourceInputs=@('uac2wdm.cpp','uac2wdm.rc','uacstream.h','streamtest.cpp','uacdesc.c','uacdesc.h',
    'uacrate.c','uacrate.h','uacusb.c','uacusb.h','uacwatch.h','watchtest.c','watchchk.ps1',
    '../common/punknown.h','imports-wdmx86.txt','imports-wdm64.txt') |
    ForEach-Object {Get-FileHash -LiteralPath "$PSScriptRoot/$_"}
$sourceInputs | ForEach-Object {"$($_.Hash)  $($_.Path)"} | Set-Content "$dest/source-inputs.sha256"
$saved=$env:PATH
try {
    $vc=Join-Path $repo 'tools/MSVC600/VC98'
    $env:PATH="$vc/BIN;$repo/tools/MSVC600/Common/MSDev98/Bin;$saved"
    & "$vc/BIN/cl.exe" /nologo /W4 /WX /MD /O2 /I"$vc/Include" /Fe"$dest/watchtest.exe" /Fo"$dest/watchtest.obj" "$PSScriptRoot/watchtest.c" /link /libpath:"$vc/Lib" > "$dest/host-build.log" 2>&1
    if($LASTEXITCODE) {throw 'Watchdog host-test build failed'}
    & "$dest/watchtest.exe" | Tee-Object "$dest/host-test.log"
    if($LASTEXITCODE) {throw 'Watchdog predicate test failed'}
    & "$vc/BIN/cl.exe" /nologo /W3 /WX /MD /O1 /D_X86_=1 /DPOOL_TAGGING=1 /I"$PSScriptRoot/../common" /I"$repo/tools/ntddk/inc/ddk" /I"$repo/tools/ntddk/inc" /I"$vc/Include" /Fe"$dest/streamtest.exe" /Fo"$dest/streamtest.obj" "$PSScriptRoot/streamtest.cpp" /link /libpath:"$vc/Lib" > "$dest/stream-build.log" 2>&1
    if($LASTEXITCODE) {throw 'Actual stream-header host test build failed'}
    & "$dest/streamtest.exe" | Tee-Object "$dest/stream-test.log"
    if($LASTEXITCODE) {throw 'Stream cleanup regression failed'}
    $source=Get-Content "$PSScriptRoot/uacusb.c" -Raw
    if($source -match 'KeQuerySystemTime' -or ([regex]::Matches($source,'KeQueryInterruptTime\(').Count -ne 4)) {throw 'Worker freshness must use interrupt time at all four observations'}
    foreach($profile in @('wdmx86','wdm64')) {foreach($flavour in @('free','checked')) {
        $base=Join-Path $repo "out/audio/uac2/$profile/$flavour"
        $out=Join-Path $dest "$profile/$flavour"
        New-Item -ItemType Directory -Force $out | Out-Null
        if(!(Test-Path "$base/PASS.TXT")) {throw "Prior baseline build required: $base"}
        $inputPaths=@('uac2wdm.obj','uacdesc.obj','uacrate.obj','uac2wdm.res','record.txt','uac2wdm.sys') |
            ForEach-Object {Join-Path $base $_}
        $baselineInputs=@($inputPaths | ForEach-Object {Get-FileHash -LiteralPath $_})
        $baselineInputs | ForEach-Object {"$($_.Hash)  $($_.Path)"} | Set-Content "$out/baseline-inputs.sha256"
        Copy-Item -LiteralPath "$base/record.txt" -Destination "$out/baseline-record.txt"
        $record=Get-Content "$base/record.txt"
        # Tokenize only the known locally-generated record flags. No shell
        # evaluates record text; quoted include/library paths stay arguments.
        $c=($record | Where-Object {$_ -match '^cflags\s+'}) -replace '^cflags\s+',''
        $c=$c -replace '/Fd"[^"]+"',''
        $cflags=@($c -split '\s+' | Where-Object {$_})
        if($flavour -eq 'checked') {$cflags+="/Fd$out/transport.cpdb"}
        $inc=($record | Where-Object {$_ -match '^include\s+'})
        $includes=@([regex]::Matches($inc,'/I"([^"]+)"') | ForEach-Object {'/I'+$_.Groups[1].Value})
        $l=($record | Where-Object {$_ -match '^lflags\s+'}) -replace '^lflags\s+',''
        $l=$l -replace '/PDB:"[^"]+"',''
        $lflags=@($l -split '\s+' | Where-Object {$_})
        if($flavour -eq 'checked') {$lflags+="/PDB:$out/transport.pdb"}
        $lib=($record | Where-Object {$_ -match '^libs\s+'})
        $libs=@([regex]::Matches($lib,'"([^"]+)"') | ForEach-Object {$_.Groups[1].Value})
        if($profile -eq 'wdmx86') {
            $bin="$repo/tools/MSVC600/VC98/BIN";$dump="$bin/dumpbin.exe";$dumpArgs=@('/nologo')
            $env:PATH="$bin;$repo/tools/MSVC600/Common/MSDev98/Bin;$saved";$machine='14C';$subsystem='4.00'
        } else {
            $bin="$repo/tools/WinDDK71/bin/x86/amd64";$dump="$bin/link.exe";$dumpArgs=@('/dump')
            $env:PATH="$bin;$repo/tools/WinDDK71/bin/x86;$saved";$machine='8664';$subsystem='5.02'
        }
        '' | Set-Content "$out/build.log"
        foreach($unit in @('uac2wdm.cpp','uacdesc.c','uacrate.c','uacusb.c')) {
            $stem=[IO.Path]::GetFileNameWithoutExtension($unit)
            & "$bin/cl.exe" /nologo /c @cflags @includes "/Fo$out/$stem.obj" "$PSScriptRoot/$unit" >> "$out/build.log" 2>&1
            if($LASTEXITCODE) {throw "Native compile failed: $unit $profile/$flavour"}
        }
        $objects=@("$out/uac2wdm.obj","$out/uacdesc.obj","$out/uacrate.obj","$out/uacusb.obj","$base/uac2wdm.res")
        & "$bin/link.exe" /nologo @lflags "/OUT:$out/watchdog.sys" "/MAP:$out/watchdog.map" @objects @libs >> "$out/build.log" 2>&1
        if($LASTEXITCODE) {throw "Transport relink failed: $profile/$flavour"}
        & $dump @dumpArgs /headers "$out/watchdog.sys" > "$out/headers.txt"
        if($LASTEXITCODE) {throw 'Own-PE headers failed'}
        & $dump @dumpArgs /imports "$out/watchdog.sys" > "$out/imports.txt"
        if($LASTEXITCODE) {throw 'Own-PE imports failed'}
        & "$PSScriptRoot/chkimp.ps1" -Headers "$out/headers.txt" -Imports "$out/imports.txt" -Allowed "$PSScriptRoot/imports-$profile.txt" -Machine $machine -Subsystem $subsystem -Report "$out/gate.txt" > "$out/gate.log"
        if(!$?) {throw "Own import gate failed: $profile/$flavour"}
        foreach($input in @($sourceInputs)+@($baselineInputs)) {
            if((Get-FileHash -LiteralPath $input.Path).Hash -ne $input.Hash) {throw "Input changed during check: $($input.Path)"}
        }
        Get-FileHash "$out/watchdog.sys" | Format-List Path,Hash | Out-File "$out/hash.txt"
        'PASS isolated transport compile/relink/import gate; NOT installed' | Set-Content "$out/PASS.TXT"
        Write-Output "PASS watchdog transport compile/relink/imports $profile/$flavour"
    }}
    'PASS host deadline/stream/source-clock gates + four isolated native compiles/relinks; no runtime claim' | Set-Content "$dest/PASS.TXT"
} finally {$env:PATH=$saved}

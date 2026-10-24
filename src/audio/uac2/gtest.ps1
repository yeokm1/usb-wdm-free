# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Optional independent host audit with a separately installed named GCC.
# No downloads, installations, driver builds, staging or device operations.
param([Parameter(Mandatory)][string]$Compiler)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$gcc=(Resolve-Path -LiteralPath $Compiler).Path
$dest=Join-Path $repo 'out/audio/uac2/gcccheck'
New-Item -ItemType Directory -Force $dest | Out-Null
$saved=$env:PATH
try {
    $env:PATH="$(Split-Path $gcc);$saved"
    & $gcc --version | Set-Content "$dest/compiler.txt"
    if($LASTEXITCODE) {throw 'Cannot query GCC'}
    $flags=@('-std=c99','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation')
    foreach($mode in @('normal','trap')) {
        $extra=@();if($mode -eq 'trap') {$extra=@('-fsanitize=undefined','-fsanitize-undefined-trap-on-error')}
        foreach($kind in @('control','mixer')) {
            $files=@("$PSScriptRoot/uacctl.c","$PSScriptRoot/ctltest.c")
            if($kind -eq 'mixer') {$files=@("$PSScriptRoot/uacctl.c","$PSScriptRoot/uacmix.c","$PSScriptRoot/mixtest.c")}
            $exe=Join-Path $dest "$kind-$mode.exe"
            & $gcc @flags @extra -o $exe @files 2>&1 | Set-Content "$dest/$kind-$mode-build.log"
            if($LASTEXITCODE) {throw "GCC $kind/$mode build failed"}
            & $exe | Tee-Object -FilePath "$dest/$kind-$mode.log"
            if($LASTEXITCODE) {throw "GCC $kind/$mode test failed"}
        }
    }
    'PASS independent GCC normal and undefined-behavior trap checks; host only' | Set-Content "$dest/PASS.TXT"
} finally {$env:PATH=$saved}

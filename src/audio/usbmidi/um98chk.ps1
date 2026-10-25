# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Research-only dispatcher host test and real Win98 WDM header compile.
# Does not build or stage an installable kernel driver.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
$dest=Join-Path $repo 'out/audio/usbmidi/98check'
$headers=Join-Path $dest 'include'
$kit=Join-Path $repo 'tools/win98ddk-extracted/all/INCS_DDK'
$vc=Join-Path $repo 'tools/MSVC600/VC98'
New-Item -ItemType Directory -Force $headers | Out-Null
# Copy only explicitly named published kit headers; never inspect sample trees.
foreach($name in @('WDM','NTDEF','NTSTATUS','BUGCODES','NTIOLOGC','BASETSD')) {
    Copy-Item -LiteralPath "$kit/INC_WIN98_$name.H" -Destination "$headers/$name.h"
}
$saved=$env:PATH
try {
    $env:PATH="$vc/BIN;$repo/tools/MSVC600/Common/MSDev98/Bin;$saved"
    Push-Location $dest
    try {
        & "$vc/BIN/cl.exe" /nologo /W3 /WX /O2 /DUM_CLEAN_TEST /DUM_CLEAN_RESEARCH /I"$vc/Include" /I"$PSScriptRoot" /Feumcltest.exe "$PSScriptRoot/umcltest.c" "$PSScriptRoot/umclean.c" /link /libpath:"$vc/Lib" kernel32.lib
        if($LASTEXITCODE) {throw 'Cleanup host test compile failed'}
        & ./umcltest.exe
        if($LASTEXITCODE) {throw 'Cleanup host contract test failed'}
        & "$vc/BIN/cl.exe" /nologo /c /W3 /WX /Gz /Zp8 /DUM_CLEAN_RESEARCH /D_X86_=1 /Di386=1 /DNT_UP=1 /DWIN32=100 /DWINNT=1 /DDBG=0 /I"$headers" /I"$vc/Include" /I"$repo/tools/ntddk/inc" /Foumclean98.obj "$PSScriptRoot/umclean.c"
        if($LASTEXITCODE) {throw 'Actual Win98 WDM header compile failed'}
        & "$vc/BIN/dumpbin.exe" /nologo /symbols umclean98.obj | Set-Content symbols.txt
        if($LASTEXITCODE) {throw 'Own-object symbol listing failed'}
        $symbols=Get-Content symbols.txt -Raw
        if($symbols -match 'Io(Allocate|Queue|Free)WorkItem|ExQueueWorkItem|OBJ_KERNEL_HANDLE') {throw 'Unexpected newer/lifetime-unsafe dependency'}
        'PASS: host dispatcher contracts and Win98 primary-header compile; RESEARCH ONLY, no installable SYS' | Set-Content PASS.TXT
    } finally {Pop-Location}
} finally {$env:PATH=$saved}

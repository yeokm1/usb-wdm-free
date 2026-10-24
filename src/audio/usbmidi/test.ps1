# Original portable-core tests; no driver install or external downloads.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$dest = Join-Path $repo 'out/audio/usbmidi'
New-Item -ItemType Directory -Force $dest | Out-Null
$compiler = Join-Path $repo 'tools/MSVC600/VC98/BIN/cl.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw 'Install named MSVC 6.0 toolchain under tools/MSVC600 first.' }
$saved = $env:PATH
try {
    $env:PATH = "$repo/tools/MSVC600/VC98/BIN;$repo/tools/MSVC600/Common/MSDev98/Bin;$saved"
    Push-Location $dest
    try {
        & $compiler /nologo /W3 /WX /O2 /DWIN32_LEAN_AND_MEAN /I"$repo/tools/MSVC600/VC98/Include" /I"$repo/tools/ntddk/inc" /I"$PSScriptRoot" /Feumtest.exe "$PSScriptRoot/umtest.c" "$PSScriptRoot/umpkt.c" "$PSScriptRoot/msdesc.c" /link /libpath:"$repo/tools/MSVC600/VC98/Lib" kernel32.lib
        if ($LASTEXITCODE) { throw 'MIDI host-test build failed' }
        & ./umtest.exe
        if ($LASTEXITCODE) { throw 'MIDI host tests failed' }
        & $compiler /nologo /W3 /WX /O2 /DWIN32_LEAN_AND_MEAN /I"$repo/tools/MSVC600/VC98/Include" /I"$repo/tools/ntddk/inc" /I"$PSScriptRoot" /Feumrtst.exe "$PSScriptRoot/umrtst.c" "$PSScriptRoot/umroland.c" "$PSScriptRoot/msdesc.c" /link /libpath:"$repo/tools/MSVC600/VC98/Lib" kernel32.lib
        if ($LASTEXITCODE) { throw 'Roland profile test build failed' }
        & ./umrtst.exe "$PSScriptRoot/testdata/umone2/DEVICE.BIN" "$PSScriptRoot/testdata/umone2/CONFIG.BIN" "$PSScriptRoot/testdata/umtab/DEVICE.BIN" "$PSScriptRoot/testdata/umtab/CONFIG.BIN"
        if ($LASTEXITCODE) { throw 'Roland profile tests failed' }
    } finally { Pop-Location }
} finally { $env:PATH = $saved }

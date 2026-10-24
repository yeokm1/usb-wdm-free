@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\serial\usbcom\probe
if not exist "%VC%\BIN\CL.EXE" (
    echo MISSING: MSVC 6.0 in tools\MSVC600
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\BIN;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W3 /WX /O1 /MD /Fe"%OUT%\CDCPROBE.EXE" "%~dp0cdcprobe.c" /link /SUBSYSTEM:WINDOWS,4.0 setupapi.lib user32.lib gdi32.lib advapi32.lib > build.log 2>&1
if errorlevel 1 (
    type build.log
    popd
    exit /b 1
)
popd
echo PASS: %OUT%\CDCPROBE.EXE
exit /b 0

@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem comtest.cmd - builds COMTEST.EXE, the Win32 guest test tool of the 98 SE /
rem ME legs (comtest.c), with the repository's MSVC 6.0: a console program for
rem Windows 4.0 with the C runtime linked in. A test tool, never released.
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\serial\usbcom\comtest
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
cl /nologo /W3 /WX /O1 /MT /Fe"%OUT%\COMTEST.EXE" "%~dp0comtest.c" /link /SUBSYSTEM:CONSOLE,4.0 user32.lib advapi32.lib > build.log 2>&1
if errorlevel 1 (
    type build.log
    popd
    exit /b 1
)
popd
echo PASS: %OUT%\COMTEST.EXE
exit /b 0

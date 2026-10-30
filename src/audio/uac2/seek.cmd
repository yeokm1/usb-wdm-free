@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem Builds UACSEEK.EXE (test only: WinMM and DirectSound seeks, uacseek.c).
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\audio\uac2\seek
if not exist "%VC%\bin\cl.exe" exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W3 /WX /O1 /MT /Fe"%OUT%\UACSEEK.EXE" "%~dp0uacseek.c" /link /SUBSYSTEM:CONSOLE,4.0 winmm.lib dsound.lib user32.lib > build.log 2>&1
if errorlevel 1 (
    type build.log
    popd
    exit /b 1
)
popd
echo PASS: %OUT%\UACSEEK.EXE
exit /b 0

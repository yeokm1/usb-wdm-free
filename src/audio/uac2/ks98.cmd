@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
rem ks98.cmd: builds the test-only KS98.EXE console probe (9x KS audio stack).
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\audio\uac2\ks98
if not exist "%VC%\bin\cl.exe" exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include;%ROOT%\tools\ntddk\inc
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W3 /WX /MD /O1 /Fe"%OUT%\KS98.EXE" "%~dp0ks98.c" /link /SUBSYSTEM:CONSOLE,4.00 kernel32.lib user32.lib advapi32.lib setupapi.lib winmm.lib > build.log 2>&1
set RESULT=%ERRORLEVEL%
type build.log
popd
if "%RESULT%"=="0" echo PASS: %OUT%\KS98.EXE
exit /b %RESULT%

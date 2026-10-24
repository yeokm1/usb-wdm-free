@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
set SRC=%~dp0
for %%I in ("%SRC%..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\audio\uac2\probe
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
"%ROOT%\tools\ntddk\bin\rc.exe" /I"%SRC%." /fo"%OUT%\probe.res" "%SRC%probe.rc" > build.log 2>&1
if errorlevel 1 goto failed
cl /nologo /W3 /WX /MD /O1 /Fe"%OUT%\UACPROBE.EXE" "%SRC%uacprobe.c" /link /SUBSYSTEM:WINDOWS,4.00 "%OUT%\probe.res" kernel32.lib user32.lib gdi32.lib advapi32.lib setupapi.lib winmm.lib >> build.log 2>&1
set RESULT=%ERRORLEVEL%
if not "%RESULT%"=="0" type build.log
popd
exit /b %RESULT%
:failed
type build.log
popd
exit /b 1

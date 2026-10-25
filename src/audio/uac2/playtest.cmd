@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem Host only: actual player included with mocked audio/files/events/clocks.
setlocal
set SRC=%~dp0
for %%I in ("%SRC%..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=E:\usb-wdm-free\wave2\uac-playtest
if not "%~1"=="" set OUT=%~f1
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
if exist PASS.TXT del PASS.TXT
call :variant legacy ""
if errorlevel 1 goto failed
call :variant strict /DUACPLAY_WALLGATE
if errorlevel 1 goto failed
call :variant phases /DUACPLAY_PHASES
if errorlevel 1 goto failed
call :variant soak /DUACPLAY_SOAK
if errorlevel 1 goto failed
echo PASS: all actual-player host variants>PASS.TXT
type PASS.TXT
popd
exit /b 0
:variant
cl /nologo /W3 /WX /MD /O1 %~2 /Fe%1.exe "%SRC%playtest.c" user32.lib gdi32.lib > %1-build.log 2>&1
if errorlevel 1 exit /b 1
dumpbin /imports %1.exe > %1-imports.txt
if errorlevel 1 exit /b 1
findstr /i "WINMM.DLL" %1-imports.txt > nul
if not errorlevel 1 exit /b 1
%1.exe > %1-test.log 2>&1
if errorlevel 1 exit /b 1
type %1-test.log
exit /b 0
:failed
type *-build.log
type *-test.log
popd
exit /b 1

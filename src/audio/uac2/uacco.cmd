@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem uacco.cmd: builds UACCI1.DLL, uac2's MEDIA class co-installer
rem (docs/design/uac2.md section 16.4), x86 with MSVC 6.0 and x64 with the
rem WDK 7.1 cross compiler, into out\audio\uac2\uacco\<x86|x64>\; with
rem "test" also the UACCO_TEST builds (failure injection) into <x86t|x64t>.
rem A release build also leaves record.txt (its commit and tree state) and
rem PASS.TXT for release\release.ps1; a test build leaves neither, so no
rem release takes it.
rem Usage: uacco.cmd [test]
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set SRC=%~dp0
set HEAD=unknown
for /f %%H in ('git -C "%ROOT%" rev-parse HEAD 2^>nul') do set HEAD=%%H
set DIRTY=clean
for /f %%D in ('git -C "%ROOT%" status --porcelain 2^>nul') do set DIRTY=dirty
set O=%ROOT%\out\audio\uac2\uacco
set VC=%ROOT%\tools\MSVC600\VC98
set W=%ROOT%\tools\WinDDK71
if not exist "%VC%\bin\cl.exe" (echo MISSING: MSVC 6.0 in tools\MSVC600 & exit /b 1)
if not exist "%W%\bin\x86\amd64\cl.exe" (echo MISSING: WDK 7.1 in tools\WinDDK71 & exit /b 1)
set CL=
set _CL_=
set LINK=
set _LINK_=
set FLAGS=/nologo /W3 /WX /DWIN32_LEAN_AND_MEAN /O1 /LD
set LIBS=kernel32.lib user32.lib advapi32.lib setupapi.lib
call :x86 x86 "" || exit /b 1
call :x64 x64 "" || exit /b 1
if /i "%~1"=="test" (
    call :x86 x86t /DUACCO_TEST || exit /b 1
    call :x64 x64t /DUACCO_TEST || exit /b 1
)
echo PASS: %O%
exit /b 0

:x86
setlocal
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
if not exist "%O%\%1" mkdir "%O%\%1"
pushd "%O%\%1" || exit /b 1
if exist UACCI1.DLL del UACCI1.DLL
if exist PASS.TXT del PASS.TXT
if exist record.txt del record.txt
cl %FLAGS% %~2 /Fe"UACCI1.DLL" "%SRC%uacco.c" /link /DEF:"%SRC%uacco.def" /SUBSYSTEM:WINDOWS,4.00 %LIBS% > build.log 2>&1
set R=%ERRORLEVEL%
if not "%R%"=="0" type build.log
if "%R%"=="0" if "%~2"=="" call :record %1 "MSVC 6.0 (cl 12.00)" || set R=1
popd
exit /b %R%

:x64
setlocal
set PATH=%W%\bin\x86\amd64;%W%\bin\x86;%SystemRoot%\system32
set INCLUDE=%W%\inc\api;%W%\inc\crt
set LIB=%W%\lib\wnet\amd64;%W%\lib\Crt\amd64
if not exist "%O%\%1" mkdir "%O%\%1"
pushd "%O%\%1" || exit /b 1
if exist UACCI1.DLL del UACCI1.DLL
if exist PASS.TXT del PASS.TXT
if exist record.txt del record.txt
cl %FLAGS% /GS- %~2 /Fe"UACCI1.DLL" "%SRC%uacco.c" /link /DEF:"%SRC%uacco.def" msvcrt.lib /NODEFAULTLIB:LIBCMT /SUBSYSTEM:WINDOWS,5.02 %LIBS% > build.log 2>&1
set R=%ERRORLEVEL%
if not "%R%"=="0" type build.log
if "%R%"=="0" if "%~2"=="" call :record %1 "WDK 7.1 (amd64 cross compiler)" || set R=1
popd
exit /b %R%

:record
> record.txt echo package  audio\uac2\UACCI1.DLL
>> record.txt echo profile  uacco %~1
>> record.txt echo kit      %~2
>> record.txt echo date     %DATE% %TIME%
>> record.txt echo commit   %HEAD%
>> record.txt echo tree     %DIRTY%
> PASS.TXT echo PASS UACCI1.DLL %~1
exit /b 0

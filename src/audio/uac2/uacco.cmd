@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem uacco.cmd: builds UACCI1.DLL, uac2's MEDIA class co-installer
rem (docs/design/uac2.md section 16.4), x86 with MSVC 6.0 and x64 with the
rem WDK 7.1 cross compiler, into out\audio\uac2\uacco\<x86|x64>\; with
rem "test" also the UACCO_TEST builds (failure injection) into <x86t|x64t>.
rem Usage: uacco.cmd [test]
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set SRC=%~dp0
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
cl %FLAGS% %~2 /Fe"UACCI1.DLL" "%SRC%uacco.c" /link /DEF:"%SRC%uacco.def" /SUBSYSTEM:WINDOWS,4.00 %LIBS% > build.log 2>&1
set R=%ERRORLEVEL%
if not "%R%"=="0" type build.log
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
cl %FLAGS% /GS- %~2 /Fe"UACCI1.DLL" "%SRC%uacco.c" /link /DEF:"%SRC%uacco.def" msvcrt.lib /NODEFAULTLIB:LIBCMT /SUBSYSTEM:WINDOWS,5.02 %LIBS% > build.log 2>&1
set R=%ERRORLEVEL%
if not "%R%"=="0" type build.log
popd
exit /b %R%

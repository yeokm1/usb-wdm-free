@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\video\uvc98\host
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
cl /nologo /W4 /WX /MD /Od /FePAYTEST.EXE "%~dp0uvcpay.c" "%~dp0paytest.c" > build.log 2>&1
if errorlevel 1 goto failed
.\PAYTEST.EXE > test.log 2>&1
if errorlevel 1 goto failed
type test.log
popd
exit /b 0
:failed
type build.log
if exist test.log type test.log
popd
exit /b 1

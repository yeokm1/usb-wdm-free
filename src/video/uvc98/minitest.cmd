@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
set SRC=%~dp0
for %%I in ("%SRC%..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\video\uvc98\minitest
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%ROOT%\tools\ntddk\inc\ddk\wdm;%ROOT%\tools\ntddk\inc;%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W3 /WX /MD /O1 /D_X86_=1 /FeMINITEST.EXE "%SRC%minitest.c" > build.log 2>&1
if errorlevel 1 goto failed
.\MINITEST.EXE > test.log 2>&1
if errorlevel 1 goto failed
type test.log
popd
exit /b 0
:failed
type build.log
if exist test.log type test.log
popd
exit /b 1

@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem Direct host tests of the original stream header, not a kernel/USB test.
setlocal
set SRC=%~dp0
for %%I in ("%SRC%..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\audio\uac2\streamtest
if not exist "%VC%\bin\cl.exe" exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%SRC%..\common;%ROOT%\tools\ntddk\inc\ddk;%ROOT%\tools\ntddk\inc;%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
if exist test.log del test.log
cl /nologo /W3 /WX /MD /O1 /D_X86_=1 /DPOOL_TAGGING=1 /FeSTREAMTEST.EXE "%SRC%streamtest.cpp" > build.log 2>&1
if errorlevel 1 goto failed
.\STREAMTEST.EXE > test.log 2>&1
if errorlevel 1 goto failed
type test.log
popd
exit /b 0
:failed
type build.log
if exist test.log type test.log
popd
exit /b 1

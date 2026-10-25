@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\video\uvc98\flow
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\BIN;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W4 /WX /O1 /MT /FeFLOWTEST.EXE "%~dp0flowtest.c" "%~dp0uvcflow.c" "%~dp0uvcown.c" > build.log 2>&1
if errorlevel 1 goto fail
FLOWTEST.EXE
if errorlevel 1 goto fail
popd
exit /b 0
:fail
type build.log
popd
exit /b 1

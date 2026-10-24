@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
set SRC=%~dp0
for %%I in ("%SRC%..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\video\uvc98\install
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W3 /WX /MD /O1 /FeUVCINST.EXE "%SRC%uvcinst.c" /link /SUBSYSTEM:WINDOWS,4.00 kernel32.lib user32.lib setupapi.lib > build.log 2>&1
if errorlevel 1 goto failed
popd
exit /b 0
:failed
type build.log
popd
exit /b 1

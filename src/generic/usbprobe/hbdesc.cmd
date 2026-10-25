@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\generic\usbprobe
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include;%ROOT%\tools\ntddk\inc
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W3 /WX /MT /O2 /FeHUBDESC.EXE "%~dp0hubdesc.c" setupapi.lib user32.lib /link /SUBSYSTEM:WINDOWS,4.0 > hubbuild.log 2>&1
if errorlevel 1 goto failed
echo PASS: %OUT%\HUBDESC.EXE
popd
exit /b 0
:failed
type hubbuild.log
popd
exit /b 1

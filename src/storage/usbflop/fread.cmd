@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\storage\usbflop\fread
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W4 /WX /MD /O1 /FeFREAD.EXE "%~dp0fread.c" user32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS,4.0 > build.log 2>&1
set RESULT=%ERRORLEVEL%
type build.log
popd
exit /b %RESULT%

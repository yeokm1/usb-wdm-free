@echo off
rem SPDX-License-Identifier: GPL-2.0-only
setlocal
set SRC=%~dp0
for %%I in ("%SRC%..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\generic\usbprobe\pnp
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W3 /WX /O1 /MT /FePNPSTAT.EXE "%SRC%pnpstat.c" /link /SUBSYSTEM:WINDOWS,4.00 setupapi.lib user32.lib kernel32.lib > build.log 2>&1
set RESULT=%ERRORLEVEL%
if not "%RESULT%"=="0" type build.log
popd
exit /b %RESULT%

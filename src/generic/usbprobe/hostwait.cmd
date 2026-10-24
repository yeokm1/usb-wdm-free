@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
rem Build only. Never automatically runs the timer-resolution experiment.
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=E:\usb-wdm-free\wave2\host-waits
if not "%~1"=="" set OUT=%~f1
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W3 /WX /MD /O1 /FeHOSTWAIT.EXE "%~dp0hostwait.c" user32.lib winmm.lib /link /subsystem:windows,4.0 > build.log 2>&1
if errorlevel 1 goto failed
echo Built HOSTWAIT.EXE only; no wait or timeBeginPeriod experiment executed.
popd
exit /b 0
:failed
type build.log
popd
exit /b 1

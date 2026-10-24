@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
rem Original Win32 idle-clock probe; no driver, USB or VM is opened.
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=E:\usb-wdm-free\wave2\timer-probe
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
cl /nologo /W4 /WX /MD /O1 /FeTIMERTST.EXE "%~dp0timertst.c" user32.lib /link /subsystem:windows,4.0 > build.log 2>&1
if errorlevel 1 goto fail
echo Built TIMERTST.EXE; runtime diagnostic not yet run.
popd
exit /b 0
:fail
type build.log
popd
exit /b 1

@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem Build only; never attaches a process automatically.
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=E:\usb-wdm-free\wave2\host-policy
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
cl /nologo /W3 /WX /MD /O1 /FeHOSTPOL.EXE "%~dp0hostpolicy.c" > build.log 2>&1
if errorlevel 1 goto failed
echo Built HOSTPOL.EXE only; no target process attached.
popd
exit /b 0
:failed
type build.log
popd
exit /b 1

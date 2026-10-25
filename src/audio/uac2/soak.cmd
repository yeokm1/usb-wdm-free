@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem Separate30-second WinMM diagnostic, NEVER replaces the2-second baseline.
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=E:\usb-wdm-free\wave2\uac-soak
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
cl /nologo /W3 /WX /MD /O1 /DUACPLAY_SOAK /FeUACSOAK.EXE "%~dp0uacplay.c" user32.lib gdi32.lib winmm.lib /link /subsystem:windows,4.0 > build.log 2>&1
if errorlevel 1 goto fail
echo Built separate UACSOAK.EXE; no runtime result.
popd
exit /b 0
:fail
type build.log
popd
exit /b 1

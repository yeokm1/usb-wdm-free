@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=E:\usb-wdm-free\wave2\host-policy-test
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
if exist PASS.TXT del PASS.TXT
cl /nologo /W3 /WX /MD /O1 /FePOLTEST.EXE "%~dp0poltest.c" > build.log 2>&1
if errorlevel 1 goto failed
POLTEST.EXE > test.log 2>&1
if errorlevel 1 goto failed
findstr /b "PASS:" test.log
if errorlevel 1 goto failed
echo PASS: mocked actual policy core>PASS.TXT
popd
exit /b 0
:failed
type build.log
if exist test.log type test.log
popd
exit /b 1

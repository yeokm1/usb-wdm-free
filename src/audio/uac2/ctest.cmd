@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\audio\uac2\control
if not exist "%VC%\bin\cl.exe" exit /b 1
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
rem Keep the assertion/fixture harness unoptimized; exercise both core modes.
cl /nologo /c /W4 /WX /MD /Od /Foctltest.obj "%~dp0ctltest.c" > build.log 2>&1
if errorlevel 1 goto fail
for %%O in (Od O2) do (
    cl /nologo /c /W4 /WX /MD /%%O /Fouacctl.obj "%~dp0uacctl.c" >> build.log 2>&1
    if errorlevel 1 goto fail
    link /nologo /OUT:ctl%%O.exe ctltest.obj uacctl.obj >> build.log 2>&1
    if errorlevel 1 goto fail
    ctl%%O.exe > test%%O.log 2>&1
    if errorlevel 1 goto fail
    type test%%O.log
)
echo PASS portable UAC2 control core; no driver/runtime claim>PASS.TXT
popd
exit /b 0
:fail
type build.log
if exist testOd.log type testOd.log
if exist testO2.log type testO2.log
popd
exit /b 1

@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
set SRC=%~dp0
for %%I in ("%SRC%..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\audio\uac2\host
if not exist "%VC%\bin\cl.exe" (
    echo MISSING: MSVC 6.0 in tools\MSVC600
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\PASS.TXT" del "%OUT%\PASS.TXT"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
type nul > test.log
cl /nologo /W4 /WX /MD /Od /Zi /Fe"%OUT%\desctest.exe" "%SRC%uacdesc.c" "%SRC%uacplan.c" "%SRC%desctest.c" > build.log 2>&1
if errorlevel 1 goto fail
desctest.exe "%SRC%testdata\sbx4\config0.bin" "%SRC%testdata\sbx4\xpconf.bin" > test.log 2>&1
if errorlevel 1 goto fail
type test.log
cl /nologo /W4 /WX /MD /Od /Zi /Fe"%OUT%\watchtest.exe" "%SRC%watchtest.c" > build.log 2>&1
if errorlevel 1 goto fail
watchtest.exe > test.log 2>&1
if errorlevel 1 goto fail
type test.log
cl /nologo /W4 /WX /MD /Od /Zi /Fe"%OUT%\topotest.exe" "%SRC%uactopo.c" "%SRC%topotest.c" > build.log 2>&1
if errorlevel 1 goto fail
topotest.exe > test.log 2>&1
if errorlevel 1 goto fail
type test.log
echo PASS uac2 host > PASS.TXT
popd
exit /b 0
:fail
type build.log
if exist test.log type test.log
popd
exit /b 1

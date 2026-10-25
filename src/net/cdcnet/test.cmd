@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
setlocal
set SRC=%~dp0
for %%I in ("%SRC%..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\net\cdcnet\host
if not exist "%VC%\bin\cl.exe" (
    echo MISSING: MSVC 6.0 in tools\MSVC600
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\PASS.TXT" del "%OUT%\PASS.TXT"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%ROOT%\tools\ntddk\inc;%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
type nul > test.log
cl /nologo /W4 /WX /MD /Od /Zi /Fe"%OUT%\cdctest.exe" "%SRC%cdcdesc.c" "%SRC%cdcncm.c" "%SRC%cdcrndis.c" "%SRC%cdctest.c" > build.log 2>&1
if errorlevel 1 goto fail
"%OUT%\cdctest.exe" > test.log 2>&1
if errorlevel 1 goto fail
type test.log
echo PASS cdcnet host > PASS.TXT
popd
exit /b 0
:fail
type build.log
type test.log
popd
exit /b 1

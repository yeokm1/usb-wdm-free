@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem uac1 host tests: the USB Audio 1.0 parser and model with the driver
rem core built as the driver builds it (UAC_V1). Writes PASS.TXT last.
setlocal
set SRC=%~dp0
for %%I in ("%SRC%..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\audio\uac1\host
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
set COMMON=%SRC%..\common
pushd "%OUT%"
type nul > test.log
for %%O in (Od O2) do (
    cl /nologo /W4 /WX /MD /%%O /DUAC_V1=1 /I"%SRC%." /I"%COMMON%" /Fe"%OUT%\uac1t%%O.exe" "%SRC%uac1test.c" "%SRC%uac1desc.c" "%SRC%uacfmt.c" "%SRC%uac1model.c" "%COMMON%\uacplan.c" "%COMMON%\uacvol.c" "%COMMON%\uacctl.c" "%COMMON%\uacrate.c" "%COMMON%\uacdesc.c" "%COMMON%\uacconv.c" > build.log 2>&1
    if errorlevel 1 goto fail
    "%OUT%\uac1t%%O.exe" "%SRC%testdata\brio100.bin" "%SRC%testdata\logi0a64.bin" "%SRC%testdata\play2.bin" > test.log 2>&1
    if errorlevel 1 goto fail
    type test.log
)
echo PASS uac1 host > PASS.TXT
popd
exit /b 0
:fail
type build.log
if exist test.log type test.log
popd
exit /b 1

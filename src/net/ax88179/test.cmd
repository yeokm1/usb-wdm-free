@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem
rem test.cmd - host tests of the AX179F pure cores, MSVC 6.0 /W4 /WX:
rem   chiptest   the chip core (axchip.c) and shared helpers (netchip.c)
rem   frametest  the receive/transmit framing (axframe.c)
rem   ctrltest   the descriptor/control planner (axctrl.c)
rem   coretest   netcore.c and netusb.c, unchanged, on the fake backend
rem              of shim\ (ethernet.md 10.3); /W3, as the driver build
rem Output and PASS.TXT go to out\net\ax88179\host\.
setlocal
set SRC=%~dp0
for %%I in ("%SRC%..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\net\ax88179\host
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
type nul > build.log
type nul > test.log
cl /nologo /W4 /WX /MD /Od /Zi /Fe"%OUT%\chiptest.exe" "%SRC%axchip.c" "%SRC%axframe.c" "%SRC%netchip.c" "%SRC%chiptest.c" >> build.log 2>&1
if errorlevel 1 goto fail
cl /nologo /W3 /WX /MD /Od /Zi /DDBG=0 /I"%SRC%shim" /I"%SRC%." /Fe"%OUT%\coretest.exe" "%SRC%netcore.c" "%SRC%netusb.c" "%SRC%netchip.c" "%SRC%shim\fakeos.c" "%SRC%shim\fakewin.c" "%SRC%coretest.c" >> build.log 2>&1
if errorlevel 1 goto fail
cl /nologo /W4 /WX /MD /Od /Fe"%OUT%\frametst.exe" "%SRC%axframe.c" "%SRC%frametest.c" >> build.log 2>&1
if errorlevel 1 goto fail
cl /nologo /W4 /WX /MD /Od /Fe"%OUT%\ctrltst.exe" "%SRC%axctrl.c" "%SRC%ctrltest.c" >> build.log 2>&1
if errorlevel 1 goto fail
"%OUT%\chiptest.exe" "%SRC%." >> test.log 2>&1
if errorlevel 1 goto fail
"%OUT%\coretest.exe" >> test.log 2>&1
if errorlevel 1 goto fail
"%OUT%\frametst.exe" >> test.log 2>&1
if errorlevel 1 goto fail
"%OUT%\ctrltst.exe" "%ROOT%" >> test.log 2>&1
if errorlevel 1 goto fail
type test.log
echo PASS ax88179 host > PASS.TXT
popd
exit /b 0
:fail
type build.log
type test.log
popd
exit /b 1

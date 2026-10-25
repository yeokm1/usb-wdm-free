@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set SRC=%~dp0
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\net\cdcnet\nettest
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%ROOT%\tools\ntddk\inc;%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W3 /WX /MD /O2 /Fe"%OUT%\NETTEST.EXE" "%SRC%nettest.c" ws2_32.lib user32.lib /link /SUBSYSTEM:WINDOWS,4.0 > build.log 2>&1
if errorlevel 1 goto fail
cl /nologo /W3 /WX /MD /O2 /Fe"%OUT%\NETLOAD.EXE" "%SRC%netload.c" ws2_32.lib user32.lib /link /SUBSYSTEM:WINDOWS,4.0 >> build.log 2>&1
if errorlevel 1 goto fail
echo PASS: %OUT%\NETTEST.EXE
echo PASS: %OUT%\NETLOAD.EXE
popd
exit /b 0
:fail
type build.log
popd
exit /b 1

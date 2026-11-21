@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Builds MTPTEST.EXE, the test client of USBMTP.SYS's interface 1.0 and
rem 1.1 for the M1 load, lifetime and standby legs, with the repository's
rem MSVC 6.0 into
rem out\image\usbmtp\test\. Not shipped.
setlocal
set SRC=%~dp0
for %%I in ("%SRC%..\..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
if not exist "%VC%\BIN\CL.EXE" (echo MISSING: %VC%\BIN\CL.EXE - see docs\toolchain.md & exit /b 2)
set OUT=%ROOT%\out\image\usbmtp\test
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W3 /WX /O1 /MT /FeMTPTEST.EXE "%SRC%mtptest.c" /link /SUBSYSTEM:WINDOWS,4.00 user32.lib kernel32.lib > build.log 2>&1
set RESULT=%ERRORLEVEL%
if not "%RESULT%"=="0" type build.log
popd
exit /b %RESULT%

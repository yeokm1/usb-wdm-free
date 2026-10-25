@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem Builds JOYTEST.EXE (the guest reading tool) with MSVC 6.0 into
rem out\hid\xpad98\joytest\. Run it in a guest only.
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set OUT=%ROOT%\out\hid\xpad98\joytest
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
cl /nologo /W3 /WX /O1 /MD /FeJOYTEST.EXE "%~dp0joytest.c" winmm.lib user32.lib /link /SUBSYSTEM:WINDOWS,4.0 > build.log 2>&1
set RESULT=%ERRORLEVEL%
type build.log
popd
if not "%RESULT%"=="0" (echo FAIL: JOYTEST.EXE & exit /b 1)
echo PASS: %OUT%\JOYTEST.EXE
exit /b 0

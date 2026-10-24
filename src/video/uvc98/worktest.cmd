@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set VC=%ROOT%\tools\MSVC600\VC98
set DDK=%ROOT%\tools\ntddk
set OUT=%ROOT%\out\video\uvc98\work
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32
set INCLUDE=%DDK%\inc\ddk\wdm;%DDK%\inc;%VC%\Include
set LIB=%VC%\Lib
set CL=
set _CL_=
set LINK=
set _LINK_=
set FLAGS=/nologo /W3 /WX /Gz /Zp8 /D_X86_=1 /Di386=1 /DSTD_CALL /DCONDITION_HANDLING=1 /DNT_UP=1 /DNT_INST=0 /DWIN32=100 /D_NT1X_=100 /DWINNT=1 /D_WIN32_WINNT=0x0500
pushd "%OUT%"
cl %FLAGS% /c /O1 /Zl /Fouvcwork.obj "%~dp0uvcwork.c" > kernel.log 2>&1
if not "%ERRORLEVEL%"=="0" goto fail
cl %FLAGS% /MD /Od /DUVWORK_TEST /FeWORKTEST.EXE "%~dp0worktest.c" "%~dp0uvcwork.c" "%~dp0uvcport.c" "%~dp0uvcflow.c" "%~dp0uvcown.c" "%~dp0uvcgen.c" kernel32.lib > build.log 2>&1
if not "%ERRORLEVEL%"=="0" goto fail
.\WORKTEST.EXE > test.log 2>&1
if not "%ERRORLEVEL%"=="0" goto fail
type test.log
popd
exit /b 0
:fail
type kernel.log
if exist build.log type build.log
if exist test.log type test.log
popd
exit /b 1

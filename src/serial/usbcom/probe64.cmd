@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
rem Native x64 test helper: NewDev installation must not run through WOW64.
setlocal
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set WDK=%ROOT%\tools\WinDDK71
set OUT=%ROOT%\out\serial\usbcom\probe64
if not exist "%WDK%\bin\x86\amd64\cl.exe" (
    echo MISSING: WDK 7.1 in tools\WinDDK71
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
set PATH=%WDK%\bin\x86\amd64;%WDK%\bin\x86;%SystemRoot%\system32
set INCLUDE=%WDK%\inc\api;%WDK%\inc\crt
set LIB=%WDK%\lib\win7\amd64;%WDK%\lib\Crt\amd64
set CL=
set _CL_=
set LINK=
set _LINK_=
pushd "%OUT%"
rc /I "%~dp0." /fo CDCPROBE.res "%~dp0cdcprobe.rc" > build.log 2>&1
if errorlevel 1 (
    type build.log
    popd
    exit /b 1
)
cl /nologo /W3 /WX /O1 /MT /GS- /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /Fe"%OUT%\CDCPROBE.EXE" "%~dp0cdcprobe.c" CDCPROBE.res /link /MANIFEST:NO /SUBSYSTEM:WINDOWS,6.01 /MACHINE:AMD64 setupapi.lib user32.lib gdi32.lib advapi32.lib >> build.log 2>&1
if errorlevel 1 (
    type build.log
    popd
    exit /b 1
)
popd
echo PASS: %OUT%\CDCPROBE.EXE
exit /b 0

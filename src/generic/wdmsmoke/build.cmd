@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem
rem build.cmd <profile> <free|checked>
rem
rem Builds wdmsmoke for one profile (build.md sections 3 and 4) into
rem out\generic\wdmsmoke\<profile>\<flavour>\. Every path is rooted in
rem this repository; a missing kit is reported, never searched for.
rem PASS.TXT is written last, only when every gate has passed.
rem
rem Copyright (C) 2026 Yeo Kheng Meng

setlocal EnableExtensions
set PKG=wdmsmoke
set CLASS=generic
set SRC=%~dp0
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set TOOLS=%ROOT%\tools

rem The source commit, read while the caller's PATH still finds git.
set HEAD=unknown
for /f %%H in ('git -C "%ROOT%" rev-parse HEAD 2^>nul') do set HEAD=%%H
set DIRTY=clean
for /f %%D in ('git -C "%ROOT%" status --porcelain 2^>nul') do set DIRTY=dirty

set PROFILE=%~1
set FLAVOUR=%~2
if "%PROFILE%"=="" goto usage
if "%FLAVOUR%"=="" goto usage
if /I not "%FLAVOUR%"=="free" if /I not "%FLAVOUR%"=="checked" goto usage
if /I "%PROFILE%"=="wdmx86" goto profile_ok
if /I "%PROFILE%"=="wdm64" goto profile_ok
goto usage

:profile_ok
set OUT=%ROOT%\out\%CLASS%\%PKG%\%PROFILE%\%FLAVOUR%
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" || goto fail
set LOG=%OUT%\build.log
set REC=%OUT%\record.txt

rem A fresh environment: nothing from the caller's compiler setup.
set INCLUDE=
set LIB=
set LIBPATH=
set CL=
set _CL_=
set LINK=
set _LINK_=

if /I "%PROFILE%"=="wdmx86" call :env_wdmx86 || goto fail
if /I "%PROFILE%"=="wdm64" call :env_wdm64 || goto fail

if /I "%FLAVOUR%"=="free" (
    set CFLAV=%CFREE%
    set LFLAV=/RELEASE
) else (
    set CFLAV=%CCHK%
    set LFLAV=/RELEASE /DEBUG /PDB:"%OUT%\%PKG%.pdb"
)

call :record || goto fail

echo === compile >> "%LOG%"
set CMDC=cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\%PKG%.obj" "%SRC%%PKG%.c"
echo %CMDC% >> "%LOG%"
%CMDC% >> "%LOG%" 2>&1 || goto fail

echo === link >> "%LOG%"
set CMDL=link /nologo %LBASE% %LFLAV% /MAP:"%OUT%\%PKG%.map" /OUT:"%OUT%\%PKG%.sys" "%OUT%\%PKG%.obj" %LLIBS%
echo %CMDL% >> "%LOG%"
%CMDL% >> "%LOG%" 2>&1 || goto fail

echo === gate >> "%LOG%"
%DUMPBIN% /nologo /headers "%OUT%\%PKG%.sys" > "%OUT%\headers.txt" 2>&1 || goto fail
%DUMPBIN% /nologo /imports "%OUT%\%PKG%.sys" > "%OUT%\imports.txt" 2>&1 || goto fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%chkimp.ps1" ^
    -Headers "%OUT%\headers.txt" -Imports "%OUT%\imports.txt" ^
    -Allowed "%SRC%imports-%PROFILE%.txt" -Machine %MACHINE% ^
    -Subsystem %SUBSYS% -Report "%OUT%\gate.txt" >> "%LOG%" 2>&1 || goto fail

rem The INF goes out with CRLF line endings, as the target systems expect.
powershell -NoProfile -Command "$t = [IO.File]::ReadAllText('%SRC%%PKG%.inf') -replace \"`r?`n\", \"`r`n\"; [IO.File]::WriteAllText('%OUT%\%PKG%.inf', $t, [Text.Encoding]::ASCII)" || goto fail

echo === hashes >> "%REC%"
powershell -NoProfile -Command "Get-FileHash -Algorithm SHA256 '%OUT%\%PKG%.sys','%OUT%\%PKG%.inf','%SRC%%PKG%.c','%SRC%%PKG%.inf' | ForEach-Object { $_.Hash + '  ' + $_.Path }" >> "%REC%" || goto fail

echo PASS %PKG% %PROFILE% %FLAVOUR% > "%OUT%\PASS.TXT"
echo PASS: %OUT%\%PKG%.sys
endlocal
exit /b 0

:env_wdmx86
rem MSVC 6.0 + Windows 2000 DDK, x86 (build.md section 3, wdmx86).
set VC=%TOOLS%\MSVC600\VC98
set DDK=%TOOLS%\ntddk
call :need "%VC%\BIN\CL.EXE" "MSVC 6.0 (tools\MSVC600)" || exit /b 1
call :need "%TOOLS%\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL" "MSVC 6.0 (tools\MSVC600)" || exit /b 1
call :need "%DDK%\inc\wdm.h" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\libfre\i386\wdm.lib" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
set PATH=%VC%\BIN;%TOOLS%\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32;%SystemRoot%\System32\WindowsPowerShell\v1.0
set DUMPBIN=dumpbin
rem The DDK headers first; VC98\Include only supplies excpt.h and the
rem other compiler headers wdm.h includes.
set CINC=/I"%DDK%\inc\ddk" /I"%DDK%\inc" /I"%VC%\Include"
set CBASE=/W3 /WX /Gz /Gy /GF /Zp8 /Zl /D_X86_=1 /Di386=1 /DSTD_CALL /DCONDITION_HANDLING=1 /DNT_UP=1 /DNT_INST=0 /DWIN32=100 /D_NT1X_=100 /DWINNT=1 /D_WIN32_WINNT=0x0500 /DWIN32_LEAN_AND_MEAN=1 /DDEVL=1
set CFREE=/Oxs /DDBG=0
set CCHK=/Od /Oi /Zi /Fd"%OUT%\%PKG%.cpdb" /DDBG=1
set LBASE=/DRIVER /SUBSYSTEM:NATIVE,4.00 /ENTRY:DriverEntry@8 /BASE:0x10000 /ALIGN:0x20 /NODEFAULTLIB /INCREMENTAL:NO /OPT:REF /OPT:ICF /MERGE:_PAGE=PAGE /MERGE:_TEXT=.text /MACHINE:IX86
set LLIBS="%DDK%\libfre\i386\wdm.lib"
set MACHINE=14C
set SUBSYS=4.00
set KITVER=MSVC 6.0 (cl 12.00.8804, link 6.00.8447) + Windows 2000 DDK
exit /b 0

:env_wdm64
rem WDK 7.1, WNET amd64 (build.md section 3, wdm64). XP x64 minimum.
set WDK=%TOOLS%\WinDDK71
call :need "%WDK%\bin\x86\amd64\cl.exe" "WDK 7.1 (tools\WinDDK71)" || exit /b 1
call :need "%WDK%\inc\ddk\wdm.h" "WDK 7.1 (tools\WinDDK71)" || exit /b 1
call :need "%WDK%\lib\wnet\amd64\ntoskrnl.lib" "WDK 7.1 (tools\WinDDK71)" || exit /b 1
set PATH=%WDK%\bin\x86\amd64;%WDK%\bin\x86;%SystemRoot%\system32;%SystemRoot%\System32\WindowsPowerShell\v1.0
set DUMPBIN=link /dump
set CINC=/I"%WDK%\inc\ddk" /I"%WDK%\inc\api" /I"%WDK%\inc\crt"
set CBASE=/W3 /WX /GS- /Gy /GF /Zl /D_AMD64_=1 /DAMD64=1 /D_WIN64=1 /DNTDDI_VERSION=0x05020000 /D_WIN32_WINNT=0x0502 /DWINVER=0x0502 /DWIN32_LEAN_AND_MEAN=1
set CFREE=/Oxs /DDBG=0
set CCHK=/Od /Oi /Zi /Fd"%OUT%\%PKG%.cpdb" /DDBG=1
set LBASE=/DRIVER /SUBSYSTEM:NATIVE,5.02 /ENTRY:DriverEntry /BASE:0x10000 /NODEFAULTLIB /INCREMENTAL:NO /OPT:REF /OPT:ICF /MERGE:_PAGE=PAGE /MERGE:_TEXT=.text /MACHINE:AMD64
set LLIBS="%WDK%\lib\wnet\amd64\ntoskrnl.lib"
set MACHINE=8664
set SUBSYS=5.02
set KITVER=WDK 7.1 7600.16385.1 (cl 15.00.30729.207, link 9.00.30729.207), WNET amd64
exit /b 0

:need
if exist "%~1" exit /b 0
echo MISSING: %~1 - install %~2 as docs\toolchain.md describes
exit /b 1

:record
> "%REC%" echo package  %CLASS%\%PKG%
>> "%REC%" echo profile  %PROFILE% %FLAVOUR%
>> "%REC%" echo kit      %KITVER%
>> "%REC%" echo date     %DATE% %TIME%
>> "%REC%" echo commit   %HEAD%
>> "%REC%" echo tree     %DIRTY%
>> "%REC%" echo cflags   %CBASE% %CFLAV%
>> "%REC%" echo include  %CINC%
>> "%REC%" echo lflags   %LBASE% %LFLAV%
>> "%REC%" echo libs     %LLIBS%
exit /b 0

:usage
echo usage: build.cmd ^<wdmx86^|wdm64^> ^<free^|checked^>
endlocal
exit /b 2

:fail
echo FAILED: %PKG% %PROFILE% %FLAVOUR% - see %LOG%
if exist "%LOG%" type "%LOG%"
endlocal
exit /b 1

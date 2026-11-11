@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem
rem build.cmd <profile> <free|checked>
rem
rem Builds uac1 (USB Audio 1.0, docs/design/uac2.md section 26) for one profile (build.md sections 3 and 4) into
rem out\audio\uac1\<profile>\<flavour>\. Every path is rooted in
rem this repository; a missing kit is reported, never searched for.
rem PASS.TXT is written last, only when every gate has passed.
rem
rem Copyright (C) 2026 Yeo Kheng Meng

setlocal EnableExtensions
set PKG=uac1wdm
set CLASS=audio
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
if not "%~3"=="" goto usage
if /I not "%FLAVOUR%"=="free" if /I not "%FLAVOUR%"=="checked" goto usage
if /I "%PROFILE%"=="wdmx86" goto profile_ok
goto usage

:profile_ok
set OUT=%ROOT%\out\%CLASS%\uac1\%PROFILE%\%FLAVOUR%
if not exist "%OUT%" mkdir "%OUT%" || goto fail
if exist "%OUT%\PASS.TXT" del "%OUT%\PASS.TXT"
set LOG=%OUT%\build.log
set REC=%OUT%\record.txt
type nul > "%LOG%"

rem A fresh environment: nothing from the caller's compiler setup.
set INCLUDE=
set LIB=
set LIBPATH=
set CL=
set _CL_=
set LINK=
set _LINK_=

if /I "%PROFILE%"=="wdmx86" call :env_wdmx86 || goto fail

if /I "%FLAVOUR%"=="free" (
    set CFLAV=%CFREE%
    set LFLAV=/RELEASE
) else (
    set CFLAV=%CCHK%
    set LFLAV=/RELEASE /DEBUG /PDB:"%OUT%\%PKG%.pdb"
)

call :record || goto fail

echo === compile >> "%LOG%"
set CMDC=cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\%PKG%.obj" "%SRC%..\common\uacwdm.cpp"
echo %CMDC% >> "%LOG%"
%CMDC% >> "%LOG%" 2>&1 || goto fail

cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\uacdesc.obj" "%SRC%..\common\uacdesc.c" >> "%LOG%" 2>&1 || goto fail
cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\uacrate.obj" "%SRC%..\common\uacrate.c" >> "%LOG%" 2>&1 || goto fail
cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\uacusb.obj" "%SRC%..\common\uacusb.c" >> "%LOG%" 2>&1 || goto fail
cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\uactopo.obj" "%SRC%..\common\uactopo.c" >> "%LOG%" 2>&1 || goto fail
cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\uacplan.obj" "%SRC%..\common\uacplan.c" >> "%LOG%" 2>&1 || goto fail
cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\uacvol.obj" "%SRC%..\common\uacvol.c" >> "%LOG%" 2>&1 || goto fail
cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\uacctl.obj" "%SRC%..\common\uacctl.c" >> "%LOG%" 2>&1 || goto fail
cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\uac1desc.obj" "%SRC%uac1desc.c" >> "%LOG%" 2>&1 || goto fail
cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\uacfmt.obj" "%SRC%uacfmt.c" >> "%LOG%" 2>&1 || goto fail
cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\uac1model.obj" "%SRC%uac1model.c" >> "%LOG%" 2>&1 || goto fail
cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\uacconv.obj" "%SRC%..\common\uacconv.c" >> "%LOG%" 2>&1 || goto fail

"%RC%" /fo"%OUT%\uac1wdm.res" "%SRC%uac1wdm.rc" >> "%LOG%" 2>&1 || goto fail

echo === link >> "%LOG%"
set CMDL=link /nologo %LBASE% %LFLAV% /MAP:"%OUT%\%PKG%.map" /OUT:"%OUT%\%PKG%.sys" "%OUT%\%PKG%.obj" "%OUT%\uacdesc.obj" "%OUT%\uacrate.obj" "%OUT%\uacusb.obj" "%OUT%\uactopo.obj" "%OUT%\uacplan.obj" "%OUT%\uacvol.obj" "%OUT%\uacctl.obj" "%OUT%\uac1desc.obj" "%OUT%\uacfmt.obj" "%OUT%\uac1model.obj" "%OUT%\uacconv.obj" "%OUT%\uac1wdm.res" %LLIBS%
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
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%record.ps1" -Output "%OUT%" >> "%REC%" || goto fail

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
call :need "%DDK%\inc\portcls.h" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\libfre\i386\portcls.lib" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\bin\rc.exe" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\bin\rcdll.dll" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
set PATH=%VC%\BIN;%TOOLS%\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32;%SystemRoot%\System32\WindowsPowerShell\v1.0
set DUMPBIN=dumpbin
set RC=%DDK%\bin\rc.exe
rem The DDK headers first; VC98\Include only supplies excpt.h and the
rem other compiler headers wdm.h includes.
set CINC=/I"%SRC%." /I"%SRC%..\common" /I"%DDK%\inc\ddk" /I"%DDK%\inc" /I"%VC%\Include"
set CBASE=/DUAC_V1=1 /DUAC1_BUILD=5 /GR- /W3 /WX /Gz /Gy /GF /Zp8 /Zl /D_X86_=1 /Di386=1 /DSTD_CALL /DCONDITION_HANDLING=1 /DNT_UP=1 /DNT_INST=0 /DWIN32=100 /D_NT1X_=100 /DWINNT=1 /D_WIN32_WINNT=0x0500 /DWIN32_LEAN_AND_MEAN=1 /DDEVL=1
set CFREE=/Oxs /DDBG=0
set CCHK=/Od /Oi /Zi /Fd"%OUT%\%PKG%.cpdb" /DDBG=1
set LBASE=/DRIVER /SUBSYSTEM:NATIVE,4.00 /ENTRY:DriverEntry@8 /BASE:0x10000 /ALIGN:0x20 /NODEFAULTLIB /INCREMENTAL:NO /OPT:REF /OPT:ICF /MERGE:_PAGE=PAGE /MERGE:_TEXT=.text /MACHINE:IX86
set LLIBS="%DDK%\libfre\i386\wdm.lib" "%DDK%\libfre\i386\portcls.lib"
set MACHINE=14C
set SUBSYS=4.00
set KITVER=MSVC 6.0 (cl 12.00.8804, link 6.00.8447) + Windows 2000 DDK
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
echo usage: build.cmd wdmx86 ^<free^|checked^>
endlocal
exit /b 2

:fail
echo FAILED: %PKG% %PROFILE% %FLAVOUR% - see %LOG%
if exist "%LOG%" type "%LOG%"
endlocal
exit /b 1

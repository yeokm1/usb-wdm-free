@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem
rem build.cmd ndis5 <free|checked|trace>
rem
rem Builds the package's NDIS 5 miniport for the ndis5 profile (build.md
rem sections 3 and 4; ethernet.md section 4.2: x86 NDIS 5.0 with a WDM
rem lower edge, MSVC 6.0 + Windows 2000 DDK, for 98 SE, ME and 2000) into
rem out\net\<package directory>\ndis5\<flavour>\. Every path is rooted in
rem this repository; a missing kit is reported, never searched for.
rem PASS.TXT is written last, only when every gate has passed.
rem
rem   trace   a free build that also writes a trace to QEMU's debug
rem           console port 0xE9: test-only, never released.
rem
rem The package-specific lines are the four "set" lines after setlocal;
rem the rest is the same in src\net\ax88179 and src\net\rtl815x.
rem
rem Copyright (C) 2026 Yeo Kheng Meng

setlocal EnableExtensions
set PKG=ax179f
set DIR=ax88179
set MODULES=netcore netusb netchip axchip axframe
set INFS=AX179F.INF AX179F98.INF
set CLASS=net
set SRC=%~dp0
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set TOOLS=%ROOT%\tools

set HEAD=unknown
for /f %%H in ('git -C "%ROOT%" rev-parse HEAD 2^>nul') do set HEAD=%%H
set DIRTY=clean
for /f %%D in ('git -C "%ROOT%" status --porcelain -- src/net/%DIR% 2^>nul') do set DIRTY=dirty

set PROFILE=%~1
set FLAVOUR=%~2
if "%PROFILE%"=="" goto usage
if "%FLAVOUR%"=="" goto usage
if not "%~3"=="" goto usage
if /I not "%PROFILE%"=="ndis5" goto usage
if /I not "%FLAVOUR%"=="free" if /I not "%FLAVOUR%"=="checked" if /I not "%FLAVOUR%"=="trace" goto usage

set OUT=%ROOT%\out\%CLASS%\%DIR%\%PROFILE%\%FLAVOUR%
if exist "%OUT%\PASS.TXT" del "%OUT%\PASS.TXT"
if not exist "%OUT%" mkdir "%OUT%" || goto fail
if exist "%OUT%\%PKG%.sys" del "%OUT%\%PKG%.sys"
if exist "%OUT%\*.obj" del "%OUT%\*.obj"
set LOG=%OUT%\build.log
set REC=%OUT%\record.txt
type nul > "%LOG%"

set INCLUDE=
set LIB=
set LIBPATH=
set CL=
set _CL_=
set LINK=
set _LINK_=

call :env_ndis5 || goto fail

if /I "%FLAVOUR%"=="checked" (
    set CFLAV=%CCHK%
    set LFLAV=/RELEASE /DEBUG /PDB:"%OUT%\%PKG%.pdb"
) else (
    set CFLAV=%CFREE%
    set LFLAV=/RELEASE
)
if /I "%FLAVOUR%"=="trace" set CFLAV=%CFREE% /DNET_TRACE_E9=1

call :record || goto fail

echo === compile >> "%LOG%"
set OBJS=
for %%M in (%MODULES%) do call :compile %%M || goto fail

set CMDR="%RC%" /fo"%OUT%\%PKG%.res" "%SRC%%PKG%.rc"
echo %CMDR% >> "%LOG%"
%CMDR% >> "%LOG%" 2>&1 || goto fail

echo === link >> "%LOG%"
set CMDL=link /nologo %LBASE% %LFLAV% /MAP:"%OUT%\%PKG%.map" /OUT:"%OUT%\%PKG%.sys" %OBJS% "%OUT%\%PKG%.res" %LLIBS%
echo %CMDL% >> "%LOG%"
%CMDL% >> "%LOG%" 2>&1 || goto fail

echo === gate >> "%LOG%"
%DUMPBIN% /nologo /headers "%OUT%\%PKG%.sys" > "%OUT%\headers.txt" 2>&1 || goto fail
%DUMPBIN% /nologo /imports "%OUT%\%PKG%.sys" > "%OUT%\imports.txt" 2>&1 || goto fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%chkimp.ps1" ^
    -Headers "%OUT%\headers.txt" -Imports "%OUT%\imports.txt" ^
    -Allowed "%SRC%imports-%PROFILE%.txt" -Machine %MACHINE% ^
    -Subsystem %SUBSYS% -Report "%OUT%\gate.txt" >> "%LOG%" 2>&1 || goto fail

rem INFs go out with CRLF line endings, as the target systems expect.
for %%N in (%INFS%) do call :inf %%N || goto fail

echo === hashes >> "%REC%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%record.ps1" -Output "%OUT%" >> "%REC%" || goto fail

echo PASS %PKG% %PROFILE% %FLAVOUR% > "%OUT%\PASS.TXT"
echo PASS: %OUT%\%PKG%.sys
endlocal
exit /b 0

:compile
set CMDC=cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\%1.obj" "%SRC%%1.c"
echo %CMDC% >> "%LOG%"
%CMDC% >> "%LOG%" 2>&1 || exit /b 1
set OBJS=%OBJS% "%OUT%\%1.obj"
exit /b 0

:inf
if not exist "%SRC%%1" (
    echo MISSING: %SRC%%1 >> "%LOG%"
    exit /b 1
)
powershell -NoProfile -Command "$t = [IO.File]::ReadAllText('%SRC%%1') -replace \"`r?`n\", \"`r`n\"; [IO.File]::WriteAllText('%OUT%\%1', $t, [Text.Encoding]::ASCII)" || exit /b 1
exit /b 0

:env_ndis5
rem MSVC 6.0 + Windows 2000 DDK, x86 NDIS 5.0 (build.md section 3, ndis5).
set VC=%TOOLS%\MSVC600\VC98
set DDK=%TOOLS%\ntddk
call :need "%VC%\BIN\CL.EXE" "MSVC 6.0 (tools\MSVC600)" || exit /b 1
call :need "%TOOLS%\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL" "MSVC 6.0 (tools\MSVC600)" || exit /b 1
call :need "%DDK%\inc\wdm.h" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\inc\ddk\ndis.h" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\libfre\i386\wdm.lib" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\libfre\i386\ndis.lib" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\bin\rc.exe" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
set PATH=%VC%\BIN;%TOOLS%\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32;%SystemRoot%\System32\WindowsPowerShell\v1.0
set DUMPBIN=dumpbin
set RC=%DDK%\bin\rc.exe
set CINC=/I"%DDK%\inc\ddk" /I"%DDK%\inc" /I"%VC%\Include"
set CBASE=/DNDIS_MINIPORT_DRIVER=1 /DNDIS50_MINIPORT=1 /DNDIS_WDM=1 /W3 /WX /Gz /Gy /GF /Zp8 /Zl /D_X86_=1 /Di386=1 /DSTD_CALL /DCONDITION_HANDLING=1 /DNT_UP=1 /DNT_INST=0 /DWIN32=100 /D_NT1X_=100 /DWINNT=1 /D_WIN32_WINNT=0x0500 /DWIN32_LEAN_AND_MEAN=1 /DDEVL=1
set CFREE=/Oxs /DDBG=0
set CCHK=/Od /Oi /Zi /Fd"%OUT%\%PKG%.cpdb" /DDBG=1
set LBASE=/DRIVER /SUBSYSTEM:NATIVE,4.00 /ENTRY:DriverEntry@8 /BASE:0x10000 /ALIGN:0x20 /NODEFAULTLIB /INCREMENTAL:NO /OPT:REF /OPT:ICF /MERGE:_PAGE=PAGE /MERGE:_TEXT=.text /MACHINE:IX86
set LLIBS="%DDK%\libfre\i386\wdm.lib" "%DDK%\libfre\i386\ndis.lib" "%DDK%\libfre\i386\hal.lib"
set MACHINE=14C
set SUBSYS=4.00
set KITVER=MSVC 6.0 (cl 12.00.8804, link 6.00.8447) + Windows 2000 DDK
exit /b 0

:need
if exist "%~1" exit /b 0
echo MISSING: %~1 - install %~2 as docs\toolchain.md describes
exit /b 1

:record
> "%REC%" echo package  %CLASS%\%DIR% (%PKG%.sys)
>> "%REC%" echo profile  %PROFILE% %FLAVOUR%
>> "%REC%" echo kit      %KITVER%
>> "%REC%" echo date     %DATE% %TIME%
>> "%REC%" echo commit   %HEAD%
>> "%REC%" echo tree     %DIRTY% (src\net\%DIR%)
>> "%REC%" echo cflags   %CBASE% %CFLAV%
>> "%REC%" echo include  %CINC%
>> "%REC%" echo lflags   %LBASE% %LFLAV%
>> "%REC%" echo libs     %LLIBS%
exit /b 0

:usage
echo usage: build.cmd ndis5 ^<free^|checked^|trace^>
endlocal
exit /b 2

:fail
echo FAILED: %PKG% %PROFILE% %FLAVOUR% - see %LOG%
if exist "%LOG%" type "%LOG%"
endlocal
exit /b 1

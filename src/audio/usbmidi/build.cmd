@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem
rem build.cmd <wdmx86|wdm64> <free|checked>
rem
rem Builds usbmidi.sys for one profile (docs/design/build.md sections 3-5)
rem into out\audio\usbmidi\<profile>\<flavour>\, with the per-folder INFs
rem generated into its inf\ subfolder. Every path is rooted in this
rem repository; a missing kit is reported, never searched for.
rem Gates: own-PE machine/subsystem/import allowlist; for wdmx86 also the
rem Windows 98 link probe (the same objects linked against the 98 SE DDK's
rem import libraries must resolve). PASS.TXT is written last.
rem
rem Copyright (C) 2026 Yeo Kheng Meng

setlocal EnableExtensions
set PKG=usbmidi
set CLASS=audio
set SRC=%~dp0
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set TOOLS=%ROOT%\tools

set HEAD=unknown
for /f %%H in ('git -C "%ROOT%" rev-parse HEAD 2^>nul') do set HEAD=%%H
set DIRTY=clean
for /f %%D in ('git -C "%ROOT%" status --porcelain -- src/audio/usbmidi 2^>nul') do set DIRTY=dirty

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
set INCLUDE=
set LIB=
set LIBPATH=
set CL=
set _CL_=
set LINK=
set _LINK_=

if /I "%PROFILE%"=="wdmx86" call :env_wdmx86 || goto fail
if /I "%PROFILE%"=="wdm64" call :env_wdm64 || goto fail
rem UMTRACE=1 in the caller's environment adds the debug-port trace (tests only).
if defined UMTRACE set CBASE=%CBASE% /DUMTRACE=1
if /I "%FLAVOUR%"=="free" (
    set CFLAV=%CFREE%
    set LFLAV=/RELEASE
) else (
    set CFLAV=%CCHK%
    set LFLAV=/RELEASE /DEBUG /PDB:"%OUT%\%PKG%.pdb"
)
call :record || goto fail

echo === compile >> "%LOG%"
set CMDC=cl /nologo /c %CBASE% %CFLAV% %CINC% /GR- /Fo"%OUT%\usbmidi.obj" "%SRC%usbmidi.cpp"
echo %CMDC% >> "%LOG%"
%CMDC% >> "%LOG%" 2>&1 || goto fail
for %%S in (umusb umpkt msdesc) do (
    echo cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\%%S.obj" "%SRC%%%S.c" >> "%LOG%"
    cl /nologo /c %CBASE% %CFLAV% %CINC% /Fo"%OUT%\%%S.obj" "%SRC%%%S.c" >> "%LOG%" 2>&1 || goto fail
)
set OBJS="%OUT%\usbmidi.obj" "%OUT%\umusb.obj" "%OUT%\umpkt.obj" "%OUT%\msdesc.obj" "%OUT%\usbmidi.res"
echo %RCEXE% /fo"%OUT%\usbmidi.res" "%SRC%usbmidi.rc" >> "%LOG%"
%RCEXE% /fo"%OUT%\usbmidi.res" "%SRC%usbmidi.rc" >> "%LOG%" 2>&1 || goto fail

echo === link >> "%LOG%"
set CMDL=link /nologo %LBASE% %LFLAV% /MAP:"%OUT%\%PKG%.map" /OUT:"%OUT%\%PKG%.sys" %OBJS% %LLIBS%
echo %CMDL% >> "%LOG%"
%CMDL% >> "%LOG%" 2>&1 || goto fail

echo === gate >> "%LOG%"
%DUMPBIN% /nologo /headers "%OUT%\%PKG%.sys" > "%OUT%\headers.txt" 2>&1 || goto fail
%DUMPBIN% /nologo /imports "%OUT%\%PKG%.sys" > "%OUT%\imports.txt" 2>&1 || goto fail
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%chkimp.ps1" ^
    -Headers "%OUT%\headers.txt" -Imports "%OUT%\imports.txt" ^
    -Allowed "%SRC%imports-%PROFILE%.txt" -Machine %MACHINE% ^
    -Subsystem %SUBSYS% -Report "%OUT%\gate.txt" >> "%LOG%" 2>&1 || goto fail
if /I "%PROFILE%"=="wdmx86" call :probe98 || goto fail

echo === inf >> "%LOG%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%mkinf.ps1" -Template "%SRC%usbmidi.inf" ^
    -Destination "%OUT%\inf" -Profile %PROFILE% >> "%LOG%" 2>&1 || goto fail

echo === hashes >> "%REC%"
powershell -NoProfile -Command "Get-ChildItem -Recurse -File '%OUT%' -Include *.sys,*.inf | Sort-Object FullName | Get-FileHash -Algorithm SHA256 | ForEach-Object { $_.Hash + '  ' + $_.Path.Substring('%OUT%'.Length+1) }" >> "%REC%" || goto fail
powershell -NoProfile -Command "Get-ChildItem -LiteralPath '%SRC%.' -File | Where-Object { $_.Extension -in '.c','.cpp','.h','.inf','.rc','.ps1','.cmd' } | Sort-Object Name | Get-FileHash -Algorithm SHA256 | ForEach-Object { $_.Hash + '  src/' + (Split-Path -Leaf $_.Path) }" >> "%REC%" || goto fail

echo PASS %PKG% %PROFILE% %FLAVOUR% > "%OUT%\PASS.TXT"
echo PASS: %OUT%\%PKG%.sys
endlocal
exit /b 0

:probe98
rem Link the same objects against the Windows 98 SE DDK's import libraries
rem (build.md 5.1: published import-library evidence). Any import those
rem libraries do not name fails the link. The probe binary is discarded.
set W98=%TOOLS%\win98ddk-extracted\all\LIBS_DDK
call :need "%W98%\LIB_I386_FREE_WDM.LIB" "Windows 98 SE DDK (tools\win98ddk-extracted)" || exit /b 1
call :need "%W98%\LIB_I386_FREE_PORTCLS.LIB" "Windows 98 SE DDK (tools\win98ddk-extracted)" || exit /b 1
echo === probe98 >> "%LOG%"
mkdir "%OUT%\probe98" || exit /b 1
set CMDP=link /nologo %LBASE% /RELEASE /OUT:"%OUT%\probe98\probe.sys" %OBJS% "%W98%\LIB_I386_FREE_WDM.LIB" "%W98%\LIB_I386_FREE_PORTCLS.LIB"
echo %CMDP% >> "%LOG%"
%CMDP% > "%OUT%\probe98\link.txt" 2>&1
if errorlevel 1 (
    type "%OUT%\probe98\link.txt" >> "%LOG%"
    echo probe98: FAIL - an import is not in the 98 SE DDK import libraries >> "%LOG%"
    exit /b 1
)
%DUMPBIN% /nologo /imports "%OUT%\probe98\probe.sys" > "%OUT%\probe98\imports.txt" 2>&1 || exit /b 1
del "%OUT%\probe98\probe.sys"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%cmpimp.ps1" -Built "%OUT%\imports.txt" -Probe "%OUT%\probe98\imports.txt" -Report "%OUT%\probe98\compare.txt" >> "%LOG%" 2>&1 || exit /b 1
echo probe98: PASS - every import resolves, same module, in the 98 SE DDK wdm.lib/portcls.lib > "%OUT%\probe98\result.txt"
type "%OUT%\probe98\result.txt" >> "%LOG%"
exit /b 0

:env_wdmx86
set VC=%TOOLS%\MSVC600\VC98
set DDK=%TOOLS%\ntddk
call :need "%VC%\BIN\CL.EXE" "MSVC 6.0 (tools\MSVC600)" || exit /b 1
call :need "%TOOLS%\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL" "MSVC 6.0 (tools\MSVC600)" || exit /b 1
call :need "%DDK%\inc\wdm.h" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\libfre\i386\portcls.lib" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
set PATH=%VC%\BIN;%TOOLS%\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32;%SystemRoot%\System32\WindowsPowerShell\v1.0
set DUMPBIN=dumpbin
set RCEXE=rc
set CINC=/I"%SRC%." /I"%ROOT%\src\audio\common" /I"%DDK%\inc\ddk" /I"%DDK%\inc" /I"%VC%\Include"
set CBASE=/W3 /WX /Gz /Gy /GF /Zp8 /Zl /D_X86_=1 /Di386=1 /DSTD_CALL /DCONDITION_HANDLING=1 /DNT_UP=1 /DNT_INST=0 /DWIN32=100 /D_NT1X_=100 /DWINNT=1 /D_WIN32_WINNT=0x0500 /DWIN32_LEAN_AND_MEAN=1 /DDEVL=1
set CFREE=/Oxs /DDBG=0
set CCHK=/Od /Oi /Zi /Fd"%OUT%\%PKG%.cpdb" /DDBG=1
set LBASE=/DRIVER /SUBSYSTEM:NATIVE,4.00 /ENTRY:DriverEntry@8 /BASE:0x10000 /ALIGN:0x20 /NODEFAULTLIB /INCREMENTAL:NO /OPT:REF /OPT:ICF /MACHINE:IX86
set LLIBS="%DDK%\libfre\i386\wdm.lib" "%DDK%\libfre\i386\portcls.lib"
set MACHINE=14C
set SUBSYS=4.00
set KITVER=MSVC 6.0 (cl 12.00.8804, link 6.00.8447) + Windows 2000 DDK; 98 SE DDK import libraries for the probe
exit /b 0

:env_wdm64
set WDK=%TOOLS%\WinDDK71
call :need "%WDK%\bin\x86\amd64\cl.exe" "WDK 7.1 (tools\WinDDK71)" || exit /b 1
call :need "%WDK%\inc\ddk\portcls.h" "WDK 7.1 (tools\WinDDK71)" || exit /b 1
call :need "%WDK%\lib\wnet\amd64\portcls.lib" "WDK 7.1 (tools\WinDDK71)" || exit /b 1
set PATH=%WDK%\bin\x86\amd64;%WDK%\bin\x86;%SystemRoot%\system32;%SystemRoot%\System32\WindowsPowerShell\v1.0
set DUMPBIN=link /dump
set RCEXE=rc /i"%WDK%\inc\api"
set CINC=/I"%SRC%." /I"%WDK%\inc\ddk" /I"%WDK%\inc\api" /I"%WDK%\inc\crt"
set CBASE=/W3 /WX /GS- /Gy /GF /Zl /D_AMD64_=1 /DAMD64=1 /D_WIN64=1 /DNTDDI_VERSION=0x05020000 /D_WIN32_WINNT=0x0502 /DWINVER=0x0502 /DWIN32_LEAN_AND_MEAN=1
set CFREE=/Oxs /DDBG=0
set CCHK=/Od /Oi /Zi /Fd"%OUT%\%PKG%.cpdb" /DDBG=1
set LBASE=/DRIVER /SUBSYSTEM:NATIVE,5.02 /ENTRY:DriverEntry /BASE:0x10000 /NODEFAULTLIB /INCREMENTAL:NO /OPT:REF /OPT:ICF /MACHINE:AMD64
set LLIBS="%WDK%\lib\wnet\amd64\ntoskrnl.lib" "%WDK%\lib\wnet\amd64\hal.lib" "%WDK%\lib\wnet\amd64\portcls.lib"
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
>> "%REC%" echo tree     %DIRTY% (src/audio/usbmidi)
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

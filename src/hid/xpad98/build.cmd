@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem
rem build.cmd <wdmx86|wdm64> <free|checked|trace>
rem   trace: wdmx86 only, a free build that also writes a trace to QEMU's
rem   debug console port 0xE9: test-only, never released.
rem
rem Builds xpad98.sys for one profile (build.md sections 3 and 4) into
rem out\hid\xpad98\<profile>\<flavour>\, with the three INFs (CRLF) and the
rem PE/import gate (chkimp.ps1). Every path is rooted in this repository;
rem a missing kit is reported, never searched for. PASS.TXT is written
rem last, only when every gate has passed.
rem
rem Copyright (C) 2026 Yeo Kheng Meng

setlocal EnableExtensions EnableDelayedExpansion
set PKG=xpad98
set CLASS=hid
set SRC=%~dp0
for %%I in ("%~dp0..\..\..") do set ROOT=%%~fI
set TOOLS=%ROOT%\tools
set SOURCES=xpad98 usbio xusb gip devtab xpcfg report
set INFS=xp98me xp2k xpxpv

set HEAD=unknown
for /f %%H in ('git -C "%ROOT%" rev-parse HEAD 2^>nul') do set HEAD=%%H
set DIRTY=clean
for /f %%D in ('git -C "%ROOT%" status --porcelain 2^>nul') do set DIRTY=dirty

set PROFILE=%~1
set FLAVOUR=%~2
if "%PROFILE%"=="" goto usage
if "%FLAVOUR%"=="" goto usage
if /I not "%FLAVOUR%"=="free" if /I not "%FLAVOUR%"=="checked" if /I not "%FLAVOUR%"=="trace" goto usage
if /I "%FLAVOUR%"=="trace" if /I not "%PROFILE%"=="wdmx86" goto usage
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

if /I "%FLAVOUR%"=="free" (
    set CFLAV=%CFREE%
    set LFLAV=/RELEASE
) else if /I "%FLAVOUR%"=="trace" (
    set CFLAV=%CFREE% /DXP_TRACE_E9=1
    set LFLAV=/RELEASE
) else (
    set CFLAV=%CCHK%
    set LFLAV=/RELEASE /DEBUG /PDB:"%OUT%\%PKG%.pdb"
)

call :record || goto fail

set OBJS=
for %%S in (%SOURCES%) do (
    echo === compile %%S >> "%LOG%"
    set CMDC=cl /nologo /c %CBASE% !CFLAV! %CINC% /I"%SRC%." /Fo"%OUT%\%%S.obj" "%SRC%%%S.c"
    echo !CMDC! >> "%LOG%"
    !CMDC! >> "%LOG%" 2>&1 || goto fail
    set OBJS=!OBJS! "%OUT%\%%S.obj"
)

echo === resource >> "%LOG%"
set CMDR=rc /l 0x409 %RCINC% /fo"%OUT%\%PKG%.res" "%SRC%%PKG%.rc"
echo %CMDR% >> "%LOG%"
%CMDR% >> "%LOG%" 2>&1 || goto fail

echo === link >> "%LOG%"
set CMDL=link /nologo %LBASE% %LFLAV% /MAP:"%OUT%\%PKG%.map" /OUT:"%OUT%\%PKG%.sys" %OBJS% "%OUT%\%PKG%.res" %LLIBS%
echo %CMDL% >> "%LOG%"
%CMDL% >> "%LOG%" 2>&1 || goto fail

echo === gate >> "%LOG%"
%DUMPBIN% /nologo /headers "%OUT%\%PKG%.sys" > "%OUT%\headers.txt" 2>&1 || goto fail
%DUMPBIN% /nologo /imports "%OUT%\%PKG%.sys" > "%OUT%\imports.txt" 2>&1 || goto fail
set W98ARGS=
if /I "%PROFILE%"=="wdmx86" (
    rem The 98 DDK's own import libraries: what NTKERN, HAL and HIDCLASS
    rem declare for 98 (a declaration, not proof of export; build.md 5.1).
    for %%L in (WDM HAL HIDCLASS) do (
        %DUMPBIN% /nologo /exports "%W98LIB%\LIB_I386_FREE_%%L.LIB" > "%OUT%\w98-%%L.txt" 2>&1 || goto fail
    )
    set W98ARGS=-Win98 "%OUT%\w98-WDM.txt,%OUT%\w98-HAL.txt,%OUT%\w98-HIDCLASS.txt"
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%chkimp.ps1" ^
    -Headers "%OUT%\headers.txt" -Imports "%OUT%\imports.txt" ^
    -Allowed "%SRC%imports-%PROFILE%.txt" -Machine %MACHINE% ^
    -Subsystem %SUBSYS% -Report "%OUT%\gate.txt" !W98ARGS! >> "%LOG%" 2>&1 || goto fail

for %%F in (%INFS%) do (
    powershell -NoProfile -Command "$t = [IO.File]::ReadAllText('%SRC%%%F.inf') -replace \"`r?`n\", \"`r`n\"; [IO.File]::WriteAllText('%OUT%\%%F.inf', $t, [Text.Encoding]::ASCII)" || goto fail
)

echo === hashes >> "%REC%"
powershell -NoProfile -Command "Get-ChildItem '%OUT%\%PKG%.sys','%OUT%\*.inf' | Get-FileHash -Algorithm SHA256 | ForEach-Object { $_.Hash + '  ' + $_.Path }; Get-ChildItem '%SRC%*.c','%SRC%*.h','%SRC%*.inf','%SRC%*.rc' | Get-FileHash -Algorithm SHA256 | ForEach-Object { $_.Hash + '  ' + $_.Path }" >> "%REC%" || goto fail

echo PASS %PKG% %PROFILE% %FLAVOUR% > "%OUT%\PASS.TXT"
echo PASS: %OUT%\%PKG%.sys
endlocal
exit /b 0

:env_wdmx86
rem MSVC 6.0 + Windows 2000 DDK, x86: one binary for 98 SE to Vista x86.
set VC=%TOOLS%\MSVC600\VC98
set DDK=%TOOLS%\ntddk
set W98LIB=%TOOLS%\win98ddk-extracted\all\LIBS_DDK
call :need "%VC%\BIN\CL.EXE" "MSVC 6.0 (tools\MSVC600)" || exit /b 1
call :need "%TOOLS%\MSVC600\Common\MSDev98\Bin\MSPDB60.DLL" "MSVC 6.0 (tools\MSVC600)" || exit /b 1
call :need "%TOOLS%\MSVC600\Common\MSDev98\Bin\RC.EXE" "MSVC 6.0 (tools\MSVC600)" || exit /b 1
call :need "%DDK%\inc\wdm.h" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\inc\hidclass.h" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\libfre\i386\wdm.lib" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%DDK%\libfre\i386\hidclass.lib" "Windows 2000 DDK (tools\ntddk)" || exit /b 1
call :need "%W98LIB%\LIB_I386_FREE_WDM.LIB" "Windows 98 SE DDK (tools\win98ddk-extracted)" || exit /b 1
call :need "%W98LIB%\LIB_I386_FREE_HAL.LIB" "Windows 98 SE DDK (tools\win98ddk-extracted)" || exit /b 1
call :need "%W98LIB%\LIB_I386_FREE_HIDCLASS.LIB" "Windows 98 SE DDK (tools\win98ddk-extracted)" || exit /b 1
set PATH=%VC%\BIN;%TOOLS%\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32;%SystemRoot%\System32\WindowsPowerShell\v1.0
set DUMPBIN=dumpbin
set CINC=/I"%DDK%\inc\ddk\wdm" /I"%DDK%\inc\ddk" /I"%DDK%\inc" /I"%VC%\Include"
set RCINC=/i"%VC%\Include"
set CBASE=/W3 /WX /Gz /Gy /GF /Zp8 /Zl /D_X86_=1 /Di386=1 /DSTD_CALL /DCONDITION_HANDLING=1 /DNT_UP=1 /DNT_INST=0 /DWIN32=100 /D_NT1X_=100 /DWINNT=1 /D_WIN32_WINNT=0x0500 /DWIN32_LEAN_AND_MEAN=1 /DDEVL=1
set CFREE=/Oxs /DDBG=0
set CCHK=/Od /Oi /Zi /Fd"%OUT%\%PKG%.cpdb" /DDBG=1
set LBASE=/DRIVER /SUBSYSTEM:NATIVE,4.00 /ENTRY:DriverEntry@8 /BASE:0x10000 /ALIGN:0x20 /NODEFAULTLIB /INCREMENTAL:NO /OPT:REF /OPT:ICF /MERGE:_PAGE=PAGE /MERGE:_TEXT=.text /MACHINE:IX86
set LLIBS="%DDK%\libfre\i386\wdm.lib" "%DDK%\libfre\i386\hidclass.lib"
set MACHINE=14C
set SUBSYS=4.00
set KITVER=MSVC 6.0 (cl 12.00.8804, link 6.00.8447) + Windows 2000 DDK
exit /b 0

:env_wdm64
rem WDK 7.1, WNET amd64: XP x64 and Vista x64.
set WDK=%TOOLS%\WinDDK71
call :need "%WDK%\bin\x86\amd64\cl.exe" "WDK 7.1 (tools\WinDDK71)" || exit /b 1
call :need "%WDK%\bin\x86\rc.exe" "WDK 7.1 (tools\WinDDK71)" || exit /b 1
call :need "%WDK%\inc\ddk\wdm.h" "WDK 7.1 (tools\WinDDK71)" || exit /b 1
call :need "%WDK%\lib\wnet\amd64\ntoskrnl.lib" "WDK 7.1 (tools\WinDDK71)" || exit /b 1
call :need "%WDK%\lib\wnet\amd64\hidclass.lib" "WDK 7.1 (tools\WinDDK71)" || exit /b 1
set PATH=%WDK%\bin\x86\amd64;%WDK%\bin\x86;%SystemRoot%\system32;%SystemRoot%\System32\WindowsPowerShell\v1.0
set DUMPBIN=link /dump
set CINC=/I"%WDK%\inc\ddk" /I"%WDK%\inc\api" /I"%WDK%\inc\crt"
set RCINC=/i"%WDK%\inc\api"
set CBASE=/W3 /WX /GS- /Gy /GF /Zl /D_AMD64_=1 /DAMD64=1 /D_WIN64=1 /DNTDDI_VERSION=0x05020000 /D_WIN32_WINNT=0x0502 /DWINVER=0x0502 /DWIN32_LEAN_AND_MEAN=1
set CFREE=/Oxs /DDBG=0
set CCHK=/Od /Oi /Zi /Fd"%OUT%\%PKG%.cpdb" /DDBG=1
set LBASE=/DRIVER /SUBSYSTEM:NATIVE,5.02 /ENTRY:DriverEntry /BASE:0x10000 /NODEFAULTLIB /INCREMENTAL:NO /OPT:REF /OPT:ICF /MERGE:_PAGE=PAGE /MERGE:_TEXT=.text /MACHINE:AMD64
set LLIBS="%WDK%\lib\wnet\amd64\ntoskrnl.lib" "%WDK%\lib\wnet\amd64\hal.lib" "%WDK%\lib\wnet\amd64\hidclass.lib"
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
echo usage: build.cmd ^<wdmx86^|wdm64^> ^<free^|checked^|trace^>  (trace: wdmx86 only)
endlocal
exit /b 2

:fail
echo FAILED: %PKG% %PROFILE% %FLAVOUR% - see %LOG%
if exist "%LOG%" type "%LOG%"
endlocal
exit /b 1

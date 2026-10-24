@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
rem Original experimental x86 STREAM.SYS miniport M2 (UVC over USB), not a release package.
setlocal EnableDelayedExpansion
set SRC=%~dp0
for %%I in ("%SRC%..\..\..") do set ROOT=%%~fI
set FLAVOUR=%~1
if "%FLAVOUR%"=="" set FLAVOUR=free
if /I not "%FLAVOUR%"=="free" if /I not "%FLAVOUR%"=="checked" exit /b 2
set VC=%ROOT%\tools\MSVC600\VC98
set DDK=%ROOT%\tools\ntddk
set OUT=%ROOT%\out\video\uvc98\m2x86\%FLAVOUR%
set TRACEFLAGS=
set GATEFLAGS=
if not "%~2"=="" if /I not "%~2"=="vmtrace" exit /b 2
if /I "%~2"=="vmtrace" (
if /I not "%FLAVOUR%"=="checked" exit /b 2
set OUT=%ROOT%\out\video\uvc98\m2trace\%FLAVOUR%
set TRACEFLAGS=/DUVWORK_VM_TRACE=1
set GATEFLAGS=-VMTrace
)
if not exist "%VC%\bin\cl.exe" exit /b 1
if not exist "%DDK%\inc\ddk\wdm\strmini.h" exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\PASS.TXT" del "%OUT%\PASS.TXT"
set PATH=%VC%\bin;%ROOT%\tools\MSVC600\Common\MSDev98\Bin;%SystemRoot%\system32;%SystemRoot%\System32\WindowsPowerShell\v1.0
set INCLUDE=%DDK%\inc\ddk\wdm;%DDK%\inc;%VC%\Include
set LIB=%DDK%\libfre\i386
set CL=
set _CL_=
set LINK=
set _LINK_=
set CFLAGS=/Oxs /DDBG=0 /DUVWORK_USB=1
if /I "%FLAVOUR%"=="checked" set CFLAGS=/Od /Oi /Zi /DDBG=1 /DUVWORK_USB=1
pushd "%OUT%"
cl /nologo /c /W3 /WX /Gz /Gy /GF /Zp8 /Zl /D_X86_=1 /Di386=1 /DSTD_CALL /DCONDITION_HANDLING=1 /DNT_UP=1 /DNT_INST=0 /DWIN32=100 /D_NT1X_=100 /DWINNT=1 /D_WIN32_WINNT=0x0500 /DWIN32_LEAN_AND_MEAN=1 /DDEVL=1 %CFLAGS% %TRACEFLAGS% /Fouvcm1.obj "%SRC%uvcm1.c" > build.log 2>&1
if not "!ERRORLEVEL!"=="0" goto failed
for %%S in (uvcwork uvcport uvcportn uvcflow uvcown uvcgen uvcusb uvcdesc uvcctrl uvcneg uvciso uvcpay uvcfmt) do (
cl /nologo /c /W3 /WX /Gz /Gy /GF /Zp8 /Zl /D_X86_=1 /Di386=1 /DSTD_CALL /DCONDITION_HANDLING=1 /DNT_UP=1 /DNT_INST=0 /DWIN32=100 /D_NT1X_=100 /DWINNT=1 /D_WIN32_WINNT=0x0500 %CFLAGS% %TRACEFLAGS% /Fo%%S.obj "%SRC%%%S.c" >> build.log 2>&1
if not "!ERRORLEVEL!"=="0" goto failed
)
"%DDK%\bin\rc.exe" /I"%VC%\Include" /fouvcm2.res "%SRC%uvcm2.rc" >> build.log 2>&1
if not "!ERRORLEVEL!"=="0" goto failed
link /nologo /DRIVER /SUBSYSTEM:NATIVE,4.00 /ENTRY:DriverEntry@8 /BASE:0x10000 /ALIGN:0x20 /NODEFAULTLIB /INCREMENTAL:NO /OPT:REF /OPT:ICF /MERGE:_PAGE=PAGE /MERGE:_TEXT=.text /MACHINE:IX86 /MAP:uvcm2.map /OUT:uvcm2.sys uvcm1.obj uvcwork.obj uvcport.obj uvcportn.obj uvcflow.obj uvcown.obj uvcgen.obj uvcusb.obj uvcdesc.obj uvcctrl.obj uvcneg.obj uvciso.obj uvcpay.obj uvcfmt.obj uvcm2.res ntoskrnl.lib hal.lib stream.lib >> build.log 2>&1
if not "!ERRORLEVEL!"=="0" goto failed
dumpbin /headers uvcm2.sys > headers.txt
if not "!ERRORLEVEL!"=="0" goto failed
dumpbin /imports uvcm2.sys > imports.txt
if not "!ERRORLEVEL!"=="0" goto failed
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%m2gate.ps1" -Output "%OUT%" -Repository "%ROOT%" %GATEFLAGS% > gate.txt 2>&1
if not "!ERRORLEVEL!"=="0" goto failed
powershell -NoProfile -Command "foreach($n in 'UVCM2.INF','UVCM298.INF'){ $t = [IO.File]::ReadAllText('%SRC%'+$n) -replace \"`r?`n\", \"`r`n\"; [IO.File]::WriteAllText('%OUT%\'+$n, $t, [Text.Encoding]::ASCII) }"
if not "!ERRORLEVEL!"=="0" goto failed
powershell -NoProfile -Command "foreach($f in 'uvcm2.sys','UVCM2.INF','UVCM298.INF'){ $s=[IO.File]::OpenRead([IO.Path]::GetFullPath($f)); try{$h=[Security.Cryptography.SHA256]::Create().ComputeHash($s); ([BitConverter]::ToString($h)-replace '-','')+'  '+$f}finally{$s.Close()} }" > SHA256.TXT
if not "!ERRORLEVEL!"=="0" goto failed
echo PASS experimental compile/link x86 %FLAVOUR%> PASS.TXT
popd
echo PASS experimental M2 STREAM.SYS miniport %FLAVOUR%
exit /b 0
:failed
type build.log
if exist gate.txt type gate.txt
popd
exit /b 1

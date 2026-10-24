@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
rem
rem build.cmd <profile> <free|checked|trace|split>   (build.md section 4; trace, split: 98 only;
rem           test builds that write to QEMU's debug port 0xE9; split also cancels a close's newest bulk IN first)
rem   inf     USBCDC.INF, 2000 to 8 (binds Windows' own usbser.sys)
rem   wdmx86  USBCOM.SYS, 98 SE / ME
rem   vxd9x   USBCOMV.VXD, 98 SE / ME
rem   w98     USBCOM.SYS + USBCOMV.VXD + USBCOM.INF + USBCOMP.INF, with the
rem           host tests and INF gates, staged as one 98 SE / ME folder
setlocal
if "%~3" NEQ "" goto usage
if /I not "%~2"=="free" if /I not "%~2"=="checked" if /I not "%~2"=="trace" if /I not "%~2"=="split" goto usage
set PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe
if /I "%~1"=="inf" (
    "%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Flavour "%~2"
    exit /b %errorlevel%
)
if /I "%~1"=="wdmx86" goto ninety8
if /I "%~1"=="vxd9x" goto ninety8
if /I "%~1"=="w98" goto ninety8
goto usage
:ninety8
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0build98.ps1" -Profile "%~1" -Flavour "%~2"
exit /b %errorlevel%
:usage
echo usage: build.cmd ^<inf^|wdmx86^|vxd9x^|w98^> ^<free^|checked^|trace^|split^>
exit /b 2

@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Copyright (C) 2026 Yeo Kheng Meng
rem
rem build.cmd <profile> <free|checked>   (build.md section 4)
rem   wdmx86  LIBUSB0.SYS, 98 SE / ME
rem   win32   LIBUSB0.DLL, LIBUSB0.LIB, LIBUSB0.A, TESTLIBU.EXE, USB.H, host tests
rem   w98     both, LIBUSB0.INF and every gate, staged as one 98 SE / ME folder
setlocal
if "%~3" NEQ "" goto usage
if /I not "%~2"=="free" if /I not "%~2"=="checked" goto usage
set PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe
if /I "%~1"=="wdmx86" goto run
if /I "%~1"=="win32" goto run
if /I "%~1"=="w98" goto run
goto usage
:run
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Profile "%~1" -Flavour "%~2"
exit /b %errorlevel%
:usage
echo usage: build.cmd ^<wdmx86^|win32^|w98^> ^<free^|checked^>
exit /b 2

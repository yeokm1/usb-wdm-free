@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem
rem build.cmd icmx86 <free|checked> [timing iterations]
rem   UVCDEC.DLL (VfW MJPEG codec, 98 SE / ME / 2000), its gates, UVCDEC.INF
rem   and the host test JDTEST.EXE; output in out\video\uvcdec\icmx86\<flavour>\
setlocal
if /I not "%~1"=="icmx86" goto usage
if /I not "%~2"=="free" if /I not "%~2"=="checked" goto usage
set IT=%~3
if "%IT%"=="" set IT=20
set PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Profile "%~1" -Flavour "%~2" -Iterations %IT%
exit /b %errorlevel%
:usage
echo usage: build.cmd icmx86 ^<free^|checked^> [timing iterations]
exit /b 2

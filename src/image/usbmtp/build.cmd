@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem
rem build.cmd ^<free^|trace^|standbytest^>   usbmtp: Windows 98 SE and ME
rem           (USBMTP.SYS, USBMTP.INF); see build.ps1.
setlocal
if "%~2" NEQ "" goto usage
if /I not "%~1"=="free" if /I not "%~1"=="trace" if /I not "%~1"=="standbytest" goto usage
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Flavour "%~1"
exit /b %errorlevel%
:usage
echo usage: build.cmd ^<free^|trace^|standbytest^>
exit /b 2

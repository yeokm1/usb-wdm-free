@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 Yeo Kheng Meng
rem
rem build.cmd ^<free^|trace^>   usbprn: Windows 98 SE (USBPRN.SYS, USBPRNMN.DLL,
rem           USBPRN.INF) and ME (USBPRNME.INF); see build.ps1.
setlocal
if "%~2" NEQ "" goto usage
if /I not "%~1"=="free" if /I not "%~1"=="trace" goto usage
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Flavour "%~1"
exit /b %errorlevel%
:usage
echo usage: build.cmd ^<free^|trace^>
exit /b 2

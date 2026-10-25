@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
setlocal
if /I not "%~1"=="inf" goto usage
if /I not "%~2"=="free" if /I not "%~2"=="checked" goto usage
if not "%~3"=="" goto usage
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Flavour "%~2"
exit /b %errorlevel%
:usage
echo usage: build.cmd inf ^<free^|checked^>
exit /b 2

# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# The INF gate must accept LIBUSB0.INF and reject unsafe changes.
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'infgate.ps1')
$t = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'LIBUSB0.INF')) -replace "`r?`n", "`r`n"
Assert-LibusbInf $t $Version
Assert-LibusbInf ($t.Replace('[Devices]', '[DEVICES]').Replace('CopyFiles=LibUsb.Sys,LibUsb.Dll', 'copyfiles = LibUsb.Sys, LibUsb.Dll')) $Version
$bad = [ordered]@{
    'class match'       = $t.Replace('USB\VID_16C0&PID_05DC', 'USB\Class_FF')
    'extra model'       = $t.Replace("`r`n[LibUsb]`r`n", "`r`n%USBasp%=LibUsb,USB\VID_1234&PID_5678`r`n[LibUsb]`r`n")
    'NT section'        = $t.Replace("[LibUsb.HW]`r`n", "[LibUsb.NT]`r`nCopyFiles=LibUsb.Sys`r`n`r`n[LibUsb.HW]`r`n")
    'upper filter'      = $t.Replace('HKR,,SurpriseRemovalOK,0x00010001,1', "HKR,,SurpriseRemovalOK,0x00010001,1`r`nHKR,,UpperFilters,0x00010000,libusb0")
    'DLL to SYSTEM32'   = $t.Replace('LibUsb.Dll=11', 'LibUsb.Dll=10,System32')
    'other loader'      = $t.Replace('*ntkern', '*vcomm')
    'other driver'      = $t.Replace('NTMPDriver,,LIBUSB0.SYS', 'NTMPDriver,,usbser.sys')
    'other class GUID'  = $t.Replace('{EB781AAF-9C70-4523-A5DF-642A87ECA567}', '{36FC9E60-C465-11CF-8056-444553540000}')
    'extra copy'        = $t.Replace('LIBUSB0.DLL=1', "LIBUSB0.DLL=1`r`nTESTLIBU.EXE=1")
    'missing string'    = $t.Replace('%Dragon%=', '%Nope%=')
    'NT signature'      = $t.Replace('$CHICAGO$', '$Windows NT$')
    'bad date'          = $t -replace 'DriverVer=\d\d/\d\d/\d{4}', 'DriverVer=13/40/2026'
    'other version'     = $t -replace 'DriverVer=(\d\d/\d\d/\d{4}),[0-9.]+', 'DriverVer=$1,9.9.9.9'
    'non-ASCII'         = $t + [char]0x100
    'duplicate section' = $t + "`r`n[Strings]`r`nX=""y""`r`n"
}
$n = 0
foreach ($k in $bad.Keys) {
    if ($bad[$k] -eq $t) { throw "mutation '$k' did not change the INF" }
    $failed = $false
    try { Assert-LibusbInf $bad[$k] $Version } catch { $failed = $true }
    if (-not $failed) { throw "INF gate accepted: $k" }
    $n++
}
Write-Output "PASS: LIBUSB0.INF accepted (2 forms); $n unsafe mutations rejected"

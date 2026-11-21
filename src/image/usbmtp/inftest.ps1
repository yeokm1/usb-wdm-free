# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# The INF gate must accept USBMTP.INF and reject unsafe changes.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'gate.ps1')
$p = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'USBMTP.INF')) -replace "`r?`n", "`r`n"
Assert-UsbmtpInf $p
Assert-UsbmtpInf ($p.Replace('[Models]', '[MODELS]').Replace('CopyFiles=UsbMtp.Sys', 'copyfiles = UsbMtp.Sys'))
$bad = [ordered]@{
    'whole-class match'     = $p.Replace('USB\Class_06&SubClass_01&Prot_01', 'USB\Class_06')
    'vendor-class match'    = $p.Replace('USB\Class_06&SubClass_01&Prot_01', 'USB\Class_FF')
    'extra model'           = $p.Replace('[UsbMtp]', "%Device%=UsbMtp,USB\Class_FF`r`n[UsbMtp]")
    'NT loader'             = $p.Replace('*ntkern', '*vcomm')
    'other driver'          = $p.Replace('NTMPDriver,,USBMTP.SYS', 'NTMPDriver,,usbscan.sys')
    'extra copy'            = $p.Replace("[UsbMtp.Sys]`r`nUSBMTP.SYS", "[UsbMtp.Sys]`r`nUSBMTP.SYS`r`nusbscan.sys")
    'wrong destination'     = $p.Replace('UsbMtp.Sys=10,System32\Drivers', 'UsbMtp.Sys=11')
    'stock class'           = $p.Replace('Class=UsbMtpDevices', 'Class=Image')
    'other GUID'            = $p.Replace('{2EFBDFB0-C5E2-4591-8903-72D29C3A8A4B}', '{6BDD1FC6-810F-11D0-BEC7-08002BE2092F}')
    'no class install'      = $p.Replace("[ClassInstall]`r`nAddReg=UsbMtp.ClassReg`r`n", '')
    'missing string'        = $p.Replace('%Device%=', '%Nope%=')
    'NT signature'          = $p.Replace('$CHICAGO$', '$Windows NT$')
    'bad date'              = $p -replace 'DriverVer=\d\d/\d\d/\d{4}', 'DriverVer=13/40/2026'
    'non-ASCII'             = $p + [char]0x100
    'duplicate section'     = $p + "`r`n[Strings]`r`nX=""y""`r`n"
}
$n = 0
foreach ($k in $bad.Keys) {
    $t = $bad[$k]
    if ($t -ceq $p) { throw "Mutation did not change USBMTP.INF: $k" }
    $failed = $false
    try { Assert-UsbmtpInf $t } catch { $failed = $true }
    if (-not $failed) { throw "USBMTP.INF gate accepted: $k" }
    $n++
}
Write-Output "PASS: USBMTP.INF and a case/spacing variant accepted; $n unsafe mutations rejected"

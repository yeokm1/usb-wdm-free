# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# The 98 INF gates must accept the package INFs and reject unsafe changes.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'gate98.ps1')
$p = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'USBCOM.INF'))
$c = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'USBCOMP.INF'))
Assert-Usbcom98Inf $p
Assert-Usbcomp98Inf $c
Assert-Usbcom98Inf ($p.Replace('[Models]', '[MODELS]').Replace('CopyFiles=UsbCom.Copy', 'copyfiles = UsbCom.Copy'))
$badP = @{
    'class-wide CDC match' = $p.Replace('USB\VID_2341&PID_0043', 'USB\Class_02')
    'extra model'          = $p.Replace('[UsbCom]', "%Uno%=UsbCom,USB\VID_1234&PID_5678`n[UsbCom]")
    'NT loader'            = $p.Replace('*ntkern', '*vcomm')
    'other driver'         = $p.Replace('NTMPDriver,,USBCOM.SYS', 'NTMPDriver,,usbser.sys')
    'in-box ccport pair'   = $p.Replace('NTMPDriver,,USBCOM.SYS', 'NTMPDriver,,"USBCOM.SYS,ccport.sys"')
    'extra copy'           = $p.Replace("[UsbCom.Copy]`r`nUSBCOM.SYS", "[UsbCom.Copy]`r`nUSBCOM.SYS`r`nccport.sys").Replace("[UsbCom.Copy]`nUSBCOM.SYS", "[UsbCom.Copy]`nUSBCOM.SYS`nccport.sys")
    'wrong destination'    = $p.Replace('UsbCom.Copy=10,System32\Drivers', 'UsbCom.Copy=11')
    'missing string'       = $p.Replace('%Pico%=', '%Nope%=')
    'NT signature'         = $p.Replace('$CHICAGO$', '$Windows NT$')
    'bad date'             = $p.Replace('10/24/2026', '13/40/2026')
    'non-ASCII'            = $p + [char]0x100
    'duplicate section'    = $p + "`n[Strings]`nX=""y""`n"
    'manual id broadened'  = $p.Replace('USBCOM\MANUAL_CDC_ACM', 'USB\Class_02&SubClass_00&Prot_00')
    'manual id dropped'    = $p.Replace('%Manual%=UsbCom,USBCOM\MANUAL_CDC_ACM', '')
    'board PID changed'    = $p.Replace('USB\VID_0483&PID_5740', 'USB\VID_0483&PID_5741')
    'string swapped'       = $p.Replace('%ArduinoBoot%=UsbCom,USB\VID_2341&PID_0036', '%Arduino%=UsbCom,USB\VID_2341&PID_0036')
}
foreach ($k in $badP.Keys) { if ($badP[$k] -ceq $p) { throw "Mutation did not change USBCOM.INF: $k" } }
$badC = @{
    'modem subclass'       = $c.Replace('PortSubClass,1,01', 'PortSubClass,1,02')
    'other port driver'    = $c.Replace('PortDriver,,USBCOMV.VXD', 'PortDriver,,serial.vxd')
    'WDM loader on child'  = $c.Replace('DevLoader,,*vcomm', 'DevLoader,,*ntkern')
    'NTMPDriver on child'  = $c.Replace('HKR,,ConfigDialog', "HKR,,NTMPDriver,,USBCOM.SYS`nHKR,,ConfigDialog")
    'broad match'          = $c.Replace('USBCOM\COMPORT', 'USBCOM\*')
    'short DCB'            = $c.Replace(',00,00,00,00,00' + "`r", ',00,00,00,00' + "`r").Replace(',00,00,00,00,00' + "`n", ',00,00,00,00' + "`n")
    'wrong class'          = $c.Replace('Class=Ports', 'Class=Modem')
}
$n = 0
foreach ($k in $badP.Keys) {
    $failed = $false
    try { Assert-Usbcom98Inf $badP[$k] } catch { $failed = $true }
    if (-not $failed) { throw "USBCOM.INF gate accepted: $k" }
    $n++
}
foreach ($k in $badC.Keys) {
    $failed = $false
    try { Assert-Usbcomp98Inf $badC[$k] } catch { $failed = $true }
    if (-not $failed) { throw "USBCOMP.INF gate accepted: $k" }
    $n++
}
Write-Output "PASS: both 98 INFs and a case/spacing variant accepted; $n unsafe mutations rejected"

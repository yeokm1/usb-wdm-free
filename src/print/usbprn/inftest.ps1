# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# The INF gates must accept the package INFs and reject unsafe changes.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'gate.ps1')
$mon = Join-Path $PSScriptRoot 'usbprnmn.c'
$p = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'USBPRN.INF')) -replace "`r?`n", "`r`n"
$m = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'USBPRNME.INF')) -replace "`r?`n", "`r`n"
Assert-UsbprnInf $p $mon
Assert-UsbprnMeInf $m
Assert-UsbprnInf ($p.Replace('[Models]', '[MODELS]').Replace('CopyFiles=UsbPrn.Sys,UsbPrn.Dll', 'copyfiles = UsbPrn.Sys, UsbPrn.Dll')) $mon
$badP = [ordered]@{
    'whole-class match'     = $p.Replace('USB\Class_07&SubClass_01&Prot_02', 'USB\Class_07')
    'whole-device match'    = $p.Replace('USB\Class_07&SubClass_01&Prot_01', 'USB\VID_04B8&PID_0005')
    'extra model'           = $p.Replace('[UsbPrn]', "%Uni%=UsbPrn,USB\Class_08&SubClass_06&Prot_50`r`n[UsbPrn]")
    'NT loader'             = $p.Replace('*ntkern', '*vcomm')
    'other driver'          = $p.Replace('NTMPDriver,,USBPRN.SYS', 'NTMPDriver,,usbprint.sys')
    'extra copy'            = $p.Replace("[UsbPrn.Sys]`r`nUSBPRN.SYS", "[UsbPrn.Sys]`r`nUSBPRN.SYS`r`nusbprint.sys")
    'wrong destination'     = $p.Replace('UsbPrn.Dll=11', 'UsbPrn.Dll=10')
    'no RunOnce'            = $p.Replace('"UsbPrnMonitor",,"RUNDLL32.EXE USBPRNMN.DLL,InstallMonitor"', '"UsbPrnMonitor",,"RUNDLL32.EXE USBPRNMN.DLL,Nothing"')
    'setting clobbered'     = $p.Replace('"ResetOnCancel",0x00010003,0', '"ResetOnCancel",0x00010001,0')
    'setting default'       = $p.Replace('"ResetOnCancel",0x00010003,0', '"ResetOnCancel",0x00010003,1')
    'setting missing'       = $p.Replace('HKLM,"Software\usb-wdm-free\usbprn","ResetOnCancel",0x00010003,0', '')
    'missing string'        = $p.Replace('%Uni%=', '%Nope%=')
    'NT signature'          = $p.Replace('$CHICAGO$', '$Windows NT$')
    'bad date'              = $p.Replace('10/31/2026', '13/40/2026')
    'non-ASCII'             = $p + [char]0x100
    'duplicate section'     = $p + "`r`n[Strings]`r`nX=""y""`r`n"
}
$badM = [ordered]@{
    'own driver'            = $m.Replace('NTMPDriver,,usbprint.sys', 'NTMPDriver,,USBPRN.SYS')
    'whole-class match'     = $m.Replace('USB\Class_07&SubClass_01&Prot_02', 'USB\Class_07')
    '1284.4 listed'         = $m.Replace('[UsbPrint]', "%Uni%=UsbPrint,USB\Class_07&SubClass_01&Prot_03`r`n[UsbPrint]")
    'copies files'          = $m.Replace('AddReg=UsbPrint.AddReg', "AddReg=UsbPrint.AddReg`r`nCopyFiles=X")
    'NT signature'          = $m.Replace('$CHICAGO$', '$Windows NT$')
}
$n = 0
foreach ($set in @(@($badP, $p, 'USBPRN.INF'), @($badM, $m, 'USBPRNME.INF'))) {
    foreach ($k in $set[0].Keys) {
        $t = $set[0][$k]
        if ($t -ceq $set[1]) { throw "Mutation did not change $($set[2]): $k" }
        $failed = $false
        try { if ($set[2] -eq 'USBPRN.INF') { Assert-UsbprnInf $t $mon } else { Assert-UsbprnMeInf $t } } catch { $failed = $true }
        if (-not $failed) { throw "$($set[2]) gate accepted: $k" }
        $n++
    }
}
# A setting the monitor reads that the INF does not write is caught, in
# every spelling of the call, and so is a second default for one setting.
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('usbprn-mon-' + [guid]::NewGuid().ToString('N') + '.c')
try {
    foreach ($extra in @('UmReadSetting("NewValue", 5)', 'UmReadSetting ("NewValue", 5)',
                         'UmReadSetting( "NewValue" , 5 )', 'UmReadSetting("NewValue", UM_X)',
                         'UmReadSetting("ResetOnCancel", 1)')) {
        [IO.File]::WriteAllText($tmp, [IO.File]::ReadAllText($mon) + "`r`nstatic DWORD x(void) { return $extra; }`r`n")
        $failed = $false
        try { Assert-UsbprnInf $p $tmp } catch { $failed = $true }
        if (-not $failed) { throw "USBPRN.INF gate accepted a monitor setting read: $extra" }
        $n++
    }
} finally { Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue }
Write-Output "PASS: both INFs and a case/spacing variant accepted; $n unsafe mutations rejected"

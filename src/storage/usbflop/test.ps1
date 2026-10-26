# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'validate.ps1')
$tested = 0
foreach ($variant in @('nt','9x','me')) {
    $text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot "$variant\USBFLOP.INF"))
    Assert-FloppyInf $text $variant
    Assert-FloppyInf ($text.Replace('Class=USB','class = USB').Replace('[Version]','[VERSION]')) $variant
    $bad = @{
        'broader class binding' = $text.Replace('USB\Class_08&SubClass_04&Prot_00','USB\Class_08')
        'bulk-only added' = $text.Replace('[Strings]', "%BOT%=UFI_BOT,USB\Class_08&SubClass_04&Prot_50`n[Strings]")
        'unknown extra model' = $text.Replace('[Models]', "[Models]`n%Extra%=Other,USB\Class_08")
        'CBI model uses CB' = $text.Replace('%CBI%=UFI_CBI','%CBI%=UFI_CB')
        'foreign executable copy' = $text.Replace('[Strings]', "[CopyPayload]`nforeign.sys`n[Strings]")
        'duplicate section' = $text + "`n[Version]`nClass=USB`n"
        'bad date' = $text.Replace('10/26/2026','02/30/2026')
        'undefined string' = $text.Replace('Provider="usb-wdm-free"','Renamed="usb-wdm-free"')
        'non-ASCII' = $text + [char]0x100
    }
    if ($variant -eq 'nt') {
        $bad['crossed install and service transport'] = $text.Replace('Needs=USBSTOR_CBI.NT.Services','Needs=USBSTOR_CB.NT.Services')
        $bad['foreign include'] = $text.Replace('Include=usbstor.inf','Include=other.inf')
        $bad['missing AMD64 models'] = $text.Replace('[Models.NTamd64]','[Renamed]')
        $bad['wrong child filter'] = $text.Replace('[Strings]', "[UFI_CBI.NT.HW]`nAddReg=Other`n[Strings]")
    } else {
        $bad['wrong CBI flag'] = $text.Replace('DriverFlags,0x00010001,2','DriverFlags,0x00010001,3')
        $bad['wrong CB flag'] = $text.Replace('DriverFlags,0x00010001,3','DriverFlags,0x00010001,1')
        $bad['wrong mapping filter'] = $text.Replace('USBNTMAP.SYS','OTHER.SYS')
        $bad['wrong loader'] = $text.Replace('*NTKERN','*IOS')
        $bad['copy installed files'] = $text.Replace('AddReg=Loader,CBI.Flags',"AddReg=Loader,CBI.Flags`nCopyFiles=Other")
        if ($variant -eq 'me') {
            $bad['missing media layout'] = $text.Replace('LayoutFile=layout.inf,layout1.inf,layout2.inf','')
            $bad['wrong media destination'] = $text.Replace('Storage.Copy=10,system32\drivers','Storage.Copy=11')
            $bad['missing storage copy'] = $text.Replace('CopyFiles=Storage.Copy','')
            $bad['foreign media binary'] = $text.Replace('[Storage.Copy]',"[Storage.Copy]`nforeign.sys")
        }
    }
    foreach ($name in $bad.Keys) {
        $rejected = $false
        try { Assert-FloppyInf $bad[$name] $variant } catch { $rejected = $true }
        if (-not $rejected) { throw "Accepted unsafe $variant mutation: $name" }
        $tested++
    }
}
Write-Output "PASS: NT/98-SE/ME originals and case/spacing variants; $tested unsafe mutations rejected"

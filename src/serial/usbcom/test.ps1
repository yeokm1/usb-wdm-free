# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'validate.ps1')
$source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'USBCDC.INF'))
Assert-CdcInf $source
# These mutations model dangerous package changes, not an implementation
# mirror: broadened matching, additional executable payload, broken service
# ownership and accidentally lost platform sections must fail closed.
$bad = @{
    'broad CDC class' = $source.Replace('USB\Class_02&SubClass_02','USB\Class_02')
    'whole Pico on XP' = $source.Replace('USB\VID_2E8A&PID_000A&MI_00','USB\VID_2E8A&PID_000A')
    'extra unparsed model' = $source.Replace('[Models.NTx86]', "%Bad%=Other,USB\Class_02`n[Models.NTx86]")
    'extra compatible ID' = $source.Replace('ACM,USB\VID_2341&PID_0043','ACM,USB\VID_2341&PID_0043,USB\Class_02')
    'service not associated' = $source.Replace('0x00000002','0x00000000')
    'wrong driver path' = $source.Replace('%12%\usbser.sys','%12%\other.sys')
    'boot start service' = $source.Replace('StartType=3','StartType=0')
    'foreign coinstaller' = $source.Replace('[Strings]', "[ACM.NT.CoInstallers]`nCopyFiles=Other`n[Strings]")
    'additional file copy' = $source.Replace('CopyFiles=FakeModemCopyFileSection','CopyFiles=FakeModemCopyFileSection,Other')
    'missing AMD64 section' = $source.Replace('[Models.NTamd64]','[Renamed]')
    'unresolved string' = $source.Replace('%Generic%=ACM','%Missing%=ACM')
    'duplicate section' = $source + "`n[Version]`nClass=USB`n"
    'invalid calendar date' = $source.Replace('10/26/2026','02/30/2026')
    'non-ASCII text' = $source + [char]0x100
    'composite board whole on XP' = $source.Replace('%Stm32%=ACM,USB\VID_0483&PID_5740' + "`r`n%Ch9102%",
        '%Stm32%=ACM,USB\VID_0483&PID_5740' + "`r`n%Arduino%=ACM,USB\VID_2341&PID_8036`r`n%Ch9102%")
    'board changed' = $source.Replace('%Stm32%=ACM,USB\VID_0483&PID_5740', '%Stm32%=ACM,USB\VID_0483&PID_5741')
    'manual id broadened' = $source.Replace('USBCOM\MANUAL_CDC_ACM', 'USB\Class_02&SubClass_00&Prot_00')
    'string swapped' = $source.Replace('%ArduinoBoot%=ACM,USB\VID_2341&PID_0036', '%Arduino%=ACM,USB\VID_2341&PID_0036')
}
foreach ($name in $bad.Keys) {
    if ($bad[$name] -ceq $source) { throw "Mutation did not change the INF: $name" }
    $failed = $false
    try { Assert-CdcInf $bad[$name] } catch { $failed = $true }
    if (-not $failed) { throw "Gate accepted unsafe mutation: $name" }
}
# INF directives are case-insensitive and allow whitespace.
Assert-CdcInf ($source.Replace('CopyFiles=','copyfiles = ').Replace('[Version]','[VERSION]'))
Write-Output "PASS: valid INF, case/spacing variant, $($bad.Count) unsafe package mutations rejected"

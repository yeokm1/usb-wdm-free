# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
#
# testinf.ps1 -Build <out\hid\absptr\wdmx86\<flavour>> -Dest <folder>
# Writes the TEST copies of absptr.inf and absptr2k.inf (docs/design/
# absptr.md section 8) with the simulator's ids added (absemu.py profiles
# kvm16, kvm12, touch, mtouch, pen: FFFF:0031, 0032, 0034, 0036, 0037),
# and copies
# absptr.sys beside them, for the VM legs. Never released: the release
# INFs list only the PenPartner.
param([Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$Dest)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Dest | Out-Null
$ids = @(
    @('KVM16', 'USB\VID_FFFF&PID_0031', 'Simulated 0..65535 absolute mouse (absptr TEST)'),
    @('KVM12', 'USB\VID_FFFF&PID_0032', 'Simulated 12-bit absolute mouse (absptr TEST)'),
    @('TOUCH', 'USB\VID_FFFF&PID_0034', 'Simulated HID touch screen (absptr TEST)'),
    @('MTOUCH', 'USB\VID_FFFF&PID_0036', 'Simulated multi-touch screen (absptr TEST)'),
    @('PEN', 'USB\VID_FFFF&PID_0037', 'Simulated pen digitizer (absptr TEST)'))
foreach ($inf in 'absptr.inf', 'absptr2k.inf') {
    $t = [IO.File]::ReadAllText((Join-Path $Build $inf))
    $models = ($ids | ForEach-Object { "%T$($_[0])%=ABSPTR,$($_[1])" }) -join "`r`n"
    $strings = ($ids | ForEach-Object { "T$($_[0])=`"$($_[2])`"" }) -join "`r`n"
    if ($t -notmatch '(?m)^%Generic%=ABSPTR,.*$') { throw "$inf has no Generic model line" }
    $t = $t -replace '(?m)^(%Generic%=ABSPTR,[^\r\n]*)', "`$1`r`n$models"
    $t = $t.TrimEnd() + "`r`n$strings`r`n"
    $t = $t -replace 'Experimental build; not released\.', 'TEST COPY with simulator ids; never released.'
    [IO.File]::WriteAllText((Join-Path $Dest $inf), $t, [Text.Encoding]::ASCII)
}
Copy-Item (Join-Path $Build 'absptr.sys') $Dest -Force
Get-ChildItem $Dest | Get-FileHash -Algorithm SHA256 | ForEach-Object { $_.Hash + '  ' + (Split-Path $_.Path -Leaf) }

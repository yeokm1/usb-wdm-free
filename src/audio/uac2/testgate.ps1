# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Negative tests for the text import gate; never build or read an OS binary.
# Existing reports come only from this package's own x86/x64 builds.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$out = Join-Path $root 'out\audio\uac2\gatetest'
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item -LiteralPath (Join-Path $out 'PASS.TXT') -Force -ErrorAction SilentlyContinue
$shell = Join-Path $PSHOME 'powershell.exe'
if (-not (Test-Path -LiteralPath $shell)) { $shell = Join-Path $PSHOME 'pwsh.exe' }
if (-not (Test-Path -LiteralPath $shell)) { throw 'PowerShell child executable missing' }
$gate = Join-Path $PSScriptRoot 'chkimp.ps1'
$script:checks = 0
$script:results = @()

function CheckGate([string]$Name, [string]$HeaderText, [string]$ImportText,
                   [string]$AllowText, [string]$Machine, [string]$Subsystem,
                   [bool]$ExpectedPass, [string]$ExpectedReason) {
    $folder = Join-Path $out $Name
    New-Item -ItemType Directory -Force $folder | Out-Null
    $headerFile = Join-Path $folder 'headers.txt'
    $importFile = Join-Path $folder 'imports.txt'
    $allowFile = Join-Path $folder 'allowed.txt'
    $reportFile = Join-Path $folder 'report.txt'
    [IO.File]::WriteAllText($headerFile, $HeaderText, [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText($importFile, $ImportText, [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText($allowFile, $AllowText, [Text.Encoding]::ASCII)
    $output = & $shell -NoProfile -ExecutionPolicy Bypass -File $gate -Headers $headerFile `
        -Imports $importFile -Allowed $allowFile -Machine $Machine -Subsystem $Subsystem -Report $reportFile 2>&1
    $code = $LASTEXITCODE
    $output | Set-Content -Encoding ASCII (Join-Path $folder 'output.txt')
    $report = [IO.File]::ReadAllText($reportFile)
    if (($code -eq 0) -ne $ExpectedPass -or
        $report -notmatch [regex]::Escape($ExpectedReason)) {
        throw "Gate case $Name failed: exit=$code expectedPass=$ExpectedPass`n$report"
    }
    $script:checks++
    $script:results += "PASS $Name (exit=$code)"
}

$h = " 14C machine (i386)`n 4.00 subsystem version`n 1 subsystem (Native)`n 0 [ 0] RVA [size] of Delay Import Directory`n"
$imports = "  Section contains the following imports:`n`n    NTOSKRNL.EXE`n 1000 Import Address Table`n 2000 Import Name Table`n 0 time date stamp`n 0 Index of first forwarder reference`n 1 DbgPrint`n`n  Summary`n"
$allow = "NTOSKRNL.EXE!DbgPrint`n"
CheckGate 'named' $h $imports $allow '14C' '4.00' $true 'result: PASS'
CheckGate 'empty' $h '' $allow '14C' '4.00' $false 'no named imports parsed'
CheckGate 'garbage' $h 'not an import report' $allow '14C' '4.00' $false 'no import section found'
CheckGate 'truncated' $h ($imports -replace '  Summary','') $allow '14C' '4.00' $false 'missing import report summary'
CheckGate 'unknownrow' $h ($imports -replace '1 DbgPrint','1 DbgPrint unexpected') $allow '14C' '4.00' $false 'unrecognized import row'
CheckGate 'emptydll' $h ($imports -replace ' 1 DbgPrint','') $allow '14C' '4.00' $false 'no named imports parsed for NTOSKRNL.EXE'
CheckGate 'emptysecond' $h ($imports -replace '  Summary', "    EMPTY.SYS`n  Summary") $allow '14C' '4.00' $false 'no named imports parsed for EMPTY.SYS'
CheckGate 'ordinal' $h ($imports -replace '1 DbgPrint','Ordinal 2') $allow '14C' '4.00' $false 'ordinal import NTOSKRNL.EXE!#2'
CheckGate 'ordinal32' $h ($imports -replace '1 DbgPrint','80000002  Ordinal 2') $allow '14C' '4.00' $false 'ordinal import NTOSKRNL.EXE!#2'
CheckGate 'ordinal64' $h ($imports -replace '1 DbgPrint','8000000000000002  Ordinal 2') $allow '14C' '4.00' $false 'ordinal import NTOSKRNL.EXE!#2'
CheckGate 'mixedordinal' $h ($imports -replace ' 1 DbgPrint', " 1 DbgPrint`n 80000002 Ordinal 2") $allow '14C' '4.00' $false 'ordinal import NTOSKRNL.EXE!#2'
CheckGate 'delaysection' $h ($imports -replace 'following imports:','following delay load imports:') $allow '14C' '4.00' $false 'delay-load import section'
CheckGate 'delayheader' ($h -replace '0 \[ 0\] RVA', '3000 [ 40] RVA') $imports $allow '14C' '4.00' $false 'nonempty PE delay-import directory'
CheckGate 'unlisted' $h ($imports -replace 'DbgPrint','UnexpectedRoutine') $allow '14C' '4.00' $false 'not allowed: NTOSKRNL.EXE!UnexpectedRoutine'
CheckGate 'machine' $h $imports $allow '8664' '4.00' $false 'machine is not 8664'
CheckGate 'subsystem' $h $imports $allow '14C' '5.02' $false 'subsystem version is not 5.02'
CheckGate 'native' ($h -replace '1 subsystem \(Native\)','3 subsystem (Windows CUI)') $imports $allow '14C' '4.00' $false 'subsystem is not Native'

foreach ($profile in @('wdmx86','wdm64')) {
    foreach ($flavour in @('free','checked')) {
        $folder = Join-Path $root "out\audio\uac2\$profile\$flavour"
        $realH = [IO.File]::ReadAllText((Join-Path $folder 'headers.txt'))
        $realI = [IO.File]::ReadAllText((Join-Path $folder 'imports.txt'))
        $realA = [IO.File]::ReadAllText((Join-Path $PSScriptRoot "imports-$profile.txt"))
        $machine = if ($profile -eq 'wdmx86') { '14C' } else { '8664' }
        $subsystem = if ($profile -eq 'wdmx86') { '4.00' } else { '5.02' }
        CheckGate "$profile-$flavour" $realH $realI $realA $machine $subsystem $true 'result: PASS'
    }
}
$results | Set-Content -Encoding ASCII (Join-Path $out 'results.txt')
"PASS $checks gate cases" | Set-Content -Encoding ASCII (Join-Path $out 'PASS.TXT')
Write-Output "PASS $checks gate cases; reports in $out"

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
# Builds and runs every seeded fault of the slot model (docs/design/mtp.md
# 5.4, "The model"): each of the model's compile switches, and broken
# copies of sys\slot.c, through slotmodel.ps1. Each must make the model
# fail, and fail for its intended reason: the text expected is looked for
# among the first failures of the scenarios that fail. The two switches
# documented as caught only together are run alone too and must pass, as
# must the one broken copy that the submission's commit covers (a second
# guard). A broken copy that does not build is an error, never a catch.
# Output: out\image\usbmtp\slotmut\ (one folder per fault, SUMMARY.TXT).
#   -Only  run only the faults whose name holds this text
param([string]$Only = '')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$top = Join-Path $root 'out\image\usbmtp\slotmut'
if (-not $Only -and (Test-Path $top)) { Remove-Item -Recurse -Force $top }
New-Item -ItemType Directory -Force $top | Out-Null

# The model's switches: name = expected first-failure text ('' = must pass).
$switches = [ordered]@{
    # the standby rework's seeded faults
    'MUT_UNBOUNDED_WAIT'        = 'an actor is stuck'
    'MUT_QUERY_NO_QUIET'        = 'a query passed down with a request outstanding'
    'MUT_VETO_KEEPS_GATE'       = 'the gate stayed closed with no reason standing'
    'MUT_DEADLINE_COMPLETES'    = 'completed with its lower IRP in the stack'
    'MUT_DEADLINE_FREES'        = 'a lower IRP with the stack while its slot is free'
    'MUT_RESUBMIT_HELD'         = 'lower IRP passed down while the stack still had it'
    'MUT_WAKE_RELEASES'         = 'admitted while a request the gate stopped is still out'
    # the review of the rework
    'MUT_SUBMIT_NO_COMMIT'      = 'a URB passed down while the device sleeps'
    'MUT_SET_IGNORES_CALLS'     = 'a URB passed down while the device sleeps'
    'MUT_REMOVAL_QUERY_SUCCESS' = 'a query refused admission (removal) completed with success'
    'MUT_DQUERY_PASSTHROUGH'    = 'a query passed down with a request outstanding'
    'MUT_S0_ENDS_DQUERY'        = 'a system S0 ended the device query'
    'MUT_VETO_ALSO_FORWARDS'    = 'a power IRP passed down after its completion'
    'MUT_LOWER_ANSWER_IGNORED'  = "a query completed with success without the lower drivers' success"
    # the third review of the rework: the worker's and the query's lifetimes
    'MUT_WORKER_RIDES_QUERY'    = 'device touched after its free'
    'MUT_QUERY_UNPROTECTED'     = 'device touched after its free'
    # earlier ones
    'HL_PIN_ACROSS_COMPLETION'  = 'an actor is stuck'
    'MUT_LATE_WORKER'           = 'an actor is stuck'
    'MUT_UNLOAD_NO_WAIT'        = 'a work routine ran after DriverUnload'
    'MUT_PUBLISH_LATE MUT_ENTER_IGNORES_GONE' = 'free with a dispatch pin held'
    'MUT_PUBLISH_LATE'          = ''
    'MUT_ENTER_IGNORES_GONE'    = ''
    'MUT_UNLOAD_WAITS_SELF'     = 'an actor is stuck'
    'MUT_UNLOAD_NO_LATE_WAIT'   = 'the late worker ran after DriverUnload on another thread'
    'MUT_LATE_INLINE'           = 'a power IRP completed above PASSIVE_LEVEL'
    'MUT_LATE_ADD_AFTER'        = 'the late worker ran after DriverUnload on another thread'
    'MUT_ONE_REASON'            = 'a URB passed down while the device sleeps'
    'MUT_SLEEP_REQUESTS_D3'     = 'REMOVE after a D3 went down during a system sleep'
}

# Broken copies of slot.c: name = (text, replacement, expected first-failure text).
$copies = [ordered]@{
    'commit ignores the stop' = @(
        "    if (S->State == SS_ACTIVE) {`n        S->Calling = 1;",
        "    if (S->State == SS_ACTIVE || S->State == SS_STOPPING) {`n        S->Calling = 1;",
        "an IoCancelIrp overlapped the lower IRP's rebuild")
    'Calling never set' = @(
        "        S->Calling = 1;     /* the call is made",
        "        (void)0;     /* the call is made",
        'a lower IRP passed down without its commit')
    'the lapse compared without the wrap' = @(
        "(unsigned long)(Now - D->QueryAt) >= HoldMs",
        "Now >= D->QueryAt + HoldMs",
        'a request admitted between an accepted query and its set')
    'DRAIN never ended' = @(
        "        D->PowerDown = (unsigned char)(D->PowerDown & ~SLOT_PD_DRAIN);",
        "        ;",
        "GET_INFO's slots still out disagree with draining")
    'DRAIN ended with tails out' = @(
        "    if (!D->Tails) {`n        /* Quiet",
        "    if (D->Tails <= D->TailLimit) {`n        /* Quiet",
        'a query passed down with a request outstanding')
    'decide ignores DRAIN' = @(
        " && !(D->PowerDown & SLOT_PD_DRAIN) &&`n        D->Active && D->Active < 0xFFFF) {",
        " &&`n        D->Active && D->Active < 0xFFFF) {",
        'a query passed down with a request outstanding')
    'power-up clears DRAIN' = @(
        "    D->PowerDown = (unsigned char)(D->PowerDown & ~Why);",
        "    D->PowerDown = (unsigned char)(D->PowerDown & ~(Why | SLOT_PD_DRAIN));",
        'admitted while a request the gate stopped is still out')
    'a device set leaves the device query' = @(
        "    if (Why & SLOT_PD_DEV) {`n        QueryOff(D, SLOT_QD);`n    }",
        "    if (Why & SLOT_PD_QUERY) {`n        QueryOff(D, SLOT_QD);`n    }",
        'a device set left the device query standing')
    'a refusal not counted' = @(
        "        D->Vetoes = (unsigned short)(D->Vetoes + 1);    /* modulo 65536 */",
        "        ;",
        'the refusal count differs from the refusals made')
    'the poll delivers with the gate closed' = @(
        "    if (Ok && D->PowerDown) {`n        D->Lost = 1;",
        "    if (Ok && D->PowerDown && D->Lost == 2) {`n        D->Lost = 1;",
        'neither queued nor flagged lost')
    'cancel during the rebuild not deferred' = @(
        "    if (S->Rebuilding) {`n        return;     /* deferred",
        "    if (S->Rebuilding && S->Pins > SLOT_MAX_PINS) {`n        return;     /* deferred",
        "an IoCancelIrp overlapped the lower IRP's rebuild")
    'deferred cancel delivered twice' = @(
        "    /* a stop came during the rebuild: its deferred cancel is honoured by",
        "    CancelLower(Slot, S, Out);`n    /* a stop came during the rebuild: its deferred cancel is honoured by",
        'a deferred cancel delivered twice')
    'deferred cancel lost (handed off although stopped)' = @(
        "    S->Rebuilding = 0;`n    if (S->State == SS_ACTIVE) {`n        return 1;",
        "    S->Rebuilding = 0;`n    if (S->State == SS_ACTIVE || S->State == SS_STOPPING) {`n        return 1;",
        'COVERED')   # the post-call handshake cancels it
    'deferred cancel lost, and no post-call cancel' = @(
        "    S->Rebuilding = 0;`n    if (S->State == SS_ACTIVE) {`n        return 1;",
        "    S->Rebuilding = 0;`n    if (S->State == SS_ACTIVE || S->State == SS_STOPPING) {`n        return 1;",
        "    if (S->State == SS_STOPPING && S->Lower) {`n        CancelLower(Slot, S, Out);`n    }`n    Finish(D, Slot, Out);`n}",
        "    Finish(D, Slot, Out);`n}",
        'an actor is stuck')
    'an accepted query takes no admission of its own' = @(
        "        D->Active++;    /* the query's own admission",
        "        (void)0;    /* the query's own admission",
        'an accepted query took no admission of its own')
    'the lower answer keeps the query''s admission' = @(
        "    SlotActiveLeave(D);     /* the query's own admission, whatever its state now */",
        "    (void)0;     /* the query's own admission, whatever its state now */",
        'an admission was never given up')
    'late completion of a stopped request sends the next segment' = @(
        "    if (S->State == SS_ACTIVE && Ok && !Short && S->SegsLeft > 0) {",
        "    if ((S->State == SS_ACTIVE || S->State == SS_STOPPING) && Ok && !Short && S->SegsLeft > 0) {",
        'COVERED')   # the commit withdraws a submission for a request no longer ACTIVE
}

$slotText = [IO.File]::ReadAllText("$PSScriptRoot\sys\slot.c")
$results = @()
function Run-One([string]$name, [string]$defs, [string]$slot, [string]$expect, [bool]$mustPass) {
    $dir = Join-Path $top (($name -replace '[^A-Za-z0-9_]+', '-').Trim('-'))
    $ok = $true
    try { & "$PSScriptRoot\slotmodel.ps1" -Defs $defs -Slot $slot -Out $dir | Out-Null } catch { $ok = $false }
    $res = Join-Path $dir 'result.txt'
    $why = @()
    $built = Test-Path $res
    if ($built) { $why = @(Get-Content $res | Where-Object { $_ -match '^\s+first failure: ' } | ForEach-Object { ($_ -replace '^\s+first failure: ', '').Trim() } | Sort-Object -Unique) }
    if (-not $built) { $verdict = 'BUILD FAILED' }
    elseif ($mustPass) { $verdict = if ($ok) { 'PASS (not caught alone, as documented)' } else { 'UNEXPECTED FAILURE' } }
    elseif ($ok) { $verdict = 'NOT CAUGHT' }
    elseif (-not $expect -or ($why | Where-Object { $_.Contains($expect) })) { $verdict = 'CAUGHT as intended' }
    else { $verdict = 'CAUGHT, but not for its intended reason' }
    $line = "$verdict : $name" + $(if ($why.Count) { "`r`n      " + ($why -join "`r`n      ") } else { '' })
    Write-Host $line
    return $line
}
foreach ($k in $switches.Keys) {
    if ($Only -and -not $k.Contains($Only)) { continue }
    $results += Run-One "model $k" $k '' $switches[$k] ($switches[$k] -eq '')
}
foreach ($k in $copies.Keys) {
    if ($Only -and -not $k.Contains($Only)) { continue }
    $c = $copies[$k]
    if ($c.Count -eq 5) { $a, $b, $a2, $b2, $e = $c } else { $a, $b, $e = $c; $a2 = $null }
    if (-not $slotText.Contains($a) -or ($a2 -and -not $slotText.Contains($a2))) { $results += "MISSING PATTERN : slot.c $k"; continue }
    $dir = Join-Path $top 'copies'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $f = Join-Path $dir ((($k -replace '[^A-Za-z0-9]+', '-').Trim('-')) + '.c')
    $t = $slotText.Replace($a, $b); if ($a2) { $t = $t.Replace($a2, $b2) }
    [IO.File]::WriteAllText($f, $t)
    if ($e -eq 'COVERED') { $results += Run-One "slot.c $k" '' $f '' $true }
    else { $results += Run-One "slot.c $k" '' $f $e $false }
}
$results | Set-Content -Encoding ASCII (Join-Path $top 'SUMMARY.TXT')
$bad = @($results | Where-Object { $_ -notmatch '^(CAUGHT as intended|PASS \(not caught alone)' })
if ($bad.Count) { throw "$($bad.Count) seeded fault(s) not caught as intended (SUMMARY.TXT)" }
'PASS every seeded fault caught for its intended reason' | Set-Content -Encoding ASCII (Join-Path $top 'PASS.TXT')

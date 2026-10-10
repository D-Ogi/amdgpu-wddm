param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\submit-watchdog",[switch]$FlatFiveHundred,[switch]$NoTdrFloor,[switch]$IgnoreProgress,[switch]$StampAtRingWrite,[switch]$ReportWithPendingCompletion,[switch]$NoFenceRange,[switch]$LogTheConstant,[switch]$RearmOutsideLock)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
# BD-114 (scratch\bd114\ANALYSIS.md 7.1, 7.2, 7.3, 7.4, 7.7; docs/design/hang-recovery.md). The headers and the
# test under test are copied next to the build, so a negative control changes the copy and never the tree (the
# pattern of run_hang_recovery.ps1 and run_vmid_pool.ps1). Each control must fail by a CHECK or a source check,
# not by a build error, or it proves nothing.
#
#   -FlatFiveHundred              7.1: the default budget is the old 500 ms literal, not TdrDelay plus a margin.
#   -NoTdrFloor                   7.1: an operator's value is no longer raised to TdrDelay. This is the exact
#                                      configuration that bugchecked unit A twice on 2026-10-10.
#   -IgnoreProgress               7.2: the watchdog judges by wall clock alone, as every build before this one.
#   -StampAtRingWrite             7.3: the budget is counted from the ring write again, so a queued packet pays
#                                      for its wait.
#   -ReportWithPendingCompletion  7.4: the aborted fence is named although a completion of that node is pending,
#                                      which is the lag the design note's objection names.
#   -NoFenceRange                 7.4: the 0x119 range guard is dropped from the aborted-fence answer.
#   -LogTheConstant               7.7: the timeout line's elapsed time is a constant again, so a reader cannot
#                                      price a workload against the budget without a bugcheck.
#   -RearmOutsideLock             the watchdog DPC re-arms its timer after releasing wddm->Lock, which is the
#                                      use-after-free WddmStop's ordering rule exists to prevent.

# ---- the source checks ------------------------------------------------------------------------------------------
#
# The host model proves the decisions, but it cannot compile wddm.c. So wddm.c itself must still read the budget
# from the registry, stamp the deadline at the head, re-arm under the lock, log the measured numbers and ask for
# the aborted-fence answer. Every check below is a line whose removal would leave this suite green.
$wddm=Get-Content "$repo\driver\kmd\wddm.c" -Raw
function Body([string]$text,[string]$start){
    $at=$text.IndexOf($start)
    if($at -lt 0){throw "source check: '$start' is not in wddm.c"}
    $end=$text.IndexOf("`n}",$at)
    if($end -lt 0){throw "source check: no end of '$start' in wddm.c"}
    return $text.Substring($at,$end-$at)
}
function Ordered([string]$body,[string]$what,[string[]]$steps){
    $pos=0
    foreach($s in $steps){
        $i=$body.IndexOf($s,$pos)
        if($i -lt 0){Write-Output "FAIL source check: $what lacks '$s' after the step before it (wddm.c)";exit 1}
        $pos=$i+$s.Length
    }
}
$dpc=Body $wddm 'static void WddmSubmitDpcCheck('
if($RearmOutsideLock){
    # Move the re-arm's KeSetTimer out of the critical section, by swapping it with the release that follows it.
    $set='KeSetTimer(&wddm->SubmitTimer, due, &wddm->SubmitDpc);'
    $rel='KeReleaseSpinLock(&wddm->Lock, irql);'
    $iSet=$dpc.IndexOf($set);$iRel=$dpc.IndexOf($rel,$iSet)
    if($iSet -lt 0 -or $iRel -lt 0){throw 'negative control: the re-arm or the lock release moved'}
    $moved=$dpc.Remove($iSet,$set.Length).Insert($iSet,$rel)
    $iRel=$moved.IndexOf($rel,$iSet+$rel.Length)
    $dpc=$moved.Remove($iRel,$rel.Length).Insert($iRel,$set)
}
# 7.1 reaches the registry, and the flat constant is gone from the whole driver.
if($wddm -notmatch 'GuardReadSetting\(L"SubmitWatchdogMs", 0\)'){Write-Output 'FAIL source check: wddm.c does not read the SubmitWatchdogMs setting (BD-114 7.1)';exit 1}
if($wddm -notmatch 'GuardReadGraphicsSetting\(L"TdrDelay", 0\)'){Write-Output 'FAIL source check: wddm.c does not read TdrDelay (BD-114 7.1)';exit 1}
$flat=Get-ChildItem "$repo\driver\kmd\*" -Recurse -File | Where-Object { $_.Extension -in '.c','.h' } |
    Select-String -SimpleMatch 'BC250_WDDM_SUBMIT_TIMEOUT_MS'
if($flat){Write-Output "FAIL source check: BC250_WDDM_SUBMIT_TIMEOUT_MS is back in $($flat[0].Path) (BD-114 7.1)";exit 1}
# 7.3: the stamp, the window and the first look all happen when the job becomes the head.
Ordered (Body $wddm 'static void WddmGfxHeadLocked(') 'WddmGfxHeadLocked' @(
    'Bc250SubmitWatchdogIdle(&Wddm->SubmitWatchdog);',
    'if (job->Deadline == 0ull)',
    'job->HeadSince = now;',
    'Bc250SubmitHeadDeadline(job->Deadline, now, Wddm->SubmitBudgetMs)',
    'Bc250SubmitWatchdogArm(&Wddm->SubmitWatchdog, now);',
    'KeSetTimer(&Wddm->SubmitTimer, due, &Wddm->SubmitDpc);')
# 7.2 and the lock: the token is read, the window judges, and the re-arm is INSIDE the critical section. A
# KeSetTimer after the release can arm a timer that WddmStop has already cancelled and is about to free.
Ordered $dpc 'the submit watchdog DPC' @(
    'progress = WddmSubmitProgress(device);',
    'KeAcquireSpinLock(&wddm->Lock, &irql);',
    'Bc250SubmitWatchdogCheck(&wddm->SubmitWatchdog, progress, now,',
    'if (stale && head != NULL && now >= head->Deadline)',
    'KeSetTimer(&wddm->SubmitTimer, due, &wddm->SubmitDpc);',
    'KeReleaseSpinLock(&wddm->Lock, irql);')
# 7.7: the timeout line carries the measured numbers.
if($wddm -notmatch 'timeout measured: head %lu ms, queued %lu ms, stale %lu ms, budget %lu ms'){Write-Output 'FAIL source check: the timeout line does not report the measured times (BD-114 7.7)';exit 1}
# 7.4: ResetEngine asks for the aborted-fence answer and reopens the ring, and both submit DDIs record the fence
# range's upper bound. Only the virtual one did in the first draft, and the non-virtual one would have left the
# guard refusing a report the contract asks for.
Ordered (Body $wddm 'static NTSTATUS Bc250WddmResetEngine(') 'Bc250WddmResetEngine' @(
    'completionPending = wddm->CompletionPending[pResetEngine->NodeOrdinal] != 0;',
    'Bc250HangAbortReportedFence(onRing, completionPending, lastKnown, lastCompleted, lastSubmittedKnown,',
    'GfxReopenAfterAbort(device)',
    'verdict = BC250_HANG_VERDICT_ABORT_REPORTED;')
foreach($ddi in @('static NTSTATUS Bc250WddmSubmitCommand(','static NTSTATUS Bc250WddmSubmitCommandVirtual(')){
    Ordered (Body $wddm $ddi) $ddi.Trim('(') @(
        'KeAcquireSpinLock(&wddm->Lock, &irql);',
        'WddmNoteSubmittedLocked(wddm, node, pSubmitCommand->SubmissionFenceId);')
}

# ---- the copies the host test compiles --------------------------------------------------------------------------
$watchdog=Get-Content "$repo\driver\kmd\submit_watchdog.h" -Raw
if($FlatFiveHundred){
    $live='    if (Requested == 0ul) { *Defaulted = 1; budget = tdr + (unsigned long)BC250_SUBMIT_MARGIN_MS; }'
    if(!$watchdog.Contains($live)){throw 'negative control: the default budget moved'}
    $watchdog=$watchdog.Replace($live,'    if (Requested == 0ul) { *Defaulted = 1; budget = 500ul; (void)tdr; }')
}
if($NoTdrFloor){
    $live='    if (budget < tdr) { budget = tdr; *Raised = 1; }'
    if(!$watchdog.Contains($live)){throw 'negative control: the TdrDelay floor moved'}
    $watchdog=$watchdog.Replace($live,'    if (budget < tdr && tdr == 0xDEADul) { budget = tdr; *Raised = 1; }')
}
if($IgnoreProgress){
    $live='    if (!watched || Progress != Watchdog->Progress || Now < Watchdog->Advanced)'
    if(!$watchdog.Contains($live)){throw 'negative control: the progress comparison moved'}
    $watchdog=$watchdog.Replace($live,'    if (!watched)')
}
if($LogTheConstant){
    $live='    return Now > Start ? (unsigned long)((Now - Start) / 10000ull) : 0ul;'
    if(!$watchdog.Contains($live)){throw 'negative control: the elapsed-time helper moved'}
    $watchdog=$watchdog.Replace($live,'    (void)Start; (void)Now; return 500ul;')
}
[IO.File]::WriteAllText((Join-Path $Out 'submit_watchdog.h'),$watchdog)

$hang=Get-Content "$repo\driver\kmd\hang_recovery.h" -Raw
if($ReportWithPendingCompletion){
    $live='    if (jobOnRing || completionPending || !lastReportedKnown || !lastSubmittedKnown) return 0;'
    if(!$hang.Contains($live)){throw 'negative control: the pending-completion guard moved'}
    $hang=$hang.Replace($live,'    if (jobOnRing || !lastReportedKnown || !lastSubmittedKnown) return 0; (void)completionPending;')
}
if($NoFenceRange){
    $live='    if (!Bc250AbortedFenceValid(lastReported, lastReported, lastSubmitted)) return 0;'
    if(!$hang.Contains($live)){throw 'negative control: the fence-range guard of the aborted-fence answer moved'}
    $hang=$hang.Replace($live,'    if (lastSubmitted == 0xDEADu) return 0;')
}
[IO.File]::WriteAllText((Join-Path $Out 'hang_recovery.h'),$hang)

$test=Get-Content "$PSScriptRoot\submit_watchdog_test.c" -Raw
if($StampAtRingWrite){
    $live='        job->Deadline = Bc250SubmitHeadDeadline(job->Deadline, m->Now, m->BudgetMs); /* STAMP SITE */'
    if(!$test.Contains($live)){throw 'negative control: the stamp site moved'}
    $test=$test.Replace($live,'        job->Deadline = Bc250SubmitHeadDeadline(job->Deadline, job->Submitted, m->BudgetMs); /* STAMP SITE */')
}
[IO.File]::WriteAllText((Join-Path $Out 'submit_watchdog_test.c'),$test)

$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /wd4127 /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\submit_watchdog_test.exe" "$Out\submit_watchdog_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'submit watchdog host build failed'}
& "$Out\submit_watchdog_test.exe"
exit $LASTEXITCODE

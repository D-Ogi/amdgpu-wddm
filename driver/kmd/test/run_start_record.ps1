# BD-122: the start record (guard.c Parameters\StartRecord) and the start path that writes it, on the host.
#
#   pwsh -NoProfile -File driver\kmd\test\run_start_record.ps1 [-Root P:\BC-250] [-Out <dir>] [-Repo <tree>]
#
# The real sources are extracted by text, like every other host test here: the BD-122 section of guard.c and the
# whole of Bc250StartDevice from pnp.c. -Repo runs the same test against another tree, which is how the negative
# control "this test fails on the base commit" is shown: a tree without the section gets the six recorder
# functions as no-ops, so the test compiles and reports what is missing instead of failing to build.
#
# Each mutation is one requirement's negative control and must make the test fail:
#   -GateOnKeepLog     the record behind Parameters\KeepLog, which is what BD-122 was: requirement 1.
#   -NoAttemptRecord   no "attempt" record at the start's entry, so an older boot's failure stays readable as
#                      this boot's reason: requirement 2.
#   -RecordBeforeCleanup  the record written before the failure path's cleanup: requirement 4 (the order).
#   -RecordAtAnyIrql   the PASSIVE_LEVEL check dropped: requirement 4 (the IRQL).
#   -LogAfterCleanup   the failure point ignored, so the kept lines are the teardown's and not the failing step's:
#                      requirement 1 (the log lines that lead up to the failure).
#   -NoInitialBlank / -NoFinalBlank  omit one hardware blank while keeping the
#                      rest of StartDevice intact: the sequencing cases must fail.
#   -KeepStopVisibilityHold  leave the startup hold set during stop: the rollback
#                      cases must fail before the modeled display handoff.
param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
      [string]$Out="$Root\scratch\build\startrecord",
      [string]$Repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path,
      [switch]$GateOnKeepLog,[switch]$NoAttemptRecord,[switch]$RecordBeforeCleanup,[switch]$RecordAtAnyIrql,
      [switch]$LogAfterCleanup,[switch]$NoInitialBlank,[switch]$NoFinalBlank,
      [switch]$KeepStopVisibilityHold)
$ErrorActionPreference='Stop'
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
New-Item -ItemType Directory -Force $Out | Out-Null

# ---- guard.c: the BD-122 section ---------------------------------------------------------------------------------
$source=Get-Content (Join-Path $Repo 'driver\kmd\guard.c') -Raw
$first=$source.IndexOf('// ---- BD-122:')
$last=$source.IndexOf('// ---- BD-090:')
if ($first -lt 0 -or $last -lt $first) {
    Write-Host "guard.c in $Repo has no BD-122 section: the recorders are stubbed, so this run is the control"
    $code=@'
/* No BD-122 section in this tree. The entry points exist so the test can run and report what is absent. Every
   function the test or the extracted Bc250StartDevice calls has to be here, or the control fails to build instead
   of reporting what the base tree does not do. */
void GuardStartStep(PCWSTR Step){(void)Step;}
void GuardStartSubStep(PCWSTR SubStep){(void)SubStep;}
void GuardStartDetail(PCWSTR Format, ...){(void)Format;}
void GuardStartFailureMark(void){}
void GuardRecordStartAttempt(void){}
void GuardRecordStartRefused(NTSTATUS Status){(void)Status;}
void GuardRecordStartFailed(NTSTATUS Status){(void)Status;}
void GuardRecordStartStarted(void){}
'@
} else {
    $code=$source.Substring($first,$last-$first)
    if ($GateOnKeepLog) {
        $code=$code.Replace('    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !DriverStateReady()) {',
            "    if (GuardReadSetting(L`"KeepLog`", 0) == 0) return;`n    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !DriverStateReady()) {")
    }
    if ($RecordAtAnyIrql) {
        $code=$code.Replace('if (KeGetCurrentIrql() != PASSIVE_LEVEL || !DriverStateReady()) {',
                            'if (!DriverStateReady()) {')
    }
    if ($LogAfterCleanup) {
        $mark='ULONG mark = g_StartLogMark;'
        if (!$code.Contains($mark)) { throw 'guard.c does not read g_StartLogMark into the record write' }
        $code=$code.Replace($mark,'ULONG mark = 0;')
    }
}
[IO.File]::WriteAllText((Join-Path $Out 'record_actual.inc'),$code)

# ---- pnp.c: the whole of Bc250StartDevice ------------------------------------------------------------------------
$source=Get-Content (Join-Path $Repo 'driver\kmd\pnp.c') -Raw
$first=$source.IndexOf('NTSTATUS Bc250StartDevice(')
$last=$source.IndexOf('NTSTATUS Bc250StopDevice(',$first)
if ($first -lt 0 -or $last -lt $first) { throw 'pnp.c has no Bc250StartDevice' }
$pnp=$source.Substring($first,$last-$first)
foreach($mutation in @(@{Enabled=$NoInitialBlank;Step='StartDisplayBlank';Call='DcnBeginStartVisibility\(device,\s*&displayBlankOwned\)'},
                       @{Enabled=$NoFinalBlank;Step='StartDisplayBlankConfirm';Call='DcnSetVisibility\(device,\s*FALSE\)'})) {
    if(!$mutation.Enabled){continue}
    $step='GuardStartStep(L"'+$mutation.Step+'");'
    $at=$pnp.IndexOf($step)
    if($at -lt 0){throw "pnp.c has no $step for the blanking control"}
    $call=[regex]::Match($pnp.Substring($at),'status\s*=\s*'+$mutation.Call+';')
    if(!$call.Success){throw "pnp.c has no visibility call after $step"}
    $at+=$call.Index
    $pnp=$pnp.Remove($at,$call.Length).Insert($at,'status=STATUS_SUCCESS; /* negative control */')
}
if ($NoAttemptRecord) {
    if ($pnp -notmatch '(?m)^\s*GuardRecordStartAttempt\(\);') { throw 'pnp.c has no GuardRecordStartAttempt call to drop' }
    $pnp=$pnp -replace '(?m)^\s*GuardRecordStartAttempt\(\);',''
}
if ($RecordBeforeCleanup) {
    if ($pnp -notmatch '(?m)^\s*GuardRecordStartFailed\(status\);') { throw 'pnp.c has no GuardRecordStartFailed(status) call to move' }
    $pnp=$pnp -replace '(?m)^\s*GuardRecordStartFailed\(status\);',''
    $pnp=$pnp.Replace("failed:`n","failed:`n    GuardRecordStartFailed(status);`n")
    $pnp=$pnp.Replace("failed:`r`n","failed:`r`n    GuardRecordStartFailed(status);`r`n")
}
[IO.File]::WriteAllText((Join-Path $Out 'pnp_actual.inc'),$pnp)
# Execute the real stop admission and startup-hold release before the modeled
# hardware teardown. This catches a missing release without copying that policy
# into the mock. The remainder of StopDevice has separate teardown tests.
$stopFirst=$source.IndexOf('NTSTATUS Bc250StopDevice(')
$stopLast=$source.IndexOf('    wasStarted=device->Started;',$stopFirst)
if($stopFirst -lt 0 -or $stopLast -lt $stopFirst){throw 'pnp.c has no stop visibility prelude'}
$stop=$source.Substring($stopFirst,$stopLast-$stopFirst).Replace('NTSTATUS Bc250StopDevice(',
    'static NTSTATUS StartTestStopPrelude(')
if($KeepStopVisibilityHold){
    $release='InterlockedExchange(&device->StartVisibilityPending,0);'
    if(!$stop.Contains($release)){throw 'pnp.c has no startup visibility release in stop'}
    $stop=$stop.Replace($release,'/* negative control: startup hold kept */')
}
$stop+="    (void)wasStarted; (void)connectorStatus; return STATUS_SUCCESS;`n}`n"
[IO.File]::WriteAllText((Join-Path $Out 'stop_visibility_actual.inc'),$stop)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'start_record_test.c') -Destination (Join-Path $Out 'test.c') -Force

$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\start_record_test.exe" "$Out\test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'start record host build failed'}
& "$Out\start_record_test.exe"
exit $LASTEXITCODE

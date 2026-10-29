# Host test of wddm.c's flip path (BD-170 r7, docs/design/vidpn-flip.md section 7.2): generate_vidpn_flip_test.py
# extracts the actual source into vidpn_flip_test.c, which models L and drives each edge at PASSIVE_LEVEL and at DIRQL.
#
#   pwsh driver\kmd\test\run_vidpn_flip.ps1
#   pwsh driver\kmd\test\run_vidpn_flip.ps1 -Mutation all          (every negative control, each must fail)
#   pwsh driver\kmd\test\run_vidpn_flip.ps1 -Mutation end-in-faulting
#   pwsh driver\kmd\test\run_vidpn_flip.ps1 -Lab [-Mutation all]  (the lab flavour: the witnesses and their controls)
#
# -Mutation changes one rule in a COPY of the extracted source, never the tree, and the test must then fail with a
# FAIL line. A changed rule keeps every static function in use, so that /W4 /WX still compiles it. A mutation whose
# text does not occur exactly once stops the run instead of passing quietly. -Mutation all runs every control and fails
# if any of them passes. The binaries go under -Out; nothing is written to drive C:. -Lab compiles the extracted source
# and the fixture with AMDGPU_WDDM_LAB_FLAVOUR (BD-167's switch), which adds BD-170's witnesses; -Mutation then takes
# the lab controls only.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\m9\vidpn-flip",
    [string]$Source = '',
    [ValidateSet('', 'all', 'high-takes-section', 'low-sync-ignored', 'even-during-program', 'writer-not-a-fact',
                 'gate-closed-immediate', 'wu-faults',
                 'unproven-success', 'program-fault-success',
                 'refused-keeps-new-record', 'deposit-not-owed', 'resume-not-marked',
                 'end-ignores-otg', 'end-in-faulting', 'end-unlocked-fallback', 'invalidate-over-program',
                 'invalidate-gate-closed', 'flush-keeps-owed', 'owed-not-reported', 'fault-no-dpc', 'fault-no-reset',
                 'fault-no-blank', 'fault-notify-twice', 'node-unbounded', 'destroy-preflight-ignored',
                 'section-result-ignored', 'begin-without-bridge', 'begin-ignores-bridge-failure',
                 'begin-refusal-faults', 'retirement-failure-ignored', 'retirement-ready-too-soon', 'destroy-before-retirement',
                 'resume-unlocked-fallback', 'recovery-without-section', 'recovery-request-dropped',
                 'exclusive-refused-success', 'writer-refusal-uncounted',
                 'unsafe-next-vsync-deposit', 'unsafe-immediate-deposit',
                 'shape-not-a-change', 'null-drops-shape', 'shape-without-allocation',
                 'witness-before-decision', 'witness-changes-answer', 'witness-stale-compared',
                 'witness-lost-uncounted')][string]$Mutation = '',
    [switch]$Lab
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Source) { $Source = "$repo\driver\kmd\wddm.c" }

# Each control: the exact text of one rule in the extracted source (LF line ends), what replaces it, and the rule it
# breaks.
$mutations = [ordered]@{
    'high-takes-section' = @{ find = 'if (high)'; with = 'if (high && !high)'
        why = 'the DIRQL entry asks for a section inside L' }
    'low-sync-ignored' = @{ find = 'if (!NT_SUCCESS(section)) { InterlockedIncrement(&wddm->LowSyncFailed); return section; }'
        with = 'if (!NT_SUCCESS(section)) { InterlockedIncrement(&wddm->LowSyncFailed); }'
        why = 'a low call whose section ran nothing is answered as if it ran' }
    'even-during-program' = @{ find = "never cross it.`n    InterlockedExchange(&wddm->PrimarySequence, (LONG)(generation + 1u));"
        with = 'never cross it.'
        why = 'the generation stays even while the plane is written' }
    'writer-not-a-fact' = @{ find = 'facts.WriterActive = wddm->WriterActive ? 1u : 0u;'
        with = 'facts.WriterActive = wddm->WriterActive && !wddm->WriterActive ? 1u : 0u;'
        why = 'a request during a writer is programmed under it' }
    'gate-closed-immediate' = @{ find = 'facts.Immediate = immediate && device->VidPnFlipEnabled ? 1u : 0u;'
        with = 'facts.Immediate = immediate ? 1u : 0u;'
        why = 'with the flip gate closed an immediate request reads the plane and can be U' }
    'wu-faults' = @{ find = "    case BC250_FLIP_EDGE_DEPOSIT_UNPROVEN:`n"
        with = "    case BC250_FLIP_EDGE_DEPOSIT_UNPROVEN:`n        if (edge == BC250_FLIP_EDGE_DEPOSIT_UNPROVEN) { WddmFlipFault(device, wddm, BC250_FLIP_FAULT_UNPROVEN, WddmFlipNode(args)); flip->Result = STATUS_DEVICE_HARDWARE_ERROR; return TRUE; }`n"
        why = 'a writer conflict (W-u) enters E5 as a hardware fault' }
    'unproven-success' = @{ find = 'if (result == BC250_FLIP_RESULT_UNPROVEN)'
        with = 'if (result == BC250_FLIP_RESULT_UNPROVEN && !immediate)'
        why = 'an immediate request without P'' is answered success' }
    'program-fault-success' = @{ find = 'else if (result == BC250_FLIP_RESULT_FAULT_PROGRAM)'
        with = 'else if (result == BC250_FLIP_RESULT_FAULT_PROGRAM && !wrote)'
        why = 'a failed write sequence is published as a programmed flip' }
    'refused-keeps-new-record' = @{ find = 'InterlockedCompareExchange64(&wddm->ScanoutObject, previous, record);'
        with = '(void)previous;'
        why = 'a refusal leaves the teardown record on a buffer the plane never had' }
    'deposit-not-owed' = @{ find = 'wddm->WriterDeposit = TRUE;'; with = ''
        why = 'a software-only deposit is lost when End invalidates the old Desired' }
    'resume-not-marked' = @{ find = 'if (NT_SUCCESS(w->Status)) w->Wddm->WriterResumed = TRUE;'; with = ''
        why = 'a DRAINED readback counts after the writer resumed the plane' }
    'end-ignores-otg' = @{ find = 'DcnOtgRunning(device))'; with = 'DcnOtgRunning(device) | 1)'
        why = 'End restores an owed Desired with the OTG stopped, where no report follows' }
    'end-in-faulting' = @{ find = '(owes || restore) && device->VidPnFlipEnabled && wddm->Faulting == 0'
        with = '(owes || restore) && device->VidPnFlipEnabled'
        why = 'End programs the plane in FAULTING' }
    'end-unlocked-fallback' = @{ find = "status = WddmSection(Device, WddmWriterEndLocked, &w);`n    if (!NT_SUCCESS(status)) {"
        with = "status = WddmSection(Device, WddmWriterEndLocked, &w);`n    if (!NT_SUCCESS(status)) {`n        (void)WddmWriterEndLocked(&w);"
        why = 'a refused End performs its protected operation outside L' }
    'invalidate-over-program' = @{ find = 'if (w->Invalidate && !programmed && '
        with = 'if (w->Invalidate && (programmed || !programmed) && '
        why = 'End invalidates the Desired it has just programmed' }
    'invalidate-gate-closed' = @{ find = '!(owes && !device->VidPnFlipEnabled)'
        with = '!(owes && !device->VidPnFlipEnabled && !owes)'
        why = 'End invalidates a Desired owed to the software timer' }
    'flush-keeps-owed' = @{ find = 'w->Wddm->DesiredOwed = FALSE;'; with = ''
        why = 'the D3 flush leaves a success owed, and the D3 writers program it' }
    'owed-not-reported' = @{ find = 'InterlockedIncrement(&wddm->OwedReported);'
        with = 'InterlockedIncrement(&wddm->OwedReported); wddm->DesiredOwed = TRUE;'
        why = 'the report that carries Desired does not complete the owed success' }
    'fault-no-dpc' = @{ find = "InterlockedExchange(&Wddm->FaultNotify, 1);`n    if (Device->Dxgk.DxgkCbQueueDpc != NULL) Device->Dxgk.DxgkCbQueueDpc(Device->Dxgk.DeviceHandle);"
        with = "InterlockedExchange(&Wddm->FaultNotify, 1);`n    if (Device->Dxgk.DxgkCbQueueDpc != NULL) (void)Device->Dxgk.DeviceHandle;"
        why = 'E5 asks for no DPC, so the OS is never told' }
    'fault-no-reset' = @{ find = 'DXGK_PAGE_FAULT_FENCE_INVALID | DXGK_PAGE_FAULT_ADAPTER_RESET_REQUIRED'
        with = 'DXGK_PAGE_FAULT_FENCE_INVALID'
        why = 'E5 does not ask for the adapter reset' }
    'fault-no-blank' = @{ find = 'Wddm->FaultBlank = DcnFaultBlank(Device);'
        with = 'Wddm->FaultBlank = Device == NULL ? DcnFaultBlank(Device) : 0;'
        why = 'E5 leaves the plane fetching' }
    'fault-notify-twice' = @{ find = 'if (InterlockedExchange(&wddm->FaultNotify, 0) == 0) return;'
        with = 'if (wddm == NULL) return;'
        why = 'every DPC repeats the fault notification' }
    'node-unbounded' = @{ find = 'context != NULL && context->NodeOrdinal < BC250_WDDM_NODE_COUNT_MAX ?'
        with = 'context != NULL ?'
        why = 'E5 names a node the adapter does not have' }
    'destroy-preflight-ignored' = @{ find = 'if (!NT_SUCCESS(retired)) return retired;'; with = '(void)retired;'
        why = 'destroy frees allocations although their retirement failed' }
    'section-result-ignored' = @{ find = 'return NT_SUCCESS(status) && !returned ? STATUS_UNSUCCESSFUL : status;'
        with = 'return status;'
        why = 'a callback that ran nothing grants its protected operation' }
    'begin-without-bridge' = @{ find = 'w->Status = w->Device->VidPnFlipEnabled ?'
        with = 'w->Status = w->Device->VidPnFlipEnabled && !w->Device->VidPnFlipEnabled ?'
        why = 'Begin authorizes a long writer without retiring the old application buffer' }
    'begin-ignores-bridge-failure' = @{ find = "if (!NT_SUCCESS(w->Status)) {`n        if (w->Wrote)"
        with = "if (!NT_SUCCESS(w->Status) && NT_SUCCESS(w->Status)) {`n        if (w->Wrote)"
        why = 'Begin grants a writer after the bridge refused or failed its proof' }
    'begin-refusal-faults' = @{ find = 'if (w.Claimed) WddmWriterSectionFailure(Device, wddm, status);'
        with = 'if (!w.Claimed || w.Claimed) WddmWriterSectionFailure(Device, wddm, status);'
        why = 'a refused Begin before any write enters E5 on the next DPC' }
    'retirement-failure-ignored' = @{ find = 'else if (!NT_SUCCESS(w->Status)) {'
        with = 'else if (!NT_SUCCESS(w->Status) && NT_SUCCESS(w->Status)) {'
        why = 'an unretired old application surface is made available to a long writer' }
    'retirement-ready-too-soon' = @{ find = "wddm->WriterActive = TRUE;`n    wddm->WriterReady = FALSE;"
        with = "wddm->WriterActive = TRUE;`n    wddm->WriterReady = TRUE;"
        why = 'the long operation is authorized before bridge programming and retirement' }
    'destroy-before-retirement' = @{ find = 'if (!wddm->WriterReady || wddm->Faulting) { d->Refused = TRUE; return TRUE; }'
        with = 'if (wddm->Faulting) { d->Refused = TRUE; return TRUE; }'
        why = 'a concurrent destroy frees the application surface before retirement' }
    'resume-unlocked-fallback' = @{ find = "status = WddmSection(Device, WddmWriterResumeLocked, &w);`n    if (!NT_SUCCESS(status)) {"
        with = "status = WddmSection(Device, WddmWriterResumeLocked, &w);`n    if (!NT_SUCCESS(status)) {`n        (void)WddmWriterResumeLocked(&w);"
        why = 'a refused Resume changes writer state outside L' }
    'recovery-without-section' = @{ find = '(void)WddmSection(Device, WddmWriterRecoveryLocked, Device);'
        with = '(void)WddmWriterRecoveryLocked(Device);'
        why = 'the DPC enters FAULTING and writes the plane without acquiring L' }
    'recovery-request-dropped' = @{ find = 'InterlockedExchange(&Wddm->WriterRecovery, 1);'
        with = 'InterlockedExchange(&Wddm->WriterRecovery, 0);'
        why = 'a refused writer section never reaches recovery at the next acquired section' }
    'exclusive-refused-success' = @{ find = 'if (exclusive && immediate) { InterlockedIncrement(&wddm->ExclusiveRefused); flip->Result = status; }'
        with = 'if (exclusive && immediate) { InterlockedIncrement(&wddm->ExclusiveRefused); flip->Result = STATUS_SUCCESS; }'
        why = 'an immediate refusal acknowledges a surface that the writer never accepted' }
    'writer-refusal-uncounted' = @{ find = "        if (w.Answer == BC250_WRITER_BEGIN_REFUSED) {`n            WddmWriterRefused(wddm, &w, BC250_WRITER_REFUSED_BRIDGE, w.Status);"
        with = "        if (w.Answer == BC250_WRITER_BEGIN_REFUSED) {`n            (void)BC250_WRITER_REFUSED_BRIDGE;"
        why = 'a refused bridge admission (BD-195) leaves no count and no line, only the caller''s BUSY' }
    'unsafe-next-vsync-deposit' = @{ find = "if (device->VidPnFlipEnabled) {`n            InterlockedIncrement(&wddm->WriterBusy);"
        with = "if (device->VidPnFlipEnabled && immediate) {`n            InterlockedIncrement(&wddm->WriterBusy);"
        why = 'a next-VSync request is accepted although the active writer can miss its target retrace' }
    'unsafe-immediate-deposit' = @{ find = "if (device->VidPnFlipEnabled) {`n            InterlockedIncrement(&wddm->WriterBusy);"
        with = "if (device->VidPnFlipEnabled && !immediate) {`n            InterlockedIncrement(&wddm->WriterBusy);"
        why = 'an immediate request is accepted without programming its requested surface in the call' }
    # The call stays and its answer is dropped: without a call the host mock is an unused function, and clang -Werror
    # refuses the build instead of running the mutant.
    'shape-not-a-change' = @{ find = ' || !DcnPlaneShapeIs(device, shape);'; with = ' || (DcnPlaneShapeIs(device, shape), FALSE);'
        why = 'a flip to the same address in another pipe shape (BD-223) writes nothing' }
    'null-drops-shape' = @{ find = 'DcnPlaneShapeCurrent(device, &kept)) shape = &kept;'
        with = 'DcnPlaneShapeCurrent(device, &kept)) shape = NULL;'
        why = 'a flip with no allocation drops the shape of the plane and reads past its smaller surface' }
    'shape-without-allocation' = @{ find = 'if (flip->Shape && (args->hAllocation == NULL || !device->VidPnFlipEnabled)) status = STATUS_NOT_SUPPORTED;'
        with = ''
        why = 'a shape is programmed with no allocation to hold it to' }
}

# The lab flavour's controls (-Lab): the witnesses read after the shipping decision and never change it.
$labMutations = [ordered]@{
    'witness-before-decision' = @{ find = "            outcome = DcnImmediateObserve(device, (ULONGLONG)address, physical, programStart, programEnd,"
        with = "            WddmWitness(device, wddm, &witnessArm, args, physical, generation + 1u, outcome, BC250_FLIP_RESULTS);`n            outcome = DcnImmediateObserve(device, (ULONGLONG)address, physical, programStart, programEnd,"
        why = 'the witnesses are read before the shipping decision' }
    'witness-changes-answer' = @{ find = '            WddmWitness(device, wddm, &witnessArm, args, physical, generation + 1u, outcome, result);'
        with = '            { WddmWitness(device, wddm, &witnessArm, args, physical, generation + 1u, outcome, result); if (!wddm->WitnessLast.E) result = BC250_FLIP_RESULT_UNPROVEN; }'
        why = 'a witness changes the answer of the shipping decision' }
    'witness-stale-compared' = @{ find = 'if (r.Fresh && r.Q != r.E)'; with = 'if (r.Q != r.E)'
        why = 'an event the clear did not reset is compared with Q' }
    'witness-lost-uncounted' = @{ find = 'else InterlockedIncrement(&Wddm->WitnessLost);'; with = 'else (void)0;'
        why = 'a disagreement past the record bound is neither kept nor counted' }
}
$flavour = @()
if ($Lab) { $flavour = @('/DAMDGPU_WDDM_LAB_FLAVOUR=1'); $mutations = $labMutations }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $Root 'toolchain\llvm-23.1.4-win64\bin\clang-cl.exe'
& $cl --version
if ($LASTEXITCODE) { throw 'Explicit clang-cl unavailable' }
$sdk = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = "$Root\scratch\tmp"; $env:TMP = $env:TEMP
& python "$PSScriptRoot\generate_vidpn_flip_test.py" --source $Source --out "$Out\vidpn_flip_actual.c"
if ($LASTEXITCODE -ne 0) { throw 'Source extraction failed' }

# Builds and runs the test from the extracted file $Actual; returns the exit code and the output.
# /wd4201: the fixture declares DXGK_SETVIDPNSOURCEADDRESS_FLAGS, DXGK_FAULT_ERROR_CODE and the notification's union
# the way the WDK declares them, nameless, because the extracted driver source reads them that way. That is the same
# nonstandard extension driver\kmd\build.ps1 turns off for the WDK's own headers, and for the same reason.
function Invoke-VidPnFlip([string]$Dir, [string]$Actual) {
    $obj = Join-Path $Dir 'obj'
    New-Item -ItemType Directory -Force $Dir, $obj | Out-Null
    Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue
    # Intentional rule removal uses x && !x to keep variables in use. Require
    # runtime rejection of these mutants, not a compiler-warning substitute.
    [string[]]$mutationWarnings = @()
    if ($Mutation) { $mutationWarnings = @('-Wno-tautological-negation-compare') }
    & $cl @('/nologo', '/TC', '/W4', '/WX', '/wd4201', '/O2', '/MT', '-fuse-ld=lld') @mutationWarnings @flavour @("/I$repo\driver\kmd", "/I$repo\third_party\linux-amdgpu",
        "/I$($msvc.FullName)\include", "/I$sdk\Include\10.0.26100.0\ucrt", "/Fo$obj\", "/Fe$Dir\vidpn_flip_test.exe", $Actual,
        "$repo\driver\kmd\dcn_translate.c", '/link', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$libs\ucrt\x64",
        "/LIBPATH:$libs\um\x64") | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Host build failed ($Dir)" }
    $output = & "$Dir\vidpn_flip_test.exe"
    return @{ Code = $LASTEXITCODE; Output = ($output -join "`n") }
}

if (-not $Mutation) {
    $run = Invoke-VidPnFlip $Out "$Out\vidpn_flip_actual.c"
    Write-Output $run.Output
    exit $run.Code
}

$names = if ($Mutation -eq 'all') { @($mutations.Keys) } else { @($Mutation) }
$actual = [IO.File]::ReadAllText("$Out\vidpn_flip_actual.c")
$passed = @()
foreach ($name in $names) {
    $m = $mutations[$name]
    $count = ([regex]::Matches($actual, [regex]::Escape($m.find))).Count
    if ($count -ne 1) { throw "mutation ${name}: its text occurs $count times in the extracted source, not once" }
    $dir = Join-Path $Out "mutation-$name"
    New-Item -ItemType Directory -Force $dir | Out-Null
    [IO.File]::WriteAllText("$dir\vidpn_flip_actual.c", $actual.Replace($m.find, $m.with))
    $run = Invoke-VidPnFlip $dir "$dir\vidpn_flip_actual.c"
    $failed = $run.Code -ne 0 -and $run.Output -match 'FAIL line'
    Write-Output ("mutation {0}: {1} ({2})" -f $name, $(if ($failed) { 'caught' } else { 'NOT CAUGHT' }), $m.why)
    if (-not $failed) { $passed += $name }
}
if ($passed.Count) { Write-Output "mutations not caught: $($passed -join ', ')"; exit 1 }
Write-Output "vidpn flip mutations: $($names.Count) caught"
exit 0

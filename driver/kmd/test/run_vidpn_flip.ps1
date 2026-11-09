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
                 'mpo3-report-off', 'mpo3-report-type', 'mpo3-legacy-kept', 'mpo3-refused-tracked', 'mpo3-not-converted',
                 'mpo3-keep-unlocked', 'mpo3-immediate-lost', 'mpo3-extra-layer-shown', 'mpo3-check-scanout-skipped',
                 'mpo3-check-gate-skipped', 'mpo3-busy-passed', 'scaled-release-enabled',
                 'witness-before-decision', 'witness-changes-answer', 'witness-stale-compared',
                 'witness-lost-uncounted', 'scaled-plane-mask-dropped', 'planes-clamp-dropped',
                 'mpo3-scaled-route-off', 'mpo3-scaled-dml-skipped', 'mpo3-scaled-scanout-skipped',
                 'mpo3-scaled-rule-unnamed', 'mpo3-scaled-shape-lost', 'mpo3-scaled-approval-skipped',
                 'mpo3-scaled-selector-skipped', 'mpo3-scaled-presentid-lost',
                 'group-colour-swapped', 'group-umd-nv12', 'group-flags-ignored', 'group-rgb-colour-ignored',
                 'group-post-ignored', 'group-source-ignored')][string]$Mutation = '',
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
    # BD-307: plane 0 of the MPO3 DDIs.
    'mpo3-report-off' = @{ find = 'if (!WddmMpo3Notify(notify->Device, &notify->Data)) WddmNotifyCall(notify->Device, &notify->Data);'
        with = 'if (((void)WddmMpo3Notify, 1)) WddmNotifyCall(notify->Device, &notify->Data);'
        why = 'an MPO3 flip is reported by its address, which does not retire it' }
    'mpo3-report-type' = @{ find = 'mpo.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2;'
        with = 'mpo.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;'
        why = 'the PresentId report goes out under the CRTC_VSYNC type' }
    'mpo3-legacy-kept' = @{ find = '            Bc250Mpo3Legacy(&flip->Wddm->Mpo3);'; with = '            (void)0;'
        why = 'a legacy flip leaves the MPO3 report on' }
    'mpo3-refused-tracked' = @{ find = '    if (NT_SUCCESS(flip->Result))'; with = '    if (((void)flip->Result, 1))'
        why = 'the PresentId of a flip the transaction refused is reported' }
    'mpo3-not-converted' = @{ find = '        if (!plane->InputFlags.FlipImmediate) { plane->OutputFlags.FlipConvertedToImmediate = 1;'
        with = '        if (((void)plane, 0)) { plane->OutputFlags.FlipConvertedToImmediate = 1;'
        why = 'a next-VSync disable completes on return without saying so' }
    'mpo3-keep-unlocked' = @{ find = '    section = WddmSection(Device, WddmMpo3KeepLocked, &keep);'
        with = '    section = ((void)Device, (void)WddmMpo3KeepLocked(&keep), STATUS_SUCCESS);'
        why = 'the tracker is written outside L at low IRQL' }
    'mpo3-immediate-lost' = @{ find = '    flip.Flags.FlipImmediate = plane->InputFlags.FlipImmediate ? 1 : 0;'
        with = '    flip.Flags.FlipImmediate = plane->InputFlags.FlipImmediate ? 0 : 0;'
        why = 'an immediate MPO3 flip is programmed for the next VSync' }
    'mpo3-busy-passed' = @{ find = '    if (status == STATUS_DEVICE_BUSY) { InterlockedIncrement(&wddm->Mpo3Retries); status = STATUS_RETRY; }'
        with = '    if (((void)0, 0)) { InterlockedIncrement(&wddm->Mpo3Retries); status = STATUS_RETRY; }'
        why = 'an MPO3 flip refused during a writer returns STATUS_DEVICE_BUSY, outside the return set of the DDI' }
    'mpo3-extra-layer-shown' = @{ find = '        if (p->InputFlags.Enabled) { InterlockedIncrement(&wddm->Mpo3Refused); return STATUS_INVALID_PARAMETER; }'
        with = ''
        why = 'an enabled plane of another layer is answered as shown' }
    'mpo3-check-scanout-skipped' = @{ find = 'if (scanout != BC250_SCANOUT_ADMIT_OK) verdict = BC250_MPO3_SCANOUT;'
        with = 'if (((void)scanout, 0)) verdict = BC250_MPO3_SCANOUT;'
        why = 'Check approves an allocation the scan-out rule refuses' }
    'mpo3-check-gate-skipped' = @{ find = 'if (verdict == BC250_MPO3_OK && !device->VidPnFlipEnabled) verdict = BC250_MPO3_GATE;'
        with = 'if (((void)device, 0)) verdict = BC250_MPO3_GATE;'
        why = 'Check approves a plane with the flip gate closed' }
    'scaled-release-enabled' = @{ find = "    UNREFERENCED_PARAMETER(Raw);`n    UNREFERENCED_PARAMETER(Handshake);`n    return 0;"
        with = '    return Handshake ? Raw : 0;'
        why = 'the release start admits a raw selector before hardware acceptance' }
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
    'scaled-plane-mask-dropped' = @{ find = '    const ULONG scaled = Raw & BC250_WDDM_SCALED_PLANE_MASK;'; with = '    const ULONG scaled = Raw;'
        why = 'WddmStart stores an unknown LabScaledPlane bit (the mask of WddmScaledPlaneLatch is gone)' }
    'planes-clamp-dropped' = @{ find = '(ScaledPlane & BC250_WDDM_SCALED_PLANE_TWO_PLANES) && Level < BC250_WDDM_MPO_CAPS_DDI_LEVEL'
        with = '(ScaledPlane & BC250_WDDM_SCALED_PLANE_TWO_PLANES) && (Level || 1)'
        why = 'DRIVERCAPS says 2 planes at level 2.2 or later, where the caps DDI says 1' }
    # BD-223 Part Y: the translation of the two planes of a Check3 call (WddmMpo3GroupDescribe, lab flavour).
    'group-colour-swapped' = @{ find = 'case D3DDDI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601: out->Color = BC250_INPUT_601_LIMITED; break;'
        with = 'case D3DDDI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601: out->Color = BC250_INPUT_601_FULL; break;'
        why = 'a studio-range 601 plane is described as full range' }
    'group-umd-nv12' = @{ find = 'if (s->Format == BC250_NV12_D3DDDI_FORMAT && !Storage[i].UmdAlloc &&'
        with = 'if (s->Format == BC250_NV12_D3DDDI_FORMAT && ((void)Storage[i].UmdAlloc, 1) &&'
        why = 'a UMD allocation is described as an NV12 plane' }
    'group-flags-ignored' = @{ find = 'out->UnsupportedFlags = a->Flags.Value || a->Blend.Value ||'
        with = 'out->UnsupportedFlags = ((void)a->Flags.Value, 0) || a->Blend.Value ||'
        why = 'a mirrored plane reaches the planner without its exclusion' }
    'group-rgb-colour-ignored' = @{ find = 'if ((UINT)a->ColorSpaceType != D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {'
        with = 'if (((void)a->ColorSpaceType, 0)) {'
        why = 'an RGB plane with a YCbCr colour space is described as full-range RGB' }
    'group-post-ignored' = @{ find = 'Description->PostCompositionChanged |= facts.PostPresent && (facts.PostFlags ||'
        with = 'Description->PostCompositionChanged |= 0 && facts.PostPresent && (facts.PostFlags ||'
        why = 'a mirrored post-composition is not reported to the planner' }
    'group-source-ignored' = @{ find = 'if (p->VidPnSourceId != 0) { RtlZeroMemory(Description, sizeof(*Description)); return STATUS_INVALID_PARAMETER; }'
        with = 'if (((void)p->VidPnSourceId, 0)) { RtlZeroMemory(Description, sizeof(*Description)); return STATUS_INVALID_PARAMETER; }'
        why = 'a plane of another source is described' }}
# BD-223: both flavours compile and test the same Check3 and SetAddress3 implementation.
$planeMutations = [ordered]@{
    'mpo3-scaled-route-off' = @{ find = 'scaled = verdict == BC250_MPO3_OK ? 1u : 0u;'; with = 'scaled = ((void)verdict, 0u);'
        why = 'with the lab bit, a scaled plane 0 never reaches the scaled-plane rule' }
    'mpo3-scaled-dml-skipped' = @{ find = 'if (verdict == BC250_PLANE_OK && !plan.Identity) { verdict = WddmPlaneDml(Device, &r, &plan, &dml); dmlRan = TRUE; }'
        with = 'if (((void)plan, 0)) { verdict = WddmPlaneDml(Device, &r, &plan, &dml); dmlRan = TRUE; }'
        why = 'Check3 approves a scaled plane that DML never fitted into the clocks and the global sync' }
    'mpo3-scaled-scanout-skipped' = @{ find = 'if (verdict == BC250_PLANE_OK && *Scanout != BC250_SCANOUT_ADMIT_OK) verdict = BC250_PLANE_SCANOUT;'
        with = 'if (((void)*Scanout, 0)) verdict = BC250_PLANE_SCANOUT;'
        why = 'Check3 approves a scaled plane whose allocation the scan-out rule refuses' }
    'mpo3-scaled-rule-unnamed' = @{ find = '            verdict = Bc250Mpo3PlaneRule(&plane, 1u);'
        with = '            verdict = Bc250Mpo3PlaneRule(&plane, 1u) == BC250_MPO3_OK ? BC250_MPO3_OK : BC250_MPO3_GEOMETRY;'
        why = 'a rotated or blended plane of another geometry is counted as a geometry refusal' }
    'mpo3-scaled-shape-lost' = @{ find = 'status = WddmFlipEntry(hAdapter, &flip, requestedShape, TRUE, plane->PresentId);'
        with = 'status = WddmFlipEntry(hAdapter, &flip, ((void)requestedShape, NULL), TRUE, plane->PresentId);'
        why = 'the approved scaler plan never reaches the flip transaction' }
    'mpo3-scaled-approval-skipped' = @{ find = 'else if (DcnPlaneApproved(device, &plan.Scaler, r.SurfacePitch)) {'
        with = 'else if (DcnPlaneApproved(device, &plan.Scaler, r.SurfacePitch) || 1) {'
        why = 'an unapproved shape, pitch or stale mode reaches the plane' }
    'mpo3-scaled-selector-skipped' = @{ find = "data->hAllocation != NULL &&`n        (wddm->ScaledPlane & BC250_WDDM_SCALED_PLANE_CHECK))"
        with = "data->hAllocation != NULL &&`n        ((wddm->ScaledPlane & BC250_WDDM_SCALED_PLANE_CHECK) || 1))"
        why = 'the scaled path takes a flip with its selector off' }
    'mpo3-scaled-presentid-lost' = @{ find = 'status = WddmFlipEntry(hAdapter, &flip, requestedShape, TRUE, plane->PresentId);'
        with = 'status = WddmFlipEntry(hAdapter, &flip, requestedShape, TRUE, 0);'
        why = 'the scaled flip reports another PresentId instead of the one Windows queued' }
}
$flavour = @('-Wno-multichar', "/I$repo\driver\kmd\test", "/I$repo\driver\shim\dml201")
# Both flavours link the actual scaled-plane rule and scaler tables.
[string[]]$planeSources = @("$repo\driver\kmd\dcn_plane.c", "$repo\driver\kmd\dcn_scale.c", "$repo\driver\shim\bc250_scaler_buffer.c",
                          "$repo\driver\shim\bc250_scaler_filter.c", "$repo\driver\kmd\display_io.c", "$repo\driver\kmd\edid.c")
if ($Lab) {
    $flavour += '/DAMDGPU_WDDM_LAB_FLAVOUR=1'
    $mutations = $labMutations
}
foreach ($name in $planeMutations.Keys) { $mutations[$name] = $planeMutations[$name] }

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
    & $cl @('/nologo', '/TC', '/W4', '/WX', '/wd4201', '/O2', '/MT', '-fuse-ld=lld') @mutationWarnings @flavour @("/I$repo\driver\kmd", "/I$repo\driver\contract", "/I$repo\third_party\linux-amdgpu",
        "/I$($msvc.FullName)\include", "/I$sdk\Include\10.0.26100.0\ucrt", "/Fo$obj\", "/Fe$Dir\vidpn_flip_test.exe", $Actual,
        "$repo\driver\kmd\dcn_translate.c") @planeSources @('/link', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$libs\ucrt\x64",
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
    $failLines = @($run.Output -split "`n" | Where-Object { $_ -match '^FAIL line' }).Count
    Write-Output ("mutation {0}: {1}, {2} FAIL lines ({3})" -f $name, $(if ($failed) { 'caught' } else { 'NOT CAUGHT' }), $failLines, $m.why)
    if (-not $failed) { $passed += $name }
}
if ($passed.Count) { Write-Output "mutations not caught: $($passed -join ', ')"; exit 1 }
Write-Output "vidpn flip mutations: $($names.Count) caught"
exit 0

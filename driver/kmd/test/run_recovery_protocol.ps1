param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\build\recovery-protocol",
    [switch]$IgnoreReportInFlight, [switch]$IgnoreLostReport,
    [switch]$UseNotifiedAsCompleted, [switch]$ShareNodeWatermark,
    [switch]$ResetWritesCompleted, [switch]$IgnoreEpoch, [switch]$TimeoutAfterUnlock,
    [switch]$ReadAfterRelease, [switch]$KeepCoveredBoundary, [switch]$DropAdmissionReason, [switch]$RejectActiveImmediately, [switch]$IgnoreDrainDeadline, [switch]$FaultHealthOnAdmission, [switch]$AdmitDuringReset
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP = Join-Path $Out 'tmp'
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
function Function-Body([string]$Source, [string]$Name) {
    $match = [regex]::Match($Source, '\b' + [regex]::Escape($Name) + '\s*\([^;{}]*\)\s*\{')
    if (-not $match.Success) { throw "missing production function $Name" }
    $open = $match.Index + $match.Length - 1
    $depth = 1; $end = $open + 1
    while ($depth -and $end -lt $Source.Length) {
        if ($Source[$end] -eq '{') { $depth++ }
        if ($Source[$end] -eq '}') { $depth-- }
        $end++
    }
    if ($depth) { throw "unclosed production function $Name" }
    return @{ Start=$match.Index; Open=$open; End=$end; Text=$Source.Substring($match.Index,$end-$match.Index) }
}
function Insert-Entry([string]$Source, [string]$Name, [string]$Code) {
    $f = Function-Body $Source $Name
    return $Source.Insert($f.Open + 1, "`n$Code`n")
}
$header = Get-Content "$repo\driver\kmd\hang_recovery.h" -Raw
$source = Get-Content "$repo\driver\kmd\wddm.c" -Raw
$gfxSource = Get-Content "$repo\driver\kmd\gfx.c" -Raw
# Controls alter only copied production code; every one must compile then fail a CHECK.
# Mutation parameter spellings and integration anchors below are pinned to the reviewed production helpers.
if ($IgnoreReportInFlight) {
    $header = $header.Replace('!state->CompletedKnown || state->ReportInFlight || state->ReportLost', '!state->CompletedKnown || state->ReportLost')
}
if ($IgnoreLostReport) {
    $header = $header.Replace('!state->CompletedKnown || state->ReportInFlight || state->ReportLost', '!state->CompletedKnown || state->ReportInFlight')
}
if ($UseNotifiedAsCompleted) {
    $header = Insert-Entry $header 'Bc250HangAbortCompletedFence' @'
    BC250_HANG_NODE_STATE counterfeit = *state;
    counterfeit.CompletedKnown = lastReportedKnown;
    counterfeit.CompletedFence = lastReported;
    state = &counterfeit;
'@
}
if ($ResetWritesCompleted) {
    $header = Insert-Entry $header 'Bc250HangResetEnd' '    if (success) Bc250HangObserveCompleted(state, boundary);'
}
if ($IgnoreEpoch) {
    $header = $header.Replace('return !state->ResetActive && state->Epoch == epoch;', '(void)epoch; return !state->ResetActive;')
}
if ($KeepCoveredBoundary) {
    $header = $header.Replace('state->BoundaryKnown = 0;', 'state->BoundaryKnown = 1;')
}
if ($DropAdmissionReason) {
    $live='state->ReportLost ? BC250_HANG_ADMISSION_REPORT_LOST : 0u'
    if (!$header.Contains($live)) { throw 'admission reason mutation anchor changed' }
    $header=$header.Replace($live,'state->ReportLost ? 0u : 0u')
}
[IO.File]::WriteAllText((Join-Path $Out 'hang_recovery.h'), $header)
$latch = 'static BOOLEAN ' + (Function-Body $source 'WddmLatchSubmitTimeoutLocked').Text
$mock = @'
typedef int BOOLEAN;
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef unsigned UINT;
typedef long LONG;
typedef int KIRQL;
typedef struct { long long QuadPart; } LARGE_INTEGER;
#define TRUE 1
#define FALSE 0
#define KernelMode 0
#define STATUS_NOT_SUPPORTED 1
#define BC250_WDDM_NODE_3D 0
#define BC250_SUBMIT_FENCE_SLOT 0
typedef struct { void *SubmitAdev; LONG SubmitFailed; ULONG SubmitSeq; } BC250_GFX;
typedef struct { int GfxClosed; int Calls; void *Gfx; int HealthFault; unsigned Wakes, DrainWakes; } BC250_DEVICE;
typedef struct {
    int Stopping, Lock, SubmitTimer;
    BC250_HANG_NODE_STATE Recovery[2];
    int WatchdogFaulted[2], DeferredValid, CompletionPending[2], ActiveSubmissions[2];
    unsigned NoteCalls, NotedFence[2];
} BC250_WDDM;
static BC250_WDDM *MockWddm;
static BC250_DEVICE *MockDevice;
static ULONGLONG MockNow, MockStart;
static unsigned MockDrainMs, MockSecondaryMs, MockFaultMs, MockWaits, MockLockDepth, MockSafetyTrips, MockPoisonRelease;
static void *GfxAccessAcquire(BC250_DEVICE *d) { return d->Gfx; }
static void GfxAccessRelease(BC250_DEVICE *d) {
    if (MockPoisonRelease && d->Gfx) ((BC250_GFX *)d->Gfx)->SubmitAdev = NULL;
}
static ULONG bc250_gfx_fence_read(void *adev, int slot) { (void)adev; (void)slot; return 123; }
static LONG InterlockedExchange(LONG *p, LONG value) { LONG old = *p; *p = value; return old; }
static void GuardLog(const char *format, ...) { (void)format; }
static void StartHealthFault(BC250_DEVICE *d) { d->HealthFault = 1; d->Calls++; }
static void StartHealthClose(BC250_DEVICE *d) { StartHealthFault(d); }
static void GfxRetireSignal(BC250_DEVICE *d) {
    d->Wakes++;
    d->GfxClosed = d->Gfx && ((BC250_GFX *)d->Gfx)->SubmitFailed;
    if (!d->GfxClosed) d->DrainWakes++;
}
static void KeAcquireSpinLock(int *lock, KIRQL *irql) {
    (void)lock; CHECK(MockLockDepth == 0); *irql = 0; MockLockDepth++;
}
static void KeReleaseSpinLock(int *lock, KIRQL irql) {
    (void)lock; CHECK(irql == 0 && MockLockDepth == 1); MockLockDepth--;
}
static int KeCancelTimer(int *timer) { CHECK(MockLockDepth == 1); *timer = 0; return 0; }
static ULONGLONG KeQueryInterruptTime(void) { return MockNow; }
static void GfxSubmitFail(BC250_DEVICE *d);
static int KeDelayExecutionThread(int mode, int alertable, LARGE_INTEGER *delay) {
    CHECK(MockLockDepth == 0 && mode == KernelMode && !alertable && delay->QuadPart == -10000ll);
    MockWaits++;
    MockNow += 10000ull;
    if ((MockNow - MockStart)/10000ull >= MockFaultMs) {
        GfxSubmitFail(MockDevice); MockFaultMs = ~0u;
    }
    if ((MockNow - MockStart)/10000ull >= MockDrainMs) MockWddm->ActiveSubmissions[0] = 0;
    if ((MockNow - MockStart)/10000ull >= MockSecondaryMs) MockWddm->Recovery[0].ReportLost = 1;
    if (MockWaits >= 600) { MockSafetyTrips++; MockWddm->Stopping = 1; }
    return 0;
}
static void WddmNoteSubmittedLocked(BC250_WDDM *w, unsigned node, UINT fence) {
    w->NoteCalls++; w->NotedFence[node] = fence;
}
'@
# Extract actual closure and health-fault wrapper, not a mock of the distinction.
foreach($name in @('GfxSubmitFailAccess','GfxSubmitClose','GfxSubmitFail')) {
    $body=(Function-Body $gfxSource $name).Text
    $body=$body.Replace('_Inout_ ','').Replace('_In_ ','')
    $mock += "`nstatic void $body`n"
}
$mock += "`n$latch`n"
$observed = 'static BOOLEAN ' + (Function-Body $gfxSource 'GfxFenceObserved').Text
$observed = $observed.Replace('_Inout_ ', '').Replace('_Out_ ', '')
if ($ReadAfterRelease) {
    if (!$observed.Contains('return observed;')) { throw 'GfxFenceObserved return anchor changed' }
    $observed = $observed.Replace('return observed;', 'return gfx != NULL && gfx->SubmitAdev != NULL;')
}
$mock += "`n$observed`n"
$admit = 'static ULONG ' + (Function-Body $source 'WddmResetAdmit').Text
$begin = 'static BOOLEAN ' + (Function-Body $source 'WddmBeginSubmissionLocked').Text
if ($FaultHealthOnAdmission) {
    if (!$admit.Contains('GfxSubmitClose(Device);')) { throw 'admission closure anchor changed' }
    $admit=$admit.Replace('GfxSubmitClose(Device);','GfxSubmitFail(Device);')
}
if ($AdmitDuringReset) {
    $live='if (Wddm->Recovery[Node].ResetActive) return FALSE;'
    if (!$begin.Contains($live)) { throw 'submit freeze anchor changed' }
    $begin=$begin.Replace($live,'if (0 && Wddm->Recovery[Node].ResetActive) return FALSE;')
}
if ($RejectActiveImmediately) {
    $live='reasons != BC250_HANG_ADMISSION_ACTIVE_SUBMISSIONS ||'
    if (!$admit.Contains($live)) { throw 'active drain mutation anchor changed' }
    $admit=$admit.Replace($live,'reasons != 0 ||')
}
if ($IgnoreDrainDeadline) {
    $live='KeQueryInterruptTime() >= deadline'
    if (!$admit.Contains($live)) { throw 'drain deadline mutation anchor changed' }
    $admit=$admit.Replace($live,'(KeQueryInterruptTime() >= deadline && 0)')
}
$mock += "`n$admit`n$begin`n"
$mock += @'
static void recover(BC250_DEVICE *d, BC250_WDDM *w)
{
    CHECK(Bc250HangResetBegin(&w->Recovery[0], 0));
    w->WatchdogFaulted[0] = 0;
    d->GfxClosed = 0;
    ((BC250_GFX *)d->Gfx)->SubmitFailed = 0;
    Bc250HangResetEnd(&w->Recovery[0], 1, 20);
}
static void test_timeout_latch(void)
{
    BC250_DEVICE d = {0};
    BC250_GFX gfx = {0};
    BC250_WDDM w = {0};
    ULONGLONG observed = w.Recovery[0].Epoch;
    d.Gfx = &gfx;
    w.DeferredValid = 1;
    CHECK(WddmLatchSubmitTimeoutLocked(&d, &w, observed));
    CHECK(d.GfxClosed && w.WatchdogFaulted[0] && !w.DeferredValid && d.Calls == 1);
    recover(&d, &w);
#ifdef TIMEOUT_AFTER_UNLOCK
    GfxSubmitFail(&d);
#endif
    CHECK(!d.GfxClosed && !w.WatchdogFaulted[0]);
    CHECK(!WddmLatchSubmitTimeoutLocked(&d, &w, observed));
    CHECK(!d.GfxClosed && !w.WatchdogFaulted[0] && d.Calls == 1);
    /* Opposite order: reset completed before the stale action tries to commit. */
    observed = w.Recovery[0].Epoch;
    recover(&d, &w);
    CHECK(!WddmLatchSubmitTimeoutLocked(&d, &w, observed));
    CHECK(!d.GfxClosed && !w.WatchdogFaulted[0] && d.Calls == 1);
    CHECK(Bc250HangResetBegin(&w.Recovery[0], 0));
    CHECK(!WddmLatchSubmitTimeoutLocked(&d, &w, w.Recovery[0].Epoch));
    Bc250HangResetEnd(&w.Recovery[0], 0, 0);
    w.Stopping = 1;
    CHECK(!WddmLatchSubmitTimeoutLocked(&d, &w, w.Recovery[0].Epoch));
    CHECK(d.Calls == 1);
    w.Stopping = 0;
    CHECK(WddmLatchSubmitTimeoutLocked(&d, &w, w.Recovery[0].Epoch));
    CHECK(d.Calls == 2 && d.GfxClosed && w.WatchdogFaulted[0]);
    {
        BC250_GFX g;
        ULONG value = 0;
        memset(&g, 0, sizeof(g));
        MockPoisonRelease = 1;
        g.SubmitAdev = &g;
        d.Gfx = &g;
        CHECK(GfxFenceObserved(&d, &value));
        CHECK(value == 123 && g.SubmitAdev == NULL);
        CHECK(!GfxFenceObserved(&d, &value) && value == 0);
        d.Gfx = NULL;
        CHECK(!GfxFenceObserved(&d, &value) && value == 0);
    }
    CHECK(PRODUCTION_SOURCE_PROTOCOL);
}
'@
# Integration shapes supplement the production-helper interleavings. False becomes a runtime CHECK.
$dpc = (Function-Body $source 'WddmSubmitDpcCheck').Text
$reset = (Function-Body $source 'Bc250WddmResetEngine').Text
$sourceOk = $true
$tailAt = $dpc.IndexOf('if (!timedOut) return;')
if ($tailAt -lt 0 -or $dpc.Substring($tailAt).Contains('GfxSubmitFail(')) { $sourceOk = $false }
$reopen = $reset.LastIndexOf('GfxReopenAfterAbort(device)')
if ($reopen -lt 0) { $sourceOk = $false }
else {
    $acquire = $reset.LastIndexOf('KeAcquireSpinLock(', $reopen)
    $release = $reset.LastIndexOf('KeReleaseSpinLock(', $reopen)
    if ($acquire -lt 0 -or $release -gt $acquire) { $sourceOk = $false }
}
if ($latch.Contains('KeFlushQueuedDpcs') -or $latch.Contains('KeWaitFor')) { $sourceOk = $false }
$soft = (Function-Body $gfxSource 'GfxSoftRecover').Text
if ($soft.Contains('GfxRetireSignal(') -or $soft.Contains('InterlockedExchange(&gfx->SubmitFailed, 0)')) { $sourceOk = $false }
$record = (Function-Body $source 'WddmRecordCompletionLocked').Text
if ($record.IndexOf('Bc250HangObserveCompleted(') -lt 0 -or
    $record.IndexOf('Bc250HangObserveCompleted(') -gt $record.IndexOf('Wddm->CompletionPending[NodeOrdinal] = 1;')) { $sourceOk = $false }
$admissionOk=$true
if (!$reset.Contains('GuardRecordHangRecovery(BC250_HANG_VERDICT_ADMISSION_GUARD, reasons, 0, 0, 0);')) { $admissionOk=$false }
if (!$reset.Contains('reasons = WddmResetAdmit(device, wddm, node, &irql);')) { $admissionOk=$false }
foreach($name in @('Bc250WddmSubmitCommand','Bc250WddmSubmitCommandVirtual')) {
    $wrapper=(Function-Body $source $name).Text
    $entry=$wrapper.IndexOf('admitted = WddmBeginSubmissionLocked(')
    $frozen=[regex]::Match($wrapper,'if \(!admitted\)\s*\{\s*ProgressExit\([^;]+;\s*return STATUS_SUCCESS;\s*\}')
    $impl=$wrapper.IndexOf('status = '+$name+'Impl(')
    if ($entry -lt 0 -or !$frozen.Success -or $entry -gt $frozen.Index -or $frozen.Index -gt $impl) { $admissionOk=$false }
}
$tailAt=$reset.IndexOf('refuseReset:')
if ($tailAt -lt 0) { throw 'refusal tail missing' }
$tail=$reset.Substring($tailAt+'refuseReset:'.Length)
if (!$tail.Contains('StartHealthClose(device);') -or $tail.Contains('GfxSubmitClose(') -or $tail.Contains('GfxReopenAfterAbort(')) { $admissionOk=$false }
$mock += @'
static int run_refusal_tail(BC250_DEVICE *device)
{
    BC250_DEVICE *hAdapter = device;
    struct { UINT NodeOrdinal, EngineOrdinal; } args = {0, 0}, *pResetEngine = &args;
'@
$mock += $tail
$mock = '#define PRODUCTION_ADMISSION_PROTOCOL ' + [int]$admissionOk + "`n" + $mock
$mock = '#define PRODUCTION_SOURCE_PROTOCOL ' + [int]$sourceOk + "`n" + $mock
[IO.File]::WriteAllText((Join-Path $Out 'timeout_latch.inc'), $mock)
Copy-Item "$PSScriptRoot\recovery_protocol_test.c" "$Out\recovery_protocol_test.c" -Force
$defines = @()
if ($ShareNodeWatermark) { $defines += '/DSHARE_NODE_WATERMARK' }
if ($TimeoutAfterUnlock) { $defines += '/DTIMEOUT_AFTER_UNLOCK' }
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c'
$libs = Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /wd4127 /O2 /MT @defines "/I$Out" "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\recovery_protocol_test.exe" "$Out\recovery_protocol_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if ($LASTEXITCODE -ne 0) { throw 'recovery protocol host build failed' }
& "$Out\recovery_protocol_test.exe"
exit $LASTEXITCODE

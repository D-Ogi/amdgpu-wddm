param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\hang-recovery",[switch]$IgnoreFenceGuard,[switch]$IgnoreVmidGuard,[switch]$SharedLastCompleted,[switch]$GartBackend,[switch]$CountRefusedKills,[switch]$NoBackendSwitch)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
# The header under test is copied next to the build, so that a negative control changes the copy and never the
# tree (the pattern of run_vmid_pool.ps1). -IgnoreFenceGuard drops the 0x119 fence-range guard, which is what
# keeps DxgkDdiResetEngine from handing dxgkrnl an aborted fence outside [last completed, last submitted].
# -IgnoreVmidGuard admits any VMID to the broadcast wave kill, including the GART domain and SDMA paging's.
# -SharedLastCompleted puts back the read of 0.7.216.13 (lab trial D, 2026-10-07): the lower bound of that range from the
# adapter-wide LastCompletedFence, which holds the newest report of either node, instead of the reset node's own
# last reported fence. It changes the copied test's read site, which mirrors wddm.c's, so the two-node checks fail.
# -GartBackend puts back the register path of 0.7.216.16 (lab trial D1, 2026-10-07): the kill runs with the GART
# sequence installed, whose table has no SQ_CMD. It changes the copied test's backend switch, which mirrors gfx.c's.
# -CountRefusedKills puts back 0.7.216.16's count: every kill the loop issued, whether or not it reached the register.
# Each must fail by a check, not by a build error, or the control proves nothing.
# -NoBackendSwitch removes the backend switch from the copy of gfx.c that the source check below reads.
$gfx=Get-Content "$repo\driver\kmd\gfx.c" -Raw
if($NoBackendSwitch){
    $live='    adev->backend = &gfx->Sequence;'
    $at=$gfx.IndexOf('ULONG GfxSoftRecover(')
    if($at -lt 0 -or $gfx.IndexOf($live,$at) -lt 0){throw 'negative control: the backend switch of GfxSoftRecover moved'}
    $cut=$gfx.IndexOf($live,$at)
    $gfx=$gfx.Remove($cut,$live.Length).Insert($cut,'    /* no backend switch */')
}
# The source check. The host model below proves that the loop is right over the GFX table, but it cannot compile
# gfx.c. So gfx.c's GfxSoftRecover must hold GartLock, install and begin the gfx sequence before the kill loop and
# put the previous backend back after it - the shape of GfxSubmitIb, and what 0.7.216.16 lacked.
$at=$gfx.IndexOf('ULONG GfxSoftRecover(')
if($at -lt 0){throw 'source check: GfxSoftRecover is not in gfx.c'}
$end=$gfx.IndexOf("`n}",$at)
$body=$gfx.Substring($at,$end-$at)
$steps=@('ExAcquireFastMutex(&Device->GartLock);','adev->backend = &gfx->Sequence;','SequenceBegin(&gfx->Sequence, Device, FALSE, NULL, 0);','Bc250HangKillLoop(','adev->backend = previousBackend;','ExReleaseFastMutex(&Device->GartLock);')
$pos=0
foreach($s in $steps){
    $i=$body.IndexOf($s,$pos)
    if($i -lt 0){Write-Output "FAIL source check: GfxSoftRecover lacks '$s' after the step before it (gfx.c)";exit 1}
    $pos=$i+$s.Length
}
$header=Get-Content "$repo\driver\kmd\hang_recovery.h" -Raw
if($IgnoreFenceGuard){
    $live='    return (int)(aborted - lastCompleted) >= 0 && (int)(lastSubmitted - aborted) >= 0;'
    if(!$header.Contains($live)){throw 'negative control: the fence-range guard moved'}
    $header=$header.Replace($live,'    return (aborted | lastCompleted | lastSubmitted) != 0xDEADu;')
}
if($IgnoreVmidGuard){
    $live='    return vmid < BC250_VMID_COUNT && vmid != BC250_VMID_GART && vmid != BC250_VMID_SDMA_PAGING;'
    if(!$header.Contains($live)){throw 'negative control: the kill VMID guard moved'}
    $header=$header.Replace($live,'    return vmid < BC250_VMID_COUNT;')
}
if($CountRefusedKills){
    $live='        if (!ops->Kill(context)) { *refused = 1; continue; }     /* one last look, then out */'
    if(!$header.Contains($live)){throw 'negative control: the refused-kill exit moved'}
    $header=$header.Replace($live,'        (void)ops->Kill(context);')
}
[IO.File]::WriteAllText((Join-Path $Out 'hang_recovery.h'),$header)
$test=Get-Content "$PSScriptRoot\hang_recovery_test.c" -Raw
if($SharedLastCompleted){
    $live='    known = Bc250HangNodeLastCompleted(f->LastReportedFence, f->LastReportedValid, MODEL_NODES, node, &lastCompleted); /* READ SITE */'
    if(!$test.Contains($live)){throw 'negative control: the read site moved'}
    $test=$test.Replace($live,'    known = 1; lastCompleted = (unsigned)f->LastCompletedFence; (void)node; /* READ SITE */')
}
if($GartBackend){
    $live='    g->Backend = &g->Gfx; /* KILL BACKEND */'
    if(!$test.Contains($live)){throw 'negative control: the kill backend moved'}
    $test=$test.Replace($live,'    g->Backend = &g->Gart; /* KILL BACKEND */')
}
[IO.File]::WriteAllText((Join-Path $Out 'hang_recovery_test.c'),$test)
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /wd4127 /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\hang_recovery_test.exe" "$Out\hang_recovery_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'hang recovery host build failed'}
& "$Out\hang_recovery_test.exe"
exit $LASTEXITCODE

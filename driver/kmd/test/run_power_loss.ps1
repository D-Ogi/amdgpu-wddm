param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\power-loss",[switch]$NoPowerLostRule,[switch]$FenceAdvanceAtResume,[switch]$IhQuietAlways,[switch]$RepairAfterUnsaved)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
# BD-188 D-2 (docs/design/power-loss.md). power_loss_test.c checks the pure rules of power_loss.h. The host cannot
# compile wddm.c, ih.c, gfx.c or pnp.c, so five of their properties are read from the text (the pattern of run_adapter_reset.ps1). Each
# negative control changes a copy of the text and must fail by a source check:
#   -NoPowerLostRule        ResetEngine no longer refuses a job that the power took, so the kill and the queue reset
#                           run on an engine that holds nothing of it.
#   -FenceAdvanceAtResume   WddmResumeRetained advances a fence: only the adapter-reset commit may (condition c).
#   -IhQuietAlways          IhPowerLost calls the IH quiet when interrupt admission did not close, so GfxPowerLost
#                           clears GpuStopUnconfirmed while an ISR can still run (review T001 M1).
#   -RepairAfterUnsaved     pnp.c repairs the display after an unsaved way down, on engines that are down (T001 M2).
Write-Output 'test-start: source checks'
$wddm=Get-Content "$repo\driver\kmd\wddm.c" -Raw
if($NoPowerLostRule){
    $live='        verdict = powerLost ? BC250_HANG_VERDICT_POWER_LOST :'
    if(!$wddm.Contains($live)){throw 'negative control: the power-lost verdict moved'}
    $wddm=$wddm.Replace($live,'        verdict =')
}
if($FenceAdvanceAtResume){
    $live='        wddm->PowerLostWork=0;'
    if(!$wddm.Contains($live)){throw 'negative control: the resume of the forced pause moved'}
    $wddm=$wddm.Replace($live,$live + "`n" + '        wddm->LastReportedFence[BC250_WDDM_NODE_3D]=wddm->LastSubmittedFence[BC250_WDDM_NODE_3D];')
}
$ih=Get-Content "$repo\driver\kmd\ih.c" -Raw
$gfx=Get-Content "$repo\driver\kmd\gfx.c" -Raw
$pnp=Get-Content "$repo\driver\kmd\pnp.c" -Raw
if($IhQuietAlways){
    $live='        Device->IhQuiet=closed;'
    if(!$ih.Contains($live)){throw 'negative control: the IH quiet of IhPowerLost moved'}
    $ih=$ih.Replace($live,'        Device->IhQuiet=TRUE;')
}
if($RepairAfterUnsaved){
    $live=' &&' + "`n" + '            Bc250PowerDownRepairsDisplay(InterlockedCompareExchange(&device->RetainedPowerPhase,0,0))) {'
    if(!$pnp.Contains($live)){throw 'negative control: the display repair rule of pnp.c moved'}
    $pnp=$pnp.Replace($live,') {')
}
function Body([string]$text,[string]$signature){
    $at=$text.IndexOf($signature)
    if($at -lt 0){return $null}
    return $text.Substring($at,$text.IndexOf("`n}",$at)-$at)
}
function Fail([string]$why){Write-Output "FAIL source check: $why";Write-Output 'test-end: source checks exit 1';exit 1}
# 1. ResetEngine decides the power-lost refusal under the lock and before any kill or queue reset.
$body=Body $wddm 'static NTSTATUS Bc250WddmResetEngine('
if(!$body){Fail 'Bc250WddmResetEngine is not in wddm.c'}
$pos=0
foreach($s in @('lostSeq = wddm->PowerLostSeq[node];','powerLost = Bc250HangPowerLost(onRing, headSeq, lostSeq);','KeReleaseSpinLock(&wddm->Lock, irql);','verdict = powerLost ? BC250_HANG_VERDICT_POWER_LOST :','GfxSoftRecover(device','GfxQueueReset(device')){
    $i=$body.IndexOf($s,$pos)
    if($i -lt 0){Fail "Bc250WddmResetEngine lacks '$s' after the step before it (wddm.c)"}
    $pos=$i+$s.Length
}
# 2. Neither the forced pause nor the resume moves a fence (condition c).
foreach($sig in @('void WddmPauseForPowerLoss(','NTSTATUS WddmResumeRetained(')){
    $b=Body $wddm $sig
    if(!$b){Fail "$sig is not in wddm.c"}
    foreach($forbidden in @('LastReportedFence','SubmittedFence[','Bc250AdapterResetFence','LastSubmittedFence[','CompletionPending[')){
        if($b.Contains($forbidden)){Fail "$sig touches '$forbidden' (wddm.c): a fence moves only at the adapter-reset commit"}
    }
}
# 3. The adapter-reset commit, after the busy refusal, ends the power-lost refusal of every node.
$body=Body $wddm 'static BOOLEAN WddmAdapterReset('
if(!$body){Fail 'WddmAdapterReset is not in wddm.c'}
$busy=$body.IndexOf('if (busy != 0) {')
$clear=$body.IndexOf('Wddm->PowerLostSeq[n] = 0;')
if($busy -lt 0 -or $clear -lt $busy){Fail 'WddmAdapterReset does not clear PowerLostSeq in its commit (wddm.c)'}
# 4. The witness proves the chip state, not a closed interrupt admission: IhPowerLost calls the IH quiet only when
#    admission closed, and GfxPowerLost clears GpuStopUnconfirmed only when the IH is quiet.
$body=Body $ih 'void IhPowerLost('
if(!$body){Fail 'IhPowerLost is not in ih.c'}
if(!$body.Contains('Device->IhQuiet=closed;') -or $body.Contains('Device->IhQuiet=TRUE')){Fail 'IhPowerLost sets IhQuiet without a closed interrupt admission (ih.c)'}
$body=Body $gfx 'void GfxPowerLost('
if(!$body){Fail 'GfxPowerLost is not in gfx.c'}
if(([regex]::Matches($body,'GpuStopUnconfirmed\s*=\s*FALSE')).Count -ne 1 -or !$body.Contains('if (Device->IhQuiet) Device->GpuStopUnconfirmed=FALSE;')){Fail 'GfxPowerLost clears GpuStopUnconfirmed without a quiet IH (gfx.c)'}
# 5. pnp.c repairs the display after a failed way down only by the rule of power_phase.h (not after an unsaved one).
$body=Body $pnp 'NTSTATUS Bc250SetPowerState('
if(!$body){Fail 'Bc250SetPowerState is not in pnp.c'}
$rule=$body.IndexOf('Bc250PowerDownRepairsDisplay(InterlockedCompareExchange(&device->RetainedPowerPhase,0,0))')
$repair=$body.IndexOf('ModesetPowerUp(device);')
if($rule -lt 0 -or $repair -lt $rule){Fail 'Bc250SetPowerState repairs the display without Bc250PowerDownRepairsDisplay (pnp.c)'}
Write-Output 'test-end: source checks exit 0'
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\power_loss_test.exe" "$PSScriptRoot\power_loss_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'power loss host build failed'}
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\power_loss_test.exe"

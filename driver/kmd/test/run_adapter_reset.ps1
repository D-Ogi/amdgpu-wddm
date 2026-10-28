param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\adapter-reset",[switch]$LastSampleOnly,[switch]$IgnoreSdmaIdle,[switch]$PartialBegin,[switch]$CompleteNonEmptyWindow,[switch]$ReorderWitnessTable,[switch]$CommitBeforeWitness)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
# TDR recovery S1 (docs/design/tdr-recovery-s1.md). The header under test is copied next to the build, so that a
# negative control changes the copy and never the tree (the pattern of run_hang_recovery.ps1). Each control must
# fail by a check, not by a build error, or it proves nothing:
#   -LastSampleOnly          the witnesses look at the last register sample only, so a busy first sample passes.
#   -IgnoreSdmaIdle          SDMA0_STATUS_REG.IDLE is not a witness.
#   -PartialBegin            the begin of the reset sets ResetActive node by node, so a refusal leaves nodes begun.
#   -CompleteNonEmptyWindow  the restart completes a window packet that has commands, which never ran.
#   -ReorderWitnessTable     wddm.c's register table swaps two entries (the source check below).
#   -CommitBeforeWitness     wddm.c opens the window before it computes the witnesses (the source check below).
$header=Get-Content "$repo\driver\kmd\adapter_reset.h" -Raw
if($LastSampleOnly){
    $live='    for (i = 0; i < count; i++) busy |= Bc250AdapterSampleBusy(&samples[i], sdmaRingInUse);'
    if(!$header.Contains($live)){throw 'negative control: the sample loop moved'}
    $header=$header.Replace($live,'    for (i = 0; i < count; i++) if (i + 1 == count) busy |= Bc250AdapterSampleBusy(&samples[i], sdmaRingInUse);')
}
if($IgnoreSdmaIdle){
    $live='        !(s->Value[BC250_ADAPTER_REG_SDMA0_STATUS] & SDMA0_STATUS_REG__IDLE_MASK))'
    if(!$header.Contains($live)){throw 'negative control: the SDMA0 idle test moved'}
    $header=$header.Replace($live,'        0)')
}
if($PartialBegin){
    $live='    if (reasons != 0) return reasons;'
    if(!$header.Contains($live)){throw 'negative control: the all-or-nothing return moved'}
    $header=$header.Replace($live,'    for (n = 0; n < nodeCount && n < 1; n++) states[n].ResetActive = 1;' + "`n" + $live)
}
if($CompleteNonEmptyWindow){
    $live='    if (nonEmpty != 0) return BC250_ADAPTER_WINDOW_CLOSE;'
    if(!$header.Contains($live)){throw 'negative control: the window close moved'}
    $header=$header.Replace($live,'    if (nonEmpty != 0) return BC250_ADAPTER_WINDOW_COMPLETE;')
}
[IO.File]::WriteAllText((Join-Path $Out 'adapter_reset.h'),$header)

# The source checks. The host model cannot compile wddm.c, so two of its properties are read from the text:
#  1. g_AdapterWitnessRegisters names the registers in the order of the BC250_ADAPTER_REG_* indexes (the test's own
#     table, whose offsets it checks against the read list, uses the same order).
#  2. WddmAdapterReset computes the witnesses, refuses on a busy mask, and only then opens the window and returns
#     TRUE. A success return before the witnesses would be a success without proof.
$wddm=Get-Content "$repo\driver\kmd\wddm.c" -Raw
$order=@('BC250_REG_GC_GRBM_STATUS,','BC250_REG_GC_CP_STAT,','BC250_REG_GC_CP_CPC_STATUS,','BC250_REG_GC_SDMA0_STATUS_REG,','BC250_REG_GC_SDMA0_GFX_RB_RPTR,','BC250_REG_GC_SDMA0_GFX_RB_RPTR_HI,','BC250_REG_GC_SDMA0_GFX_RB_WPTR,','BC250_REG_GC_SDMA0_GFX_RB_WPTR_HI,')
if($ReorderWitnessTable){
    $live='    BC250_REG_GC_GRBM_STATUS, BC250_REG_GC_CP_STAT, BC250_REG_GC_CP_CPC_STATUS,'
    if(!$wddm.Contains($live)){throw 'negative control: the witness table moved'}
    $wddm=$wddm.Replace($live,'    BC250_REG_GC_CP_STAT, BC250_REG_GC_GRBM_STATUS, BC250_REG_GC_CP_CPC_STATUS,')
}
$at=$wddm.IndexOf('static const ULONG g_AdapterWitnessRegisters[BC250_ADAPTER_SAMPLE_REGISTERS] = {')
if($at -lt 0){Write-Output 'FAIL source check: g_AdapterWitnessRegisters is not in wddm.c';exit 1}
$end=$wddm.IndexOf('};',$at)
$table=$wddm.Substring($at,$end-$at)
$pos=0
foreach($o in $order){
    $i=$table.IndexOf($o,$pos)
    if($i -lt 0){Write-Output "FAIL source check: g_AdapterWitnessRegisters lacks '$o' after the entry before it (wddm.c)";exit 1}
    $pos=$i+$o.Length
}
if($CommitBeforeWitness){
    $live='    InterlockedExchange(&Wddm->AdapterResetWindow, 1);'
    if(!$wddm.Contains($live)){throw 'negative control: the window open moved'}
    $wddm=$wddm.Replace($live,'')
    $wddm=$wddm.Replace('    GfxRingsRetired(Device, &rings, &sdmaRingInUse);',$live + "`n" + '    GfxRingsRetired(Device, &rings, &sdmaRingInUse);')
}
$at=$wddm.IndexOf('static BOOLEAN WddmAdapterReset(')
if($at -lt 0){Write-Output 'FAIL source check: WddmAdapterReset is not in wddm.c';exit 1}
$end=$wddm.IndexOf("`n}",$at)
$body=$wddm.Substring($at,$end-$at)
$steps=@('Bc250AdapterResetBeginAll(','GfxRingsRetired(Device, &rings, &sdmaRingInUse);','busy = Bc250AdapterBusyReasons(','if (busy != 0) {','InterlockedExchange(&Wddm->AdapterResetWindow, 1);','return TRUE;')
$pos=0
foreach($s in $steps){
    $i=$body.IndexOf($s,$pos)
    if($i -lt 0){Write-Output "FAIL source check: WddmAdapterReset lacks '$s' after the step before it (wddm.c)";exit 1}
    $pos=$i+$s.Length
}
$first=$body.IndexOf('return TRUE;')
if($first -ne $body.LastIndexOf('return TRUE;')){Write-Output 'FAIL source check: WddmAdapterReset has more than one success return (wddm.c)';exit 1}

Copy-Item "$PSScriptRoot\adapter_reset_test.c" (Join-Path $Out 'adapter_reset_test.c') -Force
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
# MSVC like run_hang_recovery.ps1: the header is kernel code, built by the KMD's own compiler.
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /wd4127 /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$repo\third_party\linux-amdgpu" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\adapter_reset_test.exe" "$Out\adapter_reset_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'adapter reset host build failed'}
& "$Out\adapter_reset_test.exe"
exit $LASTEXITCODE

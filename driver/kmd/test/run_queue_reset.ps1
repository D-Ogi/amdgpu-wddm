param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\kmd-queue-reset",[switch]$AnyVmidPasses,[switch]$StageBeforeKill)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
# Stage 2 of the hang recovery (docs/design/queue-reset.md), the KMD half. The header under test is copied next to
# the build, so that a negative control changes the copy and never the tree. Each control must fail by a check:
#   -AnyVmidPasses    the wave proof ignores the VMID of a wave, so a wave of the hung VMID passes.
#   -StageBeforeKill  wddm.c calls stage 2 before the stage-1 kill loop (the source check below).
$header=Get-Content "$repo\driver\kmd\queue_reset.h" -Raw
if($AnyVmidPasses){
    $live='            return BC250_QRESET_STEP_WAVES;' + "`n" + '    return 0;'
    $live=$live.Replace("`n", $(if($header.Contains("`r`n")){"`r`n"}else{"`n"}))
    if(!$header.Contains($live)){throw 'negative control: the wave loop moved'}
    $header=$header.Replace($live,'            ;' + $(if($header.Contains("`r`n")){"`r`n"}else{"`n"}) + '    return 0;')
}
[IO.File]::WriteAllText((Join-Path $Out 'queue_reset.h'),$header)

# The source check: wddm.c's ResetEngine runs stage 2 only after the stage-1 kill loop has returned its verdict, and
# before the recovered test that opens the gates. A stage 2 ahead of the kill would reset a queue that a wave kill
# alone could have drained, and one after the recovered test would never open the node.
Write-Output 'test-start: source checks'
$wddm=Get-Content "$repo\driver\kmd\wddm.c" -Raw
if($StageBeforeKill){
    $kill='            verdict = node == BC250_WDDM_NODE_COMPUTE ? GfxComputeSoftRecover('
    $at0=$wddm.IndexOf($kill)
    if($at0 -lt 0){throw 'negative control: the kill call moved'}
    $wddm=$wddm.Insert($at0,'            if (Bc250QueueResetWanted(0, 0, 0, 0, 0)) { }' + "`n")
}
$at=$wddm.IndexOf('NTSTATUS APIENTRY Bc250WddmResetEngine(')
if($at -lt 0){$at=$wddm.IndexOf('Bc250WddmResetEngine(')}
if($at -lt 0){Write-Output 'FAIL source check: Bc250WddmResetEngine is not in wddm.c';Write-Output 'test-end: source checks exit 1';exit 1}
$steps=@('GfxComputeSoftRecover(device, vmid, &seq, &kills, &micros)','Bc250QueueResetWanted(GuardLabQueueReset(), wddm->HangRecoveryMode, node, verdict, 1)','verdict = GfxQueueReset(device, vmid,','if (Bc250HangVerdictRecovered(verdict))')
$pos=$at
foreach($s in $steps){
    $i=$wddm.IndexOf($s,$pos)
    if($i -lt 0){Write-Output "FAIL source check: ResetEngine lacks '$s' after the step before it (wddm.c)";Write-Output 'test-end: source checks exit 1';exit 1}
    $pos=$i+$s.Length
}
$first=$wddm.IndexOf('Bc250QueueResetWanted(',$at)
$killAt=$wddm.IndexOf('GfxComputeSoftRecover(device',$at)
if($first -lt $killAt){Write-Output 'FAIL source check: stage 2 is asked for before the stage-1 kill (wddm.c)';Write-Output 'test-end: source checks exit 1';exit 1}
Write-Output 'test-end: source checks exit 0'
Copy-Item "$PSScriptRoot\queue_reset_test.c" (Join-Path $Out 'queue_reset_test.c') -Force
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
# MSVC like run_adapter_reset.ps1: the headers are kernel code, built by the KMD's own compiler.
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /wd4127 /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$repo\third_party\linux-amdgpu" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\queue_reset_test.exe" "$Out\queue_reset_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'queue reset host build failed'}
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\queue_reset_test.exe"

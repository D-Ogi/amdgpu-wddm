param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\hang-recovery",[switch]$IgnoreFenceGuard,[switch]$IgnoreVmidGuard,[switch]$SharedLastCompleted)
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
# Each must fail by a check, not by a build error, or the control proves nothing.
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
[IO.File]::WriteAllText((Join-Path $Out 'hang_recovery.h'),$header)
$test=Get-Content "$PSScriptRoot\hang_recovery_test.c" -Raw
if($SharedLastCompleted){
    $live='    known = Bc250HangNodeLastCompleted(f->LastReportedFence, f->LastReportedValid, MODEL_NODES, node, &lastCompleted); /* READ SITE */'
    if(!$test.Contains($live)){throw 'negative control: the read site moved'}
    $test=$test.Replace($live,'    known = 1; lastCompleted = (unsigned)f->LastCompletedFence; (void)node; /* READ SITE */')
}
[IO.File]::WriteAllText((Join-Path $Out 'hang_recovery_test.c'),$test)
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /wd4127 /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\hang_recovery_test.exe" "$Out\hang_recovery_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'hang recovery host build failed'}
& "$Out\hang_recovery_test.exe"
exit $LASTEXITCODE

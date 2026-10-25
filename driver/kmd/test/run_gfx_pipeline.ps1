param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\gfx-submit-pipeline\tests",[switch]$WithoutCapacity,[switch]$WithoutBoundaryPad,[switch]$WithoutPadCapacity,[switch]$EqualityFence,[switch]$SerializeGather,[switch]$IdleOnlyPresent,[switch]$KmdOnly,[string]$SourceRoot='',[string]$MesaSource=$(if ($env:BC250_RADV_SOURCE) { $env:BC250_RADV_SOURCE } else { '' }))
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp'; $env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
if(!$SourceRoot){$SourceRoot=$repo}
# The gather half reads RADV's WDDM winsys (radv_wddm2_cs.c), the same tree the fast gate's blob-abi check uses;
# -KmdOnly (the ring and the KMD submit path, all of it this repository's) reads no Mesa source at all.
if(!$MesaSource){$MesaSource=Join-Path $Root 'scratch\m12\mesa-current-src'}
if($KmdOnly){$MesaSource=''}
& python "$PSScriptRoot\generate_gfx_pipeline_test.py" "$Out\gfx_pipeline_actual.inc" $SourceRoot $MesaSource
if($LASTEXITCODE -ne 0){throw 'extraction failed'}
if($IdleOnlyPresent){
    $actual=Get-Content "$Out\gfx_pipeline_actual.inc" -Raw
    $marker='static BOOLEAN WddmSubmitHeldHardware('
    $at=$actual.IndexOf($marker)
    if($at -lt 0){throw 'Present admission extraction missing'}
    $actual=$actual.Substring(0,$at)+$actual.Substring($at).Replace('(GfxSubmitReady(Device) || GfxSubmitBusy(Device))','GfxSubmitReady(Device)')
    [IO.File]::WriteAllText("$Out\gfx_pipeline_actual.inc",$actual)
}
if($SerializeGather){
    $g=Get-Content "$Out\gather_actual.inc" -Raw
    $g=$g.Replace('reuse.wait_value = slot->retire_value;', 'reuse.wait_value = queue->bc250_progress.wait_value;')
    [IO.File]::WriteAllText("$Out\gather_actual.inc",$g)
}
$ring=Get-Content "$repo\driver\shim\bc250_ring.c" -Raw
if($WithoutCapacity){
    $anchor='if (ring->track_rptr && !bc250_ring_has_space(ring, ndw)) return -16;'
    if(([regex]::Matches($ring,[regex]::Escape($anchor))).Count -ne 1){throw 'capacity anchor not found once'}
    $ring=$ring.Replace($anchor,'/* negative control: old allocator ignores CP read pointer */')
}
# BD-235 negative control: amdgpu_ring_alloc() inserts no pad before a reservation that straddles 2^30 dwords.
if($WithoutBoundaryPad){
    $anchor='pad = bc250_ring_boundary_pad_size(ring, ndw);'
    if(([regex]::Matches($ring,[regex]::Escape($anchor))).Count -ne 1){throw 'boundary pad anchor not found once'}
    $ring=$ring.Replace($anchor,'pad = 0; /* negative control: no boundary pad */')
}
# BD-235 negative control: amdgpu_ring_alloc() writes the pad without the check that the pad and the request fit the ring.
if($WithoutPadCapacity){
    $anchor='if (pad != 0 && (pad > ring->buf_mask || ndw > ring->buf_mask - pad)) {'
    if(([regex]::Matches($ring,[regex]::Escape($anchor))).Count -ne 1){throw 'pad capacity anchor not found once'}
    $ring=$ring.Replace($anchor,'if (0) { /* negative control: no pad capacity check */')
}
[IO.File]::WriteAllText("$Out\bc250_ring_actual.c",$ring)
$fence=Get-Content "$repo\driver\shim\include\bc250_fence_order.h" -Raw
if($EqualityFence){$fence=$fence.Replace('(observed - requested) < 0x80000000u','observed == requested')}
[IO.File]::WriteAllText("$Out\bc250_fence_order.h",$fence)
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c'
$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
# Host tests are user-mode code: clang-cl 23.1.4 from the workspace toolchain, named by path, no fallback. The MSVC
# tree gives only the CRT headers and libraries.
$cl=Join-Path $Root 'toolchain\llvm-23.1.4-win64\bin\clang-cl.exe'
if(!(Test-Path -LiteralPath $cl)){throw "clang-cl not found: $cl"}
$clVersion=@(& $cl --version); if($LASTEXITCODE -ne 0 -or $clVersion[0] -notmatch 'clang version 23\.1\.4\b'){throw "clang-cl 23.1.4 expected: $($clVersion[0])"}
Write-Host "host compiler: $cl"; Write-Host "  $($clVersion[0])"
$env:CL=$null; $env:_CL_=$null
# The mocks keep counters that only their stubs write, and the tests start aggregates with {0} or a prefix: clang's
# warnings for those two idioms are off. Every other /W4 warning stays an error.
$flags=@('/nologo','--target=x86_64-pc-windows-msvc','-fuse-ld=lld','/clang:-Wno-missing-field-initializers','/clang:-Wno-unused-but-set-global','/TC','/W4','/WX','/O2','/MT',"/I$Out","/I$repo\driver\kmd","/I$repo\driver\shim\include","/I$repo\driver\amdgpu-import","/I$repo\third_party\linux-amdgpu","/imsvc$($msvc.FullName)\include","/imsvc$sdk\Include\10.0.26100.0\ucrt","/Fo$Out\")
if((Get-Content "$Out\gfx_pipeline_actual.inc" -Raw) -notmatch 'WddmRecordFenceLedgerLocked'){$flags+='/DNO_RECOVERY_LEDGER'}
$link=@('/link',"/LIBPATH:$($msvc.FullName)\lib\x64","/LIBPATH:$libs\ucrt\x64","/LIBPATH:$libs\um\x64")
& $cl @flags "/Fe$Out\ring_test.exe" "$PSScriptRoot\gfx_ring_capacity_test.c" "$Out\bc250_ring_actual.c" @link
if($LASTEXITCODE -ne 0){throw 'ring test build failed'}
& $cl @flags "/Fe$Out\pipeline_test.exe" "$PSScriptRoot\gfx_pipeline_test.c" @link
if($LASTEXITCODE -ne 0){throw 'pipeline test build failed'}
if(!$KmdOnly){
    & $cl @flags "/Fe$Out\gather_test.exe" "$PSScriptRoot\gfx_gather_test.c" @link
    if($LASTEXITCODE -ne 0){throw 'gather test build failed'}
}
# Every program runs bounded and windowless and counts only with its summary line and the exit code 0 or 1 that goes
# with it (host_native.ps1). Each control pins its CHECK failures as (ring test, pipeline test): EqualityFence @(2,53) is
# 2 failures of the ring test and 53 of the pipeline test. A crash, a deadline, a build error or another count is an
# invalid control and fails the gate. Every run, positive or control, must also complete at least the checks of the
# positive run (MinChecks), so that a program that stops early is not a result. SerializeGather pins no count: it needs
# a Mesa tree (not -KmdOnly) and a failure of the gather test alone, with the ring and pipeline tests clean.
. "$PSScriptRoot\host_native.ps1"
$controls=[ordered]@{WithoutCapacity=@(3,0);WithoutBoundaryPad=@(15,0);WithoutPadCapacity=@(4,0);EqualityFence=@(2,53);IdleOnlyPresent=@(0,46);SerializeGather=@(0,0)}
# The checks of the positive run (2026-10-25, with the node 2 cases): ring 10066, pipeline 2763. A control may run more (a failing
# wait loops longer: EqualityFence 4618, IdleOnlyPresent 28098 pipeline checks), never fewer.
$minChecks=@{Ring=10066;Pipeline=2763}
$chosen=@($controls.Keys | Where-Object { $PSBoundParameters.ContainsKey($_) })
if($chosen.Count -gt 1){throw "one negative control at a time: $($chosen -join ', ')"}
$ring=Invoke-HostTest "$Out\ring_test.exe" "$Out\ring_test.log" '^GFX ring capacity/fence order: (?<checks>\d+) checks, (?<failures>\d+) failures$'
$queue=Invoke-HostTest "$Out\pipeline_test.exe" "$Out\pipeline_test.log" '^GFX WDDM actual submit/completion/watchdog: (?<checks>\d+) checks, (?<failures>\d+) failures$'
$gather=$null
if(!$KmdOnly){$gather=Invoke-HostTest "$Out\gather_test.exe" "$Out\gather_test.log" '^RADV actual gather selection/retirement: (?<checks>\d+) checks, (?<failures>\d+) failures$'}
$gatherFailures=if($gather){$gather.Failures}else{0}
if($ring.Checks -lt $minChecks.Ring -or $queue.Checks -lt $minChecks.Pipeline){
    throw "incomplete run: ring $($ring.Checks) checks (at least $($minChecks.Ring)), pipeline $($queue.Checks) checks (at least $($minChecks.Pipeline))"
}
if($chosen.Count -eq 0){
    if($ring.Failures -or $queue.Failures -or $gatherFailures){exit 1}
    exit 0
}
$name=$chosen[0]; $want=$controls[$name]
if($name -eq 'SerializeGather'){
    if($KmdOnly -or $gatherFailures -eq 0 -or $ring.Failures -or $queue.Failures){throw "negative control $name did not fail the gather test alone"}
} elseif($ring.Failures -ne $want[0] -or $queue.Failures -ne $want[1] -or $gatherFailures){
    throw "negative control $name : ring $($ring.Failures), pipeline $($queue.Failures), gather $gatherFailures failures; ring $($want[0]), pipeline $($want[1]), gather 0 expected"
}
Write-Host "negative control $name : the tests failed as required (ring $($ring.Failures), pipeline $($queue.Failures), gather $gatherFailures)"
exit 0

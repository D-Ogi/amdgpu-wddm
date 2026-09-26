param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\gfx-submit-pipeline\tests",[switch]$WithoutCapacity,[switch]$EqualityFence,[switch]$SerializeGather,[string]$SourceRoot='')
$ErrorActionPreference='Stop'
$repo=Join-Path $Root 'bc250-win'
$env:TEMP=Join-Path $Root 'scratch\tmp'; $env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
if(!$SourceRoot){$SourceRoot=$repo}
& python "$PSScriptRoot\generate_gfx_pipeline_test.py" "$Out\gfx_pipeline_actual.inc" $SourceRoot
if($LASTEXITCODE -ne 0){throw 'extraction failed'}
if($SerializeGather){
    $g=Get-Content "$Out\gather_actual.inc" -Raw
    $g=$g.Replace('reuse.wait_value = slot->retire_value;', 'reuse.wait_value = queue->bc250_progress.wait_value;')
    [IO.File]::WriteAllText("$Out\gather_actual.inc",$g)
}
$ring=Get-Content "$repo\driver\shim\bc250_ring.c" -Raw
if($WithoutCapacity){$ring=$ring.Replace('if (ring->track_rptr && !bc250_ring_has_space(ring, ndw)) return -16;', '/* negative control: old allocator ignores CP read pointer */')}
[IO.File]::WriteAllText("$Out\bc250_ring_actual.c",$ring)
$fence=Get-Content "$repo\driver\shim\include\bc250_fence_order.h" -Raw
if($EqualityFence){$fence=$fence.Replace('(observed - requested) < 0x80000000u','observed == requested')}
[IO.File]::WriteAllText("$Out\bc250_fence_order.h",$fence)
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c'
$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
$cl=Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$flags=@('/nologo','/TC','/W4','/WX','/O2','/MT',"/I$Out","/I$repo\driver\kmd","/I$repo\driver\shim\include","/I$repo\driver\amdgpu-import","/I$repo\third_party\linux-amdgpu","/I$($msvc.FullName)\include","/I$sdk\Include\10.0.26100.0\ucrt","/Fo$Out\")
if((Get-Content "$Out\gfx_pipeline_actual.inc" -Raw) -notmatch 'WddmRecordFenceLedgerLocked'){$flags+='/DNO_RECOVERY_LEDGER'}
$link=@('/link',"/LIBPATH:$($msvc.FullName)\lib\x64","/LIBPATH:$libs\ucrt\x64","/LIBPATH:$libs\um\x64")
& $cl @flags "/Fe$Out\ring_test.exe" "$PSScriptRoot\gfx_ring_capacity_test.c" "$Out\bc250_ring_actual.c" @link
if($LASTEXITCODE -ne 0){throw 'ring test build failed'}
& "$Out\ring_test.exe"
$ringResult=$LASTEXITCODE
& $cl @flags "/Fe$Out\pipeline_test.exe" "$PSScriptRoot\gfx_pipeline_test.c" @link
if($LASTEXITCODE -ne 0){throw 'pipeline test build failed'}
& "$Out\pipeline_test.exe"
$queueResult=$LASTEXITCODE
& $cl @flags "/Fe$Out\gather_test.exe" "$PSScriptRoot\gfx_gather_test.c" @link
if($LASTEXITCODE -ne 0){throw 'gather test build failed'}
& "$Out\gather_test.exe"
$gatherResult=$LASTEXITCODE
if($ringResult -ne 0 -or $queueResult -ne 0 -or $gatherResult -ne 0){exit 1}
exit 0

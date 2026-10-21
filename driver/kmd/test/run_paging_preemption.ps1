param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\paging-preemption",[ValidateSet('','--old-drain','--early-reject')][string]$Mutation='',[switch]$AnyRootNative,[switch]$AnyAlignNative,[switch]$ExpectFailure)
# The paging DMA-boundary preemption host test (experiments/E27-m9-inference/generate-paging-preemption-test.py)
# against this tree: the queue harness plus the actual WddmPreemptFence, WddmReportDpcPublish and
# WddmReportDpcRoutine, with the real private-record parser and the real hang-recovery gate (hang_recovery.h).
# The node-0 rejected-fence case adds the actual Bc250WddmSubmitCommandVirtual wrapper around a stand-in body.
# The negative controls each must fail by a CHECK and never by a build error, which this script enforces by
# requiring a FAIL line in the output, not merely a non-zero exit code:
#   --old-drain      the pre-fix dispatch: WddmGpuFencePaging starts the next queued buffer although preemption
#                    is pending, so the engine never reaches the DMA boundary.
#   --early-reject   F-q44-mem-4: the report pass completes a rejected node-0 fence (SubmitCommandVirtual's
#                    STATUS_INVALID_PARAMETER) before the packets ahead of it have finished.
#   -AnyRootNative   BD-085: RootUsable admits any native root at all. That is what this fixture assumed until
#                    BD-085, and the rootless-submit check must catch it.
#   -AnyAlignNative  BD-085: RootUsable keeps the zero check and drops only the 4 KiB alignment check. The
#                    misaligned-root check must catch that half on its own. Without it the whole suite stayed
#                    green under this mutation (BD-085 review finding 2).
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
New-Item -ItemType Directory -Force $Out | Out-Null
$text=Get-Content "$repo\driver\kmd\wddm.c" -Raw
if($text -match '(?m)^<<<<<<<|^=======|^>>>>>>>'){throw 'Unresolved source conflict'}
# The parser is compiled from the tree, except under the two root controls: each changes a copy next to the
# build and never the tree (the pattern of run_hang_recovery.ps1).
$parser="$repo\driver\kmd\paging_private.c"
if($AnyRootNative -and $AnyAlignNative){throw 'the two root controls are separate mutations; pass one'}
if($AnyRootNative -or $AnyAlignNative){
    $src=Get-Content $parser -Raw
    $live='    return span->Kind==PAGING_PRIVATE_DIRECT || (root && !(root&4095u));'
    if(!$src.Contains($live)){throw 'negative control: the native root gate of RootUsable moved'}
    # -AnyRootNative drops the gate. -AnyAlignNative keeps the zero half and drops the alignment half, so each
    # half of the gate has a control of its own and one half cannot hide behind the other.
    $dead=if($AnyRootNative){'    return span->Kind==PAGING_PRIVATE_DIRECT || root==root;'}
          else{'    return span->Kind==PAGING_PRIVATE_DIRECT || root!=0;'}
    $src=$src.Replace($live,$dead)
    [IO.File]::WriteAllText((Join-Path $Out 'paging_private.c'),$src)
    $parser=Join-Path $Out 'paging_private.c'
}
$gen=@("$repo\experiments\E27-m9-inference\generate-paging-preemption-test.py",$repo,"$Out\test.c")
if($Mutation){$gen+=$Mutation}
# -E, so that PYTHONOPTIMIZE from the environment cannot strip the generator's own anchor guards.
& python -E @gen
if($LASTEXITCODE -ne 0){throw 'paging preemption test generation failed'}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /O2 /MT "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\paging_preemption_test.exe" "$Out\test.c" "$parser" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'paging preemption host build failed'}
$log=Join-Path $Out 'run.log'
& "$Out\paging_preemption_test.exe" 2>&1 | Tee-Object -FilePath $log
$code=$LASTEXITCODE
$control=if($Mutation){$Mutation}elseif($AnyRootNative){'-AnyRootNative'}elseif($AnyAlignNative){'-AnyAlignNative'}else{''}
if($ExpectFailure){
    if($code -eq 0){throw "negative control $control unexpectedly passed"}
    # A control that stopped producing its CHECK and began failing for some other runtime reason is not a
    # control. The same rule the parallel suite block of tools/quality/quick.ps1 applies to its own controls.
    if(!(Select-String -Path $log -Pattern 'FAIL' -CaseSensitive -Quiet)){
        throw "negative control $control failed without a FAIL check, exit $code"
    }
    Write-Host "Expected negative control $control failed";exit 0
}
exit $code

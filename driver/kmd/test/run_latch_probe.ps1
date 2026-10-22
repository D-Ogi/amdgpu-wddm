param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Out,
    [switch]$Mutations
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$cc=Join-Path $Root 'toolchain\llvm-23.1.4-win64\bin\clang-cl.exe'
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
$env:INCLUDE=''; $env:LIB=''
& $cc --version
if ($LASTEXITCODE) {throw 'Explicit clang-cl unavailable'}
& python "$PSScriptRoot\generate_latch_probe_test.py" --out "$Out\actual.c"
if ($LASTEXITCODE) {throw 'Probe extraction failed'}
function Invoke-Probe([string]$Source,[string]$Name) {
& $cc /nologo /TC /W4 /WX /O2 /MT -fuse-ld=lld /DAMDGPU_WDDM_LAB_FLAVOUR=1 `
    "/I$repo\driver\kmd" "/I$repo\third_party\linux-amdgpu" "/I$($crt.FullName)\include" `
    "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\$Name.exe" `
    $Source "$repo\driver\kmd\dcn_latch_sample.c" "$repo\driver\kmd\display_io.c" `
    /link "/LIBPATH:$($crt.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if ($LASTEXITCODE) {throw "Probe host compile failed: $Name"}
$output=& "$Out\$Name.exe"
return @{ Code=$LASTEXITCODE; Output=($output -join "`n") }
}
$run=Invoke-Probe "$Out\actual.c" 'latch_probe_test'
Write-Output $run.Output
if ($run.Code) {throw 'Probe host tests failed'}
& "$Out\latch_probe_test.exe" --emit-model-records | Set-Content "$Out\model-records.jsonl" -Encoding UTF8
if ($LASTEXITCODE) {throw 'Actual-core model record control failed'}
& python -B "$repo\tools\win\bc250kmd_cli\test_dcn_latch_analysis.py" --model-records "$Out\model-records.jsonl"
if ($LASTEXITCODE) {throw 'Latch evidence decoder controls failed'}
& python "$PSScriptRoot\generate_latch_escape_test.py" --out "$Out\escape.c"
if ($LASTEXITCODE) {throw 'Escape extraction failed'}
$run=Invoke-Probe "$Out\escape.c" 'latch_escape_test'
Write-Output $run.Output
if ($run.Code) {throw 'Escape admission tests failed'}
if ($Mutations) {
    $admissionChanges=@(
        @('escape-flags','Flags==required.Value','Flags!=required.Value'),
        @('escape-version','expected==BC250_KMD_VERSION','expected!=BC250_KMD_VERSION'),
        @('escape-admin','if (!Admin) {','if (!Admin && Flags==0) {'),
        @('escape-lab-gate','if (!LabDiagnosticsAllowed())','if (LabDiagnosticsAllowed())')
    )
    $escapeSource=[IO.File]::ReadAllText("$Out\escape.c")
    foreach ($change in $admissionChanges) {
        if ([regex]::Matches($escapeSource,[regex]::Escape($change[1])).Count -ne 1) {throw "Mutation anchor: $($change[0])"}
        $file="$Out\$($change[0]).c"
        [IO.File]::WriteAllText($file,$escapeSource.Replace($change[1],$change[2]))
        $run=Invoke-Probe $file $change[0]
        [IO.File]::WriteAllText("$Out\$($change[0]).txt",$run.Output)
        if (!$run.Code -or $run.Output -notmatch 'FAIL line') {throw "Mutation survived: $($change[0])"}
        Write-Output "Mutation caught at runtime: $($change[0])"
    }
    $changes=@(
        @('armed-pending', '!(Result->Armed.Word[Bc250LatchFlipControl] & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK)', '0'),
        @('single-transition', 'transitions>=2', 'transitions>=1'),
        @('fixed-phase-limit', 'C->PhaseMs=(ULONG)((2*frameUs+2000+999)/1000);', 'C->PhaseMs=40;'),
        @('no-write-cleanup', 'if (!Result->WritesAttempted) {', 'if (!Result->WritesAttempted && Arm==Bc250LatchArmCount) {'),
        @('runtime-post-valid', '!d->RuntimePostValid || C->Width!=d->RuntimePost.Width', 'C->Width!=d->RuntimePost.Width'),
        @('runtime-post-size', 'C->Width!=d->RuntimePost.Width || C->Height!=d->RuntimePost.Height', 'C->Width!=d->RuntimePost.Width'),
        @('cleanup-unlock', 'if (c.LockOwned) Result->UnlockStatus=LatchUnlock(&c);',
          'if (c.LockOwned && Arm==Bc250LatchUnlocked) Result->UnlockStatus=LatchUnlock(&c);'),
        @('cleanup-fault', 'WddmLatchProbeCheck(Device,c.Generation,TRUE)',
          'WddmLatchProbeCheck(Device,c.Generation,FALSE)'),
        @('late-lock', 'if (Arm==Bc250LatchLockAfterProgram) {', 'if (Arm==Bc250LatchArmCount) {'),
        @('end-failure', 'return Result->EndStatus;', 'return STATUS_SUCCESS;')
    )
    $source=[IO.File]::ReadAllText("$Out\actual.c")
    foreach ($change in $changes) {
        if ([regex]::Matches($source,[regex]::Escape($change[1])).Count -ne 1) {throw "Mutation anchor: $($change[0])"}
        $file="$Out\$($change[0]).c"
        [IO.File]::WriteAllText($file,$source.Replace($change[1],$change[2]))
        $run=Invoke-Probe $file $change[0]
        [IO.File]::WriteAllText("$Out\$($change[0]).txt",$run.Output)
        if (!$run.Code -or $run.Output -notmatch 'FAIL line') {throw "Mutation survived: $($change[0])"}
        Write-Output "Mutation caught at runtime: $($change[0])"
    }
}

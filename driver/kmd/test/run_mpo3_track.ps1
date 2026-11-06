# Host test for driver\kmd\mpo3_track.h: the PresentId of plane 0 for the MPO3 flip DDI and the plane rule of its
# Check (BD-307, ADR 0019 stage B4). The test includes the production header, so there is nothing to extract.
#
#   pwsh driver\kmd\test\run_mpo3_track.ps1
#   pwsh driver\kmd\test\run_mpo3_track.ps1 -Mutation all          (every negative control, each must fail)
#   pwsh driver\kmd\test\run_mpo3_track.ps1 -Mutation kept-retires-pending
#
# -Mutation changes one rule in a COPY of mpo3_track.h, never the tree, and the test must then fail with a FAIL line.
# A removed rule keeps its parameters in use, so that /W4 /WX still compiles it. A mutation whose text is no longer in
# the header once stops the run instead of passing quietly. The binaries go under -Out; nothing is written to drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\m9\mpo3-track",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('', 'all', 'immediate-pending', 'kept-retires-pending', 'report-before-shown', 'legacy-keeps-mode',
                 'report-inactive', 'count-without-current', 'count-one', 'layer', 'clip-unchecked',
                 'origin-unchecked', 'rotation', 'post-unchecked', 'flags-mirror-only', 'scaled-keeps-geometry',
                 'scaled-skips-all')][string]$Mutation = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

# Each control: the exact text of one rule in mpo3_track.h, what replaces it, and the rule it breaks.
$mutations = [ordered]@{
    'immediate-pending' = @{ find = 'if (Immediate) {'; with = 'if (((void)Immediate, 0)) {'
        why = 'an immediate flip waits for its surface, though the OS took it as complete on return' }
    'kept-retires-pending' = @{ find = 'if (T->PendingValid) {'; with = 'if (((void)T, 0)) {'
        why = 'a kept request retires a pending flip before the plane reads its surface' }
    'report-before-shown' = @{ find = 'if (T->PendingValid && T->PendingAddress == Shown) {'; with = 'if (T->PendingValid && ((void)Shown, 1)) {'
        why = 'a report names a PresentId whose surface is not shown' }
    'legacy-keeps-mode' = @{ find = 'T->Active = 0u;'; with = 'T->Active = 1u;'
        why = 'a legacy flip leaves the MPO3 report on' }
    'report-inactive' = @{ find = 'if (!T->Active) return 0u;'; with = 'if (((void)T, 0)) return 0u;'
        why = 'a driver that never had an MPO3 request reports MPO3' }
    'count-without-current' = @{ find = 'if (T->CurrentValid) {'; with = 'if (((void)T, 1)) {'
        why = 'a report names a plane with no PresentId' }
    'count-one' = @{ find = 'if (P->PlaneCount != 1u) return BC250_MPO3_COUNT;'
        with = 'if (P->PlaneCount == 0u) return BC250_MPO3_COUNT;'
        why = 'Check approves a second plane' }
    'layer' = @{ find = 'if (P->LayerIndex != 0u) return BC250_MPO3_LAYER;'; with = 'if (((void)P, 0)) return BC250_MPO3_LAYER;'
        why = 'Check approves a layer other than 0' }
    'clip-unchecked' = @{ find = '         !Bc250Mpo3WholeMode(&P->Clip, P->ModeWidth, P->ModeHeight)))'; with = '         0))'
        why = 'Check approves a clipped plane' }
    'scaled-keeps-geometry' = @{ find = 'if (!Scaled &&'; with = 'if (((void)Scaled, 1) &&'
        why = 'the scaled rule refuses every plane that is not the whole mode, so no scaled plane reaches dcn_plane.c' }
    'scaled-skips-all' = @{ find = '    if (P->PlaneCount != 1u) return BC250_MPO3_COUNT;'
        with = '    if (Scaled) return BC250_MPO3_OK;' + "`n" + '    if (P->PlaneCount != 1u) return BC250_MPO3_COUNT;'
        why = 'the scaled rule approves a rotated, blended or second plane' }
    'origin-unchecked' = @{ find = 'return R->Left == 0 && R->Top == 0 && R->Right'; with = 'return R->Right'
        why = 'Check approves a plane that is not at the origin' }
    'rotation' = @{ find = 'if (P->Rotation != BC250_MPO3_ROTATION_IDENTITY) return BC250_MPO3_ROTATION;'
        with = 'if (P->Rotation > BC250_MPO3_ROTATION_IDENTITY) return BC250_MPO3_ROTATION;'
        why = 'Check approves a rotation value 0, which is not D3DDDI_ROTATION_IDENTITY' }
    'post-unchecked' = @{ find = 'if (P->PostPresent &&'; with = 'if (0 && P->PostPresent &&'
        why = 'Check approves a post-composition transform' }
    'flags-mirror-only' = @{ find = 'if (P->Flags != 0u) return BC250_MPO3_FLAGS;'; with = 'if (P->Flags & 3u) return BC250_MPO3_FLAGS;'
        why = 'Check approves a flag bit it does not know' }
}

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $Root 'toolchain\llvm-23.1.4-win64\bin\clang-cl.exe'
& $cl --version
if ($LASTEXITCODE -ne 0) { throw 'Explicit clang-cl unavailable' }
$env:TEMP = "$Root\scratch\tmp"; $env:TMP = $env:TEMP
$env:INCLUDE = ''; $env:LIB = ''

# Builds and runs the test with the header in $Include; returns the exit code and the output.
function Invoke-Mpo3Track([string]$Dir, [string]$Include) {
    $obj = Join-Path $Dir 'obj'
    New-Item -ItemType Directory -Force $Dir, $obj | Out-Null
    Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue
    & $cl @('/nologo', '/TC', '/W4', '/WX', '/O2', '/MT', '-fuse-ld=lld', "/I$Include", "/I$($msvc.FullName)\include",
        "/I$sdk\Include\$KitVersion\ucrt", "/Fo$obj\", "/Fe$Dir\mpo3_track_test.exe", (Join-Path $here 'mpo3_track_test.c'),
        '/link', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64") | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Host build failed ($Dir)" }
    $output = & "$Dir\mpo3_track_test.exe"
    return @{ Code = $LASTEXITCODE; Output = ($output -join "`n") }
}

if (-not $Mutation) {
    $run = Invoke-Mpo3Track (Join-Path $Out 'tree') $kmd
    Write-Output $run.Output
    exit $run.Code
}

$names = if ($Mutation -eq 'all') { @($mutations.Keys) } else { @($Mutation) }
$header = Get-Content -Raw (Join-Path $kmd 'mpo3_track.h')
$passed = @()
foreach ($name in $names) {
    $m = $mutations[$name]
    $count = ([regex]::Matches($header, [regex]::Escape($m.find))).Count
    if ($count -ne 1) { throw "mutation ${name}: its text occurs $count times in mpo3_track.h, not once" }
    $dir = Join-Path $Out "mutation-$name"
    $include = Join-Path $dir 'include'
    New-Item -ItemType Directory -Force $include | Out-Null
    Set-Content -NoNewline -Path (Join-Path $include 'mpo3_track.h') -Value $header.Replace($m.find, $m.with)
    $run = Invoke-Mpo3Track $dir $include
    $failed = $run.Code -ne 0 -and $run.Output -match 'FAIL line'
    Write-Output ("mutation {0}: {1} ({2})" -f $name, $(if ($failed) { 'caught' } else { 'NOT CAUGHT' }), $m.why)
    if (-not $failed) { $passed += $name }
}
if ($passed.Count) { Write-Output "mutations not caught: $($passed -join ', ')"; exit 1 }
Write-Output "mpo3 track mutations: $($names.Count) caught"
exit 0

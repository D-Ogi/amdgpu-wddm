# Host test for driver\kmd\flip_state.h: the decisions of the BD-170 flip state machine (docs/design/vidpn-flip.md
# section 7.2). The test includes the production header, so there is nothing to extract.
#
#   pwsh driver\kmd\test\run_flip_state.ps1
#   pwsh driver\kmd\test\run_flip_state.ps1 -Mutation all          (every negative control, each must fail)
#   pwsh driver\kmd\test\run_flip_state.ps1 -Mutation overlap-one-way
#
# -Mutation changes one rule in a COPY of flip_state.h, never the tree, and the test must then fail with a FAIL line.
# A removed rule keeps its parameters in use (a condition that is never true), so that /W4 /WX still compiles it.
# A mutation whose text is no longer in the header stops the run instead of passing quietly. -Mutation all runs every
# control and fails if any of them passes. The binaries go under -Out; nothing is written to drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\m9\flip-state",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('', 'all', 'faulting-not-first', 'overlap-one-way', 'refused-after-writer', 'vsync-deposit-unproven',
                 'drained-ignores-resumed', 'drained-two-bits', 'immediate-without-proof', 'failed-write-success',
                 'begin-in-faulting', 'end-programs-when-stopped', 'wu-unblank-lost', 'wu-superseded-programmed',
                 'wu-refusal-programmed', 'drain-unbounded')][string]$Mutation = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

# Each control: the exact text of one rule in flip_state.h, what replaces it, and the rule it breaks.
$mutations = [ordered]@{
    'faulting-not-first' = @{ find = 'if (Facts->Faulting) return BC250_FLIP_EDGE_FAULTING;'; with = ''
        why = 'F0: a request in FAULTING is programmed or deposited' }
    'overlap-one-way' = @{ find = 'if ((Facts->GenerationOdd != 0) != (Facts->WriterActive != 0))'
        with = 'if (Facts->GenerationOdd && !Facts->WriterActive)'
        why = 'F1: a writer with an even generation is taken as a writer' }
    'refused-after-writer' = @{ find = 'if (!Facts->Admitted) return BC250_FLIP_EDGE_REFUSED;'
        with = 'if (!Facts->Admitted && !Facts->WriterActive) return BC250_FLIP_EDGE_REFUSED;'
        why = 'A: a refused request is deposited during a writer' }
    'vsync-deposit-unproven' = @{ find = 'if (!Facts->Immediate) return BC250_FLIP_EDGE_DEPOSIT_VSYNC;'
        with = 'if (!Facts->Immediate) return BC250_FLIP_EDGE_DEPOSIT_UNPROVEN;'
        why = 'W-v: a next-VSync request during a writer is recorded as W-u' }
    'drained-ignores-resumed' = @{ find = 'if (Facts->Drained && !Facts->Resumed)'; with = 'if (Facts->Drained)'
        why = 'W-i: a DRAINED readback is used after the writer resumed the plane' }
    'drained-two-bits' = @{ find = 'return ReadOk && BlankEn && TtuDisable && NoOutstanding;'
        with = 'return ReadOk && BlankEn && TtuDisable && ((void)NoOutstanding, 1);'
        why = 'DRAINED without HUBP_NO_OUTSTANDING_REQ' }
    'immediate-without-proof' = @{ find = 'if (Immediate && !Proof) return BC250_FLIP_RESULT_UNPROVEN;'
        with = 'if (((void)Immediate, (void)Proof, 0)) return BC250_FLIP_RESULT_UNPROVEN;'
        why = 'E3: an immediate request gets success without P' }
    'failed-write-success' = @{ find = 'if (!WriteOk) return BC250_FLIP_RESULT_FAULT_PROGRAM;'
        with = 'if (((void)WriteOk, 0)) return BC250_FLIP_RESULT_FAULT_PROGRAM;'
        why = 'a failed write sequence is answered as a programmed flip' }
    'begin-in-faulting' = @{ find = 'if (Faulting) return BC250_WRITER_BEGIN_FAULTING;'
        with = 'if (((void)Faulting, 0)) return BC250_WRITER_BEGIN_FAULTING;'
        why = 'a writer starts in FAULTING' }
    'end-programs-when-stopped' = @{ find = 'if (OtgRunning != 1) return BC250_WRITER_END_FAULT;'
        with = 'if (OtgRunning == 0) return BC250_WRITER_END_FAULT;'
        why = 'End takes a failed OTG read as a running OTG' }
    'wu-unblank-lost' = @{ find = 'if (Unblank) return BC250_WU_UNBLANK;'
        with = 'if (((void)Unblank, 0)) return BC250_WU_UNBLANK;'
        why = 'W-u: a deposit the unblank programmed is recorded by what End did' }
    'wu-superseded-programmed' = @{ find = 'if (!Current) return BC250_WU_SUPERSEDED;'
        with = 'if (((void)Current, 0)) return BC250_WU_SUPERSEDED;'
        why = 'W-u: a superseded deposit is recorded as programmed by End' }
    'wu-refusal-programmed' = @{ find = 'if (Result == BC250_FLIP_RESULT_REFUSED) return BC250_WU_END_REFUSED;'
        with = 'if (Result == BC250_FLIP_RESULT_REFUSED) return BC250_WU_END_PROGRAM;'
        why = 'W-u: a deposit that End refused is recorded as programmed' }
    'drain-unbounded' = @{ find = 'if (Reads >= BC250_FLIP_DRAIN_READS) return BC250_FLIP_DRAIN_UNKNOWN;'
        with = 'if (Reads >= BC250_FLIP_DRAIN_READS && !Reads) return BC250_FLIP_DRAIN_UNKNOWN;'
        why = 'the drain poll has no bound' }
}

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $Root 'toolchain\llvm-23.1.4-win64\bin\clang-cl.exe'
& $cl --version
if ($LASTEXITCODE -ne 0) { throw 'Explicit clang-cl unavailable' }
$env:TEMP = "$Root\scratch\tmp"; $env:TMP = $env:TEMP
$env:INCLUDE = ''; $env:LIB = ''

# Builds and runs the test with the header in $Include; returns the exit code and the output.
function Invoke-FlipState([string]$Dir, [string]$Include, [bool]$Lab) {
    $obj = Join-Path $Dir 'obj'
    New-Item -ItemType Directory -Force $Dir, $obj | Out-Null
    Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue
    $flavour = @()
    if ($Lab) { $flavour += '/DAMDGPU_WDDM_LAB_FLAVOUR=1' }
    & $cl @flavour @('/nologo', '/TC', '/W4', '/WX', '/O2', '/MT', '-fuse-ld=lld', "/I$Include", "/I$($msvc.FullName)\include",
        "/I$sdk\Include\$KitVersion\ucrt", "/Fo$obj\", "/Fe$Dir\flip_state_test.exe", (Join-Path $here 'flip_state_test.c'),
        '/link', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64") | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Host build failed ($Dir)" }
    $output = & "$Dir\flip_state_test.exe"
    return @{ Code = $LASTEXITCODE; Output = ($output -join "`n") }
}

if (-not $Mutation) {
    foreach ($lab in @($false, $true)) {
        $flavour = if ($lab) { 'lab' } else { 'release' }
        $run = Invoke-FlipState (Join-Path $Out $flavour) $kmd $lab
        Write-Output "$flavour`: $($run.Output)"
        if ($run.Code) { exit $run.Code }
    }
    exit 0
}

$names = if ($Mutation -eq 'all') { @($mutations.Keys) } else { @($Mutation) }
$header = Get-Content -Raw (Join-Path $kmd 'flip_state.h')
$passed = @()
foreach ($name in $names) {
    $m = $mutations[$name]
    $count = ([regex]::Matches($header, [regex]::Escape($m.find))).Count
    if ($count -ne 1) { throw "mutation ${name}: its text occurs $count times in flip_state.h, not once" }
    $dir = Join-Path $Out "mutation-$name"
    $include = Join-Path $dir 'include'
    New-Item -ItemType Directory -Force $include | Out-Null
    Set-Content -NoNewline -Path (Join-Path $include 'flip_state.h') -Value $header.Replace($m.find, $m.with)
    foreach ($lab in @($false, $true)) {
        $flavour = if ($lab) { 'lab' } else { 'release' }
        $run = Invoke-FlipState (Join-Path $dir $flavour) $include $lab
        $failed = $run.Code -ne 0 -and $run.Output -match 'FAIL line'
        Write-Output ("mutation {0} ({1}): {2} ({3})" -f $name, $flavour, $(if ($failed) { 'caught' } else { 'NOT CAUGHT' }), $m.why)
        if (-not $failed) { $passed += "$name/$flavour" }
    }
}
if ($passed.Count) { Write-Output "mutations not caught: $($passed -join ', ')"; exit 1 }
Write-Output "flip state mutations: $($names.Count) caught"
exit 0

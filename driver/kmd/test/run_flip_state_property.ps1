# Builds and runs the flip state machine property test (driver\kmd\test\flip-state-property-test.cpp): the
# invariants of driver\kmd\flip_state.h over generated command sequences, with RapidCheck.
#
#   pwsh driver\kmd\test\run_flip_state_property.ps1 -Out P:\BC-250\scratch\flip-property
#   pwsh driver\kmd\test\run_flip_state_property.ps1 -Mutation all     (every negative control, each must fail)
#   pwsh driver\kmd\test\run_flip_state_property.ps1 -Mutation fault-not-absorbing
#
# driver\kmd\test\run_flip_state.ps1 is the per-decision test of the same header: it drives each edge once. This
# runner is the sequence test. Both are gates, and neither replaces the other.
#
# -Seed fixes the RapidCheck seed, so the gate runs the same sequences on every machine and a failure in the log is
# reproducible. Raise -MaxSuccess to search further by hand; the gate keeps the fixed number so its cost is known.
#
# -Mutation breaks one rule in a COPY of flip_state.h, never in the tree, and the test must then fail. The copy goes
# into a private include root that the compiler reads before the repository, so the test source is untouched too. A
# mutation keeps every parameter in use (a condition that is never true), so that /W4 /WX still compiles it, and a
# mutation whose text is no longer in the header stops the run instead of passing quietly.
#
# Everything is written under -Out, never into the repository and never onto drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\flip-state-property",
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
    [string]$VsInstall = '',
    [int]$Seed = 20261126,
    [int]$MaxSuccess = 500,
    [ValidateSet('', 'all', 'fault-not-absorbing', 'writer-does-not-protect', 'end-programs-when-stopped',
                 'drain-unbounded', 'begin-in-faulting', 'wu-fault-programmed')][string]$Mutation = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$env:BC250_ROOT = $Root
. (Join-Path $repo 'tools\build\common.ps1')
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
. (Join-Path $repo 'tools\quality\rapidcheck-lib.ps1')

# Each control: the exact text of one rule in flip_state.h, what replaces it, and the invariant it breaks.
# A rule that is one statement is deleted by a (void) cast of its own condition. That keeps every argument of the
# function in use, so the copy still compiles under /W4 /WX. A plain "&& 0" does not compile: clang refuses a
# constant operand of && (-Wconstant-logical-operand). The one rule that opens a block is inverted instead.
$controls = @(
    @{ name = 'fault-not-absorbing'
       find = '    if (Facts->Faulting) return BC250_FLIP_EDGE_FAULTING;'
       with = '    (void)(Facts->Faulting);'
       rule = 'I2: after E5 every flip answers F0' }
    @{ name = 'writer-does-not-protect'
       find = '    if (Facts->WriterActive) {'
       with = '    if (!Facts->WriterActive) {'
       rule = 'I3: an admitted flip during a writer is a deposit, never E (this control inverts the rule)' }
    @{ name = 'end-programs-when-stopped'
       find = '    if (OtgRunning != 1) return BC250_WRITER_END_FAULT;'
       with = '    (void)(OtgRunning != 1);'
       rule = 'I4: End with work owed and the OTG not running is E5' }
    @{ name = 'drain-unbounded'
       find = '    if (Reads >= BC250_FLIP_DRAIN_READS) return BC250_FLIP_DRAIN_UNKNOWN;'
       with = '    (void)(Reads >= BC250_FLIP_DRAIN_READS);'
       rule = 'I6: the E5 drain ends' }
    @{ name = 'begin-in-faulting'
       find = '    if (Faulting) return BC250_WRITER_BEGIN_FAULTING;'
       with = '    (void)(Faulting);'
       rule = 'I2: no writer takes the primary after E5' }
    @{ name = 'wu-fault-programmed'
       find = '    if (Answer == BC250_WRITER_END_FAULT) return BC250_WU_END_FAULT;'
       with = '    (void)(Answer == BC250_WRITER_END_FAULT);'
       rule = 'the W-u record of a faulted End is never a programmed deposit' }
)

# Builds the test against $IncludeRoots (first match wins) and runs it. Returns the exit code.
function Invoke-FlipStateProperty([string]$Work, [string[]]$IncludeRoots) {
    New-Item -ItemType Directory -Force $Work | Out-Null
    $Work = (Resolve-Path -LiteralPath $Work).Path
    $saved = Save-ProcessEnvironment
    try {
        $env:TEMP = $Work; $env:TMP = $Work
        $null = Import-VsDevEnvironment -TempDir $Work -Arch $Arch
        $cc = Resolve-Bc250UmdCompiler $Root 'clang-cl' '' $Work $Arch
        # NOMINMAX: the host-test flags force-include a header that reaches windows.h, and its min and max macros
        # break std::numeric_limits<T>::max() in the library's own headers, which this test includes.
        $base = Get-Bc250HostTestFlags 'clang-cl' @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MD', '/O2',
            '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN') @((Get-Bc250RapidCheckInclude $repo)) $Arch @($repo)
        # The library is built once per architecture and cached; it is third-party and keeps its own warning level.
        $objects = Build-Bc250RapidCheck $repo $cc $base (Join-Path $Out "rapidcheck-$Arch")
        $flags = @($base) + @($IncludeRoots | ForEach-Object { "/I$_" })
        Push-Location $Work
        try {
            # Out-Host on both calls, never the pipeline: a function returns its whole output stream, so a line
            # left in the pipeline would be returned to the caller together with the exit code below.
            Invoke-Bc250UmdCompiler $cc @flags /Fe:flip-state-property-test.exe `
                (Join-Path $here 'flip-state-property-test.cpp') @objects | Out-Host
            if ($LASTEXITCODE -ne 0) { throw 'flip-state-property-test build failed' }
            # RapidCheck reads its parameters from RC_PARAMS. The seed is fixed so the gate is reproducible.
            $env:RC_PARAMS = "seed=$Seed max_success=$MaxSuccess"
            & .\flip-state-property-test.exe | Out-Host
            return $LASTEXITCODE
        } finally { Pop-Location }
    } finally { Restore-ProcessEnvironment $saved }
}

# Writes a copy of flip_state.h with one rule broken into a private include root, and returns that root.
function New-MutatedHeader([hashtable]$Control, [string]$Work) {
    $source = Join-Path $repo 'driver\kmd\flip_state.h'
    $text = [IO.File]::ReadAllText($source)
    $count = ([regex]::Matches($text, [regex]::Escape($Control.find))).Count
    if ($count -ne 1) {
        throw "control '$($Control.name)': the text of that rule is in flip_state.h $count times, not once. The rule moved or changed: fix the control before trusting this gate."
    }
    # Not $root: a variable name in PowerShell is case insensitive, so $root and the $Root parameter of this
    # script are one variable, and naming it $root here sent the compiler search to the mutated include root.
    $mutatedRoot = Join-Path $Work 'mutated'
    $dir = Join-Path $mutatedRoot 'driver\kmd'
    New-Item -ItemType Directory -Force $dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir 'flip_state.h'), $text.Replace($Control.find, $Control.with))
    return $mutatedRoot
}

New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path

if (-not $Mutation) {
    $code = Invoke-FlipStateProperty (Join-Path $Out $Arch) @($repo)
    if ($code -ne 0) { throw "flip-state-property-test failed (exit $code)" }
    Write-Host "PASS flip state machine properties (seed $Seed, $MaxSuccess sequences, $Arch)"
    exit 0
}

$wanted = if ($Mutation -eq 'all') { $controls } else { @($controls | Where-Object { $_.name -eq $Mutation }) }
$failed = @()
foreach ($control in $wanted) {
    $work = Join-Path $Out "mutation-$($control.name)"
    $mutatedRoot = New-MutatedHeader $control $work
    $code = Invoke-FlipStateProperty $work @($mutatedRoot, $repo)
    if ($code -eq 0) {
        Write-Host "CONTROL PASSED, WHICH IS A FAILURE: $($control.name) ($($control.rule))"
        $failed += $control.name
    } else {
        Write-Host "control ok: $($control.name) fails as it must ($($control.rule))"
    }
}
if ($failed) { throw "negative controls that the test did not catch: $($failed -join ', ')" }
Write-Host "PASS $($wanted.Count) negative controls, each caught"
exit 0

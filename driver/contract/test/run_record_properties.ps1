# Builds and runs the record property test (driver\contract\test\record-property-test.cpp): the rules of the shared
# linear surface records as properties, with RapidCheck.
#
#   pwsh driver\contract\test\run_record_properties.ps1 -Out P:\BC-250\scratch\record-properties
#   pwsh driver\contract\test\run_record_properties.ps1 -Arch x86
#   pwsh driver\contract\test\run_record_properties.ps1 -Mutation all   (every negative control, each must fail)
#
# driver\contract\test\run_record_pairs.ps1 is the matrix gate of the same contract: every producer against every
# consumer over a chosen list of sizes and formats. This runner is the generative gate. A matrix finds what the
# author of the matrix thought of; a property finds the case nobody listed, and RapidCheck shrinks it to a small one.
#
# -Seed fixes the RapidCheck seed, so the gate runs the same cases on every machine and a failure in the log is
# reproducible. Raise -MaxSuccess to search further by hand; the gate keeps the fixed number so its cost is known.
#
# -Mutation breaks one rule in a COPY of bc250_shared_surface.h, never in the tree, and the test must then fail. The
# copy goes into a private include root that the compiler reads before the repository, so the test source is
# untouched too. Each mutation keeps every argument in use (a condition that is never true), so that /W4 /WX still
# compiles it, and a mutation whose text is no longer in the header stops the run instead of passing quietly.
#
# Everything is written under -Out, never into the repository and never onto drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\record-properties",
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
    [string]$VsInstall = '',
    [int]$Seed = 20261126,
    [int]$MaxSuccess = 500,
    [ValidateSet('', 'all', 'decode-loses-the-pitch', 'refusal-says-ok', 'decode-drops-the-video-binds',
                 'shared-not-required', 'pitch-refuses-an-exact-row', 'encoder-drops-displayable')][string]$Mutation = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$env:BC250_ROOT = $Root
. (Join-Path $repo 'tools\build\common.ps1')
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
. (Join-Path $repo 'tools\quality\rapidcheck-lib.ps1')

# Each control: the exact text of one rule in bc250_shared_surface.h, what replaces it, and the property it breaks.
$controls = @(
    @{ name = 'decode-loses-the-pitch'
       find = '    out.Pitch = a.Pitch;'
       with = '    out.Pitch = a.Width;'
       rule = 'P1 and P6: the opener reads back the creator''s own pitch' }
    @{ name = 'refusal-says-ok'
       find = '        why = BC250_SHARED_SURFACE_WHY_SUBRESOURCES;'
       with = '        why = BC250_SHARED_SURFACE_WHY_OK;'
       rule = 'P4: a refusal never answers the name of a successful decode' }
    @{ name = 'decode-drops-the-video-binds'
       find = '        if ((r.BindFlags & ~BC250_SHARED_BIND_MASK) || (r.MiscFlags & ~BC250_SHARED_MISC_MASK))'
       with = '        if ((r.BindFlags & ~(BC250_SHARED_BIND_MASK & ~(BC250_SHARED_BIND_DECODER | BC250_SHARED_BIND_VIDEO_ENCODER))) || (r.MiscFlags & ~BC250_SHARED_MISC_MASK))'
       rule = 'P2: the open admits every record the encoder writes (the BD-413 class)' }
    @{ name = 'shared-not-required'
       find = '        if (Admit->RequireShared && r.Shared != 1)'
       with = '        if (Admit->RequireShared && r.Shared != 1 && 0)'
       rule = 'P5: a record that shares nothing is refused, and the caller''s surface stays untouched' }
    @{ name = 'pitch-refuses-an-exact-row'
       find = '    if ((unsigned long long)Surface->Width * BytesPerPixel > Surface->Pitch)'
       with = '    if ((unsigned long long)Surface->Width * BytesPerPixel >= Surface->Pitch)'
       rule = 'P1: a pitch that is exactly one row of pixels is a valid pitch' }
    @{ name = 'encoder-drops-displayable'
       find = '    if (Shared > 1 || (Access & ~BC250_SURFACE_RESOURCE_ACCESS_MASK))'
       with = '    if (Shared > 1 || (Access & ~(BC250_SURFACE_RESOURCE_ACCESS_MASK & ~BC250_SURFACE_RESOURCE_DISPLAYABLE)))'
       rule = 'P3: the encoder admits exactly the access words the kernel parser admits' }
)

# Builds the test against $IncludeRoots (first match wins) and runs it. Returns the exit code.
function Invoke-RecordProperty([string]$Work, [string[]]$IncludeRoots) {
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
            '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN') @(
            "$repo\driver\contract\third_party", "$repo\driver\contract\uapi-shim",
            (Get-Bc250RapidCheckInclude $repo)) $Arch @($repo)
        # The library is built once per architecture under -Out and cached, not once per run: it is third-party
        # code that no mutation of our header can reach, so every run of this gate, the negative controls
        # included, links the same objects. It also keeps its own warning level.
        $objects = Build-Bc250RapidCheck $repo $cc $base (Join-Path $Out "rapidcheck-$Arch")
        $flags = @($base) + @($IncludeRoots | ForEach-Object { "/I$_" })
        Push-Location $Work
        try {
            # Out-Host on both calls, never the pipeline: a function returns its whole output stream, so a line
            # left in the pipeline would be returned to the caller together with the exit code below.
            Invoke-Bc250UmdCompiler $cc @flags /Fe:record-property-test.exe `
                (Join-Path $here 'record-property-test.cpp') @objects | Out-Host
            if ($LASTEXITCODE -ne 0) { throw 'record-property-test build failed' }
            # RapidCheck reads its parameters from RC_PARAMS. The seed is fixed so the gate is reproducible.
            $env:RC_PARAMS = "seed=$Seed max_success=$MaxSuccess"
            & .\record-property-test.exe | Out-Host
            return $LASTEXITCODE
        } finally { Pop-Location }
    } finally { Restore-ProcessEnvironment $saved }
}

# Writes a copy of bc250_shared_surface.h with one rule broken into a private include root, and returns that root.
function New-MutatedHeader([hashtable]$Control, [string]$Work) {
    $source = Join-Path $repo 'driver\contract\bc250_shared_surface.h'
    $text = [IO.File]::ReadAllText($source)
    $count = ([regex]::Matches($text, [regex]::Escape($Control.find))).Count
    if ($count -ne 1) {
        throw "control '$($Control.name)': the text of that rule is in bc250_shared_surface.h $count times, not once. The rule moved or changed: fix the control before trusting this gate."
    }
    # A quoted include resolves against the directory of the file that states it, which for the copy is the
    # private root and not driver\contract. Each relative include of the copy is therefore rewritten to the
    # absolute path of the real file, so that the copy stays a change of one rule and nothing else.
    $sourceDir = Split-Path -Parent $source
    $text = [regex]::Replace($text, '(?m)^(\s*#\s*include\s*")([^"]+)(")', {
        param($m)
        $target = Join-Path $sourceDir $m.Groups[2].Value
        if (-not (Test-Path -LiteralPath $target)) {
            throw "the copy cannot resolve the include $($m.Groups[2].Value) of bc250_shared_surface.h"
        }
        $m.Groups[1].Value + ((Resolve-Path -LiteralPath $target).Path -replace '\\', '/') + $m.Groups[3].Value
    })
    # Not $root: a variable name in PowerShell is case insensitive, so $root and the $Root parameter of this
    # script are one variable, and naming it $root here sent the compiler search to the mutated include root.
    $mutatedRoot = Join-Path $Work 'mutated'
    $dir = Join-Path $mutatedRoot 'driver\contract'
    New-Item -ItemType Directory -Force $dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir 'bc250_shared_surface.h'), $text.Replace($Control.find, $Control.with))
    return $mutatedRoot
}

New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path

if (-not $Mutation) {
    $code = Invoke-RecordProperty (Join-Path $Out $Arch) @($repo)
    if ($code -ne 0) { throw "record-property-test failed (exit $code)" }
    Write-Host "PASS shared surface record properties (seed $Seed, $MaxSuccess cases each, $Arch)"
    exit 0
}

$wanted = if ($Mutation -eq 'all') { $controls } else { @($controls | Where-Object { $_.name -eq $Mutation }) }
$failed = @()
foreach ($control in $wanted) {
    $work = Join-Path $Out "mutation-$($control.name)"
    $mutatedRoot = New-MutatedHeader $control $work
    # The mutated root comes first, so the copy shadows the tree's header without a change to the test source.
    $code = Invoke-RecordProperty $work @($mutatedRoot, $repo)
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

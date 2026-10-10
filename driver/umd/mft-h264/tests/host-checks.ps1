# Host checks of the H.264 encoder MFT that need no GPU, no Media Foundation and no build: they read
# the component's own sources and hold the conformance sweep against them.
#
# What they are for. The deblocking filter has three schedules (`DeblockMode` in src\gpu_pipeline.h),
# one of them the default and the other two selected by a caller. Every schedule has to stay held to
# the decoder oracle, and the holder is tests\sweep.ps1: the default by every case that names no
# shape, the other two by a case each. On 2026-10-10 the default moved from Rows to Waves (audit
# finding A11 / UMD-3) and the sweep kept its explicit `waves` case, which left Rows - a shape the
# README still offers for a measurement - with no byte-equality coverage anywhere in the repository.
# A check that reads the default out of the source and the cases out of the sweep catches that the
# next time the default moves.
#
# Usage: pwsh -NoProfile -File driver\umd\mft-h264\tests\host-checks.ps1 [-Sweep <path>]
#
# -Sweep takes another copy of the sweep script, which is how the negative control runs this gate
# against the case list of an earlier revision.

[CmdletBinding()]
param(
    [string]$Sweep,
    [string]$Source,
    [string]$HostTest
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$component = (Resolve-Path (Join-Path $here '..')).Path
if (-not $Sweep)  { $Sweep  = Join-Path $here 'sweep.ps1' }
if (-not $Source) { $Source = Join-Path $component 'src' }
if (-not $HostTest) { $HostTest = Join-Path $here 'mfthost.cpp' }

$checks = 0
$failed = 0
function Ok([string]$name, [string]$detail) {
    $script:checks++
    "  {0,-34} {1}" -f $name, $detail
}
function Bad([string]$name, [string]$detail) {
    $script:checks++
    $script:failed++
    "  {0,-34} FAIL {1}" -f $name, $detail
}

$header = Get-Content -LiteralPath (Join-Path $Source 'gpu_pipeline.h') -Raw
$impl = Get-Content -LiteralPath (Join-Path $Source 'gpu_pipeline.cpp') -Raw
$sweepText = Get-Content -LiteralPath $Sweep -Raw
$hostText = Get-Content -LiteralPath $HostTest -Raw

"deblock schedule coverage"

# 1. The shapes the source admits, from the enumerator itself.
$enum = [regex]::Match($header, 'enum class DeblockMode\s*:\s*\w+\s*\{([^}]*)\}')
$modes = @()
if ($enum.Success) {
    $modes = @([regex]::Matches($enum.Groups[1].Value, '(\w+)\s*=\s*\d+') |
               ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() })
}
if ($modes.Count -ge 2) {
    Ok 'the shapes of DeblockMode' ("{0}: {1}" -f $modes.Count, ($modes -join ', '))
} else {
    Bad 'the shapes of DeblockMode' "gpu_pipeline.h has no DeblockMode enumerator with members"
}

# 2. Which one a process encodes with when the environment names none: the initial value inside
#    DeblockModeFromEnvironment, which is the one function that answers that question.
$selector = [regex]::Match($impl, 'DeblockMode DeblockModeFromEnvironment\(\)\s*\{(.*?)\n\}', 'Singleline')
$default = ''
if ($selector.Success) {
    $init = [regex]::Match($selector.Groups[1].Value, 'DeblockMode\s+\w+\s*=\s*DeblockMode::(\w+)')
    if ($init.Success) { $default = $init.Groups[1].Value.ToLowerInvariant() }
}
if ($default -and ($modes -contains $default)) {
    Ok 'the default shape' $default
} else {
    Bad 'the default shape' "DeblockModeFromEnvironment in gpu_pipeline.cpp names no DeblockMode member"
}

# 3. The names mfthost accepts for --deblock-mode, so that a sweep case cannot ask for a shape the
#    host test would refuse with exit code 2.
$argBlock = [regex]::Match($hostText,
    'wcscmp\(a, L"--deblock-mode"\)(.*?)wcscmp\(a, L"--(?!deblock-mode)', 'Singleline')
$accepted = @()
if ($argBlock.Success) {
    $accepted = @([regex]::Matches($argBlock.Groups[1].Value, 'wcscmp\(m, L"(\w+)"\)') |
                  ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } | Sort-Object -Unique)
}
if ($accepted.Count -ge 2) {
    Ok 'the names --deblock-mode takes' ($accepted -join ', ')
} else {
    Bad 'the names --deblock-mode takes' "mfthost.cpp has no --deblock-mode argument block"
}

# 4. The sweep's case list: which shape each case asks for, and how many ask for none.
$caseBlock = [regex]::Match($sweepText, '\$cases\s*=\s*@\((.*?)\n\)', 'Singleline')
$explicit = @()
$implicit = 0
if ($caseBlock.Success) {
    foreach ($line in ($caseBlock.Groups[1].Value -split "`n")) {
        if ($line -notmatch '^\s*@\(') { continue }
        $named = [regex]::Match($line, "--deblock-mode'\s*,\s*'(\w+)'")
        if ($named.Success) { $explicit += $named.Groups[1].Value.ToLowerInvariant() } else { $implicit++ }
    }
    $explicit = @($explicit)
    Ok 'the sweep cases' ("{0} name a shape ({1}), {2} take the default" -f
        $explicit.Count, (($explicit | Sort-Object -Unique) -join ', '), $implicit)
} else {
    Bad 'the sweep cases' "$Sweep has no `$cases list"
}

# 5. Every shape the source admits is held to the decoder oracle by at least one case: the default by
#    a case that names nothing, each other shape by a case that names it.
$held = @($explicit | Sort-Object -Unique)
if ($implicit -gt 0 -and $default) { $held = @($held + $default | Sort-Object -Unique) }
$missing = @($modes | Where-Object { $held -notcontains $_ })
if ($missing.Count -eq 0) {
    Ok 'every shape has a case' ("{0} shapes, all held to the inbox decoder" -f $modes.Count)
} else {
    Bad 'every shape has a case' ("no sweep case encodes with: {0}. Add --deblock-mode <shape> to one case" -f ($missing -join ', '))
}

# 6. And no case asks for a name the host test refuses.
$unknown = @($explicit | Sort-Object -Unique | Where-Object { $accepted -notcontains $_ })
if ($unknown.Count -eq 0) {
    Ok 'every case name is accepted' "no case asks for a shape mfthost would refuse"
} else {
    Bad 'every case name is accepted' ("mfthost refuses: {0}" -f ($unknown -join ', '))
}

""
"host-checks: $checks checks, $failed failed"
if ($failed -gt 0) { exit 1 }

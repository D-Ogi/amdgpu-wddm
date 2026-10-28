#Requires -Version 7.0
<#
.SYNOPSIS
The release builder passes the right manifest to the flavour selection, the commit check and the work ledger.

.DESCRIPTION
Since the build has two flavours, two manifests are in play and they are not interchangeable:

  the whole manifest (tools\release\release-sources.json), which names the rows of BOTH flavours, is what
  every gate that judges the RELEASE must read. The ledger counts a fork entry as landed only when its commit
  is an ancestor of a release pin, and a ledger entry is not per flavour: given one flavour's subset, the
  ledger calls the other flavour's integrations unlanded, the build refuses for the wrong reason, and the way
  out would be -AllowLedgerDebt, which is meant for real debt. The commit check reads it for the same reason:
  a commit id of a lab row is named by this release whether or not this build stages it.

  the flavour's selection, written under -Out, which names the rows THIS build stages.

A path can be passed and still be the wrong one, so this does not look for a string. It parses
build-release.ps1 and demands that the three calls name the variables they should:
the selection reads the whole manifest and writes somewhere else (never over the manifest itself), and the
commit check and the ledger both read the whole manifest. It fails when a call is absent, when its flag is
absent, and when either one resolves a path of its own.
#>
[CmdletBinding()]
param(
    [string]$Script = (Join-Path $PSScriptRoot 'build-release.ps1'),
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
if ($Out) { [void](New-Item -ItemType Directory -Force -Path $Out) }

$problems = @()
$errors = $null
$tokens = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $Script).Path, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw "$Script does not parse: $($errors[0].Message)" }

# The variable that holds the whole manifest: the one assigned Join-Path $PSScriptRoot 'release-sources.json'.
# The path is resolved once in the whole script and passed on from there. A second resolution is the failure
# itself: a call that builds the path for itself cannot be held to the manifest this build judges.
$manifestVariable = $null
$resolutions = @($ast.FindAll({ $args[0] -is [System.Management.Automation.Language.CommandAst] }, $true) |
    Where-Object { $_.Extent.Text -match "^Join-Path\s+\`$PSScriptRoot\s+'release-sources\.json'$" })
if ($resolutions.Count -ne 1) {
    $problems += ("tools\release\release-sources.json is resolved $($resolutions.Count) time(s) in this script: " +
                  'expected once, into a variable that every gate judging the release is then given')
} else {
    $assignment = $resolutions[0].Parent
    while ($assignment -and $assignment -isnot [System.Management.Automation.Language.AssignmentStatementAst]) {
        $assignment = $assignment.Parent
    }
    if ($assignment -and $assignment.Right.Extent.Text -eq $resolutions[0].Extent.Text) {
        $manifestVariable = $assignment.Left.Extent.Text.TrimStart('$')
    } else {
        $problems += ("the one resolution of release-sources.json is not an assignment to a variable " +
                      "(`$<name> = Join-Path `$PSScriptRoot 'release-sources.json'), so no gate can be given it")
    }
}

# One call of each of the three, found by the tool it runs. Invoke-Headless takes its arguments as an array, so
# the flags live inside the command's extent and are read from there.
function Get-OneCall([string]$tool, [string]$verb, [string]$flag) {
    # The tool's path is built by a Join-Path of its own, which is a command nested in the arguments of the call:
    # the call is the shortest of the matches that carries the flag as well.
    $found = @($ast.FindAll({ $args[0] -is [System.Management.Automation.Language.CommandAst] }, $true) |
        Where-Object { $_.Extent.Text -match [regex]::Escape($tool) } |
        Where-Object { -not $verb -or $_.Extent.Text -match [regex]::Escape($verb) } |
        Where-Object { $_.Extent.Text -match [regex]::Escape($flag) })
    if (-not $found.Count) { return $null }
    return (@($found | Sort-Object { $_.Extent.Text.Length })[0]).Extent.Text
}

function Get-Flag([string]$call, [string]$flag) {
    $m = [regex]::Match($call, [regex]::Escape($flag) + "'?,?\s+\`$(\w+)")
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}

$calls = @(
    @{ name = 'the flavour selection'; tool = 'release_flavour.py'; verb = ''; flag = '--sources' }
    @{ name = 'the commit check';      tool = 'provenance.py';      verb = "'commits'"; flag = '--sources' }
    @{ name = 'the ledger gate';       tool = '$ledgerTool';        verb = 'check';     flag = '--release-sources' }
)
foreach ($c in $calls) {
    $call = Get-OneCall $c.tool $c.verb $c.flag
    if (-not $call) { $problems += "no call of $($c.name) ($($c.tool) $($c.verb)) with $($c.flag)"; continue }
    $passed = Get-Flag $call $c.flag
    if (-not $passed) { $problems += "$($c.name) has no $($c.flag) with a variable for its value"; continue }
    if ($manifestVariable -and $passed -ne $manifestVariable) {
        $problems += ("$($c.name) passes `$$passed to $($c.flag), and the whole manifest is " +
                      "`$$manifestVariable`: it judges another build than this one")
    }
    if ($c.tool -eq 'release_flavour.py') {
        $flavourOut = Get-Flag $call '--out'
        if (-not $flavourOut) { $problems += 'the flavour selection has no --out with a variable for its value' }
        elseif ($manifestVariable -and $flavourOut -eq $manifestVariable) {
            $problems += 'the flavour selection writes its subset over the whole manifest'
        }
    }
}

foreach ($p in $problems) { Write-Host "FAIL $p" }
if ($problems.Count) {
    Write-Host "FAIL: $($problems.Count) problem(s) in the manifest paths of $Script"
    exit 1
}
Write-Host "PASS: the flavour selection, the commit check and the ledger gate all read `$$manifestVariable"
exit 0

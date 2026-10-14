#Requires -Version 7.0
<#
.SYNOPSIS
Write the version resource of the files a component build ships (BD-166), as a command of its own.

.DESCRIPTION
The helper itself is Set-Bc250VersionStamp in common.ps1 next to this script, and the builds in this directory
call it straight. The component builds elsewhere in the repository (the HIP runtime, the media foundation
transform, the two measuring tools) must not dot-source common.ps1: that file also turns on Set-StrictMode 3.0
for the caller's whole scope, which those scripts were never written for. They call this script instead. The
strict mode then lives inside this one process and the contract is the same one the release reads back with
`versionstamp.py check --signed`.

  pwsh -NoProfile -File tools\build\version-stamp.ps1 -Version 0.7.216.102-tester.31 [-Flavour release|lab]
       [-Names bc250d3d.dll,...] [-OnlyIfShipped] <file> [<file> ...]

-Names gives the name each file is shipped as, in the same order, when a build output is named otherwise; the
resource names the file it belongs to. -OnlyIfShipped leaves a file whose name no release describes alone and
says so, for a build that also produces lab tools. Without -Version the run stops with the message the helper
gives, because the package version belongs to the release, not to this repository.
#>
param(
    [string]$Version = '',
    [ValidateSet('release', 'lab')][string]$Flavour = 'release',
    [string[]]$Names = @(),
    [switch]$OnlyIfShipped,
    # The only parameter this script takes by position, so that a file name can never bind to -Flavour.
    [Parameter(Mandatory, Position = 0, ValueFromRemainingArguments)][string[]]$Files
)
$ErrorActionPreference = 'Stop'
trap { Write-Error $_; exit 1 }
. (Join-Path $PSScriptRoot 'common.ps1')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Bc250VersionStamp -Repo $repo -Files $Files -Names $Names -Version $Version -Flavour $Flavour `
    -OnlyIfShipped:$OnlyIfShipped

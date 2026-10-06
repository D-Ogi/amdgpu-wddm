#Requires -Version 7.0
# Release gate: the kernel driver of this repository compiles and links with the WDK of this workspace.
#
#   pwsh -File tools\release\test-kmd-compile.ps1 [-Root <BC250_ROOT>] [-Out <directory>] [-KitVersion <v>]
#
# Why this is a gate of its own. Until 0.7.213 tools\quality\quick.ps1 ran driver\kmd\build.ps1 with
# -ExportCommandsOnly under the name 'kmd-commands': it wrote compile_commands.json and compiled nothing. The
# static-analysis checks that follow read that file, so a type error, a missing declaration or a C_ASSERT that
# does not hold in driver\kmd or driver\shim passed every gate in the list and was found only when somebody built
# a driver package by hand. driver\kmd\dpm.c is the file this matters most for: it carries three C_ASSERTs that
# bind the escape structures to their sizes. That check is now 'kmd-compile' and compiles; this gate is the same
# compile on the release path, so a release is never cut from sources only the quality list has seen.
#
# What it does: driver\kmd\build.ps1 -CompileOnly, which is the same three cl.exe calls, the same link and the
# same stack-budget check as a package build, without the quality gates (they call this), the identity manifest,
# the catalog and the signature. It writes nothing outside -Out and installs nothing.
#
# build-release.ps1 runs it on the repository it builds from. It does not prove that the .sys in the package came
# from this tree - the package build records that, and release-sources.json holds the hash - but it does prove
# that the sources this release is cut from still build.
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path (Split-Path (Split-Path $PSScriptRoot)) }),
    [string]$Out = '',
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
$kits = Join-Path $Root 'toolchain\nuget'
if (-not (Test-Path -LiteralPath (Join-Path $kits 'microsoft.windows.wdk.x64\c'))) {
    throw "no WDK in $kits (BC250_ROOT=$Root): the kernel compile gate cannot run"
}
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\release-kmd-compile' }
$null = New-Item -ItemType Directory -Force $Out
$start = Get-Date
& pwsh -NoProfile -File (Join-Path $repo 'driver\kmd\build.ps1') -Kits $kits -Out $Out -KitVersion $KitVersion -CompileOnly
$code = $LASTEXITCODE
'  {0} s' -f [int]((Get-Date) - $start).TotalSeconds
if ($code -ne 0) { Write-Error "the kernel driver of $repo does not compile and link ($code)"; exit 1 }
$sys = Join-Path $Out 'bc250kmd.unsigned.sys'
if (-not (Test-Path -LiteralPath $sys)) { Write-Error "the compile gate produced no $sys"; exit 1 }
'PASS: driver\kmd and driver\shim compile and link with the WDK in {0}' -f $kits
exit 0

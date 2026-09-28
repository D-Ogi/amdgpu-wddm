# Entry point for an independent scheduled task, after host admission.
param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string]$ManifestSha256)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\verify-stage.ps1"
$manifest=Assert-KmdStage $Directory $ManifestSha256
$tool=Join-Path $Directory 'bounded-child.exe'
& "$PSScriptRoot\watch.ps1" -Directory $Directory -Tool $tool
exit $LASTEXITCODE

param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$Revision,
    [string]$VsInstall
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) { throw 'Use a new output directory.' }
$saved = Save-ProcessEnvironment
try {
    $env:TEMP = "$OutputDir-tmp"
    $env:TMP = $env:TEMP
    $null = Import-VsDevEnvironment $VsInstall $env:TEMP
    $arguments = @("$PSScriptRoot\radv-paging-test.py", '--source', $Source, '--output', $OutputDir)
    if ($Revision) { $arguments += @('--revision', $Revision) }
    & python @arguments
    if ($LASTEXITCODE) { throw 'Paging contract tests failed; see record.json and build/result logs.' }
} finally {
    Restore-ProcessEnvironment $saved
}

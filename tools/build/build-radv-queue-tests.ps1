param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Build,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$VsInstall
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) { throw 'Use a new output directory; previous results are immutable.' }
$saved = Save-ProcessEnvironment
try {
    # Keep compiler scratch separate from the new result directory.
    $tmp = "$OutputDir-tmp"
    $env:TEMP = $tmp
    $env:TMP = $tmp
    $null = Import-VsDevEnvironment $VsInstall $tmp
    python "$PSScriptRoot\radv-queue-tests.py" --source $Source --build $Build --output $OutputDir
    if ($LASTEXITCODE) { throw 'RADV queue tests failed; see output directory.' }
} finally {
    Restore-ProcessEnvironment $saved
}

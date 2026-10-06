param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Build,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$Python,
    [string]$VsInstall
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) { throw 'Use a new output directory; previous results are immutable.' }
$pythonExe = Resolve-Tool $Python 'python'
if (-not $pythonExe) { throw 'python not found: put it on PATH or pass -Python' }
$saved = Save-ProcessEnvironment
try {
    # Keep compiler scratch separate from the new result directory.
    $tmp = "$OutputDir-tmp"
    $env:TEMP = $tmp
    $env:TMP = $tmp
    $null = Import-VsDevEnvironment $VsInstall $tmp
    & $pythonExe "$PSScriptRoot\radv-unorm10-export-test.py" --source $Source --build $Build --output $OutputDir
    if ($LASTEXITCODE) { throw 'RADV UNORM10 export test failed; see output directory.' }
} finally {
    Restore-ProcessEnvironment $saved
}

# The BO lifetime contract test of a built WDDM2 winsys (BD-201, BD-212): radv-bo-lifetime-test.py in the
# Visual Studio environment of the build's architecture.
param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Build,
    [Parameter(Mandatory)][string]$OutputDir,
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
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
    $null = Import-VsDevEnvironment $VsInstall $tmp $Arch
    python -I "$PSScriptRoot\radv-bo-lifetime-test.py" --source $Source --build $Build --output $OutputDir
    if ($LASTEXITCODE) { throw 'BO lifetime test failed; see output directory.' }
} finally {
    Restore-ProcessEnvironment $saved
}

# The Windows disk cache tests of a built Mesa util library (BD-216): src/util/tests/cache_win32/build-tests.py of the
# source tree, in the Visual Studio environment of the build's architecture. The build must be of the lab flavour: the
# tests put their caches in directories that MESA_SHADER_CACHE_DIR names, and only the lab flavour reads it.
param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Build,
    [Parameter(Mandatory)][string]$OutputDir,
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
    [string]$Filter = 'CacheWin32*',
    [string]$VsInstall
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) { throw 'Use a new output directory; previous results are immutable.' }
$builder = Join-Path $Source 'src\util\tests\cache_win32\build-tests.py'
if (-not (Test-Path -LiteralPath $builder)) { throw "This Mesa tree has no $builder." }
$saved = Save-ProcessEnvironment
try {
    # Keep compiler scratch separate from the new result directory.
    $tmp = "$OutputDir-tmp"
    $env:TEMP = $tmp
    $env:TMP = $tmp
    $null = Import-VsDevEnvironment $VsInstall $tmp $Arch
    python -I $builder --build $Build --out $OutputDir --run --filter $Filter
    if ($LASTEXITCODE) { throw 'Windows disk cache tests failed; see output directory.' }
} finally {
    Restore-ProcessEnvironment $saved
}

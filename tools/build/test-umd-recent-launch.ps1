param([string]$OutputDir, [string]$VsInstall)
# Host gate of the recent-launch record (driver/umd/recent-launch, gate G-RG of the GUI plan). Both shell builds
# run it before they produce a DLL. Everything the test writes stays below -OutputDir and under one test key in
# HKCU that the test removes; the user's own switch is only read.
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-recent-launch' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir
    Push-Location $OutputDir
    try {
        & cl.exe /nologo /std:c++20 /EHsc /W4 /WX /O2 /MT /DNOMINMAX /Fe:recent-launch-test.exe "$repo\driver\umd\recent-launch\recent-launch-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Recent-launch test build failed' }
        & .\recent-launch-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Recent-launch tests failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

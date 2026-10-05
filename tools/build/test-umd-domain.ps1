# Build and run the UMD runtime-domain unit control; no lab access.
param([string]$OutputDir, [string]$VsInstall)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $OutputDir) { $OutputDir = Join-Path (Get-Bc250Root $repo) 'scratch\build\umd-runtime-domain' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved = Save-ProcessEnvironment
try {
    $env:TEMP = $OutputDir
    $env:TMP = $OutputDir
    $null = Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir
    Push-Location $OutputDir
    try {
        $source = Join-Path $repo 'driver\umd\dxvk\runtime-domain-test.cpp'
        & cl.exe /nologo /std:c++17 /EHsc /W4 /WX /MD /Fe:runtime-domain-test.exe /Fo:runtime-domain-test.obj $source
        if ($LASTEXITCODE -ne 0) { throw 'Runtime-domain compilation failed' }
        & .\runtime-domain-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Runtime-domain control failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

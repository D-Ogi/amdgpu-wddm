# Build and run the UMD runtime-domain unit control; no lab access.
# -Compiler clang-cl (the default) builds with clang-cl of LLVM 23.1.4 (umd-compiler.ps1), with the flags of the
# shells. -Compiler msvc builds with cl.exe and needs a concrete -MsvcReason (compiler policy). The script writes
# compiler.json into -OutputDir and prints the compiler path and version.
param([string]$OutputDir, [string]$VsInstall,
      [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl', [string]$MsvcReason = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root = Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir = Join-Path $root 'scratch\build\umd-runtime-domain' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved = Save-ProcessEnvironment
try {
    $env:TEMP = $OutputDir
    $env:TMP = $OutputDir
    $null = Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir
    $cc = Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir 'x64'
    $flags = Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++17', '/EHsc', '/W4', '/WX', '/MD') @() 'x64' @($repo)
    Push-Location $OutputDir
    try {
        $source = Join-Path $repo 'driver\umd\dxvk\runtime-domain-test.cpp'
        Invoke-Bc250UmdCompiler $cc @flags /Fe:runtime-domain-test.exe /Fo:runtime-domain-test.obj $source
        if ($LASTEXITCODE -ne 0) { throw 'Runtime-domain compilation failed' }
        & .\runtime-domain-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Runtime-domain control failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

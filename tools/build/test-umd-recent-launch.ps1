param([string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl', [string]$MsvcReason = '')
# Host gate of the recent-launch record (driver/umd/recent-launch, gate G-RG of the GUI plan). Both shell builds
# run it before they produce a DLL. Everything the test writes stays below -OutputDir and under one test key in
# HKCU that the test removes; the user's own switch is only read.
# -Compiler clang-cl (the default) builds with clang-cl of LLVM 23.1.4 (umd-compiler.ps1), with the flags of the
# shells. -Compiler msvc builds with cl.exe and needs a concrete -MsvcReason (compiler policy). The script writes
# compiler.json into -OutputDir and prints the compiler path and version.
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-recent-launch' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $cc=Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir $Arch
    $flags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/DNOMINMAX') @() $Arch @($repo)
    Push-Location $OutputDir
    try {
        Invoke-Bc250UmdCompiler $cc @flags /Fe:recent-launch-test.exe "$repo\driver\umd\recent-launch\recent-launch-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Recent-launch test build failed' }
        & .\recent-launch-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Recent-launch tests failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

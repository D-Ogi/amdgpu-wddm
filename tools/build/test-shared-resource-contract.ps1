param([string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl', [string]$MsvcReason = '')
# BD-373 step 3: the contract test of the ENGINE LAYOUT form against the kernel driver's own readers
# (driver/contract/test/shared-resource-test.cpp, linked with driver/kmd/umd_blob.c). No API header.
# -Compiler clang-cl (the default) builds with clang-cl of LLVM 23.1.4 (umd-compiler.ps1). -Compiler msvc needs a
# concrete -MsvcReason (compiler policy).
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir=Join-Path $root "scratch\build\shared-resource-contract-$Arch" }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $cc=Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir $Arch
    $headers=@("$repo\driver\contract\third_party", "$repo\driver\contract\uapi-shim")
    $flags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MD') $headers $Arch @($repo)
    Push-Location $OutputDir
    try {
        Invoke-Bc250UmdCompiler $cc @flags /Fe:shared-resource-test.exe "$repo\driver\contract\test\shared-resource-test.cpp" "/Tp$repo\driver\kmd\umd_blob.c"
        if ($LASTEXITCODE -ne 0) { throw 'Shared resource contract compilation failed' }
        & .\shared-resource-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Shared resource contract test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

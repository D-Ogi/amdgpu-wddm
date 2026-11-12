param([string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl', [string]$MsvcReason = '', [string]$DxvkSource = '')
# Host gate of the D3D11 shell's UMD logging module (driver/umd/dxvk/umd-etw.h, BD-373). The test uses a recording
# ETW sink and registers no provider with the system. The shell build runs it before it produces a DLL.
# With -DxvkSource (the DXVK fork tree that holds the engine ABI header) it also runs the test of the shell's half of
# the engine's logging (engine-memory-log.h, engine ABI 1.8).
# -Compiler clang-cl (the default) builds with clang-cl of LLVM 23.1.4 (umd-compiler.ps1), with the flags of the
# shells. -Compiler msvc builds with cl.exe and needs a concrete -MsvcReason (compiler policy).
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-etw' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $cc=Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir $Arch
    $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $flags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN') @("$wdk", "$wdk\..\shared") $Arch @($repo)
    Push-Location $OutputDir
    try {
        Invoke-Bc250UmdCompiler $cc @flags /Fe:umd-etw-test.exe "$repo\driver\umd\dxvk\umd-etw.cpp" "$repo\driver\umd\dxvk\umd-etw-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'UMD logging test build failed' }
        & .\umd-etw-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'UMD logging tests failed' }
        if ($DxvkSource) {
            $src=[IO.Path]::GetFullPath($DxvkSource)
            $engineFlags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN') @("$wdk", "$wdk\..\shared", "$src\src", "$src\include\vulkan\include") $Arch @($repo, $src)
            Invoke-Bc250UmdCompiler $cc @engineFlags /Fe:engine-memory-log-test.exe "$repo\driver\umd\dxvk\umd-etw.cpp" "$repo\driver\umd\dxvk\engine-memory-log.cpp" "$repo\driver\umd\dxvk\engine-memory-log-test.cpp"
            if ($LASTEXITCODE -ne 0) { throw 'engine memory log test build failed' }
            & .\engine-memory-log-test.exe
            if ($LASTEXITCODE -ne 0) { throw 'engine memory log tests failed' }
        }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

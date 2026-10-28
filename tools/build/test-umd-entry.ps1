param([Parameter(Mandatory)][string]$DxvkSource, [Parameter(Mandatory)][string]$UmdPath, [string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl', [string]$MsvcReason = '')
# -Compiler clang-cl (the default) builds with clang-cl of LLVM 23.1.4 (umd-compiler.ps1), with the flags of the
# shells. -Compiler msvc builds with cl.exe and needs a concrete -MsvcReason (compiler policy). The script writes
# compiler.json into -OutputDir and prints the compiler path and version.
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
$DxvkSource=[IO.Path]::GetFullPath($DxvkSource)
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-entry-test' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $cc=Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir $Arch
    $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $headers=@("$wdk", "$wdk\..\shared", "$DxvkSource\src", "$DxvkSource\include\vulkan\include", "$DxvkSource\include\spirv\include", "$DxvkSource\subprojects\dxbc-spirv")
    $flags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MD', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DVK_USE_PLATFORM_WIN32_KHR') $headers $Arch @($repo)
    Push-Location $OutputDir
    try {
        Invoke-Bc250UmdCompiler $cc @flags /Fe:umd-entry-test.exe "$repo\driver\umd\dxvk\umd-entry-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'UMD entry compilation failed' }
        $isolated=Join-Path $OutputDir 'loader'
        New-Item -ItemType Directory -Force $isolated | Out-Null
        $testDll=Join-Path $isolated 'amdgpu_wddm_d3d11.dll'
        Copy-Item -LiteralPath $UmdPath -Destination $testDll -Force
        & .\umd-entry-test.exe $testDll
        if ($LASTEXITCODE -ne 0) { throw 'UMD entry test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }





param([Parameter(Mandatory)][string]$DxvkSource, [string]$OutputDir, [string]$VsInstall,
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
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-surface-allocation' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir
    $cc=Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir 'x64'
    $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $headers=@("$wdk", "$wdk\..\shared", "$DxvkSource\src", "$DxvkSource\include\vulkan\include", "$DxvkSource\include\spirv\include", "$DxvkSource\subprojects\dxbc-spirv")
    $flags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MD', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DVK_USE_PLATFORM_WIN32_KHR') $headers 'x64' @($repo)
    Push-Location $OutputDir
    try {
        Invoke-Bc250UmdCompiler $cc @flags /Fe:surface-allocation-test.exe "/Tp$repo\driver\kmd\dcn_translate.c" "$repo\driver\umd\dxvk\runtime-surface-allocation.cpp" "$repo\driver\umd\dxvk\runtime-surface-allocation-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Surface-allocation compilation failed' }
        & .\surface-allocation-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Surface-allocation test failed' }
        # BD-065: the Present shadow request passes the same allocation admission.
        Invoke-Bc250UmdCompiler $cc @flags /Fe:present-shadow-test.exe "/Tp$repo\driver\kmd\dcn_translate.c" "$repo\driver\umd\dxvk\runtime-surface-allocation.cpp" "$repo\driver\umd\dxvk\present-shadow-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Present-shadow compilation failed' }
        & .\present-shadow-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Present-shadow test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

param([Parameter(Mandatory)][string]$DxvkSource, [string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl', [string]$MsvcReason = '')
# BD-373 step 3: the host test of the ENGINE LAYOUT form of shared resources (runtime-engine-resource-test.cpp).
# -Compiler clang-cl (the default) builds with clang-cl of LLVM 23.1.4 (umd-compiler.ps1), with the flags of the
# shells. -Compiler msvc builds with cl.exe and needs a concrete -MsvcReason (compiler policy). The script writes
# compiler.json into -OutputDir and prints the compiler path and version.
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
$DxvkSource=[IO.Path]::GetFullPath($DxvkSource)
if (-not $OutputDir) { $OutputDir=Join-Path $root "scratch\build\umd-engine-resource-$Arch" }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $cc=Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir $Arch
    $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $headers=@("$wdk", "$wdk\..\shared", "$DxvkSource\src", "$DxvkSource\include\vulkan\include", "$DxvkSource\include\spirv\include", "$DxvkSource\subprojects\dxbc-spirv", "$repo\driver\contract\third_party", "$repo\driver\contract\uapi-shim")
    $flags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MD', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DVK_USE_PLATFORM_WIN32_KHR') $headers $Arch @($repo)
    Push-Location $OutputDir
    try {
        Invoke-Bc250UmdCompiler $cc @flags /Fe:engine-resource-test.exe "$repo\driver\umd\dxvk\runtime-engine-resource.cpp" "$repo\driver\umd\dxvk\runtime-image-memory.cpp" "$repo\driver\umd\dxvk\runtime-surface-allocation.cpp" "/Tp$repo\driver\kmd\dcn_translate.c" "$repo\driver\umd\dxvk\runtime-bridge.cpp" "$repo\driver\umd\dxvk\runtime-engine-resource-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Engine-resource compilation failed' }
        & .\engine-resource-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Engine-resource test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

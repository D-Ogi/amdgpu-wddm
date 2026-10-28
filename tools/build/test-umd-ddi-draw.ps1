param([Parameter(Mandatory)][string]$DxvkSource, [string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
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
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-ddi-draw' }
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
        Invoke-Bc250UmdCompiler $cc @flags /Fe:ddi-draw-test.exe "$repo\driver\umd\dxvk\engine-session.cpp" "$repo\driver\umd\dxvk\engine-modules.cpp" "$repo\driver\umd\dxvk\ddi-adapter.cpp" "$repo\driver\umd\dxvk\hosted-instance.cpp" "$repo\driver\umd\dxvk\runtime-bridge.cpp" "$repo\driver\umd\dxvk\device-owner.cpp" "$repo\driver\umd\dxvk\runtime-surface.cpp" "$repo\driver\umd\dxvk\runtime-surface-allocation.cpp" "$repo\driver\umd\dxvk\runtime-texture.cpp" "$repo\driver\umd\dxvk\runtime-image-memory.cpp" "/Tp$repo\driver\kmd\dcn_translate.c" "$repo\driver\umd\dxvk\ddi-draw.cpp" "$repo\driver\umd\dxvk\ddi-raster.cpp" "$repo\driver\umd\dxvk\ddi-shader.cpp" "$repo\driver\umd\dxvk\ddi-sampler.cpp" "$repo\driver\umd\dxvk\ddi-fixed-state.cpp" "$repo\driver\umd\dxvk\ddi-blend.cpp" "$repo\driver\umd\dxvk\ddi-resource.cpp" "$repo\driver\umd\dxvk\ddi-buffer-binding.cpp" "$repo\driver\umd\dxvk\ddi-transfer.cpp" "$repo\driver\umd\dxvk\ddi-map.cpp" "$repo\driver\umd\dxvk\ddi-rtv.cpp" "$repo\driver\umd\dxvk\ddi-dsv.cpp" "$repo\driver\umd\dxvk\ddi-uav.cpp" "$repo\driver\umd\dxvk\ddi-output.cpp" "$repo\driver\umd\dxvk\ddi-srv.cpp" "$repo\driver\umd\dxvk\ddi-flush.cpp" "$repo\driver\umd\dxvk\ddi-table.cpp" "$repo\driver\umd\dxvk\ddi-wddm2.cpp" "$repo\driver\umd\dxvk\ddi-wddm22.cpp" "$repo\driver\umd\dxvk\ddi-present.cpp" "$repo\driver\umd\dxvk\ddi-dxgi-resources.cpp" "$repo\driver\umd\dxvk\ddi-blt.cpp" "$repo\driver\umd\dxvk\ddi-clear-view.cpp" "$repo\driver\umd\dxvk\ddi-device-create.cpp" "$repo\driver\umd\dxvk\ddi-query.cpp" "$repo\driver\umd\dxvk\ddi-lifecycle.cpp" "$repo\driver\umd\dxvk\ddi-format.cpp" "$repo\driver\umd\dxvk\ddi-resource-status.cpp" "$repo\driver\umd\dxvk\ddi-input-layout.cpp" "$repo\driver\umd\dxvk\input-layout.cpp" "$repo\driver\umd\dxvk\engine-input-layout.cpp" "$repo\driver\umd\dxvk\ddi-draw-test.cpp" "$repo\driver\umd\dxvk\ddi-wddm2-test.cpp" "$repo\driver\umd\dxvk\ddi-wddm22-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Engine-session compilation failed' }
        & .\ddi-draw-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Engine-session test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }





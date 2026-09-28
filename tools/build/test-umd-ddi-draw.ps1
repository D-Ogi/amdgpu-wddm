param([Parameter(Mandatory)][string]$DxvkSource, [string]$OutputDir, [string]$VsInstall)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
$DxvkSource=[IO.Path]::GetFullPath($DxvkSource)
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-ddi-draw' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir
    $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $includes=@("/external:I$wdk", "/external:I$wdk\..\shared", "/external:I$DxvkSource\src", "/external:I$DxvkSource\include\vulkan\include", "/external:I$DxvkSource\include\spirv\include", "/external:I$DxvkSource\subprojects\dxbc-spirv")
    Push-Location $OutputDir
    try {
        & cl.exe /nologo /std:c++20 /EHsc /W4 /WX /external:W0 /MD /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DVK_USE_PLATFORM_WIN32_KHR @includes /Fe:ddi-draw-test.exe "$repo\driver\umd\dxvk\engine-session.cpp" "$repo\driver\umd\dxvk\hosted-instance.cpp" "$repo\driver\umd\dxvk\runtime-bridge.cpp" "$repo\driver\umd\dxvk\device-owner.cpp" "$repo\driver\umd\dxvk\ddi-draw.cpp" "$repo\driver\umd\dxvk\ddi-raster.cpp" "$repo\driver\umd\dxvk\ddi-shader.cpp" "$repo\driver\umd\dxvk\ddi-sampler.cpp" "$repo\driver\umd\dxvk\ddi-fixed-state.cpp" "$repo\driver\umd\dxvk\ddi-blend.cpp" "$repo\driver\umd\dxvk\ddi-resource.cpp" "$repo\driver\umd\dxvk\ddi-buffer-binding.cpp" "$repo\driver\umd\dxvk\ddi-transfer.cpp" "$repo\driver\umd\dxvk\ddi-map.cpp" "$repo\driver\umd\dxvk\ddi-rtv.cpp" "$repo\driver\umd\dxvk\ddi-dsv.cpp" "$repo\driver\umd\dxvk\ddi-uav.cpp" "$repo\driver\umd\dxvk\ddi-output.cpp" "$repo\driver\umd\dxvk\ddi-srv.cpp" "$repo\driver\umd\dxvk\ddi-flush.cpp" "$repo\driver\umd\dxvk\ddi-table.cpp" "$repo\driver\umd\dxvk\ddi-format.cpp" "$repo\driver\umd\dxvk\ddi-resource-status.cpp" "$repo\driver\umd\dxvk\ddi-input-layout.cpp" "$repo\driver\umd\dxvk\input-layout.cpp" "$repo\driver\umd\dxvk\engine-input-layout.cpp" "$repo\driver\umd\dxvk\ddi-draw-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Engine-session compilation failed' }
        & .\ddi-draw-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Engine-session test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }





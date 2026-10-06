param([Parameter(Mandatory)][string]$DxvkSource, [string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64', [string]$EnginePath, [string]$IcdPath)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
$DxvkSource=[IO.Path]::GetFullPath($DxvkSource)
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-dxvk' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
if ([bool]$EnginePath -ne [bool]$IcdPath) { throw 'Supply both EnginePath and IcdPath for pair admission' }
if ($EnginePath) {
    & "$PSScriptRoot\test-umd-engine-pair.ps1" -DxvkSource $DxvkSource -OutputDir (Join-Path $OutputDir 'quality\engine-pair') -VsInstall $VsInstall -Arch $Arch -EnginePath $EnginePath -IcdPath $IcdPath
}

# Deferred-error state and session cleanup must pass for this ABI1.4 shell.
& "$PSScriptRoot\test-umd-engine-session.ps1" -DxvkSource $DxvkSource -OutputDir (Join-Path $OutputDir 'quality\engine-session') -VsInstall $VsInstall -Arch $Arch
# Device-table ABI and capability behavior are promotion gates, not optional
# manual checks. Run before producing a deployable shell DLL.
& "$PSScriptRoot\test-umd-ddi-draw.ps1" -DxvkSource $DxvkSource -OutputDir (Join-Path $OutputDir 'quality\ddi-table') -VsInstall $VsInstall -Arch $Arch
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $includes=@("/external:I$wdk", "/external:I$wdk\..\shared", "/external:I$DxvkSource\src", "/external:I$DxvkSource\include\vulkan\include", "/external:I$DxvkSource\include\spirv\include", "/external:I$DxvkSource\subprojects\dxbc-spirv")
    Push-Location $OutputDir
    try {
        & cl.exe /nologo /std:c++20 /EHsc /W4 /WX /external:W0 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DVK_USE_PLATFORM_WIN32_KHR @includes /LD /Fe:amdgpu_wddm_d3d11.dll "$repo\driver\umd\dxvk\engine-session.cpp" "$repo\driver\umd\dxvk\engine-modules.cpp" "$repo\driver\umd\dxvk\ddi-adapter.cpp" "$repo\driver\umd\dxvk\hosted-instance.cpp" "$repo\driver\umd\dxvk\runtime-bridge.cpp" "$repo\driver\umd\dxvk\device-owner.cpp" "$repo\driver\umd\dxvk\runtime-surface.cpp" "$repo\driver\umd\dxvk\runtime-surface-allocation.cpp" "$repo\driver\umd\dxvk\runtime-texture.cpp" "$repo\driver\umd\dxvk\runtime-image-memory.cpp" "/Tp$repo\driver\kmd\dcn_translate.c" "$repo\driver\umd\dxvk\ddi-draw.cpp" "$repo\driver\umd\dxvk\ddi-raster.cpp" "$repo\driver\umd\dxvk\ddi-shader.cpp" "$repo\driver\umd\dxvk\ddi-sampler.cpp" "$repo\driver\umd\dxvk\ddi-fixed-state.cpp" "$repo\driver\umd\dxvk\ddi-blend.cpp" "$repo\driver\umd\dxvk\ddi-resource.cpp" "$repo\driver\umd\dxvk\ddi-buffer-binding.cpp" "$repo\driver\umd\dxvk\ddi-transfer.cpp" "$repo\driver\umd\dxvk\ddi-map.cpp" "$repo\driver\umd\dxvk\ddi-rtv.cpp" "$repo\driver\umd\dxvk\ddi-dsv.cpp" "$repo\driver\umd\dxvk\ddi-uav.cpp" "$repo\driver\umd\dxvk\ddi-output.cpp" "$repo\driver\umd\dxvk\ddi-srv.cpp" "$repo\driver\umd\dxvk\ddi-flush.cpp" "$repo\driver\umd\dxvk\ddi-table.cpp" "$repo\driver\umd\dxvk\ddi-wddm2.cpp" "$repo\driver\umd\dxvk\ddi-present.cpp" "$repo\driver\umd\dxvk\ddi-dxgi-resources.cpp" "$repo\driver\umd\dxvk\ddi-blt.cpp" "$repo\driver\umd\dxvk\ddi-clear-view.cpp" "$repo\driver\umd\dxvk\ddi-device-create.cpp" "$repo\driver\umd\dxvk\ddi-query.cpp" "$repo\driver\umd\dxvk\ddi-lifecycle.cpp" "$repo\driver\umd\dxvk\ddi-format.cpp" "$repo\driver\umd\dxvk\ddi-resource-status.cpp" "$repo\driver\umd\dxvk\ddi-input-layout.cpp" "$repo\driver\umd\dxvk\input-layout.cpp" "$repo\driver\umd\dxvk\engine-input-layout.cpp" "$repo\driver\umd\dxvk\umd-entry.cpp" /link "/DEF:$repo\driver\umd\dxvk\amdgpu_wddm_d3d11.def"
        if ($LASTEXITCODE -ne 0) { throw 'UMD compilation failed' }
        & dumpbin.exe /nologo /exports .\amdgpu_wddm_d3d11.dll
        if ($LASTEXITCODE -ne 0) { throw 'UMD export inspection failed' }
        # The UMD runs inside every game. A game that ships an older msvcp140.dll beside its exe hands that module to
        # each DLL importing it by name, and a std::mutex built by newer STL headers then faults in its _Mtx_lock
        # (Rise of the Tomb Raider's CreateDevice). The shell links the CRT statically, as the engines and the D3D12
        # shell do; this gate keeps it that way.
        $deps = & dumpbin.exe /nologo /dependents .\amdgpu_wddm_d3d11.dll
        if ($LASTEXITCODE -ne 0) { throw 'UMD dependency inspection failed' }
        $crt = @($deps | Where-Object { $_ -match '^\s+(msvcp|vcruntime|concrt|ucrtbase|api-ms-win-crt-)\S*\.dll\s*$' })
        if ($crt.Count) { throw ('UMD imports a dynamic C/C++ runtime: ' + (($crt | ForEach-Object { $_.Trim() }) -join ', ')) }
        Write-Host 'Build only: no configuration, engine or ICD is installed or selected.' 
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }





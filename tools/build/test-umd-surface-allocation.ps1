param([Parameter(Mandatory)][string]$DxvkSource, [string]$OutputDir, [string]$VsInstall)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
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
    $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $includes=@("/external:I$wdk", "/external:I$wdk\..\shared", "/external:I$DxvkSource\src", "/external:I$DxvkSource\include\vulkan\include", "/external:I$DxvkSource\include\spirv\include", "/external:I$DxvkSource\subprojects\dxbc-spirv")
    Push-Location $OutputDir
    try {
        & cl.exe /nologo /std:c++20 /EHsc /W4 /WX /external:W0 /MD /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DVK_USE_PLATFORM_WIN32_KHR @includes /Fe:surface-allocation-test.exe "/Tp$repo\driver\kmd\dcn_translate.c" "$repo\driver\umd\dxvk\runtime-surface-allocation.cpp" "$repo\driver\umd\dxvk\runtime-surface-allocation-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Surface-allocation compilation failed' }
        & .\surface-allocation-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Surface-allocation test failed' }
        # BD-065: the Present shadow request passes the same allocation admission.
        & cl.exe /nologo /std:c++20 /EHsc /W4 /WX /external:W0 /MD /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DVK_USE_PLATFORM_WIN32_KHR @includes /Fe:present-shadow-test.exe "/Tp$repo\driver\kmd\dcn_translate.c" "$repo\driver\umd\dxvk\runtime-surface-allocation.cpp" "$repo\driver\umd\dxvk\present-shadow-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Present-shadow compilation failed' }
        & .\present-shadow-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Present-shadow test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

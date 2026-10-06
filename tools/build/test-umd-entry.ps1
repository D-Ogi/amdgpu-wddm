param([Parameter(Mandatory)][string]$DxvkSource, [Parameter(Mandatory)][string]$UmdPath, [string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
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
    $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $includes=@("/external:I$wdk", "/external:I$wdk\..\shared", "/external:I$DxvkSource\src", "/external:I$DxvkSource\include\vulkan\include", "/external:I$DxvkSource\include\spirv\include", "/external:I$DxvkSource\subprojects\dxbc-spirv")
    Push-Location $OutputDir
    try {
        & cl.exe /nologo /std:c++20 /EHsc /W4 /WX /external:W0 /MD /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DVK_USE_PLATFORM_WIN32_KHR @includes /Fe:umd-entry-test.exe "$repo\driver\umd\dxvk\umd-entry-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'UMD entry compilation failed' }
        $isolated=Join-Path $OutputDir 'loader'
        New-Item -ItemType Directory -Force $isolated | Out-Null
        $testDll=Join-Path $isolated 'amdgpu_wddm_d3d11.dll'
        Copy-Item -LiteralPath $UmdPath -Destination $testDll -Force
        & .\umd-entry-test.exe $testDll
        if ($LASTEXITCODE -ne 0) { throw 'UMD entry test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }





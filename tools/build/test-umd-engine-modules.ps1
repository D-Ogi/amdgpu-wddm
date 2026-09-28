param([Parameter(Mandatory)][string]$DxvkSource, [string]$OutputDir, [string]$VsInstall)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
$DxvkSource=[IO.Path]::GetFullPath($DxvkSource)
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-engine-modules' }
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
        $options=@('/nologo','/std:c++20','/EHsc','/W4','/WX','/external:W0','/MD','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/DVK_USE_PLATFORM_WIN32_KHR')
        & cl.exe @options @includes /LD /Fe:engine-modules-fixture.dll "$repo\driver\umd\dxvk\engine-modules-fixture.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Loader fixture compilation failed' }
        & cl.exe @options @includes /Fe:engine-modules-test.exe "$repo\driver\umd\dxvk\engine-modules.cpp" "$repo\driver\umd\dxvk\engine-modules-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Loader compilation failed' }
        & .\engine-modules-test.exe (Join-Path $OutputDir 'engine-modules-fixture.dll')
        if ($LASTEXITCODE -ne 0) { throw 'Loader test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

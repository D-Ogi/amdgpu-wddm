param([string]$OutputDir, [string]$VsInstall)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-runtime-bridge' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir
    $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $includes=@("/external:I$wdk", "/external:I$wdk\..\shared")
    Push-Location $OutputDir
    try {
        & cl.exe /nologo /std:c++20 /EHsc /W4 /WX /external:W0 /MD /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DVK_USE_PLATFORM_WIN32_KHR @includes /Fe:runtime-bridge-test.exe "$repo\driver\umd\dxvk\runtime-bridge.cpp" "$repo\driver\umd\dxvk\runtime-bridge-test.cpp" "$repo\driver\umd\dxvk\present-bridge-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Input-layout compilation failed' }
        & .\runtime-bridge-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Input-layout test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }




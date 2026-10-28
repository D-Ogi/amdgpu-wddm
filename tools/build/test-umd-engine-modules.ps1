param([Parameter(Mandatory)][string]$DxvkSource, [string]$OutputDir, [string]$VsInstall,
      [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
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
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-engine-modules' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $cc=Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir $Arch
    $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $headers=@("$wdk", "$wdk\..\shared", "$DxvkSource\src", "$DxvkSource\include\vulkan\include", "$DxvkSource\include\spirv\include", "$DxvkSource\subprojects\dxbc-spirv")
    Push-Location $OutputDir
    try {
        $flags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MD', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DVK_USE_PLATFORM_WIN32_KHR') $headers $Arch @($repo)
        Invoke-Bc250UmdCompiler $cc @flags /LD /Fe:engine-modules-fixture.dll "$repo\driver\umd\dxvk\engine-modules-fixture.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Loader fixture compilation failed' }
        Invoke-Bc250UmdCompiler $cc @flags /Fe:engine-modules-test.exe "$repo\driver\umd\dxvk\engine-modules.cpp" "$repo\driver\umd\dxvk\engine-modules-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Loader compilation failed' }
        $fixture=Join-Path $OutputDir 'engine-modules-fixture.dll'
        $pins=Join-Path $OutputDir 'image-identities.json'
        & python -B "$repo\tools\release\image_identity.py" --directory $OutputDir --write $pins
        if ($LASTEXITCODE -ne 0) { throw 'Image identity calculation failed' }
        $imageDigest=(Get-Content -LiteralPath $pins -Raw | ConvertFrom-Json).'engine-modules-fixture.dll'
        & .\engine-modules-test.exe $fixture (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash $imageDigest
        if ($LASTEXITCODE -ne 0) { throw 'Loader test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

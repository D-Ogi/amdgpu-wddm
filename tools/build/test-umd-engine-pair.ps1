param([Parameter(Mandatory)][string]$DxvkSource, [string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64', [Parameter(Mandatory)][string]$EnginePath, [Parameter(Mandatory)][string]$IcdPath)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
$DxvkSource=[IO.Path]::GetFullPath($DxvkSource)
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-engine-pair' }
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
        $options=@('/nologo','/std:c++20','/EHsc','/W4','/WX','/external:W0','/MD','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/DVK_USE_PLATFORM_WIN32_KHR')
        & cl.exe @options @includes /Fe:engine-pair-test.exe "$repo\driver\umd\dxvk\engine-modules.cpp" "$repo\driver\umd\dxvk\engine-pair-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Engine pair probe compilation failed' }
        $EnginePath=[IO.Path]::GetFullPath($EnginePath)
        $IcdPath=[IO.Path]::GetFullPath($IcdPath)
        $receipt=[ordered]@{
            header_sha256=(Get-FileHash "$DxvkSource\src\ddi\bc250_dxvk_engine.h").Hash
            engine_sha256=(Get-FileHash -LiteralPath $EnginePath).Hash
            icd_sha256=(Get-FileHash -LiteralPath $IcdPath).Hash
        }
        $receipt.output=@(& .\engine-pair-test.exe $EnginePath $IcdPath)
        $receipt.exit_code=$LASTEXITCODE
        $receipt | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $OutputDir 'pair-result.json')
        $receipt.output | Write-Output
        if ($receipt.exit_code -ne 0) { throw 'Engine/ICD admission failed; do not stage this pair' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

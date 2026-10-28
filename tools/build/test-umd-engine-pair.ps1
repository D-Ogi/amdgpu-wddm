param([Parameter(Mandatory)][string]$DxvkSource, [string]$OutputDir, [string]$VsInstall, [ValidateSet('x64', 'x86')][string]$Arch = 'x64', [Parameter(Mandatory)][string]$EnginePath, [Parameter(Mandatory)][string]$IcdPath,
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
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-engine-pair' }
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
        Invoke-Bc250UmdCompiler $cc @flags /Fe:engine-pair-test.exe "$repo\driver\umd\dxvk\engine-modules.cpp" "$repo\driver\umd\dxvk\engine-pair-test.cpp"
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

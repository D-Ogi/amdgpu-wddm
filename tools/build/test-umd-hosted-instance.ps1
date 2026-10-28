# Host test of the D3D11 shell's HostedInstance (driver/umd/dxvk/hosted-instance.cpp) against a DXVK source tree.
# -Compiler clang-cl (the default) builds with clang-cl of LLVM 23.1.4 (umd-compiler.ps1), with the flags of the
# shells. -Compiler msvc builds with cl.exe and needs a concrete -MsvcReason (compiler policy). The script writes
# compiler.json into -OutputDir and prints the compiler path and version.
param([Parameter(Mandatory)][string]$DxvkSource, [string]$OutputDir, [string]$VsInstall,
      [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl', [string]$MsvcReason = '')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
$DxvkSource=[IO.Path]::GetFullPath($DxvkSource)
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-hosted-instance' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir
    $cc=Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir 'x64'
    $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $headers=@($wdk, "$wdk\..\shared", "$DxvkSource\src", "$DxvkSource\include\vulkan\include", "$DxvkSource\include\spirv\include", "$DxvkSource\subprojects\dxbc-spirv")
    $flags=@('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MD', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DVK_USE_PLATFORM_WIN32_KHR')
    if ($Compiler -eq 'clang-cl') {
        # The WDK and DXVK headers are system headers for clang-cl, as /external:I makes them for cl.
        $flags=@(ConvertTo-Bc250ClangFlags $flags 'x64' @($repo)) + @($headers | ForEach-Object { "/imsvc$_" })
    } else {
        $flags+=@('/external:W0') + @($headers | ForEach-Object { "/external:I$_" })
    }
    Push-Location $OutputDir
    try {
        Invoke-Bc250UmdCompiler $cc @flags /Fe:hosted-instance-test.exe "$repo\driver\umd\dxvk\hosted-instance.cpp" "$repo\driver\umd\dxvk\hosted-instance-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Hosted-instance compilation failed' }
        & .\hosted-instance-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Hosted-instance test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }

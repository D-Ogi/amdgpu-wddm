param(
    [Parameter(Mandatory=$true)][string]$Workspace,
    [Parameter(Mandatory=$true)][string]$Out,
    [string]$VsInstall = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $repo 'tools\build\common.ps1')
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
$workspacePath = (Resolve-Path -LiteralPath $Workspace).Path
$outputPath = [IO.Path]::GetFullPath($Out)
if (-not $outputPath.StartsWith($workspacePath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output must be inside the workspace.'
}
if (Test-Path -LiteralPath $outputPath) { throw 'Use a new output directory to preserve previous artifacts.' }
New-Item -ItemType Directory -Path $outputPath | Out-Null
$saved = Save-ProcessEnvironment
Push-Location $outputPath
try {
    $env:TEMP = $outputPath
    $env:TMP = $outputPath
    Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $outputPath -Arch x64
    $clang = Resolve-Bc250UmdCompiler -Root $workspacePath -Compiler clang -MsvcReason '' -Out $outputPath -Arch x64
    $source = Join-Path $PSScriptRoot 'cube-mip.cpp'
    $sourceHash = (Get-FileHash -LiteralPath $source).Hash
    $exe = Join-Path $outputPath 'cube-mip.exe'
    $flags = @('/nologo', '/O2', '/EHsc', '/MT', '/W4', '/Brepro',
        '-march=x86-64-v3', '-mtune=znver2', "/clang:-ffile-prefix-map=$repo=.",
        "/Fo$(Join-Path $outputPath 'cube-mip.obj')", "/Fe$exe")
    Invoke-Bc250UmdCompiler $clang @flags $source /link /Brepro d3d12.lib dxgi.lib d3dcompiler.lib
    if ($LASTEXITCODE -ne 0) { throw "clang-cl failed: $LASTEXITCODE" }
    if ((Get-FileHash -LiteralPath $source).Hash -ne $sourceHash) { throw 'Source changed during compilation.' }
    [ordered]@{
        source_sha256 = $sourceHash
        build_script_sha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash
        compiler_sha256 = (Get-FileHash -LiteralPath $clang).Hash
        compiler = (Get-Content -LiteralPath (Join-Path $outputPath 'compiler.json') -Raw | ConvertFrom-Json)
        windows_sdk_version = $env:WindowsSDKVersion
        msvc_tools_version = $env:VCToolsVersion
        flags = $flags
        libraries = @('d3d12.lib', 'dxgi.lib', 'd3dcompiler.lib')
        executable_sha256 = (Get-FileHash -LiteralPath $exe).Hash
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputPath 'build.json') -Encoding UTF8
    Get-FileHash -LiteralPath $exe
} finally {
    Pop-Location
    Restore-ProcessEnvironment $saved
}

# Builds clprobe.exe (BD-083): headers and import libraries from the SDK NuGet packages under -Kits, the Khronos
# OpenCL headers from -ClHeaders (default <workspace>\ref\OpenCL-Headers, read as system headers), clang-cl and
# lld-link of LLVM 23.1.4 (tools\build\umd-compiler.ps1). The program loads OpenCL.dll at run time, so it links no
# OpenCL library.
#
#   pwsh tools\win\clprobe\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget [-Out <dir>] [-ClHeaders <dir>]
#
# Ends with the SHA-256 of clprobe.exe and writes SHA256SUMS.txt next to it.
param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = '',
    [string]$ClHeaders = ''
)
$ErrorActionPreference = 'Stop'
$Kits = [IO.Path]::GetFullPath($Kits)
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$workspace = Split-Path (Split-Path $Kits -Parent) -Parent
if (-not $Out) { $Out = Join-Path $workspace 'scratch\build\clprobe' }
if (-not $ClHeaders) { $ClHeaders = Join-Path $workspace 'ref\OpenCL-Headers' }
$ClHeaders = [IO.Path]::GetFullPath($ClHeaders)
if (-not (Test-Path -LiteralPath (Join-Path $ClHeaders 'CL\cl.h'))) { throw "no CL\cl.h under $ClHeaders" }
$Out = [IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force $Out, "$Out\tmp" | Out-Null
$env:TEMP = "$Out\tmp"; $env:TMP = $env:TEMP
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$libs = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$env:INCLUDE = "$($msvc.FullName)\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
$env:LIB = "$($msvc.FullName)\lib\x64;$libs\ucrt\x64;$libs\um\x64"
$env:PATH = "$($msvc.FullName)\bin\Hostx64\x64;$env:PATH"
$cc = Resolve-Bc250UmdCompiler $workspace 'clang-cl' '' $Out 'x64'
$repro = Get-Bc250ReproducibleFlags 'clang-cl'
$flags = @(Get-Bc250HostTestFlags 'clang-cl' @('/nologo', '/TC', '/W4', '/WX', '/O2', '/MT', '/Brepro') @($ClHeaders) 'x64' @($repo)) +
    $repro.Compile + '/clang:-fuse-ld=lld'
Push-Location $Out
try {
    Invoke-Bc250UmdCompiler $cc @flags '/Fo.\' '/Feclprobe.exe' (Join-Path $PSScriptRoot 'clprobe.c') `
        /link /Brepro '/PDBALTPATH:%_PDB%' /DYNAMICBASE /NXCOMPAT @($repro.Link) kernel32.lib
    if ($LASTEXITCODE) { throw 'clprobe compile failed' }
} finally { Pop-Location }
$sha = (Get-FileHash -LiteralPath "$Out\clprobe.exe" -Algorithm SHA256).Hash
"$($sha.ToLower()) *clprobe.exe" | Set-Content -LiteralPath "$Out\SHA256SUMS.txt" -Encoding ascii
'{0,9}  clprobe.exe  sha256 {1}' -f (Get-Item "$Out\clprobe.exe").Length, $sha

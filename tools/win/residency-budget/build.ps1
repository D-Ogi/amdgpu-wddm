param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\residency-budget",
    [string]$KitVersion = '10.0.26100.0',
    # LLVM directory whose bin\ holds clang-cl.exe and lld-link.exe.
    [string]$Llvm = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\toolchain\llvm-23.1.4-win64"
)

# Builds residency-budget.exe (README.md) with clang-cl for x86-64-v3. Nothing here touches unit A.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$clang = Join-Path $Llvm 'bin\clang-cl.exe'
# The compiler is named by path and must exist; nothing falls back to whatever cl or clang is on PATH.
if (-not (Test-Path -LiteralPath $clang)) { throw "compiler not found: $clang" }
"clang-cl  $clang"
"          $(& $clang --version | Select-Object -First 1)"
New-Item -ItemType Directory -Force $Out | Out-Null
$root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $here '..\..\..\..')).Path }
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# The compiler reads the headers and libraries from the environment, so it does not depend on a developer prompt.
$env:INCLUDE = @((Join-Path $msvc.FullName 'include'), "$sdk\Include\$KitVersion\ucrt", "$sdk\Include\$KitVersion\um",
    "$sdk\Include\$KitVersion\shared") -join ';'
$env:LIB = @((Join-Path $msvc.FullName 'lib\x64'), "$sdkLib\ucrt\x64", "$sdkLib\um\x64") -join ';'

$exe = Join-Path $Out 'residency-budget.exe'
& $clang @('/nologo', '/O2', '/EHsc', '/std:c++17', '/W4', '/WX', '/MT', '-march=x86-64-v3', '-mtune=znver2', '/Brepro',
    '-fuse-ld=lld', "/Fo$(Join-Path $Out 'residency-budget.obj')", "/Fe$exe", (Join-Path $here 'residency-budget.cpp'),
    '/link', '/Brepro', '/INCREMENTAL:NO') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "residency-budget.exe: compiler failed ($LASTEXITCODE)" }
Get-Item $exe | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

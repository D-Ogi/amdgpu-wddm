# Builds bc250tdr.exe (README.md) with clang-cl from the workspace LLVM, against the SDK NuGet packages under
# -Kits. No fallback: the compiler is named by path and must exist.
#
#   pwsh -NoProfile -File tools\win\tdrclient\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\tdrclient
param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\tdrclient",
    [string]$KitVersion = '10.0.26100.0',
    # LLVM directory whose bin\ holds clang-cl.exe and lld-link.exe.
    [string]$Llvm = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\toolchain\llvm-23.1.4-win64"
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$clang = Join-Path $Llvm 'bin\clang-cl.exe'
if (-not (Test-Path -LiteralPath $clang)) { throw "compiler not found: $clang" }
"clang-cl  $clang"
"          $(& $clang --version | Select-Object -First 1)"
New-Item -ItemType Directory -Force $Out | Out-Null
$root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $here '..\..\..\..')).Path }
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
$env:INCLUDE = @((Join-Path $msvc.FullName 'include'), "$sdk\Include\$KitVersion\ucrt", "$sdk\Include\$KitVersion\um",
    "$sdk\Include\$KitVersion\shared") -join ';'
$env:LIB = @((Join-Path $msvc.FullName 'lib\x64'), "$sdkLib\ucrt\x64", "$sdkLib\um\x64") -join ';'

# The previous binary is kept by its hash, so a lab kit pinned to it can still find it.
$exe = Join-Path $Out 'bc250tdr.exe'
if (Test-Path -LiteralPath $exe) {
    $kept = Join-Path $Out 'retained'
    New-Item -ItemType Directory -Force $kept | Out-Null
    Copy-Item -LiteralPath $exe (Join-Path $kept ("{0}.exe" -f (Get-FileHash -LiteralPath $exe).Hash)) -Force
}
# x86-64-v3 is the system requirement; this tool gains nothing from it, but it must not need less either.
& $clang @('/nologo', '/O2', '/MT', '/W4', '/WX', '-march=x86-64-v3', '-mtune=znver2', '-fuse-ld=lld',
    "/Fo$(Join-Path $Out 'tdr.obj')", "/Fe$exe", (Join-Path $here 'tdr.c'), '/link', 'gdi32.lib', 'advapi32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "bc250tdr.exe: compiler failed ($LASTEXITCODE)" }

# Smoke checks that need no BC-250: a usage error is refused, and the help path runs.
& $exe --force adapter --node 1 | Out-Null
if ($LASTEXITCODE -ne 2) { throw "bc250tdr.exe accepted --force adapter --node 1 (exit $LASTEXITCODE)" }
& $exe | Out-Null
if ($LASTEXITCODE -ne 2) { throw "bc250tdr.exe accepted an empty command line (exit $LASTEXITCODE)" }
'{0,9}  {1}  sha256 {2}' -f (Get-Item $exe).Length, 'bc250tdr.exe', (Get-FileHash -LiteralPath $exe).Hash

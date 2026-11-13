# SPDX-License-Identifier: MIT
# Builds shared-interop.exe (BD-373, the D3D11-to-D3D12 shared resource probe) like tools\win\capture-share\build.ps1:
# headers and import libraries from the SDK NuGet packages under -Kits, the C runtime of the installed Visual Studio,
# temporary files under scratch\tmp. The compiler is clang-cl with lld-link of LLVM 23.1.4 (tools\build\umd-compiler.ps1:
# x86-64-v3, the CRT and SDK as system headers), or cl and link (-Compiler msvc, which needs -MsvcReason). Both link
# with /Brepro. The build ends with the --help check, the argument check, SHA256SUMS.txt and the hash on the console.
#
#   pwsh tools\win\shared-interop\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget [-Out <dir>]

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\shared-interop",
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl',
    [string]$MsvcReason = '',
    [string]$VsInstall = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Kits = [IO.Path]::GetFullPath($Kits)
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$workspace = Split-Path (Split-Path $Kits -Parent) -Parent

. (Join-Path $repo 'tools\build\umd-compiler.ps1')
$tool = Resolve-Bc250Toolchain $VsInstall 'x64'
$msvc = $tool.Msvc
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
New-Item -ItemType Directory -Force (Join-Path $Out 'obj') | Out-Null
$env:TEMP = Join-Path $workspace 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$headers = @((Join-Path $msvc 'include'), "$sdk\Include\$KitVersion\ucrt", "$sdk\Include\$KitVersion\um",
    "$sdk\Include\$KitVersion\shared", "$sdk\Include\$KitVersion\winrt")
$libs = @((Join-Path $msvc 'lib\x64'), "$sdkLib\ucrt\x64", "$sdkLib\um\x64")
# INCLUDE and LIB name only the Visual Studio of this recipe and the SDK packages (see capture-share\build.ps1).
$env:INCLUDE = $headers -join ';'
$env:LIB = $libs -join ';'
$env:PATH = "$(Join-Path $msvc 'bin\Hostx64\x64');$env:PATH"
$cc = Resolve-Bc250UmdCompiler $workspace $Compiler $MsvcReason $Out 'x64' $tool.Cl
$repro = Get-Bc250ReproducibleFlags $Compiler
$own = @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++20', '/permissive-', '/Z7', '/Brepro',
    '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN')
$flags = if ($Compiler -eq 'clang-cl') {
    @(ConvertTo-Bc250ClangFlags $own 'x64' @($repo)) + $repro.Compile + '/clang:-fuse-ld=lld'
} else { $own + @($headers | ForEach-Object { "/I$_" }) }

# Compile and link in -Out with relative paths, so the PDB does not depend on the output directory (BD-268).
Push-Location -LiteralPath $Out
try {
    Invoke-Bc250UmdCompiler $cc @flags '/Foobj\' '/Feshared-interop.exe' (Join-Path $here 'shared-interop.cpp') '/link' `
        '/Brepro' '/SUBSYSTEM:CONSOLE' '/DEBUG' '/OPT:REF' '/OPT:ICF' '/PDB:shared-interop.pdb' '/PDBALTPATH:%_PDB%' `
        @($repro.Link) 'd3d11.lib' 'd3d12.lib' 'dxgi.lib' 'kernel32.lib' |
        ForEach-Object { if ($_ -notmatch '^\s*$|^\S+\.cpp$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$Compiler failed ($LASTEXITCODE)" }
} finally { Pop-Location }

$exe = Join-Path $Out 'shared-interop.exe'
& $exe --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "help check failed ($LASTEXITCODE)" }
& $exe --bad-option 2>$null | Out-Null
if ($LASTEXITCODE -ne 2) { throw "argument check failed ($LASTEXITCODE)" }
$hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
[IO.File]::WriteAllText((Join-Path $Out 'SHA256SUMS.txt'), ('{0} *shared-interop.exe' -f $hash.ToLowerInvariant()) + "`n")
Get-Item $exe | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, $hash }

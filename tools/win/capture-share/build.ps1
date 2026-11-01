# SPDX-License-Identifier: MIT
# Builds capshare.exe (M15.13 capture and shared-surface witness): headers and import libraries from the SDK NuGet
# packages under -Kits (C++/WinRT projection included), the C runtime of the installed Visual Studio, temp files under
# scratch\tmp. The compiler is clang-cl with lld-link of LLVM 23.1.4 (-Compiler clang-cl, the default, through
# tools\build\umd-compiler.ps1: x86-64-v3, the CRT and SDK as system headers, the version on the console and in
# compiler.json), or cl and link of that Visual Studio (-Compiler msvc, which needs -MsvcReason). Both link with
# /Brepro. capshare-peer.exe is a byte copy: a second image name, so that the application router's allowlist can send
# the two processes of a cell to different D3D11 UMDs. Ends with SHA256SUMS.txt (sha256sum format) and the hashes on
# the console.
#
#   pwsh tools\win\capture-share\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget [-Out <dir>]

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\capture-share",
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
$cl = $tool.Cl
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
New-Item -ItemType Directory -Force (Join-Path $Out 'obj') | Out-Null
$env:TEMP = Join-Path $workspace 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$headers = @((Join-Path $msvc 'include'), "$sdk\Include\$KitVersion\ucrt", "$sdk\Include\$KitVersion\um",
    "$sdk\Include\$KitVersion\shared", "$sdk\Include\$KitVersion\winrt", "$sdk\Include\$KitVersion\cppwinrt")
$libs = @((Join-Path $msvc 'lib\x64'), "$sdkLib\ucrt\x64", "$sdkLib\um\x64")
# INCLUDE and LIB name only the Visual Studio of this recipe and the SDK packages, so neither compiler adds the
# headers or libraries of an installed SDK. clang-cl reads the INCLUDE roots as system headers: /W4 /WX diagnose
# this code only. lld-link and link read LIB.
$env:INCLUDE = $headers -join ';'
$env:LIB = $libs -join ';'
$env:PATH = "$(Join-Path $msvc 'bin\Hostx64\x64');$env:PATH"
$cc = Resolve-Bc250UmdCompiler $workspace $Compiler $MsvcReason $Out 'x64' $cl
$repro = Get-Bc250ReproducibleFlags $Compiler
# NOMINMAX and WIN32_LEAN_AND_MEAN on the command line: the clang-cl prelude (tools\build\clang-windows.h) includes
# windows.h before the first line of a source, so the defines at the top of capshare.h come too late there.
# C++20: under C++17, C++/WinRT (winrt/base.h) includes <experimental/coroutine>, which the Visual Studio library
# refuses to compile with Clang. Under C++20 it uses <coroutine>, for both compilers.
$own = @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++20', '/permissive-', '/bigobj', '/Z7', '/Brepro',
    '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN')
$flags = if ($Compiler -eq 'clang-cl') {
    @(ConvertTo-Bc250ClangFlags $own 'x64' @($repo)) + $repro.Compile + '/clang:-fuse-ld=lld'
} else { $own + @($headers | ForEach-Object { "/I$_" }) }

# BD-268: compile and link in -Out with paths relative to it, so the PDB, and with it the PDB GUID and the PE time
# stamp of the /Brepro link, do not depend on the output directory.
$sources = 'main.cpp', 'common.cpp', 'sides.cpp', 'cells.cpp', 'capture.cpp', 'alpha-sample.cpp' | ForEach-Object { Join-Path $here $_ }
Push-Location -LiteralPath $Out
try {
    Invoke-Bc250UmdCompiler $cc @flags '/Foobj\' '/Fecapshare.exe' @sources '/link' '/Brepro' '/SUBSYSTEM:WINDOWS' '/DEBUG' `
        '/OPT:REF' '/OPT:ICF' '/PDB:capshare.pdb' '/PDBALTPATH:%_PDB%' @($repro.Link) 'kernel32.lib' |
        ForEach-Object { if ($_ -notmatch '^\s*$|^\S+\.cpp$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$Compiler failed ($LASTEXITCODE)" }
} finally { Pop-Location }

Copy-Item -LiteralPath "$Out\capshare.exe" -Destination "$Out\capshare-peer.exe" -Force
$lines = foreach ($name in 'capshare.exe', 'capshare-peer.exe') {
    $hash = (Get-FileHash -LiteralPath (Join-Path $Out $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    '{0} *{1}' -f $hash, $name
}
[IO.File]::WriteAllText((Join-Path $Out 'SHA256SUMS.txt'), (($lines -join "`n") + "`n"))
Get-Item "$Out\capshare.exe", "$Out\capshare-peer.exe" |
    ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

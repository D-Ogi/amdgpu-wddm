# SPDX-License-Identifier: MIT
# Builds capshare.exe (M15.13 capture and shared-surface witness) like tools\win\d3d11mt\build.ps1: headers and import
# libraries from the SDK NuGet packages under -Kits (C++/WinRT projection included), the compiler from the installed
# Visual Studio, temp files under scratch\tmp. capshare-peer.exe is a byte copy: a second image name, so that the
# application router's allowlist can send the two processes of a cell to different D3D11 UMDs. Ends with
# SHA256SUMS.txt (sha256sum format) and the hashes on the console.
#
#   pwsh tools\win\capture-share\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\capture-share",
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
$root = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$include = @("/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/I$sdk\Include\$KitVersion\cppwinrt")
$env:INCLUDE = ''; $env:LIB = ''

$sources = 'main.cpp', 'common.cpp', 'sides.cpp', 'cells.cpp', 'capture.cpp' | ForEach-Object { Join-Path $here $_ }
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/permissive-', '/bigobj', '/Zi', '/DUNICODE', '/D_UNICODE') `
    @include "/Fo$obj\" "/Fd$obj\vc.pdb" "/Fe$Out\capshare.exe" @sources '/link' '/SUBSYSTEM:WINDOWS' '/DEBUG' '/OPT:REF' `
    '/OPT:ICF' "/PDB:$Out\capshare.pdb" "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')" "/LIBPATH:$sdkLib\ucrt\x64" `
    "/LIBPATH:$sdkLib\um\x64" 'kernel32.lib' |
    ForEach-Object { if ($_ -notmatch '^\s*$|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

Copy-Item -LiteralPath "$Out\capshare.exe" -Destination "$Out\capshare-peer.exe" -Force
$lines = foreach ($name in 'capshare.exe', 'capshare-peer.exe') {
    $hash = (Get-FileHash -LiteralPath (Join-Path $Out $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    '{0} *{1}' -f $hash, $name
}
[IO.File]::WriteAllText((Join-Path $Out 'SHA256SUMS.txt'), (($lines -join "`n") + "`n"))
Get-Item "$Out\capshare.exe", "$Out\capshare-peer.exe" |
    ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

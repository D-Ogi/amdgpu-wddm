# SPDX-License-Identifier: MIT
# Builds flash-watch.exe (the frame recorder of measure-flash.ps1) like tools\win\capture-share\build.ps1: headers and
# import libraries from the SDK NuGet packages under -Kits, the compiler from the installed Visual Studio, temp files
# under scratch\tmp. Runs the program's --self-test before it reports the hash. No window is opened here.
#
#   pwsh tools\win\app-launch\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\app-launch",
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
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt")
$env:INCLUDE = ''; $env:LIB = ''

& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/permissive-', '/Zi', '/DUNICODE', '/D_UNICODE') `
    @include "/Fo$obj\" "/Fd$obj\vc.pdb" "/Fe$Out\flash-watch.exe" (Join-Path $here 'flash-watch.cpp') '/link' `
    '/SUBSYSTEM:WINDOWS' '/DEBUG' '/OPT:REF' '/OPT:ICF' "/PDB:$Out\flash-watch.pdb" `
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')" "/LIBPATH:$sdkLib\ucrt\x64" "/LIBPATH:$sdkLib\um\x64" 'kernel32.lib' |
    ForEach-Object { if ($_ -notmatch '^\s*$|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

# The detector and the colour classes, on synthetic frames: no window, no screen, no device.
$test = Start-Process -FilePath "$Out\flash-watch.exe" -ArgumentList '--self-test' -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput "$Out\self-test.txt"
Get-Content -LiteralPath "$Out\self-test.txt" | ForEach-Object { "  $_" }
if ($test.ExitCode -ne 0) { throw "flash-watch --self-test failed ($($test.ExitCode))" }

$hash = (Get-FileHash -LiteralPath "$Out\flash-watch.exe" -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $Out 'SHA256SUMS.txt'), "$hash *flash-watch.exe`n")
'{0,9}  flash-watch.exe  sha256 {1}' -f (Get-Item "$Out\flash-watch.exe").Length, $hash

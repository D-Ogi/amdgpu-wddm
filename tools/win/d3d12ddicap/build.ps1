# Builds d3d12ddicap.exe on the development PC and, unless -NoRun, runs it there on WARP. Nothing here touches unit A.
# The three ray tracing libraries come from ue426.hlsl through the SDK's dxc (signed by its dxil.dll), into the
# output directory as headers; the program includes them from there.
param(
    [string]$Kits = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\toolchain\nuget",
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\d3d12ddicap",
    [string]$KitVersion = '10.0.26100.0',
    [switch]$NoRun,
    # The cases to run (ddicap.cpp, main); empty: all.
    [string[]]$Cases = @()
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$wdk = Join-Path $Kits "microsoft.windows.wdk.x64\c\Include\$KitVersion"
$dxc = Join-Path $sdk "bin\$KitVersion\x64\dxc.exe"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = $Out; $env:TMP = $Out

foreach ($stage in 'RGS', 'MS', 'HIT') {
    $name = $stage.ToLowerInvariant()
    & $dxc -nologo -T lib_6_3 -D $stage -Vn "g_ue426_$name" -Fh (Join-Path $Out "ue426-$name.h") (Join-Path $here 'ue426.hlsl')
    if ($LASTEXITCODE -ne 0) { throw "dxc $stage failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++20', '/DNOMINMAX',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/external:I$wdk\um", "/external:I$wdk\shared", '/external:W0',
    "/I$Out", "/Fo$Out\d3d12ddicap.obj", "/Fe$Out\d3d12ddicap.exe", (Join-Path $here 'ddicap.cpp'), '/link',
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'd3d12.lib', 'dxgi.lib', 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }
Get-Item "$Out\d3d12ddicap.exe" | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
if ($NoRun) { return }
& "$Out\d3d12ddicap.exe" @Cases
if ($LASTEXITCODE -ne 0) { throw "d3d12ddicap failed ($LASTEXITCODE)" }

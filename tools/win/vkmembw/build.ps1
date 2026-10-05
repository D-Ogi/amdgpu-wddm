param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\vkmembw",
    [string]$KitVersion = '10.0.26100.0',
    [string]$Glslang = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\glslang\bin\glslangValidator.exe",
    [string]$VulkanInclude = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\m15\vkd3d\khronos\Vulkan-Headers\include"
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$root = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its full hash.
$previous = Join-Path $Out 'vkmembw.exe'
if (Test-Path -LiteralPath $previous) {
    $hash = (Get-FileHash -LiteralPath $previous).Hash
    $keep = Join-Path $Out "retained\vkmembw-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $previous -Destination $keep
    }
}

# SPIR-V 1.0 with Vulkan 1.0 semantics, embedded as C arrays (write_spv, copy_spv, read_spv).
foreach ($k in 'write', 'copy', 'read') {
    & $Glslang -V --vn "${k}_spv" -o (Join-Path $Out "${k}_spv.h") (Join-Path $here "shaders\$k.comp") | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "glslang failed on $k.comp ($LASTEXITCODE)" }
}

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/TC', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$Out", "/I$VulkanInclude",
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\vkmembw.obj", "/Fe$Out\vkmembw.exe", (Join-Path $here 'vkmembw.c'),
    '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

& "$Out\vkmembw.exe" --mib 2>$null
if ($LASTEXITCODE -ne 2) { throw 'usage check failed' }
Get-ChildItem $Out -Filter *.h | Sort-Object Name | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
Get-Item "$Out\vkmembw.exe" | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

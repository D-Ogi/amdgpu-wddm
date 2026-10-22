param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\vkmembw",
    [string]$KitVersion = '10.0.26100.0',
    [string]$Glslang = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\glslang\bin\glslangValidator.exe",
    [string]$VulkanInclude = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\m15\vkd3d\khronos\Vulkan-Headers\include",
    [string]$Clang = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\toolchain\llvm-23.1.4-win64\bin\clang-cl.exe"
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
if (-not (Test-Path -LiteralPath $Clang)) { throw "Required clang-cl not found: $Clang" }
& $Clang --version
if ($LASTEXITCODE) { throw 'clang-cl version query failed' }
New-Item -ItemType Directory -Force $Out | Out-Null
$root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $here '..\..\..\..')).Path }
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
& $Clang @('-fuse-ld=lld', '-march=x86-64-v3', '-mtune=znver2', '/nologo', '/W4', '/WX', '/O2', '/MT', '/TC', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$Out", "/imsvc$VulkanInclude",
    "/imsvc$(Join-Path $msvc.FullName 'include')", "/imsvc$sdk\Include\$KitVersion\ucrt", "/imsvc$sdk\Include\$KitVersion\um",
    "/imsvc$sdk\Include\$KitVersion\shared", "/Fo$Out\vkmembw.obj", "/Fe$Out\vkmembw.exe", (Join-Path $here 'vkmembw.c'),
    '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "clang-cl failed ($LASTEXITCODE)" }

& $Clang @('-fuse-ld=lld', '-march=x86-64-v3', '-mtune=znver2', '/nologo', '/W4', '/WX', '/O2', '/MT', '/TC',
    "/imsvc$(Join-Path $msvc.FullName 'include')", "/imsvc$sdk\Include\$KitVersion\ucrt",
    "/Fo$Out\test-check.obj", "/Fe$Out\test-check.exe", (Join-Path $here 'test-check.c'),
    '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64")
if ($LASTEXITCODE) { throw 'Check fixture build failed' }
& "$Out\test-check.exe"
if ($LASTEXITCODE) { throw 'Check fixture failed' }

& "$Out\vkmembw.exe" --mib 2>$null
if ($LASTEXITCODE -ne 2) { throw 'usage check failed' }
Get-ChildItem $Out -Filter *.h | Sort-Object Name | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
Get-Item "$Out\vkmembw.exe" | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

# Builds vkace.exe (x64) with clang-cl, the project's user-mode compiler, without a WDK or SDK installation:
# headers and import libraries come from the SDK NuGet packages under -Kits, the C runtime from the installed
# Visual Studio, the Vulkan headers from -VulkanInclude and the SPIR-V of shaders\stage.comp from glslang.
#
# The build ends with --help, a usage check, a self-test on the first Vulkan device of this PC that has a
# compute-only queue family (skipped when there is none), and the SHA-256 of the program, which a lab kit pins.
#
#   pwsh tools\win\vkace\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget
#   pwsh tools\win\vkace\build.ps1 -Kits ... -Out $env:BC250_ROOT\scratch\build\vkace -SkipSelfTest

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\vkace",
    [string]$KitVersion = '10.0.26100.0',
    [string]$Glslang = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\glslang\bin\glslangValidator.exe",
    [string]$VulkanInclude = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\m15\vkd3d\khronos\Vulkan-Headers\include",
    [string]$Clang = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\toolchain\llvm-23.1.4-win64\bin\clang-cl.exe",
    [switch]$SkipSelfTest
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
foreach ($tool in @($Clang, $Glslang)) { if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool not found: $tool" } }
if (-not (Test-Path -LiteralPath (Join-Path $VulkanInclude 'vulkan\vk_icd.h'))) {
    throw "Vulkan headers not found: $VulkanInclude (expected vulkan\vk_icd.h)"
}
Write-Host "  $((& $Clang --version | Select-Object -First 1))"
if ($LASTEXITCODE) { throw 'clang-cl version query failed' }
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
New-Item -ItemType Directory -Force $Out | Out-Null
$root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $here '..\..\..\..')).Path }
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its full hash.
$exe = Join-Path $Out 'vkace.exe'
if (Test-Path -LiteralPath $exe) {
    $hash = (Get-FileHash -LiteralPath $exe).Hash
    $keep = Join-Path $Out "retained\vkace-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $exe -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\vkace-$hash.exe"
}

# SPIR-V 1.0 with Vulkan 1.0 semantics, embedded as a C array (stage_spv).
& $Glslang -V --vn stage_spv -o (Join-Path $Out 'stage_spv.h') (Join-Path $here 'shaders\stage.comp') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "glslang failed on stage.comp ($LASTEXITCODE)" }

$env:INCLUDE = ''; $env:LIB = ''
& $Clang @('-fuse-ld=lld', '-march=x86-64-v3', '-mtune=znver2', '/nologo', '/W4', '/WX', '/O2', '/MT', '/TC', '/std:c11',
    '/D_CRT_SECURE_NO_WARNINGS', '/Brepro', "/I$Out", "/imsvc$VulkanInclude",
    "/imsvc$(Join-Path $msvc.FullName 'include')", "/imsvc$sdk\Include\$KitVersion\ucrt", "/imsvc$sdk\Include\$KitVersion\um",
    "/imsvc$sdk\Include\$KitVersion\shared", "/Fo$Out\vkace.obj", "/Fe$exe", (Join-Path $here 'vkace.c'),
    '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'kernel32.lib', 'psapi.lib', 'bcrypt.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "clang-cl failed ($LASTEXITCODE)" }

& $exe --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "help check failed ($LASTEXITCODE)" }
& $exe --bad-option 2>$null | Out-Null
if ($LASTEXITCODE -ne 2) { throw "usage check failed ($LASTEXITCODE)" }
& $exe --step 7 2>$null | Out-Null
if ($LASTEXITCODE -ne 2) { throw "step range check failed ($LASTEXITCODE)" }

# The self-test runs steps 1 to 5 on the first Vulkan device of this PC. Exit 3 (no usable device) and exit 5
# (no compute-only family) skip it. Any other failure stops the build: another vendor's driver passing every
# check is the positive control of the client itself.
if ($SkipSelfTest) {
    Write-Host '  self-test skipped on request'
} else {
    $log = Join-Path $Out 'selftest.log'
    & $exe --any-device --deadline 120 > $log 2>&1
    $code = $LASTEXITCODE
    if ($code -eq 3 -or $code -eq 5) {
        Write-Host "  self-test skipped: exit $code ($((Get-Content $log | Select-String 'FAIL' | Select-Object -First 1)))"
    } else {
        Get-Content $log | Where-Object { $_ -match '^(FAIL|INFO|PASS)|^vkace |^device|^icd' } | ForEach-Object { Write-Host "    $_" }
        if ($code -ne 0) { throw "self-test failed ($code)" }
    }
}

Get-Item (Join-Path $Out 'stage_spv.h'), $exe | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

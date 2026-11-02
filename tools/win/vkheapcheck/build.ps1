#Requires -Version 7.0
# Builds vkheapcheck for x64 (and x86 on request) with clang-cl of -ClangToolchain, without a WDK or SDK
# installation: headers and import libraries come from the SDK NuGet packages under -Kits, the Vulkan headers from
# -VulkanInclude, the C runtime headers and libraries from the installed Visual Studio. Same flow as
# tools\win\vkfillcheck\build.ps1. The build ends with --help, a usage check, a self-test when this PC has a Vulkan
# device, and a SHA-256 of each artifact, the receipt a lab kit pins.
#
#   pwsh tools\win\vkheapcheck\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\vkheapcheck",
    [string]$KitVersion = '10.0.26100.0',
    [string]$VulkanInclude = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\m15\vkd3d\khronos\Vulkan-Headers\include",
    [string]$ClangToolchain = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\toolchain\llvm-23.1.4-win64",
    [ValidateSet('x64', 'x86', 'both')][string]$Arch = 'x64',
    [switch]$SkipSelfTest
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
if (-not (Test-Path -LiteralPath (Join-Path $VulkanInclude 'vulkan\vk_icd.h'))) {
    throw "Vulkan headers not found: $VulkanInclude (expected vulkan\vk_icd.h)"
}
$clang = Join-Path $ClangToolchain 'bin\clang-cl.exe'
if (-not (Test-Path -LiteralPath $clang)) { throw "clang-cl not found: $clang (no fallback to cl.exe)" }
Write-Host "  compiler $clang : $((& $clang --version | Select-Object -First 1))"
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
New-Item -ItemType Directory -Force $Out | Out-Null
$root = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$targets = if ($Arch -eq 'both') { @('x64', 'x86') } else { @($Arch) }
$built = @()
foreach ($a in $targets) {
    $sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$a\c"
    if (-not (Test-Path -LiteralPath $sdkLib)) { throw "no $a SDK libraries: $sdkLib" }
    $exe = Join-Path $Out "vkheapcheck-$a.exe"
    if (Test-Path -LiteralPath $exe) {
        $hash = (Get-FileHash -LiteralPath $exe).Hash
        $keep = Join-Path $Out "retained\vkheapcheck-$a-$hash.exe"
        if (-not (Test-Path -LiteralPath $keep)) {
            New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
            Copy-Item -LiteralPath $exe -Destination $keep
        }
    }
    $target = if ($a -eq 'x86') { 'i686-pc-windows-msvc' } else { 'x86_64-pc-windows-msvc' }
    Write-Host "  clang-cl $a"
    $env:INCLUDE = ''; $env:LIB = ''
    & $clang @("--target=$target", '/nologo', '/W4', '/WX', '/O2', '/MT', '/TC', '/std:c11', '/Brepro',
        '/D_CRT_SECURE_NO_WARNINGS', "/I$VulkanInclude",
        # /imsvc: the C runtime and SDK headers are system headers, so /WX applies to our code only.
        "/imsvc$(Join-Path $msvc.FullName 'include')", "/imsvc$sdk\Include\$KitVersion\ucrt",
        "/imsvc$sdk\Include\$KitVersion\um", "/imsvc$sdk\Include\$KitVersion\shared", "/Fo$Out\vkheapcheck-$a.obj", "/Fe$exe",
        (Join-Path $here 'vkheapcheck.c'),
        '/link', "/LIBPATH:$(Join-Path $msvc.FullName "lib\$a")", "/LIBPATH:$sdkLib\ucrt\$a", "/LIBPATH:$sdkLib\um\$a",
        'kernel32.lib', 'psapi.lib', 'bcrypt.lib') | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) { throw "clang-cl failed for $a ($LASTEXITCODE)" }
    $built += $exe
}

foreach ($exe in $built) {
    & $exe --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "help check failed for $exe ($LASTEXITCODE)" }
    & $exe --bad-option 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "usage check failed for $exe ($LASTEXITCODE)" }
    & $exe --expect neither 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "usage check of --expect failed for $exe ($LASTEXITCODE)" }
    & $exe --readback 6 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "usage check of --readback failed for $exe ($LASTEXITCODE)" }
}

# The self-test needs a Vulkan device of any vendor. Without one the build still succeeds and says so.
if (-not $SkipSelfTest) {
    foreach ($exe in $built) {
        $name = Split-Path -Leaf $exe
        Write-Host "  self-test $name : --any-device --expect-cached --readback 1024"
        & $exe --any-device --expect-cached --readback 1024 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -eq 3) { Write-Host "  self-test skipped for ${name}: no usable Vulkan device on this PC"; continue }
        if ($LASTEXITCODE -ne 0) { throw "self-test failed for $name ($LASTEXITCODE)" }
    }
}

Get-ChildItem $Out -Filter 'vkheapcheck-*.exe' | Sort-Object Name | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

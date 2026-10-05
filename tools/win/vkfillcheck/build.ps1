# Builds vkfillcheck.exe without a WDK or SDK installation: headers and import libraries come from the SDK NuGet
# packages unpacked under -Kits, the Vulkan headers from -VulkanInclude, the compiler from the installed Visual
# Studio. Same flow as tools\win\vkmembw\build.ps1, without the shader step: vkfillcheck has no shaders of its own.
# The build ends with --help, a quick self-test when this PC has a Vulkan device, and a SHA-256 of the artifact,
# the receipt a lab runner pins.
#
#   pwsh tools\win\vkfillcheck\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\vkfillcheck",
    [string]$KitVersion = '10.0.26100.0',
    [string]$VulkanInclude = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\m15\vkd3d\khronos\Vulkan-Headers\include",
    [switch]$SkipSelfTest
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
if (-not (Test-Path -LiteralPath (Join-Path $VulkanInclude 'vulkan\vk_icd.h'))) {
    throw "Vulkan headers not found: $VulkanInclude (expected vulkan\vk_icd.h)"
}
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$root = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its full hash.
$previous = Join-Path $Out 'vkfillcheck.exe'
if (Test-Path -LiteralPath $previous) {
    $hash = (Get-FileHash -LiteralPath $previous).Hash
    $keep = Join-Path $Out "retained\vkfillcheck-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $previous -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\vkfillcheck-$hash.exe"
}

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/TC', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$VulkanInclude",
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\vkfillcheck.obj", "/Fe$Out\vkfillcheck.exe",
    (Join-Path $here 'vkfillcheck.c'),
    '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'kernel32.lib', 'psapi.lib', 'bcrypt.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

& "$Out\vkfillcheck.exe" --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "help check failed ($LASTEXITCODE)" }
& "$Out\vkfillcheck.exe" --bad-option 2>$null | Out-Null
if ($LASTEXITCODE -ne 2) { throw "usage check failed ($LASTEXITCODE)" }

# The self-test needs a Vulkan device of any vendor. Without one the build still succeeds and says so: a
# development PC without a Vulkan driver is not a reason to refuse the artifact.
if ($SkipSelfTest) {
    Write-Host '  self-test skipped on request'
} else {
    & "$Out\vkfillcheck.exe" --list-devices | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  self-test skipped: this PC has no Vulkan device'
    } else {
        Write-Host '  self-test: --quick --any-device'
        & "$Out\vkfillcheck.exe" --quick --any-device --only-fail | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { throw "self-test failed ($LASTEXITCODE)" }
        # The negative control proves the comparison can fail: displaced operations must fail every case.
        Write-Host '  negative control: --quick --negative-control (every case must fail)'
        & "$Out\vkfillcheck.exe" --quick --any-device --negative-control --only-fail |
            Select-Object -Last 3 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 1) { throw "negative control did not fail as required ($LASTEXITCODE)" }
    }
}

Get-Item "$Out\vkfillcheck.exe" | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

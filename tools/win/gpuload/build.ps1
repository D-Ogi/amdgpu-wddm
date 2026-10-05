# Builds gpuload.exe and its host test without a WDK or SDK installation: headers and import libraries come from the
# SDK NuGet packages under -Kits, the compiler from the installed Visual Studio (as tools\win\vkmembw\build.ps1).
# Then runs everything that can run on the development PC without a GPU: the host test (gpuload_logic.h), the
# client's usage check (exit 2) and --format-sample. Keeps a replaced binary under retained\ by its hash and prints
# the SHA256 of what it built; /Brepro makes two builds of the same source the same bytes.
#
#   pwsh bc250-win\tools\win\gpuload\build.ps1
#   pwsh bc250-win\tools\win\gpuload\build.ps1 -Kits P:\bc-250\toolchain\nuget -Out P:\bc-250\scratch\build\gpuload

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Kits = '',
    [string]$Out = '',
    [string]$KitVersion = '10.0.26100.0',
    [string]$Glslang = '',
    [string]$VulkanInclude = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Kits) { $Kits = Join-Path $Root 'toolchain\nuget' }
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\gpuload' }
if (-not $Glslang) { $Glslang = Join-Path $Root 'scratch\glslang\bin\glslangValidator.exe' }
if (-not $VulkanInclude) { $VulkanInclude = Join-Path $Root 'scratch\m15\vkd3d\khronos\Vulkan-Headers\include' }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
# Compiler temporaries stay off drive C: of the development PC.
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its full hash.
$previous = Join-Path $Out 'gpuload.exe'
if (Test-Path -LiteralPath $previous) {
    $hash = (Get-FileHash -LiteralPath $previous).Hash
    $keep = Join-Path $Out "retained\gpuload-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $previous -Destination $keep
    }
}

$common = @('/nologo', '/W4', '/WX', '/O2', '/MT', '/TC', '/std:c11', '/Brepro', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$Out", "/I$VulkanInclude",
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared")
$link = @('/link', '/Brepro', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'kernel32.lib')
function Invoke-Cl([string[]]$Arguments, [string]$What) {
    $env:INCLUDE = ''; $env:LIB = ''
    & $cl @Arguments | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $What ($LASTEXITCODE)" }
}

# Host test first: a client whose logic fails its checks is not worth building.
Invoke-Cl ($common + @("/Fo$Out\gpuload_host_test.obj", "/Fe$Out\gpuload_host_test.exe",
    (Join-Path $here 'gpuload_host_test.c')) + $link) 'gpuload_host_test'
& "$Out\gpuload_host_test.exe" | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw "gpuload_host_test failed ($LASTEXITCODE checks)" }

# SPIR-V 1.0 with Vulkan 1.0 semantics, embedded as a C array (spin_spv).
& $Glslang -V --vn spin_spv -o (Join-Path $Out 'spin_spv.h') (Join-Path $here 'shaders\spin.comp') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "glslang failed on spin.comp ($LASTEXITCODE)" }

Invoke-Cl ($common + @("/Fo$Out\gpuload.obj", "/Fe$Out\gpuload.exe", (Join-Path $here 'gpuload.c')) + $link) 'gpuload'

& "$Out\gpuload.exe" --seconds 2>$null
if ($LASTEXITCODE -ne 2) { throw "usage check failed ($LASTEXITCODE)" }
& "$Out\gpuload.exe" --seconds 0 2>$null
if ($LASTEXITCODE -ne 2) { throw "range check failed ($LASTEXITCODE)" }
$sample = @(& "$Out\gpuload.exe" --format-sample)
if ($LASTEXITCODE -ne 0 -or $sample.Count -ne 2 -or $sample[1] -notmatch '^gpuload result=ok ') { throw '--format-sample failed' }
Write-Host '  usage, range and --format-sample checks pass'

foreach ($f in @('spin_spv.h', 'gpuload.exe', 'gpuload_host_test.exe')) {
    $item = Get-Item (Join-Path $Out $f)
    '{0,9}  {1}  sha256 {2}' -f $item.Length, $item.Name, (Get-FileHash -LiteralPath $item.FullName).Hash
}

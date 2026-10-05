# Builds vkfillcheck for x64 and for x86 without a WDK or SDK installation: headers and import libraries
# come from the SDK NuGet packages unpacked under -Kits, the Vulkan headers from -VulkanInclude, the
# compiler from the installed Visual Studio. Same flow as tools\win\vkmembw\build.ps1, without the shader
# step: vkfillcheck has no shaders of its own.
#
# Both architectures are built by default, because the ICD comes as a pair: amdgpu_wddm_radv.dll is built
# for x64 and for x86 (the WOW64 clients), and an x86 DLL cannot be loaded into a 64-bit process. A gate
# that only ran the x64 program would cover half of a release.
#
# The build ends with --help, a usage check, a quick self-test when this PC has a Vulkan device, and a
# SHA-256 of each artifact, the receipt a lab runner pins.
#
#   pwsh tools\win\vkfillcheck\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget
#   pwsh tools\win\vkfillcheck\build.ps1 -Kits ... -Arch x64        # only the 64-bit program

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\vkfillcheck",
    [string]$KitVersion = '10.0.26100.0',
    [string]$VulkanInclude = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\m15\vkd3d\khronos\Vulkan-Headers\include",
    [ValidateSet('x64', 'x86', 'both')][string]$Arch = 'both',
    [switch]$SkipSelfTest
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
if (-not (Test-Path -LiteralPath (Join-Path $VulkanInclude 'vulkan\vk_icd.h'))) {
    throw "Vulkan headers not found: $VulkanInclude (expected vulkan\vk_icd.h)"
}
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
New-Item -ItemType Directory -Force $Out | Out-Null
$root = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$targets = if ($Arch -eq 'both') { @('x64', 'x86') } else { @($Arch) }
$built = @()
foreach ($a in $targets) {
    $cl = Join-Path $msvc.FullName "bin\Hostx64\$a\cl.exe"
    if (-not (Test-Path -LiteralPath $cl)) { throw "no $a compiler: $cl" }
    $sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$a\c"
    if (-not (Test-Path -LiteralPath $sdkLib)) { throw "no $a SDK libraries: $sdkLib" }
    $exe = Join-Path $Out "vkfillcheck-$a.exe"

    # A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its
    # full hash.
    if (Test-Path -LiteralPath $exe) {
        $hash = (Get-FileHash -LiteralPath $exe).Hash
        $keep = Join-Path $Out "retained\vkfillcheck-$a-$hash.exe"
        if (-not (Test-Path -LiteralPath $keep)) {
            New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
            Copy-Item -LiteralPath $exe -Destination $keep
        }
        Write-Host "  previous $a artifact retained as retained\vkfillcheck-$a-$hash.exe"
    }

    Write-Host "  cl $a"
    $env:INCLUDE = ''; $env:LIB = ''
    & $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/TC', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
        "/I$VulkanInclude",
        "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
        "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\vkfillcheck-$a.obj", "/Fe$exe",
        (Join-Path $here 'vkfillcheck.c'),
        '/link', "/LIBPATH:$(Join-Path $msvc.FullName "lib\$a")", "/LIBPATH:$sdkLib\ucrt\$a", "/LIBPATH:$sdkLib\um\$a",
        'kernel32.lib', 'psapi.lib', 'bcrypt.lib') |
        ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "    $_" } }
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $a ($LASTEXITCODE)" }
    $built += $exe
}

# The image of each artifact must be the architecture it was asked for: a 64-bit program cannot test the
# x86 ICD, and the whole point of building both is that each one tests its own half.
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
foreach ($exe in $built) {
    $want = if ($exe -match 'x86\.exe$') { '14C machine \(x86\)' } else { '8664 machine \(x64\)' }
    $hdr = & $dumpbin /nologo /headers $exe | Select-String 'machine \('
    if ("$hdr" -notmatch $want) { throw "$exe is not the expected image: $hdr" }
    Write-Host "  image $(Split-Path -Leaf $exe): $(($hdr -join ' ').Trim())"
}

foreach ($exe in $built) {
    & $exe --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "help check failed for $exe ($LASTEXITCODE)" }
    & $exe --bad-option 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "usage check failed for $exe ($LASTEXITCODE)" }
}

# The self-test needs a Vulkan device of any vendor. Without one the build still succeeds and says so: a
# development PC without a Vulkan driver is not a reason to refuse the artifact. An x86 program needs a
# 32-bit ICD on this PC, which a development PC may not have, so its self-test is not required either.
if ($SkipSelfTest) {
    Write-Host '  self-test skipped on request'
} else {
    foreach ($exe in $built) {
        $name = Split-Path -Leaf $exe
        & $exe --list-devices | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  self-test skipped for ${name}: this PC offers it no Vulkan device"
            continue
        }
        Write-Host "  self-test $name : --quick --any-device"
        & $exe --quick --any-device --only-fail | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { throw "self-test failed for $name ($LASTEXITCODE)" }
        # The negative control proves the comparison can fail: displaced operations must fail every case.
        Write-Host "  negative control $name : --quick --negative-control (every case must fail)"
        & $exe --quick --any-device --negative-control --only-fail |
            Select-Object -Last 3 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 1) { throw "negative control did not fail as required for $name ($LASTEXITCODE)" }
    }
}

Get-ChildItem $Out -Filter 'vkfillcheck-*.exe' | Sort-Object Name | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

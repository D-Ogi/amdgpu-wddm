# Builds vksemcheck for x64 and for x86 without a WDK or SDK installation: headers and import libraries
# come from the SDK NuGet packages unpacked under -Kits, the Vulkan headers from -VulkanInclude, the
# compiler from the installed Visual Studio. The flow of tools\win\vkfillcheck\build.ps1.
#
# Both architectures are built by default. The x64 program is the check of the system ICD; the x86 program
# runs against the 32-bit ICD, which in 0.7.216.100-tester.27 is the earlier build and is the control.
#
# The build ends with --help, a usage check, a self-test on any Vulkan device of this PC that offers
# VK_KHR_external_semaphore_win32 (skipped when there is none), and a SHA-256 of each artifact, the
# receipt a lab runner pins.
#
#   pwsh tools\win\vksemcheck\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget
#   pwsh tools\win\vksemcheck\build.ps1 -Kits ... -Arch x64        # only the 64-bit program

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\vksemcheck",
    [string]$KitVersion = '10.0.26100.0',
    [string]$VulkanInclude = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\m15\vkd3d\khronos\Vulkan-Headers\include",
    [ValidateSet('x64', 'x86', 'both')][string]$Arch = 'both',
    [switch]$SkipSelfTest
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
if (-not (Test-Path -LiteralPath (Join-Path $VulkanInclude 'vulkan\vulkan_win32.h'))) {
    throw "Vulkan headers not found: $VulkanInclude (expected vulkan\vulkan_win32.h)"
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
    $exe = Join-Path $Out "vksemcheck-$a.exe"

    # A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its
    # full hash.
    if (Test-Path -LiteralPath $exe) {
        $hash = (Get-FileHash -LiteralPath $exe).Hash
        $keep = Join-Path $Out "retained\vksemcheck-$a-$hash.exe"
        if (-not (Test-Path -LiteralPath $keep)) {
            New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
            Copy-Item -LiteralPath $exe -Destination $keep
        }
        Write-Host "  previous $a artifact retained as retained\vksemcheck-$a-$hash.exe"
    }

    Write-Host "  cl $a"
    $env:INCLUDE = ''; $env:LIB = ''
    & $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/TC', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS', '/DUNICODE',
        '/D_UNICODE', "/I$VulkanInclude",
        "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
        "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\vksemcheck-$a.obj", "/Fe$exe",
        (Join-Path $here 'vksemcheck.c'),
        '/link', "/LIBPATH:$(Join-Path $msvc.FullName "lib\$a")", "/LIBPATH:$sdkLib\ucrt\$a", "/LIBPATH:$sdkLib\um\$a",
        'kernel32.lib') |
        ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "    $_" } }
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $a ($LASTEXITCODE)" }
    $built += $exe
}

# Each artifact must be the architecture it was asked for: the x86 program is the control of the 32-bit ICD
# only when it really is a 32-bit image.
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
foreach ($exe in $built) {
    $want = if ($exe -match 'x86\.exe$') { '14C machine \(x86\)' } else { '8664 machine \(x64\)' }
    $hdr = & $dumpbin /nologo /headers $exe | Select-String 'machine \('
    if ("$hdr" -notmatch $want) { throw "$exe is not the expected image: $hdr" }
    Write-Host "  image $(Split-Path -Leaf $exe): $(($hdr -join ' ').Trim())"
}

foreach ($exe in $built) {
    & $exe --help 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "help check failed for $exe ($LASTEXITCODE)" }
    & $exe --bad-option 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "usage check failed for $exe ($LASTEXITCODE)" }
}

# The self-test runs the whole walk on the first Vulkan device of this PC. Exit 3 means that this PC offers
# no device with timeline semaphores and VK_KHR_external_semaphore_win32, which is not a reason to refuse
# the artifact. Any other failure stops the build: another vendor's driver passing every check is the
# positive control of the client itself.
if ($SkipSelfTest) {
    Write-Host '  self-test skipped on request'
} else {
    foreach ($exe in $built) {
        $name = Split-Path -Leaf $exe
        $log = Join-Path $env:TEMP "vksemcheck-selftest-$([IO.Path]::GetFileNameWithoutExtension($name)).log"
        Write-Host "  self-test $name : --any-device"
        $lines = & $exe --any-device --wait-ms 5000 --child-log $log
        $code = $LASTEXITCODE
        if ($code -eq 3) {
            Write-Host "  self-test skipped for ${name}: this PC offers it no usable device ($(($lines | Select-String 'FAIL' | Select-Object -First 1)))"
            continue
        }
        $lines | Where-Object { $_ -match '^(\[child\] )?(FAIL|INFO)|^vksemcheck ' } | ForEach-Object { Write-Host "    $_" }
        if ($code -ne 0) { throw "self-test failed for $name ($code)" }
    }
}

Get-ChildItem $Out -Filter 'vksemcheck-*.exe' | Sort-Object Name | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

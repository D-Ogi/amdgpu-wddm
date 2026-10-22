# Builds vkpresent for x64 and for x86 without a WDK or SDK installation: headers and import libraries come
# from the SDK NuGet packages unpacked under -Kits, the Vulkan headers from -VulkanInclude, the compiler from
# the installed Visual Studio. Same flow as tools\win\vkfillcheck\build.ps1.
#
# Both architectures are built by default, and here that is the point of the tool: the release registers a
# 32-bit Vulkan ICD for the WOW64 half of the machine (the WOW6432Node Khronos key and VulkanDriverNameWow),
# and only an x86 client enters that half. A gate that ran the x64 program alone would say nothing about it.
#
# The build ends with --help, a usage check, --no-present on each artifact (which creates no window, so it is
# safe on any PC, including this development PC) and a SHA-256 of each artifact, the receipt a lab runner
# pins. It never presents here: a window on the development PC needs explicit permission.
#
#   pwsh tools\win\vkpresent\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget
#   pwsh tools\win\vkpresent\build.ps1 -Kits ... -Arch x86        # only the 32-bit program

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\vkpresent",
    [string]$KitVersion = '10.0.26100.0',
    [string]$VulkanInclude = '',
    [ValidateSet('x64', 'x86', 'both')][string]$Arch = 'both',
    [switch]$SkipSelfTest
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# The workspace root, which holds the Vulkan headers and scratch\. BC250_ROOT when the session sets it,
# because a work tree of this repository is not always one level under the workspace.
$root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $here '..\..\..\..')).Path }
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'

# The Vulkan headers of this workspace, in the order the other clients look for them. Only the headers are
# needed: every entry point is taken with GetProcAddress, so no import library of the loader is linked.
if (-not $VulkanInclude) {
    foreach ($candidate in @(
            (Join-Path $root 'scratch\m15\vkd3d\khronos\Vulkan-Headers\include'),
            (Join-Path $root 'scratch\forksync\vkd3d-proton\khronos\Vulkan-Headers\include'),
            (Join-Path $root 'ref\Vulkan-Headers\include'))) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'vulkan\vulkan.h')) { $VulkanInclude = $candidate; break }
    }
}
if (-not $VulkanInclude -or -not (Test-Path -LiteralPath (Join-Path $VulkanInclude 'vulkan\vulkan.h'))) {
    # Join-Path with an empty string throws a binding error about a parameter nobody passed, which says nothing
    # about the missing headers, so the empty case is tested before it and names the three places looked in.
    throw ("Vulkan headers not found" + $(if ($VulkanInclude) { ": '$VulkanInclude' has no vulkan\vulkan.h" }
        else { " under '$root' (scratch\m15\vkd3d, scratch\forksync\vkd3d-proton, ref\Vulkan-Headers)" }) +
        ". Pass -VulkanInclude, or BC250_ROOT for the workspace that holds them.")
}
Write-Host "  vulkan headers $VulkanInclude"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$targets = if ($Arch -eq 'both') { @('x64', 'x86') } else { @($Arch) }
$built = @()
foreach ($a in $targets) {
    $cl = Join-Path $msvc.FullName "bin\Hostx64\$a\cl.exe"
    if (-not (Test-Path -LiteralPath $cl)) { throw "no $a compiler: $cl" }
    $sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$a\c"
    if (-not (Test-Path -LiteralPath $sdkLib)) { throw "no $a SDK libraries: $sdkLib" }
    $exe = Join-Path $Out "vkpresent-$a.exe"

    # A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its hash.
    if (Test-Path -LiteralPath $exe) {
        $hash = (Get-FileHash -LiteralPath $exe).Hash
        $keep = Join-Path $Out "retained\vkpresent-$a-$hash.exe"
        if (-not (Test-Path -LiteralPath $keep)) {
            New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
            Copy-Item -LiteralPath $exe -Destination $keep
        }
        Write-Host "  previous $a artifact retained as retained\vkpresent-$a-$hash.exe"
    }

    Write-Host "  cl $a"
    $env:INCLUDE = ''; $env:LIB = ''
    & $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/TC', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
        "/I$VulkanInclude",
        "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
        "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\vkpresent-$a.obj", "/Fe$exe",
        (Join-Path $here 'vkpresent.c'),
        '/link', "/LIBPATH:$(Join-Path $msvc.FullName "lib\$a")", "/LIBPATH:$sdkLib\ucrt\$a", "/LIBPATH:$sdkLib\um\$a",
        'kernel32.lib', 'user32.lib', 'advapi32.lib', 'psapi.lib') |
        ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "    $_" } }
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $a ($LASTEXITCODE)" }
    $built += $exe
}

# The image of each artifact must be the architecture it was asked for: a 64-bit program cannot test the x86
# ICD, and the whole point of building both is that each one tests its own half of the registration.
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
foreach ($exe in $built) {
    $want = if ($exe -match 'x86\.exe$') { '14C machine \(x86\)' } else { '8664 machine \(x64\)' }
    $hdr = & $dumpbin /nologo /headers $exe | Select-String 'machine \('
    if ("$hdr" -notmatch $want) { throw "$exe is not the expected image: $hdr" }
    Write-Host "  image $(Split-Path -Leaf $exe): $(($hdr -join ' ').Trim())"
}

# No module of the DXGI route may be a static import of this program: the ICD's module gate acts on what the
# process has loaded, and a static import would load the application-local copy before the ICD ever runs.
foreach ($exe in $built) {
    $imports = & $dumpbin /nologo /dependents $exe
    foreach ($bad in @('dxgi.dll', 'd3d12.dll', 'd3d12core.dll', 'dcomp.dll', 'vulkan-1.dll')) {
        if ($imports -match [regex]::Escape($bad)) { throw "$exe statically imports $bad" }
    }
}

foreach ($exe in $built) {
    & $exe --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "help check failed for $exe ($LASTEXITCODE)" }
    & $exe --bad-option 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "usage check failed for $exe ($LASTEXITCODE)" }
}

# The self-test enumerates only (--no-present): no window is created, so it is safe on the development PC and
# in a session-0 shell. A PC without a Vulkan driver for that architecture is not a reason to refuse the
# artifact, and a development PC has no 32-bit ICD of ours, so a failure here is reported and not thrown.
if ($SkipSelfTest) {
    Write-Host '  self-test skipped on request'
} else {
    foreach ($exe in $built) {
        $name = Split-Path -Leaf $exe
        & $exe --list-devices | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  self-test: ${name} finds no Vulkan device on this PC (exit $LASTEXITCODE)"
            continue
        }
        & $exe --no-present --any-device | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { throw "self-test failed for $name ($LASTEXITCODE)" }
    }
}

Get-ChildItem $Out -Filter 'vkpresent-*.exe' | Sort-Object Name | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

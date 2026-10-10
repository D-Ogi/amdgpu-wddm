param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [string]$VulkanInclude,
    [ValidateSet('shadowtest', 'presenttest', 'sharecell12', 'sharecellvk', 'both', 'sharecells', 'all')][string]$Harness = 'both',
    [switch]$SkipSelfTest
)
# Builds the WSI DXGI clients of this directory. All of them are console processes with no window and
# nothing resident. None of them touches the lab: they run wherever they are started.
#
#   shadowtest, presenttest   the two M16 WSI harnesses. The flags are the ones that produced the E56
#                             binaries, and the source hashes this prints are the ones E56 records.
#   sharecell12, sharecellvk  the two b27 cross-stack share cells: a D3D12 producer with a RADV
#                             consumer, and the reverse, over the handle types the Vulkan WSI DXGI
#                             route shares (a D3D12 texture imported as a VK D3D12_RESOURCE, and a
#                             RADV timeline opened by D3D12 as an ID3D12Fence). They need the Vulkan
#                             headers; -VulkanInclude says where they are.
#
# The binary hash depends on the compiler and on the SDK in use, so it is a property of the run and
# not of the source: each E56 manifest names the compiler of its own run.
#
#   pwsh tools\win\wsi-dxgi\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Harness sharecells
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
if (-not (Test-Path -LiteralPath (Join-Path $sdk "Include\$KitVersion\um\dcomp.h"))) {
    throw "SDK $KitVersion not found under $sdk"
}

# The workspace root: BC250_ROOT when it is set, else the grandparent of -Kits, which is
# <workspace>\toolchain\nuget. It is never guessed from this script's own place, because a build from a git
# worktree sits under <workspace>\scratch\... and the parent of the checkout is then not the workspace.
$root = if ($env:BC250_ROOT) { [IO.Path]::GetFullPath($env:BC250_ROOT) } else { (Resolve-Path (Join-Path $Kits '..\..')).Path }
if (-not $Out) { $Out = Join-Path $root 'scratch\build\wsi-dxgi' }
if (-not $VulkanInclude) { $VulkanInclude = Join-Path $root 'scratch\m15\vkd3d\khronos\Vulkan-Headers\include' }
New-Item -ItemType Directory -Force $Out | Out-Null
# Temporary files of the compiler stay in the workspace, never on the system drive.
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$env:INCLUDE = ''; $env:LIB = ''

$sharecells = @('sharecell12', 'sharecellvk')
$names = switch ($Harness) {
    'both' { @('shadowtest', 'presenttest') }
    'sharecells' { $sharecells }
    'all' { @('shadowtest', 'presenttest') + $sharecells }
    default { @($Harness) }
}
if (($names | Where-Object { $sharecells -contains $_ }) -and
    -not (Test-Path -LiteralPath (Join-Path $VulkanInclude 'vulkan\vulkan.h'))) {
    throw "Vulkan headers not found: $VulkanInclude (expected vulkan\vulkan.h)"
}

foreach ($name in $names) {
    $source = Join-Path $here "$name.cpp"
    $exe = Join-Path $Out "$name.exe"
    # A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its
    # full hash, the same rule as tools\win\vkfillcheck\build.ps1.
    if (Test-Path -LiteralPath $exe) {
        $hash = (Get-FileHash -LiteralPath $exe).Hash
        $keep = Join-Path $Out "retained\$name-$hash.exe"
        if (-not (Test-Path -LiteralPath $keep)) {
            New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
            Copy-Item -LiteralPath $exe -Destination $keep
        }
        Write-Host "  previous artifact retained as retained\$name-$hash.exe"
    }
    $clArgs = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MD', '/O2', '/DUNICODE', '/D_UNICODE',
        "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
        "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt")
    if ($sharecells -contains $name) { $clArgs += "/external:W0", "/external:I$VulkanInclude" }
    $clArgs += @("/Fo$Out\$name.obj", "/Fe$exe", $source, '/link',
        "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
        'psapi.lib', 'bcrypt.lib')
    & $cl @clArgs | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $name ($LASTEXITCODE)" }
    Get-Item $exe | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
    '{0,9}  {1}.cpp  sha256 {2}' -f (Get-Item $source).Length, $name, (Get-FileHash -LiteralPath $source).Hash
}

# The share cells carry their own host mode, which needs no device of either stack: the pattern, the
# comparison and the fence schedule. It runs here, with its negative control, so a client whose own rules
# were wrong cannot reach the lab and report a sharing failure that it caused itself. The GPU part is not
# run here: it belongs to a lab arm, where both stacks are ours.
if (-not $SkipSelfTest) {
    foreach ($name in ($names | Where-Object { $sharecells -contains $_ })) {
        $exe = Join-Path $Out "$name.exe"
        & $exe --help | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "help check failed for $name ($LASTEXITCODE)" }
        & $exe --bad-option 2>$null | Out-Null
        if ($LASTEXITCODE -ne 2) { throw "usage check failed for $name ($LASTEXITCODE)" }
        Write-Host "  host mode $name : --selftest"
        $selftestOutput = @(& $exe --selftest)
        $selftestOutput | Select-Object -Last 1 | ForEach-Object { Write-Host "    $_" }
        if ($selftestOutput -cmatch '^UNSAFE\b') { throw "selftest leaked UNSAFE marker for $name" }
        if ($LASTEXITCODE -ne 0) { throw "selftest failed for $name ($LASTEXITCODE)" }
        foreach ($origin in @('radv', 'd3d12')) {
            & $exe --selftest --fence-origin $origin | Select-Object -Last 1 | Write-Host
            if ($LASTEXITCODE -ne 0) { throw "origin $origin selftest failed for $name" }
        }
        & $exe --selftest --fence-origin invalid | Out-Null
        if ($LASTEXITCODE -ne 2) { throw "invalid origin was accepted by $name" }
        & $exe --selftest --fence-origin | Out-Null
        if ($LASTEXITCODE -ne 2) { throw "missing origin was accepted by $name" }
        Write-Host "  negative control $name : --selftest --negative-control (every case must fail)"
        & $exe --selftest --negative-control | Select-Object -Last 1 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 1) { throw "negative control did not fail as required for $name ($LASTEXITCODE)" }
    }
} else {
    Write-Host '  host mode skipped on request'
}

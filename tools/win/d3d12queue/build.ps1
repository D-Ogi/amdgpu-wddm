param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\amdgpu_wddm_d3d12_queue",
    [string]$KitVersion = '10.0.26100.0',
    # Diagnostic variant: value of RADV_EXPERIMENTAL the client sets in its own process.
    [ValidatePattern('^[a-z0-9_]*$')][string]$RadvExperimental = '',
    # Diagnostic variant: request feature level 12_1 instead of 11_0 at device creation.
    [switch]$FeatureLevel12_1,
    # Diagnostic variant: UPLOAD -> DEFAULT -> READBACK with a transition barrier in between.
    [switch]$DefaultHeap
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
$previous = Join-Path $Out 'amdgpu_wddm_d3d12_queue.exe'
if (Test-Path -LiteralPath $previous) {
    $hash = (Get-FileHash -LiteralPath $previous).Hash
    $keep = Join-Path $Out "retained\amdgpu_wddm_d3d12_queue-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $previous -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\amdgpu_wddm_d3d12_queue-$hash.exe"
}

$env:INCLUDE = ''; $env:LIB = ''
$variant = @(); if ($RadvExperimental) { $variant = @("/DINTERACTIVE_RADV_EXPERIMENTAL=$RadvExperimental") }
if ($FeatureLevel12_1) { $variant += '/DINTERACTIVE_FEATURE_LEVEL_12_1' }
if ($DefaultHeap) { $variant += '/DINTERACTIVE_DEFAULT_HEAP' }
& $cl @($variant + '/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/Fo$Out\amdgpu_wddm_d3d12_queue.obj",
    "/Fe$Out\amdgpu_wddm_d3d12_queue.exe", (Join-Path $here 'queue.cpp'), '/link',
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'dxgi.lib', 'user32.lib', 'psapi.lib', 'bcrypt.lib', 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

& "$Out\amdgpu_wddm_d3d12_queue.exe" --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
Get-Item "$Out\amdgpu_wddm_d3d12_queue.exe" | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

& "$Out\amdgpu_wddm_d3d12_queue.exe" --invalid
if ($LASTEXITCODE -ne 2) { throw "Invalid CLI accepted" }

# Pure command/selection checks do not enumerate adapters or call D3D.
foreach ($test in @('parser-test','interactive-test')) {
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/Fo$Out\$test.obj",
    "/Fe$Out\$test.exe", (Join-Path $here "$test.cpp"), '/link',
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'dxgi.lib', 'user32.lib', 'psapi.lib', 'bcrypt.lib', 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "$test build failed" }
& "$Out\$test.exe"
if ($LASTEXITCODE -ne 0) { throw "$test failed" }
}

# Immutable receipt/trace classification is part of the probe artifact gate.
python -B -m unittest discover -s $here -p 'test_*.py'
if ($LASTEXITCODE -ne 0) { throw 'interactive planner/summary tests failed' }

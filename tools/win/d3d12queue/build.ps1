param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\amdgpu_wddm_d3d12_queue",
    [string]$KitVersion = '10.0.26100.0',
    # Diagnostic variant: value of RADV_EXPERIMENTAL the client sets in its own process.
    [ValidatePattern('^[a-z0-9_]*$')][string]$RadvExperimental = '',
    # Diagnostic variant: request feature level 12_1 instead of 11_0 at device creation.
    [switch]$FeatureLevel12_1,
    # Diagnostic variant: UPLOAD -> DEFAULT -> READBACK with a transition barrier in between.
    [switch]$DefaultHeap,
    # Diagnostic variant: the copy verb draws one triangle to an offscreen target and compares every word.
    [switch]$Draw,
    # Diagnostic variant: the copy verb draws four indexed triangles with a depth buffer and a source texture
    # bound through a descriptor table, and compares every word.
    [switch]$Scene,
    # Diagnostic variant: the copy verb opens a window, creates a flip-model swap chain on the queue, presents
    # two cleared frames and resizes the chain. It opens a window: run it on the lab, not unannounced elsewhere.
    [switch]$Present,
    # Diagnostic variant: the copy verb maps a reserved buffer's tiles to a heap on the queue and sends a
    # pattern through it. The device must report tiled resources (see -RadvExperimental).
    [switch]$Sparse,
    # Diagnostic variant: the copy verb builds a bottom and a top level acceleration structure on the queue and
    # compares the 64 words of an inline ray query program. The device must report raytracing tier 1.1.
    [switch]$RayQuery,
    # Diagnostic variant: the copy verb builds the same scene and traces it with DispatchRays through a raytracing
    # pipeline state object (raygen, miss and closest hit shaders), comparing the same 64 words.
    [switch]$RayPipeline,
    # Diagnostic variant: the copy verb creates and releases the state object of -RayPipeline twice, with no
    # acceleration structure, command list or submission.
    [switch]$RayState,
    # Diagnostic variant: the copy verb traces the scene of -RayPipeline through a pipeline grown by AddToStateObject
    # (a local root constant in the hit group record, a second miss shader from the addition).
    [switch]$RayGrow,
    # Diagnostic variant: the copy verb traces the scene of -RayPipeline through a pipeline made of a collection.
    [switch]$RayCollection,
    # Diagnostic variant: the copy verb runs a game-like load in steps of 256 MB to 2 GB of committed resources,
    # written and sampled back through the queue, then four threads on the smallest step; milestones.log is
    # written through to the disk.
    [switch]$GameLoad,
    # Arms of -GameLoad: SMALL (64 KB buffers), LARGE (64 MB buffers and textures) or both, SMALL first.
    [ValidateSet('Small', 'Large', 'Both')][string]$GameLoadArm = 'Both'
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
if (@($Draw, $Scene, $Present, $Sparse, $RayQuery, $RayPipeline, $RayState, $RayGrow, $RayCollection, $GameLoad | Where-Object { $_ }).Count -gt 1) {
    throw 'Draw, Scene, Present, Sparse, RayQuery, RayPipeline, RayState, RayGrow, RayCollection and GameLoad each replace the copy verb; choose one' }
if ($GameLoadArm -ne 'Both' -and -not $GameLoad) { throw 'GameLoadArm needs GameLoad' }
if ($Draw) { $variant += '/DINTERACTIVE_DRAW' }
if ($Scene) { $variant += '/DINTERACTIVE_SCENE' }
if ($Present) { $variant += '/DINTERACTIVE_PRESENT' }
if ($Sparse) { $variant += '/DINTERACTIVE_SPARSE' }
if ($RayQuery) { $variant += '/DINTERACTIVE_RAYQUERY' }
if ($RayPipeline) { $variant += '/DINTERACTIVE_RAYPIPELINE' }
if ($RayState) { $variant += '/DINTERACTIVE_RAYSTATE' }
if ($RayGrow) { $variant += '/DINTERACTIVE_RAYGROW' }
if ($RayCollection) { $variant += '/DINTERACTIVE_RAYCOLLECTION' }
if ($GameLoad) { $variant += '/DINTERACTIVE_GAMELOAD', "/DINTERACTIVE_GAMELOAD_ARMS=$(@{Small = 1; Large = 2; Both = 3}[$GameLoadArm])" }
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

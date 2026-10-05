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
    # Diagnostic variant: the copy verb opens a borderless window over the whole output, creates a flip-model
    # swap chain at the output's size on the queue, and presents -FlipFrames frames of exact content, comparing
    # the back buffer before each Present and reporting DXGI's present statistics. M15.14's scan-out candidate.
    # It opens a fullscreen window and takes the foreground: run it on the lab, not unannounced elsewhere.
    [switch]$Flip,
    # -Flip: back buffers in the chain.
    [ValidateRange(2, 3)][int]$FlipBuffers = 3,
    # -Flip: frames presented, as many as the session's deadline allows.
    [ValidateRange(1, 1200)][int]$FlipFrames = 120,
    # -Flip: ask DXGI for exclusive fullscreen on the output instead of a borderless window. A refusal is traced
    # and the run goes on borderless.
    [switch]$FlipFullscreen,
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
    [ValidateSet('Small', 'Large', 'Both')][string]$GameLoadArm = 'Both',
    # Test build of -GameLoad only: the render thread keeps tracing this long after its last frame, past the
    # 10 s join bound, so that the session's end with a detached thread can be exercised (0 = off).
    [ValidateRange(0, 30000)][int]$GameLoadRenderHoldMs = 0,
    # -GameLoad only: how many single-thread steps (256 MB, 512 MB, 1 GB, 2 GB) run before the four-thread phase;
    # 1 leaves the threads their time on a fast driver (169 finished three steps and never reached them).
    [ValidateRange(0, 4)][int]$GameLoadSingleSteps = 4,
    # Test build with -GameLoadRenderHoldMs only: throw right after the render thread is detached.
    [switch]$GameLoadThrowAfterDetach,
    # Diagnostic variant: the copy verb runs recording threads that reset their allocators and lists every batch,
    # record draws with root constants and root CBVs, and replace their allocators in turn; the main thread executes
    # each batch with a fence round trip and compares every constant the draws read back. No window.
    [switch]$ResetChurn,
    # -ResetChurn: recording threads, each with its own list and allocator.
    [ValidateRange(1, 8)][int]$ResetChurnThreads = 4,
    # -ResetChurn: most batches (one list per thread each).
    [ValidateRange(1, 100000)][int]$ResetChurnBatches = 2000,
    # -ResetChurn: batches start until this many seconds after the copy verb began, never in the last 10 s before the
    # client's deadline. 20 fits the lab runner as it is (client deadline 70 s, Drive 65 s).
    [ValidateRange(1, 140)][int]$ResetChurnSeconds = 20,
    # -ResetChurn: draws per list.
    [ValidateRange(16, 4096)][int]$ResetChurnDraws = 1024,
    # -ResetChurn: lists an allocator records before a new one replaces it; 0 keeps one allocator per thread.
    [ValidateRange(0, 64)][int]$ResetChurnRenew = 2,
    # Measurement variant: the copy verb records frames of many small draws with state changes on several lists,
    # executes them in several ExecuteCommandLists calls, compares every word the draws wrote, and reports the
    # recording thread's CPU time per frame (QueryThreadCycleTime) in three phases: burst, interleaved with stand-in
    # application work, and one recording thread per list. For the A/B of the UMD's deferred-replay experiment: one
    # binary, the arm set by the trial's AMDGPU_WDDM_D3D12_EXPERIMENT. No window.
    [switch]$RecordBench,
    # -RecordBench: command lists per frame, each recorded by its own thread in the threaded phase.
    [ValidateRange(1, 8)][int]$RecordBenchLists = 4,
    # -RecordBench: draws per list, a multiple of 16.
    [ValidateRange(64, 4096)][int]$RecordBenchDraws = 512,
    # -RecordBench: ExecuteCommandLists calls per frame, the lists split evenly between them (at most the lists).
    [ValidateRange(1, 8)][int]$RecordBenchExecutes = 2,
    # -RecordBench: microseconds of stand-in application work after each group of 16 draws in the interleaved and
    # threaded phases.
    [ValidateRange(0, 1000)][int]$RecordBenchWorkUs = 64,
    # -RecordBench: the three phases share this many seconds from the copy verb's start, never the last 10 s before
    # the client's deadline. 20 fits the lab runner as it is (client deadline 70 s, Drive 65 s).
    [ValidateRange(3, 140)][int]$RecordBenchSeconds = 20,
    # Back buffer format of -Present: B8G8R8A8_UNORM, or R10G10B10A2_UNORM as a 10-bit swap chain composed on
    # the desktop whatever the monitor's depth.
    [ValidateSet('Bgra8', 'Rgb10a2')][string]$PresentFormat = 'Bgra8'
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
if (@($Draw, $Scene, $Present, $Flip, $Sparse, $RayQuery, $RayPipeline, $RayState, $RayGrow, $RayCollection, $GameLoad, $ResetChurn, $RecordBench | Where-Object { $_ }).Count -gt 1) {
    throw 'Draw, Scene, Present, Flip, Sparse, RayQuery, RayPipeline, RayState, RayGrow, RayCollection, GameLoad, ResetChurn and RecordBench each replace the copy verb; choose one' }
foreach ($name in 'ResetChurnThreads', 'ResetChurnBatches', 'ResetChurnSeconds', 'ResetChurnDraws', 'ResetChurnRenew') {
    if ($PSBoundParameters.ContainsKey($name) -and -not $ResetChurn) { throw "$name needs ResetChurn" } }
foreach ($name in 'RecordBenchLists', 'RecordBenchDraws', 'RecordBenchExecutes', 'RecordBenchWorkUs', 'RecordBenchSeconds') {
    if ($PSBoundParameters.ContainsKey($name) -and -not $RecordBench) { throw "$name needs RecordBench" } }
if ($RecordBenchDraws % 16) { throw 'RecordBenchDraws must be a multiple of 16' }
if ($RecordBenchExecutes -gt $RecordBenchLists) { throw 'RecordBenchExecutes must not exceed RecordBenchLists' }
if ($GameLoadArm -ne 'Both' -and -not $GameLoad) { throw 'GameLoadArm needs GameLoad' }
if ($GameLoadRenderHoldMs -and -not $GameLoad) { throw 'GameLoadRenderHoldMs needs GameLoad' }
if ($GameLoadRenderHoldMs) { $variant += "/DINTERACTIVE_GAMELOAD_RENDER_HOLD_MS=$GameLoadRenderHoldMs" }
if ($GameLoadThrowAfterDetach -and -not $GameLoadRenderHoldMs) { throw 'GameLoadThrowAfterDetach needs GameLoadRenderHoldMs' }
if ($GameLoadThrowAfterDetach) { $variant += '/DINTERACTIVE_GAMELOAD_THROW_AFTER_DETACH' }
if ($PresentFormat -ne 'Bgra8' -and -not $Present) { throw 'PresentFormat needs Present' }
foreach ($name in 'FlipBuffers', 'FlipFrames', 'FlipFullscreen') {
    if ($PSBoundParameters.ContainsKey($name) -and -not $Flip) { throw "$name needs Flip" } }
if ($PresentFormat -eq 'Rgb10a2') { $variant += '/DINTERACTIVE_PRESENT_RGB10A2' }
if ($Draw) { $variant += '/DINTERACTIVE_DRAW' }
if ($Scene) { $variant += '/DINTERACTIVE_SCENE' }
if ($Present) { $variant += '/DINTERACTIVE_PRESENT' }
if ($Flip) { $variant += '/DINTERACTIVE_FLIP', "/DINTERACTIVE_FLIP_BUFFERS=$FlipBuffers", "/DINTERACTIVE_FLIP_FRAMES=$FlipFrames" }
if ($FlipFullscreen) { $variant += '/DINTERACTIVE_FLIP_FULLSCREEN' }
if ($Sparse) { $variant += '/DINTERACTIVE_SPARSE' }
if ($RayQuery) { $variant += '/DINTERACTIVE_RAYQUERY' }
if ($RayPipeline) { $variant += '/DINTERACTIVE_RAYPIPELINE' }
if ($RayState) { $variant += '/DINTERACTIVE_RAYSTATE' }
if ($RayGrow) { $variant += '/DINTERACTIVE_RAYGROW' }
if ($RayCollection) { $variant += '/DINTERACTIVE_RAYCOLLECTION' }
if ($GameLoad) { $variant += '/DINTERACTIVE_GAMELOAD', "/DINTERACTIVE_GAMELOAD_ARMS=$(@{Small = 1; Large = 2; Both = 3}[$GameLoadArm])" }
if ($GameLoadSingleSteps -ne 4 -and -not $GameLoad) { throw 'GameLoadSingleSteps needs GameLoad' }
if ($GameLoad) { $variant += "/DINTERACTIVE_GAMELOAD_SINGLE_STEPS=$GameLoadSingleSteps" }
# resetchurn-test checks the oracle with the same values as the client; without -ResetChurn, with the defaults.
$churn = @()
if ($ResetChurn) { $churn = @("/DINTERACTIVE_RESETCHURN_THREADS=$ResetChurnThreads",
    "/DINTERACTIVE_RESETCHURN_BATCHES=$ResetChurnBatches", "/DINTERACTIVE_RESETCHURN_SECONDS=$ResetChurnSeconds",
    "/DINTERACTIVE_RESETCHURN_DRAWS=$ResetChurnDraws", "/DINTERACTIVE_RESETCHURN_RENEW=$ResetChurnRenew")
    $variant += @('/DINTERACTIVE_RESETCHURN') + $churn }
# recordbench-test likewise.
$bench = @()
if ($RecordBench) { $bench = @("/DINTERACTIVE_RECORDBENCH_LISTS=$RecordBenchLists",
    "/DINTERACTIVE_RECORDBENCH_DRAWS=$RecordBenchDraws", "/DINTERACTIVE_RECORDBENCH_EXECUTES=$RecordBenchExecutes",
    "/DINTERACTIVE_RECORDBENCH_WORK_US=$RecordBenchWorkUs", "/DINTERACTIVE_RECORDBENCH_SECONDS=$RecordBenchSeconds")
    $variant += @('/DINTERACTIVE_RECORDBENCH') + $bench }
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
foreach ($test in @('parser-test','interactive-test','resetchurn-test','recordbench-test')) {
& $cl @(@(if ($test -eq 'resetchurn-test') { $churn } elseif ($test -eq 'recordbench-test') { $bench }) + '/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE',
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

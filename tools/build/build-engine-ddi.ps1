# Builds and checks driver/umd/d3d12/engine-ddi on the development PC. Nothing here touches unit A.
#
#   0. Engine ABI pin: bc250_vkd3d_engine.h is included by path from the vkd3d-proton fork checkout (-EngineSource,
#      libs/ddi), and its SHA-256 must equal header_sha256 in engine-ddi/engine-abi.json. The Vulkan headers come from
#      the same checkout (khronos/Vulkan-Headers/include) unless -VulkanInclude says otherwise.
#   1. Header gate: engine-ddi-header-test.cpp (static layout checks of the boundary), built and run.
#   2. Native library engine-ddi.lib, as the shell's DLL links it: no harness macro, /analyze. Its symbols must contain
#      no harness_ entry point; native-policy-test.exe must see EnginePrivateTest refused and both tables filled, and
#      caps-test.exe must pass the GetCaps checks against its stub engine.
#   With -NativeOnly the script stops here, and it needs no engine DLL and no GPU.
#   3. Harness library (AMDGPU_WDDM_ENGINE_DDI_HARNESS) and engine-ddi-harness.exe, which plays the runtime and the
#      shell against the real engine DLL on this PC's GPU (Vulkan loader, first hardware adapter unless -Adapter).
#   4. Runs, unless -NoRun: the engine DLL's SHA-256 must equal engine_dll_sha256 in engine-abi.json; caps-test.exe
#      --engine (query_adapter_caps on the real engine), then the harness; with -Vvl a second harness run under the
#      Khronos validation layer (toolchain\vvl) with synchronization validation, which must report no message and
#      must show the loader line that inserts the layer (the witness that "no message" is not "no layer").
# Any failed step fails the script. Output goes to <workspace>\scratch\build\d3d12-engine-ddi unless -OutputDir.
param(
    [string]$OutputDir,
    [string]$VsInstall,
    # The vkd3d-proton fork checkout at the commit engine-abi.json names (read only; nothing is built there).
    [string]$EngineSource,
    # Vulkan-Headers include directory (vulkan/vulkan_core.h); default: the one in -EngineSource.
    [string]$VulkanInclude,
    # The engine DLL the runs load; default: the pinned build under scratch\m15.
    [string]$EngineDll,
    # Substring of the DXGI adapter description to run on.
    [string]$Adapter,
    [switch]$Vvl,
    # Build only.
    [switch]$NoRun,
    # Steps 0 to 2 only: the static library the shell links, and its host tests.
    [switch]$NativeOnly
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root = Get-Bc250Root $repo
$src = Join-Path $repo 'driver\umd\d3d12\engine-ddi'
$pin = Get-Content -Raw -LiteralPath (Join-Path $src 'engine-abi.json') | ConvertFrom-Json
if (-not $OutputDir) { $OutputDir = Join-Path $root 'scratch\build\d3d12-engine-ddi' }
if (-not $EngineSource) { $EngineSource = Join-Path $root 'scratch\m15\vkd3d-1.2-src' }
if (-not $VulkanInclude) { $VulkanInclude = Join-Path $EngineSource ($pin.vulkan_headers_path -replace '/', '\') }
if (-not $EngineDll) { $EngineDll = Join-Path $root "scratch\m15\engine-1.2-$($pin.commit.Substring(0, 8))\$($pin.engine_dll)" }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)

# Step 0: the pinned engine header, included by path.
$engineHeader = Join-Path $EngineSource ($pin.header_path -replace '/', '\')
if (-not (Test-Path -LiteralPath $engineHeader)) { throw "engine header $engineHeader does not exist (-EngineSource)" }
$headerSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $engineHeader).Hash
if ($headerSha -ne $pin.header_sha256) {
    throw "engine header $engineHeader has SHA-256 $headerSha; engine-abi.json pins $($pin.header_sha256) (ABI $($pin.abi_version) $($pin.header_revision), commit $($pin.commit))"
}
$EngineInclude = Split-Path -Parent $engineHeader
if (-not (Test-Path -LiteralPath (Join-Path $VulkanInclude 'vulkan\vulkan_core.h'))) { throw "$VulkanInclude has no vulkan\vulkan_core.h" }
Write-Host "engine ABI $($pin.abi_version) $($pin.header_revision): $engineHeader (SHA-256 $headerSha)"

$libSources = @('context.cpp', 'caps.cpp', 'queue.cpp', 'commands.cpp', 'resources.cpp', 'descriptors.cpp',
                'root-signature.cpp', 'pipelines.cpp', 'queries.cpp') | ForEach-Object { Join-Path $src $_ }
$harnessSources = @(Get-ChildItem -LiteralPath (Join-Path $src 'tests') -Filter '*.cpp' |
    Where-Object { $_.Name -like 'harness*.cpp' -or $_.Name -like 'test-*.cpp' } | ForEach-Object FullName)

New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved = Save-ProcessEnvironment
try {
    $env:TEMP = $OutputDir; $env:TMP = $OutputDir
    $null = Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir
    $wdk = Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
    $flags = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/external:W0', '/MT', '/DNOMINMAX', '/Zi',
               "/external:I$wdk", "/external:I$wdk\..\shared", "/external:I$VulkanInclude", "/external:I$EngineInclude",
               "/I$src")
    $harnessFlag = '/DAMDGPU_WDDM_ENGINE_DDI_HARNESS'

    # A native step's verdict is its exit code. Windows PowerShell 5.1 turns every stderr line of a native command
    # under 2>&1 into an ErrorRecord, and the script-wide 'Stop' made the first one terminating (NativeCommandError)
    # although the step had passed. 'Continue' is local to this function and the step's scriptblock. Native stderr
    # lines (a RemoteException, in 5.1 and pwsh alike) are printed as text in order with stdout; any other error
    # record, such as a command that does not exist, still fails the step.
    function Invoke-Step([string]$Name, [scriptblock]$Body) {
        Write-Host "== $Name"
        $ErrorActionPreference = 'Continue'
        $global:LASTEXITCODE = 0
        & $Body 2>&1 | ForEach-Object {
            if ($_ -isnot [Management.Automation.ErrorRecord]) { $_ }
            elseif ($_.Exception -is [Management.Automation.RemoteException]) { "$_" }
            else { throw "$Name failed: $_" }
        } | Out-Host
        $code = $global:LASTEXITCODE
        if ($code) { throw "$Name failed ($code)" }
    }

    Push-Location $OutputDir
    try {
        foreach ($d in @('native', 'harness')) { New-Item -ItemType Directory -Force (Join-Path $OutputDir $d) | Out-Null }

        Invoke-Step 'header gate build' { & cl.exe @flags /Fo:native\ /Fd:native\ /Fe:engine-ddi-header-test.exe (Join-Path $src 'engine-ddi-header-test.cpp') }
        Invoke-Step 'header gate' { & .\engine-ddi-header-test.exe }

        # Native build. /analyze runs with the same /WX, so a finding fails the build.
        Invoke-Step 'native library (/analyze)' { & cl.exe @flags /analyze /analyze:external- /c /Fo:native\ /Fd:native\engine-ddi.pdb @libSources }
        $nativeObjs = @($libSources | ForEach-Object { Join-Path 'native' ([IO.Path]::ChangeExtension((Split-Path -Leaf $_), '.obj')) })
        Invoke-Step 'native library link' { & lib.exe /nologo /OUT:engine-ddi.lib @nativeObjs }
        $symbols = & dumpbin.exe /nologo /symbols engine-ddi.lib
        if ($LASTEXITCODE) { throw 'dumpbin failed' }
        $harnessSymbols = @($symbols | Select-String -Pattern 'harness_' -SimpleMatch)
        if ($harnessSymbols.Count) { throw "native engine-ddi.lib contains harness symbols: $($harnessSymbols[0].Line)" }
        Write-Host 'native engine-ddi.lib: no harness_ symbols'
        Invoke-Step 'native policy test build' { & cl.exe @flags /Fo:native\ /Fd:native\ /Fe:native-policy-test.exe (Join-Path $src 'tests\native-policy-test.cpp') engine-ddi.lib }
        Invoke-Step 'native policy test' { & .\native-policy-test.exe }
        Invoke-Step 'caps test build' { & cl.exe @flags /Fo:native\ /Fd:native\ /Fe:caps-test.exe (Join-Path $src 'tests\caps-test.cpp') engine-ddi.lib dxgi.lib }
        Invoke-Step 'caps test (stub engine)' { & .\caps-test.exe }

        if (-not $NativeOnly) {
            Invoke-Step 'harness library' { & cl.exe @flags $harnessFlag /c /Fo:harness\ /Fd:harness\engine-ddi.pdb @libSources }
            $harnessObjs = @($libSources | ForEach-Object { Join-Path 'harness' ([IO.Path]::ChangeExtension((Split-Path -Leaf $_), '.obj')) })
            Invoke-Step 'harness library link' { & lib.exe /nologo /OUT:engine-ddi-harness.lib @harnessObjs }
            if (-not $harnessSources.Count) { throw 'no harness sources in engine-ddi\tests' }
            Invoke-Step 'harness executable' { & cl.exe @flags $harnessFlag /Fo:harness\ /Fd:harness\ /Fe:engine-ddi-harness.exe @harnessSources engine-ddi-harness.lib dxgi.lib }
        }
    } finally { Pop-Location }

    if ($NativeOnly) { Write-Host "native library and host tests passed (-NativeOnly): $(Join-Path $OutputDir 'engine-ddi.lib')"; return }
    if ($NoRun) { Write-Host 'build only (-NoRun)'; return }
    if (-not (Test-Path -LiteralPath $EngineDll)) { throw "engine DLL $EngineDll does not exist" }
    $engineSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $EngineDll).Hash
    if ($engineSha -ne $pin.engine_dll_sha256) {
        throw "engine DLL $EngineDll has SHA-256 $engineSha; engine-abi.json pins $($pin.engine_dll_sha256)"
    }

    function Invoke-Run([string]$Tag, [string]$Exe, [bool]$WithVvl) {
        $runDir = Join-Path $OutputDir "run-$Tag"
        if (Test-Path -LiteralPath $runDir) { Remove-Item -LiteralPath $runDir -Recurse -Force }
        New-Item -ItemType Directory -Force $runDir | Out-Null
        $envSaved = Save-ProcessEnvironment
        try {
            $env:VKD3D_DEBUG = 'warn'
            if ($WithVvl) {
                $env:VK_LAYER_PATH = Join-Path $root 'toolchain\vvl\bin'
                $env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
                $env:VK_LOADER_LAYERS_ENABLE = '*validation'
                $env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC = 'true'
                $env:VK_KHRONOS_VALIDATION_REPORT_FLAGS = 'error,warn,perf'
                $env:VK_LOADER_DEBUG = 'layer'
            }
            $argList = @('--engine', "`"$EngineDll`"")
            if ($Adapter) { $argList += '--adapter', "`"$Adapter`"" }
            $p = Start-Process -FilePath (Join-Path $OutputDir $Exe) -ArgumentList $argList `
                -WorkingDirectory $runDir -NoNewWindow -PassThru `
                -RedirectStandardOutput "$runDir\stdout.txt" -RedirectStandardError "$runDir\stderr.txt"
            # Windows PowerShell 5.1: unless the process handle is opened while the process runs, ExitCode reads
            # $null after it exits, and a failed run would pass. A missing exit code fails the run below.
            $null = $p.Handle
            $timedOut = -not $p.WaitForExit(170000)
            if ($timedOut) { $p.Kill() }
            $p.WaitForExit()
            $exitCode = $p.ExitCode
        } finally { Restore-ProcessEnvironment $envSaved }
        $stdout = @(Get-Content "$runDir\stdout.txt")
        $stderr = @(Get-Content "$runDir\stderr.txt")
        $validation = @(($stdout + $stderr) | Select-String -Pattern '^Validation (Error|Warning|Performance)')
        $insert = @(($stderr + $stdout) | Select-String -Pattern 'Insert(ed)? instance layer.*VK_LAYER_KHRONOS_validation' |
            ForEach-Object Line | Select-Object -First 1)
        $summary = [ordered]@{
            utc               = [DateTime]::UtcNow.ToString('o')
            host              = 'development PC (not unit A)'
            executable        = $Exe
            vvl               = $WithVvl
            vvl_insert_line   = if ($WithVvl -and $insert.Count) { $insert[0] } else { $null }
            timed_out         = $timedOut
            exit_code         = $exitCode
            result            = ($stdout | Select-String -Pattern '^(PASSED|FAILED)' | Select-Object -Last 1).Line
            fail_lines        = @($stdout | Select-String -Pattern '^FAIL' | ForEach-Object Line)
            ok_count          = @($stdout | Select-String -Pattern '^ok  ' -CaseSensitive).Count
            skip_lines        = @($stdout | Select-String -Pattern '^SKIP' | ForEach-Object Line)
            validation_count  = $validation.Count
            validation_first  = @($validation | Select-Object -First 5 | ForEach-Object Line)
            adapter_line      = ($stdout | Select-String -Pattern '^adapter:' | Select-Object -First 1).Line
            engine_abi        = "$($pin.abi_version) $($pin.header_revision), commit $($pin.commit)"
            engine_sha256     = $engineSha
            executable_sha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $OutputDir $Exe)).Hash
        }
        $summary | ConvertTo-Json | Set-Content (Join-Path $runDir 'summary.json')
        Get-Content (Join-Path $runDir 'summary.json') | Write-Host
        if ($timedOut) { throw "$Exe ($Tag) timed out" }
        if ($null -eq $exitCode) { throw "$Exe ($Tag): exit code unavailable" }
        if ($exitCode -ne 0) { throw "$Exe ($Tag) failed with exit code $exitCode" }
        if ($summary.result -ne 'PASSED') { throw "$Exe ($Tag) exited 0 without its PASSED line" }
        if ($WithVvl) {
            if (-not $insert.Count) { throw 'VVL run without the loader line that inserts VK_LAYER_KHRONOS_validation' }
            if ($validation.Count) { throw "VVL run reported $($validation.Count) validation messages" }
        }
    }

    Invoke-Run 'caps' 'caps-test.exe' $false
    Invoke-Run 'plain' 'engine-ddi-harness.exe' $false
    if ($Vvl) { Invoke-Run 'vvl' 'engine-ddi-harness.exe' $true }
} finally { Restore-ProcessEnvironment $saved }
Write-Host 'engine-ddi: build and checks passed (development PC only)'

# Build Ollama for this driver: the processor base, the Vulkan backend and the HIP backend for gfx1013.
#
# The build has five stages. Each one can run alone, and `all` runs them in order:
#
#   prepare   check the toolchains, apply the patches, record every input
#   base      llama-server, ggml-base, ggml-cpu*  ->  <Staging>/lib/ollama
#   vulkan    ggml-vulkan                         ->  <Staging>/lib/ollama/vulkan
#   hip       ggml-hip for gfx1013 and our runtime->  <Staging>/lib/ollama/rocm_bc250
#   go        ollama.exe                          ->  <Staging>/ollama.exe
#
# Ollama loads a GGML backend from a directory under lib/ollama, so each backend is its own CMake
# project over one pinned llama.cpp source. Every stage uses the same compiler and the same ABI
# target as our llama.cpp release build, so the backends and the base agree.
#
# This script never runs a produced binary. host-checks.ps1 does that, and package-ollama.ps1
# makes the zip.
[CmdletBinding()]
param(
    [ValidateSet('prepare', 'base', 'vulkan', 'hip', 'go', 'all')][string]$Stage = 'all',
    [string]$Source,
    # The upstream commit this recipe is written against. prepare clones it when $Source is absent and
    # refuses a checkout that is at another commit: the version string comes from version/version.go and
    # would not change, so an unpinned source would produce other bytes under the same name and the
    # installer would then refuse the component against its own SHA-256 pin.
    [string]$Commit = 'eab97e9f92b9a25c2d52d2cc6c1b1c99bd9fae21',
    [string]$Upstream = 'https://github.com/ollama/ollama.git',
    [string]$Work,
    [string]$Staging,
    [string]$RocmRoot = 'P:/BC-250/scratch/bd117/consumer/rocm-root',
    [string]$CMakeExe = 'C:/msys64/mingw64/bin/cmake.exe',
    [string]$NinjaExe = 'C:/msys64/mingw64/bin/ninja.exe',
    [string]$HipPatchDir,
    [string]$Train = '27',
    [ValidateRange(1, 8)][int]$Jobs = 4,
    [bool]$CpuAllVariants = $true
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workspace = 'P:/BC-250'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path.Replace('\', '/')
if (-not $Source) { $Source = "$workspace/scratch/llm-components/ollama-src/ollama" }
if (-not $Work) { $Work = "$workspace/scratch/llm-components/build" }
if (-not $Staging) { $Staging = "$workspace/scratch/llm-components/staging/ollama" }
if (-not $HipPatchDir) { $HipPatchDir = "$repoRoot/compute/hip/patches" }
$Source = $Source.Replace('\', '/')
$Work = $Work.Replace('\', '/')
$Staging = $Staging.Replace('\', '/')
$RocmRoot = $RocmRoot.Replace('\', '/')

# The toolchain. One compiler for every stage: AMD's clang, targeting the MSVC ABI, which is the
# compiler our llama.cpp HIP asset uses. The Windows SDK and the MSVC headers come from the
# installed Visual Studio, the way scratch/llama-asset-b29 does it.
$clangBin = "$workspace/toolchain/llvm-amdgpu-22.1.8/mingw64/bin"
$cmake = $CMakeExe.Replace('\', '/')
$ninja = $NinjaExe.Replace('\', '/')
$goExe = "$workspace/toolchain/go-1.27.2/go/bin/go.exe"
$vulkanDir = "$workspace/toolchain/vk-build/Library"

function Write-Step([string]$text) { Write-Host "== $text" }

function Assert-Tool([string]$path, [string]$what) {
    if (-not (Test-Path $path)) { throw "$what is missing: $path" }
}

# A native command whose failure is an answer, not a fault. PowerShell turns anything a native
# command writes to stderr into an error record, and with Stop set that ends the script, so a
# probe such as `git apply --reverse --check` needs its own error preference.
function Invoke-Probe([scriptblock]$command) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $command 2>&1 | Out-Null
        return $LASTEXITCODE
    } finally { $ErrorActionPreference = $previous }
}

function Enter-BuildEnvironment {
    # vcvars64 gives INCLUDE, LIB and the SDK locations. clang reads them like cl does.
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    Assert-Tool $vswhere 'vswhere'
    $vs = & $vswhere -latest -products * -property installationPath
    if (-not $vs) { throw 'no Visual Studio installation found for the Windows SDK headers' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    Assert-Tool $vcvars 'vcvars64.bat'
    & cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^(INCLUDE|LIB|LIBPATH|UCRTVersion|WindowsSdkDir|WindowsSDKVersion|VCToolsInstallDir|VSINSTALLDIR)=(.*)$') {
            Set-Item "env:$($Matches[1])" $Matches[2]
        }
        # msys64 goes last, so its own clang and gcc never shadow the compiler of this build.
        if ($_ -match '^Path=(.*)$') { $env:PATH = "$clangBin;$vulkanDir/bin;$($Matches[1]);C:/msys64/mingw64/bin" }
    }
    $env:TEMP = "$Work/tmp"; $env:TMP = $env:TEMP
    $env:PYTHONDONTWRITEBYTECODE = '1'
    New-Item -ItemType Directory -Force $env:TEMP | Out-Null
}

function Get-OllamaVersion {
    $file = Join-Path $Source 'version/version.go'
    $text = if (Test-Path $file) { Get-Content $file -Raw } else { '' }
    $base = if ($text -match 'Version\s*=\s*"([^"]+)"') { $Matches[1] } else { '0.0.0' }
    if ($base -eq '0.0.0') {
        # A checkout between releases carries 0.0.0 in the source. Name it by the newest tag the
        # checkout contains, which is what the describe in inputs.json records.
        $described = [string](& git -C $Source describe --tags --always)
        if ($described -match '^v?(\d+\.\d+\.\d+)') { $base = $Matches[1] }
    }
    "$base-bc250.$Train"
}

# ---------------------------------------------------------------- prepare

function Invoke-Prepare {
    Write-Step 'prepare'
    Assert-Tool $cmake 'cmake'
    Assert-Tool $ninja 'ninja'
    Assert-Tool "$clangBin/clang.exe" 'AMD clang'
    Assert-Tool $goExe 'the portable Go toolchain'
    Assert-Tool "$vulkanDir/bin/glslc.exe" 'glslc'
    Assert-Tool "$RocmRoot/lib/amdhip64.dll" 'our HIP runtime in the ROCm-shaped root'
    Assert-Tool "$RocmRoot/lib/bc250hipblas.dll" 'our BLAS library in the ROCm-shaped root'

    # The source, at the commit this recipe is written against. A clone when there is none, and a check
    # in every case: the patches below apply to the working tree and leave HEAD where it is, so this
    # answer stays true after a build as well.
    if (-not (Test-Path $Source)) {
        Write-Host "   clone $Upstream -> $Source"
        New-Item -ItemType Directory -Force (Split-Path $Source) | Out-Null
        & git clone --no-checkout $Upstream $Source
        if ($LASTEXITCODE) { throw "the Ollama clone failed: $Upstream" }
        & git -C $Source checkout --detach $Commit
        if ($LASTEXITCODE) { throw "the Ollama commit is not in that clone: $Commit" }
    }
    Assert-Tool $Source 'the Ollama source'
    $head = ([string](& git -C $Source rev-parse HEAD)).Trim()
    if ($head -ne $Commit) {
        throw ("the Ollama source is at $head and this recipe pins $Commit. Check that commit out, or pass " +
               "-Commit $head to build it on purpose (then the asset's SHA-256 pin in tools/release/components.json " +
               'has to be filled again).')
    }

    # The Ollama patches. git apply is the gate: a patch that no longer matches stops the build
    # instead of producing a quietly unpatched binary.
    foreach ($patch in Get-ChildItem "$PSScriptRoot/patches/ollama" -Filter '*.patch' | Sort-Object Name) {
        $patchPath = $patch.FullName
        $applied = (Invoke-Probe { git -C $Source apply --reverse --check $patchPath }) -eq 0
        if ($applied) {
            Write-Host "   ollama patch already applied: $($patch.Name)"
            continue
        }
        & git -C $Source apply --whitespace=nowarn $patch.FullName
        if ($LASTEXITCODE) { throw "the Ollama patch does not apply: $($patch.Name)" }
        Write-Host "   ollama patch applied: $($patch.Name)"
    }

    # The llama.cpp patches are the ones our llama.cpp release uses. They are not copies: the
    # files live in compute/hip/patches, and this stage places them where Ollama's own patch
    # applier finds them, so the pinned llama.cpp gets them in every backend build.
    $compat = Join-Path $Source 'llama/compat'
    Assert-Tool $compat "Ollama's llama.cpp compatibility directory"
    $hipPatches = [ordered]@{
        '003-bc250-hip-no-device-printf.patch' = "$HipPatchDir/llama-no-device-printf.patch"
        '004-bc250-hip-allocation-cap.patch'   = "$HipPatchDir/llama-bc250-allocation-cap.patch"
    }
    foreach ($name in $hipPatches.Keys) {
        $from = $hipPatches[$name]
        Assert-Tool $from "the llama.cpp patch $name"
        Copy-Item $from (Join-Path $compat $name) -Force
        Write-Host "   llama.cpp patch staged: $name"
    }

    $record = [ordered]@{
        utc              = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        ollama_commit     = (& git -C $Source rev-parse HEAD)
        ollama_describe   = (& git -C $Source describe --tags --always)
        ollama_version    = Get-OllamaVersion
        llama_cpp_pin     = (Get-Content (Join-Path $Source 'LLAMA_CPP_VERSION') -Raw).Trim()
        bc250_win_commit  = (& git -C $repoRoot rev-parse HEAD)
        clang             = (& "$clangBin/clang.exe" --version | Select-Object -First 1)
        go                = (& $goExe version)
        rocm_root         = $RocmRoot
        # The two patch sets are numbered in their own directories, so each entry names the
        # source file as well. Without it two different patches read as 003 in this record.
        patches           = @(
            @(Get-ChildItem "$PSScriptRoot/patches/ollama" -Filter '*.patch' | ForEach-Object {
                [ordered]@{
                    patches = 'ollama'
                    name    = $_.Name
                    source  = "compute/ollama/patches/ollama/$($_.Name)"
                    sha256  = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
                }
            }) +
            @($hipPatches.Keys | ForEach-Object {
                [ordered]@{
                    patches = 'llama.cpp'
                    name    = $_
                    source  = ($hipPatches[$_] -replace '^.*/compute/', 'compute/')
                    sha256  = (Get-FileHash $hipPatches[$_] -Algorithm SHA256).Hash
                }
            })
        )
    }
    New-Item -ItemType Directory -Force $Work | Out-Null
    $record | ConvertTo-Json -Depth 5 | Set-Content "$Work/inputs.json" -Encoding utf8
    Write-Host "   inputs recorded in $Work/inputs.json"
}

# ---------------------------------------------------------------- shared configure

function Get-CommonCacheArguments {
    @(
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$ninja",
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_C_COMPILER=$clangBin/clang.exe",
        "-DCMAKE_CXX_COMPILER=$clangBin/clang++.exe",
        "-DCMAKE_RC_COMPILER=$clangBin/llvm-rc.exe",
        '-DCMAKE_C_COMPILER_TARGET=x86_64-pc-windows-msvc',
        '-DCMAKE_CXX_COMPILER_TARGET=x86_64-pc-windows-msvc',
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
        # Ollama's own presets: a loadable backend per directory, no host tuning, no OpenMP.
        '-DBUILD_SHARED_LIBS=ON',
        '-DGGML_BACKEND_DL=ON',
        '-DGGML_NATIVE=OFF',
        '-DGGML_OPENMP=OFF',
        # Max optimisation, the same set as our llama.cpp asset.
        '-DGGML_AVX=ON', '-DGGML_AVX2=ON', '-DGGML_FMA=ON', '-DGGML_F16C=ON', '-DGGML_BMI2=ON',
        # llama.cpp's processor kernels are written for the Microsoft compiler on Windows and
        # prefetch a block pointer through _mm_prefetch, whose parameter is const char *. Clang
        # has made that class of C warning an error by default since version 20. The call is
        # benign, so the diagnostics stay warnings here instead of the sources being edited.
        ('-DCMAKE_C_FLAGS=' + (@(
            '-Wno-error=incompatible-pointer-types',
            '-Wno-error=int-conversion',
            '-Wno-error=implicit-function-declaration',
            '-Wno-error=implicit-int'
        ) -join ' ')),
        '-DLLAMA_BUILD_TESTS=OFF', '-DLLAMA_BUILD_UI=OFF', '-DLLAMA_CURL=OFF', '-DLLAMA_OPENSSL=OFF'
    )
}

function Invoke-Cmake([string[]]$arguments, [string]$what) {
    & $cmake @arguments
    if ($LASTEXITCODE) { throw "$what failed: $LASTEXITCODE" }
}

function Invoke-ServerProject([string]$name, [string[]]$extra, [string[]]$targets, [string]$component) {
    $build = "$Work/$name"
    $configure = @('-S', "$Source/llama/server", '-B', $build) + (Get-CommonCacheArguments) + $extra
    Invoke-Cmake $configure "the $name configure"
    Invoke-Cmake (@('--build', $build, '--target') + $targets + @('-j', $Jobs)) "the $name build"
    # A GPU stage builds one target, so a whole-project install stops on the processor backend
    # that this build tree never produced. The install of that one target is its own component.
    $install = @('--install', $build, '--prefix', $Staging)
    if ($component) { $install += @('--component', $component) }
    Invoke-Cmake $install "the $name install"
}

# ---------------------------------------------------------------- base

function Invoke-Base {
    Write-Step 'base: llama-server and the processor backend'
    $extra = @(
        '-DOLLAMA_RUNNER_DIR=',
        "-DGGML_CPU_ALL_VARIANTS=$(if ($CpuAllVariants) { 'ON' } else { 'OFF' })"
    )
    Invoke-ServerProject 'llama-server-cpu' $extra @('llama-server', 'llama-quantize')
    Assert-Tool "$Staging/lib/ollama/llama-server.exe" 'the built llama-server'
    # The install carries llama.cpp's development payload as well: a second copy of every runtime
    # DLL in bin, the headers, the import libraries and the CMake and pkg-config files. Ollama
    # reads none of it, and a second copy of a backend DLL in a zip is a support question waiting
    # to happen, so the staging tree keeps lib/ollama and nothing else.
    foreach ($path in 'bin', 'include', 'lib/cmake', 'lib/pkgconfig') {
        Remove-Item -Recurse -Force "$Staging/$path" -ErrorAction SilentlyContinue
    }
    Get-ChildItem "$Staging/lib" -Filter '*.lib' -File -ErrorAction SilentlyContinue | Remove-Item -Force
    if (Get-ChildItem "$Staging/lib" -Exclude 'ollama' -ErrorAction SilentlyContinue) {
        throw "the staging tree still carries a development payload under lib: $((Get-ChildItem "$Staging/lib" -Exclude 'ollama').Name -join ', ')"
    }
}

# ---------------------------------------------------------------- vulkan

function Invoke-Vulkan {
    Write-Step 'vulkan: ggml-vulkan against our system Vulkan driver'
    # One shader compile job: the shader generator with four jobs exhausted this machine's
    # temporary space in scratch/llamacpp-vulkan-tuning.
    $env:GGML_VULKAN_SHADER_MAX_JOBS = '1'
    # llama.cpp builds its Vulkan shader generator as a nested CMake project, and passes the
    # outer build's compiler into it only when the outer build is a cross compilation. This one
    # is not, so the nested project picks a compiler from PATH and finds this clang with its own
    # default target, which is MinGW, and fails on the first standard library header. CC and CXX
    # are what the nested project reads, and the Microsoft compiler answers. The generator is a
    # build-time tool that emits shader headers: nothing it produces is linked into the backend,
    # so which compiler built it does not reach the artifact.
    $previousCc, $previousCxx = $env:CC, $env:CXX
    $env:CC = 'cl.exe'; $env:CXX = 'cl.exe'
    $extra = @(
        '-DOLLAMA_RUNNER_DIR=vulkan',
        '-DOLLAMA_GPU_BACKEND=vulkan',
        '-DGGML_VULKAN=ON',
        "-DVulkan_INCLUDE_DIR=$vulkanDir/include",
        "-DVulkan_LIBRARY=$vulkanDir/lib/vulkan-1.lib",
        "-DVulkan_GLSLC_EXECUTABLE=$vulkanDir/bin/glslc.exe",
        "-DCMAKE_PREFIX_PATH=$vulkanDir"
    )
    try {
        Invoke-ServerProject 'llama-server-vulkan' $extra @('ggml-vulkan') 'llama-server'
    } finally {
        $env:CC = $previousCc; $env:CXX = $previousCxx
    }
    Assert-Tool "$Staging/lib/ollama/vulkan/ggml-vulkan.dll" 'the built Vulkan backend'
}

# ---------------------------------------------------------------- hip

function Invoke-Hip {
    Write-Step 'hip: ggml-hip for gfx1013 against our HIP runtime'
    # The runner directory is deliberately not named rocm_v*: upstream gates two sets of ROCm
    # compiler flags on that prefix, and those flags belong to ROCm's compiler, not to ours.
    $extra = @(
        '-DOLLAMA_RUNNER_DIR=rocm_bc250',
        '-DOLLAMA_GPU_BACKEND=hip',
        '-DGGML_HIP=ON',
        '-DCMAKE_HIP_PLATFORM=amd',
        "-DCMAKE_PREFIX_PATH=$RocmRoot",
        # Our cmake answer for find_package(hip) reads this, not AMDGPU_TARGETS.
        '-DBC250_HIP_OFFLOAD_ARCH=gfx1013',
        # Measured on this part: no virtual memory management, no HIP graphs, MMQ kernels.
        '-DGGML_HIP_NO_VMM=ON',
        '-DGGML_HIP_GRAPHS=OFF',
        '-DGGML_CUDA_FORCE_MMQ=ON',
        # Flash attention on, as every HIP build requires.
        '-DGGML_CUDA_FA=ON',
        # The two measured BC-250 switches the staged llama.cpp patches read.
        '-DCMAKE_CXX_FLAGS=-DBC250_HIP_NO_DEVICE_PRINTF=1 -DBC250_HIP_MAX_ALLOC_BYTES=0xfffc0000ULL'
    )
    $previousRocm = $env:ROCM_PATH
    try {
        $env:ROCM_PATH = $RocmRoot
        Invoke-ServerProject 'llama-server-rocm_bc250' $extra @('ggml-hip') 'llama-server'
    } finally {
        $env:ROCM_PATH = $previousRocm
    }
    $dest = "$Staging/lib/ollama/rocm_bc250"
    Assert-Tool "$dest/ggml-hip.dll" 'the built HIP backend'
    # Our runtime next to the backend, so the loader finds it without a PATH change.
    foreach ($dll in 'amdhip64.dll', 'bc250hipblas.dll') {
        Copy-Item "$RocmRoot/lib/$dll" $dest -Force
    }
    $cache = Get-Content "$Work/llama-server-rocm_bc250/CMakeCache.txt"
    if ($cache -notcontains 'GGML_CUDA_FA:BOOL=ON') { throw 'the HIP backend was configured without Flash Attention' }
}

# ---------------------------------------------------------------- go

function Invoke-Go {
    Write-Step 'go: ollama.exe'
    $version = Get-OllamaVersion
    $env:GOPATH = "$Work/go/path"
    $env:GOMODCACHE = "$Work/go/mod"
    $env:GOCACHE = "$Work/go/cache"
    $env:GOTMPDIR = "$Work/tmp"
    $env:GOTOOLCHAIN = 'local'
    # cgo on, the way upstream builds the Windows binary. The MLX package declares its types in
    # cgo files on every platform, so a build without cgo does not compile. The compiler is the
    # MinGW gcc of msys64, because Go's cgo wants a GCC-style driver, not the MSVC-targeted clang
    # the CMake stages use.
    $env:CGO_ENABLED = '1'
    $env:CC = 'C:/msys64/mingw64/bin/gcc.exe'
    Assert-Tool $env:CC 'the MinGW gcc that cgo needs'
    New-Item -ItemType Directory -Force $env:GOPATH, $env:GOMODCACHE, $env:GOCACHE, $env:GOTMPDIR, $Staging | Out-Null
    Push-Location $Source
    try {
        $ldflags = "-s -w -X=github.com/ollama/ollama/version.Version=$version" +
            ' -X=github.com/ollama/ollama/server.mode=release'
        & $goExe build -trimpath -ldflags $ldflags -o "$Staging/ollama.exe" .
        if ($LASTEXITCODE) { throw "the Go build failed: $LASTEXITCODE" }
    } finally { Pop-Location }
    Assert-Tool "$Staging/ollama.exe" 'the built ollama.exe'
    Write-Host "   ollama.exe $version"
}

# ---------------------------------------------------------------- run

Enter-BuildEnvironment
$started = Get-Date
switch ($Stage) {
    'prepare' { Invoke-Prepare }
    'base' { Invoke-Base }
    'vulkan' { Invoke-Vulkan }
    'hip' { Invoke-Hip }
    'go' { Invoke-Go }
    'all' { Invoke-Prepare; Invoke-Base; Invoke-Vulkan; Invoke-Hip; Invoke-Go }
}
"stage $Stage took {0:N0} s" -f ((Get-Date) - $started).TotalSeconds

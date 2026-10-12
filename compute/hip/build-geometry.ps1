# Build the launch-geometry regression and run its host-only oracle controls.
# The GPU executable is never run by this script. An alternate include root is
# for a scratch-only old-header negative control; the repository header is intact.
param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$ImportLibrary,
    [string]$IncludeRoot = '',
    [string]$ClangBin = ''
)
$ErrorActionPreference = 'Stop'
if (-not $ClangBin) { $ClangBin = Join-Path $Root 'toolchain/llvm-amdgpu-22.1.8/mingw64/bin' }
if (-not $IncludeRoot) { $IncludeRoot = Join-Path $PSScriptRoot 'include' }
$clang = Join-Path $ClangBin 'clang.exe'
$readobj = Join-Path $ClangBin 'llvm-readobj.exe'
$source = Join-Path $PSScriptRoot 'tests/device/geometry.cu'
$header = Join-Path $IncludeRoot 'hip/hip_runtime.h'
foreach ($path in @($clang, $readobj, $source, $header, $ImportLibrary)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing input: $path" }
}
$ImportLibrary = (Resolve-Path -LiteralPath $ImportLibrary).Path
if ((Split-Path $ImportLibrary -Leaf) -ne 'amdhip64.lib') { throw 'Use the runtime amdhip64.lib' }
$importDir = Split-Path $ImportLibrary -Parent
$inputs = @($PSCommandPath, $source, $ImportLibrary, $clang, $readobj)
$inputs += @(Get-ChildItem -LiteralPath $IncludeRoot -File -Recurse | ForEach-Object FullName)
$pins = @($inputs | ForEach-Object {
    [ordered]@{path=(Resolve-Path -LiteralPath $_).Path; sha256=(Get-FileHash -LiteralPath $_).Hash}
})
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$saved = @{}
foreach ($name in @('TEMP', 'TMP', 'PATH', 'INCLUDE', 'LIB', 'LIBPATH')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $env:TEMP = $Out; $env:TMP = $Out
    $vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -property installationPath
    if (-not $vs) { throw 'Visual Studio toolset was not found' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    & cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^(INCLUDE|LIB|LIBPATH|Path)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
    }
    if ($LASTEXITCODE -ne 0) { throw 'Toolset environment failed' }
    $env:PATH = "$ClangBin;$env:PATH"
    $flags = @('-x', 'hip', '--offload-arch=gfx1013', '--target=x86_64-pc-windows-msvc',
        '-nogpuinc', '-nogpulib', '-O2', '-std=c++17', '-Wall', '-Wextra', '-Werror', "-I$IncludeRoot")
    $exe = Join-Path $Out 'geometry.exe'
    & $clang @flags $source "-L$importDir" -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'HIP geometry compilation failed' }
    $codeObject = Join-Path $Out 'geometry.gfx1013.co'
    & $clang @flags --offload-device-only --no-gpu-bundle-output $source -o $codeObject
    if ($LASTEXITCODE -ne 0) { throw 'Geometry code-object compilation failed' }
    & $readobj --notes $codeObject | Set-Content (Join-Path $Out 'geometry.metadata.txt')
    if ($LASTEXITCODE -ne 0) { throw 'Code-object metadata inspection failed' }
    $metadata = Get-Content (Join-Path $Out 'geometry.metadata.txt') -Raw
    if ($metadata -notmatch '(?m)^\s+\.wavefront_size:\s+32\s*$') {
        throw 'Geometry host oracle expects the compiled gfx1013 wave32 kernel'
    }
    $imports = & $readobj --coff-imports $exe
    if ($LASTEXITCODE -ne 0) { throw 'Import inspection failed' }
    $imports | Set-Content (Join-Path $Out 'imports.txt')
    $names = @($imports | ForEach-Object {
        if ($_ -match '^\s*Name: (\S+\.dll)\s*$') { $Matches[1].ToLowerInvariant() }
    })
    if ('amdhip64.dll' -notin $names -or @($names | Where-Object { $_ -notin @('amdhip64.dll', 'kernel32.dll') }).Count) {
        throw 'Unexpected geometry imports'
    }
    $oracle = Join-Path $Out 'geometry-oracle.exe'
    & $clang -x c++ --target=x86_64-pc-windows-msvc -O2 -std=c++17 -Wall -Wextra -Werror `
        -DBC250_GEOMETRY_HOST_ONLY=1 $source -o $oracle
    if ($LASTEXITCODE -ne 0) { throw 'Host oracle compilation failed' }
    & $oracle | Tee-Object (Join-Path $Out 'oracle.txt')
    if ($LASTEXITCODE -ne 0) { throw 'Host oracle controls failed' }
    foreach ($pin in $pins) {
        if ((Get-FileHash -LiteralPath $pin.path).Hash -ne $pin.sha256) {
            throw "Input changed during compilation; freeze headers and rebuild: $($pin.path)"
        }
    }
    $manifest = [ordered]@{kind='hip-geometry-control'; gpu_executed=$false; host_oracle_passed=$true;
        inputs_stable=$true; inputs=$pins; flags=$flags; files=@()}
    foreach ($path in @($source, $header, $ImportLibrary, $clang, $exe, $codeObject, $oracle)) {
        $manifest.files += @{path=$path; sha256=(Get-FileHash -LiteralPath $path).Hash}
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Out 'build.json')
    Get-FileHash -LiteralPath $exe
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}

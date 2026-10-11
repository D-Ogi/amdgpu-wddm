# Rebuild shipped HIP samples and HIPBLAS after a device-header change.
# Runs only the independent HIPBLAS CPU core test. No GPU executable is run.
param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$ImportLibrary,
    [string]$ClangBin = ''
)
$ErrorActionPreference = 'Stop'
if (-not $ClangBin) { $ClangBin = Join-Path $Root 'toolchain/llvm-amdgpu-22.1.8/mingw64/bin' }
$clang = Join-Path $ClangBin 'clang.exe'
$readobj = Join-Path $ClangBin 'llvm-readobj.exe'
$hipblas = Join-Path $PSScriptRoot '../hipblas'
foreach ($path in @($clang, $readobj, $ImportLibrary)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing input: $path" }
}
$ImportLibrary = (Resolve-Path -LiteralPath $ImportLibrary).Path
if ((Split-Path $ImportLibrary -Leaf) -ne 'amdhip64.lib') { throw 'Use the runtime amdhip64.lib' }
$importDir = Split-Path $ImportLibrary -Parent
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$inputs = @($PSCommandPath, (Join-Path $PSScriptRoot 'samples/vadd.hip'),
    (Join-Path $PSScriptRoot 'samples/vadd_expect.h'), (Join-Path $PSScriptRoot 'samples/hipbench.hip'),
    (Join-Path $hipblas 'build-hipblas.ps1'), (Join-Path $hipblas 'bc250hipblas.def'),
    (Join-Path $hipblas 'tests/gemm_check.hip'), (Join-Path $hipblas 'tests/host/test_gemm_core.cpp'),
    $ImportLibrary, $clang, $readobj, (Join-Path $ClangBin 'clang++.exe'),
    (Join-Path $ClangBin 'llvm-dlltool.exe'))
$inputs += @(Get-ChildItem (Join-Path $PSScriptRoot 'include') -File -Recurse | ForEach-Object FullName)
$inputs += @(Get-ChildItem (Join-Path $hipblas 'src') -File -Recurse | ForEach-Object FullName)
$pins = @($inputs | ForEach-Object {
    [ordered]@{path=(Resolve-Path -LiteralPath $_).Path; sha256=(Get-FileHash -LiteralPath $_).Hash}
})
$saved = @{}
foreach ($name in @('TEMP', 'TMP', 'PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'UCRTVersion', 'WindowsSdkDir', 'WindowsSDKVersion')) {
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
        '-nogpuinc', '-nogpulib', '-O2', '-std=c++17', '-Wall', '-Wextra', '-Werror',
        # Shared sample-oracle headers define static helpers used only by their
        # separate host tests (vadd_produced); do not edit those production inputs.
        '-Wno-unused-function', "-I$PSScriptRoot/include")
    foreach ($name in @('vadd', 'hipbench')) {
        $exe = Join-Path $Out "$name.exe"
        & $clang @flags (Join-Path $PSScriptRoot "samples/$name.hip") "-L$importDir" -o $exe
        if ($LASTEXITCODE -ne 0) { throw "HIP compilation failed: $name" }
        $imports = & $readobj --coff-imports $exe
        if ($LASTEXITCODE -ne 0) { throw "Import inspection failed: $name" }
        $imports | Set-Content (Join-Path $Out "$name-imports.txt")
        $names = @($imports | ForEach-Object {
            if ($_ -match '^\s*Name: (\S+\.dll)\s*$') { $Matches[1].ToLowerInvariant() }
        })
        if ('amdhip64.dll' -notin $names -or @($names | Where-Object { $_ -notin @('amdhip64.dll', 'kernel32.dll') }).Count) {
            throw "Unexpected imports: $name"
        }
    }
    & (Join-Path $hipblas 'build-hipblas.ps1') -Root $Root -Out (Join-Path $Out 'hipblas') `
        -HipLib $importDir -ClangBin $ClangBin -SkipMockRun
    # The called builder throws on every failing tool or CPU-test exit.
    foreach ($pin in $pins) {
        if ((Get-FileHash -LiteralPath $pin.path).Hash -ne $pin.sha256) {
            throw "Input changed during compilation; freeze headers and rebuild: $($pin.path)"
        }
    }
    $outputs = @('vadd.exe', 'hipbench.exe', 'hipblas/bc250hipblas.dll',
        'hipblas/bc250hipblas.lib', 'hipblas/gemm_check.exe', 'hipblas/test_gemm_core.exe')
    $artifacts = @($outputs | ForEach-Object {
        $path = Join-Path $Out $_
        [ordered]@{path=$path; bytes=(Get-Item -LiteralPath $path).Length; sha256=(Get-FileHash -LiteralPath $path).Hash}
    })
    [ordered]@{gpu_executed=$false; inputs_stable=$true; inputs=$pins; flags=$flags; outputs=$artifacts} |
        ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Out 'build.json')
    $artifacts | Format-Table -AutoSize
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}

# Build a device conformance client without executing it on the build host.
# The operator runs the executable with a bounded lab supervisor.
param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$ImportLibrary,
    [ValidatePattern('^[a-z][a-z0-9_-]*$')][string]$Name = 'builtins',
    [string]$ClangBin = ''
)
$ErrorActionPreference = 'Stop'
if (-not $ClangBin) { $ClangBin = Join-Path $Root 'toolchain/llvm-amdgpu-22.1.8/mingw64/bin' }
$clang = Join-Path $ClangBin 'clang.exe'
$readobj = Join-Path $ClangBin 'llvm-readobj.exe'
$includeRoot = Join-Path $PSScriptRoot 'include'
$source = Join-Path $PSScriptRoot "tests/device/$Name.cu"
foreach ($path in @($clang, $readobj, $source, $ImportLibrary)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing input: $path" }
}
$ImportLibrary = (Resolve-Path -LiteralPath $ImportLibrary).Path
if ((Split-Path $ImportLibrary -Leaf) -ne 'amdhip64.lib') { throw 'Use the runtime amdhip64.lib' }
$importDir = Split-Path $ImportLibrary -Parent
$inputs = @($PSCommandPath, $source, $ImportLibrary, $clang, $readobj)
$inputs += @(Get-ChildItem -LiteralPath $includeRoot -File -Recurse | ForEach-Object FullName)
$pins = @($inputs | ForEach-Object {
    [ordered]@{path=(Resolve-Path -LiteralPath $_).Path; sha256=(Get-FileHash -LiteralPath $_).Hash}
})
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$saved = @{}
foreach ($envName in @('TEMP', 'TMP', 'PATH', 'INCLUDE', 'LIB', 'LIBPATH')) {
    $saved[$envName] = [Environment]::GetEnvironmentVariable($envName, 'Process')
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
        '-nogpuinc', '-nogpulib', '-O2', '-std=c++17', '-Wall', '-Wextra', '-Werror', "-I$includeRoot")
    $exe = Join-Path $Out "$Name.exe"
    & $clang @flags $source "-L$importDir" -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'Device control compilation failed' }
    $codeObject = Join-Path $Out "$Name.gfx1013.co"
    & $clang @flags --offload-device-only --no-gpu-bundle-output $source -o $codeObject
    if ($LASTEXITCODE -ne 0) { throw 'Device control code-object compilation failed' }
    $metadata = & $readobj --notes $codeObject
    if ($LASTEXITCODE -ne 0) { throw 'Code-object metadata inspection failed' }
    $metadata | Set-Content (Join-Path $Out "$Name.metadata.txt")
    $waveSizes = @($metadata | ForEach-Object {
        if ($_ -match '^\s+\.wavefront_size:\s+(\d+)\s*$') { [int]$Matches[1] }
    })
    if (-not $waveSizes.Count -or @($waveSizes | Where-Object { $_ -ne 32 }).Count) {
        throw 'Device control host oracle requires gfx1013 wave32 kernels'
    }
    $imports = & $readobj --coff-imports $exe
    if ($LASTEXITCODE -ne 0) { throw 'Import inspection failed' }
    $imports | Set-Content (Join-Path $Out 'imports.txt')
    $names = @($imports | ForEach-Object {
        if ($_ -match '^\s*Name: (\S+\.dll)\s*$') { $Matches[1].ToLowerInvariant() }
    })
    if ('amdhip64.dll' -notin $names -or @($names | Where-Object { $_ -notin @('amdhip64.dll', 'kernel32.dll') }).Count) {
        throw 'Unexpected device control imports'
    }
    foreach ($pin in $pins) {
        if ((Get-FileHash -LiteralPath $pin.path).Hash -ne $pin.sha256) {
            throw "Input changed during compilation; freeze headers and rebuild: $($pin.path)"
        }
    }
    $outputs = @($exe, $codeObject) | ForEach-Object {
        [ordered]@{path=$_; bytes=(Get-Item -LiteralPath $_).Length; sha256=(Get-FileHash -LiteralPath $_).Hash}
    }
    [ordered]@{kind='hip-device-control'; name=$Name; gpu_executed=$false;
        inputs_stable=$true; inputs=$pins; flags=$flags; outputs=@($outputs)} |
        ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Out 'build.json')
    $outputs | Format-List
} finally {
    foreach ($envName in $saved.Keys) { [Environment]::SetEnvironmentVariable($envName, $saved[$envName], 'Process') }
}

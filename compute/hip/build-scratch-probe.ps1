# Compile and inspect the numeric fixed-private probe. This script never runs it.
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
$source = Join-Path $PSScriptRoot 'samples/scratch.hip'
foreach ($path in @($clang, $readobj, $source, $ImportLibrary)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing input: $path" }
}
$ImportLibrary = (Resolve-Path -LiteralPath $ImportLibrary).Path
if ((Split-Path $ImportLibrary -Leaf) -ne 'amdhip64.lib') { throw 'Use the runtime amdhip64.lib' }
$importDir = Split-Path $ImportLibrary -Parent
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$env:TEMP = $Out
$env:TMP = $Out
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
    "-I$PSScriptRoot/include")
$exe = Join-Path $Out 'scratch.exe'
& $clang @flags $source "-L$importDir" -o $exe
if ($LASTEXITCODE -ne 0) { throw 'HIP probe compilation failed' }
$imports = & $readobj --coff-imports $exe
if ($LASTEXITCODE -ne 0) { throw 'Import inspection failed' }
$imports | Set-Content (Join-Path $Out 'imports.txt')
$names = @($imports | ForEach-Object {
    if ($_ -match '^\s*Name: (\S+\.dll)\s*$') { $Matches[1].ToLowerInvariant() }
})
if ('amdhip64.dll' -notin $names -or @($names | Where-Object { $_ -notin @('amdhip64.dll', 'kernel32.dll') }).Count) {
    throw 'Unexpected probe imports'
}
$manifest = [ordered]@{kind='lab-only-scratch-probe'; executed=$false; flags=$flags; files=@()}
foreach ($path in @($source, $ImportLibrary, $clang, $exe, (Join-Path $PSScriptRoot 'include/hip/hip_runtime.h'))) {
    $manifest.files += @{path=$path; sha256=(Get-FileHash -LiteralPath $path).Hash}
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Out 'build.json')
Get-FileHash -LiteralPath $exe

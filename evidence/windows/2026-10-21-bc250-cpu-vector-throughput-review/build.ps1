# Builds the two q44bench binaries of lab-run-q44.txt from the sources in this directory, the same way as
# tools/win/cpubench/build.ps1: both compilers named by path, the headers and libraries from -Kits.
#   q44bench-clang.exe  clang-cl /O2 /MT -mavx2 -mfma: the C parts and the explicit-assembly kernels of gen.py
#   q44bench-msvc.exe   cl /O2 /MT, no /arch (the flags of wsi_common_win32.cpp in the shipped Mesa build): the C
#                       parts only, mesa_gdi.cpp as C++ with the Mesa flags /std:c++17 /permissive- /EHsc
# The measured binaries came from clang-cl 23.1.4 and cl 19.44.35221 (SHA-256 in README.md). gen.py writes
# kernels_gen.inc into -Out, and the clang build finds it there.
param(
    [Parameter(Mandatory)][string]$Kits,
    # LLVM directory whose bin\ holds clang-cl.exe and lld-link.exe.
    [Parameter(Mandatory)][string]$Llvm,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$clang = Join-Path $Llvm 'bin\clang-cl.exe'
foreach ($tool in $cl, $clang) { if (-not (Test-Path -LiteralPath $tool)) { throw "compiler not found: $tool" } }
"cl        $cl"
"          version $((& $cl 2>&1 | Out-String) -replace '(?s).*?\b(19\.\d+\.\d+(\.\d+)?)\b.*', '$1')"
"clang-cl  $clang"
"          $(& $clang --version | Select-Object -First 1)"
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
$env:INCLUDE = @((Join-Path $msvc.FullName 'include'), "$sdk\Include\$KitVersion\ucrt", "$sdk\Include\$KitVersion\um",
    "$sdk\Include\$KitVersion\shared") -join ';'
$env:LIB = @((Join-Path $msvc.FullName 'lib\x64'), "$sdkLib\ucrt\x64", "$sdkLib\um\x64") -join ';'
python -I (Join-Path $here 'gen.py') (Join-Path $Out 'kernels_gen.inc')
if ($LASTEXITCODE -ne 0) { throw 'gen.py failed' }
$c = Join-Path $here 'q44bench.c'
$cpp = Join-Path $here 'mesa_gdi.cpp'
Push-Location $Out
try {
    & $clang /nologo /O2 /MT -mavx2 -mfma -fuse-ld=lld "/I$Out" /Fe:q44bench-clang.exe $c $cpp
    if ($LASTEXITCODE -ne 0) { throw "clang-cl failed ($LASTEXITCODE)" }
    & $cl /nologo /O2 /W3 /MT /c $c
    if ($LASTEXITCODE -ne 0) { throw "cl (c) failed ($LASTEXITCODE)" }
    & $cl /nologo /O2 /W2 /MT /std:c++17 /permissive- /EHsc /c $cpp
    if ($LASTEXITCODE -ne 0) { throw "cl (cpp) failed ($LASTEXITCODE)" }
    & $cl /nologo /Fe:q44bench-msvc.exe q44bench.obj mesa_gdi.obj
    if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }
} finally { Pop-Location }
Get-ChildItem $Out -Filter *.exe | Sort-Object Name | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }

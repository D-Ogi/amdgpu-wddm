param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$env:TEMP = $Out; $env:TMP = $Out
& python -B -m unittest discover -s $PSScriptRoot -p test_dcn_observe_client.py -v
if ($LASTEXITCODE) { throw 'DCN observer evidence controls failed' }
. "$repo\tools\build\umd-compiler.ps1"
$compiler = Resolve-Bc250UmdCompiler $Root 'clang' '' $Out 'x64'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c\Include\10.0.26100.0"
$libs = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
$flags = @('/nologo', '/std:c++17', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/Brepro', '-fuse-ld=lld',
    '--target=x86_64-pc-windows-msvc', '/clang:-march=x86-64-v3', '/clang:-mtune=znver2',
    "/FI$repo\tools\build\clang-windows.h", "/imsvc$($msvc.FullName)\include",
    "/imsvc$sdk\ucrt", "/imsvc$sdk\um", "/imsvc$sdk\shared",
    "/Fo$Out\dcn_observe.obj", "/Fe$Out\dcn_observe.exe", "$PSScriptRoot\dcn_observe.cpp", '/link', '/Brepro',
    "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$libs\ucrt\x64", "/LIBPATH:$libs\um\x64",
    'gdi32.lib', 'dxgi.lib')
Invoke-Bc250UmdCompiler $compiler @flags
if ($LASTEXITCODE) { throw 'DCN observer compilation failed' }
& "$Out\dcn_observe.exe" --help
if ($LASTEXITCODE) { throw 'Observer help failed' }
Get-FileHash "$Out\dcn_observe.exe"
$inputs = @('tools/win/bc250kmd_cli/dcn_observe.cpp', 'tools/win/bc250kmd_cli/build-dcn-observe.ps1',
    'tools/win/bc250kmd_cli/run-dcn-observe.ps1', 'tools/win/bc250kmd_cli/analyze-dcn-observe.py',
    'tools/win/bc250kmd_cli/test_dcn_observe_client.py',
    'tools/build/umd-compiler.ps1', 'tools/build/clang-windows.h', 'driver/kmd/bc250kmd_escape.h')
$hashes = @{}
foreach ($inputPath in $inputs) { $hashes[$inputPath] = (Get-FileHash -LiteralPath "$repo\$inputPath").Hash }
@{schema=1; source_commit=(& git -C $repo rev-parse HEAD); source_status=@(& git -C $repo status --porcelain);
  compiler=(Get-Content "$Out\compiler.json" -Raw | ConvertFrom-Json); arguments=$flags; inputs=$hashes;
  executable_sha256=(Get-FileHash "$Out\dcn_observe.exe").Hash; built_utc=[DateTime]::UtcNow.ToString('o')} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$Out\build.json" -Encoding UTF8

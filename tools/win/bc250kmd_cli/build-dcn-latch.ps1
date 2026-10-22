param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..\..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out=(Resolve-Path $Out).Path
$env:TEMP=$Out; $env:TMP=$Out
. "$repo\tools\build\umd-compiler.ps1"
$compiler=Resolve-Bc250UmdCompiler $Root 'clang' '' $Out 'x64'
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c\Include\10.0.26100.0"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
$flags=@('/nologo','/std:c++17','/W4','/WX','/O2','/MT','/EHsc','/Brepro','-fuse-ld=lld',
    '--target=x86_64-pc-windows-msvc','/clang:-march=x86-64-v3','/clang:-mtune=znver2',
    "/FI$repo\tools\build\clang-windows.h", "/imsvc$($msvc.FullName)\include",
    "/imsvc$sdk\ucrt", "/imsvc$sdk\um", "/imsvc$sdk\shared",
    "/Fo$Out\dcn_latch.obj", "/Fe$Out\dcn_latch.exe", "$PSScriptRoot\dcn_latch.cpp", '/link','/Brepro',
    "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$libs\ucrt\x64", "/LIBPATH:$libs\um\x64",'gdi32.lib','dxgi.lib')
Invoke-Bc250UmdCompiler $compiler @flags
if ($LASTEXITCODE) {throw 'Latch client compilation failed'}
& "$Out\dcn_latch.exe" --help
if ($LASTEXITCODE) {throw 'Latch client help failed'}
$testFlags=@($flags | ForEach-Object {$_.Replace('dcn_latch.obj','dcn_latch_client_test.obj').Replace('dcn_latch.exe','dcn_latch_client_test.exe').Replace('dcn_latch.cpp','dcn_latch_client_test.cpp')})
Invoke-Bc250UmdCompiler $compiler @testFlags
if ($LASTEXITCODE) {throw 'Latch client test compilation failed'}
& "$Out\dcn_latch_client_test.exe"
if ($LASTEXITCODE) {throw 'Latch client reply validation failed'}
$stubFlags=@($flags | ForEach-Object {$_.Replace('dcn_latch.obj','dcn_latch_runner_stub.obj').Replace('dcn_latch.exe','dcn_latch_runner_stub.exe').Replace('dcn_latch.cpp','dcn_latch_runner_stub.cpp')})
Invoke-Bc250UmdCompiler $compiler @stubFlags
if ($LASTEXITCODE) {throw 'Latch runner stub compilation failed'}
# These malformed requests return before adapter enumeration. Never run a valid
# request on the build host; real calls belong to the frozen, guarded lab kit.
$invalid=@(
    @('--arm','0'),
    @('--expected-version','0x000700D8','--arm','3'),
    @('--expected-version','0x000700D8','--arm','-1'),
    @('--expected-version','0x000700D8','--arm','4294967296'),
    @('--expected-version','0','--arm','0'),
    @('--arm','0','--arm','1'),
    @('--unknown','1','--arm','0')
)
foreach ($arguments in $invalid) {
    $output=& "$Out\dcn_latch.exe" @arguments 2>&1
    $code=$LASTEXITCODE
    $record=($output -join "`n") | ConvertFrom-Json
    if ($code -ne 1 -or $record.stage -notin @('arguments','bounds')) {throw 'Latch client pre-adapter refusal failed'}
}
$inputs=@('tools/win/bc250kmd_cli/dcn_latch.cpp','tools/win/bc250kmd_cli/dcn_latch_client_test.cpp','tools/win/bc250kmd_cli/build-dcn-latch.ps1',
    'tools/win/bc250kmd_cli/run-dcn-latch.ps1','tools/win/bc250kmd_cli/dcn_latch_runner_stub.cpp','tools/win/bc250kmd_cli/test-dcn-latch-runner.ps1',
    'tools/build/umd-compiler.ps1','tools/build/clang-windows.h','driver/kmd/bc250kmd_escape.h',
    'driver/kmd/dcn_latch_escape.h','driver/kmd/dcn_latch_probe.h','driver/kmd/dcn_latch_sample.h','driver/kmd/display_io.h')
$hashes=@{}
foreach ($inputPath in $inputs) {$hashes[$inputPath]=(Get-FileHash -LiteralPath "$repo\$inputPath").Hash}
@{schema=1; source_commit=(& git -C $repo rev-parse HEAD); source_status=@(& git -C $repo status --porcelain);
  compiler=(Get-Content "$Out\compiler.json" -Raw | ConvertFrom-Json); arguments=$flags; inputs=$hashes;
  executable_sha256=(Get-FileHash "$Out\dcn_latch.exe").Hash; pre_adapter_refusals=$invalid.Count;
  adapter_opened=$false; built_utc=[DateTime]::UtcNow.ToString('o')} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$Out\build.json" -Encoding UTF8
Get-FileHash "$Out\dcn_latch.exe"

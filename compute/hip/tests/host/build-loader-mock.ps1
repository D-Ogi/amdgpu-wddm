param(
 [Parameter(Mandatory)][string]$Root,
 [Parameter(Mandatory)][string]$Out,
 [string]$LoaderSource = '',
 [string]$KitVersion = '10.0.26100.0'
)
# Explicit opt-in host-only DLL. Never links kmt_device, kmt_memory, submit or gdi32.
$ErrorActionPreference='Stop'
$hip=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if(-not $LoaderSource){$LoaderSource=Join-Path $hip 'bc250hsa'}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl=Join-Path $msvc.FullName 'bin/Hostx64/x64/cl.exe'
$dumpbin=Join-Path $msvc.FullName 'bin/Hostx64/x64/dumpbin.exe'
$sdk=Join-Path $Root 'toolchain/nuget/microsoft.windows.sdk.cpp/c'
$libs=Join-Path $Root 'toolchain/nuget/microsoft.windows.sdk.cpp.x64/c'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out=(Resolve-Path $Out).Path
$env:TEMP=$Out;$env:TMP=$Out;$env:INCLUDE='';$env:LIB=''
$incs=@("/I$($msvc.FullName)/include","/I$sdk/Include/$KitVersion/ucrt","/I$sdk/Include/$KitVersion/um","/I$sdk/Include/$KitVersion/shared","/I$hip/include","/I$hip/runtime","/I$hip/tests/host","/I$hip/bc250hsa")
$common=@('/nologo','/W4','/WX','/O2','/Brepro','/MD','/D_CRT_SECURE_NO_WARNINGS','/DBC250_HIP_BUILD_DLL','/DBC250_HIP_MOCK_PRODUCTION_LOADER')+$incs
$c=@((Join-Path $hip 'tests/host/hipmock_backend.c'))+@('co_loader.c','co_metadata.c','co_msgpack.c','kernarg.c' | ForEach-Object {Join-Path $LoaderSource $_})
& $cl @common /c /std:c11 "/Fo$Out/" @c
if($LASTEXITCODE){throw 'host-only C compile failed'}
$cpp=@('hip_device.cpp','hip_error.cpp','hip_event.cpp','hip_launch.cpp','hip_log.cpp','hip_memory.cpp','hip_module.cpp','hip_perf.cpp','hip_stream.cpp','dllmain.cpp' | ForEach-Object {Join-Path $hip "runtime/$_"})
& $cl @common /c /std:c++17 /EHsc "/Fo$Out/" @cpp
if($LASTEXITCODE){throw 'runtime compile failed'}
$objects=@($c+$cpp | ForEach-Object {Join-Path $Out (([IO.Path]::GetFileNameWithoutExtension($_))+'.obj')})
$exe=Join-Path $Out 'amdhip64.dll'
& $cl /nologo /LD /MD "/Fe$exe" @objects /link /Brepro "/DEF:$hip/runtime/amdhip64.def" "/LIBPATH:$($msvc.FullName)/lib/x64" "/LIBPATH:$libs/ucrt/x64" "/LIBPATH:$libs/um/x64" kernel32.lib
if($LASTEXITCODE){throw 'mock link failed'}
$imports=& $dumpbin /nologo /imports $exe | Out-String
$imports | Set-Content (Join-Path $Out 'imports.txt')
if($imports -match '(?i)(gdi32|win32u|dxgi|d3d\d+|vulkan|opencl|nvcuda|bc250hsa)\.dll|D3DKMT|CreateFile[AW]'){throw 'unexpected device-facing import'}
$manifest=[ordered]@{kind='production-loader-host-transport-only';sources=@();headers=@();dll=(Get-FileHash $exe).Hash;imports_sha256=(Get-FileHash (Join-Path $Out 'imports.txt')).Hash}
foreach($p in $c+$cpp){$manifest.sources+=@{path=$p;sha256=(Get-FileHash $p).Hash}}
$headers=@(Get-ChildItem -LiteralPath (Join-Path $hip 'include'),(Join-Path $hip 'runtime'),(Join-Path $hip 'tests/host'),(Join-Path $hip 'bc250hsa'),$LoaderSource -Filter '*.h' -File -Recurse | Sort-Object FullName -Unique)
foreach($p in $headers){$manifest.headers+=@{path=$p.FullName;sha256=(Get-FileHash $p.FullName).Hash}}
$manifest | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Out 'mock-build.json')
Get-FileHash $exe


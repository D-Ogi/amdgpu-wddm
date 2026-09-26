param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\m9\smu-native",[string]$Source='')
$ErrorActionPreference='Stop'
$repo="$Root\bc250-win"
if(-not $Source){$Source="$repo\driver\kmd\smu.c"}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl="$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$wdk="$Root\toolchain\nuget\microsoft.windows.wdk.x64\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP="$Root\scratch\tmp";$env:TMP=$env:TEMP
$inc=@("/I$Out","/I$PSScriptRoot","/I$repo\driver\kmd","/I$repo\third_party\linux-amdgpu","/I$repo\driver\shim","/I$repo\driver\shim\include","/I$repo\driver\amdgpu-import")
& python "$PSScriptRoot\extract_smu.py" --check
if($LASTEXITCODE -ne 0){throw 'AMD mailbox source/register drift'}
& python "$PSScriptRoot\extract_clock.py" --check
if($LASTEXITCODE -ne 0){throw 'AMD source drift'}
$native=(Get-Content -LiteralPath $Source -Raw).Replace('#include "bc250kmd.h"','#include "smu_native_mock.h"').Replace('../shim/generated/','generated/')
[IO.File]::WriteAllText("$Out\smu-native.inc",$native,[Text.UTF8Encoding]::new($false))
& $cl /nologo /TC /W4 /WX /O2 /MT @inc "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\smu_test.exe" "$repo\driver\shim\bc250_smu.c" "$repo\driver\shim\bc250_clock.c" "$PSScriptRoot\smu_native_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Host build failed'}
& "$Out\smu_test.exe"
exit $LASTEXITCODE

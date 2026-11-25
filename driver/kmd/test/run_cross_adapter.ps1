param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\cross-adapter",[switch]$EveryLevel,[switch]$Truncate)
# Host control of cross_adapter.h: the level of MemoryManagementCaps.CrossAdapterResource and the answer of
# DxgkDdiDescribeAllocation. The header under test is copied next to the build, so that a negative control changes
# the copy and never the tree (/I$Out comes first, so the copy wins).
# -EveryLevel: the cap is reported at every level, also below 2.4. -Truncate: a BC2A allocation above 4 GiB - 1 bytes
# is answered as a buffer, which would truncate its Width. Each control must fail the test.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$h=Get-Content "$repo\driver\kmd\cross_adapter.h" -Raw
if($EveryLevel){
    $live='    return Level >= BC250_WDDM_LEVEL_2_4;'
    if(!$h.Contains($live)){throw 'negative control: the level rule moved'}
    $h=$h.Replace($live,'    return Level >= BC250_WDDM_LEVEL_2_0;')
}
if($Truncate){
    $live='    if (Bytes == 0 || Bytes > BC250_CROSS_ADAPTER_MAX_WIDTH) return BC250_DESCRIBE_REFUSED;'
    if(!$h.Contains($live)){throw 'negative control: the width rule moved'}
    $h=$h.Replace($live,'    if (Bytes == 0) return BC250_DESCRIBE_REFUSED;')
}
[IO.File]::WriteAllText((Join-Path $Out 'cross_adapter.h'),$h)
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4127 /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\cross_adapter_test.exe" "$PSScriptRoot\cross_adapter_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'cross adapter host build failed'}
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\cross_adapter_test.exe"

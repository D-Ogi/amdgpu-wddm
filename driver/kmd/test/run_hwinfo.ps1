param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\hwinfo-host")
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$chipTable=(Get-Content (Join-Path $repo 'driver\kmd\chip_table_gen.h') -Raw)
$source=(Get-Content (Join-Path $repo 'driver\kmd\hwinfo.c') -Raw).Replace('#include "bc250kmd.h"','').Replace('#include <ntstrsafe.h>','').Replace('#include "chip_table_gen.h"',$chipTable)
$source=$source.Replace('WCHAR ','wchar_t ').Replace('sizeof(WCHAR)','sizeof(wchar_t)').Replace('RTL_NUMBER_OF','_countof').Replace('RtlStringCchPrintfW','_snwprintf')
$template=Get-Content (Join-Path $PSScriptRoot 'hwinfo_test.c') -Raw
[IO.File]::WriteAllText((Join-Path $Out 'actual.c'),$template.Replace('/* ACTUAL_HWINFO */',$source))
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT /D_CRT_SECURE_NO_WARNINGS "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\hwinfo_test.exe" "$Out\actual.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'hwinfo host build failed'}
& "$Out\hwinfo_test.exe"
exit $LASTEXITCODE

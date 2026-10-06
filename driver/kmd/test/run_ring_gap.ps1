param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\ring-gap")
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /wd4127 /O2 /MT "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\ring_gap_test.exe" "$PSScriptRoot\ring_gap_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'ring gap host build failed'}
& "$Out\ring_gap_test.exe"
exit $LASTEXITCODE

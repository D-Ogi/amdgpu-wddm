# The KMD's stored fan-choice reader with a host query adapter; no hardware I/O.
param([Parameter(Mandatory=$true)][string]$Out,[Parameter(Mandatory=$true)][string]$Kits)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$libs=Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$env:INCLUDE="$($msvc.FullName)\include;$sdk\Include\10.0.26100.0\ucrt;$repo\driver\shim\include"
$env:LIB="$($msvc.FullName)\lib\x64;$libs\ucrt\x64;$libs\um\x64"
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/Fo$Out\" "/Fe$Out\fan_choice_test.exe" `
    "$repo\driver\shim\bc250_fan.c" "$repo\driver\shim\bc250_hwmon.c" "$repo\driver\kmd\test\fan_choice_test.c"
if ($LASTEXITCODE) { throw 'Fan-choice test compilation failed' }
& "$Out\fan_choice_test.exe"
if ($LASTEXITCODE) { throw 'Fan-choice test failed' }

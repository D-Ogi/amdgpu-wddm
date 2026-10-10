# CPU-only tests; callbacks operate on an in-memory block, never hardware.
param([Parameter(Mandatory)][string]$Root, [string]$Out = "$Root\scratch\uma-windows\host",
      [ValidateSet("none")][string]$Mutation = "none")
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl = "$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = "$Root\scratch\tmp"; $env:TMP = $env:TEMP
$source = [IO.File]::ReadAllText("$repo\driver\kmd\board_memory_store.c")
foreach ($include in '#include <ntifs.h>','#include "bc250kmd.h"','#include "board_memory_store.h"') { $source=$source.Replace($include,'') }
$template=[IO.File]::ReadAllText("$PSScriptRoot\board_memory_store_test.c")
[IO.File]::WriteAllText("$Out\actual.c",$template.Replace('/* ACTUAL_STORE */',$source))
& $cl /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\store_test.exe" "$Out\actual.c" "$repo\driver\shim\bc250_uma.c" /link advapi32.lib "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Store mock compile failed'}
$output=@(& "$Out\store_test.exe");$code=$LASTEXITCODE
$output | Set-Content "$Out\checks.txt";$output | Write-Output
Get-FileHash "$repo\driver\kmd\board_memory_store.c","$PSScriptRoot\board_memory_store_test.c","$PSCommandPath","$Out\store_test.exe" | Select-Object Path,Hash | ConvertTo-Json | Set-Content "$Out\pins.json"
if($code -ne 0){throw 'Store mock checks failed'}

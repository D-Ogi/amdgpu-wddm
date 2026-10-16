param([Parameter(Mandatory=$true)][string]$Out,[Parameter(Mandatory=$true)][string]$Kits)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..").Path
New-Item -ItemType Directory -Force "$Out\tmp" | Out-Null
$env:TEMP="$Out\tmp";$env:TMP=$env:TEMP
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$libs=Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$env:INCLUDE="$($msvc.FullName)\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
$env:LIB="$($msvc.FullName)\lib\x64;$libs\ucrt\x64;$libs\um\x64"
$production=[IO.File]::ReadAllText("$repo\driver\kmd\state_kernel.c")
$production=[regex]::Replace($production,'(?m)^#include[^\r\n]*','')
$template=[IO.File]::ReadAllText("$repo\driver\state\kernel_test.c")
$board=[regex]::Replace([IO.File]::ReadAllText("$repo\driver\kmd\state_board.c"),'(?m)^#include[^\r\n]*','')
[IO.File]::WriteAllText("$Out\kernel_actual.c",$template.Replace('/* ACTUAL_KERNEL */',$production).Replace('/* ACTUAL_BOARD */',$board))
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\state" "/Fo$Out\" `
    "/Fe$Out\kernel_test.exe" "$repo\driver\state\state.c" "$repo\driver\state\transfer.c" "$Out\kernel_actual.c" /link advapi32.lib ntdll.lib
if($LASTEXITCODE){throw 'Kernel bootstrap fixture compile failed'}
& "$Out\kernel_test.exe"
if($LASTEXITCODE){throw 'Kernel bootstrap fixture failed'}

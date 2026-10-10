# CPU-only tests; the production provider operates on a mocked device, never hardware.
param([Parameter(Mandatory)][string]$Root, [string]$Out = "$Root\scratch\device-separation\provider-host",
      [ValidateSet("none","admission")][string]$Mutation="none")
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl = "$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs = "$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = "$Root\scratch\tmp"; $env:TMP = $env:TEMP
$source=[IO.File]::ReadAllText("$repo\driver\kmd\board_provider.c").Replace('#include "bc250kmd.h"','')
$envelope=[IO.File]::ReadAllText("$repo\driver\shim\include\bc250_board_envelope.h")
if($Mutation -eq 'admission') {
    $old='return provider_id == 1u;'
    if($envelope.Split(@($old),[StringSplitOptions]::None).Count -ne 2){throw 'Mutation anchor changed'}
    $envelope=$envelope.Replace($old,'(void)provider_id; return 1;')
}
[IO.File]::WriteAllText("$Out\bc250_board_envelope.h",$envelope)
$template=[IO.File]::ReadAllText("$PSScriptRoot\board_provider_test.c")
[IO.File]::WriteAllText("$Out\actual.c",$template.Replace('/* ACTUAL_PROVIDER */',$source))
& $cl /nologo /TC /W4 /WX /wd4201 /O2 /MT /Gy "/I$Out" "/I$repo\driver\shim\include" "/I$repo\driver\amdgpu-import" "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\board_provider_test.exe" "$Out\actual.c" "$repo\driver\shim\bc250_clock.c" "$repo\driver\shim\bc250_fan.c" "$repo\driver\shim\bc250_hwmon.c" /link /OPT:REF "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Provider host compile failed'}
$output=@(& "$Out\board_provider_test.exe" "$Out\board-caps.bin");$code=$LASTEXITCODE
$output | Set-Content "$Out\checks.txt";$output | Write-Output
Get-FileHash "$repo\driver\kmd\board_provider.c","$repo\driver\kmd\bc250kmd_escape.h","$Out\bc250_board_envelope.h","$repo\driver\shim\bc250_clock.c","$repo\driver\shim\bc250_fan.c","$repo\driver\shim\bc250_hwmon.c","$repo\driver\shim\bc250_board_gpu_points.inc","$repo\driver\shim\bc250_board_fan_profiles.inc","$PSScriptRoot\board_provider_test.c","$PSCommandPath","$Out\board_provider_test.exe","$Out\board-caps.bin" | Select-Object Path,Hash | ConvertTo-Json | Set-Content "$Out\pins.json"
if($Mutation -eq 'none'){if($code -ne 0){throw 'Provider checks failed'}}
elseif($code -ne 1 -or -not ($output -match '^FAIL CHECK')){throw 'Mutation did not fail runtime checks'}
exit 0

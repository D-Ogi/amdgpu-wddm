param([string]$Root='P:\bc-250',[string]$Out='P:\bc-250\scratch\build\health145',[switch]$IgnoreFlush)
$ErrorActionPreference='Stop'
$repo=Join-Path $Root 'bc250-win'
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$source=Get-Content (Join-Path $repo 'driver\kmd\start_health.c') -Raw
[IO.File]::WriteAllText((Join-Path $Out 'start_health_actual.inc'),$source.Replace('#include "bc250kmd.h"',''))
$state=Get-Content (Join-Path $repo 'driver\kmd\start_health.h') -Raw
[IO.File]::WriteAllText((Join-Path $Out 'start_health_state.inc'),$state.Replace('#include "bc250kmd_escape.h"',''))
$guard=Get-Content (Join-Path $repo 'driver\kmd\guard.c') -Raw
$code=$guard.Substring($guard.IndexOf('NTSTATUS GuardConfirmStartDurable('))
# Up to the CU mode's own settings helpers (cumode.c), which this test does not model.
$end=$code.IndexOf('// ---- settings the driver itself owns');if($end -gt 0){$code=$code.Substring(0,$end)}
if($IgnoreFlush){$code=$code.Replace('status=ZwFlushKey(key);','(void)ZwFlushKey(key);')}
[IO.File]::WriteAllText((Join-Path $Out 'confirm_actual.inc'),$code)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'start_health_test.c') -Destination (Join-Path $Out 'test.c') -Force
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /wd4127 /O2 /MT "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\health_test.exe" "$Out\test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'health host build failed'}
& "$Out\health_test.exe"
exit $LASTEXITCODE

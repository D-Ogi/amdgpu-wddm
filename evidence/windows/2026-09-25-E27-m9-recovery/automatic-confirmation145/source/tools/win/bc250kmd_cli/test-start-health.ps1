param([string]$Root='P:\bc-250',[string]$Out='P:\bc-250\scratch\build\start-health-client-tests',[switch]$IdleRead)
$ErrorActionPreference='Stop'
$repo=Join-Path $Root 'bc250-win';$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$source=Get-Content "$repo\tools\win\bc250kmd_cli\bc250kmd_cli.c" -Raw
$first=$source.IndexOf('BC250_CONTROL_API LONG WINAPI Bc250StartHealth(')
$last=$source.IndexOf('static int StartHealth(',$first)
$actual=$source.Substring($first,$last-$first)
if($IdleRead){$actual=$actual.Replace('op==BC250_START_HEALTH_CONFIRM))return status;', '1))return status;')}
$template=Get-Content "$repo\tools\win\bc250kmd_cli\test\start_health_test.c" -Raw
[IO.File]::WriteAllText("$Out\test.c",$template.Replace('// ACTUAL_START_HEALTH',$actual))
Copy-Item "$repo\driver\kmd\bc250kmd_escape.h" "$Out\bc250kmd_escape.h" -Force
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\start_health_test.exe" "$Out\test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'CLI test build failed'}
& "$Out\start_health_test.exe"
if($LASTEXITCODE -ne 0){throw 'Start health client tests failed'}

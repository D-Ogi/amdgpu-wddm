param([string]$Root='P:\BC-250', [string]$Out="$Root\scratch\build\stoprecord")
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
# Reuse the bounded actual-source extraction and registry fault model.
& "$PSScriptRoot\run_start_record.ps1" -Root $Root -Out $Out -Repo $repo
if ($LASTEXITCODE -ne 0) { throw 'start record regression failed' }
$s=Get-Content "$repo\driver\kmd\guard.c" -Raw
$at=$s.IndexOf('// StopDevice can follow')
if ($at -lt 0) { throw 'StopRecord implementation absent' }
[IO.File]::WriteAllText("$Out\stop_actual.inc",$s.Substring($at))
Copy-Item "$PSScriptRoot\start_record_test.c" "$Out\start_record_test.c"
Copy-Item "$PSScriptRoot\stop_record_test.c" "$Out\stop_test.c"
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\stop_test.exe" "$Out\stop_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if ($LASTEXITCODE -ne 0) { throw 'stop record host build failed' }
& "$Out\stop_test.exe"
exit $LASTEXITCODE

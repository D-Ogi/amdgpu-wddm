param([string]$Root='P:\bc-250')
$ErrorActionPreference='Stop'
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$out="$Root\scratch\m9\gpu-residency-probe"
New-Item -ItemType Directory -Force $out | Out-Null
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /W4 /WX /O2 /MT /D_CRT_SECURE_NO_WARNINGS "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/I$Root\bc250-win\driver\contract\third_party" "/I$Root\bc250-win\driver\contract\uapi-shim" "/Fo$out\gpu-residency-probe.obj" "/Fe$out\gpu-residency-probe.exe" "$PSScriptRoot\gpu-residency-probe.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64" gdi32.lib
if($LASTEXITCODE -ne 0){throw "Build failed: $LASTEXITCODE"}
& "$out\gpu-residency-probe.exe" --help
if($LASTEXITCODE -ne 0){throw 'Help smoke test failed'}

param([string]$Root='P:\bc-250')
$ErrorActionPreference='Stop'
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$out=$PSScriptRoot
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /W4 /WX /O2 /MT /D_CRT_SECURE_NO_WARNINGS "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$out\shared-map-probe.obj" "/Fe$out\shared-map-probe.exe" "$out\shared-map-probe.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64" gdi32.lib
if($LASTEXITCODE -ne 0){throw "Build failed: $LASTEXITCODE"}
& "$out\shared-map-probe.exe" --help
if($LASTEXITCODE -ne 0){throw 'Help failed'}
& "$out\shared-map-probe.exe" --selftest
if($LASTEXITCODE -ne 0){throw 'Self-test failed'}
foreach($arguments in @(@(),@('--run'),@('--run','v3'),@('--run','v1','extra'),@('v1'))){
    & "$out\shared-map-probe.exe" @arguments
    if($LASTEXITCODE -ne 2){throw 'Invalid argument accepted'}
}
Get-FileHash "$out\shared-map-probe.exe","$out\shared-map-probe.c","$out\kmtprobe-snapshot.c" -Algorithm SHA256

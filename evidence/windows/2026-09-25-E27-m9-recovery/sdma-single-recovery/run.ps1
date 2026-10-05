param([string]$Source='P:\bc-250\bc250-win',[string]$Out='P:\bc-250\scratch\m9\sdma-single-recovery\focused',[string]$SdmaSource='')
$ErrorActionPreference='Stop'
$base='P:\bc-250\scratch\m9\sdma-single-recovery'
$shim="$Source\driver\shim"
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin=Join-Path $msvc.FullName 'bin\Hostx64\x64'
$sdk='P:\bc-250\toolchain\nuget\microsoft.windows.sdk.cpp\c'
$lib='P:\bc-250\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$inc=@("/I$shim\include","/I$shim","/I$shim\test","/I$Source\driver\amdgpu-import","/I$Source\third_party\linux-amdgpu","/I$Source\third_party\libdrm","/I$sdk\Include\10.0.26100.0\ucrt","/I$sdk\Include\10.0.26100.0\um","/I$sdk\Include\10.0.26100.0\shared","/I$($msvc.FullName)\include")
& "$bin\cl.exe" /nologo /c /TC /W4 /WX /Od /D_CRT_SECURE_NO_WARNINGS @inc "/Fo$Out\test.obj" "$base\test.c"
if($LASTEXITCODE){throw 'fixture compile'}
$objs=(Get-ChildItem "$base\replay\obj-user\*.obj" | Where-Object {$_.Name -ne 'replay_gfx.obj' -and (!$SdmaSource -or $_.Name -ne 'bc250_sdma.obj')}).FullName
if($SdmaSource){
 & "$bin\cl.exe" /nologo /c /TC /W4 /WX /Od /wd4245 /D_CRT_SECURE_NO_WARNINGS @inc "/Fo$Out\bc250_sdma.obj" $SdmaSource
 if($LASTEXITCODE){throw 'mutation compile'}
 $objs+= "$Out\bc250_sdma.obj"
}
& "$bin\link.exe" /nologo /MACHINE:X64 /SUBSYSTEM:CONSOLE "/LIBPATH:$lib\ucrt\x64" "/LIBPATH:$lib\um\x64" "/LIBPATH:$($msvc.FullName)\lib\x64" "/OUT:$Out\test.exe" "$Out\test.obj" @objs
if($LASTEXITCODE){throw 'fixture link'}
& "$Out\test.exe"
exit $LASTEXITCODE

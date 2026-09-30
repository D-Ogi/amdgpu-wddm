param([string]$Root='P:\bc-250',[string]$Out='P:\bc-250\scratch\m9\clock-policy',[string]$Source='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..\..").Path # the tree this script lives in; -Root only locates the toolchain
if(-not $Source){$Source="$repo\driver\shim\bc250_clock.c"}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl="$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$wdk="$Root\toolchain\nuget\microsoft.windows.wdk.x64\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP="$Root\scratch\tmp";$env:TMP=$env:TEMP
$inc=@("/I$repo\driver\shim\include","/I$repo\driver\amdgpu-import")
& python "$PSScriptRoot\extract_clock.py" --check
if($LASTEXITCODE -ne 0){throw 'AMD source drift'}
& $cl /nologo /TC /W4 /WX /O2 /MT @inc "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\clock_test.exe" $Source "$PSScriptRoot\clock_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Host build failed'}
& $cl /nologo /c /TC /kernel /GS- /Zp8 /W4 /WX /O2 /D_AMD64_ /DAMD64 /D_WIN64 @inc "/I$wdk\Include\10.0.26100.0\km\crt" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\clock-kernel.obj" $Source
if($LASTEXITCODE -ne 0){throw 'Kernel compile failed'}
& "$Out\clock_test.exe"
exit $LASTEXITCODE

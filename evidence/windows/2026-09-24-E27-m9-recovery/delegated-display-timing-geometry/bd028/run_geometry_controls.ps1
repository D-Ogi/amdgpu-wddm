$ErrorActionPreference='Stop'
$root='P:\bc-250'
$out="$root\scratch\build\bd028\geometry"
$src="$root\scratch\m9\bd028"
$hostsrc="$root\bc250-win\tools\wddm_contract_check\host"
$kmd="$root\bc250-win\driver\kmd"
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl="$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$wdk="$root\toolchain\nuget\microsoft.windows.wdk.x64\c"
$sdk="$root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $out | Out-Null
$env:TEMP="$root\scratch\tmp"; $env:TMP=$env:TEMP
$flags=@('/nologo','/c','/TC','/GS-','/W4','/WX','/O2','/Zp8','/wd4201','/wd4214','/D_AMD64_','/DAMD64','/D_WIN64','/DWINNT=1','/DNTDDI_VERSION=0x0A00000C','/D_WIN32_WINNT=0x0A00','/DNDEBUG',"/I$hostsrc","/I$kmd","/I$wdk\Include\10.0.26100.0\km","/I$wdk\Include\10.0.26100.0\km\crt","/I$wdk\Include\10.0.26100.0\shared","/I$sdk\Include\10.0.26100.0\shared","/I$sdk\Include\10.0.26100.0\um","/Fo$out\")
& $cl @flags "$src\geometry_actual.c" "$hostsrc\qai_bridge.c"
if($LASTEXITCODE){throw 'WDK bridge compile failed'}
$usr=@('/nologo','/c','/TC','/W4','/WX','/O2','/MT','/D_CRT_SECURE_NO_WARNINGS',"/I$hostsrc","/I$($msvc.FullName)\include","/I$sdk\Include\10.0.26100.0\ucrt","/I$sdk\Include\10.0.26100.0\shared","/I$sdk\Include\10.0.26100.0\um","/Fo$out\")
& $cl @usr "$src\geometry_main.c" "$hostsrc\qai_test.c"
if($LASTEXITCODE){throw 'User compile failed'}
& "$($msvc.FullName)\bin\Hostx64\x64\link.exe" /nologo /MACHINE:X64 /SUBSYSTEM:CONSOLE "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64" "/OUT:$out\geometry_test.exe" "$out\geometry_actual.obj" "$out\geometry_main.obj"
if($LASTEXITCODE){throw 'Link failed'}
& "$out\geometry_test.exe"
exit $LASTEXITCODE

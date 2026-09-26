$ErrorActionPreference='Stop'
$root='P:\bc-250';$repo=Join-Path $root 'bc250-win';$out=Join-Path $root 'scratch\build\guard144\wdk'
New-Item -ItemType Directory -Force $out | Out-Null
$env:TEMP=Join-Path $root 'scratch\tmp';$env:TMP=$env:TEMP
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c'
$sdk=Join-Path $root 'toolchain\nuget\microsoft.windows.sdk.cpp\c'
$flags=@('/nologo','/c','/kernel','/GS-','/W4','/WX','/O2','/Zi','/Zp8','/GF','/Gy','/wd4201','/wd4214','/D_AMD64_','/DAMD64','/D_WIN64','/DWINNT=1','/DNTDDI_VERSION=0x0A00000C','/D_WIN32_WINNT=0x0A00','/DNDEBUG',"/I$wdk\Include\10.0.26100.0\km","/I$wdk\Include\10.0.26100.0\km\crt","/I$wdk\Include\10.0.26100.0\shared","/I$sdk\Include\10.0.26100.0\shared","/I$sdk\Include\10.0.26100.0\um","/Fo$out\","/Fd$out\cl.pdb","/I$repo\driver\shim\include","/I$repo\driver\amdgpu-import","/I$repo\third_party\linux-amdgpu","/I$repo\third_party\libdrm",'/DBC250_SHIM_KERNEL')
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" @flags "$repo\driver\kmd\guard.c" "$repo\driver\kmd\wddm.c" "$repo\driver\kmd\pnp.c"
if($LASTEXITCODE -ne 0){throw 'WDK compilation failed'}

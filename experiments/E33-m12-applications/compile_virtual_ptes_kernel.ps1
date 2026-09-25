param([string]$Out='P:\bc-250\scratch\m12\virtual-pte-kernel',
      [string]$Kits='P:\bc-250\toolchain\nuget',
      [string]$KitVersion='10.0.26100.0')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$shim=Join-Path $repo 'driver\shim'
$wdk=Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$vswhere=Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products * -property installationPath
$msvc=Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin=Join-Path $msvc.FullName 'bin\Hostx64\x64'
New-Item -ItemType Directory -Force $Out | Out-Null
$env:INCLUDE='';$env:LIB=''
$flags=@('/nologo','/c','/TC','/kernel','/GS-','/W4','/WX','/O2','/Zp8',
    '/wd4201','/wd4214','/D_AMD64_','/DAMD64','/D_WIN64','/DWINNT=1',
    '/DNTDDI_VERSION=0x0A00000C','/D_WIN32_WINNT=0x0A00','/DNDEBUG','/DBC250_SHIM_KERNEL',
    "/I$wdk\Include\$KitVersion\km","/I$wdk\Include\$KitVersion\km\crt",
    "/I$wdk\Include\$KitVersion\shared","/I$sdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\um","/I$shim\include",
    "/I$repo\driver\amdgpu-import","/I$repo\third_party\linux-amdgpu",
    "/Fo$Out\bc250_sdma_virtual_ptes.obj")
& (Join-Path $bin 'cl.exe') @flags "$shim\bc250_sdma_virtual_ptes.c"
if($LASTEXITCODE -ne 0){throw 'Kernel compile failed'}
'WDK26100 virtual PTE builder compile PASS (not linked or deployed)'

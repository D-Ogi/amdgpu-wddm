# CPU-only wave reader tests plus compilation with the miniport's kernel flags.
param(
    [Parameter(Mandatory=$true)][string]$Out,
    [Parameter(Mandatory=$true)][string]$Kits,
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$shim = Join-Path $repo 'driver\shim'
$imports = Join-Path $repo 'driver\amdgpu-import'
$amdhdr = Join-Path $repo 'third_party\linux-amdgpu'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$objUser = Join-Path $Out 'obj-user'
$objKern = Join-Path $Out 'obj-kernel'
New-Item -ItemType Directory -Force $Out, $objUser, $objKern | Out-Null
$env:TEMP = $Out
$env:TMP = $Out
$env:INCLUDE = ''
$env:LIB = ''
function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "$exe failed ($LASTEXITCODE)" }
}
$sources = @((Join-Path $shim 'bc250_wave.c'), (Join-Path $shim 'bc250_cu_mode.c'),
    (Join-Path $shim 'shim.c'), (Join-Path $shim 'test\wave_test.c'))
$objects = @('bc250_wave','bc250_cu_mode','shim','wave_test','cyan_skillfish_reg_init') |
    ForEach-Object { Join-Path $objUser "$_.obj" }
$inc = @("/I$shim\include", "/I$imports", "/I$amdhdr",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo','/c','/TC','/W4','/WX','/Od','/Zi') +
    $inc + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $sources)
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo','/c','/TC','/W4','/WX','/wd4244','/Od') +
    $inc + @("/Fo$objUser\", (Join-Path $imports 'cyan_skillfish_reg_init.c')))
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo','/c','/kernel','/GS-','/W4','/WX','/O2','/Zp8','/TC',
    '/D_AMD64_','/DAMD64','/D_WIN64','/DBC250_SHIM_KERNEL','/wd4201','/wd4214',
    "/I$shim\include", "/I$imports", "/I$amdhdr", "/I$wdk\Include\$KitVersion\km",
    "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$objKern\", (Join-Path $shim 'bc250_wave.c')))
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo','/DEBUG','/MACHINE:X64','/SUBSYSTEM:CONSOLE',
    "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64",
    "/OUT:$Out\wave_test.exe", "/PDB:$Out\wave_test.pdb") + $objects)
& (Join-Path $Out 'wave_test.exe')
exit $LASTEXITCODE

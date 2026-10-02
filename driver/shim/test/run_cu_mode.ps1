# Builds and runs the CU mode host test (cu_mode_test.c): the setting and boot guard, the register
# values, the CU bitmap of the caps, and the register sequence against a banked model, with every
# access checked against the miniport's Gfx allow table. Also compile-checks bc250_cu_mode.c with the
# kernel flags. Host-side only: nothing here touches the lab machine.
#
#   pwsh driver\shim\test\run_cu_mode.ps1
#   pwsh driver\shim\test\run_cu_mode.ps1 -Out P:\BC-250\scratch\build\cu-mode
#
# Everything is written under -Out, never into the repository and never onto drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\build\cu-mode',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
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
Remove-Item "$objUser\*.obj", "$objKern\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''

# shim.c for the amdgpu_sriov_* stubs soc15_common.h's register macros name; the test declares its
# own register file and backend, no backend_mem.c.
$plainSources = @('bc250_cu_mode.c', 'shim.c') | ForEach-Object { Join-Path $shim $_ }
$testSources = @((Join-Path $shim 'test\cu_mode_test.c'))
$importSources = @((Join-Path $imports 'cyan_skillfish_reg_init.c'))
$importWarn = @('/wd4244', '/wd4701')

$incUser = @("/I$shim\include", "/I$shim", "/I$imports", "/I$amdhdr",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode)'
$userFlags = @('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS')
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $plainSources + $testSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags + $importWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $importSources)

Write-Host 'compile (kernel flags, bc250_cu_mode.c)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zp8', '/TC',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DBC250_SHIM_KERNEL', '/wd4201', '/wd4214',
    "/I$shim\include", "/I$imports", "/I$amdhdr", "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt",
    "/I$wdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\shared", "/Fo$objKern\") + @((Join-Path $shim 'bc250_cu_mode.c')))

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64",
        "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\cu_mode_test.exe", "/PDB:$Out\cu_mode_test.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

Write-Host 'run'
& "$Out\cu_mode_test.exe"
$code = $LASTEXITCODE
Write-Host "cu_mode_test.exe exit code $code"
exit $code

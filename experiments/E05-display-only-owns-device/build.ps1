# Builds Microsoft's KMDOD sample (kernel-mode display-only WDDM driver) UNMODIFIED from a checkout outside this
# repository, with our INF that matches only PCI\VEN_1002&DEV_13FE, and test-signs the package.
# The sample is MS-PL licensed and is an instrument of this experiment, not part of our driver.
#
#   pwsh experiments\E05-display-only-owns-device\build.ps1 -Sample P:\BC-250\ref\Windows-driver-samples\video\KMDOD `
#        -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\e05-kmdod

param(
    [Parameter(Mandatory)][string]$Sample,
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [string]$CertSubject = 'CN=BC-250 lab test signing'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$pkg = Join-Path $Out 'package'
New-Item -ItemType Directory -Force $pkg, (Join-Path $Out 'obj') | Out-Null

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.(cxx|c)$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''; $env:LIB = ''
$sources = 'bdd.cxx', 'bdd_ddi.cxx', 'bdd_dmm.cxx', 'bdd_util.cxx', 'bltfuncs.cxx', 'blthw.cxx', 'memory.cxx' | ForEach-Object { Join-Path $Sample $_ }
Write-Host 'compile'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/kernel', '/GS-', '/W3', '/O2', '/Zi', '/Zp8', '/GF', '/Gy', '/GR-', '/EHs-c-',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\um", "/Fo$Out\obj\", "/Fd$Out\obj\cl.pdb") + $sources)
Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,10.00', '/ENTRY:DriverEntry', '/NODEFAULTLIB', '/RELEASE',
    '/DEBUG', '/OPT:REF', '/OPT:ICF', '/MACHINE:X64', "/LIBPATH:$wdk\Lib\$KitVersion\km\x64",
    'displib.lib', 'ntoskrnl.lib', 'hal.lib', 'bufferoverflowfastfailk.lib', 'libcntpr.lib',
    "/OUT:$pkg\bc250kmdod.sys", "/PDB:$Out\bc250kmdod.pdb") + (Get-ChildItem "$Out\obj\*.obj").FullName)

Copy-Item (Join-Path $here 'bc250kmdod.inf') $pkg -Force
Write-Host 'catalog'
Invoke-Tool "$wdk\bin\$KitVersion\x86\Inf2Cat.exe" @("/driver:$pkg", '/os:10_X64', '/uselocaltime')

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
if (-not $cert) { throw "test certificate '$CertSubject' not found: run tools\win\bc250rd\build.ps1 once, it creates it" }
Export-Certificate -Cert $cert -FilePath "$pkg\bc250-lab-test.cer" | Out-Null
$signtool = Join-Path $sdk "bin\$KitVersion\x64\signtool.exe"
Write-Host 'sign'
foreach ($f in 'bc250kmdod.sys', 'bc250kmdod.cat') { Invoke-Tool $signtool @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint, "$pkg\$f") }
Get-ChildItem $pkg -File | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }

# Builds and test-signs tpmserv.sys, the kernel half of the TPM servicer (KMDF 1.33 lower filter).
#
#   pwsh driver\tpm\filter\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\tpmserv
#
# Output: <Out>\package\{tpmserv.sys, tpmserv.inf, tpmserv.cat, bc250-lab-test.cer}
# The kernel compiler is the WDK's MSVC, as for bc250kmd (driver\kmd\build.ps1): the clang-cl rule is for user mode.
param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [string]$CertSubject = 'CN=BC-250 lab test signing',
    [switch]$CompileOnly
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$tpm = Split-Path $here
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$pkg = Join-Path $Out 'package'
$obj = Join-Path $Out 'obj'
# A fresh output: cl appends to an existing PDB, and /Brepro then hashes a different layout.
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $pkg, $obj | Out-Null

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''; $env:LIB = ''
$kmdf = '1.33'
$clFlags = @('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/we4013', '/we4020', '/we4024', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/Brepro', '/wd4201', '/wd4214', '/TC',
    '/wd4324',                      # wdfrequest.h pads its own aligned structures
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    '/DKMDF_VERSION_MAJOR=1', "/DKMDF_VERSION_MINOR=$($kmdf.Split('.')[1])",
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$wdk\Include\wdf\kmdf\$kmdf", "/I$sdk\Include\$KitVersion\shared",
    "/Fo$obj\", "/Fd$obj\cl.pdb")
Write-Host 'compile'
Invoke-Tool (Join-Path $bin 'cl.exe') ($clFlags + @((Join-Path $here 'tpmserv.c'), (Join-Path $tpm 'crb.c'), (Join-Path $tpm 'handoff.c'), (Join-Path (Split-Path $tpm) 'common\board\board_identity.c')))
Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') @('/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,10.00', '/ENTRY:FxDriverEntry', '/NODEFAULTLIB',
    '/RELEASE', '/DEBUG', '/OPT:REF', '/OPT:ICF', '/MACHINE:X64', '/Brepro', '/PDBALTPATH:%_PDB%',
    "/LIBPATH:$wdk\Lib\$KitVersion\km\x64", "/LIBPATH:$wdk\Lib\wdf\kmdf\x64\$kmdf",
    'wdfdriverentry.lib', 'wdfldr.lib', 'aux_klib.lib', 'wdmsec.lib', 'ntoskrnl.lib', 'hal.lib',
    'bufferoverflowfastfailk.lib', 'libcntpr.lib',
    "/OUT:$pkg\tpmserv.sys", "/PDB:$Out\tpmserv.pdb", "$obj\tpmserv.obj", "$obj\crb.obj", "$obj\handoff.obj", "$obj\board_identity.obj")
'  {0}  tpmserv.sys (unsigned)' -f (Get-FileHash "$pkg\tpmserv.sys").Hash
if ($CompileOnly) { return }

Copy-Item (Join-Path $here 'tpmserv.inf') $pkg -Force
Write-Host 'catalog'
Invoke-Tool "$wdk\bin\$KitVersion\x86\Inf2Cat.exe" @("/driver:$pkg", '/os:10_X64')
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
if (-not $cert) { throw "test certificate '$CertSubject' not found" }
Export-Certificate -Cert $cert -FilePath "$pkg\bc250-lab-test.cer" | Out-Null
$signtool = Join-Path $sdk "bin\$KitVersion\x64\signtool.exe"
Write-Host 'sign'
foreach ($f in 'tpmserv.sys', 'tpmserv.cat') { Invoke-Tool $signtool @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint, "$pkg\$f") }
Get-ChildItem $pkg -File | ForEach-Object { '{0,9}  {1}  {2}' -f $_.Length, (Get-FileHash $_.FullName).Hash.Substring(0, 8), $_.Name }

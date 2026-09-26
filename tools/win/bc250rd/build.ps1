# Builds bc250rd.sys and bc250rd_cli.exe without a WDK installation: kernel headers and libraries come
# from the WDK/SDK NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio.
# The driver is test-signed with a self-signed certificate (the target runs with testsigning on).
#
#   pwsh tools\win\bc250rd\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\bc250rd

param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$ControlDll,
    [string]$KitVersion = '10.0.26100.0',
    [string]$CertSubject = 'CN=BC-250 lab test signing'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$controlLib = Join-Path (Split-Path -Parent $ControlDll) 'bc250control.lib'
if (-not (Test-Path -LiteralPath $ControlDll) -or -not (Test-Path -LiteralPath $controlLib)) { throw 'Build bc250kmd_cli control DLL/import library first' }
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$cl = Join-Path $bin 'cl.exe'; $link = Join-Path $bin 'link.exe'
New-Item -ItemType Directory -Force $Out | Out-Null

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

# ---- driver -------------------------------------------------------------------------------------------------
$env:INCLUDE = ''; $env:LIB = ''
Write-Host 'driver: compile'
Invoke-Tool $cl @('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/Gz', '/GF', '/Gy',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$sdk\Include\$KitVersion\shared",
    "/Fo$Out\bc250rd.obj", "/Fd$Out\bc250rd_cl.pdb", (Join-Path $here 'driver\bc250rd.c'))
Write-Host 'driver: link'
Invoke-Tool $link @('/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,10.00', '/ENTRY:DriverEntry', '/NODEFAULTLIB', '/RELEASE',
    '/DEBUG', '/OPT:REF', '/OPT:ICF', '/MACHINE:X64', "/LIBPATH:$wdk\Lib\$KitVersion\km\x64",
    'ntoskrnl.lib', 'hal.lib', 'wdmsec.lib', 'bufferoverflowfastfailk.lib',
    "/OUT:$Out\bc250rd.sys", "/PDB:$Out\bc250rd.pdb", "$Out\bc250rd.obj")

# ---- command line tool --------------------------------------------------------------------------------------
Write-Host 'cli: compile and link'
$crt = Join-Path $msvc.FullName 'lib\x64'
Invoke-Tool $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\bc250rd_cli.obj", "/Fe$Out\bc250rd_cli.exe",
    (Join-Path $here 'app\bc250rd_cli.c'), $controlLib, '/link', "/LIBPATH:$crt",
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64")

# ---- test signing -------------------------------------------------------------------------------------------
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -CertStoreLocation Cert:\CurrentUser\My `
        -KeyUsage DigitalSignature -NotAfter (Get-Date).AddYears(5)
    Write-Host "created test certificate $($cert.Thumbprint)"
}
Export-Certificate -Cert $cert -FilePath "$Out\bc250-lab-test.cer" | Out-Null
$signtool = Join-Path $sdk "bin\$KitVersion\x64\signtool.exe"
if (-not (Test-Path $signtool)) { $signtool = Join-Path $sdkLib "bin\$KitVersion\x64\signtool.exe" }
if (-not (Test-Path $signtool)) { $signtool = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\$KitVersion\x64\signtool.exe" }
Write-Host 'driver: sign'
Invoke-Tool $signtool @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint, "$Out\bc250rd.sys")

Copy-Item -LiteralPath $ControlDll -Destination (Join-Path $Out 'bc250control.dll') -Force
Copy-Item (Join-Path $here 'reglist.txt') $Out -Force
Get-ChildItem $Out -File | Where-Object Extension -in '.sys', '.exe', '.cer', '.txt' | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }

# Builds and test-signs bc250kmd (the M3 display-only miniport) without Visual Studio project files.
#
#   pwsh driver\kmd\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250kmd
#
# Output: <Out>\package\{bc250kmd.sys, bc250kmd.inf, bc250kmd.cat, bc250-lab-test.cer}

param(
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
$obj = Join-Path $Out 'obj'
if (Test-Path $obj) { Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $pkg, $obj | Out-Null

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''; $env:LIB = ''
$sources = (Get-ChildItem (Join-Path $here '*.c')).FullName
# M4: AMD's imported hub code and the shim it compiles against (ADR 0002). Same flags; the imports get the two
# warning disables documented in driver\shim\README.md, from the build line, never by editing them.
$repo = Split-Path (Split-Path $here)
$shimInc = @("/I$repo\driver\shim\include", "/I$repo\driver\amdgpu-import", "/I$repo\third_party\linux-amdgpu", '/DBC250_SHIM_KERNEL')
$shimSources = @("$repo\driver\shim\shim.c", "$repo\driver\shim\bc250_gmc.c")
$importSources = (Get-ChildItem "$repo\driver\amdgpu-import\*.c").FullName
Write-Host 'compile'
$clFlags = @('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/wd4201', '/wd4214',           # nameless unions and bit fields in the WDK's own headers
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\um",
    "/Fo$obj\", "/Fd$obj\cl.pdb")
Invoke-Tool (Join-Path $bin 'cl.exe') ($clFlags + $shimInc + $sources + $shimSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($clFlags + $shimInc + @('/TC', '/wd4244', '/wd4701') + $importSources)
Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,10.00', '/ENTRY:DriverEntry', '/NODEFAULTLIB', '/RELEASE',
    '/DEBUG', '/OPT:REF', '/OPT:ICF', '/MACHINE:X64', "/LIBPATH:$wdk\Lib\$KitVersion\km\x64",
    'displib.lib', 'ntoskrnl.lib', 'hal.lib', 'bufferoverflowfastfailk.lib', 'libcntpr.lib', 'ntstrsafe.lib',
    "/OUT:$pkg\bc250kmd.sys", "/PDB:$Out\bc250kmd.pdb") + (Get-ChildItem "$obj\*.obj").FullName)

Copy-Item (Join-Path $here 'bc250kmd.inf') $pkg -Force
Write-Host 'catalog'
Invoke-Tool "$wdk\bin\$KitVersion\x86\Inf2Cat.exe" @("/driver:$pkg", '/os:10_X64', '/uselocaltime')

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
if (-not $cert) { throw "test certificate '$CertSubject' not found: run tools\win\bc250rd\build.ps1 once, it creates it" }
Export-Certificate -Cert $cert -FilePath "$pkg\bc250-lab-test.cer" | Out-Null
$signtool = Join-Path $sdk "bin\$KitVersion\x64\signtool.exe"
Write-Host 'sign'
foreach ($f in 'bc250kmd.sys', 'bc250kmd.cat') { Invoke-Tool $signtool @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint, "$pkg\$f") }
Get-ChildItem $pkg -File | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }

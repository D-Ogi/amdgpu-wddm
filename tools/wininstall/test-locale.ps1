# Compiles and runs fake WLAN API tests only. No network/profile query or Windows configuration change.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force $Out | Out-Null
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$exe = Join-Path $Out 'wlan-locale-tests.exe'
& $csc /nologo /target:exe /platform:x64 /warnaserror+ "/out:$exe" (Join-Path $here 'WlanProfiles.cs') (Join-Path $here 'test\WlanProfilesTests.cs')
if ($LASTEXITCODE -ne 0) { throw 'WLAN locale test compile failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'WLAN locale tests failed' }

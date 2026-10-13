# Host-only fault injection of the actual psp.c reader. No firmware or OS changes.
param([Parameter(Mandatory=$true)][string]$Out,
      [string]$Kits = "$env:BC250_ROOT\toolchain\nuget",
      [string]$KitVersion = '10.0.26100.0', [switch]$AcceptShort)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
$argsGen = @('-B', "$PSScriptRoot\generate_firmware_io_test.py", $repo, "$Out\firmware_io_actual.inc")
if ($AcceptShort) { $argsGen += '--accept-short' }
& python @argsGen
if ($LASTEXITCODE) { throw 'firmware I/O extraction failed' }
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$lib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$env:INCLUDE = "$($msvc.FullName)\include;$sdk\Include\$KitVersion\ucrt"
$env:LIB = "$($msvc.FullName)\lib\x64;$lib\ucrt\x64;$lib\um\x64"
& "$bin\cl.exe" /nologo /TC /W4 /WX /Od /D_CRT_SECURE_NO_WARNINGS "/I$Out" "/Fo$Out\firmware_io.obj" "/Fe$Out\firmware_io.exe" "$PSScriptRoot\firmware_io_test.c"
if ($LASTEXITCODE) { throw 'firmware I/O compile failed' }
& "$Out\firmware_io.exe"
exit $LASTEXITCODE

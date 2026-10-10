# CPU-only tests of the production KMD wave-access wrapper. No hardware access.
param(
    [Parameter(Mandatory=$true)][string]$Out,
    [Parameter(Mandatory=$true)][string]$Kits,
    [string]$KitVersion = '10.0.26100.0',
    [string]$Source = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Source) { $Source = Join-Path $repo 'driver\kmd\gfx.c' }
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$libs = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = $Out
$env:TMP = $Out
$env:INCLUDE = ''
$env:LIB = ''
& python -B "$PSScriptRoot\wave_access_extract.py" --source $Source --display "$repo\driver\kmd\display.c" --out "$Out\wave_access_actual.c"
if ($LASTEXITCODE -ne 0) { throw 'Source extraction failed' }
& $cl /nologo /TC /std:c11 /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$repo\third_party\linux-amdgpu" "/I$($msvc.FullName)\include" "/I$sdk\Include\$KitVersion\ucrt" "/Fo$Out\" "/Fe$Out\wave_access_test.exe" "$Out\wave_access_actual.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if ($LASTEXITCODE -ne 0) { throw 'Wave access host build failed' }
& "$Out\wave_access_test.exe"
exit $LASTEXITCODE

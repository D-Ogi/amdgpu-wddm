# Builds cumode.exe (the CU mode tool, docs/design/cu-mode.md) without a WDK or SDK installation: headers and
# import libraries from the SDK NuGet packages under -Kits, the compiler from the installed Visual Studio. Same
# flow as tools\win\bc250kmd_cli\build.ps1. Builds only; it never runs the tool.
#
#   pwsh tools\win\cumode\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\cumode

param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('x64','x86')][string]$Arch = 'x64'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
& "$here\test-readonly-state.ps1"
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName "bin\Hostx64\$Arch\cl.exe"
New-Item -ItemType Directory -Force $Out | Out-Null

$env:INCLUDE = ''; $env:LIB = ''
$repo = (Resolve-Path "$here\..\..\..").Path
$stateFlags = @('/O2','/MT',"/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt",
    "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared")
$stateObjects = @(& "$repo\tools\state\build-objects.ps1" -Compiler $cl -Out "$Out\state" -Flags $stateFlags)
& $cl /nologo /W4 /WX /EHsc /std:c++17 @stateFlags "/Fo$Out\state-test.obj" "/Fe$Out\state-test.exe" "$here\state-test.cpp" `
    /link "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")" "/LIBPATH:$sdkLib\ucrt\$Arch" "/LIBPATH:$sdkLib\um\$Arch"
if ($LASTEXITCODE -ne 0) { throw 'CU state test build failed' }
& "$Out\state-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'CU state test failed' }
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\cumode.obj", "/Fe$Out\cumode.exe",
    (Join-Path $here 'cumode.cpp')) @stateObjects @('/link', "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")",
    "/LIBPATH:$sdkLib\ucrt\$Arch", "/LIBPATH:$sdkLib\um\$Arch",
    'user32.lib', 'gdi32.lib', 'setupapi.lib', 'advapi32.lib', 'onecore.lib', 'uuid.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

$image=[IO.File]::ReadAllBytes("$Out\cumode.exe")
$pe=[BitConverter]::ToInt32($image,0x3c)
$machine=[BitConverter]::ToUInt16($image,$pe+4)
$expected=if($Arch -eq 'x64'){0x8664}else{0x14c}
if($machine -ne $expected){throw "cumode.exe architecture mismatch: $machine"}
Get-Item "$Out\cumode.exe" | ForEach-Object { '{0,9}  {1}  {2}' -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.Name }

# Host test for driver\kmd\scanout_admit.h: which allocation SetVidPnSourceAddress may scan out
# (M15.14). The test includes the production header, so there is nothing to extract and no fixture to
# drift; dcn_translate.c is linked for DcnSurfaceBytes and DcnPrimaryPitch.
#
#   pwsh driver\kmd\test\run_scanout_admit.ps1
#   pwsh driver\kmd\test\run_scanout_admit.ps1 -Out $env:BC250_ROOT\scratch\scanout-admit
#
# The binary goes under -Out. Nothing is written to drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\scanout-admit",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$repo = Split-Path (Split-Path $kmd)
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'

$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null
Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue
$env:TEMP = "$Root\scratch\tmp"; $env:TMP = $env:TEMP
$env:INCLUDE = ''; $env:LIB = ''

& $cl @('/nologo', '/TC', '/W4', '/WX', '/O2', '/MT',
    "/I$kmd", "/I$repo\driver\contract", "/I$($msvc.FullName)\include",
    "/I$sdk\Include\$KitVersion\ucrt", "/Fo$obj\", "/Fe$Out\scanout_admit_test.exe",
    (Join-Path $here 'scanout_admit_test.c'), (Join-Path $kmd 'dcn_translate.c'),
    '/link', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64")
if ($LASTEXITCODE -ne 0) { throw 'Host build failed' }

& "$Out\scanout_admit_test.exe"
exit $LASTEXITCODE

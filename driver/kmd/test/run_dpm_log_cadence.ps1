param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\dpm-log-cadence",[switch]$NoIdleCadence,[switch]$IdleSixtySeconds)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
# The header under test is copied next to the build, so that a negative control changes the copy and never the
# tree (/I$Out comes first, so the copy wins). There are two controls, because the fix has two halves.
# -NoIdleCadence is the mechanism: a driver that keeps the 5 s period at the idle point as well, which is what
# the driver did before the b23 fix of BD-097. -IdleSixtySeconds is the number: the 60 s cadence that b23 gave
# and the lab measured as 3000 s of ring, against the 3600 s the plan asks for. Each control must fail the
# test, and the cases it fails are the ones that state the cadence.
$h=Get-Content "$repo\driver\kmd\dpm_log_cadence.h" -Raw
if($NoIdleCadence){
    $live='    if (!Idle || IdleLogMs <= (unsigned long)BC250_DPM_LOG_MS) return (unsigned long)BC250_DPM_LOG_MS;'
    if(!$h.Contains($live)){throw 'negative control: the period clause moved'}
    # Every period the driver accepts gives the 5 s cadence, which is "no idle cadence at all". Both parameters
    # stay in the expression, so the control fails the test's cases and not the compiler's /W4 /WX.
    $h=$h.Replace($live,'    if (!Idle || IdleLogMs <= (unsigned long)BC250_DPM_IDLE_LOG_MAX_MS) return (unsigned long)BC250_DPM_LOG_MS;')
}
if($IdleSixtySeconds){
    $live='#define BC250_DPM_IDLE_LOG_MS 120000u'
    if(!$h.Contains($live)){throw 'negative control: the idle period constant moved'}
    $h=$h.Replace($live,'#define BC250_DPM_IDLE_LOG_MS 60000u')
}
[IO.File]::WriteAllText((Join-Path $Out 'dpm_log_cadence.h'),$h)
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4127 /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\dpm_log_cadence_test.exe" "$PSScriptRoot\dpm_log_cadence_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'dpm log cadence host build failed'}
& "$Out\dpm_log_cadence_test.exe"
exit $LASTEXITCODE

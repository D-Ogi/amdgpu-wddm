param([string]$Root='P:\bc-250',[string]$Out='P:\bc-250\scratch\build\bd018',[switch]$AssumePhaseHz,[switch]$OldDescribe60)
$ErrorActionPreference='Stop'
$repo=Join-Path $Root 'bc250-win'
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$source=Get-Content (Join-Path $repo 'driver\kmd\display.c') -Raw
$first=$source.IndexOf('NTSTATUS DisplayPrepareInheritedTiming(')
$last=$source.IndexOf('static NTSTATUS OfferSourceMode(',$first)
$code=$source.Substring($first,$last-$first)
[IO.File]::WriteAllText((Join-Path $Out 'display_timing_actual.inc'),$code)
$wddm=Get-Content (Join-Path $repo 'driver\kmd\wddm.c') -Raw
$first=$wddm.IndexOf('static NTSTATUS Bc250WddmDescribeAllocation(')
$last=$wddm.IndexOf('static DXGKDDI_OPENALLOCATIONINFO',$first)
$describe=$wddm.Substring($first,$last-$first)
if($OldDescribe60){$describe=$describe.Replace('pDescribeAllocation->RefreshRate = device->InheritedSignal.VSyncFreq;', 'pDescribeAllocation->RefreshRate.Numerator=60000; pDescribeAllocation->RefreshRate.Denominator=1000;')}
[IO.File]::WriteAllText((Join-Path $Out 'describe_timing_actual.inc'),$describe)
$test=Get-Content (Join-Path $PSScriptRoot 'display_timing_test.c') -Raw
$helper=Get-Content (Join-Path $repo 'driver\kmd\display_timing.h') -Raw
if($AssumePhaseHz){$helper=$helper.Replace('pixel=((ULONGLONG)Values[TimingPhase]*referenceHz+Values[TimingModulo]/2)/Values[TimingModulo];','pixel=Values[TimingPhase];')}
[IO.File]::WriteAllText((Join-Path $Out 'display_timing_under_test.h'),$helper)
[IO.File]::WriteAllText((Join-Path $Out 'test.c'),$test.Replace('#include "../display_timing.h"','#include "display_timing_under_test.h"'))
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c'
$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
$cl=Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
& $cl /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$repo\third_party\linux-amdgpu" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\timing_test.exe" "$Out\test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'host build failed'}
& "$Out\timing_test.exe"
exit $LASTEXITCODE

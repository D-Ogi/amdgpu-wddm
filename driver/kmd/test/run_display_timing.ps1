param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\bd018",[switch]$AssumePhaseHz,[switch]$OldDescribe60,[switch]$OldPixelRateFixed)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$source=Get-Content (Join-Path $repo 'driver\kmd\display.c') -Raw
$first=$source.IndexOf('NTSTATUS DisplayPrepareInheritedTiming(')
$last=$source.IndexOf('static NTSTATUS OfferSourceMode(',$first)
$code=$source.Substring($first,$last-$first)
[IO.File]::WriteAllText((Join-Path $Out 'display_timing_actual.inc'),$code)
$vidpn=''
foreach($signature in @('static ULONG ScalingFromPath(','static BOOLEAN ScalingPinned(','static const char* DisplayInheritedModeRefusal(','static BOOLEAN DisplayInheritedModeMatches(','NTSTATUS Bc250UpdateActiveVidPnPresentPath(')){
    $first=$source.IndexOf($signature)
    $last=$source.IndexOf("`n}",$first)+3
    if($first -lt 0 -or $last -lt $first){throw "Missing body: $signature"}
    $vidpn+=$source.Substring($first,$last-$first)+"`n"
}
[IO.File]::WriteAllText((Join-Path $Out 'inherited_vidpn_actual.inc'),$vidpn)
$negotiation=''
foreach($signature in @('static NTSTATUS OfferSourceMode(','static NTSTATUS OfferTargetMode(','static NTSTATUS SourceModeIsPinned(','static NTSTATUS TargetModeIsPinned(')){
    $first=$source.IndexOf($signature);$last=$source.IndexOf("`n}",$first)+3
    if($first -lt 0 -or $last -lt $first){throw "Missing body: $signature"}
    $negotiation+=$source.Substring($first,$last-$first)+"`n"
}
[IO.File]::WriteAllText((Join-Path $Out 'inherited_negotiation_actual.inc'),$negotiation)
$ddi=''
foreach($signature in @('NTSTATUS Bc250IsSupportedVidPn(','NTSTATUS Bc250EnumVidPnCofuncModality(')){
    $first=$source.IndexOf($signature);$last=$source.IndexOf("`n}",$first)+3
    if($first -lt 0 -or $last -lt $first){throw "Missing body: $signature"}
    $ddi+=$source.Substring($first,$last-$first)+"`n"
}
[IO.File]::WriteAllText((Join-Path $Out 'inherited_ddi_actual.inc'),$ddi)
$first=$source.IndexOf('static NTSTATUS CommitVidPnCore(')
$last=$source.IndexOf("`n}",$first)+3
if($first -lt 0 -or $last -lt $first){throw 'CommitVidPnCore not found'}
[IO.File]::WriteAllText((Join-Path $Out 'inherited_vidpn_commit_actual.inc'),$source.Substring($first,$last-$first))
$modeset=Get-Content (Join-Path $repo 'driver\kmd\modeset.c') -Raw
$first=$modeset.IndexOf('NTSTATUS ModesetPrepareInherited(')
$last=$modeset.IndexOf('static ULONG InheritedScalingDefault(',$first)
if($first -lt 0 -or $last -lt $first){throw 'inherited pipe admission not found'}
$inherited=$modeset.Substring($first,$last-$first)
foreach($signature in @('static ULONG InheritedScalingDefault(','NTSTATUS ModesetValidateInherited(','ULONG ModesetScalingSupport(','NTSTATUS ModesetCommit(')){
    $first=$modeset.IndexOf($signature)
    $last=$modeset.IndexOf("`n}",$first)+3
    if($first -lt 0 -or $last -lt $first){throw "Missing body: $signature"}
    $inherited+=$modeset.Substring($first,$last-$first)+"`n"
}
$connector=Get-Content (Join-Path $repo 'driver\kmd\connector.c') -Raw
foreach($signature in @('static BOOLEAN ConnectorNeedsInherited(','static NTSTATUS ConnectorStartInherited(','BOOLEAN ConnectorRefusalKeepsDisplay(')){
    $first=$connector.IndexOf($signature)
    $last=$connector.IndexOf("`n}",$first)+3
    if($first -lt 0 -or $last -lt $first){throw "Missing body: $signature"}
    $inherited+=$connector.Substring($first,$last-$first)+"`n"
}
# BD-169 mutation control: put 0.7.216.64's pixel-rate clause back. Every
# inherited timing above 300 MHz then takes the fixed inherited route, and
# the BD-169 cases must fail.
if($OldPixelRateFixed){
    $anchor='m->InheritedFixed = Device->Post.Width != Signal->ActiveSize.cx || Device->Post.Height != Signal->ActiveSize.cy;'
    if(!$inherited.Contains($anchor)){throw 'BD-169 mutation anchor missing'}
    $inherited=$inherited.Replace($anchor,$anchor.TrimEnd(';')+" ||`n        Signal->PixelRate > 300000000ull;")
}
[IO.File]::WriteAllText((Join-Path $Out 'inherited_actual.inc'),$inherited)
$wddm=Get-Content (Join-Path $repo 'driver\kmd\wddm.c') -Raw
$first=$wddm.IndexOf('static NTSTATUS Bc250WddmDescribeAllocation(')
# The function alone, to its closing brace: what follows it in wddm.c (an .inc of WDK-typed code since 0.7.192) is not
# this test's to compile.
$end=[regex]::Match($wddm.Substring($first),'\r?\n}\r?\n')
if($first -lt 0 -or !$end.Success){throw 'Bc250WddmDescribeAllocation not found in wddm.c'}
$last=$first+$end.Index+$end.Length
$describe=$wddm.Substring($first,$last-$first)
if($OldDescribe60){
    $anchor='pDescribeAllocation->RefreshRate = signal.VSyncFreq;'
    if (!$describe.Contains($anchor)) { throw 'Describe refresh mutation anchor missing' }
    $describe=$describe.Replace($anchor, 'pDescribeAllocation->RefreshRate.Numerator=60000; pDescribeAllocation->RefreshRate.Denominator=1000;')
}
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
& $cl /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$repo\driver\kmd\test" "/I$repo\third_party\linux-amdgpu" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\timing_test.exe" "$Out\test.c" "$repo\driver\kmd\dcn_scale.c" "$repo\driver\kmd\display_io.c" "$repo\driver\kmd\edid.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'host build failed'}
& "$Out\timing_test.exe"
exit $LASTEXITCODE

param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\dpm-curve-mark",[switch]$ClearAnyPending,[switch]$SumSeal)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
# BD-343. The header under test is copied next to the build, so that a negative control changes the copy and never
# the tree (/I$Out comes first, so the copy wins).
#  -ClearAnyPending  a confirmation without output that clears the mark of any curve, also one that no display
#                    start confirmed.
#  -SumSeal          a seal that is the 16-bit checksum of the values (review C1): the rule compares sums only, so a
#                    curve with a colliding checksum and the value of an older store pass as confirmed.
$h=Get-Content "$repo\driver\kmd\dpm_curve_mark.h" -Raw
if($ClearAnyPending){
    $live='    return ConfirmedPresent && Confirmed == Seal;'
    if(!$h.Contains($live)){throw 'negative control: the known-curve clause moved'}
    $h=$h.Replace($live,'    (void)ConfirmedPresent; (void)Confirmed; (void)Seal; return 1;')
}
if($SumSeal){
    $m=[regex]::Match($h,'(?s)static __inline unsigned int Bc250DpmCurveSeal\(const unsigned int\* Mv, unsigned int Count\)\r?\n\{.*?\r?\n    return h;\r?\n\}')
    if(!$m.Success){throw 'negative control: the seal moved'}
    $sum=@"
static __inline unsigned int Bc250DpmCurveSeal(const unsigned int* Mv, unsigned int Count)
{
    unsigned int i, a = 0xA5u, b = 0x5Au;
    for (i = 0; i < Count; i++) {
        a = (a + (Mv[i] & 0xFFu)) % 255u; b = (b + a) % 255u;
        a = (a + ((Mv[i] >> 8) & 0xFFu)) % 255u; b = (b + a) % 255u;
    }
    return ((b & 0xFFu) << 8) | (a & 0xFFu);
}
"@
    $h=$h.Remove($m.Index,$m.Length).Insert($m.Index,$sum)
}
[IO.File]::WriteAllText((Join-Path $Out 'dpm_curve_mark.h'),$h)
# The model is the driver's only if dpm.c takes the rules at the places the test models. Each must occur once, and
# both writes of DpmCurveConfirmed must store the seal, never the checksum.
$c=Get-Content "$repo\driver\kmd\dpm.c" -Raw
foreach($call in @('if (Bc250DpmCurveDropAtStart(any, pendingPresent)) {',
                   'if (Bc250DpmCurveClearOnConfirm(s->CurvePending, 1, 0, 0u, 0u)) {',
                   'if (Bc250DpmCurveClearOnConfirm(1, 0, present, confirmed, seal)) {',
                   'curveStatus = GuardStoreSetting(DPM_SETTING_CURVE_CONFIRMED, seal);',
                   'StoreLogged(DPM_SETTING_CURVE_CONFIRMED, seal);',
                   'return Bc250DpmCurveSeal(Curve->mv, BC250_CURVE_POINTS);')){
    $n=([regex]::Matches($c,[regex]::Escape($call))).Count
    if($n -ne 1){throw "dpm.c has $n of: $call"}
}
$w=([regex]::Matches($c,'(GuardStoreSetting|StoreLogged)\(DPM_SETTING_CURVE_CONFIRMED,')).Count
if($w -ne 2){throw "dpm.c writes DpmCurveConfirmed at $w places, the test models 2"}
$s=Get-Content "$repo\driver\kmd\start_health.c" -Raw
if(!$s.Contains('if (NT_SUCCESS(status) && !recovered && !display) (void)DpmConfirmKnownCurve(Device,"start-health");')){
    throw 'start_health.c does not call DpmConfirmKnownCurve for a confirmation without output'
}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$repo\driver\shim\include" "/I$repo\driver\shim\generated" "/I$repo\driver\amdgpu-import" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\dpm_curve_mark_test.exe" "$PSScriptRoot\dpm_curve_mark_test.c" "$repo\driver\shim\bc250_clock.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'dpm curve mark host build failed'}
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\dpm_curve_mark_test.exe"
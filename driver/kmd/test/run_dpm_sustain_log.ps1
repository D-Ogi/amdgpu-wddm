param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\dpm-sustain-log",[switch]$ConfigOnlyAfterChoice,[switch]$NoOffReason)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
# BD-336. The header under test is copied next to the build, so that a negative control changes the copy and never
# the tree (/I$Out comes first, so the copy wins). Two controls, one for each half of the fix:
# -ConfigOnlyAfterChoice is the driver before BD-336: the summary says nothing about the rule until it chose a raise.
# -NoOffReason prints "on" for a start whose hold the governor refused, so an off rule reads as on.
$h=Get-Content "$repo\driver\kmd\dpm_sustain_log.h" -Raw
if($ConfigOnlyAfterChoice){
    $live='    if (Summary) lines |= BC250_DPM_SUSTAIN_LINE_CONFIG;'
    if(!$h.Contains($live)){throw 'negative control: the configuration clause moved'}
    $h=$h.Replace($live,'    if (Summary && (Raises != 0u || Refusals != 0u)) lines |= BC250_DPM_SUSTAIN_LINE_CONFIG;')
}
if($NoOffReason){
    $live='    return Accepted ? BC250_DPM_SUSTAIN_STATE_ON : BC250_DPM_SUSTAIN_STATE_HOLD_REFUSED;'
    if(!$h.Contains($live)){throw 'negative control: the refused clause moved'}
    $h=$h.Replace($live,'    return Accepted ? BC250_DPM_SUSTAIN_STATE_ON : BC250_DPM_SUSTAIN_STATE_ON;')
}
[IO.File]::WriteAllText((Join-Path $Out 'dpm_sustain_log.h'),$h)
# The three format strings come from dpm.c itself, so the test prints what the driver prints. Each must occur once.
$c=Get-Content "$repo\driver\kmd\dpm.c" -Raw
$formats=[ordered]@{
    DPM_SUSTAIN_START_FORMAT='"dpm: sustained raise: [^"]*"'
    DPM_SUSTAIN_CONFIG_FORMAT='"dpm: summary rules: sustained raise: [^"]*"'
    DPM_SUSTAIN_COUNTERS_FORMAT='"dpm: %s sustained raises [^"]*"'
}
$defs=foreach($k in $formats.Keys){
    $m=[regex]::Matches($c,'GuardLog\(('+$formats[$k]+')')
    if($m.Count -ne 1){throw "dpm.c has $($m.Count) GuardLog formats for $k"}
    "#define $k $($m[0].Groups[1].Value)"
}
[IO.File]::WriteAllText((Join-Path $Out 'dpm_sustain_formats.h'),(($defs -join "`n")+"`n"))
# The summary calls the configuration line with no condition of its own: dpm_sustain_log.h decides.
$i=$c.IndexOf('void DpmLogSummary(')
if($i -lt 0){throw 'dpm.c has no DpmLogSummary'}
$body=$c.Substring($i,$c.IndexOf("`n}",$i)-$i)
if(([regex]::Matches($body,'(?m)^    DpmLogSustainConfigLine\(sustain, &snap\);')).Count -ne 1){throw 'DpmLogSummary does not call DpmLogSustainConfigLine once, unconditionally'}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4127 /O2 /MT "/I$Out" "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\dpm_sustain_log_test.exe" "$PSScriptRoot\dpm_sustain_log_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'dpm sustain log host build failed'}
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\dpm_sustain_log_test.exe"

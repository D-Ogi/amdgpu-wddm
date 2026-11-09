param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\m9\retained-power",[string]$Source='',
      # BD-188 negative controls of the bounded wait of the way down (tools/quality/quick.ps1): each must fail the test.
      [switch]$DrainNone,      # no wait: the first busy answer refuses, as before the fix
      [switch]$DrainWatchdog,  # a watchdog fault is waited for too
      [switch]$DrainStepCount) # the bound counts delays, not elapsed time
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if(-not $Source){$Source="$repo\driver\kmd\wddm.c"}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl="$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c"
$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP="$Root\scratch\tmp"; $env:TMP=$env:TEMP
function Set-Mutant([string]$Text,[string]$Old,[string]$New) {
    if(-not $Text.Contains($Old)){throw "Mutant anchor not found: $Old"}
    return $Text.Replace($Old,$New)
}
if($DrainNone -or $DrainWatchdog -or $DrainStepCount){
    $Out="$Out-mutant"                       # never over the files of the real test
    New-Item -ItemType Directory -Force $Out | Out-Null
    $text=[IO.File]::ReadAllText($Source)
    if($DrainNone){$text=Set-Mutant $text 'waited>=BC250_WDDM_SUSPEND_DRAIN_MS)' '1)'}
    if($DrainWatchdog){$text=Set-Mutant $text '!Bc250PowerDrainWorthWaiting(busy) ||' '!(busy!=0) ||'}
    if($DrainStepCount){$text=Set-Mutant $text 'waited>=BC250_WDDM_SUSPEND_DRAIN_MS)' 'rounds>=BC250_WDDM_SUSPEND_DRAIN_MS)'}
    $Source="$Out\wddm-mutant.c"
    [IO.File]::WriteAllText($Source,$text)
}
& python "$PSScriptRoot\generate_retained_power_test.py" --source $Source --out "$Out\retained_power_actual.c"
if($LASTEXITCODE -ne 0){throw 'Source extraction failed'}
& $cl /nologo /TC /W4 /WX /O2 /MT "/I$repo\driver\kmd" "/I$repo\third_party\linux-amdgpu" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\retained_power_test.exe" "$Out\retained_power_actual.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'Host build failed'}
# The test-start and test-end lines of host_native.ps1 mark the step a negative control must fail (BD-237).
. "$PSScriptRoot\host_native.ps1"
Exit-NativeTest "$Out\retained_power_test.exe"

# Host test of the start-confirm task's decision rules (installer\start-confirm-core.ps1, Get-StartConfirmStep).
# Each case drives one task run as a list of polls (seconds since the task started, the start-health reading) and
# checks the step that ends it. BD-269 adds the cases of a recovered reset: the epoch moves inside the same
# generation, readiness counts again from 0, and the bound must give it the time to reach 60 s again, within the
# limit of moves and the latest bound. The case 'bd269-s1b' is the S1-B boot of 2026-10-30 (recoveries at about +35 s
# and +80 s, epoch 13 and then 21); the rules before BD-269 end it at 120 s, unconfirmed.
#
#   powershell -NoProfile -File tools\release\test-start-confirm.ps1 [-Core <start-confirm-core.ps1>]
#
# -Core takes another copy of the rules, for the negative control: the copy before BD-269 fails the BD-269 cases.
# Runs under Windows PowerShell 5.1 and PowerShell 7 and reads nothing from the host.
param([string]$Core = (Join-Path $PSScriptRoot 'installer\start-confirm-core.ps1'))
$ErrorActionPreference = 'Stop'
. $Core
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }

# A reading of a full, ready, visible start (flags 7, QUIET when -Quiet) with the given numbers.
function New-Reading([uint64]$Ready, [uint64]$Epoch = 5, [uint64]$Generation = 3, [int]$Flags = 7, [uint64]$Completed = 40, [uint64]$Age = 1000) {
    return [ordered]@{version='0x000700D8'; flags=$Flags; generation=$Generation; epoch=$Epoch; completed=$Completed
        age_ms=$Age; ready_ms=$Ready; source='test'}
}
# One task run: polls every 5 s from 0 to the hard end, the reading from $Reading (a script block of the time, or
# $null for no reading). A confirm step stands for a confirmation the KMD accepted: the next reading has flags 15.
function Invoke-Run([double]$Seconds, [scriptblock]$Reading, $Kit = $null, [double]$End = 400) {
    $state = New-StartConfirmState $Seconds
    $confirmed = $false
    $log = New-Object System.Collections.Generic.List[string]
    for ($now = 0.0; $now -le $End; $now += 5.0) {
        $r = & $Reading $now
        if ($confirmed -and $r) { $r.flags = $r.flags -bor 8 }
        $step = Get-StartConfirmStep -State $state -Reading $r -Now $now -Stop $false -Kit $Kit -ReadError 'test: no reading'
        $log.Add("+$now $($step.action) $($step.reason)")
        if ($step.action -eq 'confirm') { $confirmed = $true; $state.confirmed_here = $true; $state.last_confirm = $now; continue }
        if ($step.action -eq 'exit') { return [ordered]@{code=$step.code; at=$now; reason=$step.reason; log=$log; state=$state} }
    }
    return [ordered]@{code=$null; at=$End; reason='no exit'; log=$log; state=$state}
}
function Show($Run) { "    exit $($Run.code) at +$($Run.at) s: $($Run.reason)" }

'case healthy: ready from +10 s, confirmed once ready_ms reaches 60 s'
$run = Invoke-Run 120 { param($t) New-Reading ([uint64][Math]::Max(0, ($t - 10) * 1000)) }
Show $run
Check ($run.code -eq 0 -and $run.at -le 80) 'confirmed by this task before +80 s'

'case nothing ready: the bound ends the run unconfirmed at 120 s'
$run = Invoke-Run 120 { param($t) New-Reading 0 -Flags 3 }
Show $run
Check ($run.code -eq 1 -and $run.at -eq 120) 'exit 1 at the bound'

'case no reading at all: exit 5 at the bound'
$run = Invoke-Run 120 { param($t) $null }
Show $run
Check ($run.code -eq 5 -and $run.at -eq 120) 'exit 5 at the bound'

'case not a full-WDDM start: exit 5 at once'
$run = Invoke-Run 120 { param($t) New-Reading 0 -Flags 6 }
Check ($run.code -eq 5 -and $run.at -eq 0) 'exit 5 at the first poll'

'case adapter restart: the generation changes, exit 3'
$run = Invoke-Run 120 { param($t) if ($t -lt 30) { New-Reading 1000 } else { New-Reading 1000 -Generation 4 } }
Check ($run.code -eq 3 -and $run.at -eq 30) 'exit 3 when the generation changes'

'case a transition task of a kit runs: exit 2 at once'
$run = Invoke-Run 120 { param($t) New-Reading 1000 } -Kit ([ordered]@{transition='BC250 kit'; heartbeat=$false})
Check ($run.code -eq 2 -and $run.at -eq 0) 'exit 2, left to the kit'

'case bd269-s1b: recoveries at +35 s and +80 s (epoch 5 -> 13 -> 21), readiness from 0 after each'
$s1b = {
    param($t)
    if ($t -lt 35) { return New-Reading ([uint64]($t * 1000 + 20000)) -Epoch 5 }
    if ($t -lt 80) { return New-Reading ([uint64](($t - 35) * 1000)) -Epoch 13 }
    return New-Reading ([uint64](($t - 80) * 1000)) -Epoch 21
}
$run = Invoke-Run 120 $s1b
Show $run
Check ($run.code -eq 0) 'confirmed: two recoveries are inside the limit'
Check ($run.at -ge 140 -and $run.at -le 170) 'confirmed after readiness is 60 s again (+140 s) and before the moved bound (+170 s)'
Check ($run.state.recoveries -eq 2) 'two epoch moves counted'
Check (@($run.log | Where-Object { $_ -match 'recovery 2 of 2: bound now 170 s' }).Count -eq 1) 'the log names the second move and the new bound'

'case bd269-limit: a third recovery does not move the bound again'
$three = {
    param($t)
    if ($t -lt 20) { return New-Reading ([uint64]($t * 1000)) -Epoch 5 }
    if ($t -lt 40) { return New-Reading ([uint64](($t - 20) * 1000)) -Epoch 6 }
    if ($t -lt 75) { return New-Reading ([uint64](($t - 40) * 1000)) -Epoch 7 }
    return New-Reading ([uint64](($t - 75) * 1000)) -Epoch 8
}
$run = Invoke-Run 120 $three
Show $run
Check ($run.code -eq 1) 'unconfirmed: three recoveries are over the limit'
Check ($run.at -eq 130) 'ends at the bound of the second move (+40 s + 90 s), not later'
Check (@($run.log | Where-Object { $_ -match 'recovery 3 is over the limit of 2: bound stays 130 s' }).Count -eq 1) 'the log says the bound stays'

'case bd269-chain: with the bound of 120 s, two late recoveries end the run at 290 s'
$late = {
    param($t)
    if ($t -lt 115) { return New-Reading 0 -Flags 3 -Epoch 5 }
    if ($t -lt 200) { return New-Reading ([uint64](($t - 115) * 1000)) -Flags 3 -Epoch 6 }
    return New-Reading ([uint64](($t - 200) * 1000)) -Flags 3 -Epoch 7
}
$run = Invoke-Run 120 $late
Show $run
Check ($run.code -eq 1 -and $run.at -eq 290) 'exit 1 at +200 s + 90 s'

'case bd269-ceiling: a longer -Seconds (lab use), a recovery at +245 s moves the bound to 300 s, not 335 s'
$run = Invoke-Run 250 { param($t) if ($t -lt 245) { New-Reading 0 -Flags 3 -Epoch 5 } else { New-Reading 1000 -Flags 3 -Epoch 6 } }
Show $run
Check ($run.code -eq 1 -and $run.at -eq 300) 'exit 1 at the latest bound of 300 s'

'case bd269-confirmed-epoch: a confirmation of another client after a move still ends the run with 0'
$run = Invoke-Run 120 { param($t) if ($t -lt 50) { New-Reading 1000 -Epoch 5 -Flags 3 } else { New-Reading 1000 -Epoch 9 -Flags 15 } }
Check ($run.code -eq 0 -and $run.at -eq 50) 'exit 0 at the first confirmed reading'

'case bd343-no-output: a start without a firmware display and no monitor (flags FULL|READY|NO_OUTPUT, no completion)'
# Unit A, 2026-11-10: such a start read flags 3, completed 0, ready_ms 0, and was never confirmed. The KMD with
# NO_OUTPUT counts its ready time from the engines and sets flag 32.
$headless = { param($t) New-Reading ([uint64][Math]::Max(0, ($t - 5) * 1000)) -Flags 35 -Completed 0 -Age ([uint64]::MaxValue) }
$run = Invoke-Run 120 $headless
Show $run
Check ($run.code -eq 0 -and $run.at -ge 65 -and $run.at -le 75) 'confirmed by this task once ready_ms reaches 60 s'
Check (@($run.log | Where-Object { $_ -match ' confirm eligible by the no-output rule \(flag 32\): flags 35 ' }).Count -eq 1) 'the log names the rule and flag 32'
$run = Invoke-Run 120 { param($t) New-Reading ([uint64][Math]::Max(0, ($t - 5) * 1000)) -Flags 35 -Completed 0 -Age 0 } -End 60
Check ($null -eq $run.code) 'not before ready_ms reaches 60 s'
'case bd343-61s: ready_ms 61000 at the first reading confirms at once'
$run = Invoke-Run 120 { param($t) New-Reading ([uint64](61000 + $t * 1000)) -Flags 35 -Completed 0 -Age ([uint64]::MaxValue) }
Check ($run.code -eq 0 -and $run.at -eq 5) 'confirmed at the first poll after the confirm'
'case bd343-child-change: the child changes at +30 s (a new epoch, ready time from 0 again)'
$run = Invoke-Run 120 { param($t) if ($t -lt 30) { New-Reading ([uint64]($t * 1000)) -Flags 35 -Completed 0 -Age ([uint64]::MaxValue) }
    else { New-Reading ([uint64](($t - 30) * 1000)) -Epoch 6 -Flags 35 -Completed 0 -Age ([uint64]::MaxValue) } }
Show $run
Check ($run.code -eq 0 -and $run.at -ge 90 -and $run.at -le 100) 'confirmed only after 60 s of the new interval'
'case bd343-confirmed-rearmed: another client confirmed the start, and a recovery then opened a new interval'
# Flags FULL|READY|CONFIRMED|NO_OUTPUT (43) with ready_ms under 60 s: the start is confirmed, so the run ends at the
# first poll with 0 and does not wait for the minute of the new interval.
$rearmed = { param($t) New-Reading ([uint64]($t * 1000 + 5000)) -Epoch 9 -Flags 43 -Completed 0 -Age ([uint64]::MaxValue) }
$run = Invoke-Run 120 $rearmed
Show $run
Check ($run.code -eq 0 -and $run.at -eq 0) 'exit 0 at the first poll'
Check ($run.reason -match '^confirmed by another client: flags 43 ') 'the log says that another client confirmed it'
$run = Invoke-Run 120 { param($t) if ($t -lt 20) { New-Reading 0 -Epoch 5 -Flags 35 -Completed 0 -Age ([uint64]::MaxValue) }
    else { New-Reading ([uint64](($t - 20) * 1000)) -Epoch 9 -Flags 43 -Completed 0 -Age ([uint64]::MaxValue) } }
Check ($run.code -eq 0 -and $run.at -eq 20 -and $run.reason -match '^confirmed by another client') 'exit 0 at the first confirmed reading after the epoch move'
'negative control bd343-confirmed-rearmed: the rules without the confirmed exit wait to the bound'
$coreText = [IO.File]::ReadAllText($Core)
$exitBlock = [regex]::Match($coreText,'(?m)^ if \(\$null -ne \$Reading -and \(\$Reading\.flags -band 9\) -eq 9 .*\r?\n.*\r?\n.*\r?\n \}\r?\n')
Check $exitBlock.Success 'the confirmed exit is in the rules'
if ($exitBlock.Success) {
    . ([scriptblock]::Create($coreText.Remove($exitBlock.Index, $exitBlock.Length)))
    $run = Invoke-Run 120 $rearmed
    Check ($run.code -ne 0) 'without it the confirmed start is not ended with 0 (the control fails as it must)'
    . $Core
}
'case bd343-contradiction: VISIBLE and NO_OUTPUT together (flags 39) are never confirmed, by either rule'
$run = Invoke-Run 120 { param($t) New-Reading 90000 -Flags 39 -Completed 40 -Age 0 }
Check ($run.code -eq 1 -and $run.at -eq 120) 'exit 1 at the bound'
Check (@($run.log | Where-Object { $_ -match 'VISIBLE \(4\) and NO_OUTPUT \(32\) together' }).Count -ge 1) 'the log says why'
Check ($null -eq (Get-StartHealthRule -Flags 39 -ReadyMs 90000) -and $null -eq (Get-StartHealthRule -Flags 47 -ReadyMs 90000)) 'and verify has no rule for it'
'case bd343-no-output-not-ready: the engines did not come up (flags FULL|NO_OUTPUT): unconfirmed at the bound'
$run = Invoke-Run 120 { param($t) New-Reading 0 -Flags 33 -Completed 0 -Age ([uint64]::MaxValue) }
Check ($run.code -eq 1 -and $run.at -eq 120) 'exit 1 at the bound'
'case bd343-lab: the reading of a KMD without NO_OUTPUT (flags 3, ready_ms 0) stays unconfirmed, as on the lab'
$run = Invoke-Run 120 { param($t) New-Reading 0 -Flags 3 -Completed 0 -Age ([uint64]::MaxValue) }
Check ($run.code -eq 1 -and $run.at -eq 120) 'exit 1 at the bound'
'negative control bd343: the rules without the NO_OUTPUT line do not confirm the start without output'
$coreText = [IO.File]::ReadAllText($Core)
$line = [regex]::Match($coreText, '(?m)^ if \(\(\(\$Reading\.flags -band 39\) -eq 35\).*\r?\n')
Check $line.Success 'the NO_OUTPUT line is in the rules'
if ($line.Success) {
    . ([scriptblock]::Create($coreText.Remove($line.Index, $line.Length)))
    $run = Invoke-Run 120 $headless
    Check ($run.code -eq 1) 'without it the start without output stays unconfirmed (the control fails as it must)'
    . $Core
}

'case bd343-verify: the verify rule of install.ps1 and diagnostics.ps1 (Get-StartHealthRule)'
$cases = @(
    @{ f = 7;  ms = 0;     want = 'display'; stage = 50; why = 'full, ready, visible' }
    @{ f = 15; ms = 0;     want = 'display'; stage = 50; why = 'a confirmed display start' }
    @{ f = 31; ms = 90000; want = 'display'; stage = 50; why = 'a confirmed, quiet display start' }
    @{ f = 35; ms = 60000; want = 'no output'; stage = 39; why = 'full, ready, no output, the minute reached' }
    @{ f = 43; ms = 0;     want = 'no output'; stage = 39; why = 'a confirmed start without output' }
    @{ f = 35; ms = 59999; want = $null; stage = $null; why = 'no output, one millisecond short of the minute' }
    @{ f = 35; ms = $null; want = $null; stage = $null; why = 'no output, ready time unknown' }
    @{ f = 33; ms = 90000; want = $null; stage = $null; why = 'no output, the engines not ready' }
    @{ f = 3;  ms = 90000; want = $null; stage = $null; why = 'flags 3, the BD-343 lab reading of a KMD without NO_OUTPUT' }
    @{ f = $null; ms = 90000; want = $null; stage = $null; why = 'no reading' }
)
foreach ($c in $cases) {
    $r = Get-StartHealthRule -Flags $c.f -ReadyMs $c.ms
    $name = $(if ($r) { $r.name } else { $null }); $stage = $(if ($r) { $r.stage } else { $null })
    Check ($name -eq $c.want -and $stage -eq $c.stage) "flags $($c.f) ready_ms $($c.ms): $(if ($c.want) { "$($c.want) rule, LastStage $($c.stage)" } else { 'no rule' }) ($($c.why))"
}
# install.ps1 must read both its checks through the rule: the LastStage it wants and the start-health verdict.
$inst = [IO.File]::ReadAllText((Join-Path (Split-Path $Core) 'install.ps1'))
Check ($inst -match '\$rule = Get-StartHealthRule -Flags \$hflags -ReadyMs \$hready') 'install.ps1 verify reads the rule'
Check ($inst -match '"LastStage \$wantStage\$"') 'install.ps1 driver start wants the LastStage of the rule'
Check ($inst -match "Add-Result 'start health' \(\`$null -ne \`$rule\)") 'install.ps1 start health passes on the rule'
Check ($inst -notmatch "'LastStage 50\$'") 'install.ps1 has no fixed LastStage 50 left'

'constants'
# The time limit of each task that runs the confirmation in the installer's plan (common.ps1 Get-LogonTaskPlan: the
# logon confirmation and, since BD-271, the startup confirmation) must hold the latest bound, the hard-bound margin of
# start-confirm.ps1 (10 s) and the script's start (a few seconds).
$plan = [IO.File]::ReadAllText((Join-Path (Split-Path $Core) 'common.ps1'))
foreach ($role in @('confirm', 'startup')) {
    $m = [regex]::Match($plan, "role\s*=\s*'$role'(?:(?!role\s*=).)*?minutes\s*=\s*(\d+)", 'Singleline')
    $minutes = $(if ($m.Success) { [int]$m.Groups[1].Value } else { 0 })
    "    $role task time limit in common.ps1: $minutes min"
    Check ($m.Success -and $script:StartConfirmMaxSeconds + 30 -le $minutes * 60) "the latest bound plus 30 s fits the $role task time limit"
}

'bd335: a clock step after the boot does not hide the run of this boot'
# The measured case: boot 10:03:31Z, the task's run 28 s before it, the verdict already final.
$lateRun = [pscustomobject]@{ LastRunTime = [datetime]'2026-11-09T10:03:03Z' }
$bootUtc = [datetime]'2026-11-09T10:03:31Z'
# A marker and a reading of the shared page (KUSER_SHARED_DATA), and the lagging registry copy (BD-130).
function New-Marker([int64]$Id, [string]$Source = 'KUSER_SHARED_DATA') { return [pscustomobject]@{ boot_id = $Id; source = $Source } }
function New-BootReading($Id, [string]$Source = 'KUSER_SHARED_DATA') { return [ordered]@{ boot_id = $Id; source = $Source } }
$answer = Get-StartConfirmRunOfThisBoot -TaskInfo $lateRun -BootUtc $bootUtc -Marker (New-Marker 42) -ThisBoot (New-BootReading 42)
Check ($answer.ran -and $answer.source -eq 'boot id') 'the marker of this boot wins over the misread wall clock'
$answer = Get-StartConfirmRunOfThisBoot -TaskInfo $lateRun -BootUtc $bootUtc -Marker $null -ThisBoot (New-BootReading 42)
Check (-not $answer.ran) 'without the marker the wall-clock rule still refuses the misread run'
$answer = Get-StartConfirmRunOfThisBoot -TaskInfo ([pscustomobject]@{ LastRunTime = $bootUtc }) -BootUtc $bootUtc -Marker $null -ThisBoot (New-BootReading 42)
Check ($answer.ran -and $answer.source -eq 'wall clock') 'the wall-clock rule is the fallback'
$answer = Get-StartConfirmRunOfThisBoot -TaskInfo $lateRun -BootUtc $bootUtc -Marker (New-Marker 41) -ThisBoot (New-BootReading 42)
Check (-not $answer.ran) 'a marker of another boot does not count'
$answer = Get-StartConfirmRunOfThisBoot -TaskInfo ([pscustomobject]@{ LastRunTime = $bootUtc }) -BootUtc $bootUtc -Marker (New-Marker 41) -ThisBoot (New-BootReading 47)
Check ($answer.ran -and $answer.source -eq 'wall clock') 'a stale marker does not block the wall-clock fallback'
$answer = Get-StartConfirmRunOfThisBoot -TaskInfo $lateRun -BootUtc $bootUtc -Marker (New-Marker 42) -ThisBoot (New-BootReading $null 'nothing')
Check (-not $answer.ran) 'an unreadable boot id cannot match a marker'
# The previous boot's marker 387 and a registry reading that still says 387 in the new boot: only the wall clock.
$answer = Get-StartConfirmRunOfThisBoot -TaskInfo $lateRun -BootUtc $bootUtc -Marker (New-Marker 387) -ThisBoot (New-BootReading 387 'PrefetchParameters registry')
Check (-not $answer.ran -and $answer.source -ne 'boot id') 'a registry reading of this boot never matches a marker'
$answer = Get-StartConfirmRunOfThisBoot -TaskInfo $lateRun -BootUtc $bootUtc -Marker (New-Marker 42 'PrefetchParameters registry') -ThisBoot (New-BootReading 42)
Check (-not $answer.ran -and $answer.source -ne 'boot id') 'a marker with a registry BootId never matches'
$answer = Get-StartConfirmRunOfThisBoot -TaskInfo ([pscustomobject]@{ LastRunTime = $bootUtc }) -BootUtc $bootUtc -Marker (New-Marker 42 'PrefetchParameters registry') -ThisBoot (New-BootReading 42 'PrefetchParameters registry')
Check ($answer.ran -and $answer.source -eq 'wall clock') 'with a registry reading the wall clock still answers'

'bd335: the marker the task writes is the one verify reads'
$oldProgramData = $env:ProgramData
$oldShared = $env:AMDGPU_WDDM_TEST_SHARED_BOOT_ID
$temp = Join-Path ([IO.Path]::GetTempPath()) ("bd335-" + [Guid]::NewGuid().ToString('N'))
try {
    $env:ProgramData = $temp
    $env:AMDGPU_WDDM_TEST_SHARED_BOOT_ID = '42'
    $why = Write-StartConfirmRunMarker -ExitCode 0 -Reason 'test'
    Check ($null -eq $why) 'the marker writes'
    $marker = Get-StartConfirmRunMarker
    Check ($marker -and [int64]$marker.boot_id -eq 42 -and [int]$marker.schema -eq 1 -and [int]$marker.exit -eq 0) 'the marker reads back with the boot id of the run'
    $env:AMDGPU_WDDM_TEST_SHARED_BOOT_ID = '43'
    $why = Write-StartConfirmRunMarker -ExitCode 6 -Reason 'second run'
    $marker = Get-StartConfirmRunMarker
    Check ($null -eq $why -and $marker -and [int64]$marker.boot_id -eq 43 -and [int]$marker.exit -eq 6) 'a second run replaces the marker'
    Check (@(Get-ChildItem -LiteralPath (Split-Path (Get-StartConfirmRunMarkerPath)) -Filter '*.tmp').Count -eq 0) 'no temporary file stays after a write'
    $env:ProgramData = Join-Path $temp 'not-a-folder.txt'
    [IO.File]::WriteAllText($env:ProgramData, 'a file where the folder must be')
    $why = Write-StartConfirmRunMarker -ExitCode 0 -Reason 'test'
    Check ($why -is [string] -and $why.Length -gt 0) 'a folder that cannot be written gives a reason and does not throw'
    $env:ProgramData = $temp
    [IO.File]::WriteAllText((Get-StartConfirmRunMarkerPath), 'not json')
    Check ($null -eq (Get-StartConfirmRunMarker)) 'a corrupt marker is no marker'
} finally {
    $env:ProgramData = $oldProgramData
    if ($null -eq $oldShared) { Remove-Item Env:\AMDGPU_WDDM_TEST_SHARED_BOOT_ID -ErrorAction SilentlyContinue }
    else { $env:AMDGPU_WDDM_TEST_SHARED_BOOT_ID = $oldShared }
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}

if ($fail) { "FAIL: $fail check(s)"; exit 1 }
'PASS: all checks'
exit 0

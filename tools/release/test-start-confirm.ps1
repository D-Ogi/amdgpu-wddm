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

if ($fail) { "FAIL: $fail check(s)"; exit 1 }
'PASS: all checks'
exit 0

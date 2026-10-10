# test-arm-bounds.ps1 - the failure paths of the M16 HIP lab wrappers, on the development PC,
# with no GPU, no lab and no driver installed.
#
# Each case replaces one real part of compute\hip\lab\armlib.ps1 with a fake: a helper that hangs,
# a sampler that answers nothing, a child that never cooperates with any budget, a kill that
# cannot be confirmed. The audit of 2026-10-10 (finding HIP-F2) asks for exactly these, because
# the wrappers they replace enforced their bound and their thermal rule only when nothing went
# wrong.
#
#   pwsh compute\hip\tests\lab\test-arm-bounds.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File compute\hip\tests\lab\test-arm-bounds.ps1
#
# It prints one line a check it failed and exits with the number of failures, as the C host tests
# of this component do. The whole run stays under a minute: the longest case is a 12 s bound.
#
# The negative control runs the same cases against the control flow of the wrappers these replace:
#
#   pwsh compute\hip\tests\lab\test-arm-bounds.ps1 -Supervisor Invoke-LegacyLabArm -ExpectFailures
#
# That mode exits 0 when the old logic fails checks, which is the point of it, and 1 when the old
# logic passes everything, because then the cases prove nothing.
param(
    [string]$WorkDir = '',
    [string]$Supervisor = 'Invoke-LabArm',
    [switch]$ExpectFailures
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\lab\armlib.ps1')
if ($Supervisor -ne 'Invoke-LabArm') {
    . (Join-Path $PSScriptRoot 'legacy-arm-supervisor.ps1')
}
$arm = (Get-Command $Supervisor).Name
$real = ($Supervisor -eq 'Invoke-LabArm')

if (-not $WorkDir) {
    # $env:TEMP, which the build scripts of this component point at the workspace scratch, so
    # nothing of a test run lands on drive C: of the development PC.
    $base = $env:TEMP
    if (-not $base) { $base = [IO.Path]::GetTempPath() }
    $WorkDir = Join-Path $base ("arm-bounds-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
}
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$script:checks = 0
$script:failures = 0

function Check {
    param([bool]$Condition, [string]$What)
    $script:checks++
    if (-not $Condition) {
        $script:failures++
        Write-Host "FAIL $What"
    }
}

function New-FakeCli {
    param([string]$Name, [string[]]$Body)
    $path = Join-Path $WorkDir $Name
    Set-Content -LiteralPath $path -Value (@('@echo off') + $Body) -Encoding ASCII
    return $path
}

# A child that never stops by itself and reads no budget of its own.
function Start-StubbornChild {
    $exe = (Get-Command powershell.exe).Source
    $p = Start-Process -FilePath $exe -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep -Seconds 600') `
        -NoNewWindow -PassThru
    $null = $p.Handle
    return $p
}

# A child that finishes by itself with a known exit code.
function Start-QuickChild {
    param([int]$Code = 7, [double]$Seconds = 1.0)
    $exe = (Get-Command powershell.exe).Source
    $p = Start-Process -FilePath $exe -ArgumentList @('-NoProfile', '-Command',
        "Start-Sleep -Seconds $Seconds; exit $Code") -NoNewWindow -PassThru
    $null = $p.Handle
    return $p
}

# Samplers. Each one is what the supervisor sees in place of a clock read.
$sampleOk = { [pscustomobject]@{ Ok = $true; Tctl = 60.0; Line = 'temperature_mc=60000'; Reason = '' } }
$sampleDead = { [pscustomobject]@{ Ok = $false; Tctl = $null; Line = ''
                                   Reason = 'the clock read carried no temperature_mc field' } }
$sampleHot = { [pscustomobject]@{ Ok = $true; Tctl = 87.5; Line = 'temperature_mc=87500'; Reason = '' } }
# Cool before the arm starts and hot from the first sample inside it: the precondition at 87 C is
# a separate rule, and these two cases are about the rules that act while the child runs.
$script:hotCalls = 0
$sampleHotAfterStart = {
    $script:hotCalls++
    if ($script:hotCalls -le 1) {
        [pscustomobject]@{ Ok = $true; Tctl = 60.0; Line = 'temperature_mc=60000'; Reason = '' }
    } else {
        [pscustomobject]@{ Ok = $true; Tctl = 87.5; Line = 'temperature_mc=87500'; Reason = '' }
    }
}
$script:veryHotCalls = 0
$sampleVeryHotAfterStart = {
    $script:veryHotCalls++
    if ($script:veryHotCalls -le 1) {
        [pscustomobject]@{ Ok = $true; Tctl = 60.0; Line = 'temperature_mc=60000'; Reason = '' }
    } else {
        [pscustomobject]@{ Ok = $true; Tctl = 89.2; Line = 'temperature_mc=89200'; Reason = '' }
    }
}

# A program that must exist for the supervisor to agree to start at all.
$stubProgram = Join-Path $WorkDir 'stub.cmd'
Set-Content -LiteralPath $stubProgram -Value @('@echo off', 'echo stub') -Encoding ASCII

Write-Host "arm-bounds tests in $WorkDir"

# ---------------------------------------------------------------------------------------------
# 1. The bound itself. A non-game lab trial is at most 180 s including cleanup, so a caller
#    cannot ask for more, and the work part is smaller than the whole bound.
# ---------------------------------------------------------------------------------------------
$threw = $false
try { $null = New-ArmDeadline -BoundSec 500 } catch { $threw = $true }
Check $threw 'a bound of 500 s is refused'
$d = New-ArmDeadline -BoundSec 170 -CleanupReserveSec 20
Check ($d.TotalSec -eq 170 -and $d.WorkSec -eq 150) 'a 170 s bound keeps 20 s for cleanup'
$threw = $false
try { $null = New-ArmDeadline -BoundSec 2 } catch { $threw = $true }
Check $threw 'a bound of 2 s is refused'
# The supervisor under test must itself hold a caller to the limit. The wrappers of 2026-10-09
# took any -BoundSec the caller passed.
$capped = $false
try {
    $null = & $arm -Name 'over-bound' -Dir $WorkDir -Exe 'stub.cmd' -BoundSec 500 `
        -Sampler $sampleOk -Launcher { Start-QuickChild -Code 0 -Seconds 0.2 } `
        -ReportPath (Join-Path $WorkDir 'over-bound.json')
} catch {
    $capped = $true
}
Check $capped 'the supervisor refuses a 500 s arm, cleanup included'

# ---------------------------------------------------------------------------------------------
# 2. The temperature read, against four broken helpers and one good one.
# ---------------------------------------------------------------------------------------------
$cliOk = New-FakeCli 'fake-clock-ok.cmd' @('echo gfx_clock_mhz=1000 temperature_mc=86500')
$read = Read-ArmTctl -Cli $cliOk -TimeoutSec 10 -TempDir $WorkDir
Check ($read.Ok -and [math]::Abs($read.Tctl - 86.5) -lt 0.001) 'a good clock read gives 86.5 C'

$cliHang = New-FakeCli 'fake-clock-hang.cmd' @('powershell -NoProfile -Command "Start-Sleep -Seconds 60"')
$watch = [Diagnostics.Stopwatch]::StartNew()
$read = Read-ArmTctl -Cli $cliHang -TimeoutSec 3 -TempDir $WorkDir
$watch.Stop()
Check (-not $read.Ok) 'a clock read that hangs is a failed read'
Check ($watch.Elapsed.TotalSeconds -lt 12) ("a hung clock read returns inside its bound, not after it (took {0:N1} s)" -f $watch.Elapsed.TotalSeconds)
Check ($read.Reason -like '*no answer within 3 s*') "the reason names the bound: '$($read.Reason)'"

$cliSilent = New-FakeCli 'fake-clock-silent.cmd' @('echo ok')
$read = Read-ArmTctl -Cli $cliSilent -TimeoutSec 10 -TempDir $WorkDir
Check (-not $read.Ok) 'a clock read with no temperature field is a failed read'

$cliFail = New-FakeCli 'fake-clock-fail.cmd' @('echo broken', 'exit /b 1')
$read = Read-ArmTctl -Cli $cliFail -TimeoutSec 10 -TempDir $WorkDir
Check (-not $read.Ok) 'a clock read that exits non-zero is a failed read'

$cliCrazy = New-FakeCli 'fake-clock-crazy.cmd' @('echo temperature_mc=250000')
$read = Read-ArmTctl -Cli $cliCrazy -TimeoutSec 10 -TempDir $WorkDir
Check (-not $read.Ok) 'a clock read of 250 C is a failed read and not a temperature'

$read = Read-ArmTctl -Cli (Join-Path $WorkDir 'no-such-cli.exe') -TimeoutSec 5 -TempDir $WorkDir
Check (-not $read.Ok) 'an absent CLI is a failed read'

# ---------------------------------------------------------------------------------------------
# 3. No trusted temperature means the arm does not start. The launcher proves it: it is never
#    called.
# ---------------------------------------------------------------------------------------------
$script:launched = 0
$countingLauncher = { $script:launched++; Start-QuickChild -Code 0 -Seconds 0.2 }
$r = & $arm -Name 'dead-telemetry' -Dir $WorkDir -Exe 'stub.cmd' -BoundSec 20 `
    -CleanupReserveSec 5 -Sampler $sampleDead -Launcher $countingLauncher `
    -ReportPath (Join-Path $WorkDir 'dead-telemetry.json')
Check ($r.verdict -eq 'REFUSED') "a dead sampler refuses the arm (verdict $($r.verdict))"
Check ($r.exit_status -eq 3) "a refused arm exits 3 (exit_status $($r.exit_status))"
Check ($script:launched -eq 0) 'a refused arm starts no child'
Check ($r.stop_reason -like '*no trusted temperature before the arm*') "the reason says so: '$($r.stop_reason)'"
Check (Test-Path -LiteralPath (Join-Path $WorkDir 'dead-telemetry.json')) 'a refused arm still writes its report'

$r = & $arm -Name 'hot-before' -Dir $WorkDir -Exe 'stub.cmd' -BoundSec 20 `
    -CleanupReserveSec 5 -Sampler $sampleHot -Launcher $countingLauncher `
    -ReportPath (Join-Path $WorkDir 'hot-before.json')
Check ($r.verdict -eq 'REFUSED' -and $r.exit_status -eq 3) '87.5 C before the arm refuses it'
Check ($script:launched -eq 0) 'an arm refused for heat starts no child'

# ---------------------------------------------------------------------------------------------
# 4. A child that never stops by itself, and a bound that must still hold.
# ---------------------------------------------------------------------------------------------
$child = $null
$holdLauncher = { $script:child = Start-StubbornChild; $script:child }
$watch = [Diagnostics.Stopwatch]::StartNew()
$r = & $arm -Name 'stubborn' -Dir $WorkDir -Exe 'stub.cmd' -BoundSec 12 `
    -CleanupReserveSec 6 -SampleEverySec 1 -Sampler $sampleOk -Launcher $holdLauncher `
    -ReportPath (Join-Path $WorkDir 'stubborn.json')
$watch.Stop()
Check ($r.verdict -eq 'STOPPED') "a child that ignores every budget is stopped (verdict $($r.verdict))"
Check ($r.exit_status -eq 4) "a stopped arm exits 4 (exit_status $($r.exit_status))"
Check ($r.stop_reason -like '*work part of the bound is spent*') "the reason names the bound: '$($r.stop_reason)'"
Check ($r.termination_confirmed -eq $true) 'the termination of the child is confirmed'
Check ($watch.Elapsed.TotalSeconds -le 12.0) ("the whole arm fits in its 12 s bound ({0:N1} s)" -f $watch.Elapsed.TotalSeconds)
Check ($script:child.HasExited) 'the child is really gone'
Check ($r.samples -ge 3) "the arm sampled while the child ran ($($r.samples) samples)"

# ---------------------------------------------------------------------------------------------
# 5. A kill that cannot be confirmed is reported as unknown, not as success.
# ---------------------------------------------------------------------------------------------
$liar = { param($p) [pscustomobject]@{ Confirmed = $false; Reason = 'the fake terminator confirms nothing' } }
$script:child2 = $null
$holdLauncher2 = { $script:child2 = Start-StubbornChild; $script:child2 }
$r = & $arm -Name 'unconfirmed' -Dir $WorkDir -Exe 'stub.cmd' -BoundSec 12 `
    -CleanupReserveSec 6 -SampleEverySec 1 -Sampler $sampleOk -Launcher $holdLauncher2 `
    -Terminator $liar -ReportPath (Join-Path $WorkDir 'unconfirmed.json')
Check ($r.verdict -eq 'UNKNOWN') "an unconfirmed kill is UNKNOWN (verdict $($r.verdict))"
Check ($r.exit_status -eq 5) "an unknown arm exits 5 (exit_status $($r.exit_status))"
Check ($r.stop_reason -like '*termination was NOT confirmed*') "the reason says so: '$($r.stop_reason)'"
Check ($r.termination_confirmed -eq $false) 'the report does not claim a confirmed termination'
# The test cleans up what the fake terminator did not.
if ($null -ne $script:child2) { $null = Stop-ArmProcessTree -Process $script:child2 -TimeoutSec 8 }

# ---------------------------------------------------------------------------------------------
# 6. The thermal rules, by the clock.
# ---------------------------------------------------------------------------------------------
$script:child3 = $null
$holdLauncher3 = { $script:child3 = Start-StubbornChild; $script:child3 }
$watch = [Diagnostics.Stopwatch]::StartNew()
$r = & $arm -Name 'at-once' -Dir $WorkDir -Exe 'stub.cmd' -BoundSec 30 `
    -CleanupReserveSec 8 -SampleEverySec 0.5 -Sampler $sampleVeryHotAfterStart -Launcher $holdLauncher3 `
    -ReportPath (Join-Path $WorkDir 'at-once.json')
$watch.Stop()
Check ($r.verdict -eq 'STOPPED' -and $r.stop_reason -like '*at or above 89*') "89.2 C stops the arm at once: '$($r.stop_reason)'"
Check ($watch.Elapsed.TotalSeconds -lt 8) ("and it does not wait for the hold time ({0:N1} s)" -f $watch.Elapsed.TotalSeconds)

# 87 C held. The hold is 3 s here so that the test is short; the default is checked below.
$script:child4 = $null
$holdLauncher4 = { $script:child4 = Start-StubbornChild; $script:child4 }
$watch = [Diagnostics.Stopwatch]::StartNew()
$r = & $arm -Name 'held-hot' -Dir $WorkDir -Exe 'stub.cmd' -BoundSec 30 `
    -CleanupReserveSec 8 -SampleEverySec 0.5 -HotHoldSec 3 -Sampler $sampleHotAfterStart `
    -Launcher $holdLauncher4 -ReportPath (Join-Path $WorkDir 'held-hot.json')
$watch.Stop()
Check ($r.verdict -eq 'STOPPED' -and $r.stop_reason -like '*held at or above 87*') "87.5 C held stops the arm: '$($r.stop_reason)'"
Check ($watch.Elapsed.TotalSeconds -ge 3.0) ("and only after the hold time has passed ({0:N1} s)" -f $watch.Elapsed.TotalSeconds)

# The same temperature, not held: one hot sample between cool ones must not stop the arm. The
# child finishes by itself, so the verdict is DONE.
$script:alternating = 0
$sampleAlternating = {
    $script:alternating++
    if (($script:alternating % 4) -eq 0) {
        [pscustomobject]@{ Ok = $true; Tctl = 87.5; Line = 'temperature_mc=87500'; Reason = '' }
    } else {
        [pscustomobject]@{ Ok = $true; Tctl = 70.0; Line = 'temperature_mc=70000'; Reason = '' }
    }
}
$quick = { Start-QuickChild -Code 0 -Seconds 5 }
$r = & $arm -Name 'hot-but-not-held' -Dir $WorkDir -Exe 'stub.cmd' -BoundSec 30 `
    -CleanupReserveSec 8 -SampleEverySec 0.5 -HotHoldSec 3 -Sampler $sampleAlternating `
    -Launcher $quick -ReportPath (Join-Path $WorkDir 'not-held.json')
Check ($r.verdict -eq 'DONE') "a hot sample that does not hold lets the arm finish (verdict $($r.verdict), reason '$($r.stop_reason)')"
Check ($r.tctl_max -ge 87.0) "and the hot sample is still in the report (max $($r.tctl_max) C)"

# The defaults of the rule are the owner's numbers, not the test's.
$meta = (Get-Command Invoke-LabArm).Parameters
Check ($meta['HotHoldSec'].Attributes.Count -ge 0) 'Invoke-LabArm takes a hold time'
$defaults = (Get-Command Invoke-LabArm).ScriptBlock.Ast.Body.ParamBlock.Parameters |
    Where-Object { $_.Name.VariablePath.UserPath -in @('HotHoldSec', 'HotC', 'StopAtOnceC', 'SampleEverySec', 'BoundSec', 'StaleSec') } |
    ForEach-Object { "$($_.Name.VariablePath.UserPath)=$($_.DefaultValue.Extent.Text)" }
Check (($defaults -join ' ') -like '*HotHoldSec=10*') "the default hold time is 10 s ($($defaults -join ' '))"
Check (($defaults -join ' ') -like '*HotC=87*') 'the default warm threshold is 87 C'
Check (($defaults -join ' ') -like '*StopAtOnceC=89*') 'the default at-once threshold is 89 C'
Check (($defaults -join ' ') -like '*SampleEverySec=3*') 'the default sample cadence is 3 s'
Check (($defaults -join ' ') -like '*BoundSec=170*') 'the default bound is 170 s'

# ---------------------------------------------------------------------------------------------
# 7. Telemetry that stops answering during the arm. Fail closed: the arm stops.
# ---------------------------------------------------------------------------------------------
$script:sampleCount = 0
$sampleDegrading = {
    $script:sampleCount++
    if ($script:sampleCount -le 1) {
        [pscustomobject]@{ Ok = $true; Tctl = 61.0; Line = 'temperature_mc=61000'; Reason = '' }
    } else {
        [pscustomobject]@{ Ok = $false; Tctl = $null; Line = ''; Reason = 'no answer within 3 s' }
    }
}
$script:child5 = $null
$holdLauncher5 = { $script:child5 = Start-StubbornChild; $script:child5 }
$r = & $arm -Name 'stale' -Dir $WorkDir -Exe 'stub.cmd' -BoundSec 30 -CleanupReserveSec 8 `
    -SampleEverySec 0.5 -StaleSec 3 -Sampler $sampleDegrading -Launcher $holdLauncher5 `
    -ReportPath (Join-Path $WorkDir 'stale.json')
Check ($r.verdict -eq 'STOPPED' -and $r.stop_reason -like '*no trusted temperature for*') "stale telemetry stops the arm: '$($r.stop_reason)'"
Check ($r.telemetry_failures -ge 1) "and the report counts the failed reads ($($r.telemetry_failures))"
Check ($r.termination_confirmed -eq $true) 'the child is terminated when the telemetry goes'

# ---------------------------------------------------------------------------------------------
# 8. Cleanup on every path, and the report a session keeps.
# ---------------------------------------------------------------------------------------------
$env:BC250_HIP_BATCH = $null
Remove-Item -Path 'Env:BC250_ARM_TEST' -ErrorAction SilentlyContinue
$script:child6 = $null
$holdLauncher6 = { $script:child6 = Start-StubbornChild; $script:child6 }
$reportPath = Join-Path $WorkDir 'cleanup.json'
$r = & $arm -Name 'cleanup' -Dir $WorkDir -Exe 'stub.cmd' -BoundSec 12 -CleanupReserveSec 6 `
    -SampleEverySec 1 -Sampler $sampleOk -Launcher $holdLauncher6 `
    -ChildEnv @{ BC250_ARM_TEST = '1' } -ReportPath $reportPath
Check ($null -eq (Get-Item -Path 'Env:BC250_ARM_TEST' -ErrorAction SilentlyContinue)) 'the child environment is removed after a stopped arm'
Check (Test-Path -LiteralPath $reportPath) 'the report is on disk'
$json = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
Check ($json.arm -eq 'cleanup') 'the report names the arm'
Check ($json.bound_sec -eq 12 -and $json.work_sec -eq 6) 'the report carries the bound and the work part'
Check ($json.elapsed_sec -gt 0) 'the report carries the elapsed time'
Check ($json.verdict -eq 'STOPPED') 'the report carries the verdict'
Check ($null -ne $json.tctl_before) 'the report carries the temperature before the arm'
Check ($json.env.BC250_ARM_TEST -eq '1') 'the report carries the child environment'

# A child that finishes by itself: its exit code reaches the report and the exit status.
$r = & $arm -Name 'quick' -Dir $WorkDir -Exe 'stub.cmd' -BoundSec 20 -CleanupReserveSec 5 `
    -SampleEverySec 1 -Sampler $sampleOk -Launcher { Start-QuickChild -Code 7 -Seconds 1 } `
    -ReportPath (Join-Path $WorkDir 'quick.json')
Check ($r.verdict -eq 'DONE') "a child that finishes gives DONE (verdict $($r.verdict))"
Check ($r.exit_code -eq 7) "and its exit code reaches the report ($($r.exit_code))"
Check ($r.exit_status -eq 7) "and the arm exits with it ($($r.exit_status))"

# An absent program is a refusal with no child and no temperature read.
$r = & $arm -Name 'absent' -Dir $WorkDir -Exe 'no-such-program.exe' -BoundSec 20 `
    -Sampler $sampleDead -ReportPath (Join-Path $WorkDir 'absent.json')
Check ($r.verdict -eq 'REFUSED' -and $r.exit_status -eq 2) 'an absent program is refused with exit 2'

# ---------------------------------------------------------------------------------------------
# 9. The two wrappers must parse under the shell of the lab, which is Windows PowerShell 5.1.
# ---------------------------------------------------------------------------------------------
foreach ($script in @('armlib.ps1', 'perf-arm.ps1', 'arm3b.ps1')) {
    $path = Join-Path $PSScriptRoot "..\..\lab\$script"
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $path).Path, [ref]$tokens, [ref]$errors)
    Check ($errors.Count -eq 0) "$script parses ($($errors.Count) errors)"
}

Write-Host "test-arm-bounds ($Supervisor): $script:checks checks, $script:failures failed"
if ($script:failures -eq 0) {
    Remove-Item -LiteralPath $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
}
if ($ExpectFailures) {
    # The negative control. The cases must fail on the logic they were written against, or they
    # are not cases.
    if ($script:failures -eq 0) {
        Write-Host "NEGATIVE CONTROL FAILED: $Supervisor passed every check, so these cases prove nothing"
        exit 1
    }
    Write-Host "negative control: $Supervisor fails $script:failures of $script:checks checks, as it must"
    exit 0
}
exit $script:failures

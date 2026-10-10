# armlib.ps1 - the supervisor every M16 HIP lab arm runs under.
#
# Why it exists. The arm wrappers of the 2026-10-09 sessions promised a bound and a thermal rule
# and did not enforce either in a failure path (independent audit of 2026-10-10, finding HIP-F2):
# a missing temperature started the workload, the deadline was read only after a synchronous
# helper call returned, the hold time at 87 C was counted as 3 s a sample instead of by the clock,
# the final WaitForExit had no bound, and the promised cleanup had no try/finally behind it. One
# stalled helper therefore stopped both the time and the thermal enforcement.
#
# What this file guarantees, and what the tests under compute/hip/tests/lab prove without a GPU:
#   1. One deadline for the whole arm, cleanup inside it. The bound cannot exceed 180 s, which is
#      the owner's limit for a non-game lab trial, and a cleanup reserve is kept out of the work
#      part of it.
#   2. Every helper is bounded, and bounded by the time that is left. A helper's own timeout is
#      clipped to the budget the deadline still has, the worst case of a kill included, and a
#      helper is not started at all when there is no room for it. A clock read that does not
#      answer is a failed read, not a wait. The review of 2026-10-10 found the first version of
#      this file short of that: the in-loop read and the read after the arm took their nominal
#      timeouts whatever the clock said, so the shipped defaults could leave the arm at about
#      188 s, over the owner's 180 s ceiling, which is the very failure HIP-F2 reported.
#   3. Fail closed on telemetry. No temperature before the arm means the arm does not start. No
#      fresh temperature during the arm means the arm stops.
#   4. The thermal rule by the clock: stop at once at or above 89 C, and stop when 87 C or more
#      has held for 10 s of elapsed time (not 10 s of counted samples).
#   5. The child is terminated as a tree, and termination is confirmed. If it cannot be
#      confirmed, the arm says so and exits 5 instead of reporting success.
#   6. Cleanup in try/finally: the child environment is restored and the report is written on
#      every path, including an exception.
#   7. A machine-readable report per arm, so the console numbers of a session are not the only
#      copy of its exits, wall times and temperatures (finding HIP-F4).
#
# Windows PowerShell 5.1 is the shell on the lab, so nothing here uses a feature newer than that.
# Nothing in this file touches the lab by itself: the caller passes the program, and the sampler,
# the launcher and the terminator can be replaced, which is how the offline tests run.
#
# Dot-source it:  . (Join-Path $PSScriptRoot 'armlib.ps1')

Set-StrictMode -Version 2.0

$script:ArmMaxBoundSec = 180

# The time constants of the bound, in seconds. They are named here because both the supervisor and
# its tests have to reason about the same worst cases.
#
#   ArmHelperKillSec       the nominal wait for the kill of a helper that did not answer. One read
#                          therefore costs at most its timeout plus this.
#   ArmMinSampleSec        below this there is no room for a bounded temperature read, so no read
#                          is started.
#   ArmTailSlackSec        kept free at the end of the bound for what cannot be clipped: the start
#                          of a process such as taskkill, the write of the report, the shell
#                          itself. Measured at about 1.3 s on the development PC, so the margin is
#                          about twice what it costs there.
#   ArmMinCleanupReserveSec  a cleanup reserve smaller than this cannot hold the tail slack and a
#                          kill at all, so the deadline refuses it. At the floor itself the kill
#                          gets its one second and the temperature read after the arm is dropped
#                          with a note, which is why the wrappers reserve 20 s.
$script:ArmHelperKillSec = 5.0
$script:ArmMinSampleSec = 1.0
$script:ArmTailSlackSec = 3.0
$script:ArmMinCleanupReserveSec = 4

function Get-ArmUtc {
    return [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
}

# The release CLI that reads the clock and the temperature. Absent means no telemetry, which is a
# refusal and not a warning.
function Get-ArmClockCli {
    $key = 'HKLM:\SOFTWARE\amdgpu-wddm\Release'
    try {
        $root = (Get-ItemProperty -Path $key -Name 'InstallRoot' -ErrorAction Stop).InstallRoot
    } catch {
        return $null
    }
    if (-not $root) { return $null }
    $cli = Join-Path $root 'tools\bc250kmd_cli.exe'
    if (-not (Test-Path -LiteralPath $cli)) { return $null }
    return $cli
}

# Terminates a process and everything it started, then confirms it. Confirmed is the only honest
# basis for saying an arm is over.
function Stop-ArmProcessTree {
    param(
        [Parameter(Mandatory = $true)]$Process,
        [double]$TimeoutSec = 10
    )
    $reasons = @()
    if ($null -eq $Process) {
        return [pscustomobject]@{ Confirmed = $true; Reason = 'no child' }
    }
    $targetId = -1
    try { $targetId = [int]$Process.Id } catch { $reasons += "no process id: $_" }
    if ($targetId -gt 0) {
        try {
            $null = & taskkill.exe /PID $targetId /T /F 2>&1
        } catch {
            $reasons += "taskkill failed: $_"
        }
    }
    try {
        if (-not $Process.HasExited) { $Process.Kill() }
    } catch {
        $reasons += "kill failed: $_"
    }
    $confirmed = $false
    try {
        $confirmed = $Process.WaitForExit([int]($TimeoutSec * 1000))
    } catch {
        $reasons += "wait after kill failed: $_"
    }
    if (-not $confirmed) { $reasons += "the child did not exit within $TimeoutSec s of the kill" }
    return [pscustomobject]@{ Confirmed = $confirmed; Reason = ($reasons -join '; ') }
}

# One bounded run of a helper program whose one line of output matters. A helper that does not
# answer inside the bound is killed and the read fails; it never becomes a wait of its own.
function Invoke-ArmBoundedProgram {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string[]]$Argv = @(),
        [double]$TimeoutSec = 10,
        [double]$KillTimeoutSec = 0,
        [string]$TempDir = $env:TEMP
    )
    if ($KillTimeoutSec -le 0) { $KillTimeoutSec = $script:ArmHelperKillSec }
    if (-not (Test-Path -LiteralPath $Path)) {
        return [pscustomobject]@{ Ok = $false; ExitCode = $null; Stdout = ''; Stderr = ''
                                  Reason = "$Path is absent" }
    }
    if (-not (Test-Path -LiteralPath $TempDir)) {
        New-Item -ItemType Directory -Force -Path $TempDir | Out-Null
    }
    $tag = [Guid]::NewGuid().ToString('N')
    $outFile = Join-Path $TempDir "arm-helper-$tag.out"
    $errFile = Join-Path $TempDir "arm-helper-$tag.err"
    $proc = $null
    try {
        $sp = @{ FilePath = $Path; PassThru = $true; NoNewWindow = $true
                 RedirectStandardOutput = $outFile; RedirectStandardError = $errFile }
        if ($Argv.Count -gt 0) { $sp['ArgumentList'] = $Argv }
        $proc = Start-Process @sp
        # Reading .Handle once makes the object keep the handle, without which .ExitCode comes
        # back empty after the process has gone (measured on the lab, 2026-10-09).
        $null = $proc.Handle
        $exited = $proc.WaitForExit([int]($TimeoutSec * 1000))
        if (-not $exited) {
            $stop = Stop-ArmProcessTree -Process $proc -TimeoutSec $KillTimeoutSec
            return [pscustomobject]@{ Ok = $false; ExitCode = $null; Stdout = ''; Stderr = ''
                                      Reason = ("no answer within $TimeoutSec s" +
                                                $(if ($stop.Confirmed) { '' } else { ' and the kill was not confirmed' })) }
        }
        $stdout = ''
        $stderr = ''
        if (Test-Path -LiteralPath $outFile) { $stdout = (Get-Content -LiteralPath $outFile -Raw) }
        if (Test-Path -LiteralPath $errFile) { $stderr = (Get-Content -LiteralPath $errFile -Raw) }
        return [pscustomobject]@{ Ok = ($proc.ExitCode -eq 0); ExitCode = $proc.ExitCode
                                  Stdout = $stdout; Stderr = $stderr
                                  Reason = $(if ($proc.ExitCode -eq 0) { '' } else { "exit $($proc.ExitCode)" }) }
    } catch {
        return [pscustomobject]@{ Ok = $false; ExitCode = $null; Stdout = ''; Stderr = ''
                                  Reason = "cannot run $Path : $_" }
    } finally {
        foreach ($f in @($outFile, $errFile)) {
            if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
        }
    }
}

# One temperature sample. Ok false means there is no trusted temperature, which every caller
# treats as a reason to refuse or to stop, never as a reason to go on.
function Read-ArmTctl {
    param(
        [string]$Cli = '',
        [double]$TimeoutSec = 10,
        [double]$KillTimeoutSec = 0,
        [string]$TempDir = $env:TEMP
    )
    if (-not $Cli) { $Cli = Get-ArmClockCli }
    if (-not $Cli) {
        return [pscustomobject]@{ Ok = $false; Tctl = $null; Line = ''
                                  Reason = 'no release CLI to read the clock with' }
    }
    $run = Invoke-ArmBoundedProgram -Path $Cli -Argv @('clock', 'read') -TimeoutSec $TimeoutSec `
        -KillTimeoutSec $KillTimeoutSec -TempDir $TempDir
    $text = ''
    if ($run.Stdout) { $text = $run.Stdout }
    if ($run.Stderr) { $text = ($text + "`n" + $run.Stderr) }
    $line = ''
    foreach ($candidate in ($text -split "`r?`n")) {
        if ($candidate.Trim()) { $line = $candidate.Trim(); break }
    }
    if (-not $run.Ok) {
        return [pscustomobject]@{ Ok = $false; Tctl = $null; Line = $line
                                  Reason = "the clock read failed: $($run.Reason)" }
    }
    if ($text -match 'temperature_mc=(\d+)') {
        $tctl = [double]$Matches[1] / 1000.0
        # A plausibility band: this part idles near 50 C and the rules act at 87 and 89 C. A
        # reading outside the band is a broken reading, and a broken reading is not a temperature.
        if ($tctl -lt 5.0 -or $tctl -gt 130.0) {
            return [pscustomobject]@{ Ok = $false; Tctl = $null; Line = $line
                                      Reason = "the clock read gave $tctl C, which is outside 5 to 130 C" }
        }
        return [pscustomobject]@{ Ok = $true; Tctl = $tctl; Line = $line; Reason = '' }
    }
    return [pscustomobject]@{ Ok = $false; Tctl = $null; Line = $line
                              Reason = 'the clock read carried no temperature_mc field' }
}

# How a helper's own timeout is cut down to the budget the deadline still has. One bounded read
# costs its timeout plus the wait for the kill of a helper that did not answer, so both parts are
# taken out of the same budget and both come back clipped. The caller must not ask for a read at
# all below ArmMinSampleSec; the floors here only keep the numbers positive.
function Get-ArmHelperBudget {
    param(
        [Parameter(Mandatory = $true)][double]$BudgetSec,
        [Parameter(Mandatory = $true)][double]$TimeoutSec
    )
    $budget = $BudgetSec
    if ($budget -lt $script:ArmMinSampleSec) { $budget = $script:ArmMinSampleSec }
    $kill = $script:ArmHelperKillSec
    if ($kill -gt ($budget * 0.25)) { $kill = $budget * 0.25 }
    if ($kill -lt 0.25) { $kill = 0.25 }
    $timeout = $budget - $kill
    if ($timeout -gt $TimeoutSec) { $timeout = $TimeoutSec }
    if ($timeout -lt 0.25) { $timeout = 0.25 }
    return [pscustomobject]@{ TimeoutSec = $timeout; KillSec = $kill }
}

# The sampler the supervisor uses for real: one bounded clock read whose timeout follows the budget
# it is given. The supervisor passes that budget as the first argument on every call, so a sampler
# that keeps the shape of this one cannot outlive the deadline. A caller that passes no budget gets
# the nominal timeout, which is what the wrappers of 2026-10-09 did on every call.
function New-ArmTctlSampler {
    param(
        [string]$Cli = '',
        [double]$TimeoutSec = 10,
        [string]$TempDir = $env:TEMP
    )
    return {
        param([double]$BudgetSec = 0)
        $t = $TimeoutSec
        $k = 0
        if ($BudgetSec -gt 0) {
            $split = Get-ArmHelperBudget -BudgetSec $BudgetSec -TimeoutSec $TimeoutSec
            $t = $split.TimeoutSec
            $k = $split.KillSec
        }
        Read-ArmTctl -Cli $Cli -TimeoutSec $t -KillTimeoutSec $k -TempDir $TempDir
    }.GetNewClosure()
}

# One sample inside a budget. Below ArmMinSampleSec the sampler is not called: there is no room for
# a read that can be bounded, and a read that cannot be bounded is what put the arm over its limit.
function Invoke-ArmBoundedSample {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Sampler,
        [Parameter(Mandatory = $true)][double]$BudgetSec
    )
    if ($BudgetSec -lt $script:ArmMinSampleSec) {
        return [pscustomobject]@{ Ok = $false; Tctl = $null; Line = ''
                                  Reason = ('no room inside the bound for a temperature read: ' +
                                            ('{0:N1} s left, {1:N1} s needed' -f $BudgetSec, $script:ArmMinSampleSec)) }
    }
    return & $Sampler $BudgetSec
}

# The bound of one arm, with the cleanup time reserved out of it.
function New-ArmDeadline {
    param(
        [int]$BoundSec = 170,
        [int]$CleanupReserveSec = 20,
        [int]$MaxBoundSec = 0
    )
    if ($MaxBoundSec -le 0) { $MaxBoundSec = $script:ArmMaxBoundSec }
    if ($BoundSec -lt 5) {
        throw "the arm bound $BoundSec s is too short to do anything with"
    }
    if ($BoundSec -gt $MaxBoundSec) {
        throw ("the arm bound $BoundSec s is over the $MaxBoundSec s limit of a non-game lab " +
               'trial; cleanup is inside the bound, so the bound cannot be raised here')
    }
    if ($CleanupReserveSec -lt $script:ArmMinCleanupReserveSec) {
        throw ("the cleanup reserve $CleanupReserveSec s is under the " +
               "$script:ArmMinCleanupReserveSec s the tail slack and one confirmed kill need; " +
               'cleanup is inside the bound, so it has to be reserved out of it')
    }
    $work = $BoundSec - $CleanupReserveSec
    if ($work -lt 2) { $work = [int]([math]::Floor($BoundSec / 2)) }
    return [pscustomobject]@{
        Watch = [Diagnostics.Stopwatch]::StartNew()
        TotalSec = $BoundSec
        WorkSec = $work
        CleanupReserveSec = $CleanupReserveSec
    }
}

function Get-ArmElapsedSec {
    param([Parameter(Mandatory = $true)]$Deadline)
    return $Deadline.Watch.Elapsed.TotalSeconds
}

# The time the work part still has: what an in-loop helper may be given.
function Get-ArmWorkLeftSec {
    param([Parameter(Mandatory = $true)]$Deadline)
    return ($Deadline.WorkSec - (Get-ArmElapsedSec -Deadline $Deadline))
}

# The time a cleanup step may be given: what is left of the whole bound, less the tail slack that
# process starts and the report write need, less whatever a later step of the cleanup is owed.
function Get-ArmCleanupLeftSec {
    param(
        [Parameter(Mandatory = $true)]$Deadline,
        [double]$ReserveSec = 0
    )
    $left = $Deadline.TotalSec - (Get-ArmElapsedSec -Deadline $Deadline) - $script:ArmTailSlackSec - $ReserveSec
    if ($left -lt 0) { $left = 0 }
    return $left
}

# The whole supervised arm. Returns the report object and writes it as JSON.
#
# -Sampler, -Launcher and -Terminator exist for the offline tests: each one replaces the real
# thing with a fake (a hung helper, a child that never cooperates, a kill that cannot be
# confirmed), so every failure path is exercised on the development PC with no GPU.
function Invoke-LabArm {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Dir,
        [Parameter(Mandatory = $true)][string]$Exe,
        [string[]]$Argv = @(),
        [hashtable]$ChildEnv = @{},
        [string]$Redirect = '',
        [int]$BoundSec = 170,
        [int]$CleanupReserveSec = 20,
        [int]$MaxBoundSec = 0,
        [double]$SampleEverySec = 3.0,
        [double]$HotHoldSec = 10.0,
        [double]$HotC = 87.0,
        [double]$StopAtOnceC = 89.0,
        [double]$StaleSec = 15.0,
        [double]$HelperTimeoutSec = 10.0,
        [string]$ReportPath = '',
        [scriptblock]$Sampler = $null,
        [scriptblock]$Launcher = $null,
        [scriptblock]$Terminator = $null
    )

    $deadline = New-ArmDeadline -BoundSec $BoundSec -CleanupReserveSec $CleanupReserveSec -MaxBoundSec $MaxBoundSec
    $prog = Join-Path $Dir $Exe
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
    if (-not $ReportPath) { $ReportPath = Join-Path $Dir "$Name-$stamp.report.json" }
    $outFile = Join-Path $Dir "$Name-$stamp.out.txt"
    $errFile = Join-Path $Dir "$Name-$stamp.err.txt"

    if (-not $Sampler) {
        $Sampler = New-ArmTctlSampler -Cli (Get-ArmClockCli) -TimeoutSec $HelperTimeoutSec
    }
    if (-not $Terminator) {
        # The kill takes the budget the cleanup reserve still has, and never less than a second: a
        # child left running is worse than one second over the bound.
        $Terminator = {
            param($p, [double]$BudgetSec = 0)
            $t = 8.0
            if ($BudgetSec -gt 0 -and $BudgetSec -lt $t) { $t = $BudgetSec }
            if ($t -lt 1.0) { $t = 1.0 }
            Stop-ArmProcessTree -Process $p -TimeoutSec $t
        }
    }

    $report = [pscustomobject]@{
        arm = $Name
        utc_start = Get-ArmUtc
        utc_end = ''
        dir = $Dir
        command = (@($Exe) + $Argv) -join ' '
        bound_sec = $deadline.TotalSec
        work_sec = $deadline.WorkSec
        elapsed_sec = 0.0
        verdict = 'not started'
        stop_reason = ''
        exit_code = $null
        termination_confirmed = $null
        samples = 0
        tctl_before = $null
        tctl_min = $null
        tctl_max = $null
        tctl_after = $null
        telemetry_failures = 0
        env = @{}
        stdout_path = $outFile
        stderr_path = $errFile
        redirect = ''
        redirect_sha256 = ''
        cleanup_notes = ''
        exit_status = 2
    }
    foreach ($k in $ChildEnv.Keys) { $report.env[$k] = [string]$ChildEnv[$k] }

    $setNames = @()
    $proc = $null
    $samples = @()
    $stopReason = ''
    $terminationConfirmed = $null

    try {
        if (-not (Test-Path -LiteralPath $prog)) {
            $report.verdict = 'REFUSED'
            $report.stop_reason = "$prog is absent"
            $report.exit_status = 2
            return $report
        }

        # Precondition 1: a trusted temperature. No reading is a refusal. The read is bounded by
        # the work part, so a CLI that does not answer refuses the arm instead of spending it.
        $before = Invoke-ArmBoundedSample -Sampler $Sampler -BudgetSec (Get-ArmWorkLeftSec -Deadline $deadline)
        if (-not $before.Ok) {
            $report.verdict = 'REFUSED'
            $report.stop_reason = "no trusted temperature before the arm: $($before.Reason)"
            $report.telemetry_failures = 1
            $report.exit_status = 3
            return $report
        }
        $report.tctl_before = $before.Tctl
        if ($before.Tctl -ge $HotC) {
            $report.verdict = 'REFUSED'
            $report.stop_reason = "Tctl $($before.Tctl) C is at or above $HotC C"
            $report.exit_status = 3
            return $report
        }

        foreach ($k in $ChildEnv.Keys) {
            Set-Item -Path ("Env:" + $k) -Value ([string]$ChildEnv[$k])
            $setNames += $k
        }

        if ($Launcher) {
            $proc = & $Launcher
        } else {
            $sp = @{ FilePath = $prog; WorkingDirectory = $Dir; PassThru = $true; NoNewWindow = $true
                     RedirectStandardOutput = $outFile; RedirectStandardError = $errFile }
            if ($Argv.Count -gt 0) { $sp['ArgumentList'] = $Argv }
            $proc = Start-Process @sp
            $null = $proc.Handle
        }

        $lastGood = Get-ArmElapsedSec -Deadline $deadline
        $hotSince = $null
        $nextSample = $lastGood + $SampleEverySec
        $telemetryFailures = 0

        while ($true) {
            if ($proc.HasExited) { break }
            $now = Get-ArmElapsedSec -Deadline $deadline
            if ($now -ge $deadline.WorkSec) {
                $stopReason = ("the work part of the bound is spent: {0:N1} s of {1} s " -f $now, $deadline.TotalSec) +
                              "($($deadline.CleanupReserveSec) s are reserved for cleanup)"
                break
            }
            # A read is started only when the work part can still hold a bounded one. Near the
            # deadline the arm waits for the bound instead, which is the whole point of HIP-F2:
            # the read must not be the thing that carries the arm past its own limit.
            $sampleBudget = $deadline.WorkSec - $now
            if ($now -ge $nextSample -and $sampleBudget -ge $script:ArmMinSampleSec) {
                $nextSample = $now + $SampleEverySec
                $sample = Invoke-ArmBoundedSample -Sampler $Sampler -BudgetSec $sampleBudget
                if ($sample.Ok) {
                    $lastGood = Get-ArmElapsedSec -Deadline $deadline
                    $samples += $sample.Tctl
                    if ($sample.Tctl -ge $StopAtOnceC) {
                        $stopReason = "Tctl $($sample.Tctl) C is at or above $StopAtOnceC C"
                        break
                    }
                    if ($sample.Tctl -ge $HotC) {
                        if ($null -eq $hotSince) { $hotSince = $lastGood }
                        $held = $lastGood - $hotSince
                        if ($held -ge $HotHoldSec) {
                            $stopReason = ("Tctl $($sample.Tctl) C held at or above $HotC C for " +
                                           ("{0:N1} s" -f $held))
                            break
                        }
                    } else {
                        $hotSince = $null
                    }
                } else {
                    $telemetryFailures++
                    $age = (Get-ArmElapsedSec -Deadline $deadline) - $lastGood
                    if ($age -ge $StaleSec) {
                        $stopReason = ("no trusted temperature for " + ("{0:N1} s" -f $age) +
                                       ": $($sample.Reason)")
                        break
                    }
                }
            }
            Start-Sleep -Milliseconds 250
        }
        $report.telemetry_failures = $telemetryFailures

        if ($stopReason) {
            # The kill runs inside the cleanup reserve, with the room for the read after the arm
            # held back out of it.
            $stop = & $Terminator $proc (Get-ArmCleanupLeftSec -Deadline $deadline -ReserveSec $script:ArmMinSampleSec)
            $terminationConfirmed = [bool]$stop.Confirmed
            if (-not $terminationConfirmed) {
                $stopReason = ($stopReason + "; termination was NOT confirmed: $($stop.Reason)")
            }
        } else {
            # The child finished by itself. Its exit code still needs a bounded wait, because the
            # streams can be open after the process object says it has exited. The wait keeps the
            # room for the read after the arm out of its own budget.
            $left = Get-ArmCleanupLeftSec -Deadline $deadline -ReserveSec $script:ArmMinSampleSec
            if ($left -lt 0.25) { $left = 0.25 }
            $closed = $false
            try { $closed = $proc.WaitForExit([int]($left * 1000)) } catch { $closed = $false }
            $terminationConfirmed = $closed
            if (-not $closed) {
                $stopReason = 'the child reported exit but its streams did not close inside the bound'
            }
        }

        try { $report.exit_code = $proc.ExitCode } catch { $report.exit_code = $null }
        $report.termination_confirmed = $terminationConfirmed
        $report.stop_reason = $stopReason
        if ($samples.Count -gt 0) {
            $report.samples = $samples.Count
            $report.tctl_min = [math]::Round(($samples | Measure-Object -Minimum).Minimum, 2)
            $report.tctl_max = [math]::Round(($samples | Measure-Object -Maximum).Maximum, 2)
        }

        if (-not $terminationConfirmed) {
            $report.verdict = 'UNKNOWN'
            $report.exit_status = 5
        } elseif ($stopReason) {
            $report.verdict = 'STOPPED'
            $report.exit_status = 4
        } else {
            $report.verdict = 'DONE'
            if ($null -eq $report.exit_code) { $report.exit_status = 5 } else { $report.exit_status = [int]$report.exit_code }
        }

        # The copy and the hash of the kept output are cleanup too, so they happen only while the
        # cleanup reserve still has room. A note in the report says when they did not.
        $notes = @()
        if ($Redirect -and (Test-Path -LiteralPath $outFile)) {
            if ((Get-ArmCleanupLeftSec -Deadline $deadline -ReserveSec $script:ArmMinSampleSec) -le 0) {
                $notes += 'no room inside the bound to copy and hash the kept output'
            } else {
                $target = Join-Path $Dir $Redirect
                Copy-Item -LiteralPath $outFile -Destination $target -Force
                $report.redirect = $target
                $report.redirect_sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
            }
        }

        $after = Invoke-ArmBoundedSample -Sampler $Sampler -BudgetSec (Get-ArmCleanupLeftSec -Deadline $deadline)
        if ($after.Ok) { $report.tctl_after = $after.Tctl } else { $notes += "no temperature after the arm: $($after.Reason)" }
        if ($notes.Count -gt 0) { $report.cleanup_notes = ($notes -join '; ') }
        return $report
    } finally {
        # Every path: the child is not left running, the environment is restored, and the report
        # exists on disk even if something above threw.
        if ($null -ne $proc) {
            $alive = $false
            try { $alive = -not $proc.HasExited } catch { $alive = $false }
            if ($alive) {
                $stop = & $Terminator $proc (Get-ArmCleanupLeftSec -Deadline $deadline)
                $report.termination_confirmed = [bool]$stop.Confirmed
                if (-not $stop.Confirmed) {
                    $report.verdict = 'UNKNOWN'
                    $report.exit_status = 5
                    $report.stop_reason = ($report.stop_reason +
                        "; the child was still running at cleanup and termination was NOT confirmed").Trim('; ')
                }
            }
        }
        foreach ($n in $setNames) {
            Remove-Item -Path ("Env:" + $n) -ErrorAction SilentlyContinue
        }
        $report.elapsed_sec = [math]::Round((Get-ArmElapsedSec -Deadline $deadline), 2)
        $report.utc_end = Get-ArmUtc
        try {
            $json = $report | ConvertTo-Json -Depth 4
            Set-Content -LiteralPath $ReportPath -Value $json -Encoding UTF8
        } catch {
            Write-Host "arm $Name : the report could not be written to $ReportPath : $_"
        }
    }
}

# What the console of an arm says. The same values are in the JSON report, which is the copy that
# survives the session (finding HIP-F4).
function Write-ArmReport {
    param([Parameter(Mandatory = $true)]$Report, [string]$ReportPath = '')
    "arm $($Report.arm) start utc $($Report.utc_start)"
    "arm $($Report.arm) dir $($Report.dir)"
    "arm $($Report.arm) command $($Report.command)"
    "arm $($Report.arm) bound $($Report.bound_sec) s, work $($Report.work_sec) s"
    foreach ($k in $Report.env.Keys) { "arm $($Report.arm) env $k=$($Report.env[$k])" }
    if ($Report.tctl_before) { "arm $($Report.arm) Tctl before $($Report.tctl_before) C" }
    if ($Report.samples -gt 0) {
        "arm $($Report.arm) Tctl during: min $($Report.tctl_min) max $($Report.tctl_max) C over $($Report.samples) samples"
    }
    if ($Report.telemetry_failures -gt 0) {
        "arm $($Report.arm) telemetry read failures $($Report.telemetry_failures)"
    }
    if ($Report.tctl_after) { "arm $($Report.arm) Tctl after $($Report.tctl_after) C" }
    if ($Report.redirect) { "arm $($Report.arm) redirect $($Report.redirect) sha256 $($Report.redirect_sha256)" }
    if ($Report.PSObject.Properties['cleanup_notes'] -and $Report.cleanup_notes) {
        "arm $($Report.arm) cleanup: $($Report.cleanup_notes)"
    }
    if ($Report.stop_reason) { "arm $($Report.arm) stop reason: $($Report.stop_reason)" }
    "arm $($Report.arm) exit $($Report.exit_code) after $($Report.elapsed_sec) s, termination confirmed $($Report.termination_confirmed)"
    "arm $($Report.arm) end $($Report.verdict)"
    if ($ReportPath) { "arm $($Report.arm) report $ReportPath" }
}

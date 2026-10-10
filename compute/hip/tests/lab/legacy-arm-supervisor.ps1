# legacy-arm-supervisor.ps1 - the control flow of the arm wrappers of 2026-10-09, kept here as the
# negative control of test-arm-bounds.ps1 and used nowhere else.
#
# It is the logic the independent audit of 2026-10-10 read out of scratch\m16-hip\lab\arm3b.ps1
# (SHA-256 22624A62D64D903F3973573FF39FC3766D3F59D55B2E5B9F01900559DAF99AC7) and perf-arm.ps1
# (4424B7E0F51CB13A7D4AD6937525D5EB54D0D547385F1F5FFC0D64B71A9C838F), behind the parameter surface
# of Invoke-LabArm, so that the same test cases can run against both:
#
#   * a missing or malformed temperature permits the arm to start (`if ($null -ne $t -and ...)`),
#   * the hold time at 87 C is counted as 3 s a hot sample, not measured by the clock,
#   * the deadline is read only after the sampling call returns,
#   * a failed kill is not detected, and the arm reports the child's exit code as its result,
#   * there is no maximum bound,
#   * cleanup has no try/finally behind it.
#
# One deliberate difference from the original, and it understates the defect: the final wait here
# is bounded at 30 s, where the original called WaitForExit() with no argument at all. An unbounded
# wait would hang the test process instead of failing a check, and a hung test proves nothing that
# a failed check does not.
#
# Nothing in the product line loads this file. It exists to show that the cases of
# test-arm-bounds.ps1 fail on the code they were written against.

Set-StrictMode -Version 2.0

function Invoke-LegacyLabArm {
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

    $report = [pscustomobject]@{
        arm = $Name; utc_start = (Get-ArmUtc); utc_end = ''; dir = $Dir
        command = (@($Exe) + $Argv) -join ' '
        bound_sec = $BoundSec; work_sec = $BoundSec; elapsed_sec = 0.0
        verdict = 'not started'; stop_reason = ''; exit_code = $null
        termination_confirmed = $null; samples = 0; tctl_before = $null; tctl_min = $null
        tctl_max = $null; tctl_after = $null; telemetry_failures = 0; env = @{}
        stdout_path = ''; stderr_path = ''; redirect = ''; redirect_sha256 = ''; exit_status = 2
    }
    foreach ($k in $ChildEnv.Keys) { $report.env[$k] = [string]$ChildEnv[$k] }

    $prog = Join-Path $Dir $Exe
    if (-not (Test-Path -LiteralPath $prog)) {
        $report.verdict = 'REFUSED'; $report.stop_reason = "$prog ABSENT"; $report.exit_status = 2
        return $report
    }

    # The flaw: a reading that is not there is not a reason to refuse.
    $before = & $Sampler
    if ($before.Ok) {
        $report.tctl_before = $before.Tctl
        if ($before.Tctl -ge $HotC) {
            $report.verdict = 'REFUSED'
            $report.stop_reason = "Tctl $($before.Tctl) C is at or above $HotC C"
            $report.exit_status = 3
            return $report
        }
    } else {
        $report.telemetry_failures = 1
    }

    foreach ($k in $ChildEnv.Keys) { Set-Item -Path ("Env:" + $k) -Value ([string]$ChildEnv[$k]) }

    $watch = [Diagnostics.Stopwatch]::StartNew()
    if ($Launcher) { $proc = & $Launcher } else {
        $proc = Start-Process -FilePath $prog -WorkingDirectory $Dir -PassThru -NoNewWindow
        $null = $proc.Handle
    }

    $hot = 0
    $killed = ''
    $samples = @()
    $failures = 0
    while (-not $proc.HasExited) {
        Start-Sleep -Milliseconds ($SampleEverySec * 1000)
        if ($proc.HasExited) { break }
        $s = & $Sampler
        if ($s.Ok) {
            $samples += $s.Tctl
            if ($s.Tctl -ge $StopAtOnceC) { $killed = "Tctl $($s.Tctl) C at or above $StopAtOnceC C"; break }
            # The flaw: 3 s a sample, whatever the clock says.
            if ($s.Tctl -ge $HotC) { $hot += 3 } else { $hot = 0 }
            if ($hot -ge $HotHoldSec) { $killed = "Tctl $($s.Tctl) C held at or above $HotC C for $hot s"; break }
        } else {
            # The flaw: a failed read is not a reason to stop.
            $failures++
        }
        if ($watch.Elapsed.TotalSeconds -gt $BoundSec) { $killed = "bound $BoundSec s spent"; break }
    }
    if ($killed -ne '') {
        if ($Terminator) { $null = & $Terminator $proc } else { try { $proc.Kill() } catch { } }
    }
    # The flaw: the original waits here with no bound and does not ask whether the kill worked.
    $null = $proc.WaitForExit(30000)
    $watch.Stop()

    foreach ($k in $ChildEnv.Keys) { Remove-Item -Path ("Env:" + $k) -ErrorAction SilentlyContinue }

    try { $report.exit_code = $proc.ExitCode } catch { $report.exit_code = $null }
    $report.telemetry_failures = $failures
    $report.stop_reason = $killed
    $report.elapsed_sec = [math]::Round($watch.Elapsed.TotalSeconds, 2)
    if ($samples.Count -gt 0) {
        $report.samples = $samples.Count
        $report.tctl_min = [math]::Round(($samples | Measure-Object -Minimum).Minimum, 2)
        $report.tctl_max = [math]::Round(($samples | Measure-Object -Maximum).Maximum, 2)
    }
    $after = & $Sampler
    if ($after.Ok) { $report.tctl_after = $after.Tctl }
    if ($killed -ne '') {
        $report.verdict = 'STOPPED'; $report.exit_status = 4
    } else {
        $report.verdict = 'DONE'
        if ($null -eq $report.exit_code) { $report.exit_status = 0 } else { $report.exit_status = [int]$report.exit_code }
    }
    $report.utc_end = Get-ArmUtc
    if ($ReportPath) {
        try { Set-Content -LiteralPath $ReportPath -Value ($report | ConvertTo-Json -Depth 4) -Encoding UTF8 } catch { }
    }
    return $report
}

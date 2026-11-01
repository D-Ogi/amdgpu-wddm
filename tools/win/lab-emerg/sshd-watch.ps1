# LAB (Windows PowerShell 5.1, task "Lab sshd watch", SYSTEM, at startup; install-sshd-watch.ps1 registers it):
# the local watch of the BD-051 sshd accept stall.
#
# The stall: sshd completes the TCP handshake of a new connection and sends no banner, for minutes, while the sessions
# it already has keep working. Until now only a game-session script noticed it, on the host, about three minutes in,
# and healed it through the emergency channel (scripts\sshd-heal.sh). A stall outside a session (trials 152, 154 and
# 220) stayed until a person noticed it. This watch reads the banner itself, from 127.0.0.1, once a minute.
#
# The result of a probe is one of these:
#   ok              an "SSH-" line came
#   banner-timeout  connected, and no byte came in -ProbeTimeoutSeconds: the stall signature
#   closed          connected, and the connection closed without a banner: a refusal (MaxStartups, a penalty)
#   connect-timeout no handshake in -ProbeTimeoutSeconds. In the stall the handshake completes, so this is not it
#   refused         no listener: the service is stopped or starts
#   bad-banner      a first line that is not "SSH-"
# Only banner-timeout counts. After -Failures of them in a row, -ConfirmSeconds apart, the watch:
#   1. runs scripts\sshd-stall-forensics.ps1 (read-only: the listener stack, the children tables, the pipe holders,
#      the process audit) in a child powershell.exe with the limit -ForensicsSeconds, into forensics-<utc>.txt,
#   2. restarts the sshd service, with the command of the restart-sshd action of the emergency channel,
#   3. probes again and logs the result.
# A heal needs at least -MinRestartGapSeconds since the last one and fewer than -MaxRestartsPerHour in the last hour.
# A restart also needs the service in the state Running. Otherwise the watch logs the stall and does not act.
#
# Why this shape. One process, and no child process between two probes, because a process-spawn loop is one half of
# the stall (BD-051, trial 220). One probe a minute, because each probe is a connection that closes without
# authentication: PerSourcePenalties counts it (noauth, 1 s each, no block below 15 s), and sshd writes it to
# OpenSSH/Operational, the log that the stall evidence comes from.
#
# Files in -Base\sshd-watch: watch.log (a line for each change of the result, an hourly count and each action;
# rotated at 1 MB), state.json (the last probe), forensics-<utc>.txt, and stop: create it to end the watch. The task
# does not start the watch again after it ends with exit code 0.
param(
    [int]$EverySeconds = 60,
    [int]$ConfirmSeconds = 20,
    [int]$ProbeTimeoutSeconds = 10,
    [int]$Failures = 2,
    [int]$Port = 22,
    [string]$Base = 'C:\BC250\emergency',
    [ValidateSet('restart', 'log-only')][string]$Heal = 'restart',
    [string]$Service = 'sshd',
    [switch]$NoServiceCheck,
    [string]$ForensicsScript = '',
    [int]$ForensicsSeconds = 150,
    [int]$MinRestartGapSeconds = 600,
    [int]$MaxRestartsPerHour = 3,
    [int]$AfterHealSeconds = 5,
    [int]$Cycles = 0
)
$ErrorActionPreference = 'Continue'
if ($Heal -eq 'restart' -and $NoServiceCheck) { throw 'a restart needs the service check' }
$dir = Join-Path $Base 'sshd-watch'
$null = New-Item -ItemType Directory -Force -Path $dir
$logPath = Join-Path $dir 'watch.log'
$statePath = Join-Path $dir 'state.json'
$stopPath = Join-Path $dir 'stop'
$forensicsPath = if ($ForensicsScript) { $ForensicsScript } else { Join-Path $Base 'scripts\sshd-stall-forensics.ps1' }

function Get-Utc { [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ') }

function Write-WatchLog([string]$Text) {
    try {
        if ((Test-Path -LiteralPath $logPath) -and (Get-Item -LiteralPath $logPath).Length -gt 1MB) {
            Move-Item -LiteralPath $logPath -Destination ($logPath + '.1') -Force
        }
        [IO.File]::AppendAllText($logPath, "$(Get-Utc) $Text`r`n", (New-Object Text.UTF8Encoding $false))
    } catch { }
}

function Test-SshBanner([string]$TargetHost, [int]$TargetPort, [int]$TimeoutMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $client = New-Object Net.Sockets.TcpClient
    try {
        try {
            if (-not $client.ConnectAsync($TargetHost, $TargetPort).Wait($TimeoutMs)) {
                return @{ result = 'connect-timeout'; ms = $sw.ElapsedMilliseconds; detail = 'no handshake' }
            }
        } catch {
            return @{ result = 'refused'; ms = $sw.ElapsedMilliseconds; detail = $_.Exception.GetBaseException().Message }
        }
        $stream = $client.GetStream()
        $buf = New-Object byte[] 256
        $got = 0
        while ($got -lt $buf.Length) {
            $left = $TimeoutMs - [int]$sw.ElapsedMilliseconds
            $read = $stream.ReadAsync($buf, $got, $buf.Length - $got)
            if ($left -le 0 -or -not $read.Wait($left)) {
                return @{ result = 'banner-timeout'; ms = $sw.ElapsedMilliseconds; detail = "$got bytes" }
            }
            if ($read.Result -eq 0) { break }
            $got += $read.Result
            if ([Array]::IndexOf($buf, [byte]10, 0, $got) -ge 0) { break }
        }
        $line = ([Text.Encoding]::ASCII.GetString($buf, 0, $got) -split "`r?`n")[0]
        $clean = ($line -replace '[^\x20-\x7E]', '?')
        if ($clean.Length -gt 60) { $clean = $clean.Substring(0, 60) }
        if ($got -eq 0) { return @{ result = 'closed'; ms = $sw.ElapsedMilliseconds; detail = 'no byte' } }
        if ($line.StartsWith('SSH-')) { return @{ result = 'ok'; ms = $sw.ElapsedMilliseconds; detail = $clean } }
        return @{ result = 'bad-banner'; ms = $sw.ElapsedMilliseconds; detail = $clean }
    } catch {
        return @{ result = 'closed'; ms = $sw.ElapsedMilliseconds; detail = $_.Exception.Message }
    } finally {
        $client.Close()
    }
}

function Save-State($Probe, [int]$Run, [int]$CycleIndex, [int]$HealsInHour) {
    try {
        $json = @{ utc = (Get-Utc); result = $Probe.result; ms = [int]$Probe.ms; detail = [string]$Probe.detail;
                   failures_in_a_row = $Run; cycle = $CycleIndex; pid = $PID; heals_last_hour = $HealsInHour } |
            ConvertTo-Json -Compress
        $tmp = $statePath + '.tmp'
        [IO.File]::WriteAllText($tmp, $json, (New-Object Text.UTF8Encoding $false))
        Move-Item -LiteralPath $tmp -Destination $statePath -Force
    } catch { }
}

function Wait-Watch([int]$Seconds) {
    # Steps of one second, so that the stop file ends the watch within a second while it waits.
    $until = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $until) {
        if (Test-Path -LiteralPath $stopPath) { return }
        Start-Sleep -Milliseconds 1000
    }
}

$healTimes = New-Object 'Collections.Generic.List[DateTime]'

function Invoke-Heal {
    $now = [DateTime]::UtcNow
    $recent = @($healTimes | Where-Object { ($now - $_).TotalHours -lt 1 })
    $healTimes.Clear()
    foreach ($t in $recent) { $healTimes.Add($t) }
    if ($healTimes.Count -gt 0 -and ($now - $healTimes[$healTimes.Count - 1]).TotalSeconds -lt $MinRestartGapSeconds) {
        Write-WatchLog ("stall confirmed; no heal: the last heal was {0:N0} s ago, the gap is {1} s" -f ($now - $healTimes[$healTimes.Count - 1]).TotalSeconds, $MinRestartGapSeconds)
        return
    }
    if ($healTimes.Count -ge $MaxRestartsPerHour) {
        Write-WatchLog "stall confirmed; no heal: $($healTimes.Count) heals in the last hour, the limit is $MaxRestartsPerHour"
        return
    }
    $healTimes.Add($now)
    Write-WatchLog "stall confirmed: $Failures banner timeouts in a row; forensics, then the restart"
    if (Test-Path -LiteralPath $forensicsPath) {
        $outFile = Join-Path $dir ('forensics-{0}.txt' -f $now.ToString('yyyyMMddTHHmmssZ'))
        try {
            $child = Start-Process -FilePath 'powershell.exe' -NoNewWindow -PassThru -RedirectStandardOutput $outFile `
                -RedirectStandardError ($outFile + '.err') -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass',
                '-File', "`"$forensicsPath`"", '-Minutes', '15')
            $null = $child.Handle  # keeps the handle, so that ExitCode is readable after the exit
            if ($child.WaitForExit($ForensicsSeconds * 1000)) {
                Write-WatchLog "forensics ended with exit code $($child.ExitCode): $outFile"
            } else {
                $child.Kill()
                Write-WatchLog "forensics stopped at its limit of $ForensicsSeconds s: $outFile"
            }
        } catch { Write-WatchLog "forensics failed: $($_.Exception.Message)" }
    } else {
        Write-WatchLog "no forensics: $forensicsPath does not exist"
    }
    if (-not $NoServiceCheck) {
        $svc = Get-Service -Name $Service -ErrorAction SilentlyContinue
        if (-not $svc -or $svc.Status -ne 'Running') {
            Write-WatchLog "service $Service is not in the state Running: not restarted"
            return
        }
    }
    if ($Heal -eq 'restart') {
        try {
            Restart-Service -Name $Service -Force -ErrorAction Stop
            Write-WatchLog "service $Service restarted"
        } catch {
            Write-WatchLog "restart of $Service failed: $($_.Exception.Message)"
        }
    } else {
        Write-WatchLog "log-only: service $Service not restarted"
    }
    if ($AfterHealSeconds -gt 0) { Start-Sleep -Seconds $AfterHealSeconds }
    $after = Test-SshBanner '127.0.0.1' $Port ($ProbeTimeoutSeconds * 1000)
    Write-WatchLog "after the heal: $($after.result) in $($after.ms) ms ($($after.detail))"
}

Write-WatchLog ("watch starts: pid $PID, port $Port, every $EverySeconds s, confirm $ConfirmSeconds s, timeout " +
    "$ProbeTimeoutSeconds s, $Failures failures, heal $Heal, gap $MinRestartGapSeconds s, max $MaxRestartsPerHour per hour")
$failRun = 0
$lastResult = ''
$cycleNo = 0
$hourProbes = 0
$hourBad = 0
$hourStart = [DateTime]::UtcNow
while ($true) {
    if (Test-Path -LiteralPath $stopPath) { Write-WatchLog 'stop file: the watch ends'; break }
    if ($Cycles -gt 0 -and $cycleNo -ge $Cycles) { Write-WatchLog "watch ends after $cycleNo probes"; break }
    $cycleNo++
    $pause = $EverySeconds
    try {
        $reading = Test-SshBanner '127.0.0.1' $Port ($ProbeTimeoutSeconds * 1000)
        if ($reading.result -eq 'banner-timeout') { $failRun++ } else { $failRun = 0 }
        Save-State $reading $failRun $cycleNo $healTimes.Count
        if ($reading.result -ne $lastResult) {
            Write-WatchLog "probe $($reading.result) in $($reading.ms) ms ($($reading.detail))"
            $lastResult = $reading.result
        }
        $hourProbes++
        if ($reading.result -ne 'ok') { $hourBad++ }
        if (([DateTime]::UtcNow - $hourStart).TotalHours -ge 1) {
            Write-WatchLog "hour: $hourProbes probes, $hourBad not ok"
            $hourProbes = 0; $hourBad = 0; $hourStart = [DateTime]::UtcNow
        }
        if ($failRun -ge $Failures) {
            Invoke-Heal
            $failRun = 0
            $lastResult = ''
        } elseif ($failRun -gt 0) {
            $pause = $ConfirmSeconds
        }
    } catch {
        Write-WatchLog "error: $($_.Exception.Message)"
    }
    if ($Cycles -gt 0 -and $cycleNo -ge $Cycles) { continue }
    Wait-Watch $pause
}
exit 0

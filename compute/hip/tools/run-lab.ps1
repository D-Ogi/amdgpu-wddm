# run-lab.ps1 - the lab side of the step-1 trial of milestone M16.
#
# It performs the six runs of section 6.2 of docs/design/m16-hip-route-b.md in one
# session, under one wall-clock bound, and writes one record with one entry per run. It
# pulls nothing and needs no network: the operator pushes the kit, runs this script, and
# pulls result.json afterwards.
#
# Windows PowerShell 5.1, because that is the shell of the lab. No ternary operator, no
# null-coalescing operator and no pipeline chain operator.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File run-lab.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File run-lab.ps1 -Only selftest
#
# The bound is the whole session, from the first call of the release client to the last
# line of the driver log tail. A non-game lab trial is limited to three minutes, so the
# default is 150 s. Every single call is bounded by the time that is left, so no sum of
# slow answers can carry the session past the bound: the printed bound is the real one.
# -TailReserveSeconds is held back for the clock reading and the log tail, which is the
# evidence of a hung dispatch and must still fit after the last run.
#
# The session also stops at once after a timeout or a lost device: stop rule K5 of the
# design says do not resubmit after a hang.

param(
    [string]$WorkDir = 'C:\BC250\m16\step1',
    [string]$Probe = '',
    [string]$CodeObject = '',
    [string]$Result = '',
    [int]$TimeoutSeconds = 150,
    [int]$N = 1048576,
    [int]$ReduceN = 65536,
    [string]$Grid = '7,3,2',
    [string]$Block = '64,2,1',
    [int]$WaitSliceMs = 1000,
    [int]$WaitTotalMs = 10000,
    [int]$KmdLogTail = 400,
    [int]$TailReserveSeconds = 45,
    [int]$MaxStartTempC = 87,
    [string]$Only = '',
    [switch]$SkipKmdLog,
    [switch]$LogTrace
)

$ErrorActionPreference = 'Stop'

if (-not $Probe) { $Probe = Join-Path $WorkDir 'hipprobe.exe' }
if (-not $CodeObject) { $CodeObject = Join-Path $WorkDir 'm16_kernels.gfx1013.co' }
if (-not $Result) { $Result = Join-Path $WorkDir 'result.json' }
if (-not (Test-Path -LiteralPath $WorkDir)) {
    New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
}
foreach ($needed in @($Probe, $CodeObject)) {
    if (-not (Test-Path -LiteralPath $needed)) {
        Write-Host ('MISSING: ' + $needed)
        exit 2
    }
}

function File-Hash([string]$Path) {
    if (-not $Path) { return '' }
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

# The release client, as the installer registers it. Every other lab kit reads the same
# value, and the form of every call below is listed in tools/release/cli-commands.json.
function Release-Client {
    try {
        $key = Get-ItemProperty -Path 'HKLM:\SOFTWARE\amdgpu-wddm' -Name 'Release InstallRoot' -ErrorAction Stop
        $root = $key.'Release InstallRoot'
    } catch {
        return ''
    }
    if (-not $root) { return '' }
    $cli = Join-Path $root 'tools\bc250kmd_cli.exe'
    if (Test-Path -LiteralPath $cli) { return $cli }
    return ''
}

# One bounded call of any program, with its two streams captured. A program that hangs
# must not take the session with it.
function Invoke-Bounded([string]$Path, [string[]]$Arguments, [int]$Seconds, [string]$OutFile,
                        [string]$ErrFile) {
    $record = [ordered]@{ exit_code = -1; timed_out = $false; seconds = 0; stdout = ''; stderr = '' }
    $temporary = $false
    if (-not $OutFile) {
        $OutFile = [IO.Path]::GetTempFileName()
        $ErrFile = [IO.Path]::GetTempFileName()
        $temporary = $true
    }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        $p = Start-Process -FilePath $Path -ArgumentList $Arguments -NoNewWindow -PassThru `
            -WorkingDirectory $WorkDir -RedirectStandardOutput $OutFile -RedirectStandardError $ErrFile
        # Windows PowerShell 5.1 gives an empty ExitCode on a -PassThru process that it did
        # not wait for itself, because the process handle is already closed. One read of
        # the Handle property keeps the handle, and the exit code then survives the exit.
        # Measured on this lab's shell: without this line every run reports no exit code.
        $null = $p.Handle
        if (-not $p.WaitForExit([Math]::Max(1, $Seconds) * 1000)) {
            $record.timed_out = $true
            try { $p.Kill() } catch { }
            Start-Sleep -Milliseconds 300
        }
        try { $record.exit_code = $p.ExitCode } catch { $record.exit_code = -1 }
        if ($null -eq $record.exit_code) { $record.exit_code = -1 }
    } catch {
        $record.stderr = 'the program did not start: ' + $_.Exception.Message
    }
    $timer.Stop()
    $record.seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3)
    foreach ($pair in @(@('stdout', $OutFile), @('stderr', $ErrFile))) {
        $text = ''
        if (Test-Path -LiteralPath $pair[1]) {
            $text = Get-Content -LiteralPath $pair[1] -Raw -ErrorAction SilentlyContinue
        }
        if ($null -eq $text) { $text = '' }
        if (-not $record[$pair[0]]) { $record[$pair[0]] = $text }
    }
    if ($temporary) { Remove-Item -LiteralPath $OutFile, $ErrFile -Force -ErrorAction SilentlyContinue }
    return $record
}

function Client-Text([string]$Cli, [string[]]$Arguments, [int]$Seconds) {
    if (-not $Cli) { return '' }
    $r = Invoke-Bounded $Cli $Arguments $Seconds '' ''
    if ($r.timed_out) { return '(the client did not answer inside ' + $Seconds + ' s)' }
    return ($r.stdout + $r.stderr)
}

# Tctl in degrees, or -1 for "unknown". The client prints a negative temperature_mc when
# the read fails (bc250kmd_cli.c), so the sign is part of the pattern and a negative
# reading is unknown and not a cold part.
function Temp-C([string]$Text) {
    if ($Text -match 'temperature_mc=(-?\d+)') {
        $c = [double]$Matches[1] / 1000
        if ($c -le 0) { return -1 }
        return $c
    }
    return -1
}

function Lines([string]$Text) {
    if (-not $Text) { return @() }
    return @($Text -split "`r?`n" | Where-Object { $_ -ne '' })
}

# The session clock starts here, in front of the first call of the release client, so that
# every bound below is taken out of one measured budget and record.session_seconds covers
# the whole run.
$session = [Diagnostics.Stopwatch]::StartNew()

# The seconds left of the session bound, minus a reserve that later work still needs.
function Seconds-Left([int]$Reserve) {
    $left = $TimeoutSeconds - [int]$session.Elapsed.TotalSeconds - $Reserve
    if ($left -lt 0) { return 0 }
    return $left
}
# A per-call bound: what the call wants, or all that is left, whichever is smaller.
function Call-Bound([int]$Want, [int]$Reserve) {
    $left = Seconds-Left $Reserve
    if ($left -lt $Want) { return $left }
    return $Want
}

$cli = Release-Client
$record = [ordered]@{
    schema                = 1
    tool                  = 'm16-step1'
    utc_start             = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    host_name             = $env:COMPUTERNAME
    work_dir              = $WorkDir
    probe                 = $Probe
    probe_sha256          = (File-Hash $Probe)
    code_object           = $CodeObject
    code_object_sha256    = (File-Hash $CodeObject)
    release_client        = $cli
    release_client_sha256 = (File-Hash $cli)
    bound_seconds         = $TimeoutSeconds
    tail_reserve_seconds  = $TailReserveSeconds
    kmd_info              = ''
    clock_before          = ''
    clock_after           = ''
    temp_start_c          = -1
    temp_end_c            = -1
    runs                  = @()
    counters              = $null
    kmd_log_pulled        = $false
    kmd_log_lines         = 0
    kmd_log_suspect       = @()
    criteria              = $null
    session_seconds       = 0
    verdict               = 'not run'
    exit_code             = 1
}

if ($cli) {
    $record.kmd_info = Client-Text $cli @('info') (Call-Bound 10 $TailReserveSeconds)
    $record.clock_before = Client-Text $cli @('clock', 'read') (Call-Bound 10 $TailReserveSeconds)
    $record.temp_start_c = Temp-C $record.clock_before
}

# The 87 C rule of this workspace: a trial does not start on a hot part, and it does not
# start on a part whose temperature nobody can read either.
$refusal = ''
if ($record.temp_start_c -lt 0) {
    if ($cli) {
        $refusal = 'refused: the release client did not report Tctl, so the ' +
            $MaxStartTempC + ' C start limit cannot be checked'
    } else {
        $refusal = 'refused: no release client is registered, so Tctl cannot be read and the ' +
            $MaxStartTempC + ' C start limit cannot be checked'
    }
} elseif ($record.temp_start_c -ge $MaxStartTempC) {
    $refusal = 'refused: Tctl ' + $record.temp_start_c + ' C is at or above the ' +
        $MaxStartTempC + ' C start limit'
}
if ($refusal) {
    $record.verdict = $refusal
    $record.exit_code = 4
    $record.session_seconds = [Math]::Round($session.Elapsed.TotalSeconds, 3)
    $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Result -Encoding UTF8
    Write-Host $record.verdict
    exit 4
}

$common = @('--co', $CodeObject, '--wait-slice', [string]$WaitSliceMs,
    '--wait-total', [string]$WaitTotalMs)
if ($LogTrace) { $common += '--verbose' }

# The six runs of section 6.2. Run 0 needs no adapter, run 1 and run 5 only open and
# close it, and the three kernel runs each carry their own bound.
$plan = @(
    [ordered]@{ label = 'selftest';      bound = 15; arguments = @('--selftest') + $common },
    [ordered]@{ label = 'info';          bound = 20; arguments = @('--info') + $common },
    [ordered]@{ label = 'vadd';          bound = 40;
        arguments = @('--kernel', 'vadd', '--n', [string]$N) + $common },
    [ordered]@{ label = 'reduce256';     bound = 40;
        arguments = @('--kernel', 'reduce256', '--n', [string]$ReduceN) + $common },
    [ordered]@{ label = 'writeGridSize'; bound = 40;
        arguments = @('--kernel', 'writeGridSize', '--grid', $Grid, '--block', $Block) + $common },
    [ordered]@{ label = 'info-after';    bound = 20; arguments = @('--info') + $common }
)

$runs = New-Object Collections.Generic.List[object]
$stopped = ''
$stopCode = 5
foreach ($step in $plan) {
    if ($Only -and $Only -ne $step.label) { continue }
    $left = Seconds-Left $TailReserveSeconds
    if ($left -le 1) {
        $stopped = 'the session bound left no time for ' + $step.label
        $stopCode = 6
        break
    }
    $bound = $step.bound
    if ($bound -gt $left) { $bound = $left }

    $outFile = Join-Path $WorkDir ('hipprobe-' + $step.label + '.out.txt')
    $errFile = Join-Path $WorkDir ('hipprobe-' + $step.label + '.err.txt')
    Remove-Item -LiteralPath $outFile, $errFile -Force -ErrorAction SilentlyContinue
    $r = Invoke-Bounded $Probe $step.arguments $bound $outFile $errFile

    $entry = [ordered]@{
        label      = $step.label
        arguments  = $step.arguments
        bound      = $bound
        seconds    = $r.seconds
        exit_code  = $r.exit_code
        timed_out  = $r.timed_out
        stdout     = (Lines $r.stdout)
        stderr     = (Lines $r.stderr)
        json       = @()
    }
    $parsed = New-Object Collections.Generic.List[object]
    foreach ($line in $entry.stdout) {
        if ($line.StartsWith('{')) {
            try { $parsed.Add((ConvertFrom-Json $line)) } catch { }
        }
    }
    $entry.json = $parsed.ToArray()
    $runs.Add($entry)
    Write-Host ('{0,-14} exit {1,2}  {2,7:F3} s' -f $step.label, $r.exit_code, $r.seconds)
    foreach ($line in $entry.stdout) { Write-Host ('  ' + $line) }

    # Stop rule K5: after a timeout or a lost device, nothing is submitted again.
    if ($r.exit_code -eq 3 -or $r.exit_code -eq 4 -or $r.timed_out) {
        $stopped = 'stopped after ' + $step.label + ': a submission is in flight or the device is lost'
        $stopCode = 3
        break
    }
    # A host failure or an adapter that does not open repeats in every later run, and it
    # says nothing new. The session stops and keeps the remaining criteria unknown.
    if ($r.exit_code -eq 2) {
        $stopped = 'stopped after ' + $step.label + ': the tool could not do its host work'
        $stopCode = 5
        break
    }
    if ($r.exit_code -eq 6) {
        $stopped = 'stopped after ' + $step.label + ': the BC-250 adapter did not open'
        $stopCode = 5
        break
    }
}
$record.runs = $runs.ToArray()

# The counters of the last run that printed them. Each run is its own process, so the
# counters count that run alone, and criterion 7 asks every one of them.
$hostcallAny = $null
foreach ($entry in $record.runs) {
    foreach ($object in $entry.json) {
        if ($object.PSObject.Properties.Name -contains 'counters') {
            $record.counters = $object.counters
            if ($null -eq $hostcallAny) { $hostcallAny = $true }
            if ($object.counters.hostcall_buffer_requests -ne 0) { $hostcallAny = $false }
        }
    }
}

# The tail now spends the reserve, so each of its calls is bounded by the time that is
# left of the whole session and not by a number of its own.
if ($cli) {
    $record.clock_after = Client-Text $cli @('clock', 'read') (Call-Bound 10 0)
    $record.temp_end_c = Temp-C $record.clock_after
    if (-not $SkipKmdLog) {
        # The tail of the driver log ring: the header first, which states how many lines
        # this driver load wrote, then the last $KmdLogTail of them.
        $header = Client-Text $cli @('log', '0') (Call-Bound 10 0)
        $from = 0
        if ($header -match 'log\s+(\d+) lines since') {
            $from = [int]$Matches[1] - $KmdLogTail
            if ($from -lt 0) { $from = 0 }
        }
        $tail = Client-Text $cli @('log', [string]$from) (Call-Bound 15 0)
        Set-Content -LiteralPath (Join-Path $WorkDir 'kmd-log.txt') -Value $tail -Encoding UTF8
        Set-Content -LiteralPath (Join-Path $WorkDir 'kmd-summary.txt') `
            -Value (Client-Text $cli @('log', 'summary') (Call-Bound 10 0)) -Encoding UTF8
        $bad = @()
        foreach ($line in (Lines $tail)) {
            if ($line -match 'not run|fault|timeout|TDR|reset') { $bad += $line }
        }
        $record.kmd_log_pulled = $true
        $record.kmd_log_lines = (Lines $tail).Count
        $record.kmd_log_suspect = $bad
    }
}

# ---------------------------------------------------------------------------------------
# The seven pass criteria of section 6.3. Each one is a fact of the record above, so a
# reader does not have to judge, and an unfinished run leaves its criterion unknown.
# ---------------------------------------------------------------------------------------
function Run-Of([string]$Label) {
    foreach ($entry in $record.runs) { if ($entry.label -eq $Label) { return $entry } }
    return $null
}
function Kernel-Of($Entry, [string]$Name) {
    if ($null -eq $Entry) { return $null }
    foreach ($object in $Entry.json) {
        if ($object.PSObject.Properties.Name -contains 'kernel' -and $object.kernel -eq $Name) {
            return $object
        }
    }
    return $null
}
function State([object]$Value) {
    if ($null -eq $Value) { return 'unknown' }
    if ($Value) { return 'pass' }
    return 'FAIL'
}

# PowerShell variable names do not tell capitals apart, so none of these may be called
# $grid, $block or $n: those are parameters of this script.
$vadd = Kernel-Of (Run-Of 'vadd') 'vadd'
$reduce = Kernel-Of (Run-Of 'reduce256') 'reduce256'
$gridRun = Kernel-Of (Run-Of 'writeGridSize') 'writeGridSize'
$infoAfter = Run-Of 'info-after'
$fenceOk = $null
$lost = $false
foreach ($entry in $record.runs) {
    foreach ($object in $entry.json) {
        if ($object.PSObject.Properties.Name -contains 'fence_value' -and $object.fence_value -gt 0) {
            if ($null -eq $fenceOk) { $fenceOk = $true }
            if ($object.fence_read -lt $object.fence_value) { $fenceOk = $false }
        }
        if ($object.PSObject.Properties.Name -contains 'status' -and $object.status -eq 'device-lost') {
            $lost = $true
        }
    }
}
$hostcall = $hostcallAny
$logClean = $null
if ($record.kmd_log_pulled) { $logClean = (@($record.kmd_log_suspect).Count -eq 0) }
$noLoss = $null
if ($null -ne $infoAfter) { $noLoss = (-not $lost -and $infoAfter.exit_code -eq 0) }

$values = [ordered]@{}
if ($null -eq $vadd) { $values['1 vadd matches'] = $null } else { $values['1 vadd matches'] = ($vadd.status -eq 'ok' -and $vadd.mismatches -eq 0) }
$values['2 fence retires once per dispatch'] = $fenceOk
if ($null -eq $reduce) { $values['3 reduce256 matches (LDS_SIZE)'] = $null } else { $values['3 reduce256 matches (LDS_SIZE)'] = ($reduce.status -eq 'ok' -and $reduce.mismatches -eq 0) }
if ($null -eq $gridRun) { $values['4 writeGridSize matches (hidden block)'] = $null } else { $values['4 writeGridSize matches (hidden block)'] = ($gridRun.status -eq 'ok' -and $gridRun.mismatches -eq 0) }
$values['5 driver log clean'] = $logClean
$values['6 no device loss, device reopens'] = $noLoss
$values['7 hostcall_buffer_requests is 0'] = $hostcall

$criteria = [ordered]@{}
$failed = 0
$unknown = 0
foreach ($name in $values.Keys) {
    $state = State $values[$name]
    $criteria[$name] = $state
    if ($state -eq 'FAIL') { $failed++ }
    if ($state -eq 'unknown') { $unknown++ }
}
$record.criteria = $criteria

if ($stopped) {
    $record.verdict = 'FAIL: ' + $stopped
    $record.exit_code = $stopCode
} elseif ($failed -gt 0) {
    $record.verdict = 'FAIL: ' + $failed + ' of the seven pass criteria failed'
    $record.exit_code = 5
} elseif ($unknown -gt 0) {
    $record.verdict = 'INCOMPLETE: ' + $unknown + ' of the seven pass criteria are unknown'
    $record.exit_code = 6
} else {
    $record.verdict = 'PASS: all seven pass criteria of section 6.3 hold'
    $record.exit_code = 0
}
$session.Stop()
$record.session_seconds = [Math]::Round($session.Elapsed.TotalSeconds, 3)

$record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Result -Encoding UTF8
Write-Host ''
foreach ($name in $criteria.Keys) { Write-Host ('{0,-40} {1}' -f $name, $criteria[$name]) }
Write-Host ''
Write-Host $record.verdict
Write-Host ('record: ' + $Result)
exit $record.exit_code

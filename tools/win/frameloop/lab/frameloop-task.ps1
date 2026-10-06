# One set of amdgpu_wddm_frameloop runs in unit A's console session, driven from one SSH call.
#
# SSH on this machine lands in session 0, where a process can create no window and gets no desktop, so a client
# that opens a swap chain over the primary output cannot be started from here directly. The way round it is the
# one the m8/m13 harnesses already used (evidence/windows/2026-09-22-E25-m8-address32, 2026-09-24-E26-desktop-
# resume): a one-shot scheduled task whose principal is the logged-on console user, started from this session,
# polled until it leaves Running, then unregistered. The action is a .cmd wrapper, because a task action cannot
# redirect output and the client's text summary is worth keeping next to its JSON.
#
# Called by lab\run-frameloop.py with a plan file it pushed next to the exe; it is not meant to be run by hand.
# Everything it writes lands under <dir>\runs\<trial>, which must not exist: an attempt is never overwritten.
param([Parameter(Mandatory)][string]$Plan)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Compress-Archive draws a progress bar into the SSH pipe otherwise

. "$PSScriptRoot\cmd-lines.ps1"
$cfg = Get-Content -LiteralPath $Plan -Raw | ConvertFrom-Json
$d = [string]$cfg.dir
$task = 'BC250-FRAMELOOP'

# Admission. Each line is a reason to do nothing at all rather than produce a result nobody can place.
if ($d -ine 'C:\BC250\frameloop') { throw "Directory admission: $d" }
if ($cfg.trial -notmatch '^[A-Za-z0-9._-]{1,48}$') { throw 'Trial name admission' }
$exe = Join-Path $d 'amdgpu_wddm_frameloop.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "No client at $exe" }
$hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
if ($hash -ine [string]$cfg.exe_sha256) { throw "Client hash $hash is not the plan's $($cfg.exe_sha256)" }
$runs = Join-Path (Join-Path $d 'runs') $cfg.trial
if (Test-Path -LiteralPath $runs) { throw "Results for $($cfg.trial) already exist; never overwrite an attempt" }
if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) { throw 'Task exists; inspect it' }

# The competing-task gate of the native-caps kit (package\preflight171.ps1): a trial measuring latency shares
# nothing with another BC250 trial, a DWM experiment or a G0/WSI run.
# The resident overlay and network watchdog are always running; the kit allows exactly these two.
$allowedTasks = @('BC250 monitor overlay', 'BC250 net watchdog')
$busy = @(Get-ScheduledTask | Where-Object { $_.State -eq 'Running' -and $_.TaskName -match 'BC250|DWM|G0|WSI' -and $allowedTasks -notcontains $_.TaskName })
if ($busy.Count) { throw ('Competing tasks running: ' + (($busy | ForEach-Object { $_.TaskName }) -join ', ')) }

# The console session. Only its user can be given a window; the name itself is a lab identifier and stays out
# of the output, so the record says that there is one and in which session.
$user = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $user) { throw 'No interactive session on the lab: nobody is logged on, so no client can be shown' }
$console = @(Get-Process -Name explorer -ErrorAction SilentlyContinue | Select-Object -First 1)
$consoleSession = if ($console.Count) { $console[0].SessionId } else { 0 }
if ($consoleSession -le 0) { throw 'No explorer in a session above 0: the desktop is not up' }

# Temperature before the set, with the owner's 87 C cap (CLAUDE.md, 2026-10-01). The reader is a diagnostic
# tool that may not be deployed; its absence is noted, not fatal.
$rd = 'C:\BC250\bc250rd\bc250rd_cli.exe'
$cap = if ($cfg.temp_cap_c) { [double]$cfg.temp_cap_c } else { 87.0 }
function Read-Tctl {
    if (-not (Test-Path -LiteralPath $rd)) { return $null }
    $raw = & $rd temp 1 1 2>&1 | Out-String
    if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { return [double]$Matches[1] }
    return $null
}
$tctlBefore = Read-Tctl
if ($null -ne $tctlBefore -and $tctlBefore -ge $cap) { throw "Tctl $tctlBefore C is at or above the $cap C cap: cool down first" }

# The operating point of each run, not of the set. Since DPM shipped, the GPU clock is the governor's
# choice and falls as the part warms, so two arms run back to back are two operating points unless the
# record says otherwise. K178 is why this is recorded and not assumed: every game rate of sessions
# 347-403 was silently a 24 CU number, because nothing in the evidence said which.
#
# Both readers are diagnostic tools that may not be deployed; absence is recorded, never fatal.
$cliCandidates = @((Join-Path $env:ProgramFiles 'amdgpu-wddm\tools\bc250kmd_cli.exe'),
                   'C:\BC250\tools\bc250kmd_cli.exe')
$cli = $cliCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
function Read-ClockPoint {
    # `clock read` answers "MHz=<n> VID=<n> temperature_mc=<n> ready=<0|1>" (cli-commands.json).
    if (-not $cli) { return $null }
    $raw = & $cli clock read 2>&1 | Out-String
    if ($raw -match 'MHz=([0-9]+)\s+VID=([0-9]+)\s+temperature_mc=(-?[0-9]+)\s+ready=([0-9]+)') {
        return [ordered]@{ mhz = [int]$Matches[1]; vid = [int]$Matches[2];
            temperature_c = [math]::Round([int]$Matches[3] / 1000.0, 1); ready = [int]$Matches[4] }
    }
    return [ordered]@{ error = 'clock read did not answer in the known shape' }
}
function Read-OperatingPoint {
    [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); tctl_c = Read-Tctl; clock = Read-ClockPoint }
}
# The compute units the driver was asked for. This is the registry record, not the counted units: the
# escape that counts them needs the control DLL, and the overlay's own panel says a record without a
# start that ran the CU stage can be stale. A reader of these numbers checks them against the overlay.
$cuRecord = [ordered]@{ parameters_present = $false }
try {
    $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
    if (Test-Path -LiteralPath $pk) {
        $values = Get-ItemProperty -LiteralPath $pk -ErrorAction Stop
        $cuRecord.parameters_present = $true
        foreach ($name in 'CuMode', 'CuDisableWgp', 'CuModeConfirmed', 'CuModeLastApplied') {
            $cuRecord[$name] = if ($null -ne $values.$name) { [int]$values.$name } else { $null }
        }
    }
} catch { $cuRecord.error = $_.Exception.Message }

$null = New-Item -ItemType Directory -Force -Path $runs
$started = Get-Date
# The deadline is the lab's bound for one trial, not the plan's expectation: a run whose startup costs more than
# expected should not cause the next one to be skipped while there are still seconds in the slot. What keeps the
# set inside the bound is the per-run gate below, which asks for the worst case before starting anything.
$deadlineSeconds = if ($cfg.deadline_seconds) { [int]$cfg.deadline_seconds } else { [int]$cfg.budget_seconds }
$deadline = $started.AddSeconds($deadlineSeconds)
$records = @()

foreach ($run in $cfg.runs) {
    $name = [string]$run.name
    $out = Join-Path $runs ($name + '.json')
    $log = Join-Path $runs ($name + '.txt')
    # Knobs of this run only, as NAME=VALUE; recorded, because a run whose arm is not in the record is not evidence.
    $runEnv = @()
    if ($run.PSObject.Properties.Name -contains 'env' -and $run.env) {
        foreach ($p in $run.env.PSObject.Properties) { $runEnv += ([string]$p.Name + '=' + [string]$p.Value) }
    }
    # The ICD writes its own log next to this run's output unless the plan named a path, so the knobs and counters
    # the client reads back come from this run and are pulled home with it.
    if (-not ($runEnv -match '^BC250_DEFERRED_LOG=')) {
        $runEnv += ('BC250_DEFERRED_LOG=' + (Join-Path $runs ($name + '-icd.log')))
    }
    $record = [ordered]@{ name = $name; seconds = [int]$run.seconds; argline = [string]$run.argline;
        env = $runEnv; started_utc = ''; elapsed_seconds = 0; task_state = ''; exit_code = $null; skipped = '';
        point_before = $null; point_after = $null; }
    # The set's own wall bound, so that a slow run cannot push the last ones past the lab's three minutes. What is
    # asked for is the worst case, not the expectation: this run's seconds plus the slack between the client's
    # watchdog and the poll's deadline below (seconds + 30). The number comes from the plan, so the runner's
    # arithmetic and this check can never disagree.
    $overhead = if ($cfg.run_worst_case_extra_seconds) { [int]$cfg.run_worst_case_extra_seconds } else { 30 }
    $left = ($deadline - (Get-Date)).TotalSeconds
    if ($left -lt ([int]$run.seconds + $overhead)) {
        $record.skipped = ('out of budget: ' + [int]$left + ' s left')
        $records += ,$record
        continue
    }
    # A task action cannot redirect, so the action is a .cmd that does (cmd-lines.ps1, with its own host check).
    # It is checked again here, after writing it: a wrapper that is not exactly those four lines has cmd running
    # something nobody asked for, and the lab is the wrong place to find that out.
    $cmd = Join-Path $runs ($name + '.cmd')
    $lines = New-FrameloopCmdLines -Exe $exe -ArgLine ([string]$run.argline) -Out $out -Log $log -Env $runEnv
    Assert-FrameloopCmdLines -Lines $lines
    Set-Content -LiteralPath $cmd -Value $lines -Encoding ASCII
    Assert-FrameloopCmdLines -Lines @(Get-Content -LiteralPath $cmd)
    $action = New-ScheduledTaskAction -Execute $cmd -WorkingDirectory $runs
    # RunLevel Highest, as the m13 control script used: the client needs no privilege of its own, but it writes
    # its JSON under C:\BC250, where a filtered token may not be allowed to create a file.
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
    # The client's own watchdog asks the loop to stop at seconds + 15 and kills the process at seconds + 20,
    # writing what it has either way. The task limit sits above that at seconds + 25, so a stuck run ends with
    # the client's evidence rather than with SCHED_S_TASK_TERMINATED and nothing to read.
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ([int]$run.seconds + 25)) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
    $record.started_utc = [DateTime]::UtcNow.ToString('o')
    $record.point_before = Read-OperatingPoint
    $begin = Get-Date
    try {
        Start-ScheduledTask -TaskName $task
        $runDeadline = $begin.AddSeconds([int]$run.seconds + 30)
        $state = 'Unknown'
        do {
            # One second, not two: the poll's granularity is pure overhead on every run of the set.
            Start-Sleep -Seconds 1
            $t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
            $state = if ($t) { [string]$t.State } else { 'Missing' }
        } while ($state -eq 'Running' -and (Get-Date) -lt $runDeadline)
        $record.task_state = $state
        $record.elapsed_seconds = [int]((Get-Date) - $begin).TotalSeconds
        if ($state -eq 'Running') { Stop-ScheduledTask -TaskName $task; $record.task_state = 'Stopped-deadline' }
        else { $record.exit_code = (Get-ScheduledTaskInfo -TaskName $task).LastTaskResult }
    } finally {
        Unregister-ScheduledTask -TaskName $task -Confirm:$false
    }
    $record.point_after = Read-OperatingPoint
    $records += ,$record
    Start-Sleep -Seconds 1   # let the previous window and its device go away before the next client starts
}

$tctlAfter = Read-Tctl
$index = [ordered]@{
    trial = [string]$cfg.trial; set = [string]$cfg.set; utc = $started.ToUniversalTime().ToString('o')
    exe_sha256 = $hash; console_session = $consoleSession; budget_seconds = [int]$cfg.budget_seconds
    deadline_seconds = $deadlineSeconds
    elapsed_seconds = [int]((Get-Date) - $started).TotalSeconds
    tctl_before_c = $tctlBefore; tctl_after_c = $tctlAfter; tctl_cap_c = $cap
    cu_record = $cuRecord; clock_reader = $(if ($cli) { 'bc250kmd_cli clock read' } else { 'absent' })
    kmd_version = ''
    runs = $records
}
# Which driver answered. One line, from the same place the kits read it.
try {
    $v = Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceClass='DISPLAY'" -ErrorAction Stop |
        Where-Object { $_.DriverProviderName -match 'BC250|AMD|amdgpu' } | Select-Object -First 1
    if ($v) { $index.kmd_version = [string]$v.DriverVersion }
} catch { }
$index | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runs 'index.json') -Encoding UTF8

$zip = Join-Path $d ('runs-' + $cfg.trial + '.zip')
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $runs '*') -DestinationPath $zip
'frameloop set ' + $cfg.set + ' trial ' + $cfg.trial + ': ' + $records.Count + ' runs, ' + $index.elapsed_seconds + ' s, console session ' + $consoleSession
foreach ($r in $records) {
    if ($r.skipped) { '  ' + $r.name + ': SKIPPED (' + $r.skipped + ')' }
    else { '  ' + $r.name + ': state ' + $r.task_state + ' exit ' + $r.exit_code + ' in ' + $r.elapsed_seconds + ' s' }
}
'archive ' + $zip

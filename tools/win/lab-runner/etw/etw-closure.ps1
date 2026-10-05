# Operator's independent closure receipt for a trial's ETW capture (Codex 850, 854): first the capture task
# (the producer) must be seen not Running, since between windows zero sessions can be listed while it may still
# start the next one; only then are this trial's owned sessions (BC250-N<trial>-*) stopped and the final query
# taken. Query failure or an unfinished logman call is reported as such, never as absence. Removes the one-shot
# task once closure holds. Exit 0 closure established, 1 sessions remain, 2 query failed, 3 producer running,
# 4 a logman helper could not be joined.
param([Parameter(Mandatory)][ValidatePattern('^[0-9]{3}$')][string]$Trial)
$ErrorActionPreference = 'Stop'
$prefix = "BC250-N$Trial-"
$task = "BC250-Etw$Trial"
$tmp = 'C:\BC250\tmp'
$t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
if ($t) {
    $i = Get-ScheduledTaskInfo -TaskName $task
    "task state=$($t.State) last_result=0x$('{0:X}' -f $i.LastTaskResult) last_run=$($i.LastRunTime.ToUniversalTime().ToString('o'))"
    if ($t.State -eq 'Running') { 'producer running: closure NOT established; run again once the task has ended'; exit 3 }
} else { 'task absent' }
# The task's end does not end its helpers: a PerfView or logman process naming this trial's sessions that is
# still alive is an unjoined producer helper.
try {
    $helpers = @(Get-CimInstance Win32_Process -Filter "Name='PerfView.exe' OR Name='logman.exe'" -ErrorAction Stop |
        Where-Object { $_.CommandLine -like "*$prefix*" })
} catch { "helper process query failed: closure NOT established ($($_.Exception.Message))"; exit 2 }
if ($helpers.Count) { "producer helpers alive: $(($helpers | ForEach-Object { "$($_.Name):$($_.ProcessId)" }) -join ','): closure NOT established"; exit 4 }
# One bounded logman call, each with its own output files; @{ ok = exited by itself within $cap s; exit; out }.
# A helper that does not exit even after Kill is unjoined: closure stops there (exit 4) with its PID, and no
# further helper is started, because a zero-session query cannot stand in for that helper's end (Codex 865).
$script:calls = 0
function Logman([string[]]$argv, [int]$cap) {
    $script:calls++
    $out = Join-Path $tmp "etw-closure-$Trial-logman-$PID-$($script:calls).txt"
    $p = Start-Process -FilePath 'logman.exe' -ArgumentList $argv -RedirectStandardOutput $out -RedirectStandardError "$out.err" -NoNewWindow -PassThru
    $null = $p.Handle
    if (!$p.WaitForExit($cap * 1000)) {
        try { $p.Kill() } catch {}
        if (!$p.WaitForExit(5000)) {
            "logman $($argv -join ' ') pid $($p.Id) not joined after Kill: closure NOT established"
            exit 4
        }
        return @{ ok = $false; exit = $null; out = @() }
    }
    return @{ ok = $true; exit = $p.ExitCode; out = @(Get-Content -LiteralPath $out) }
}
function Owned {
    $r = Logman @('query', '-ets') 20
    if (!$r.ok -or $r.exit -ne 0) { return @{ ok = $false; why = "ok=$($r.ok) exit=$($r.exit)"; names = @() } }
    $names = @($r.out | ForEach-Object { ($_ -split '\s+')[0] } | Where-Object { $_ -like "$prefix*" })
    return @{ ok = $true; names = $names }
}
$q = Owned
if (!$q.ok) { "query failed ($($q.why)): closure NOT established"; exit 2 }
"owned sessions before: $($q.names.Count) $($q.names -join ',')"
foreach ($n in $q.names) {
    $r = Logman @('stop', $n, '-ets') 20
    "stop $n ok=$($r.ok) exit $($r.exit)"
}
$q = Owned
if (!$q.ok) { "final query failed ($($q.why)): closure NOT established"; exit 2 }
"owned sessions after: $($q.names.Count) $($q.names -join ',')"
if ($q.names.Count) { 'closure FAILED'; exit 1 }
# The producer was not Running before the stops; recheck so a task started meanwhile is not removed or certified.
$t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
if ($t -and $t.State -eq 'Running') { 'producer started during closure: closure NOT established'; exit 3 }
if ($t) {
    Unregister-ScheduledTask -TaskName $task -Confirm:$false
    'task removed'
}
'closure established: producer ended, no owned ETW session'

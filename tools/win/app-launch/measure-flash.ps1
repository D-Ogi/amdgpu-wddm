# What DWM composes over the start window after logon, frame by frame, with and without the poll of the start
# confirmation (owner, 2026-10-14: "the blue bar flashes for one or two frames every 6-8 s"). Windows PowerShell 5.1,
# over ssh (elevated, session 0), bounded to three minutes:
#
#   python tools\win\target.py push <build>\flash-watch.exe tools\win\app-launch\poll-repro.ps1 tools\win\app-launch\dpm-sample.ps1 <fixed>\start-window.ps1 --to C:\BC250\tmp\flash-kit
#   python tools\win\target.py ps tools\win\app-launch\measure-flash.ps1 -Kit C:\BC250\tmp\flash-kit
#
#   -Kit       the folder with flash-watch.exe (build.ps1 next to this script), poll-repro.ps1, dpm-sample.ps1 and the
#              start-window.ps1 to compare with the installed one (case c); -Watch, -Repro, -Sampler and -Fixed name
#              them one by one instead
#   -Shipped   the installed start-window.ps1 (default: the install's tools folder)
#   -Tools     the installed tools folder, where poll-repro.ps1 takes the CLI, the DLL and start-confirm-core.ps1
#   -Cases     a,b,c by default
#   -CaseSeconds  how long each window stays (default 45)
#
# Cases:
#   a  the installed window, with the poll of start-confirm.ps1 on its real schedule (poll-repro.ps1, read-only)
#   b  the installed window, no poll
#   c  the -Fixed window, with the poll
# Each case starts flash-watch.exe in the console session, as the user logged on there, through a scheduled task with
# the limited run level (as the logon task "amdgpu-wddm start window"). flash-watch starts the window with the task's
# own arguments and records every frame that DWM composes over the window's rectangle (Desktop Duplication, or GDI
# reads of the screen when duplication cannot start). The poll runs here, in session 0, as the confirmation's task
# runs in the services session. Both write QueryPerformanceCounter ticks, so each flash is placed against the poll's
# steps exactly. A flash is a content of the window's rectangle that stayed less than 60 ms between two others.
# dpm-sample.ps1 reads the GPU clock and the DPM governor's state once a second for all the cases (the read the
# overlay makes too), on the same clock, so each flash is also placed against the clock changes.
#
# The two scripts run as copies in the work folder, where no control application sits next to them, so the window
# starts no sign-in notice. The script refuses to run while the logon tasks or their scripts run, and when nobody is
# logged on at the console. It stops only the processes it started (the poll, the DPM sampler, and the window
# flash-watch started when flash-watch could not end it), registers one task and removes it on every exit path, and writes only under
# -Work. The results stay in C:\BC250\tmp\flash\<utc>\ and on standard output.
#
# Exit codes: 0 every case ran, 1 a case failed or did not fit in the bound, 2 refused before any case.
param(
    [string]$Kit = $PSScriptRoot,
    [string]$Watch = (Join-Path $Kit 'flash-watch.exe'),
    [string]$Repro = (Join-Path $Kit 'poll-repro.ps1'),
    [string]$Sampler = (Join-Path $Kit 'dpm-sample.ps1'),
    [string]$Fixed = (Join-Path $Kit 'start-window.ps1'),
    [string]$Shipped = 'C:\Program Files\amdgpu-wddm\tools\start-window.ps1',
    [string]$Tools = 'C:\Program Files\amdgpu-wddm\tools',
    [string]$Cases = 'a,b,c',
    [int]$CaseSeconds = 45,
    [int]$BoundSeconds = 170,
    [string]$Work = 'C:\BC250\tmp\flash',
    [string]$TaskName = 'BC250 flash probe'
)
$ErrorActionPreference = 'Continue'
$clock = [Diagnostics.Stopwatch]::StartNew()
$bad = @()
$ours = @()
$rows = @()
$samplerProcess = $null
$inv = [Globalization.CultureInfo]::InvariantCulture
# The DPM values whose change between two samples counts as a clock transition.
$dpmKeys = @('mode', 'throttle', 'target_mhz', 'current_mhz', 'observed_mhz', 'raises', 'lowers', 'idle_entries', 'idle_exits')

function Remove-Probe {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    }
}

function Get-Field([string]$Line, [string]$Key) {
    if ($Line -match "(^| )$([regex]::Escape($Key))=(\S+)") { return $Matches[2] } else { return $null }
}

# The poll's steps of one case: @{ poll; step; t0; t1 } in seconds since the window appeared.
function Read-Polls([string]$Path, [long]$Zero, [double]$Freq) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    return @(Import-Csv -LiteralPath $Path | ForEach-Object {
        [pscustomobject]@{ poll = [int]$_.poll; step = $_.step; t0 = ([long]$_.qpc0 - $Zero) / $Freq; t1 = ([long]$_.qpc1 - $Zero) / $Freq; result = $_.result }
    })
}

# Where a moment falls against the poll: inside a step, or how far from the nearest step.
function Format-AgainstPoll([double]$T, $Steps) {
    if (-not $Steps.Count) { return 'no poll' }
    foreach ($s in $Steps) {
        if ($T -ge $s.t0 -and $T -le $s.t1) {
            return [string]::Format($inv, 'inside poll {0} {1} (+{2:F3} s of {3:F3} s)', $s.poll, $s.step, ($T - $s.t0), ($s.t1 - $s.t0))
        }
    }
    $near = $Steps | Sort-Object { [Math]::Min([Math]::Abs($T - $_.t0), [Math]::Abs($T - $_.t1)) } | Select-Object -First 1
    $d = $(if ($T -lt $near.t0) { $T - $near.t0 } else { $T - $near.t1 })
    return [string]::Format($inv, 'outside the poll, {0:+0.000;-0.000} s from poll {1} {2}', $d, $near.poll, $near.step)
}

# The DPM samples that read without an error: @{ t; row } in seconds since the window appeared.
function Read-Dpm([string]$Path, [long]$Zero, [double]$Freq) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    return @(Import-Csv -LiteralPath $Path | Where-Object { $_.status -eq '0x00000000' } | ForEach-Object {
        [pscustomobject]@{ t = ([long]$_.qpc - $Zero) / $Freq; row = $_ }
    })
}

# The changes between two samples in a row: @{ t0; t1; what }. The change happened somewhere in (t0, t1].
function Get-DpmChanges($Samples) {
    $changes = @()
    for ($i = 1; $i -lt $Samples.Count; $i++) {
        $a = $Samples[$i - 1].row
        $b = $Samples[$i].row
        $what = @($dpmKeys | Where-Object { $a.$_ -ne $b.$_ } | ForEach-Object { "$_ $($a.$_)->$($b.$_)" })
        if ($what.Count) { $changes += [pscustomobject]@{ t0 = $Samples[$i - 1].t; t1 = $Samples[$i].t; what = $what -join ' ' } }
    }
    return $changes
}

# The DPM state before a moment, and the change in the same sample interval or the nearest one.
function Format-AgainstDpm([double]$T, $Samples, $Changes) {
    if (-not $Samples.Count) { return 'no DPM samples' }
    $before = $Samples | Where-Object { $_.t -le $T } | Select-Object -Last 1
    $state = $(if ($before) { "dpm $($before.row.current_mhz) MHz (target $($before.row.target_mhz), observed $($before.row.observed_mhz), idle entries $($before.row.idle_entries))" } else { 'dpm: before the first sample' })
    $in = $Changes | Where-Object { $T -gt $_.t0 -and $T -le $_.t1 } | Select-Object -First 1
    if ($in) { return [string]::Format($inv, '{0}; changed in the interval of the flash, +{1:F3}..+{2:F3} s: {3}', $state, $in.t0, $in.t1, $in.what) }
    if (-not $Changes.Count) { return "$state; no DPM change in the case" }
    $near = $Changes | Sort-Object { [Math]::Min([Math]::Abs($T - $_.t0), [Math]::Abs($T - $_.t1)) } | Select-Object -First 1
    $d = $(if ($T -le $near.t0) { $T - $near.t0 } else { $T - $near.t1 })
    return [string]::Format($inv, '{0}; nearest change {1:+0.000;-0.000} s: {2}', $state, $d, $near.what)
}

function Invoke-Case([string]$Case) {
    $windowScript = $(if ($Case -eq 'c') { $fixedCopy } else { $shippedCopy })
    $withPoll = ($Case -ne 'b')
    $prefix = Join-Path $dir $Case
    $left = [int]($BoundSeconds - $clock.Elapsed.TotalSeconds) - 6
    if ($left -lt $CaseSeconds + 6) { $script:bad += "case ${Case}: skipped, no time left"; return }
    $poll = $null
    if ($withPoll) {
        $poll = Start-Process -FilePath $ps -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass',
            '-File', ('"' + $Repro + '"'), '-Tools', ('"' + $Tools + '"'), '-Seconds', ($CaseSeconds + 1), '-Out', ('"' + "$prefix-polls.csv" + '"'))
        $script:ours += $poll.Id
    }
    Remove-Probe
    $deadlineMs = ($CaseSeconds + 4) * 1000
    $argText = (@($prefix, $deadlineMs, $ps, $windowScript, $CaseSeconds) | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ($CaseSeconds + 30)) `
        -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    $action = New-ScheduledTaskAction -Execute $watchCopy -Argument $argText -WorkingDirectory $dir
    Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Settings $settings | Out-Null
    Start-ScheduledTask -TaskName $TaskName
    $end = [DateTime]::UtcNow.AddSeconds($CaseSeconds + 12)
    do {
        Start-Sleep -Milliseconds 500
        $state = (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue).State
    } while ($state -eq 'Running' -and [DateTime]::UtcNow -lt $end)
    if ($state -eq 'Running') {
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        $script:bad += "case ${Case}: flash-watch did not end inside its bound"
    }
    Remove-Probe
    if ($poll) {
        if (-not $poll.WaitForExit(8000)) {
            Stop-Process -Id $poll.Id -Force -ErrorAction SilentlyContinue
            $script:bad += "case ${Case}: the poll did not end by itself, stopped"
        }
    }

    $log = @(if (Test-Path -LiteralPath "$prefix-log.txt") { Get-Content -LiteralPath "$prefix-log.txt" })
    $child = $log | Where-Object { $_ -match '^child pid=' } | Select-Object -First 1
    if ($child) {
        # The window that flash-watch started, if it is somehow still there: ours, and only if it runs our copy.
        $id = [int](Get-Field $child 'pid')
        $p = Get-CimInstance Win32_Process -Filter "ProcessId=$id" -ErrorAction SilentlyContinue
        if ($p -and ([string]$p.CommandLine).Contains($dir)) {
            Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
            $script:bad += "case ${Case}: the window process was still running at the end, stopped"
        }
    }
    $start = $log | Where-Object { $_ -match '^start ' } | Select-Object -First 1
    $window = $log | Where-Object { $_ -match '^target window ' } | Select-Object -First 1
    $overlay = $log | Where-Object { $_ -match '^target overlay ' } | Select-Object -First 1
    $endLine = $log | Where-Object { $_ -match '^end ' } | Select-Object -Last 1
    $mode = (@($log | Where-Object { $_ -match '^mode=' }) | ForEach-Object { ($_ -split ' ')[0] }) -join ' then '
    if (-not $start -or -not $endLine) { $script:bad += "case ${Case}: flash-watch wrote no complete log"; return }
    if (-not $window) { $script:bad += "case ${Case}: no window appeared ($endLine)"; return }
    $freq = [double](Get-Field $start 'freq')
    $zero = [long](Get-Field $window 'qpc')
    $steps = Read-Polls "$prefix-polls.csv" $zero $freq
    $polls = @($steps | ForEach-Object { $_.poll } | Sort-Object -Unique).Count
    $busy = 0.0
    foreach ($s in $steps) { $busy += ($s.t1 - $s.t0) }
    $script:rows += [string]::Format($inv, 'case {0} ({1}, {2})  {3}  window {4}  overlay {5}  {6}  polls {7}, busy {8:F1} s of {9} s',
        $Case, $(if ($Case -eq 'c') { 'fixed window' } else { 'installed window' }), $(if ($withPoll) { 'poll' } else { 'no poll' }),
        $mode, (Get-Field $window 'rect'), $(if ($overlay) { Get-Field $overlay 'rect' } else { 'none' }), ($endLine -replace '^end ', ''),
        $polls, $busy, $CaseSeconds)
    foreach ($l in ($log | Where-Object { $_ -match '^summary ' })) { $script:rows += "  $($l -replace '^summary ', '')" }
    # The DPM samples of this case's window, one before it and one after it included.
    $dpmAll = Read-Dpm "$dir\dpm.csv" $zero $freq
    $dpm = @($dpmAll | Where-Object { $_.t -ge -1.5 -and $_.t -le $CaseSeconds + 1.5 })
    $changes = @(Get-DpmChanges $dpm)
    if ($dpm.Count) {
        $mhz = @($dpm | ForEach-Object { [int]$_.row.current_mhz } | Sort-Object)
        $script:rows += [string]::Format($inv, '  dpm: {0} samples, {1}..{2} MHz, a change in {3} of {4} sample intervals',
            $dpm.Count, $mhz[0], $mhz[-1], $changes.Count, [Math]::Max(0, $dpm.Count - 1))
        foreach ($c in $changes) { $script:rows += [string]::Format($inv, '    dpm change in +{0:F3}..+{1:F3} s: {2}', $c.t0, $c.t1, $c.what) }
    } else { $script:rows += '  dpm: no samples' }
    foreach ($target in @('window', 'overlay')) {
        $flashes = @($log | Where-Object { $_ -match "^flash target=$target " })
        $inside = 0
        $inDpm = 0
        foreach ($f in $flashes) {
            $t = ([long](Get-Field $f 'qpc') - $zero) / $freq
            $where = Format-AgainstPoll $t $steps
            if ($where -like 'inside*') { $inside++ }
            $clockAt = Format-AgainstDpm $t $dpm $changes
            if ($clockAt -like '*changed in the interval*') { $inDpm++ }
            $script:rows += [string]::Format($inv, '  {0} flash at +{1:F3} s  {2} ms  {3}  navy {4} cyan {5} black {6} other {7}  {8}  bmp {9}',
                $target, $t, (Get-Field $f 'ms'), (Get-Field $f 'kind'), (Get-Field $f 'navy'), (Get-Field $f 'cyan'),
                (Get-Field $f 'black'), (Get-Field $f 'other'), $where, (Get-Field $f 'bmp'))
            $script:rows += [string]::Format($inv, '    frame {0} ({1}, present_qpc {2}, accumulated {3}); {4}',
                (Get-Field $f 'frame'), (Get-Field $f 'mode'), (Get-Field $f 'present'), (Get-Field $f 'accumulated'), $clockAt)
        }
        # Against chance: the share of flashes inside a poll step next to the share of the time the steps take, and
        # the share in a sample interval with a DPM change next to the share of such intervals.
        if ($flashes.Count -and $steps.Count) {
            $script:rows += [string]::Format($inv, '  {0}: {1} of {2} flashes inside a poll step; the steps take {3:F1} % of the {4} s',
                $target, $inside, $flashes.Count, (100.0 * $busy / $CaseSeconds), $CaseSeconds)
        }
        if ($flashes.Count -and $dpm.Count -gt 1) {
            $script:rows += [string]::Format($inv, '  {0}: {1} of {2} flashes in a sample interval with a DPM change; {3:F1} % of the intervals hold one',
                $target, $inDpm, $flashes.Count, (100.0 * $changes.Count / ($dpm.Count - 1)))
        }
    }
    foreach ($s in ($steps | Where-Object { $_.step -eq 'health' })) {
        $all = @($steps | Where-Object { $_.poll -eq $s.poll })
        $script:rows += [string]::Format($inv, '  poll {0} at +{1:F3} s: {2}', $s.poll, $s.t0,
            (($all | ForEach-Object { [string]::Format($inv, '{0} {1:F3} s ({2})', $_.step, ($_.t1 - $_.t0), $_.result) }) -join ', '))
    }
    if ($log | Where-Object { $_ -match '^watchdog' }) { $script:bad += "case ${Case}: the watchdog of flash-watch fired" }
}

try {
    "utc $([DateTime]::UtcNow.ToString('o'))"
    if ($BoundSeconds -gt 175) { throw 'refused: the bound of a lab trial is three minutes' }
    $caseList = @($Cases -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if (@($caseList | Where-Object { $_ -notin @('a', 'b', 'c') }).Count) { throw "refused: cases are a, b and c, not '$Cases'" }
    if ($caseList.Count * ($CaseSeconds + 5) + 10 -gt $BoundSeconds) { throw "refused: $($caseList.Count) cases of $CaseSeconds s do not fit in $BoundSeconds s" }
    $need = @($Watch, $Repro, $Sampler, $Shipped, (Join-Path $Tools 'bc250kmd_cli.exe'), (Join-Path $Tools 'bc250control.dll'), (Join-Path $Tools 'start-confirm-core.ps1'))
    if ($caseList -contains 'c') { $need += $Fixed }
    foreach ($f in $need) { if (-not $f -or -not (Test-Path -LiteralPath $f -PathType Leaf)) { throw "refused: no file '$f'" } }

    $explorer = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $explorer) { throw 'refused: no explorer.exe, so nobody is logged on at the console' }
    $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwner
    $user = '{0}\{1}' -f $owner.Domain, $owner.User
    "console user $user session $($explorer.SessionId)"
    foreach ($t in @('amdgpu-wddm start window', 'amdgpu-wddm start confirm')) {
        $task = Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue
        if ($task -and $task.State -eq 'Running') { throw "refused: the task '$t' runs; wait until it ends" }
    }
    $running = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
        Where-Object { [string]$_.CommandLine -match 'start-window\.ps1|start-confirm\.ps1' })
    if ($running.Count) { throw "refused: a start window or confirmation runs (pid $(($running | ForEach-Object { $_.ProcessId }) -join ', ')); this script stops no process it did not start" }
    $overlay = @(Get-Process -Name 'bc250mon' -ErrorAction SilentlyContinue)
    "overlay bc250mon $(if ($overlay.Count) { "runs (pid $(($overlay | ForEach-Object { $_.Id }) -join ', '))" } else { 'not running' })"

    $ps = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $dir = Join-Path $Work ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
    [void][IO.Directory]::CreateDirectory((Join-Path $dir 'shipped'))
    [void][IO.Directory]::CreateDirectory((Join-Path $dir 'fixed'))
    & icacls.exe $dir /grant "${user}:(OI)(CI)M" | Out-Null
    # Copies, so that the window finds no control application next to it and starts no sign-in notice.
    $shippedCopy = Join-Path $dir 'shipped\start-window.ps1'
    $fixedCopy = Join-Path $dir 'fixed\start-window.ps1'
    $watchCopy = Join-Path $dir 'flash-watch.exe'
    Copy-Item -LiteralPath $Shipped -Destination $shippedCopy
    if ($caseList -contains 'c') { Copy-Item -LiteralPath $Fixed -Destination $fixedCopy }
    Copy-Item -LiteralPath $Watch -Destination $watchCopy
    "work $dir"
    foreach ($f in @($watchCopy, $shippedCopy, $(if ($caseList -contains 'c') { $fixedCopy }), $Repro, $Sampler, (Join-Path $Tools 'bc250kmd_cli.exe'), (Join-Path $Tools 'bc250control.dll'))) {
        if ($f) { "file $f sha256 $((Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash)" }
    }
    # The clock for all the cases; it ends by itself inside the bound, and is stopped after the last case.
    $samplerProcess = Start-Process -FilePath $ps -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', ('"' + $Sampler + '"'), '-Tools', ('"' + $Tools + '"'), '-Seconds', ([int]($BoundSeconds - $clock.Elapsed.TotalSeconds) - 4),
        '-PeriodMs', 1000, '-Out', ('"' + (Join-Path $dir 'dpm.csv') + '"'))
    Start-Sleep -Seconds 2
    foreach ($c in $caseList) { Invoke-Case $c }
}
catch { $bad += "$($_.Exception.Message)" }
finally {
    Remove-Probe
    if ($samplerProcess -and -not $samplerProcess.HasExited) { Stop-Process -Id $samplerProcess.Id -Force -ErrorAction SilentlyContinue }
    if ($samplerProcess -and -not (Test-Path -LiteralPath (Join-Path $dir 'dpm.csv'))) { $bad += 'the DPM sampler wrote no dpm.csv' }
    foreach ($id in ($ours | Sort-Object -Unique)) {
        if (Get-Process -Id $id -ErrorAction SilentlyContinue) {
            Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
            $bad += "process $id (the poll) was still running at the end, stopped"
        }
    }
}

''
foreach ($r in $rows) { $r }
"elapsed $([int]$clock.Elapsed.TotalSeconds) s"
if ($bad.Count) {
    foreach ($b in $bad) { "problem: $b" }
    if (@($bad | Where-Object { $_ -like 'refused:*' }).Count) { 'flash: REFUSED'; exit 2 }
    "flash: FAIL, $($bad.Count) problem(s)"
    exit 1
}
'flash: ok'
exit 0

# Does the logon window task give the person a console window? (fact M899; on 2026-10-14 a console window came
# up with the start window). Windows PowerShell 5.1, over ssh (elevated, session 0), bounded to three minutes:
#
#   python tools\win\target.py push <build>\amdgpu_wddm_control.exe <build>\bc250control.dll tools\release\installer\common.ps1 --to C:\BC250\tmp\console-kit
#   python tools\win\target.py ps tools\win\app-launch\console-proof.ps1 -Kit C:\BC250\tmp\console-kit
#
#   -Kit      the folder with the control application to test (amdgpu_wddm_control.exe and its bc250control.dll) and
#             the installer's common.ps1 of the same source (Get-LogonTaskPlan, Test-LogonTaskPlan)
#   -Shipped  the start-window.ps1 the window runs (default: the installed one)
#   -Forms    a,b by default
#   -Seconds  how long each window stays (default 15)
#   -Plan     print what a run would do and exit: no task, no process, no file, on any computer
#
# Forms, both built by common.ps1 Get-LogonTaskPlan and held against Test-LogonTaskPlan before they run:
#   a  powershell.exe runs start-window.ps1 (the plan without -WindowHost: what tester.31 rc4 and older register)
#   b  the control application runs it (amdgpu_wddm_control.exe --start-window, the plan with -WindowHost)
# The run lays out a copy of an install under -Work: control\ (the kit's application and DLL) and tools\ (a copy of
# -Shipped), so form b finds its own script where an install puts it. For each form it registers one task with the
# form's program and arguments, for the user logged on at the console with the limited run level (as the logon task),
# starts it, and records every process that Windows creates in the console session until the task has ended (WMI
# Win32_ProcessStartTrace, with the command line of each process read at once). A console window that Windows
# Terminal hosts comes with an OpenConsole.exe (one for each console it takes over) and a WindowsTerminal.exe when no
# Terminal process runs yet. Form a is the positive control: without one of these the trace cannot see the console
# window, and the run gives no verdict. Form b passes when its task returns 0, the application started powershell.exe
# over the copy of the script, and Windows created no OpenConsole.exe and no WindowsTerminal.exe while it ran.
# The default terminal of the console user (HKU\<sid>\Console\%%Startup, read only) is printed with the results.
#
# The script refuses to run while the logon tasks or their scripts run, and when nobody is logged on at the console.
# It stops only what it started (its task, and a window process of its copy when the task could not end it),
# registers one task and removes it on every exit path, and writes only under -Work. A Terminal window of form a that
# stays open is reported, not closed: its process can hold windows that this script did not open.
#
# Exit codes: 0 form b gave no console window and form a gave one, 1 form b gave a console window or failed,
# 2 refused before any form, 3 no verdict (form a showed no console window, or a form did not run).
param(
    [string]$Kit = $PSScriptRoot,
    [string]$Application = (Join-Path $Kit 'amdgpu_wddm_control.exe'),
    [string]$Dll = (Join-Path $Kit 'bc250control.dll'),
    [string]$Common = (Join-Path $Kit 'common.ps1'),
    [string]$Shipped = 'C:\Program Files\amdgpu-wddm\tools\start-window.ps1',
    [string]$Forms = 'a,b',
    [int]$Seconds = 15,
    [int]$BoundSeconds = 170,
    [string]$Work = 'C:\BC250\tmp\console-proof',
    [string]$TaskName = 'BC250 console probe',
    [switch]$Plan
)
$ErrorActionPreference = 'Continue'
$clock = [Diagnostics.Stopwatch]::StartNew()
$bad = @()
$verdict = @{}
$traceId = 'BC250ConsoleProbe'
$terminalNames = @('OpenConsole.exe', 'WindowsTerminal.exe')

function Remove-Probe {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    }
}

function Stop-Trace {
    Get-EventSubscriber -SourceIdentifier $traceId -ErrorAction SilentlyContinue | Unregister-Event -ErrorAction SilentlyContinue
    Get-Event -SourceIdentifier $traceId -ErrorAction SilentlyContinue | Remove-Event -ErrorAction SilentlyContinue
}

# The processes created since the last read, with the command line of each one that still runs.
function Read-Trace {
    $rows = @()
    foreach ($e in @(Get-Event -SourceIdentifier $traceId -ErrorAction SilentlyContinue)) {
        $n = $e.SourceEventArgs.NewEvent
        $id = [int]$n.ProcessID
        $p = Get-CimInstance Win32_Process -Filter "ProcessId=$id" -ErrorAction SilentlyContinue
        $rows += [pscustomobject]@{
            utc     = [DateTime]::FromFileTimeUtc([long]$n.TIME_CREATED).ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
            name    = [string]$n.ProcessName
            id      = $id
            parent  = [int]$n.ParentProcessID
            session = [int]$n.SessionID
            command = $(if ($p -and [int]$p.ParentProcessId -eq [int]$n.ParentProcessID) { [string]$p.CommandLine } else { '' })
        }
        Remove-Event -EventIdentifier $e.EventIdentifier
    }
    return $rows
}

function Invoke-Form([string]$Form) {
    $left = [int]($BoundSeconds - $clock.Elapsed.TotalSeconds) - 8
    if ($left -lt $Seconds + 25) { $script:bad += "form ${Form}: skipped, no time left"; return }
    $hostPath = $(if ($Form -eq 'b') { $hostCopy } else { $null })
    $plan = @(Get-LogonTaskPlan -ConfirmScript $confirmScript -WindowScript $windowCopy -WindowHost $hostPath -Seconds $Seconds)
    $problems = @(Test-LogonTaskPlan $plan)
    $w = @($plan | Where-Object { $_.role -eq 'window' })[0]
    "form ${Form}: task program $($w.execute) $($w.arguments)"
    "form ${Form}: plan rules $(if ($problems.Count) { 'broken: ' + ($problems -join '; ') } else { 'all held' })$(if ($w.console) { " (console: $($w.console))" })"
    if ($problems.Count) { $script:bad += "form ${Form}: the plan breaks its rules"; return }

    Remove-Probe
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ($Seconds + 60)) `
        -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    $action = New-ScheduledTaskAction -Execute $w.execute -Argument $w.arguments -WorkingDirectory $dir
    Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Settings $settings | Out-Null

    Stop-Trace
    Register-CimIndicationEvent -ClassName Win32_ProcessStartTrace -SourceIdentifier $traceId | Out-Null
    Start-Sleep -Milliseconds 800
    $rows = @(Read-Trace)
    $t0 = [DateTime]::UtcNow
    Start-ScheduledTask -TaskName $TaskName
    $end = $t0.AddSeconds($Seconds + 25)
    do {
        Start-Sleep -Milliseconds 250
        $rows += @(Read-Trace)
        $state = (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue).State
    } while (($state -eq 'Running' -or [DateTime]::UtcNow -lt $t0.AddSeconds(2)) -and [DateTime]::UtcNow -lt $end)
    if ($state -eq 'Running') {
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        $script:bad += "form ${Form}: the task did not end inside its bound, stopped"
    }
    $t1 = [DateTime]::UtcNow
    Start-Sleep -Milliseconds 1500
    $rows += @(Read-Trace)
    Stop-Trace
    $info = Get-ScheduledTaskInfo -TaskName $TaskName -ErrorAction SilentlyContinue
    $result = $(if ($info) { [int64]$info.LastTaskResult } else { $null })
    Remove-Probe
    # A window process of our copy that is still there: ours, stopped.
    foreach ($p in @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue | Where-Object { ([string]$_.CommandLine).Contains($dir) })) {
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        $script:bad += "form ${Form}: the window process $($p.ProcessId) was still running at the end, stopped"
    }

    $mine = @($rows | Where-Object { $_.session -eq $session })
    $mine | Export-Csv -LiteralPath (Join-Path $dir "trace-$Form.csv") -NoTypeInformation -Encoding UTF8
    "form ${Form}: task result $result, $([Math]::Round(($t1 - $t0).TotalSeconds, 1)) s, $($mine.Count) process(es) created in session $session (trace-$Form.csv)"
    foreach ($r in $mine) { "  $($r.utc) $($r.name) pid $($r.id) parent $($r.parent): $($r.command)" }
    $terminal = @($mine | Where-Object { $terminalNames -contains $_.name })
    # The task's own process: the application with --start-window (b), or powershell.exe over the copy (a).
    $taskProcess = $(if ($Form -eq 'b') { @($mine | Where-Object { $_.name -eq [IO.Path]::GetFileName($hostCopy) -and $_.command.Contains('--start-window') }) }
        else { @($mine | Where-Object { $_.name -eq 'powershell.exe' -and $_.command.Contains($windowCopy) }) }) | Select-Object -First 1
    $shell = $null
    if ($Form -eq 'b' -and $taskProcess) { $shell = @($mine | Where-Object { $_.name -eq 'powershell.exe' -and $_.parent -eq $taskProcess.id -and $_.command.Contains($windowCopy) }) | Select-Object -First 1 }
    "form ${Form}: console window processes $($terminal.Count)$(if ($terminal.Count) { ': ' + (($terminal | ForEach-Object { "$($_.name) pid $($_.id)" }) -join ', ') })"
    $stillThere = @($terminal | Where-Object { Get-Process -Id $_.id -ErrorAction SilentlyContinue })
    if ($stillThere.Count) { "form ${Form}: still running at the end, not closed by this script: $(($stillThere | ForEach-Object { "$($_.name) pid $($_.id)" }) -join ', ')" }
    if ($Form -eq 'a') {
        $script:verdict['a'] = $(if ($terminal.Count) { 'console window' } else { 'no console window' })
    } else {
        "form b: the task's process $(if ($taskProcess) { "pid $($taskProcess.id)" } else { 'not found' }), its powershell.exe over the copy $(if ($shell) { "pid $($shell.id)" } else { 'not found' })"
        $script:verdict['b'] = $(if ($result -ne 0 -or -not $taskProcess -or -not $shell) { 'failed' } elseif ($terminal.Count) { 'console window' } else { 'no console window' })
    }
    "form ${Form}: $($script:verdict[$Form])"
}

try {
    $formList = @($Forms.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    foreach ($f in $formList) { if (@('a', 'b') -notcontains $f) { throw "refused: unknown form '$f' (a, b)" } }
    $need = @($Application, $Dll, $Common, $Shipped)
    if ($Plan) {
        [string]::Format([Globalization.CultureInfo]::InvariantCulture, 'plan: form(s) {0}, window {1} s, bound {2} s, work {3}\<utc>', ($formList -join ','), $Seconds, $BoundSeconds, $Work)
        foreach ($f in $need) { "plan: file $f $(if ($f -and (Test-Path -LiteralPath $f -PathType Leaf)) { "sha256 $((Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash)" } else { 'not on this computer' })" }
        "plan: refusals checked on the target: explorer.exe at the console, the logon tasks and their scripts not running"
        "plan: layout <work>\control\ (application, DLL) and <work>\tools\start-window.ps1 (copy of -Shipped), read and run rights for the console user"
        foreach ($f in $formList) {
            "plan: form $f, $(if ($f -eq 'a') { 'powershell.exe runs the script (plan without -WindowHost)' } else { 'the application runs it (plan with -WindowHost <work>\control\' + [IO.Path]::GetFileName($Application) + ')' }), rules held first"
            "plan:   task '$TaskName', console user, Interactive, Limited, limit $($Seconds + 60) s; process trace of the console session from 0.8 s before the start"
            "plan:   wait at most $($Seconds + 25) s, then remove the task"
        }
        'plan: nothing was started, registered or written'
        exit 0
    }
    foreach ($f in $need) { if (-not $f -or -not (Test-Path -LiteralPath $f -PathType Leaf)) { throw "refused: no file '$f'" } }
    . $Common

    $explorer = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $explorer) { throw 'refused: no explorer.exe, so nobody is logged on at the console' }
    $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwner
    $sid = (Invoke-CimMethod -InputObject $explorer -MethodName GetOwnerSid).Sid
    $user = '{0}\{1}' -f $owner.Domain, $owner.User
    $session = [int]$explorer.SessionId
    "console user $user session $session"
    foreach ($t in @('amdgpu-wddm start window', 'amdgpu-wddm start confirm')) {
        $task = Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue
        if ($task -and $task.State -eq 'Running') { throw "refused: the task '$t' runs; wait until it ends" }
    }
    $running = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
        Where-Object { [string]$_.CommandLine -match 'start-window\.ps1|start-confirm\.ps1' })
    if ($running.Count) { throw "refused: a start window or confirmation runs (pid $(($running | ForEach-Object { $_.ProcessId }) -join ', ')); this script stops no process it did not start" }
    $startup = "Registry::HKEY_USERS\$sid\Console\%%Startup"
    $delegation = Get-ItemProperty -LiteralPath $startup -ErrorAction SilentlyContinue
    "default terminal of $user (HKU\<sid>\Console\%%Startup): DelegationConsole $(if ($delegation.DelegationConsole) { $delegation.DelegationConsole } else { 'absent' }), DelegationTerminal $(if ($delegation.DelegationTerminal) { $delegation.DelegationTerminal } else { 'absent' })"
    $before = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $terminalNames -contains $_.Name -and $_.SessionId -eq $session })
    "console window processes before the run in session ${session}: $($before.Count)$(if ($before.Count) { ' (' + (($before | ForEach-Object { "$($_.Name) pid $($_.ProcessId)" }) -join ', ') + ')' })"

    $dir = Join-Path $Work ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
    [void][IO.Directory]::CreateDirectory((Join-Path $dir 'control'))
    [void][IO.Directory]::CreateDirectory((Join-Path $dir 'tools'))
    & icacls.exe $dir /grant "${user}:(OI)(CI)RX" | Out-Null
    $hostCopy = Join-Path $dir ('control\' + [IO.Path]::GetFileName($Application))
    $windowCopy = Join-Path $dir 'tools\start-window.ps1'
    $confirmScript = Join-Path (Split-Path -Parent $Shipped) 'start-confirm.ps1'
    Copy-Item -LiteralPath $Application -Destination $hostCopy
    Copy-Item -LiteralPath $Dll -Destination (Join-Path $dir ('control\' + [IO.Path]::GetFileName($Dll)))
    Copy-Item -LiteralPath $Shipped -Destination $windowCopy
    "work $dir"
    foreach ($f in @($hostCopy, (Join-Path $dir ('control\' + [IO.Path]::GetFileName($Dll))), $windowCopy, $Common)) { "file $f sha256 $((Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash)" }
    foreach ($f in $formList) { Invoke-Form $f }
}
catch { $bad += "$($_.Exception.Message)" }
finally {
    Stop-Trace
    Remove-Probe
}
foreach ($b in $bad) { "problem: $b" }
"verdict: form a $(if ($verdict.ContainsKey('a')) { $verdict['a'] } else { 'not run' }), form b $(if ($verdict.ContainsKey('b')) { $verdict['b'] } else { 'not run' }), $([Math]::Round($clock.Elapsed.TotalSeconds, 1)) s"
if (@($bad | Where-Object { $_ -like 'refused*' }).Count) { exit 2 }
if ($verdict['b'] -eq 'failed' -or $verdict['b'] -eq 'console window') { exit 1 }
if ($verdict['a'] -ne 'console window' -or $verdict['b'] -ne 'no console window') { exit 3 }
exit 0

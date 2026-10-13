# BD-062 on hardware: the clock escape's READ answers a caller without administrator rights, and its SET still
# does not (Windows PowerShell 5.1, SSH session, session 0).
#
#   clock-read-user.ps1 [-Wait 60] [-TaskName 'BC250 clock read probe']
#
# Why a scheduled task. Every call of a train arm arrives over ssh and is elevated, so an ordinary arm cannot
# see this defect at all: it would read the clock as an administrator and pass whatever the driver does to an
# ordinary user. The probe therefore runs the command in the console session, as the user who is logged on
# there, with the limited run level. That is the caller the defect refused.
#
# Three readings, in this order:
#   1. clock read, console user, limited run level. mhz, vid and temperature_mc must all be there.
#   2. clock read, this elevated session. The same three keys, read live from the chip.
#   3. clock set 1000 820, console user, limited run level. It must be refused.
# Then the health of the part: no GPU fault in the driver log, and the fan still on our curve.
#
# The probe writes only under C:\BC250\tmp and registers one task, which it removes on every exit path. It
# changes no setting and no clock: the set it tries must fail, and it names the operating point the lab already
# runs at, so even an unexpected success would not move the part.
param([int]$Wait = 60, [string]$TaskName = 'BC250 clock read probe')
$ErrorActionPreference = 'Continue'
$bad = @()
$work = 'C:\BC250\tmp\clock-read-user'
$exe = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'

function Remove-Probe {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    }
}

try {
    "utc $([DateTime]::UtcNow.ToString('o'))"
    $inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
    $cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
    "installroot $inst"
    if (-not (Test-Path -LiteralPath $cli)) { throw "no bc250kmd_cli.exe at $cli" }
    [void][IO.Directory]::CreateDirectory($work)

    # The user of the console session. explorer.exe is the one process that is certainly that user's, and the
    # start-confirm window of the installer runs in the same session.
    $explorer = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $explorer) { throw 'no explorer.exe: no console session is logged on, so there is no ordinary caller to test' }
    $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwner
    $user = '{0}\{1}' -f $owner.Domain, $owner.User
    "console user $user session $((Get-CimInstance Win32_Process -Filter "ProcessId=$($explorer.ProcessId)").SessionId)"
    $admins = @(Get-LocalGroupMember -Group 'Administrators' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    "console user in Administrators: $([bool]($admins -contains $user))"

    Remove-Probe
    $readOut = Join-Path $work 'read-user.txt'
    $setOut = Join-Path $work 'set-user.txt'
    foreach ($f in @($readOut, $setOut)) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } }

    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 120) `
        -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

    function Invoke-AsConsoleUser([string]$Arguments, [string]$OutFile, [string]$Label) {
        Remove-Probe
        # cmd.exe carries the redirection, so the probe needs no script file in the console user's reach.
        $command = '/c "" "{0}" {1} > "{2}" 2>&1' -f $cli, $Arguments, $OutFile
        $action = New-ScheduledTaskAction -Execute (Join-Path $env:WINDIR 'System32\cmd.exe') `
            -Argument $command -WorkingDirectory $work
        Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Settings $settings | Out-Null
        Start-ScheduledTask -TaskName $TaskName
        $deadline = [DateTime]::UtcNow.AddSeconds($Wait)
        do {
            Start-Sleep -Seconds 2
            $info = Get-ScheduledTaskInfo -TaskName $TaskName -ErrorAction SilentlyContinue
            $state = (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue).State
        } while ($state -eq 'Running' -and [DateTime]::UtcNow -lt $deadline)
        "$Label task state $state last result $($info.LastTaskResult)"
        Remove-Probe
        if (Test-Path -LiteralPath $OutFile) { return (Get-Content -LiteralPath $OutFile -Raw) }
        return ''
    }

    # 1. the ordinary caller's read
    $userRead = Invoke-AsConsoleUser 'clock read' $readOut 'read-user'
    "read-user output: $(($userRead -split "`r?`n" | Where-Object { $_ -ne '' }) -join ' | ')"
    foreach ($key in @('mhz', 'vid', 'temperature_mc')) {
        if ($userRead -notmatch [regex]::Escape($key)) { $bad += "the ordinary caller's clock read has no $key" }
    }
    if ($userRead -match 'access denied|ACCESS_DENIED|NOT_ADMIN') {
        $bad += "the ordinary caller's clock read was refused: this is the defect BD-062, not the fix"
    }
    $userTctl = if ($userRead -match 'temperature_mc=(\d+)') { [double]$Matches[1] / 1000 } else { -1 }
    "read-user tctl $userTctl"

    # 2. the same read from this elevated session, live from the chip
    $adminRead = (& $cli clock read 2>&1) -join ' | '
    "read-admin output: $adminRead"
    foreach ($key in @('mhz', 'vid', 'temperature_mc')) {
        if ($adminRead -notmatch [regex]::Escape($key)) { $bad += "the elevated clock read has no $key" }
    }
    $adminTctl = if ($adminRead -match 'temperature_mc=(\d+)') { [double]$Matches[1] / 1000 } else { -1 }
    "read-admin tctl $adminTctl"
    if ($userTctl -ge 0 -and $adminTctl -ge 0) {
        "tctl difference $([Math]::Abs($adminTctl - $userTctl)) C (the published reading is up to one second old)"
    }

    # 3. the ordinary caller's set, which must be refused
    $userSet = Invoke-AsConsoleUser 'clock set 1000 820' $setOut 'set-user'
    "set-user output: $(($userSet -split "`r?`n" | Where-Object { $_ -ne '' }) -join ' | ')"
    if ($userSet -notmatch 'access denied|ACCESS_DENIED|NOT_ADMIN|denied') {
        $bad += 'the ordinary caller was not refused a clock set: SET must stay an administrator operation'
    }

    # the part itself, after the probe
    $log = @(& $cli log 2>&1)
    $faults = @($log | Select-String 'GPU FAULT').Count
    "counters: faults=$faults log_lines=$($log.Count)"
    if ($faults -gt 0) { $bad += "$faults GPU FAULT line(s) in the driver log" }
    $fan = ((& $cli fan 2>&1) -join ' ').Trim()
    "fan: $fan"
    if ($fan -notmatch 'state=curve' -or $fan -notmatch 'controlling=1') { $bad += "the fan is not on our curve: $fan" }
    $clockAfter = (& $cli clock read 2>&1) -join ' '
    "clock after: $clockAfter"
}
catch { $bad += "probe error: $($_.Exception.Message)" }
finally { Remove-Probe }

if ($bad.Count) {
    foreach ($b in $bad) { "problem: $b" }
    "clock read user arm: FAIL, $($bad.Count) problem(s)"
    exit 1
}
'clock read user arm: ok'
exit 0

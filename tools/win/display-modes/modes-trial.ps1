# Runs modes.ps1 in the console session of unit A from an SSH session (session 0), and prints its output.
# Lab plan: docs/design/display-modes.md, "The lab plan for stage A". One call is one bounded step: the task gets
# an execution limit, the wait has a bound, and the task is removed in every case. When the wait runs out, a second
# task sends the desktop back to the registry mode.
#
#   modes-trial.ps1 -Arguments '-List'
#   modes-trial.ps1 -Arguments '-Width 1920 -Height 1080 -HoldSeconds 30'
#   modes-trial.ps1 -Arguments '-Width 1920 -Height 1080 -Fixed stretch -HoldSeconds 20'
#   modes-trial.ps1 -Arguments '-Revert'
param(
    [Parameter(Mandatory)][string]$Arguments,
    [string]$User = "$env:USERDOMAIN\$env:USERNAME",
    [string]$Work = 'C:\BC250\tmp\display-modes',
    [ValidateRange(10, 170)][int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
$task = 'DisplayModesTrial'
New-Item -ItemType Directory -Force $Work | Out-Null
$worker = Join-Path $Work 'modes.ps1'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'modes.ps1') -Destination $worker -Force

function RunTask([string]$taskArguments, [int]$limit) {
    $log = Join-Path $Work ("modes-{0}.log" -f (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ'))
    $command = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$worker`" $taskArguments -Out `"$log`""
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $command -WorkingDirectory $Work
    $principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ($limit + 10)) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings -Force | Out-Null
    $done = $false
    try {
        Start-ScheduledTask -TaskName $task
        $deadline = (Get-Date).AddSeconds($limit)
        Start-Sleep -Seconds 2
        while ((Get-Date) -lt $deadline) {
            if ((Get-ScheduledTask -TaskName $task).State -ne 'Running') { $done = $true; break }
            Start-Sleep -Seconds 2
        }
        if (-not $done) { Stop-ScheduledTask -TaskName $task }
        $info = Get-ScheduledTaskInfo -TaskName $task
        Write-Host "task: done $done, last result $($info.LastTaskResult)"
    } finally {
        Unregister-ScheduledTask -TaskName $task -Confirm:$false
    }
    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log | ForEach-Object { Write-Host $_ } } else { Write-Host "task: no log at $log" }
    return $done
}

if (-not (RunTask $Arguments $TimeoutSeconds)) {
    Write-Host 'task: the wait ran out; sending the desktop back to the registry mode'
    [void](RunTask '-Revert' 30)
}

# Startup task body: apply the lab underclock as soon as bc250rd answers. At boot the scheduled task can run
# before the driver service is up (seen after a bugcheck restart: the task failed once and the GPU stayed at
# the firmware default of 1500 MHz), so retry instead of failing.
#
#   register (elevated, once):  powershell -File apply-clock.ps1 -Register
param(
    [int]$MHz = 1000,
    [int]$MilliVolt = 820,
    [int]$TimeoutSec = 180,
    [switch]$Register
)

$cli = Join-Path $PSScriptRoot 'bc250rd_cli.exe'
$log = Join-Path $PSScriptRoot 'apply-clock.log'

if ($Register) {
    $name = 'BC250 GPU clock 1000MHz 820mV'
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`" -MHz $MHz -MilliVolt $MilliVolt"
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -RunLevel Highest
    Register-ScheduledTask -TaskName $name -Action $action -Trigger (New-ScheduledTaskTrigger -AtStartup) -Principal $principal -Force | Out-Null
    "registered '$name'"
    return
}

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$tries = 0
do {
    $tries++
    $out = & $cli clock $MHz $MilliVolt 2>&1
    if ($LASTEXITCODE -eq 0) {
        Add-Content $log ((Get-Date).ToString('s') + " applied $MHz MHz $MilliVolt mV after $tries tries")
        exit 0
    }
    Start-Sleep -Seconds 2
} while ((Get-Date) -lt $deadline)
Add-Content $log ((Get-Date).ToString('s') + " FAILED after $tries tries: " + (($out | Select-Object -Last 1) -as [string]))
exit 1

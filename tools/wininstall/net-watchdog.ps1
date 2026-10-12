# Keeps the lab machine reachable over Wi-Fi. The USB dongle's driver has been seen to drop the link and then
# fail every automatic reconnect ("the driver disconnected while associating") until somebody connected by
# hand. This loop does what the hand did, and if that is not enough, restarts the adapter.
# It logs times and actions only: no SSID, no addresses.
#
#   register (elevated, once):  powershell -File net-watchdog.ps1 -Register
#   runs as SYSTEM from the scheduled task "BC250 net watchdog" at startup
param(
    [switch]$Register,
    [string]$Log = 'C:\BC250\net-watchdog.log',
    [int]$IntervalSec = 15,
    [int]$FailuresBeforeAction = 3
)

$ErrorActionPreference = 'Continue'

if ($Register) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'WlanProfiles.cs') -PathType Leaf)) {
        throw 'Deploy WlanProfiles.cs next to net-watchdog.ps1 before registering the watchdog.'
    }
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`""
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $principal = New-ScheduledTaskPrincipal -UserId 'S-1-5-18' -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName 'BC250 net watchdog' -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
    Start-ScheduledTask -TaskName 'BC250 net watchdog'
    'registered and started'
    return
}

function Write-Log([string]$text) {
    Add-Content -Path $Log -Value ((Get-Date).ToString('s') + ' ' + $text) -Encoding UTF8
}

function Get-Gateway {
    (Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Sort-Object RouteMetric | Select-Object -First 1).NextHop
}

function Test-Link([string]$gateway) {
    if (-not $gateway) { return $false }
    [bool](Test-Connection -ComputerName $gateway -Count 1 -Quiet -ErrorAction SilentlyContinue)
}

function Get-WlanProfile {
    # Compile only once. This script and WlanProfiles.cs must be deployed together.
    if (-not ('AmdgpuWddm.WinInstall.WlanProfiles' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'WlanProfiles.cs') -ErrorAction Stop
    }
    [AmdgpuWddm.WinInstall.WlanProfiles]::First()
}

Write-Log 'watchdog started'
$lastGateway = Get-Gateway
$failures = 0
$stage = 0
while ($true) {
    Start-Sleep -Seconds $IntervalSec
    $gateway = Get-Gateway
    if ($gateway) { $lastGateway = $gateway }
    if (Test-Link $lastGateway) {
        if ($failures -ge $FailuresBeforeAction) { Write-Log "link back after $failures failed checks (stage $stage)" }
        $failures = 0; $stage = 0
        continue
    }
    $failures++
    if ($failures -lt $FailuresBeforeAction -or ($failures % $FailuresBeforeAction) -ne 0) { continue }
    $stage++
    $wlanProfile = $null
    try { $wlanProfile = Get-WlanProfile } catch { Write-Log 'WLAN profile query failed' }
    if ($stage % 3 -ne 0) {
        Write-Log "link down for $failures checks: asking WLAN to connect"
        if ($wlanProfile) { netsh wlan connect name="$wlanProfile" | Out-Null }
    }
    else {
        Write-Log "link down for $failures checks: restarting the wireless adapter"
        Get-NetAdapter -Physical | Where-Object { $_.NdisPhysicalMedium -eq 9 } | Restart-NetAdapter -Confirm:$false
    }
}

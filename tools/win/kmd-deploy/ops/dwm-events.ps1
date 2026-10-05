# Read-only: event log entries between two UTC instants (DWM exit/respawn analysis, desktop-umd173-002).
param([Parameter(Mandatory)][string]$FromUtc,[Parameter(Mandatory)][string]$ToUtc)
$ErrorActionPreference = 'Stop'
$from = [DateTime]::Parse($FromUtc).ToUniversalTime().ToLocalTime()
$to = [DateTime]::Parse($ToUtc).ToUniversalTime().ToLocalTime()
foreach ($log in @('System', 'Application', 'Microsoft-Windows-Dwm-Core/Operational', 'Microsoft-Windows-Winlogon/Operational')) {
    try {
        $events = @(Get-WinEvent -FilterHashtable @{LogName = $log; StartTime = $from; EndTime = $to} -ErrorAction Stop)
    } catch { "${log}: none ($($_.Exception.Message.Split([char]10)[0]))"; continue }
    "${log}: $($events.Count)"
    foreach ($e in ($events | Sort-Object TimeCreated)) {
        $msg = ([string]$e.Message) -replace '\s+', ' '
        if ($msg.Length -gt 220) { $msg = $msg.Substring(0, 220) }
        '  {0:HH:mm:ss.fff}Z {1} id={2} {3}' -f $e.TimeCreated.ToUniversalTime(), $e.ProviderName, $e.Id, $msg
    }
}

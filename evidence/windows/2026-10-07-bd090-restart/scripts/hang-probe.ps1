# LAB: a restart that does not finish. What is the shutdown waiting for: pending services, session-1 processes,
# thread wait reasons of the shutdown participants. Read-only.
"now $((Get-Date).ToUniversalTime().ToString('o'))"
'--- services not Running/Stopped'
Get-CimInstance Win32_Service | Where-Object { $_.State -notin 'Running', 'Stopped' } | ForEach-Object { '{0} {1} pid {2}' -f $_.Name, $_.State, $_.ProcessId }
'--- session 1 processes'
Get-Process | Where-Object { $_.SessionId -eq 1 } | Sort-Object StartTime | ForEach-Object { '{0} {1} threads {2}' -f $_.Id, $_.ProcessName, $_.Threads.Count }
'--- shutdown participants: thread wait reasons'
foreach ($n in 'wininit', 'winlogon', 'csrss', 'services', 'dwm', 'LogonUI', 'svchost') {
    Get-Process $n -ErrorAction SilentlyContinue | Where-Object { $n -ne 'svchost' -or $_.Threads.Count -lt 0 } | ForEach-Object {
        $w = $_.Threads | Group-Object { "$($_.ThreadState)/$($_.WaitReason)" } | ForEach-Object { "$($_.Name)x$($_.Count)" }
        '{0} {1} s{2}: {3}' -f $_.Id, $_.ProcessName, $_.SessionId, ($w -join ' ')
    }
}
'--- device'
$d = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' }
"$($d.Status) $($d.Problem)"
'--- recent System events'
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddMinutes(-6)} -ErrorAction SilentlyContinue | Select-Object -First 15 | ForEach-Object { '{0:o} {1} {2} {3}' -f $_.TimeCreated.ToUniversalTime(), $_.ProviderName, $_.Id, ($_.Message -split "`n")[0] }

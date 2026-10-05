# sshd stall snapshot: processes created in the last 4 minutes (with parents), sshd tree, thread wait states of
# sshd and sshd-session processes, port 22 states, last OpenSSH events.
$ErrorActionPreference = 'Continue'
$now = Get-Date
$all = Get-CimInstance Win32_Process
"utc $([DateTime]::UtcNow.ToString('o'))"
"== recent processes"
$all | Where-Object { $_.CreationDate -gt $now.AddMinutes(-4) } | Sort-Object CreationDate | ForEach-Object {
  '{0} {1} pid {2} parent {3} {4}' -f $_.CreationDate.ToUniversalTime().ToString('HH:mm:ss'), $_.Name, $_.ProcessId,
    $_.ParentProcessId, ([string]$_.CommandLine).Substring(0, [Math]::Min(120, ([string]$_.CommandLine).Length)) }
"== sshd tree"
$ssh = $all | Where-Object { $_.Name -match '^sshd' }
foreach ($s in $ssh) {
  '{0} {1} parent {2} created {3}' -f $s.Name, $s.ProcessId, $s.ParentProcessId, $s.CreationDate.ToUniversalTime().ToString('HH:mm:ss')
  $all | Where-Object { $_.ParentProcessId -eq $s.ProcessId } | ForEach-Object { "   child $($_.Name) $($_.ProcessId)" }
  $p = Get-Process -Id $s.ProcessId -ErrorAction SilentlyContinue
  if ($p) { $p.Threads | ForEach-Object { "   thread $($_.Id) $($_.ThreadState) $($_.WaitReason)" } }
}
"== tcp22"
Get-NetTCPConnection -LocalPort 22 -ErrorAction SilentlyContinue | Group-Object State | ForEach-Object { "$($_.Name) $($_.Count)" }
"== openssh"
Get-WinEvent -LogName 'OpenSSH/Operational' -MaxEvents 12 -ErrorAction SilentlyContinue | ForEach-Object {
  '{0} {1}' -f $_.TimeCreated.ToUniversalTime().ToString('HH:mm:ss'), (([string]$_.Message) -replace '\s+', ' ').Substring(0, [Math]::Min(150, ([string]$_.Message).Length)) }
"== game"
Get-Process witcher3 -ErrorAction SilentlyContinue | ForEach-Object { "witcher3 $($_.Id) $($_.StartTime.ToUniversalTime().ToString('HH:mm:ss'))" }

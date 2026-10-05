# Evidence of an sshd accept stall (BD-051) before restarting sshd: sshd processes and their age/handles/threads,
# TCP state on port 22, recent OpenSSH events, held PowerShell sessions, CPU load.
$ErrorActionPreference = 'Continue'
$out = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o') }
$out.sshd = @(Get-CimInstance Win32_Process -Filter "Name='sshd.exe' OR Name='sshd-session.exe'" | ForEach-Object {
  $p = Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
  '{0} {1} parent {2} created {3} handles {4} threads {5} cpu {6} cmd {7}' -f $_.Name, $_.ProcessId, $_.ParentProcessId,
    $_.CreationDate.ToUniversalTime().ToString('HH:mm:ss'), $p.HandleCount, $p.Threads.Count, $p.CPU,
    ([string]$_.CommandLine).Substring(0, [Math]::Min(80, ([string]$_.CommandLine).Length)) })
$out.tcp22 = @(Get-NetTCPConnection -LocalPort 22 -ErrorAction SilentlyContinue | ForEach-Object {
  '{0} {1}:{2} owner {3}' -f $_.State, $_.RemoteAddress, $_.RemotePort, $_.OwningProcess })
$out.events = @(Get-WinEvent -LogName 'OpenSSH/Operational' -MaxEvents 25 -ErrorAction SilentlyContinue | ForEach-Object {
  '{0} {1}' -f $_.TimeCreated.ToUniversalTime().ToString('HH:mm:ss'), ([string]$_.Message).Replace("`r", ' ').Replace("`n", ' ') })
$out.powershell = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | ForEach-Object {
  '{0} parent {1} created {2} {3}' -f $_.ProcessId, $_.ParentProcessId, $_.CreationDate.ToUniversalTime().ToString('HH:mm:ss'),
    ([string]$_.CommandLine).Substring(0, [Math]::Min(140, ([string]$_.CommandLine).Length)) })
$out.cli = @(Get-CimInstance Win32_Process -Filter "Name='bc250kmd_cli.exe'" | ForEach-Object {
  '{0} parent {1} created {2} {3}' -f $_.ProcessId, $_.ParentProcessId, $_.CreationDate.ToUniversalTime().ToString('HH:mm:ss'),
    ([string]$_.CommandLine).Substring(0, [Math]::Min(140, ([string]$_.CommandLine).Length)) })
$out.cpu = (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average
$out.game = @(Get-Process witcher3 -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Id) $($_.StartTime)" })
$out | ConvertTo-Json -Depth 4

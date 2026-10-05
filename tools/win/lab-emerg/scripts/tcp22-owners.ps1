# Port 22 connections with owning process, remote port and creation time; the temp guard's command line and children.
Get-NetTCPConnection -LocalPort 22 -ErrorAction SilentlyContinue | ForEach-Object {
  $o = Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue
  '{0} raddr {5} rport {1} owner {2} {3} created {4}' -f $_.State, $_.RemotePort, $_.OwningProcess, $o.Name,
    $_.CreationTime.ToUniversalTime().ToString("HH:mm:ss"), $_.RemoteAddress }
"== guard and children"
$all = Get-CimInstance Win32_Process
$all | Where-Object { $_.CommandLine -match 'temp-g' } | ForEach-Object {
  "$($_.ProcessId) parent $($_.ParentProcessId) $($_.CommandLine)"
  $g = $_.ProcessId
  $all | Where-Object { $_.ParentProcessId -eq $g } | ForEach-Object { "   child $($_.Name) $($_.ProcessId) $($_.CreationDate.ToUniversalTime().ToString('HH:mm:ss'))" } }

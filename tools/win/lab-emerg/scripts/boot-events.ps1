# Last boot time and the System log's warnings and errors of the last N minutes (default 10), oldest first. Read-only.
param([int]$Minutes = 10)
"boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'))"
"now  $([DateTime]::UtcNow.ToString('o'))"
Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddMinutes(-$Minutes) } -ErrorAction SilentlyContinue |
  Where-Object { $_.Level -le 3 } | Sort-Object TimeCreated | ForEach-Object {
    $m = ([string]$_.Message) -replace '\s+', ' '
    '{0} {1} {2} L{3} {4}' -f $_.TimeCreated.ToUniversalTime().ToString('HH:mm:ss.fff'), $_.ProviderName, $_.Id, $_.Level,
      $m.Substring(0, [Math]::Min(240, $m.Length)) }

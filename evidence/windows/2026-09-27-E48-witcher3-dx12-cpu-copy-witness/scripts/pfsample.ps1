# Sample page faults and memory counters of the game and DWM every 2 s for -Seconds (read-only side sampler for run 009).
param([int]$Seconds = 110, [string]$Out = 'C:\BC250\m12\witcher3-dx12\pfsample-009.txt')
$ErrorActionPreference = 'Continue'
$deadline = (Get-Date).AddSeconds($Seconds)
"start " + [DateTime]::UtcNow.ToString('o') | Tee-Object -FilePath $Out
while ((Get-Date) -lt $deadline) {
  try {
    $pf = Get-Counter -Counter '\Process(witcher3)\Page Faults/sec', '\Process(dwm)\Page Faults/sec', '\Memory\Available MBytes', '\Memory\Pages Input/sec', '\Memory\Page Faults/sec', '\Process(witcher3)\Working Set', '\Process(witcher3)\Working Set - Private' -SampleInterval 1 -MaxSamples 1 -ErrorAction Stop
    $parts = foreach ($s in $pf.CounterSamples) { $name = $s.Path.Substring($s.Path.IndexOf('\', 2) + 1); "$name=" + [math]::Round($s.CookedValue, 0) }
    ([DateTime]::UtcNow.ToString('HH:mm:ss') + ' ' + ($parts -join ' | ')) | Tee-Object -FilePath $Out -Append
  } catch { ([DateTime]::UtcNow.ToString('HH:mm:ss') + ' failed: ' + $_.Exception.Message) | Tee-Object -FilePath $Out -Append }
  Start-Sleep -Seconds 1
}
"end " + [DateTime]::UtcNow.ToString('o') | Tee-Object -FilePath $Out -Append

$ErrorActionPreference='Stop'
if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP'}
$names=@('dwm','bc250mon','System')
$before=@{};Get-Process -Name $names -ErrorAction SilentlyContinue | ForEach-Object {$before[$_.Id]=$_.CPU}
$start=Get-Date
Start-Sleep -Seconds 5
$elapsed=((Get-Date)-$start).TotalSeconds
Get-Process -Name $names -ErrorAction SilentlyContinue | ForEach-Object {if($before.ContainsKey($_.Id)){[pscustomobject]@{Name=$_.ProcessName;Id=$_.Id;CpuSeconds=$_.CPU-$before[$_.Id];Interval=$elapsed;Threads=$_.Threads.Count;WorkingMiB=[math]::Round($_.WorkingSet64/1MB,1)}}} | Format-Table
& C:\BC250\m8\bc250kmd_cli.exe log summary

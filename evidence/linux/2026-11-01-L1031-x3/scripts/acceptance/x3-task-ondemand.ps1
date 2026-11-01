# Start the X3 acknowledgement task once in the current interactive session and read its outcome.
$ErrorActionPreference = 'Stop'
$name = 'Lab boot counter reset'
$log = 'C:\BC250\x3\x3-clear.log'
$before = if (Test-Path $log) { @(Get-Content $log).Count } else { 0 }
Start-ScheduledTask -TaskName $name
$deadline = (Get-Date).AddSeconds(60)
do { Start-Sleep -Seconds 2; $s = (Get-ScheduledTask -TaskName $name).State } while ($s -eq 'Running' -and (Get-Date) -lt $deadline)
$i = Get-ScheduledTaskInfo -TaskName $name
'state={0} lastResult={1} lastRun={2:o}' -f $s, $i.LastTaskResult, $i.LastRunTime.ToUniversalTime()
if (Test-Path $log) { Get-Content $log | Select-Object -Skip $before }
Get-ChildItem 'C:\BC250\x3' -Filter 'x3-reader-*.receipt.json' | Sort-Object LastWriteTime | Select-Object -Last 2 | ForEach-Object { $_.Name; Get-Content $_.FullName -Raw }
Get-ChildItem 'C:\BC250\x3' -Filter 'x3-reader-*.err' | Where-Object Length -gt 0 | ForEach-Object { 'ERR ' + $_.Name + ': ' + (Get-Content $_.FullName -Raw) }
$v = @(Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.DriveLetter })
$g = "$($v[0].DriveLetter):\boot\grub"
'grubenv sha256=' + (Get-FileHash "$g\grubenv" -Algorithm SHA256).Hash
Get-ChildItem $g -Filter 'grubenv.x3-*' | ForEach-Object { '{0} {1}' -f $_.Name, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0,16) }

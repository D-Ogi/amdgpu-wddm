# Host test fixture only. Always launch under bounded-child.exe.
param([string]$Mode,[string]$PidFile,[string]$Value)
if($Mode -eq 'ok'){$Value;exit 0}
if($Mode -eq 'fail'){exit 41}
if($Mode -eq 'hang'){Start-Sleep -Seconds 30;exit 0}
$p=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile -Command Start-Sleep -Seconds 30'
@{pid=$p.Id;start=$p.StartTime.ToUniversalTime().ToString('o')}|ConvertTo-Json|Set-Content $PidFile
Start-Sleep -Milliseconds 100
if($Mode -eq 'orphan'){exit 0}
Start-Sleep -Seconds 30

param([string]$Mode,[string]$Prefix)
$ErrorActionPreference='Stop'
$p=Get-Process -Id $PID
@{pid=$PID;session=$p.SessionId;start=$p.StartTime.ToUniversalTime().ToString('o')}|ConvertTo-Json|Set-Content "$Prefix-root.json"
if($Mode -eq 'ok'){exit 0}
if($Mode -eq 'fail'){exit 41}
$p=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile -Command Start-Sleep -Seconds 30'
@{pid=$p.Id;start=$p.StartTime.ToUniversalTime().ToString('o')}|ConvertTo-Json|Set-Content "$Prefix-child.json"
if($Mode -eq 'orphan'){exit 0}
Start-Sleep -Seconds 30

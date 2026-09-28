param([string]$Prefix)
$ErrorActionPreference='Stop'
[IO.File]::WriteAllText($Prefix+'.entered','yes')
$p=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile -Command Start-Sleep -Seconds 30'
@{pid=$p.Id;start=$p.StartTime.ToUniversalTime().ToString('o')}|ConvertTo-Json|Set-Content ($Prefix+'.child.json')
[IO.File]::WriteAllText($Prefix+'.cancel','cancel')
Start-Sleep -Seconds 30

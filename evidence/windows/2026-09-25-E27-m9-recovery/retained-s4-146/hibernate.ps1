$ErrorActionPreference='Stop'
$out='C:\BC250\m9\resume146-s4'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags).stop) {throw 'Owner STOP requested'}
$ready=Get-Content "$out\ready.txt" -Raw
if($ready -notmatch '(?m)^pid=(\d+)'){throw 'Ready PID absent'}
$probeId=[int]$Matches[1]
$p=Get-Process -Id $probeId
if($p.ProcessName -ne 'gpu-residency-probe' -or (Test-Path "$out\probe.exit") -or (Test-Path "$out\release.txt")){throw 'Probe not waiting'}
$health=& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe health read | Out-String
$health
if($LASTEXITCODE -ne 0 -or $health -notmatch 'flags=15'){throw 'Health not confirmed'}
$worker=@(
 '[IO.File]::WriteAllText("C:\BC250\m9\resume146-s4\hibernate-request.txt",[DateTime]::UtcNow.ToString("o"))',
 'shutdown.exe /h',
 '$code=$LASTEXITCODE',
 '[IO.File]::WriteAllText("C:\BC250\m9\resume146-s4\hibernate-return.txt",([DateTime]::UtcNow.ToString("o")+" exit="+$code))'
)
$worker | Set-Content "$out\hibernate-worker.ps1" -Encoding ASCII
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -ExecutionPolicy Bypass -File $out\hibernate-worker.ps1"
$principal=New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask -TaskName BC250-M9-Resume146Hibernate -Action $action -Principal $principal -Force | Out-Null
'hibernate_task_requested_utc='+[DateTime]::UtcNow.ToString('o')
Start-ScheduledTask BC250-M9-Resume146Hibernate
'hibernate_task_started'

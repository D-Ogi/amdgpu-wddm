$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted043'
Get-ScheduledTask -TaskName BC250-G0-DwmRun043,BC250-G0-DwmWatch043,BC250-G0-Composition043 -ErrorAction SilentlyContinue | ForEach-Object {
 $info=Get-ScheduledTaskInfo -TaskName $_.TaskName
 @{name=$_.TaskName;state=[string]$_.State;result=$info.LastTaskResult}
} | ConvertTo-Json
Get-Process dwm | Select-Object Id,CPU | ConvertTo-Json
$last=Get-ChildItem -LiteralPath $d -Filter 'process-*.json' | Sort-Object LastWriteTime | Select-Object -Last 1
if($last){Get-Content -LiteralPath $last.FullName}
foreach($name in 'done.json','restored.json','watchdog.log') {if(Test-Path "$d\$name"){Get-Content "$d\$name"}}
Get-Content "$d\run.log" -Tail 8

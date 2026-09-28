$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted038'
Get-ScheduledTask -TaskName BC250-G0-DwmRun038,BC250-G0-DwmWatch038,BC250-G0-Composition038,BC250-G0-Handshake038 -ErrorAction SilentlyContinue | ForEach-Object {
 $info=Get-ScheduledTaskInfo -TaskName $_.TaskName
 @{name=$_.TaskName;state=[string]$_.State;result=$info.LastTaskResult}
} | ConvertTo-Json
Get-Process dwm | Select-Object Id,CPU | ConvertTo-Json
$last=Get-ChildItem -LiteralPath $d -Filter 'process-*.json' | Sort-Object LastWriteTime | Select-Object -Last 1
if($last){Get-Content -LiteralPath $last.FullName}
foreach($name in 'done.json','restored.json','handshake-done.json','watchdog.log') {if(Test-Path "$d\$name"){Get-Content "$d\$name"}}
Get-Content "$d\run.log" -Tail 8

$ErrorActionPreference='Stop'
$out='C:\BC250\m13\mesa-main-control'
if(Test-Path "$out\render-probe.txt"){throw 'Output exists'}
$start=Get-Date
'control_begin='+$start.ToString('s')
Get-Process dwm | Select-Object Id,StartTime,CPU | Format-Table
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No lab interactive session'}
$action=New-ScheduledTaskAction -Execute "$out\render_probe_mesa_main.exe"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName 'BC250-M13-Control' -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask -TaskName 'BC250-M13-Control'
$deadline=(Get-Date).AddSeconds(90)
$i=0
try {
 do {
  Start-Sleep -Seconds 3
  $state=(Get-ScheduledTask 'BC250-M13-Control').State
  if($i -lt 3){& C:\BC250\m8\bc250kmd_cli.exe fbdump "$out\scanout-$i.bmp"}
  'task_state='+$state
  $i++
 } while($state -eq 'Running' -and (Get-Date) -lt $deadline)
 if($state -eq 'Running'){throw 'Control deadline'}
 $result=(Get-ScheduledTaskInfo 'BC250-M13-Control').LastTaskResult
 'control_exit='+$result
 Get-Content "$out\render-probe.txt"
 if($result -ne 0){throw 'Control failed'}
} finally {
 if((Get-ScheduledTask 'BC250-M13-Control').State -eq 'Running'){Stop-ScheduledTask 'BC250-M13-Control'}
 Unregister-ScheduledTask 'BC250-M13-Control' -Confirm:$false
}
Get-Process dwm | Select-Object Id,StartTime,CPU | Format-Table
& C:\BC250\m8\bc250kmd_cli.exe log summary
& C:\BC250\m8\bc250kmd_cli.exe log
'control_complete'

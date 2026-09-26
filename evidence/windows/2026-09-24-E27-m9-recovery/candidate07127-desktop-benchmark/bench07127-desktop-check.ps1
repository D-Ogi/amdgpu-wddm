$ErrorActionPreference='Stop'
Get-Process -Name llama-bench,dwm -ErrorAction SilentlyContinue | Select-Object Name,Id,StartTime,CPU,Responding | Format-Table
Get-ScheduledTask -TaskName BC250-M9-Bench07127Desktop | Select-Object TaskName,State
Get-ScheduledTaskInfo -TaskName BC250-M9-Bench07127Desktop | Select-Object LastRunTime,LastTaskResult
Get-ChildItem 'C:\BC250\m9\bench07127-desktop' | Select-Object Name,Length,LastWriteTime | Format-Table
Get-Content 'C:\BC250\m9\bench07127-desktop\run.cmd'
foreach($name in @('stories15M','tinyllama')) {
 foreach($ext in @('out','err','exit')) {
  $file="C:\BC250\m9\bench07127-desktop\$name.$ext"
  if(Test-Path $file){'FILE='+$name+'.'+$ext; Get-Content $file -Tail 14}
 }
}
& C:\BC250\m8\bc250kmd_cli.exe info
& C:\BC250\m8\bc250kmd_cli.exe log summary
'check_time='+(Get-Date).ToString('s')

# LAB: did the logon start-confirm run, and what did it decide.
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
"UnconfirmedStarts $((Get-ItemProperty $par).UnconfirmedStarts) GuardBoot $((Get-ItemProperty "$par\GuardBoot" -ErrorAction SilentlyContinue).Confirmed)"
Get-Content "$env:ProgramData\amdgpu-wddm\start-confirm.log" -Tail 12 -ErrorAction SilentlyContinue
$t = Get-ScheduledTask -TaskName 'amdgpu-wddm start confirm' -ErrorAction SilentlyContinue
if ($t) { $i = $t | Get-ScheduledTaskInfo; "task last run $($i.LastRunTime) result 0x{0:X}" -f $i.LastTaskResult } else { 'no task' }
Get-Process explorer -ErrorAction SilentlyContinue | ForEach-Object { "explorer pid $($_.Id) session $($_.SessionId)" }

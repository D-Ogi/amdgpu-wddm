$ErrorActionPreference='Stop'
$out='C:\BC250\m9\bench07127-desktop'
foreach($name in @('stories15M','tinyllama')) {
 $exit=[int](Get-Content "$out\$name.exit")
 "$name exit=$exit"
 if($exit -ne 0){throw 'Native benchmark failure'}
 $trace=Get-Content "$out\$name.err" -Raw
 if($trace -notmatch 'cache-intent-v2\\vulkan_radeon.dll' -or $trace -notmatch 'bc250: progress before submit'){throw 'Missing ICD witness'}
}
if(Get-Process llama-bench -ErrorAction SilentlyContinue){throw 'Workload still running'}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\after.log"
if($LASTEXITCODE -ne 0){throw 'Driver summary failed'}
& C:\BC250\m8\bc250kmd_cli.exe confirm
Get-Process dwm | Select-Object Id,StartTime,CPU,Responding | Format-Table
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' | Select-Object EnableFullWddm,UnconfirmedStarts
Get-ScheduledTaskInfo BC250-M9-Bench07127Desktop | Format-List LastRunTime,LastTaskResult
Unregister-ScheduledTask BC250-M9-Bench07127Desktop -Confirm:$false
'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'final_time='+(Get-Date).ToString('s')
'benchmark_collected_without_rerun'

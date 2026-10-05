$ErrorActionPreference='Stop'
$t=Get-ScheduledTask BC250-M9-RadvMainControl2 -ErrorAction SilentlyContinue
if($t){if($t.State -eq 'Running'){throw 'Failed task unexpectedly running'};Unregister-ScheduledTask BC250-M9-RadvMainControl2 -Confirm:$false}
if(Get-Process vkcompute,llama-completion,llama-bench -ErrorAction SilentlyContinue){throw 'Workload still active'}
& C:\BC250\m8\bc250kmd_cli.exe confirm
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File C:\BC250\m9\radv-main\final-kmd.log
'icd_sha256='+(Get-FileHash C:\BC250\m9\radv-main-icd2\vulkan_radeon.dll).Hash
Get-Process dwm | Select-Object Id,StartTime,Responding | Format-Table
& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1
& cmd.exe /c C:\BC250\m9\radv-main\run.cmd C:\BC250\m9\llama\llama-bench.exe --version
'launcher_exit='+$LASTEXITCODE
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'final_time='+(Get-Date).ToString('s')

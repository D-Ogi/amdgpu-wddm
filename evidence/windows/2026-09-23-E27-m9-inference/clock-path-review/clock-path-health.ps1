$ErrorActionPreference = 'Stop'
$os = Get-CimInstance Win32_OperatingSystem
'boot=' + $os.LastBootUpTime.ToString('s')
foreach ($name in @('bc250rd','bc250kmd')) {
    $svc = Get-CimInstance Win32_SystemDriver -Filter ("Name='" + $name + "'")
    if ($svc) { 'driver=' + $name + ' state=' + $svc.State + ' start=' + $svc.StartMode }
}
$task = Get-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV' -ErrorAction SilentlyContinue
if ($task) {
    $info = $task | Get-ScheduledTaskInfo
    'clock_task_result=' + $info.LastTaskResult
    'clock_task_last_run=' + $info.LastRunTime.ToString('s')
}
$clockLog = 'C:\BC250\bc250rd\apply-clock.log'
if (Test-Path -LiteralPath $clockLog) { Get-Content -LiteralPath $clockLog -Tail 3 }
$path = 'C:\Windows\System32\drivers\bc250kmd.sys'
if (Test-Path -LiteralPath $path) { 'sys_sha256=' + (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }

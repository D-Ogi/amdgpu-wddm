# C85 lab arm, read only: this boot, the KMD image, the start-confirm log of this boot, the health reading, StateV2,
# and whether any window of the start check exists.
"utc $([DateTime]::UtcNow.ToString('o')) boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'))"
$ip = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd' -Name ImagePath).ImagePath
$path = $ip -replace '^\\SystemRoot\\', "$env:SystemRoot\" -replace '^\\\?\?\\', '' -replace '^System32\\', "$env:SystemRoot\System32\"
"kmd image $((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash)"
$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1
"device $($dev.Status) $($dev.Problem)"
$cli = Get-ChildItem -Path 'C:\Program Files\amdgpu-wddm' -Recurse -Filter bc250kmd_cli.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
"--- health read"
& $cli health read 2>&1 | Select-Object -First 12
"--- version"
& $cli version 2>&1 | Select-Object -First 6
"--- start-confirm.log (tail)"
Get-Content (Join-Path $env:ProgramData 'amdgpu-wddm\start-confirm.log') -Tail 25 -ErrorAction SilentlyContinue
"--- StateV2"
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\StateV2' -ErrorAction SilentlyContinue | ForEach-Object {
    $p = Get-ItemProperty $_.PSPath
    "$($_.PSChildName) LastStage=$($p.LastStage) Unconfirmed=$($p.UnconfirmedStarts) Confirmed=$($p.ConfirmedStarts)"
}
"--- processes of the start check"
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'start-window|start-confirm|start-notice' } | ForEach-Object { "$($_.ProcessId) $($_.Name) $($_.CommandLine)" }
"--- tasks"
foreach ($t in 'amdgpu-wddm start confirm', 'amdgpu-wddm start window') {
    $i = Get-ScheduledTaskInfo -TaskName $t -ErrorAction SilentlyContinue
    "$t last=$($i.LastRunTime) result=$($i.LastTaskResult)"
}

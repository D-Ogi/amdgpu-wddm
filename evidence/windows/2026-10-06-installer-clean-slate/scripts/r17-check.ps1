# r17 fresh-install check (owner's installer test, 2026-10-06): after the clean uninstall and a fresh install of
# tester.17 every component must be in place and working: KMD, desktop UMD on the GPU route, Vulkan ICD, D3D11/D3D12
# UMDs, MFT, control application (GUI), Tuner, start-confirm task, Start menu. Reads only.
$ErrorActionPreference = 'Continue'
$pf = 'C:\Program Files\amdgpu-wddm'
$cli = "$pf\tools\bc250kmd_cli.exe"
'boot {0:o}' -f (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1
'device {0} version {1} problem {2}' -f $dev.Status, (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data, (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
& $cli info 2>&1 | Select-Object -First 6
& $cli dpm 1 2>&1 | Select-Object -Last 1
& $cli fan 2>&1 | Select-Object -Last 1
Get-CimInstance Win32_Process -Filter "Name='dwm.exe'" | ForEach-Object { 'dwm pid {0} session {1} created {2:o}' -f $_.ProcessId, $_.SessionId, $_.CreationDate.ToUniversalTime() }
$d = Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1
if ($d) { 'dwm modules: ' + (($d.Modules | Where-Object { $_.ModuleName -match 'bc250|amdgpu|zink|radv|vulkan' } | ForEach-Object { $_.ModuleName }) -join ', ') }
Get-WinEvent -FilterHashtable @{LogName = 'System'; Id = 1001; StartTime = (Get-Date).AddHours(-2) } -ErrorAction SilentlyContinue | Select-Object -First 3 | ForEach-Object { 'bugcheck event ' + $_.TimeCreated.ToString('s') }
Get-ChildItem C:\Windows\Minidump -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt (Get-Date).AddHours(-2) } | ForEach-Object { 'minidump ' + $_.Name }
'--- components'
foreach ($f in 'control\amdgpu_wddm_control.exe', 'tools\bc250kmd_cli.exe', 'd3d11\amdgpu_wddm_d3d11.dll', 'd3d12\amdgpu_wddm_d3d12.dll', 'desktop\bc250d3d.dll', 'desktop\bc250d3d_router.dll', 'vulkan\radeon_icd.json', 'mft\amdgpu_wddm_mft_h264.dll') {
    $p = Join-Path $pf $f; '{0,-40} {1}' -f $f, $(if (Test-Path $p) { (Get-FileHash $p -Algorithm SHA256).Hash.Substring(0, 8) } else { 'MISSING' })
}
Get-ChildItem $pf -Directory | ForEach-Object { 'dir {0}: {1} files' -f $_.Name, @(Get-ChildItem $_.FullName -Recurse -File).Count }
'start menu: ' + ((Get-ChildItem "$env:ProgramData\Microsoft\Windows\Start Menu\Programs" -Filter 'amdgpu-wddm*' | ForEach-Object Name) -join ', ')
$t = Get-ScheduledTask -TaskName 'amdgpu-wddm start confirm' -ErrorAction SilentlyContinue
if ($t) { $i = $t | Get-ScheduledTaskInfo; 'start-confirm task: last run {0:o} result 0x{1:X}' -f $i.LastRunTime.ToUniversalTime(), $i.LastTaskResult }
'vulkan drivers: ' + ((Get-Item 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers').Property -join '; ')
$r = Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\AppRouter' -ErrorAction SilentlyContinue
'AppRouter Mode {0} Allow [{1}] Deny [{2}]' -f $r.Mode, ($r.Allow -join ','), ($r.Deny -join ',')
'D3D12 application profiles: ' + ((Get-ChildItem 'HKLM:\SOFTWARE\amdgpu-wddm\D3D12\Applications' -ErrorAction SilentlyContinue | ForEach-Object PSChildName) -join ', ')
'--- control application status (GUI binary, console mode)'
New-Item -ItemType Directory -Force C:\BC250\tmp\rel0 | Out-Null
& "$pf\control\amdgpu_wddm_control.exe" --status --out C:\BC250\tmp\rel0\status-r17.txt | Out-Null; Start-Sleep 2
Get-Content C:\BC250\tmp\rel0\status-r17.txt -ErrorAction SilentlyContinue | Select-Object -First 16
'--- kernel log lines'
& $cli log summary 2>&1 | Select-String -Pattern 'notify dpc|DirectFlip handshake|hwmon: base|thermal zone|0.7.213|CU |cu mode|fail|refus' | Select-Object -First 16 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
'--- verify report'
Get-ChildItem 'C:\ProgramData\amdgpu-wddm\installer' -Filter 'verify*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1 | ForEach-Object { $_.Name; Get-Content $_.FullName -TotalCount 30 }

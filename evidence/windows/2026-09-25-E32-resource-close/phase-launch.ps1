param([string]$RunId='control-01',[int]$DurationSeconds=86400,[int]$MaxCycles=0)
$ErrorActionPreference='Stop'
if($RunId -notmatch '^[a-z0-9-]+$'){throw 'Invalid run identifier'}
$out="C:\BC250\m11\$RunId"
if(Test-Path $out){throw 'Existing run; inspect it instead of restarting'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
$expected='9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'
if((Get-FileHash C:\BC250\m10\wsi-final\vulkan_radeon.dll).Hash -ne $expected){throw 'ICD mismatch'}
$service=(Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd).ImagePath
if($service.StartsWith('\SystemRoot\')){$service=Join-Path $env:windir $service.Substring(12)}
if($service.StartsWith('\??\')){$service=$service.Substring(4)}
if((Get-FileHash $service).Hash -ne '5FCB554AE77B04506AA80B4590EE33D7CAA4F8D5A6E720CA89666F736760EC31'){throw 'KMD mismatch'}
if((Get-FileHash C:\BC250\m11\resource-close\bc250d3d.dll).Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Desktop UMD mismatch'}
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No interactive user'}
New-Item -ItemType Directory $out | Out-Null
$files=@($service,'C:\BC250\m11\resource-close\bc250d3d.dll','C:\BC250\m10\wsi-final\vulkan_radeon.dll','C:\BC250\m10\wsi-final\vkcube.exe','C:\BC250\m8\vkcompute.exe','C:\BC250\m11\phase-worker.ps1','C:\BC250\m11\monitor.ps1','C:\BC250\m11\poolmon.exe','C:\BC250\m11\compute-reference.json','C:\BC250\m11\stories15M-ngl99.out','C:\BC250\m11\tinyllama-ngl99.out')
$files+=@(Get-ChildItem C:\BC250\m8\spv -File -Filter '*.spv' | ForEach-Object FullName)
$files+=@(Get-ChildItem C:\BC250\m9\llama -File | Where-Object {$_.Extension -in @('.dll','.exe')} | ForEach-Object FullName)
$files+=@('C:\BC250\m9\models\stories15M-q4_0.gguf','C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf')
$files | ForEach-Object {Get-FileHash -LiteralPath $_} | Select-Object Path,Hash | ConvertTo-Json | Set-Content "$out\inputs.json"
$workerName="BC250-M11-$RunId-Worker";$monitorName="BC250-M11-$RunId-Monitor"
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 26) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
$a=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\BC250\m11\phase-worker.ps1 -Out $out -DurationSeconds $DurationSeconds -MaxCycles $MaxCycles"
$p=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Limited
Register-ScheduledTask $workerName -Action $a -Principal $p -Settings $settings | Out-Null
$a=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\BC250\m11\monitor.ps1 -Out $out -WorkerTask $workerName"
$p=New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask $monitorName -Action $a -Principal $p -Settings $settings | Out-Null
Start-ScheduledTask $monitorName
$deadline=(Get-Date).AddSeconds(20)
do {
 if(Test-Path "$out\monitor-result.json"){Get-Content "$out\monitor-result.json";throw 'Monitor failed before worker'}
 Start-Sleep -Milliseconds 250
}while(-not (Test-Path "$out\monitor-ready.json") -and (Get-Date) -lt $deadline)
if(-not (Test-Path "$out\monitor-ready.json")){throw 'Monitor startup not observed'}
Start-ScheduledTask $workerName
@{run=$RunId;seconds=$DurationSeconds;max_cycles=$MaxCycles;worker_task=$workerName;monitor_task=$monitorName;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$out\launch.json"
Get-Content "$out\launch.json"

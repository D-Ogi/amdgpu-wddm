$ErrorActionPreference='Stop'
$out='C:\BC250\m9\candidate07136'
$backup=Join-Path $out 'before-handover'
if(Test-Path $backup){throw 'Existing backup must remain immutable'}
if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP requested'}
foreach($item in (Get-Content (Join-Path $out 'smu136-stage-manifest.json') -Raw | ConvertFrom-Json)){
 $file=Join-Path $out $item.relative
 if((Get-FileHash -LiteralPath $file).Hash -ne $item.sha256 -or (Get-Item -LiteralPath $file).Length -ne $item.bytes){throw ('Staged identity mismatch: '+$item.relative)}
}
'staged_hashes_verified=13'
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070087' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Expected active full135'}
$info
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Legacy clock control mismatch'}
$temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
$temp
if($LASTEXITCODE -ne 0 -or $temp -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)'){throw 'SMN temperature unavailable'}
if([double]$Matches[1] -ge 85){throw 'Temperature limit'}
New-Item -ItemType Directory $backup | Out-Null
foreach($name in @('bc250rd.sys','bc250rd_cli.exe','apply-clock.ps1')){Copy-Item -LiteralPath ('C:\BC250\bc250rd\'+$name) -Destination (Join-Path $backup $name)}
Copy-Item -LiteralPath C:\BC250\mon\bc250mon.exe -Destination (Join-Path $backup 'bc250mon.exe')
Copy-Item -LiteralPath C:\BC250\mon\graphics-modules.json -Destination (Join-Path $backup 'graphics-modules.json')
Export-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV' | Set-Content (Join-Path $backup 'clock-task.xml')
Export-ScheduledTask -TaskName 'BC250 monitor overlay' | Set-Content (Join-Path $backup 'monitor-task.xml')
Get-CimInstance Win32_SystemDriver -Filter "Name='bc250rd'" | Select-Object Name,State,StartMode,PathName | ConvertTo-Json | Set-Content (Join-Path $backup 'reader-service.json')
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' | Format-List | Out-File (Join-Path $backup 'kmd-settings.log')
& C:\BC250\m8\bc250kmd_cli.exe log | Out-File (Join-Path $backup 'kmd135.log')
$info | Set-Content (Join-Path $backup 'info135.log')
Get-ChildItem $backup -File | Get-FileHash | Select-Object Hash,Path | ConvertTo-Json | Set-Content (Join-Path $backup 'SHA256.json')
Get-Content C:\BC250\bc250rd\apply-clock.ps1
Get-Process dwm,bc250mon | Select-Object Id,ProcessName,StartTime | Format-Table
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'backup_complete='+(Get-Date).ToString('s')
'no_service_device_or_task_state_changes'

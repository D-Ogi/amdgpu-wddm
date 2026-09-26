$ErrorActionPreference='Stop'
$mon='C:\BC250\mon'
$stage="$mon\health145-deploy"
$expected='4F5597D3C34AF92A205CB3CC4AF031B1EB2983C6F477E89334EE04B5D2F7A875'
$control='02412B52C68E4B02030BBD26E33CD948FADDE74AADD14406813B2EB46B2B0962'
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP requested'}
if((Get-FileHash "$stage\bc250mon.exe").Hash -ne $expected){throw 'Staged monitor hash mismatch'}
if((Get-FileHash "$stage\bc250control.dll").Hash -ne $control){throw 'Control library changed'}
$prior=@(Get-Process bc250mon | Where-Object {$_.Path -eq "$mon\bc250mon.exe"})
if($prior.Count -ne 1){throw 'Expected one existing monitor'}
$beforeDwm=Get-Process dwm
$record=[ordered]@{StartedUtc=[DateTime]::UtcNow.ToString('o');OldPid=$prior[0].Id;OldSha=(Get-FileHash "$mon\bc250mon.exe").Hash;DwmPid=$beforeDwm.Id;DwmStart=$beforeDwm.StartTime.ToString('o');Boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')}
if(Test-Path "$stage\before"){throw 'Backup already exists'}
New-Item -ItemType Directory "$stage\before" | Out-Null
Copy-Item "$mon\bc250mon.exe" "$stage\before\bc250mon.exe"
Copy-Item "$mon\bc250control.dll" "$stage\before\bc250control.dll"
if(Test-Path "$mon\vulkan-inventory.json"){Copy-Item "$mon\vulkan-inventory.json" "$stage\before\vulkan-inventory.json"}
Export-ScheduledTask -TaskName 'BC250 monitor overlay' | Set-Content "$stage\before\task.xml"
$record | ConvertTo-Json | Set-Content "$stage\before\identity.json"
Disable-ScheduledTask -TaskName 'BC250 monitor overlay' | Out-Null
try {
 Stop-ScheduledTask -TaskName 'BC250 monitor overlay'
 Start-Sleep -Seconds 1
 $prior | Where-Object {-not $_.HasExited} | Stop-Process -Force
 Copy-Item "$stage\bc250mon.exe" "$mon\bc250mon.exe" -Force
 Copy-Item "$stage\bc250control.dll" "$mon\bc250control.dll" -Force
} finally {Enable-ScheduledTask -TaskName 'BC250 monitor overlay' | Out-Null}
Start-ScheduledTask -TaskName 'BC250 monitor overlay'
$state=$null
for($i=0;$i -lt 15;$i++){
 Start-Sleep -Seconds 1
 try {$state=Invoke-RestMethod http://127.0.0.1:2250/state; if(@($state.panels | Where-Object name -eq 'kmd').Count -eq 1){break}}catch{}
}
if(-not $state -or @($state.panels | Where-Object name -eq 'kmd').Count -ne 1){throw 'New overlay did not publish KMD panel'}
$running=@(Get-Process bc250mon | Where-Object {$_.Path -eq "$mon\bc250mon.exe"})
if($running.Count -ne 1){throw 'Expected one replacement monitor'}
$record.NewPid=$running[0].Id;$record.NewSha=(Get-FileHash "$mon\bc250mon.exe").Hash
if($record.NewSha -ne $expected){throw 'Installed monitor mismatch'}
$native=@($running[0].Modules | Where-Object ModuleName -eq bc250control.dll)
if($native.Count -ne 1 -or (Get-FileHash $native[0].FileName).Hash -ne $control){throw 'Loaded control DLL mismatch'}
$record.ControlSha=$control
$afterDwm=Get-Process dwm
if($afterDwm.Id -ne $beforeDwm.Id -or $afterDwm.StartTime -ne $beforeDwm.StartTime){throw 'DWM restarted unexpectedly'}
$record.Stop=$state.stop;$record.CompletedUtc=[DateTime]::UtcNow.ToString('o')
$record | ConvertTo-Json | Set-Content "$stage\deployment.json" -Encoding UTF8
$state | ConvertTo-Json -Depth 12 | Set-Content "$stage\state-after.json" -Encoding UTF8
$record | ConvertTo-Json
$state.panels | Where-Object name -eq kmd | ConvertTo-Json -Depth 8
'overlay_deployment_complete'


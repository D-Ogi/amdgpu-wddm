$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted048'
if(!(Test-Path "$d\watchdog-done.json")){throw 'Watchdog closure missing'}
if((Get-ScheduledTask -TaskName BC250-G0-DwmWatch048 -ErrorAction SilentlyContinue).State -eq 'Running'){throw 'Watchdog still running'}
if(Test-Path "$d\colour\process.json") {
 $identity=Get-Content "$d\colour\process.json" -Raw | ConvertFrom-Json
 $process=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
 if($process -and $process.StartTime.ToUniversalTime().ToString('o') -eq $identity.start){throw 'Original colour process still active'}
}
# The independent collector must be terminal before receipts or task cleanup.
if(Test-Path "$d\collector-start.json"){
 $identity=Get-Content "$d\collector-start.json" -Raw | ConvertFrom-Json
 $process=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
 if($process -and $process.StartTime.ToUniversalTime().ToString('o') -eq $identity.start){throw 'Original startup collector still active'}
 if(!(Test-Path "$d\collector-done.json")){throw 'Startup collector has no terminal receipt'}
}
if(!(Test-Path "$d\restored.json")){throw 'No restoration witness'}
if((Get-ScheduledTask -TaskName BC250-G0-DwmRun048).State -eq 'Running'){throw 'Runner still active'}
if((Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'UMD baseline mismatch'}
if((Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash -ne 'CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157'){throw 'ICD baseline mismatch'}
if(Test-Path "$d\interop-pending"){throw 'Interop rollback still pending'}
$settings=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
if($settings.EnableCddDwmInterop -ne 0 -or $settings.EnableGpuPresentBlit -ne 0){throw 'Registry gates not restored'}
& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe log summary *> "$d\closure-driver.log"
if($LASTEXITCODE -ne 0 -or (Get-Content "$d\closure-driver.log" -Raw) -notmatch 'CDD interop0 GPU Present gate0 identity probe1'){throw 'Latched gates not restored'}
foreach($name in 'BC250-G0-DwmRun048','BC250-G0-DwmWatch048','BC250-G0-Composition048','BC250-G0-GpuWindow048'){
 if(!(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue)){continue}
 Stop-ScheduledTask -TaskName $name
 for($i=0;$i -lt 20 -and (Get-ScheduledTask -TaskName $name).State -eq 'Running';$i++){Start-Sleep -Milliseconds 100}
 if((Get-ScheduledTask -TaskName $name).State -eq 'Running'){throw 'Task still live'}
 Unregister-ScheduledTask -TaskName $name -Confirm:$false
}
'Verified baselines; all DWM048 tasks ended and removed'

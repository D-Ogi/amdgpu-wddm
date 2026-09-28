$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted024'
if(!(Test-Path "$d\restored.json")){throw 'No restoration witness'}
if((Get-ScheduledTask -TaskName BC250-G0-DwmRun024).State -eq 'Running'){throw 'Runner still active'}
if((Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'UMD baseline mismatch'}
if((Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'){throw 'ICD baseline mismatch'}
if(Test-Path "$d\interop-pending"){throw 'Interop rollback still pending'}
$settings=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
if($settings.EnableCddDwmInterop -ne 0 -or $settings.EnableGpuPresentBlit -ne 0){throw 'Registry gates not restored'}
& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe log summary *> "$d\closure-driver.log"
if($LASTEXITCODE -ne 0 -or (Get-Content "$d\closure-driver.log" -Raw) -notmatch 'CDD interop0 GPU Present gate0 identity probe1'){throw 'Latched gates not restored'}
foreach($name in 'BC250-G0-DwmRun024','BC250-G0-DwmWatch024','BC250-G0-Composition024'){
 Stop-ScheduledTask -TaskName $name
 for($i=0;$i -lt 20 -and (Get-ScheduledTask -TaskName $name).State -eq 'Running';$i++){Start-Sleep -Milliseconds 100}
 if((Get-ScheduledTask -TaskName $name).State -eq 'Running'){throw 'Task still live'}
 Unregister-ScheduledTask -TaskName $name -Confirm:$false
}
'Verified baselines; all DWM024 tasks ended and removed'

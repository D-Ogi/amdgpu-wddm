$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted016'
if(!(Test-Path "$d\restored.json")){throw 'No restoration witness'}
if((Get-ScheduledTask -TaskName BC250-G0-DwmRun016).State -eq 'Running'){throw 'Runner still active'}
if((Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'UMD baseline mismatch'}
if((Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'){throw 'ICD baseline mismatch'}
foreach($name in 'BC250-G0-DwmRun016','BC250-G0-DwmWatch016','BC250-G0-Composition016'){
 Stop-ScheduledTask -TaskName $name
 for($i=0;$i -lt 20 -and (Get-ScheduledTask -TaskName $name).State -eq 'Running';$i++){Start-Sleep -Milliseconds 100}
 if((Get-ScheduledTask -TaskName $name).State -eq 'Running'){throw 'Task still live'}
 Unregister-ScheduledTask -TaskName $name -Confirm:$false
}
'Verified baselines; all DWM016 tasks ended and removed'

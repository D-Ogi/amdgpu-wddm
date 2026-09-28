$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted012'
$task='BC250-G0-CpuMove013'
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
if((Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Unexpected UMD'}
if((Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'){throw 'Unexpected ICD'}
if(Test-Path "$d\cpu013.json"){throw 'Existing control'}
$before=@(Get-Process dwm | Select-Object -ExpandProperty Id)
$who=(Get-CimInstance Win32_ComputerSystem).UserName
$action=New-ScheduledTaskAction -Execute "$d\composition-control.exe"
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 1)) | Out-Null
$success=$false
try {
 Start-ScheduledTask -TaskName $task
 for($i=0;$i -lt 6;$i++){
  Start-Sleep -Seconds 5
  if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
  if((Get-ScheduledTask -TaskName $task).State -ne 'Running'){throw 'Window task ended'}
  if(Compare-Object $before @(Get-Process dwm | Select-Object -ExpandProperty Id)){throw 'DWM changed'}
  if($i -eq 3){
   & C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe fbdump "$d\cpu013.bmp" *> "$d\cpu013-dump.log"
   if($LASTEXITCODE -ne 0){throw 'Primary dump failed'}
  }
 }
 $success=$true
} finally {
 Stop-ScheduledTask -TaskName $task
 Get-Process composition-control -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq "$d\composition-control.exe"} | Stop-Process -Force -ErrorAction SilentlyContinue
 Unregister-ScheduledTask -TaskName $task -Confirm:$false
 @{success=$success;utc=[DateTime]::UtcNow.ToString('o');dwm_before=$before;dwm_after=@(Get-Process dwm | Select-Object -ExpandProperty Id)} | ConvertTo-Json | Set-Content "$d\cpu013.json"
}
Get-Content "$d\cpu013.json"

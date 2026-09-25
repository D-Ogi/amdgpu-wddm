$ErrorActionPreference='Stop'
$cts='C:\BC250\m12\cts-sparse-full151q'
if(@(Get-ScheduledTask BC250-M12-cts-sparse-full151q | Where-Object State -in @('Running','Queued')).Count){throw 'CTS still active'}
if(Get-Process deqp-vk -ErrorAction SilentlyContinue){throw 'CTS process still active'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
$restored=Get-Content "$cts\registration-restored.json" -Raw | ConvertFrom-Json
if($restored.candidate_present -or $restored.old_value -ne 0){throw 'CTS restoration incomplete'}
$b=Get-Content "$cts\registration-before.json" -Raw | ConvertFrom-Json
$b.candidate_manifest='C:\BC250\m12\mesa05-ib-collection\radeon_icd.json'
$names=@((Get-ItemProperty $b.class_path).VulkanDriverName)
if($names.Count -ne 1 -or $names[0] -ne $b.old_manifest){throw 'Unexpected current ICD'}
if((Get-FileHash 'C:\BC250\m12\mesa05-ib-collection\vulkan_radeon.dll').Hash -ne '8B5EC055501CD042F723F9EBC9FC29787A6173C61937A739DD7C4C520DBD2AA6'){throw 'Candidate hash mismatch'}
$reg='C:\BC250\m12\zink-probe-reg151-006'
New-Item -ItemType Directory -Path $reg -ErrorAction Stop | Out-Null
$b | ConvertTo-Json -Depth 5 | Set-Content "$reg\before.json"
try{
 New-ItemProperty -Path $b.global_path -Name $b.old_manifest -PropertyType DWord -Value 1 -Force | Out-Null
 New-ItemProperty -Path $b.global_path -Name $b.candidate_manifest -PropertyType DWord -Value 0 -Force | Out-Null
 New-ItemProperty -Path $b.class_path -Name VulkanDriverName -PropertyType MultiString -Value @($b.candidate_manifest) -Force | Out-Null
 $gallery=Get-Process ShaderGallery3 -ErrorAction SilentlyContinue
 if($gallery){$gallery | Stop-Process}
 $g=Get-ScheduledTask BC250-M12-ShaderGalleryView -ErrorAction SilentlyContinue
 if($g.State -eq 'Running'){Stop-ScheduledTask BC250-M12-ShaderGalleryView}
 if(@(Get-ScheduledTask 'BC250-M12-*' | Where-Object {$_.State -in @('Running','Queued')}).Count){throw 'M12 task active'}
 $who=(Get-CimInstance Win32_ComputerSystem).UserName
 if(-not $who){throw 'No interactive user'}
 $taskName='BC250-M12-app-zink-probe151-006'
 if(Get-ScheduledTask $taskName -ErrorAction SilentlyContinue){throw 'Task exists'}
 $arguments='-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\BC250\m12\zink-probe006\worker.ps1 -Out C:\BC250\m12\zink-probe151-006'
 $action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument $arguments
 $principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
 $settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::FromSeconds(100)) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
 Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings | Out-Null
 Start-ScheduledTask $taskName
 $limit=[DateTime]::UtcNow.AddSeconds(240)
 do{
  Start-Sleep -Seconds 2
  $task=Get-ScheduledTask BC250-M12-app-zink-probe151-006
  if($task.State -notin @('Running','Queued')){break}
 }while([DateTime]::UtcNow -lt $limit)
 if($task.State -in @('Running','Queued')){throw 'Task still active after deadline; inspect before another test'}
 Get-Content 'C:\BC250\m12\zink-probe151-006\result.json' -Raw

}finally{
 New-ItemProperty -Path $b.class_path -Name VulkanDriverName -PropertyType MultiString -Value @($b.driver_names) -Force | Out-Null
 New-ItemProperty -Path $b.global_path -Name $b.old_manifest -PropertyType DWord -Value 0 -Force | Out-Null
 Remove-ItemProperty -Path $b.global_path -Name $b.candidate_manifest -ErrorAction Stop
 @{utc=[DateTime]::UtcNow.ToString('o');old_value=(Get-Item $b.global_path).GetValue($b.old_manifest);candidate_present=((Get-Item $b.global_path).GetValueNames() -contains $b.candidate_manifest);driver_names=@((Get-ItemProperty $b.class_path).VulkanDriverName)} | ConvertTo-Json | Set-Content "$reg\restored.json"
}

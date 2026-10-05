$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted048'
# Refuse to create any task until every staged file matches the package manifest.
if([IO.Path]::GetFullPath($PSScriptRoot) -ne $d){throw 'Wrong staged directory'}
$manifest=Get-Content "$d\manifest.json" -Raw|ConvertFrom-Json
foreach($entry in $manifest.PSObject.Properties){
 if((Get-FileHash -LiteralPath (Join-Path $d $entry.Name)).Hash -ne $entry.Value){throw ('Staged hash mismatch: '+$entry.Name)}
}
if(Test-Path "$d\started"){throw 'Trial already started; inspect original run'}
if(Test-Path "$d\abort"){throw 'Trial has abort receipt; inspect original run'}
$task='BC250-G0-DwmRun048'
if(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue){throw 'Existing task'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $d\run.ps1"
$principal=New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 3)) | Out-Null
Start-ScheduledTask -TaskName $task
Start-Sleep -Seconds 2
Get-ScheduledTask -TaskName $task | Select-Object TaskName,State | ConvertTo-Json

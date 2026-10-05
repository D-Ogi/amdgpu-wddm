$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\duplicate-control001'
$task='BC250-G0-Present123'
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
if(Test-Path "$dir\done123.json"){throw 'Existing run'}
if(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue){throw 'Existing task'}
foreach($file in (Get-ChildItem -LiteralPath $dir -Filter '*.ps1')){
 $tokens=$null;$errors=$null;[System.Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors)|Out-Null
 if($errors.Count){throw ('Parser failure: '+$file.Name)}
}
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $dir\worker123.ps1"
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 4)
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $task
Get-ScheduledTask -TaskName $task | Select-Object TaskName,State | ConvertTo-Json

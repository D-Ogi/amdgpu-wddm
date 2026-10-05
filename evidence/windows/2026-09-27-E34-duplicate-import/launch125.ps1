$ErrorActionPreference='Stop'
$dir='C:\BC250\m13\hosted-runtime053'
$task='BC250-G0-Present125'
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
if(Test-Path "$dir\done125.json"){throw 'Existing run'}
if(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue){throw 'Existing task'}
foreach($file in (Get-ChildItem -LiteralPath $dir -Filter '*.ps1')){
 $tokens=$null;$errors=$null;[System.Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors)|Out-Null
 if($errors.Count){throw ('Parser failure: '+$file.Name)}
}
$baseline=Get-Content 'C:\BC250\m13\duplicate-control001\done123.json' -Raw | ConvertFrom-Json
if($baseline.exit -ne 0){throw 'CPU reference not passed'}
if(@(Get-Process cross-process-control,cross-process-duplicate-control -ErrorAction SilentlyContinue).Count){throw 'Previous control still active'}
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $dir\worker125.ps1"
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 4)
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $task
Get-ScheduledTask -TaskName $task | Select-Object TaskName,State | ConvertTo-Json

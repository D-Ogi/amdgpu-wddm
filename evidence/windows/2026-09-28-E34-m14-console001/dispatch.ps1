param([ValidateSet('Start','Inspect','Cleanup')][string]$Mode,[string]$ManifestSha256)
$ErrorActionPreference='Stop';$d=$PSScriptRoot;$name='BC250-M14-Console001'
if($d -ine 'C:\BC250\m14\console001'){throw 'Wrong directory'}
. "$d\verify-stage.ps1"
if($Mode -eq 'Start'){
 $null=Assert-KmdStage $d $ManifestSha256
 if((Test-Path "$d\requested") -or (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue)){throw 'Consumed attempt'}
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
 $action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument "-NoProfile -ExecutionPolicy Bypass -File $d\run.ps1 -ManifestSha256 $ManifestSha256"
 $principal=New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest
 $settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 90)
 Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings|Out-Null
 [IO.File]::WriteAllText("$d\requested",[DateTime]::UtcNow.ToString('o'))
 Start-ScheduledTask -TaskName $name
}
$task=Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
$result=if(Test-Path "$d\result.json"){Get-Content "$d\result.json" -Raw|ConvertFrom-Json}else{$null}
if($Mode -eq 'Cleanup'){
 if($task.State -eq 'Running' -or !$result){throw 'No terminal result'}
 if($task){Unregister-ScheduledTask -TaskName $name -Confirm:$false}
}
@{state=if($task){[string]$task.State}else{'Missing'};result=$result;cleanup=($Mode -eq 'Cleanup')}|ConvertTo-Json -Depth 6

$ErrorActionPreference='Stop'
$flags=Invoke-RestMethod http://127.0.0.1:2250/flags
if($null -eq $flags.stop -or $flags.stop){throw 'Owner STOP or missing flag'}
if(-not(Test-Path C:\BC250\m9\candidate07145\coldboot-01\before-driver.log)){throw 'Checkpoint absent'}
if((Get-ScheduledTask -TaskName 'BC250 cold145 health recorder').State -ne 'Ready'){throw 'Recorder not ready'}
'full_shutdown_requested='+(Get-Date).ToString('o')
& shutdown.exe /s /t 5 /d p:0:0 /c 'BC250 M9 dedicated AC-cold startup verification'
if($LASTEXITCODE -ne 0){throw 'Shutdown request failed'}
'shutdown_request_accepted'

'listener sha256 ' + (Get-FileHash C:\BC250\emergency\listener.ps1).Hash.Substring(0, 16)
Stop-ScheduledTask -TaskName 'Lab emergency channel'
Start-Sleep 3
Start-ScheduledTask -TaskName 'Lab emergency channel'
Start-Sleep 3
(Get-ScheduledTask -TaskName 'Lab emergency channel').State

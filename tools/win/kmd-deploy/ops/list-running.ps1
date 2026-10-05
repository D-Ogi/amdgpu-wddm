Get-ScheduledTask | Where-Object { $_.State -eq 'Running' -and $_.TaskName -match 'BC250|DWM|G0|WSI' } | ForEach-Object { '{0} {1}' -f $_.TaskPath, $_.TaskName }

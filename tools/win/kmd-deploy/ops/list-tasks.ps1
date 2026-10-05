Get-ScheduledTask | Where-Object { $_.TaskName -like 'BC250*' } | ForEach-Object { '{0} {1} {2}' -f $_.State, $_.TaskName, $_.TaskPath }

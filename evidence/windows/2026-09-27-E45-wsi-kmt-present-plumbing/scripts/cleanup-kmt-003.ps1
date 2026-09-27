# After wsi-kmt-003: remove the task, confirm no vkcube/PresentMon/ETW session, registered ICD untouched.
$task = 'BC250-M12-wsi-kmt-003'
$t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
if ($t) { if ($t.State -eq 'Running') { Stop-ScheduledTask -TaskName $task }; Unregister-ScheduledTask -TaskName $task -Confirm:$false; "task removed" } else { "task absent" }
Get-Process vkcube -ErrorAction SilentlyContinue | ForEach-Object { "killing vkcube $($_.Id)"; Stop-Process -Id $_.Id -Force }
Get-Process PresentMon-2.6.0-x64 -ErrorAction SilentlyContinue | ForEach-Object { "killing presentmon $($_.Id)"; Stop-Process -Id $_.Id -Force }
& logman query -ets 2>&1 | Select-String BC250KMT | ForEach-Object { "ETW session still present: $_"; & logman stop BC250KMT -ets | Out-Null }
"registered_icd " + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash.Substring(0, 8)
"tasks_running " + ((Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250*' -and $_.State -eq 'Running' } | ForEach-Object { $_.TaskName }) -join ', ')

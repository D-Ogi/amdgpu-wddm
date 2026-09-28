# Status of cpucopy-001: task state, done file, log tail; unregisters the task once it is not running.
$dir = 'C:\BC250\m12\witcher3-dx12'
$task = 'BC250-M12-cpucopy-001'
"utc " + [DateTime]::UtcNow.ToString('o')
$t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
"task " + $(if ($t) { $t.State } else { 'absent' })
"done " + (Test-Path "$dir\done-cpucopy-001.json")
if (Test-Path "$dir\done-cpucopy-001.json") { Get-Content "$dir\done-cpucopy-001.json" }
"vkcube " + ((Get-Process vkcube -ErrorAction SilentlyContinue | Measure-Object).Count)
if (Test-Path "$dir\run-cpucopy-001.log") { Get-Content "$dir\run-cpucopy-001.log" | Where-Object { $_ -notmatch '^t=' } | Select-Object -Last 40 }
if ($t -and $t.State -ne 'Running' -and (Test-Path "$dir\done-cpucopy-001.json")) { Unregister-ScheduledTask -TaskName $task -Confirm:$false; 'task_unregistered' }
"registered_icd " + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash.Substring(0, 8)
"dwm " + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)

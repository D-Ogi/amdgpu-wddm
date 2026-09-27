# Poll cts-extsem-002: done file, task state, last log lines, registered ICD hash.
$dir = 'C:\BC250\m12\witcher3-dx12'
if (Test-Path "$dir\done-cts-extsem-002.json") { "DONE " + (Get-Content "$dir\done-cts-extsem-002.json" -Raw) } else { 'RUNNING' }
if (Test-Path "$dir\run-cts-extsem-002.log") { Get-Content "$dir\run-cts-extsem-002.log" | Select-Object -Last 6 }
$t = Get-ScheduledTask -TaskName 'BC250-M12-cts-extsem-002' -ErrorAction SilentlyContinue
if ($t) { "task_state=$($t.State)"; if ((Test-Path "$dir\done-cts-extsem-002.json") -and $t.State -ne 'Running') { Unregister-ScheduledTask -TaskName 'BC250-M12-cts-extsem-002' -Confirm:$false; 'task_unregistered' } }
"icd=" + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash.Substring(0, 8)
"deqp_procs=" + ((Get-Process deqp-vk -ErrorAction SilentlyContinue | Measure-Object).Count)

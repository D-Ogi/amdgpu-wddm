$dir = 'C:\BC250\m12\wsi-kmt'
$task = 'BC250-M12-wsi-kmt-004'
"utc " + [DateTime]::UtcNow.ToString('o')
$t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
"task " + $(if ($t) { $t.State } else { 'absent' })
"done " + (Test-Path "$dir\done-kmt-004.json")
if (Test-Path "$dir\done-kmt-004.json") { Get-Content "$dir\done-kmt-004.json" }
"vkcube " + ((Get-Process vkcube -ErrorAction SilentlyContinue | Measure-Object).Count)
"presentmon " + ((Get-Process PresentMon-2.6.0-x64 -ErrorAction SilentlyContinue | Measure-Object).Count)
if (Test-Path "$dir\run-kmt-004.log") { Get-Content "$dir\run-kmt-004.log" | Select-Object -Last 40 }

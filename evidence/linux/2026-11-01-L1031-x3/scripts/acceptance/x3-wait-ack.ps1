# Wait (at most 150 s) for this boot's X3 acknowledgement run, then show its outcome and the counter.
$name = 'Lab boot counter reset'
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
$logon = Get-WinEvent -FilterHashtable @{ LogName='Security'; Id=4624 } -MaxEvents 40 -ErrorAction SilentlyContinue |
    Where-Object { $_.Properties[8].Value -eq 2 -and $_.Properties[5].Value -eq 'bc250' -and $_.TimeCreated.ToUniversalTime() -gt $boot } |
    Select-Object -Last 1
'boot={0:o} interactive logon={1}' -f $boot, $(if ($logon) { $logon.TimeCreated.ToUniversalTime().ToString('o') } else { 'not found' })
$deadline = (Get-Date).AddSeconds(150)
do {
    $i = Get-ScheduledTaskInfo -TaskName $name
    $done = $i.LastRunTime.ToUniversalTime() -gt $boot -and (Get-ScheduledTask -TaskName $name).State -eq 'Ready'
    if (-not $done) { Start-Sleep -Seconds 5 }
} while (-not $done -and (Get-Date) -lt $deadline)
'task lastRun={0:o} lastResult={1} done={2} now={3:o}' -f $i.LastRunTime.ToUniversalTime(), $i.LastTaskResult, $done, [DateTime]::UtcNow
Get-Content 'C:\BC250\x3\x3-clear.log' -Tail 2
$v = @(Get-Volume | Where-Object { $_.FileSystemLabel -eq 'BC250DIAG' -and $_.DriveLetter })
$g = "$($v[0].DriveLetter):\boot\grub"
'grubenv sha256=' + (Get-FileHash "$g\grubenv" -Algorithm SHA256).Hash
Get-ChildItem $g -Filter 'grubenv.x3-backup-*' | Sort-Object LastWriteTime | Select-Object -Last 1 | ForEach-Object {
    '{0} {1} vars: {2}' -f $_.Name, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0,16),
        (([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($_.FullName)).Substring(25) -split "`n" | Where-Object { $_ -and $_ -notmatch '^#+$' }) -join ' ') }
Get-Process conhost -IncludeUserName -ErrorAction SilentlyContinue | Where-Object StartTime -gt $boot.ToLocalTime() | Select-Object Id, SessionId, UserName, StartTime | Format-Table -AutoSize | Out-String -Width 160

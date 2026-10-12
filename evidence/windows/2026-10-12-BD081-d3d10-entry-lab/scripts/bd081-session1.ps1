# BD-081 lab step in the interactive session: d3d10probe --trace-entry x64 and x86 as a one-shot task of user bc250
# (session 1, where a swap chain can be created). Each run bounded to 60 s; the task is removed at the end.
$ErrorActionPreference = 'Continue'
$dir = 'C:\BC250\tmp\bd081'
$task = 'BD-081 d3d10probe'
$child = Join-Path $dir 'run-s1.cmd'
$lines = @('@echo off')
foreach ($arch in 'x64', 'x86') {
    $lines += "`"$dir\$arch\d3d10probe.exe`" --trace-entry > `"$dir\s1-$arch.txt`" 2>&1"
    $lines += "echo exit %ERRORLEVEL% >> `"$dir\s1-$arch.txt`""
}
$lines += "echo done > `"$dir\s1-done.txt`""
Set-Content -LiteralPath $child -Value $lines -Encoding ASCII
Remove-Item -LiteralPath "$dir\s1-x64.txt", "$dir\s1-x86.txt", "$dir\s1-done.txt" -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
$act = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"$child`""
$pr = New-ScheduledTaskPrincipal -UserId 'bc250' -LogonType Interactive -RunLevel Limited
$st = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 150) -AllowStartIfOnBatteries
$null = Register-ScheduledTask -TaskName $task -Action $act -Principal $pr -Settings $st
Start-ScheduledTask -TaskName $task
$w = [Diagnostics.Stopwatch]::StartNew()
while ($w.Elapsed.TotalSeconds -lt 140 -and -not (Test-Path -LiteralPath "$dir\s1-done.txt")) { Start-Sleep -Milliseconds 500 }
"done $(Test-Path -LiteralPath "$dir\s1-done.txt") after $([int]$w.Elapsed.TotalSeconds) s"
$ti = Get-ScheduledTaskInfo -TaskName $task -ErrorAction SilentlyContinue
"task last result $($ti.LastTaskResult)"
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
foreach ($arch in 'x64', 'x86') { "=== $arch"; Get-Content -LiteralPath "$dir\s1-$arch.txt" -ErrorAction SilentlyContinue }

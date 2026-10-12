# BD-067 lab step: vulkaninfo --text with a VkDevice, once as a one-shot task of the interactive user (session 1) and
# once in this SSH process tree (session 0). Each run bounded to 45 s; a session 0 run still alive at the bound gets a
# user-mode stack from cdb (if present on the lab) and is then killed. Read only otherwise.
$ErrorActionPreference = 'Continue'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$vi = Join-Path $inst 'tools\vulkaninfo.exe'
$dir = 'C:\BC250\tmp\bd067'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Remove-Item -LiteralPath "$dir\*" -Force -ErrorAction SilentlyContinue
"utc $([DateTime]::UtcNow.ToString('o'))"
"vulkaninfo $vi $((Get-FileHash -LiteralPath $vi -Algorithm SHA256).Hash.Substring(0, 8))"

# Session 1
$task = 'BD-067 vulkaninfo'
Set-Content -LiteralPath "$dir\s1.cmd" -Encoding ASCII -Value @(
    '@echo off',
    "`"$vi`" --text > `"$dir\s1.txt`" 2>&1",
    "echo exit %ERRORLEVEL% > `"$dir\s1-exit.txt`"")
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
$act = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"$dir\s1.cmd`""
$pr = New-ScheduledTaskPrincipal -UserId 'bc250' -LogonType Interactive -RunLevel Limited
$st = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 90) -AllowStartIfOnBatteries
$null = Register-ScheduledTask -TaskName $task -Action $act -Principal $pr -Settings $st
$sw = [Diagnostics.Stopwatch]::StartNew()
Start-ScheduledTask -TaskName $task
while ($sw.Elapsed.TotalSeconds -lt 45 -and -not (Test-Path -LiteralPath "$dir\s1-exit.txt")) { Start-Sleep -Milliseconds 250 }
$s1done = Test-Path -LiteralPath "$dir\s1-exit.txt"
"session 1: ended $s1done after $('{0:F1}' -f $sw.Elapsed.TotalSeconds) s; $(if ($s1done) { Get-Content -LiteralPath "$dir\s1-exit.txt" })"
if (-not $s1done) { Get-Process vulkaninfo -ErrorAction SilentlyContinue | Where-Object SessionId -eq 1 | Stop-Process -Force; 'session 1 run killed at 45 s' }
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
$t = Get-Content -LiteralPath "$dir\s1.txt" -ErrorAction SilentlyContinue
"session 1 output lines $($t.Count); deviceName: $(($t | Select-String 'deviceName' | Select-Object -First 1).Line.Trim())"

# Session 0
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $vi -ArgumentList '--text' -NoNewWindow -PassThru -RedirectStandardOutput "$dir\s0.txt" -RedirectStandardError "$dir\s0.err"
$null = $p.Handle
if ($p.WaitForExit(45000)) {
    "session 0: ended exit $($p.ExitCode) after $('{0:F1}' -f $sw.Elapsed.TotalSeconds) s"
} else {
    "session 0: still running at 45 s (pid $($p.Id))"
    $cdb = Get-ChildItem -LiteralPath 'C:\BC250' -Filter cdb.exe -Recurse -File -ErrorAction SilentlyContinue -Depth 6 |
        Where-Object { $_.FullName -match 'x64|amd64' } | Select-Object -First 1
    if ($cdb) {
        "cdb $($cdb.FullName)"
        & $cdb.FullName -pv -p $p.Id -c '.symfix C:\BC250\tmp\sym; ~*kn 30; qd' 2>&1 | Select-Object -Last 120 | Set-Content -LiteralPath "$dir\s0-stack.txt"
        "stack lines $((Get-Content -LiteralPath "$dir\s0-stack.txt").Count)"
    } else { 'no cdb.exe under C:\BC250' }
    $p.Kill(); 'session 0 run killed'
}
$t = Get-Content -LiteralPath "$dir\s0.txt" -ErrorAction SilentlyContinue
"session 0 output lines $($t.Count); last: $(($t | Select-Object -Last 1))"

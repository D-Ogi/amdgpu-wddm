# LAB (elevated SSH): starts input-server.ps1 in the lab user's interactive session as a one-shot scheduled task
# "Lab game input" (the name stays outside the kits' 'BC250|DWM|G0|WSI' competing-task gate), bounded by -Seconds
# (<= 1200, owner's game-session bound) plus 30 s of task limit. -Stop ends it: a quit command first, then the task.
param([string]$Session = '', [int]$Seconds = 900, [switch]$Stop)
$ErrorActionPreference = 'Stop'
$task = 'Lab game input'
$server = 'C:\BC250\tmp\input\input-server.ps1'
if ($Stop) {
    if ($Session) {
        $dir = "C:\BC250\tmp\control\$Session"
        $n = @(Get-ChildItem -LiteralPath $dir -Filter 'cmd-*.txt' -File -ErrorAction SilentlyContinue).Count + 1
        [IO.File]::WriteAllText((Join-Path $dir ('cmd-{0:d3}.txt' -f $n)), 'quit', [Text.Encoding]::ASCII)
        Start-Sleep -Seconds 2
    }
    $t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
    if ($t) { if ($t.State -eq 'Running') { Stop-ScheduledTask -TaskName $task }; Unregister-ScheduledTask -TaskName $task -Confirm:$false }
    'stopped'
    return
}
if ($Session -notmatch '^game-[a-z0-9-]{3,40}$') { throw 'bad session name' }
if (-not (Test-Path $server)) { throw "missing $server" }
$Seconds = [Math]::Min([Math]::Max($Seconds, 10), 1200)
if (@(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue | Where-Object { $_.State -eq 'Running' }).Count) { throw 'input server already running' }
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
$dir = "C:\BC250\tmp\control\$Session"
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
$null = New-Item -ItemType Directory -Force -Path $dir
$act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$server`" -Session $Session -Seconds $Seconds"
$pr = New-ScheduledTaskPrincipal -UserId 'bc250' -LogonType Interactive -RunLevel Highest
$st = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ($Seconds + 30)) -AllowStartIfOnBatteries
$null = Register-ScheduledTask -TaskName $task -Action $act -Principal $pr -Settings $st
Start-ScheduledTask -TaskName $task
Start-Sleep -Seconds 3
"started '$task' session $Session bound $Seconds s"
Get-Content (Join-Path $dir 'server.log') -ErrorAction SilentlyContinue

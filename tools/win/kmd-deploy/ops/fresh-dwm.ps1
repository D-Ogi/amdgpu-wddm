# Operator pre-step before a desktop-umd173 attempt (RUNBOOK Part 2). desktop-umd173-002 lost its candidate budget
# because the 3 h old DWM took 17.6 s to exit at RestartDwm (a 70 s old one took 0.4 s) and the KMD ready clock
# restarts with the new DWM. This pays that exit outside the task: restart DWM, time the exit and the respawn,
# wait for the new ready interval and confirm it, so admission (flags 15, fresh-DWM bound) passes.
# Bounded: exit 30 s, respawn 15 s, ready 80 s. Never runs next to a transition task.
$ErrorActionPreference = 'Stop'
$cli = 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
function Read-Health {
    $t = & $cli health read | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'Health read failed' }
    if ($t -notmatch 'flags=(\d+) generation=(\d+) epoch=(\d+) completed=(\d+) age_ms=(\d+) ready_ms=(\d+)') { throw 'Health line unreadable' }
    # age_ms is UINT64_MAX until the first completion after an invalidation (start_health.c:166).
    [pscustomobject]@{ flags = [int]$Matches[1]; generation = $Matches[2]; epoch = $Matches[3]; completed = [UInt64]$Matches[4]
        age_ms = [UInt64]$Matches[5]; ready_ms = [UInt64]$Matches[6]; text = $t.Trim() }
}
function Assert-NoStop { if ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) { throw 'Owner STOP' } }
Assert-NoStop
if (@(Get-ScheduledTask | Where-Object { $_.State -eq 'Running' -and $_.TaskName -match '^BC250-(KMD|UMD)(17[34])?-Watch$' }).Count) { throw 'A transition task is running' }
$old = @(Get-Process dwm -ErrorAction SilentlyContinue)
if ($old.Count -ne 1) { throw 'Exactly one DWM required' }
$oldStart = $old[0].StartTime.ToUniversalTime()
$before = Read-Health
$r = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); old_pid = $old[0].Id; old_start = $oldStart.ToString('o')
    old_age_s = ([DateTime]::UtcNow - $oldStart).TotalSeconds; health_before = $before.text }
$sw = [Diagnostics.Stopwatch]::StartNew()
Stop-Process -Id $old[0].Id -Force
if (!$old[0].WaitForExit(30000)) { throw 'Old DWM did not exit within 30 s' }
$r.exit_s = $sw.Elapsed.TotalSeconds
$fresh = @()
while (!$fresh.Count -and $sw.Elapsed.TotalSeconds -lt $r.exit_s + 15) {
    $fresh = @(Get-Process dwm -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $old[0].Id -or $_.StartTime.ToUniversalTime() -ne $oldStart })
    if (!$fresh.Count) { Start-Sleep -Milliseconds 100 }
}
if ($fresh.Count -ne 1) { throw "No single new DWM after the restart ($($fresh.Count))" }
$r.respawn_s = $sw.Elapsed.TotalSeconds
$r.new_pid = $fresh[0].Id
$r.new_start = $fresh[0].StartTime.ToUniversalTime().ToString('o')
# The same adapter start must reach the confirmation rule of verify-cpu.ps1 (Assert-KmdConfirmEligible).
$deadline = $sw.Elapsed.TotalSeconds + 80
$h = $null
while ($sw.Elapsed.TotalSeconds -lt $deadline) {
    Assert-NoStop
    $h = Read-Health
    if ($h.generation -ne $before.generation) { throw 'Adapter start changed during the DWM restart' }
    if ($h.flags -in @(7, 15) -and $h.completed -gt 0 -and $h.ready_ms -ge 60000 -and $h.age_ms -le 15000) { break }
    $h = $null
    Start-Sleep -Milliseconds 1000
}
if (!$h) { throw 'Ready interval did not reach 60 s' }
$r.eligible_s = $sw.Elapsed.TotalSeconds
$r.health_eligible = $h.text
if ($h.flags -eq 7) {
    $confirm = & $cli health confirm $h.generation $h.epoch | Out-String
    if ($LASTEXITCODE -ne 0) { throw "Health confirm failed: $($confirm.Trim())" }
    $r.health_confirm = $confirm.Trim()
}
$after = Read-Health
if ($after.flags -ne 15 -or $after.generation -ne $h.generation -or $after.epoch -ne $h.epoch) { throw "Not confirmed: $($after.text)" }
$r.health_after = $after.text
$now = @(Get-Process dwm -ErrorAction SilentlyContinue)
if ($now.Count -ne 1 -or $now[0].Id -ne $r.new_pid) { throw 'DWM replaced during the ready interval' }
$r.modules = @($now[0].Modules | Where-Object { $_.ModuleName -match 'bc250|vulkan_radeon|amdgpu_wddm' } | ForEach-Object { $_.FileName })
$r | ConvertTo-Json -Depth 4

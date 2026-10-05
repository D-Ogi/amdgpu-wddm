# Starts etw-capture.ps1 as a one-shot SYSTEM task for game trial NNN (runs through target.py ps, elevated
# session 0). The capture's deadline is the trial's own (Codex 854): watch.ps1 records its QPC origin in the
# stage's start.json and cuts the game runtime at origin + 255 s (templates/watch.ps1, $runtimeEnd for a game);
# the capture gets origin + 250 s on the same QPC clock, so operator delay before this script cannot extend it.
# Refuses when the trial has not started, is not a game trial, has less than 60 s left, when the trial
# directory already holds a capture, or when PerfView is not the staged, hashed copy. The task keeps a 5-minute
# limit as an outer bound only.
param([Parameter(Mandatory)][ValidatePattern('^[0-9]{3}$')][string]$Trial, [int]$Seconds = 30, [int]$StartA = 5,
    [int]$DwmPct = 15, [int]$LatestB = 110, [int]$FpsSeconds = 0, [int]$WorldSeconds = 0, [switch]$PresentMode)
$ErrorActionPreference = 'Stop'
$perf = 'C:\BC250\tools\perfview\PerfView.exe'
$expect = 'E6B89A6DA0FE7DA5F64302306153C6AAC7A4BE67C800426A91AE014093E4DF2D'
if ((Get-FileHash -LiteralPath $perf).Hash -ne $expect) { throw 'PerfView hash mismatch' }
$stage = "C:\BC250\m15\native-caps$Trial"
$config = Get-Content -LiteralPath "$stage\config.json" -Raw | ConvertFrom-Json
if ($config.remote -ine $stage -or !$config.game) { throw "not a game trial stage: $stage" }
$startFile = "$stage\start.json"
if (!(Test-Path -LiteralPath $startFile)) { throw 'trial not started (no start.json)' }
$origin = [long](Get-Content -LiteralPath $startFile -Raw | ConvertFrom-Json).origin
$freq = [Diagnostics.Stopwatch]::Frequency
# config.game_seconds (since 161, owner-consented longer game trial) moves the deadline with watch.ps1's bounds.
$extra = if ($config.game_seconds) { [int]$config.game_seconds - 300 } else { 0 }
$notAfter = $origin + (250 + $extra) * $freq
$left = ($notAfter - [Diagnostics.Stopwatch]::GetTimestamp()) / [double]$freq
if ($left -lt 60) { throw ('only {0:0.0} s left before the trial deadline' -f $left) }
$root = "C:\BC250\m15\etw\$Trial"
if (Test-Path -LiteralPath (Join-Path $root 'etw-notes.txt')) { throw "capture exists in $root" }
$null = New-Item -ItemType Directory -Force -Path $root
$script = 'C:\BC250\tmp\etw-capture.ps1'
if (!(Test-Path -LiteralPath $script)) { throw 'etw-capture.ps1 not staged' }
# Game runner: the profile's ETW process names (the stage's game-profile.json) reach the capture as -Process.
# Witcher 3's (witcher3, the capture's default) add nothing, so its task line stays as before.
$process = ''
if (Test-Path -LiteralPath "$stage\game-profile.json") {
    $names = @((Get-Content -LiteralPath "$stage\game-profile.json" -Raw | ConvertFrom-Json).processes.etw | Where-Object { $_ })
    if ($names.Count -and ($names -join ',') -ne 'witcher3') { $process = $names -join ',' }
}
if ($process -and !(Select-String -LiteralPath $script -SimpleMatch -Quiet -Pattern '[string]$Process')) {
    throw 'the staged etw-capture.ps1 has no -Process: push tools\win\lab-runner\etw\etw-capture.ps1 to C:\BC250\tmp first'
}
Copy-Item -LiteralPath $script -Destination (Join-Path $root 'etw-capture.ps1')
$name = "BC250-Etw$Trial"
if (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) { throw "task $name already registered; run etw-closure.ps1 first" }
# -FpsSeconds N (Full HD FPS tests): DxgKrnl only, no window A, window B from the world (walk start in the
# trial's game log) for up to N s. etw-capture.ps1 keeps its own deadline and reserve.
$fps = ''
if ($FpsSeconds -gt 0) { $fps = " -GpuOnly -SkipA -SecondsB $FpsSeconds -WorldLog `"$stage\game\game-log.txt`"" }
# -WorldSeconds N (owner 2026-09-30, "40 ms per frame off the GPU"): the same world-triggered window B with the full
# PerfView CPU profile (samples, context switches) next to DxgKrnl, for etw-cpu.py.
elseif ($WorldSeconds -gt 0) { $fps = " -SkipA -SecondsB $WorldSeconds -WorldLog `"$stage\game\game-log.txt`" -ReserveSeconds 25" }
if ($process) { $fps += " -Process `"$process`"" }
# -PresentMode (M15.14): Win32k and Dwm-Core next to DxgKrnl, for etw-present-mode.py. The staged capture script
# must know the switch, or the task would start and ignore it.
if ($PresentMode) {
    if (!(Select-String -LiteralPath $script -SimpleMatch -Quiet -Pattern '[switch]$PresentMode')) {
        throw 'the staged etw-capture.ps1 has no -PresentMode: push tools\win\lab-runner\etw\etw-capture.ps1 to C:\BC250\tmp first' }
    $fps += ' -PresentMode'
}
$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ("-NoProfile -ExecutionPolicy Bypass -File `"$root\etw-capture.ps1`" -Root `"$root`" -Tag N$Trial -NotAfterQpc $notAfter -Seconds $Seconds -StartA $StartA -DwmPct $DwmPct -LatestB $LatestB$fps")
$s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds (300 + $extra)) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
$p = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$null = Register-ScheduledTask -TaskName $name -Action $a -Settings $s -Principal $p
Start-ScheduledTask -TaskName $name
"started $name root $root utc $([DateTime]::UtcNow.ToString('o')) origin $origin not_after_qpc $notAfter left_s $([Math]::Round($left,1)) capture_sha256 $((Get-FileHash -LiteralPath (Join-Path $root 'etw-capture.ps1')).Hash)"

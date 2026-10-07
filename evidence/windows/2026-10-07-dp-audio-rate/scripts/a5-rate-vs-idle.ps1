# DP audio rate at the idle point versus out of it. KMD 0.7.216.4 (AFMT memories on) consumes 60 s of audio in 107 s
# at an idle desktop, where the DPM idle state holds 500 MHz and the SOC power reads about 1 W (13.6 W at 1000 MHz).
# Play the 10 s 48 kHz tone twice: once idle, once with a light GPU load that keeps the governor off the idle point.
# The dpm line is sampled in the middle of each playback.
$ErrorActionPreference = 'Continue'
$cli = 'C:\BC250\dpaudio\bc250kmd_cli.exe'
$runner = 'C:\BC250\m14\app-route-001\ops\app-run.ps1'
$dir = 'C:\BC250\tmp\audio'
function Play([string]$tag) {
    $p = New-Object Media.SoundPlayer "$dir\t48.wav"; $p.Load()
    $job = Start-Job -ArgumentList $cli -ScriptBlock { param($cli) Start-Sleep -Seconds 4; & $cli dpm 2>&1 | Select-Object -Last 1 }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try { $p.PlaySync() } catch { $_.Exception.Message }
    '{0}: PlaySync {1:N3} s for 10.000 s of audio' -f $tag, $sw.Elapsed.TotalSeconds
    Receive-Job $job -Wait | ForEach-Object { "   mid: $_" }; Remove-Job $job
}
Play 'idle'
$gpu = Start-Process powershell.exe -PassThru -WindowStyle Hidden -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -File "{0}" -Client d3d11bench -Seconds 60 -Arguments "--mode offscreen --scenes fill --layers 2 --size 640x360 --frames 100000"' -f $runner)
Start-Sleep -Seconds 8
& $cli dpm 2>&1 | Select-Object -Last 1
Play 'loaded'
Get-Process d3d11bench -ErrorAction SilentlyContinue | Stop-Process -Force
& $cli smu 2>&1 | Select-Object -First 14

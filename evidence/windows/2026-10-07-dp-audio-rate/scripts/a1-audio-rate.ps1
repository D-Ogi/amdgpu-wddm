# r19 DP audio: no sound (owner, headphones on the monitor) and Edge video at ~0.8x. Measure how fast the DP endpoint
# consumes samples: PlaySync of a 10.000 s WAV takes 10 s at the right rate, 12.5 s at 0.8x. 48 kHz and 44.1 kHz.
# The stream registers are read once during the 48 kHz playback. Session 0 (SSH) first.
$ErrorActionPreference = 'Continue'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$dir = 'C:\BC250\tmp\audio'
$null = New-Item -ItemType Directory -Force -Path $dir
function Wav([string]$path, [int]$rate, [double]$sec) {
    $n = [int]($rate * $sec); $ch = 2; $bytes = $n * $ch * 2
    $ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter($ms)
    $w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $bytes)); $w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
    $w.Write([int]16); $w.Write([int16]1); $w.Write([int16]$ch); $w.Write([int]$rate); $w.Write([int]($rate * $ch * 2)); $w.Write([int16]($ch * 2)); $w.Write([int16]16)
    $w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]$bytes)
    for ($i = 0; $i -lt $n; $i++) { $v = [int16](8000 * [Math]::Sin(2 * [Math]::PI * 1000 * $i / $rate)); $w.Write($v); $w.Write($v) }
    $w.Flush(); [IO.File]::WriteAllBytes($path, $ms.ToArray())
}
Wav "$dir\t48.wav" 48000 10
Wav "$dir\t44.wav" 44100 10
'--- render endpoints'
Get-PnpDevice -Class AudioEndpoint -PresentOnly | Format-Table -AutoSize Status, FriendlyName | Out-String -Width 160
$mm = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
Get-ChildItem $mm | ForEach-Object {
    $st = (Get-ItemProperty $_.PSPath).DeviceState
    $name = (Get-ItemProperty "$($_.PSPath)\Properties" -ErrorAction SilentlyContinue).'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
    $fmt = (Get-ItemProperty "$($_.PSPath)\Properties" -ErrorAction SilentlyContinue).'{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0'
    $rate = if ($fmt) { [BitConverter]::ToInt32($fmt, 12) } else { '' }
    'render state {0} name {1} device-format rate {2}' -f $st, $name, $rate
}
'audiosrv: ' + (Get-Service Audiosrv).Status
foreach ($f in 't48', 't44') {
    $p = New-Object Media.SoundPlayer "$dir\$f.wav"; $p.Load()
    if ($f -eq 't48') {
        $job = Start-Job -ScriptBlock { param($cli) Start-Sleep -Seconds 4; & $cli dpaudio 2>&1 | Select-String 'SEC_CNTL|AFMT_CNTL' } -ArgumentList $cli
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try { $p.PlaySync(); $err = '' } catch { $err = $_.Exception.Message }
    '{0}: PlaySync {1:N3} s for 10.000 s of audio {2}' -f $f, $sw.Elapsed.TotalSeconds, $err
    if ($f -eq 't48') { Receive-Job $job -Wait | ForEach-Object { '   during: ' + $_.Line.Trim() }; Remove-Job $job }
}

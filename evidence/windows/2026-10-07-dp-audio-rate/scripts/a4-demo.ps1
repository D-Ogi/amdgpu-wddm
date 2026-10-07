# DP audio demo for the owner (headphones on the monitor): 60 s of a stereo 48 kHz melody (C major arpeggios, 0.25 s
# notes, left/right alternation every 4 bars), then the time PlaySync took (60 s at the right rate) and the stream
# state. Overlay first, so the owner knows when to listen.
$ErrorActionPreference = 'Continue'
$cli = 'C:\BC250\dpaudio\bc250kmd_cli.exe'
$dir = 'C:\BC250\tmp\audio'
$null = New-Item -ItemType Directory -Force -Path $dir
$rate = 48000; $sec = 60; $n = $rate * $sec
$notes = 261.63, 329.63, 392.00, 523.25, 392.00, 329.63, 261.63, 196.00, 220.00, 261.63, 329.63, 440.00, 329.63, 261.63, 220.00, 174.61
$ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter($ms)
$bytes = $n * 4
$w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $bytes)); $w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
$w.Write([int]16); $w.Write([int16]1); $w.Write([int16]2); $w.Write([int]$rate); $w.Write([int]($rate * 4)); $w.Write([int16]4); $w.Write([int16]16)
$w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]$bytes)
$noteLen = [int]($rate / 4)
for ($i = 0; $i -lt $n; $i++) {
    $k = [int][Math]::Floor($i / $noteLen); $f = $notes[$k % $notes.Count]; $t = ($i % $noteLen) / $rate
    $env = [Math]::Min(1.0, $t * 200) * [Math]::Exp(-3.0 * $t)
    $v = 9000 * $env * [Math]::Sin(2 * [Math]::PI * $f * $i / $rate)
    $side = [int][Math]::Floor($k / 16) % 3        # 0 both, 1 left, 2 right
    $l = if ($side -eq 2) { 0 } else { $v }; $r = if ($side -eq 1) { 0 } else { $v }
    $w.Write([int16]$l); $w.Write([int16]$r)
}
$w.Flush(); [IO.File]::WriteAllBytes("$dir\demo60.wav", $ms.ToArray())
$p = New-Object Media.SoundPlayer "$dir\demo60.wav"; $p.Load()
"start $([DateTime]::UtcNow.ToString('o'))"
$sw = [Diagnostics.Stopwatch]::StartNew()
try { $p.PlaySync(); $err = '' } catch { $err = $_.Exception.Message }
'demo60: PlaySync {0:N3} s for 60.000 s of audio {1}' -f $sw.Elapsed.TotalSeconds, $err
& $cli dpaudio 2>&1 | Select-String 'stream on|stream half|DP0_SEC_CNTL|M_READBACK|record|state'
& $cli read DMU.mmDIO_MEM_PWR_CTRL 2>&1
& $cli read DMU.mmDIO_MEM_PWR_STATUS 2>&1

# LAB, interactive session (started by the one-shot scheduled task of t2-tone.ps1): DP audio step 3 tone arms.
# Writes marker files so the SSH side can read the audio registers while a tone is actually playing.
#   arm 1  wasapi-probe excl-poll  48000 16-bit stereo, 10 s, 1 kHz tone   (IAudioClock position vs QPC)
#   arm 2  wasapi-probe excl-event 48000 16-bit stereo, 10 s, 1 kHz tone   (same, plus the DMA notification pacing)
#   arm 3  wasapi-probe shared-event (mix format), 10 s                     (the path an ordinary application takes)
#   arm 4  SoundPlayer.PlaySync of a generated 10.000 s WAV                 (the BD-092 symptom: 10 s took 30.6 s)
# Nothing is written to the hardware; these are ordinary WASAPI/winmm render streams.
param([string]$Dir = 'C:\BC250\tmp\dpaudio-step3')
$ErrorActionPreference = 'Continue'
function Mark([string]$n) { [IO.File]::WriteAllText((Join-Path $Dir $n), [DateTime]::UtcNow.ToString('o')) }
$out = Join-Path $Dir 'child-out.txt'
function Say([string]$s) { Add-Content -LiteralPath $out -Value $s }
$exe = Join-Path $Dir 'wasapi-probe.exe'
Say "child session=$((Get-Process -Id $PID).SessionId) user=$env:USERNAME utc=$([DateTime]::UtcNow.ToString('o'))"
$arms = @(
  @{ name = 'excl-poll-48000';  args = @('excl-poll',  '-rate', '48000', '-secs', '10') },
  @{ name = 'excl-event-48000'; args = @('excl-event', '-rate', '48000', '-secs', '10') },
  @{ name = 'shared-event';     args = @('shared-event', '-secs', '10') }
)
foreach ($a in $arms) {
  Say ""
  Say "### arm $($a.name)  wasapi-probe $($a.args -join ' ')  utc $([DateTime]::UtcNow.ToString('HH:mm:ss.fff'))"
  Mark "begin-$($a.name)"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $part = Join-Path $Dir "part-$($a.name).txt"
  $p = Start-Process -FilePath $exe -ArgumentList $a.args -NoNewWindow -PassThru -RedirectStandardOutput $part
  if (-not $p.WaitForExit(30000)) { $p.Kill(); Say 'KILLED after 30 s' }
  Mark "end-$($a.name)"
  Get-Content -LiteralPath $part | Where-Object { $_ -notmatch '^sample ' } | ForEach-Object { Say $_ }
  Say ("wall_s={0:F3} exit={1}" -f $sw.Elapsed.TotalSeconds, $p.ExitCode)
}

# arm 4: a generated 10.000 s WAV through SoundPlayer.PlaySync. A stream that consumes samples at the right
# rate returns in 10.0 s. r19 took 30.6 s for the same file (BD-092).
Say ""
Say "### arm playsync-wav  utc $([DateTime]::UtcNow.ToString('HH:mm:ss.fff'))"
$wav = Join-Path $Dir 'tone-1khz-48k.wav'
if (-not (Test-Path -LiteralPath $wav)) {
  $rate = 48000; $secs = 10; $frames = $rate * $secs; $bytes = $frames * 4
  $fs = [IO.File]::Create($wav); $bw = New-Object IO.BinaryWriter($fs)
  $bw.Write([char[]]'RIFF'); $bw.Write([uint32](36 + $bytes)); $bw.Write([char[]]'WAVE')
  $bw.Write([char[]]'fmt '); $bw.Write([uint32]16); $bw.Write([uint16]1); $bw.Write([uint16]2)
  $bw.Write([uint32]$rate); $bw.Write([uint32]($rate * 4)); $bw.Write([uint16]4); $bw.Write([uint16]16)
  $bw.Write([char[]]'data'); $bw.Write([uint32]$bytes)
  $buf = New-Object byte[] $bytes
  for ($i = 0; $i -lt $frames; $i++) {
    $v = [int16](8192 * [Math]::Sin(2 * [Math]::PI * 1000 * $i / $rate))
    $b = [BitConverter]::GetBytes($v)
    $o = $i * 4; $buf[$o] = $b[0]; $buf[$o+1] = $b[1]; $buf[$o+2] = $b[0]; $buf[$o+3] = $b[1]
  }
  $bw.Write($buf); $bw.Flush(); $bw.Close(); $fs.Close()
}
Say "wav bytes=$((Get-Item -LiteralPath $wav).Length) sha256=$((Get-FileHash -Algorithm SHA256 -LiteralPath $wav).Hash.Substring(0,8)) nominal_s=10.000"
Mark 'begin-playsync'
$sw = [Diagnostics.Stopwatch]::StartNew()
try {
  $sp = New-Object Media.SoundPlayer $wav
  $sp.Load()
  $load = $sw.Elapsed.TotalSeconds
  $sp.PlaySync()
  Say ("load_s={0:F3} playsync_s={1:F3} ratio_vs_10s={2:F4}" -f $load, ($sw.Elapsed.TotalSeconds - $load), (($sw.Elapsed.TotalSeconds - $load) / 10.0))
} catch { Say "playsync error: $_" }
Mark 'end-playsync'
Say "done utc=$([DateTime]::UtcNow.ToString('o'))"
[IO.File]::WriteAllText((Join-Path $Dir 'done.json'), '{"done":true}')

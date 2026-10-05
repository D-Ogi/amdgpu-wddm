# Audible signal on the development PC when a game trial starts on the lab (owner request, 2026-09-29).
# Plays one Windows alarm sound of a few seconds. No window and no resident process. Falls back to
# console beeps when the sound file is missing.
param(
    [ValidateRange(1, 3)][int]$Times = 1,
    [string]$Sound
)
if (-not $Sound) {
    $mediaRoot = if ($env:windir) { $env:windir } else { 'C:\Windows' }
    $Sound = Join-Path $mediaRoot 'Media\Alarm01.wav'
}
for ($i = 0; $i -lt $Times; $i++) {
    try {
        if (!(Test-Path -LiteralPath $Sound)) { throw "no sound file at $Sound" }
        $player = New-Object Media.SoundPlayer $Sound
        $player.PlaySync()
        $player.Dispose()
    } catch {
        foreach ($f in 880, 1175, 1568) { [Console]::Beep($f, 250) }
    }
}

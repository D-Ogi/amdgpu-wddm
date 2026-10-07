# DP audio rate probe on unit A (about 75 s of playback, under the 3-minute lab bound). Read-only towards the
# hardware: ordinary WASAPI streams on the default render endpoint, a 1 kHz tone at -12 dBFS (amp 0.25).
# Push first:  python tools\win\target.py push P:\BC-250\scratch\dp-audio\probe\wasapi-probe.cs P:\BC-250\scratch\dp-audio\probe\run-probe.ps1 --to C:\BC250\tmp\dpaudio-probe
# Then run:    python tools\win\target.py ps P:\BC-250\scratch\dp-audio\probe\run-probe.ps1
# Optional argument: a label for the output file (default "base"), e.g. "msi" after hda-msi.ps1 -On.
param([string]$Label = 'base', [double]$Secs = 10)
$ErrorActionPreference = 'Continue'
$dir = 'C:\BC250\tmp\dpaudio-probe'
$null = New-Item -ItemType Directory -Force -Path $dir
$exe = Join-Path $dir 'wasapi-probe.exe'
$src = Join-Path $dir 'wasapi-probe.cs'
if (-not (Test-Path $exe) -or ((Get-Item $src).LastWriteTime -gt (Get-Item $exe).LastWriteTime)) {
    & "$env:windir\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /optimize /platform:x64 "/out:$exe" $src
    if ($LASTEXITCODE -ne 0) { 'build failed'; exit 1 }
}
$out = Join-Path $dir ("probe-{0}-{1:yyyyMMdd-HHmmss}.txt" -f $Label, (Get-Date).ToUniversalTime())
$runs = @(
    @('info'),
    @('excl-poll',  '-rate', '48000'),
    @('excl-event', '-rate', '48000'),
    @('excl-poll',  '-rate', '44100'),
    @('excl-event', '-rate', '44100'),
    @('shared-event'),
    @('shared-poll')
)
foreach ($r in $runs) {
    $args2 = @($r) + @('-secs', "$Secs")
    "### wasapi-probe $($args2 -join ' ')  utc $((Get-Date).ToUniversalTime().ToString('HH:mm:ss.fff'))" | Tee-Object -FilePath $out -Append
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -ArgumentList $args2 -NoNewWindow -PassThru -RedirectStandardOutput "$out.part"
    if (-not $p.WaitForExit(25000)) { $p.Kill(); "probe killed after 25 s" | Tee-Object -FilePath $out -Append }
    Get-Content "$out.part" | Where-Object { $_ -notmatch '^sample ' -or $_ -match 't=(0\.\d|[1-9]\.[02]|\d\d\.[02])' } | Tee-Object -FilePath $out -Append
    Get-Content "$out.part" | Add-Content -Path ($out -replace '\.txt$', '-samples.txt')
    Remove-Item "$out.part" -ErrorAction SilentlyContinue
    "wall_s={0:F2} exit={1}" -f $sw.Elapsed.TotalSeconds, $p.ExitCode | Tee-Object -FilePath $out -Append
}
"output: $out"

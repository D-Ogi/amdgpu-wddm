# One Cyberpunk 2077 benchmark run of the VM flush arm on unit A (host side, pwsh 7), from a fixed start temperature.
# The settings are those of the 1920x1200 runs of scratch\cyberpunk\t32 (r11, r12): the linux-high profile, full
# screen, no overlay, the foreground guard and PresentMon. The DPM state is whatever the lab has (check it first).
#   pwsh cp-run.ps1 -Label a1 [-StartC 54] [-CoolBound 1200]
# Refuses (exit 3) when a game already runs, (exit 4) when the start temperature does not come within -CoolBound.
param(
    [Parameter(Mandatory)][string]$Label,
    [double]$StartC = 54,
    [int]$CoolBound = 1200,
    [string]$Out = 'P:\BC-250\scratch\vmflush\arm'
)
$ErrorActionPreference = 'Stop'
$env:BC250_ROOT = 'P:\BC-250'
Set-Location P:\BC-250\bc250-win
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out "cp-$Label.log"
function Stamp([string]$m) { $line = "$([DateTime]::UtcNow.ToString('o')) $m"; Add-Content -LiteralPath $log -Value $line; $line }

$games = @(python tools\win\target.py ps P:\BC-250\scratch\kmd-lab-kit\gamecheck.ps1 2>&1 | ForEach-Object { [string]$_ })
$games | ForEach-Object { Stamp "gamecheck: $_" } | Out-Null
if (-not ($games -contains 'games 0')) { Stamp 'refused: a game runs, or the check failed'; exit 3 }

$t0 = Get-Date
while ($true) {
    $line = [string](python tools\win\bc250rd\temp.py 2>&1 | Select-Object -Last 1)
    $c = if ($line -match 'Tctl ([0-9.]+) C') { [double]$Matches[1] } else { 999 }
    Stamp "cool: $line" | Out-Null
    if ($c -le $StartC) { break }
    if (((Get-Date) - $t0).TotalSeconds -gt $CoolBound) { Stamp "refused: Tctl not at $StartC C within $CoolBound s"; exit 4 }
    Start-Sleep -Seconds 30
}
python tools\win\target.py ps P:\BC-250\scratch\vmflush\lab\vmflush-summary.ps1 2>&1 | ForEach-Object { Stamp "before: $_" } | Out-Null
powershell -NoProfile -ExecutionPolicy Bypass -File P:/BC-250/scratch/monitors/alarm.ps1 *> $null
$pt = Join-Path $Out "pt-$Label.log"
$t0Run = Get-Date
$job = Start-Job -ArgumentList $pt -ScriptBlock {
    param($pt)
    $env:BC250_ROOT = 'P:\BC-250'
    Set-Location P:\BC-250\bc250-win
    python tools/win/target.py ps P:/BC-250/scratch/pathtrace/lab/pt-run.ps1 -Demo cp2077-bench -GameDir 'S:\GOG Galaxy\Games\Cyberpunk 2077' -CpProfile linux-high -Width 1920 -Height 1200 -Fullscreen -Interactive -Seconds 420 -HiddenConsole -NoLoaderDebug -Overlay off -FgGuard 'C:\BC250\tmp\fg-guard.ps1' -PresentMon 'C:\BC250\tmp\bd165\PresentMon-2.6.0-x64.exe' *> $pt
    $LASTEXITCODE
}
# Camera frames inside the benchmark (it runs at about 40-110 s after the start): the picture on the screen is the
# game's, not the desktop's, and the case shows no smoke or glow. The frames are cropped by snap.sh.
foreach ($at in 70, 100) {
    $wait = $at - ((Get-Date) - $t0Run).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Seconds ([int]$wait) }
    if ($job.State -ne 'Running') { break }
    $frame = [string](& 'C:\Program Files\Git\bin\bash.exe' P:/BC-250/scratch/labcam/snap.sh 2>&1 | Select-Object -Last 1)
    if ($frame -match 'cam-(\d{8}T\d{6}Z)\.jpg') {
        $src = "P:\BC-250\scratch\labcam\cam-$($Matches[1]).jpg"
        $dst = Join-Path $Out "cam-$Label-$at.jpg"
        Move-Item -LiteralPath $src -Destination $dst -Force
        Stamp "camera t=$at s: $dst" | Out-Null
    } else { Stamp "camera t=$at s: no frame ($frame)" | Out-Null }
}
$code = Receive-Job -Job $job -Wait | Select-Object -Last 1
Remove-Job -Job $job
Stamp "pt-run exit $code" | Out-Null
python tools\win\target.py ps P:\BC-250\scratch\vmflush\lab\vmflush-summary.ps1 2>&1 | ForEach-Object { Stamp "after: $_" } | Out-Null
Select-String -LiteralPath $pt -Pattern 'gate temperature|clock: clock|foreground:|benchmark: averageFps|settings:|NOT|run directory|end:' |
    ForEach-Object { Stamp ("run: " + $_.Line.Substring([Math]::Min(28, $_.Line.Length))) }

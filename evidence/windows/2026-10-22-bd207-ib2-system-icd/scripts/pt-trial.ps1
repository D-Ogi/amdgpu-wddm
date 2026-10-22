# One bounded pt-run.ps1 trial on unit A (host side, pwsh 7), from a fixed start temperature: the game check, the
# cool-down, the alarm on this PC right before the start, the run itself in a job, cropped camera frames at fixed
# times (fire watch, and proof that the picture on the screen is the game's), and the key lines of the run log.
# Generalized from scratch\vmflush\cp-run.ps1 (2026-10-22): every pt-run.ps1 argument comes through -PtArgs, so
# no arm needs a copy of this file.
#
#   & pt-trial.ps1 -Label q2p-rel32 -Out P:\BC-250\scratch\bd207-ib2\gate `
#       -PtArgs '-Demo','q2rtx-timedemo','-RtApi','pipeline','-Width','1280','-Height','720','-Seconds','150',
#               '-Overlay','off','-SetEnv','VK_DRIVER_FILES=C:\BC250\tmp\vkroute-cp\icd-rel32\radeon_icd.json'
#       [-StartC 60] [-CoolBound 1200] [-CameraAt 70,140] [-Probe <lab .ps1, run before and after>]
#
# Call it with & from pwsh, not with pwsh -File: -PtArgs is a string array.
# Exit 3: a game already runs, or the game check failed (the owner may be playing). Exit 4: the start temperature
# did not come within -CoolBound. Otherwise the exit code of target.py ps, which is the exit code of pt-run.ps1.
# pt-run.ps1 holds the time bound, the thermal stop and the STOP flag itself. This script never cycles power and
# stops no process.
param(
    [Parameter(Mandatory)][string]$Label,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string[]]$PtArgs,
    [double]$StartC = 60,
    [int]$CoolBound = 1200,
    [int[]]$CameraAt = @(70, 140),
    [string]$Probe = ''
)
$ErrorActionPreference = 'Stop'
$env:BC250_ROOT = 'P:\BC-250'
Set-Location P:\BC-250\bc250-win
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out "trial-$Label.log"
$pt = Join-Path $Out "pt-$Label.log"
if (Test-Path -LiteralPath $pt) { throw "$pt exists: use a new -Label" }
function Stamp([string]$m) { $line = "$([DateTime]::UtcNow.ToString('o')) $m"; Add-Content -LiteralPath $log -Value $line; $line }
Stamp "pt-run args: $($PtArgs -join ' ')" | Out-Null

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
if ($Probe) { python tools\win\target.py ps $Probe 2>&1 | ForEach-Object { Stamp "before: $_" } | Out-Null }
powershell -NoProfile -ExecutionPolicy Bypass -File P:/BC-250/scratch/monitors/alarm.ps1 *> $null
$t0Run = Get-Date
$job = Start-Job -ArgumentList $pt, $PtArgs -ScriptBlock {
    param($pt, $ptArgs)
    $env:BC250_ROOT = 'P:\BC-250'
    Set-Location P:\BC-250\bc250-win
    python tools/win/target.py ps P:/BC-250/scratch/pathtrace/lab/pt-run.ps1 @ptArgs *> $pt
    $LASTEXITCODE
}
foreach ($at in $CameraAt) {
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
Stamp "pt-run exit $code, elapsed $([int]((Get-Date) - $t0Run).TotalSeconds) s" | Out-Null
# One frame after the run: the case and the screen once the load has stopped.
$frame = [string](& 'C:\Program Files\Git\bin\bash.exe' P:/BC-250/scratch/labcam/snap.sh 2>&1 | Select-Object -Last 1)
if ($frame -match 'cam-(\d{8}T\d{6}Z)\.jpg') {
    $dst = Join-Path $Out "cam-$Label-after.jpg"
    Move-Item -LiteralPath "P:\BC-250\scratch\labcam\cam-$($Matches[1]).jpg" -Destination $dst -Force
    Stamp "camera after: $dst" | Out-Null
} else { Stamp "camera after: no frame ($frame)" | Out-Null }
if ($Probe) { python tools\win\target.py ps $Probe 2>&1 | ForEach-Object { Stamp "after: $_" } | Out-Null }
if (Test-Path -LiteralPath $pt) {
    Select-String -LiteralPath $pt -Pattern 'gate temperature|extra env|clock: clock|module vulkan_radeon|Using VK_KHR|frames, .* fps|foreground:|benchmark: averageFps|settings:|presentmode:|NOT |run directory|end:' |
        ForEach-Object { Stamp ("run: " + $_.Line.Substring([Math]::Min(28, $_.Line.Length))) }
}
exit ([int]$code)

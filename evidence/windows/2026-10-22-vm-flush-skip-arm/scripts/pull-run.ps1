# Copies the result files of one arm run from unit A to arm\<label>\ (host side, pwsh 7): the game's frames.csv and
# summary.json, the PresentMon CSV and the run log. The run directory comes from the pt-<label>.log of cp-run.ps1.
#   pwsh pull-run.ps1 -Label a1 [-Out P:\BC-250\scratch\vmflush\arm]
param(
    [Parameter(Mandatory)][string]$Label,
    [string]$Out = 'P:\BC-250\scratch\vmflush\arm'
)
$ErrorActionPreference = 'Stop'
$env:BC250_ROOT = 'P:\BC-250'
Set-Location P:\BC-250\bc250-win
$pt = Join-Path $Out "pt-$Label.log"
$m = Select-String -LiteralPath $pt -Pattern 'run directory (C:\\BC250\\pathtrace\\runs\\\S+)' | Select-Object -Last 1
if (-not $m) { throw "no run directory in $pt" }
$run = $m.Matches[0].Groups[1].Value
$dst = Join-Path $Out $Label
New-Item -ItemType Directory -Force $dst | Out-Null
foreach ($rel in 'benchmarkResults\frames.csv', 'benchmarkResults\summary.json', 'presentmon.csv') {
    python tools\win\target.py pull "$run\$rel" (Join-Path $dst $rel) 2>&1 | ForEach-Object { "pull $rel : $_" }
}
Get-ChildItem -LiteralPath $dst -Recurse -File | ForEach-Object { '{0} {1} {2}' -f $_.FullName.Substring($dst.Length + 1), $_.Length, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.Substring(0, 16) }

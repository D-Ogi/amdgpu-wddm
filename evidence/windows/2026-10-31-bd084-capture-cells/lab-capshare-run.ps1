# LAB SIDE, interactive session (started through o77-task.ps1): run one capture cell of capshare.exe (BD-084, M15.13)
# and write its verdict to -Out; "<Out>.done" marks the end. The cells dda and wgc show a borderless window and read
# the screen, so they must run in the logged-on session, never in session 0.
#   capshare-run.ps1 -Out <file> -Cell dda|wgc [-Bound 100] [-Format bgra8] [-Extra '<more capshare options>']
# -Extra is one string, split at spaces (a scheduled task passes one argument string), for example
# '--wgc-item monitor' or '--producer-window app'.
param([Parameter(Mandatory)][string]$Out, [string]$Cell = 'dda', [int]$Bound = 100, [string]$Format = 'bgra8',
      [string]$Extra = '')
$ErrorActionPreference = 'Continue'
$dir = 'C:\BC250\tmp\capshare'
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$base = Join-Path $dir "$Cell-$stamp"
$lines = @()
$lines += "CAPSHARE cell=$Cell start=$stamp bound=$Bound extra='$Extra' session=$([System.Diagnostics.Process]::GetCurrentProcess().SessionId) user=$env:USERNAME"
$exe = Join-Path $dir 'capshare.exe'
$peer = Join-Path $dir 'capshare-peer.exe'
$lines += "CAPSHARE exe sha256=$((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash)"
$args = @('--cell', $Cell, '--interactive-ok', '--bound', "$Bound", '--format', $Format,
          '--peer-exe', $peer, '--out', "$base.txt", '--json', "$base.json", '--stderr', "$base-err.txt", '--dbwin', 'dwm.exe')
$args += @($Extra -split ' ' | Where-Object { $_ })
$lines += "CAPSHARE cmd: $exe $($args -join ' ')"
try {
    $p = Start-Process -FilePath $exe -ArgumentList $args -PassThru -Wait
    $lines += "CAPSHARE exit=$($p.ExitCode) end=$((Get-Date).ToUniversalTime().ToString('HH:mm:ssZ'))"
} catch {
    $lines += "CAPSHARE start failed: $($_.Exception.Message)"
}
if (Test-Path "$base.txt") {
    $v = Get-Content "$base.txt" | Select-String -Pattern '\bVERDICT cell=' | Select-Object -Last 1
    if ($v) { $lines += "CAPSHARE $($v.Line)" } else { $lines += "CAPSHARE no VERDICT line in $base.txt" }
    $lines += "CAPSHARE log $base.txt lines $((Get-Content "$base.txt").Count)"
} else { $lines += "CAPSHARE no log at $base.txt" }
$lines | Set-Content -Path $Out -Encoding UTF8
New-Item -ItemType File -Path "$Out.done" -Force | Out-Null

# W3 RT effect-cost series: sets or clears the two per-session lab switches.
#  -Perftest <value>  writes C:\BC250\tools\radv-perftest.txt (game-runtime.ps1 251+ reads it into RADV_PERFTEST of a
#                     direct game start and records it as radv_perftest); no value removes the file.
#  -Pause on|off      creates or removes C:\BC250\mon\graphics-summary.pause (the overlay's log-summary poll, M464).
param([string]$Perftest = '', [ValidateSet('on', 'off')][string]$Pause = 'off')
$ErrorActionPreference = 'Stop'
$marker = 'C:\BC250\tools\radv-perftest.txt'
if ($Perftest) {
    if ($Perftest -notmatch '^[a-z0-9_,]{1,64}$') { throw 'Bad perftest value' }
    [IO.File]::WriteAllText($marker, $Perftest, (New-Object Text.UTF8Encoding($false)))
} elseif (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker -Force }
$pauseFile = 'C:\BC250\mon\graphics-summary.pause'
if ($Pause -eq 'on') { if (!(Test-Path -LiteralPath $pauseFile)) { New-Item -ItemType File -Path $pauseFile | Out-Null } }
elseif (Test-Path -LiteralPath $pauseFile) { Remove-Item -LiteralPath $pauseFile -Force }
"perftest marker: $(if (Test-Path -LiteralPath $marker) { (Get-Content -LiteralPath $marker -Raw).Trim() } else { 'absent' })"
"summary pause: $(Test-Path -LiteralPath $pauseFile)"

$src = 'C:\BC250\m15\kmd175-deploy003\kmd-transition'
$dst = 'C:\BC250\tmp\pfprobe'
if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
Copy-Item -LiteralPath $src -Destination $dst -Recurse
Copy-Item -LiteralPath 'C:\BC250\tmp\postflight-probe.ps1' -Destination "$dst\postflight-probe.ps1" -Force
try { & "$dst\postflight-probe.ps1" -Directory 'C:\BC250\m15\kmd175-deploy003' 2>&1 | Select-String 'RUNNING' | ForEach-Object Line } catch { "ERR $_" }

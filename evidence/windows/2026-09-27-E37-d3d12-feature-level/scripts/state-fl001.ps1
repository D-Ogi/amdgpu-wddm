# Read-only lab state for the feature level probe: registered ICD, sparse candidate, loader, stage directory.
$ErrorActionPreference = 'Continue'
function H($p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.Substring(0,8) + ' ' + (Get-Item -LiteralPath $p).Length } else { 'MISSING' } }
"time     " + (Get-Date -Format o)
"boot     " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
"stop     " + (Test-Path -LiteralPath 'C:\BC250\STOP')
"icd-reg  " + (Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers' -ErrorAction SilentlyContinue | Out-String).Trim()
"icd-sys  " + (H 'C:\BC250\m10\wsi-final\vulkan_radeon.dll')
"icd-q    " + (H 'C:\BC250\m12\mesa05-queue\vulkan_radeon.dll')
"icd-cand " + (H 'C:\BC250\m12\mesa05-candidate\vulkan_radeon.dll')
"loader   " + (H 'C:\Windows\System32\vulkan-1.dll')
"stage    " + (Test-Path -LiteralPath 'C:\BC250\m12\fl-probe001')
"dwm      " + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
"tasks    " + ((Get-ScheduledTask -TaskPath '\' -ErrorAction SilentlyContinue | Where-Object { $_.State -eq 'Running' } | ForEach-Object { $_.TaskName }) -join ',')
"procs    " + ((Get-Process | Where-Object { $_.ProcessName -match 'runtime|vkcompute|deqp|cts|fl-probe|d3d12|Asteroids|witcher' } | ForEach-Object { $_.ProcessName + ':' + $_.Id }) -join ',')

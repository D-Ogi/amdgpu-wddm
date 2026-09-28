# Promote the registered Vulkan ICD C:\BC250\m10\wsi-final\vulkan_radeon.dll from 9C40083C (pre-M496) to the
# KMT-enumeration candidate 93B1D1FD (fork branch amdgpu-wddm/radv-wddm2-kmt-enum, c34ab7cd on 940ab0eb).
# Owner's decision 2026-09-27 ("okej") after the M546 flip control kmtflip001 passed on this candidate.
# - the old file stays next to it as vulkan_radeon.9C40083C.dll (rollback: copy it back over the active file);
# - the promoted file is also kept as vulkan_radeon.93B1D1FD.dll (restore target for candidate swaps);
# - no registry change (same manifest and path), no UMD change, no DWM restart, no KMD change;
# - positive control afterwards: E14 compute smoke through the recorded runner on the promoted file in place.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m10\wsi-final'
$active = "$dir\vulkan_radeon.dll"
$oldHash = '9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'
$candidate = 'C:\BC250\m12\fl-probe001\candidate-kmt-enum\vulkan_radeon.dll'
$newHash = '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'
$rollback = "$dir\vulkan_radeon.9C40083C.dll"
$keep = "$dir\vulkan_radeon.93B1D1FD.dll"
$out = 'C:\BC250\m13\kmt-flip001\promote001'
$smokeRunner = 'C:\BC250\m13\fork-consolidated001\run-smoke.ps1'
function Temp-Now { $raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String; if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { [double]$Matches[1] } else { -1 } }

if ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) { throw 'Owner STOP' }
if (Test-Path $out) { throw 'Result directory exists' }
if ((Get-FileHash -LiteralPath $active).Hash -ne $oldHash) { throw 'Unexpected registered ICD' }
if ((Get-FileHash -LiteralPath $candidate).Hash -ne $newHash) { throw 'Unexpected candidate' }
if (Test-Path $rollback) { throw 'Rollback file already present' }
if (Test-Path $keep) { throw 'Keep file already present' }
$running = Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250*' -and $_.State -eq 'Running' -and $_.TaskName -notin 'BC250 monitor overlay', 'BC250 net watchdog' }
if ($running) { throw ('A BC250 test task is running: ' + (($running | ForEach-Object { $_.TaskName }) -join ', ')) }
if (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'runtime-|fl-probe|vkcompute' }) { throw 'A test process is running' }
if (-not (Test-Path $smokeRunner)) { throw 'Smoke runner missing' }
$t = Temp-Now; "temperature_before=$t"; if ($t -ge 85) { throw 'Temperature limit' }
New-Item -ItemType Directory -Path $out | Out-Null
"boot " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
"start_utc " + (Get-Date).ToUniversalTime().ToString('o')
"dwm_before=" + ((Get-Process dwm | Select-Object -ExpandProperty Id) -join ',')
"--- registry (unchanged by this script)"
foreach ($k in 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers') { if (Test-Path $k) { (Get-Item $k).Property | ForEach-Object { "$_ = $((Get-ItemProperty $k).$_)" } } }
"manifest " + (Get-FileHash -LiteralPath "$dir\radeon_icd.json").Hash
"--- before"
Get-ChildItem -LiteralPath $dir -File | ForEach-Object { "{0} {1} {2}" -f (Get-FileHash -LiteralPath $_.FullName).Hash.Substring(0,8), $_.Length, $_.Name }

Copy-Item -LiteralPath $active -Destination $rollback
if ((Get-FileHash -LiteralPath $rollback).Hash -ne $oldHash) { throw 'Rollback copy mismatch' }
'rollback copy written'
Copy-Item -LiteralPath $candidate -Destination $active -Force
$now = (Get-FileHash -LiteralPath $active).Hash
"active=$now"
if ($now -ne $newHash) {
  'PROMOTION_MISMATCH: restoring old file'
  Copy-Item -LiteralPath $rollback -Destination $active -Force
  "restored=" + (Get-FileHash -LiteralPath $active).Hash
  throw 'Promotion mismatch'
}
Copy-Item -LiteralPath $candidate -Destination $keep
if ((Get-FileHash -LiteralPath $keep).Hash -ne $newHash) { throw 'Keep copy mismatch' }
'promoted'
"--- after"
Get-ChildItem -LiteralPath $dir -File | ForEach-Object { "{0} {1} {2}" -f (Get-FileHash -LiteralPath $_.FullName).Hash.Substring(0,8), $_.Length, $_.Name }

$config = @{
  exe = 'C:\BC250\m8\vkcompute.exe'
  arguments = 'C:\BC250\m8\spv --runs 1'
  timeout_seconds = 90
  pass_pattern = '(?m)^8 test\(s\) run, 0 mismatch\(es\)\s*$'
  modules = @(@{ name = 'vulkan_radeon.dll'; path = $active; sha256 = $newHash })
} | ConvertTo-Json -Depth 4
Set-Content -LiteralPath "$out\smoke-config.json" -Value $config -Encoding ASCII
& $smokeRunner -Config "$out\smoke-config.json" -Out "$out\smoke"
"smoke_exit=$LASTEXITCODE"
if (Test-Path "$out\smoke\receipt.json") { Get-Content "$out\smoke\receipt.json" }
if (Test-Path "$out\smoke\stdout.txt") { Get-Content "$out\smoke\stdout.txt" | Select-Object -Last 3 }
"final_active=" + (Get-FileHash -LiteralPath $active).Hash
"dwm_after=" + ((Get-Process dwm | Select-Object -ExpandProperty Id) -join ',')
"temperature_after=$(Temp-Now)"
"end_utc " + (Get-Date).ToUniversalTime().ToString('o')

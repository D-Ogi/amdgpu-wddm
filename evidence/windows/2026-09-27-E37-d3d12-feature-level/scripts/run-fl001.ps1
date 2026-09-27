# E37 feature level probe: vkd3d-proton (app-local d3d12.dll + DXVK dxgi.dll) over the registered RADV ICD.
# Four bounded runs of fl-probe.exe (CheckFeatureSupport only, no rendering, no window):
#   A  baseline ICD 9C40083C, default environment
#   B  baseline ICD 9C40083C, RADV_EXPERIMENTAL=sparse
#   C  sparse candidate 4D027149 (C:\BC250\m12\mesa05-queue) swapped into the registered path, default environment
#   D  same candidate, RADV_EXPERIMENTAL=sparse
# The swap follows the E36/M538 procedure: backup, hash check, restore in finally, hash check. Nothing else changes.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\fl-probe001'
$out = "$dir\out"
$active = 'C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$baselineHash = '9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'
$candidate = 'C:\BC250\m12\mesa05-queue\vulkan_radeon.dll'
$candidateHash = '4D027149571DC000DA1E5006E6E393FCA6178DB32F1D9CB25D684E60849A5805'
$backup = "$dir\baseline-9C40083C.dll"
$probeHash = '30F69C0577D35921'

function Stop-Requested { try { (Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop } catch { Test-Path -LiteralPath 'C:\BC250\STOP' } }
function Temp-Now { $raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String; if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { [double]$Matches[1] } else { -1 } }
function Run-Probe([string]$label, [hashtable]$env) {
  if (Stop-Requested) { throw 'Owner STOP' }
  $t = Temp-Now; "$label temperature_before=$t"; if ($t -ge 85) { throw 'Temperature limit' }
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = "$dir\fl-probe.exe"
  $psi.WorkingDirectory = $dir
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.EnvironmentVariables['VKD3D_DEBUG'] = 'info'
  $psi.EnvironmentVariables['DXVK_LOG_LEVEL'] = 'info'
  $psi.EnvironmentVariables['DXVK_LOG_PATH'] = 'none'
  foreach ($k in $env.Keys) { $psi.EnvironmentVariables[$k] = $env[$k] }
  $p = [System.Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  if (-not $p.WaitForExit(60000)) { try { $p.Kill() } catch {}; "$label TIMEOUT" }
  $p.WaitForExit()
  Set-Content -LiteralPath "$out\$label.json" -Value $stdout.Result -Encoding ASCII
  Set-Content -LiteralPath "$out\$label.stderr.txt" -Value $stderr.Result -Encoding UTF8
  "$label exit=$($p.ExitCode) icd_in_registered_path=$((Get-FileHash -LiteralPath $active).Hash.Substring(0,8))"
  $j = $stdout.Result
  foreach ($key in 'description','max_feature_level','tiled_resources_tier','resource_binding_tier','conservative_rasterization_tier','rovs_supported','highest_shader_model','devices_created') {
    $m = [regex]::Matches($j, '"' + $key + '": ("[^"]*"|[^,\s}]+)')
    "$label $key=" + (($m | ForEach-Object { $_.Groups[1].Value }) -join ' | ')
  }
}

if (Stop-Requested) { throw 'Owner STOP' }
if ((Get-FileHash -LiteralPath $active).Hash -ne $baselineHash) { throw 'Unexpected baseline ICD' }
if ((Get-FileHash -LiteralPath $candidate).Hash -ne $candidateHash) { throw 'Unexpected candidate ICD' }
if ((Get-FileHash -LiteralPath "$dir\fl-probe.exe").Hash.Substring(0,16) -ne $probeHash) { throw 'Unexpected probe' }
if (Test-Path $out) { throw 'Result directory exists' }
New-Item -ItemType Directory -Path $out | Out-Null
"boot " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
"start " + (Get-Date -Format o)
Get-ChildItem -LiteralPath $dir -File | ForEach-Object { "{0} {1} {2}" -f (Get-FileHash -LiteralPath $_.FullName).Hash.Substring(0,8), $_.Length, $_.Name }

$code = 0
Run-Probe 'A-baseline-default' @{}
Run-Probe 'B-baseline-sparse' @{ RADV_EXPERIMENTAL = 'sparse' }

Copy-Item -LiteralPath $active -Destination $backup -Force
if ((Get-FileHash -LiteralPath $backup).Hash -ne $baselineHash) { throw 'Backup mismatch' }
try {
  Copy-Item -LiteralPath $candidate -Destination $active -Force
  if ((Get-FileHash -LiteralPath $active).Hash -ne $candidateHash) { throw 'Candidate swap mismatch' }
  'candidate in registered path'
  Run-Probe 'C-candidate-default' @{}
  Run-Probe 'D-candidate-sparse' @{ RADV_EXPERIMENTAL = 'sparse' }
} catch {
  "PROBE_ERROR $($_.Exception.Message)"
  $code = 1
} finally {
  Copy-Item -LiteralPath $backup -Destination $active -Force
  $restored = (Get-FileHash -LiteralPath $active).Hash
  "restored=$restored"
  if ($restored -ne $baselineHash) { 'RESTORE_MISMATCH'; $code = 2 } else { Remove-Item -LiteralPath $backup -Force }
  "temperature_after=$(Temp-Now)"
  "dwm=" + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
  "end " + (Get-Date -Format o)
}
"probe_exit=$code"
exit $code

# E37 run 007: feature-level probe on the promoted registered ICD 93B1D1FD (E38), no swap.
#   A-default : control, environment as in runs 005/006 (expected FL 11_1, TiledResourcesTier 0)
#   B-sparse  : RADV_EXPERIMENTAL=sparse (the WDDM port's sparse gate, radv_physical_device.c) -> tier and FL
# fl-probe.exe with vkd3d-proton + DXVK dxgi next to it, 30 s limit, minidump on timeout. Interactive session.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\fl-probe001'
$out = "$dir\out007"
$active = 'C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$baselineHash = '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'
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
  $psi.EnvironmentVariables['DXVK_LOG_PATH'] = "$out\$label"
  New-Item -ItemType Directory -Path "$out\$label" | Out-Null
  foreach ($k in $env.Keys) { $psi.EnvironmentVariables[$k] = $env[$k] }
  "$label env=" + (($env.Keys | ForEach-Object { "$_=$($env[$_])" }) -join ' ')
  $p = [System.Diagnostics.Process]::Start($psi)
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  if (-not $p.WaitForExit(30000)) {
    "$label TIMEOUT pid=$($p.Id) writing minidump"
    $dump = "$out\$label.dmp"
    $d = Start-Process -FilePath rundll32.exe -ArgumentList "C:\Windows\System32\comsvcs.dll, MiniDump $($p.Id) $dump full" -Wait -PassThru -NoNewWindow
    "$label minidump_exit=$($d.ExitCode) size=$((Get-Item -LiteralPath $dump -ErrorAction SilentlyContinue).Length)"
    try { $p.Kill() } catch {}
  }
  $p.WaitForExit()
  Set-Content -LiteralPath "$out\$label.json" -Value $stdout.Result -Encoding ASCII
  Set-Content -LiteralPath "$out\$label.stderr.txt" -Value $stderr.Result -Encoding UTF8
  "$label exit=$($p.ExitCode) icd_in_registered_path=$((Get-FileHash -LiteralPath $active).Hash.Substring(0,8))"
  $j = $stdout.Result
  foreach ($key in 'description','max_feature_level','tiled_resources_tier','resource_binding_tier','conservative_rasterization_tier','rovs_supported','typed_uav_load_additional_formats','highest_shader_model','raytracing_tier','mesh_shader_tier','variable_shading_rate_tier','devices_created') {
    $m = [regex]::Matches($j, '"' + $key + '": ("[^"]*"|[^,\s}]+)')
    "$label $key=" + (($m | ForEach-Object { $_.Groups[1].Value }) -join ' | ')
  }
}

if (Stop-Requested) { throw 'Owner STOP' }
if ((Get-FileHash -LiteralPath $active).Hash -ne $baselineHash) { throw 'Unexpected registered ICD' }
if ((Get-FileHash -LiteralPath "$dir\fl-probe.exe").Hash.Substring(0,16) -ne $probeHash) { throw 'Unexpected probe' }
if (Test-Path $out) { throw 'Result directory exists' }
New-Item -ItemType Directory -Path $out | Out-Null
"boot " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
"start " + (Get-Date -Format o)
Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Extension -in '.exe', '.dll' } | ForEach-Object { "{0} {1} {2}" -f (Get-FileHash -LiteralPath $_.FullName).Hash.Substring(0,8), $_.Length, $_.Name }
"registered $((Get-FileHash -LiteralPath $active).Hash) $((Get-Item -LiteralPath $active).Length)"

$code = 0
try {
  Run-Probe 'A-default' @{}
  Run-Probe 'B-sparse' @{ 'RADV_EXPERIMENTAL' = 'sparse' }
} catch {
  "PROBE_ERROR $($_.Exception.Message)"
  $code = 1
} finally {
  "final_registered=" + (Get-FileHash -LiteralPath $active).Hash
  "temperature_after=$(Temp-Now)"
  "dwm=" + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
  "probe_procs=" + ((Get-Process fl-probe -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')
  "end " + (Get-Date -Format o)
}
"probe_exit=$code"
exit $code

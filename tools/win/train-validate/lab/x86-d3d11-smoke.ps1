# A 32-bit D3D11 smoke on the installed wow64 stack, so the x86 ICD is exercised through D3D11 (shell +
# DXVK + ICD) as well as through Vulkan. Generic: it names no train and no package. Short run, read-only
# apart from the output directory. The modules the process loaded are read while it runs.
param([string]$Exe = 'C:\BC250\tmp\b23-x86\d3d11bench-x86\d3d11bench.exe',
      [string]$Out = 'C:\BC250\tmp\train-x86')
$ErrorActionPreference = 'Continue'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$exe = $Exe
$out = $Out
New-Item -ItemType Directory -Force $out | Out-Null
$cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
function Faults { $l = & $cli log 2>&1; '{0} faults, {1} timeouts' -f ($l | Select-String 'GPU FAULT').Count, ($l | Select-String 'HARDWARE FENCE TIMEOUT|ResetEngine node').Count }
"start $([DateTime]::UtcNow.ToString('o')); before: $(Faults)"
"client $((Get-FileHash $exe -Algorithm SHA256).Hash.Substring(0,8)) $exe"
foreach ($rel in 'wow64\d3d11\amdgpu_wddm_d3d11.dll', 'wow64\d3d11\amdgpu_wddm_dxvk.dll', 'wow64\d3d11\amdgpu_wddm_radv.dll', 'wow64\desktop\bc250d3d_router.dll') {
    $p = Join-Path $inst $rel
    '{0} {1}' -f (Get-FileHash $p -Algorithm SHA256).Hash.Substring(0, 8), $rel
}
"app router Mode=$((Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\AppRouter').Mode)"
$env:AMDGPU_WDDM_LOG = 'file:' + "$out\umd.log"
$s = Get-Date
$p = Start-Process $exe -ArgumentList "--mode offscreen --frames 60 --warmup 10 --deadline 40 --out $out\result.json" -NoNewWindow -PassThru -RedirectStandardOutput "$out\stdout.txt" -RedirectStandardError "$out\stderr.txt" -WorkingDirectory $out
$null = $p.Handle
Start-Sleep -Milliseconds 2500
$mods = @()
try { $mods = @($p.Modules | Where-Object { $_.FileName -like "$inst*" } | ForEach-Object { '{0} {1}' -f (Get-FileHash $_.FileName -Algorithm SHA256).Hash.Substring(0, 8), $_.FileName.Substring($inst.Length) }) } catch { "modules: $($_.Exception.Message)" }
if (-not $p.WaitForExit(60000)) { $p.Kill(); 'd3d11bench killed at 60 s' }
'exit {0} after {1:N1} s' -f $p.ExitCode, ((Get-Date) - $s).TotalSeconds
'--- modules of ours in the 32-bit process'
if ($mods.Count) { $mods | ForEach-Object { '  ' + $_ } } else { '  none read (process ended first); see the UMD log below' }
Remove-Item Env:\AMDGPU_WDDM_LOG -ErrorAction SilentlyContinue
'--- stdout'
Get-Content "$out\stdout.txt" -ErrorAction SilentlyContinue | Select-Object -Last 14 | ForEach-Object { '  ' + $_ }
'--- stderr'
Get-Content "$out\stderr.txt" -ErrorAction SilentlyContinue | Select-Object -Last 6 | ForEach-Object { '  ' + $_ }
'--- UMD log lines that name the stack'
Get-Content "$out\umd.log" -ErrorAction SilentlyContinue | Select-String -Pattern 'settings|icd|radv|dxvk|route|adapter|device' | Select-Object -Last 10 | ForEach-Object { '  ' + $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
"after: $(Faults)"

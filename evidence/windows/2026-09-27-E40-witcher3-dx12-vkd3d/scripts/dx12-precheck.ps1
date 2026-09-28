# Read-only precheck for the Witcher 3 DX12 run: game settings relevant to renderer choice, RT, DLSS, resolution;
# KMD health tool availability; vkd3d-proton package hashes; registered ICD; running processes.
$ErrorActionPreference = 'Continue'
$doc = Join-Path $env:USERPROFILE 'Documents\The Witcher 3'
"--- user.settings (renderer/RT/DLSS/FSR/resolution/window keys only)"
$us = Join-Path $doc 'user.settings'
if (Test-Path -LiteralPath $us) {
  $section = ''
  foreach ($line in Get-Content -LiteralPath $us) {
    if ($line -match '^\[(.+)\]') { $section = $Matches[1]; continue }
    if ($line -match '^(Resolution|FullScreenMode|VSync|DLSS|FSR|RayTrac|Upscal|FrameGen|HardwareAccel|TextureMemoryBudget|Preset|Streamline|GraphicsPreset|Hairworks|LimitFPS|AntiAlias|NvidiaReflex|Sharpen|MaxFps)' ) { "[$section] $line" }
  }
  "settings_sha256=" + (Get-FileHash -LiteralPath $us).Hash
} else { 'user.settings missing' }
"--- game dx12 dir all files"
Get-ChildItem -LiteralPath 'C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3\bin\x64_dx12' -File | ForEach-Object { "{0,-40} {1,12}" -f $_.Name, $_.Length }
"--- vkd3d package on lab"
foreach ($n in 'd3d12.dll', 'd3d12core.dll', 'dxgi.dll') { $p = "C:\BC250\m12\fl-probe001\$n"; if (Test-Path $p) { "$((Get-FileHash $p).Hash) $n" } else { "missing $n" } }
"--- registered ICD"
(Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash
"--- kmd cli"
foreach ($c in 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe', 'C:\BC250\m12\candidate07152\client\bc250kmd_cli.exe', 'C:\BC250\m12\candidate07151\client\bc250kmd_cli.exe') { if (Test-Path $c) { "present $c" } }
$cli = Get-ChildItem 'C:\BC250\m12\candidate0715*\client\bc250kmd_cli.exe', 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe' -ErrorAction SilentlyContinue | Select-Object -Last 1
if ($cli) { "using $($cli.FullName)"; & $cli.FullName health read | Out-String; & $cli.FullName clock read | Out-String }
"--- processes"
Get-Process witcher3, steam, REDprelauncher -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Id) $($_.ProcessName)" }
"dwm=" + ((Get-Process dwm | Select-Object -First 1).Id)
"stop=" + ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop)
$raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String; if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { "temperature=$($Matches[1])" }

# Read-only lab state check before a bounded control window (no changes on the target).
$ErrorActionPreference = 'Continue'
"time_utc=" + (Get-Date).ToUniversalTime().ToString('o')
try { "stop_flag=" + ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) } catch { "stop_flag=unreachable" }
"--- scheduled tasks BC250-*"
Get-ScheduledTask -TaskPath '\' -ErrorAction SilentlyContinue | Where-Object { $_.TaskName -like 'BC250*' } | ForEach-Object { "$($_.TaskName) $($_.State)" }
"--- processes of interest"
Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'runtime-|control|probe|dwm|powershell|pwsh|bc250' } |
  Select-Object Id, ProcessName, StartTime | ForEach-Object { "$($_.Id) $($_.ProcessName) $($_.StartTime)" }
"--- powershell command lines"
Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='pwsh.exe'" | ForEach-Object { "$($_.ProcessId) $($_.CommandLine)" }
"--- baseline files"
foreach ($f in 'C:\BC250\m10\wsi-final\vulkan_radeon.dll', 'C:\BC250\m11\resource-close\bc250d3d.dll', 'C:\BC250\m11\resource-close\bc250d3d.m13-original.dll',
               'C:\BC250\m13\runtime-probe001\router.dll', 'C:\BC250\m13\runtime-probe001\bc250d3d_zink.dll', 'C:\BC250\m13\runtime-probe001\baseline.dll',
               'C:\BC250\m13\shared-import001\vulkan_radeon.dll', 'C:\BC250\m13\hosted-runtime016\vulkan_radeon.dll', 'C:\BC250\m13\hosted-runtime016\bc250d3d_zink.dll',
               'C:\BC250\m13\hosted-runtime016\runtime-flip-control.exe') {
  if (Test-Path -LiteralPath $f) { "$((Get-FileHash -LiteralPath $f).Hash.Substring(0,8)) $f" } else { "missing  $f" }
}
"--- registered Vulkan ICDs"
foreach ($k in 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers', 'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers') {
  if (Test-Path $k) { (Get-Item $k).Property | ForEach-Object { "$_ = $((Get-ItemProperty $k).$_)" } }
}
"--- dwm"
Get-Process dwm | ForEach-Object { "dwm pid=$($_.Id) start=$($_.StartTime)" }
"--- free space C:"
(Get-PSDrive C).Free

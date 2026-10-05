# Preserve the malformed first pass of cpucopy-002 (size list collapsed to 1920x / 1200x) before the rerun.
$ErrorActionPreference = 'Stop'
$m12 = 'C:\BC250\m12'
if (Get-Process vkcube -ErrorAction SilentlyContinue) { throw 'vkcube still running' }
if (Get-ScheduledTask -TaskName 'BC250-M12-cpucopy-002' -ErrorAction SilentlyContinue) { throw 'task still registered' }
Rename-Item -LiteralPath "$m12\cpucopy-002" -NewName 'cpucopy-002-bad-sizes'
foreach ($n in 'run-cpucopy-002.log', 'done-cpucopy-002.json') {
  if (Test-Path -LiteralPath "$m12\witcher3-dx12\$n") { Rename-Item -LiteralPath "$m12\witcher3-dx12\$n" -NewName ($n -replace 'cpucopy-002', 'cpucopy-002-bad-sizes') }
}
Get-ChildItem "$m12\cpucopy-002-bad-sizes" | Select-Object Name, Length | Format-Table -AutoSize | Out-String
"registered=" + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash.Substring(0, 8)

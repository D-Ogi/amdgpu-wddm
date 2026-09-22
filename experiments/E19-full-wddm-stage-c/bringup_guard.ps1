# M78: a bring-up run starts from a fresh boot. This says whether this boot has already seen one, by looking for
# the phase logs the bring-up itself writes (gart, psp, ih, gfx, fence, submit) with a timestamp after the boot.
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$used = @(Get-ChildItem 'C:\BC250\e16\out', 'C:\BC250\e16-umd\out' -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $boot -and $_.Name -match '^(gart|psp|ih|gfx|fence|submit)-' })
"boot     $($boot.ToString('s'))"
"bringup  $($used.Count) phase log(s) since boot" + $(if ($used.Count) { ": " + (($used | Sort-Object LastWriteTime | Select-Object -Last 4 | ForEach-Object { $_.Name }) -join ' ') } else { '' })
if ($used.Count -eq 0) { "verdict  FRESH" } else { "verdict  USED" }

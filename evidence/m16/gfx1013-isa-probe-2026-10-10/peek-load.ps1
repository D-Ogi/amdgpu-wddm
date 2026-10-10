# Read-only: what is loading unit A right now, and the health lines the probe leaves behind.
# Writes nothing, changes nothing.
$ErrorActionPreference = 'Continue'
"utc $([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))"
"uptime from $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))"
'--- top 8 processes by CPU time ---'
Get-Process | Sort-Object CPU -Descending | Select-Object -First 8 |
    ForEach-Object { "  {0,-28} pid {1,-7} cpu {2,8:N1} s  ws {3,7:N0} MB" -f $_.ProcessName, $_.Id, $_.CPU, ($_.WorkingSet64 / 1MB) }
'--- processes that name a GPU client ---'
$names = 'llama|dotprobe|hipbench|hipthreads|vkcube|Q2RTX|quake|ROTTR|witcher|vkfill|dxr|3DMark'
$hits = @(Get-Process | Where-Object { $_.ProcessName -match $names })
if ($hits.Count) { $hits | ForEach-Object { "  $($_.ProcessName) pid $($_.Id) cpu $($_.CPU)" } } else { '  none' }
'--- kernel driver ---'
$root = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name 'InstallRoot' -ErrorAction SilentlyContinue).InstallRoot
$cli = if ($root) { Join-Path $root 'tools\bc250kmd_cli.exe' } else { '' }
if ($cli -and (Test-Path -LiteralPath $cli)) {
    & $cli health read 2>&1 | Select-Object -First 1
    & $cli clock read 2>&1 | Select-Object -First 1
    & $cli dpm 2>&1 | Where-Object { $_ -match 'cap \d+ max \d+' } | Select-Object -First 1
    & $cli fan 2>&1 | Select-Object -First 2
    'TdrDelay = ' + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -Name TdrDelay -ErrorAction SilentlyContinue).TdrDelay
    'DpmMode  = ' + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' -Name DpmMode -ErrorAction SilentlyContinue).DpmMode
    'DpmMaxMHz= ' + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' -Name DpmMaxMHz -ErrorAction SilentlyContinue).DpmMaxMHz
} else { 'bc250kmd_cli.exe not found' }
'--- bugchecks and display faults since boot ---'
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$bc = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1001; StartTime = $boot } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'BugCheck' })
"  bugcheck events: $($bc.Count)"
$dsp = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 4101; StartTime = $boot } -ErrorAction SilentlyContinue)
"  Display 4101 events: $($dsp.Count)"

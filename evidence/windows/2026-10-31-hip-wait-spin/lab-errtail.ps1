param([string]$Run = 'run-new-t34', [string]$Tool = 'bench', [int]$Lines = 40)
$d = "C:\BC250\tmp\t35hip\$Run"
"--- $Tool.err (filtered tail)"
Get-Content "$d\$Tool.err" -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch '^\s*$' } | Select-Object -Last $Lines
"--- $Tool.out tail"
Get-Content "$d\$Tool.out" -ErrorAction SilentlyContinue | Select-Object -Last 8
"--- display events, last 15 min"
$since = (Get-Date).AddMinutes(-15)
Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'Display|bc250|amdgpu|dxgkrnl|LiveKernel' -or $_.Id -in 4101, 141, 117 } |
    Select-Object -First 10 | ForEach-Object { "$($_.TimeCreated.ToUniversalTime().ToString('HH:mm:ss'))Z $($_.ProviderName) $($_.Id) $($_.Message -replace '\s+', ' ' | Select-Object -First 1)" }
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
& $cli clock read 2>&1 | Select-Object -First 1
Get-CimInstance Win32_PnPEntity -Filter "DeviceID LIKE 'PCI\\VEN_1002&DEV_13FE%'" | ForEach-Object { "problem $($_.ConfigManagerErrorCode)" }
Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { "dwm pid $($_.Id) start $($_.StartTime.ToUniversalTime().ToString('HH:mm:ss'))Z" }

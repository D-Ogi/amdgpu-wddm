# Read-only lab state before and after each session of the ring VM flush game arm.
$ErrorActionPreference = 'Continue'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction SilentlyContinue).InstallRoot
$cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
"utc $([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')) boot $($boot.ToString('yyyy-MM-ddTHH:mm:ssZ')) uptime $([Math]::Round(([DateTime]::UtcNow-$boot).TotalMinutes,1)) min"
$rel = Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -ErrorAction SilentlyContinue
"release $($rel.Version) root $inst"
$lock = 'C:\BC250\tmp\LAB-LOCK.txt'
if (Test-Path -LiteralPath $lock) { "lock: $(@(Get-Content -LiteralPath $lock | Where-Object { $_.Trim() }) -join ' | ')" } else { 'lock: absent' }
"interactive user present: $([bool](Get-CimInstance Win32_ComputerSystem).UserName)"
$names = @('witcher3', 'hipbench', 'hipthreads', 'llama-bench', 'llama-completion', 'llama-cli', 'llama-server', 'ollama',
           'vadd', 'gemm_check', 'vkcube', 'vkcubepp', 'q2rtx', 'quake2rtx', 'ROTTR', 'DXRPathTracer', '3DMark', 'python')
$hit = @()
foreach ($n in $names) { foreach ($p in @(Get-Process -Name $n -ErrorAction SilentlyContinue)) { $hit += "$($p.ProcessName) pid $($p.Id) session $($p.SessionId)" } }
if ($hit.Count) { "GPU CLIENTS RUNNING: $($hit -join '; ')" } else { 'gpu clients: none running' }
"steam running: $([bool](Get-Process -Name steam -ErrorAction SilentlyContinue))  bc250mon running: $([bool](Get-Process -Name bc250mon -ErrorAction SilentlyContinue))"
if (Test-Path -LiteralPath $cli) {
    'health ' + ((& $cli health read 2>&1) -join ' ').Trim()
    $c = (& $cli clock read 2>&1) -join ' '
    if ($c -match 'temperature_mc=(\d+)') { "tctl $([double]$Matches[1] / 1000) C  $(($c -replace '\s+', ' ').Trim())" } else { "tctl UNREADABLE $c" }
    'fan ' + ((& $cli fan 2>&1) -join ' ').Trim()
} else { "cli ABSENT at $cli" }
$par = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' -ErrorAction SilentlyContinue
$pn = @($par.PSObject.Properties.Name)
foreach ($n in @('EnableRingVmFlush', 'EnableVmidPool', 'HangRecoveryMode', 'DpmMode', 'DpmMaxMHz', 'DpmIdleMHz', 'CuMode', 'UnconfirmedStarts', 'KeepLog')) {
    if ($pn -contains $n) { '  {0} = {1}' -f $n, $par.$n } else { '  {0} ABSENT' -f $n }
}
$gd = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -ErrorAction SilentlyContinue
'  TdrDelay = {0}  TdrDdiDelay = {1}  TdrLevel = {2}' -f $gd.TdrDelay, $gd.TdrDdiDelay, $gd.TdrLevel
$sys = Join-Path $env:SystemRoot 'System32\drivers\bc250kmd.sys'
$drv = Get-CimInstance Win32_SystemDriver -Filter "Name='bc250kmd'" -ErrorAction SilentlyContinue
if ($drv) { "kmd service $($drv.State) path $($drv.PathName)" }
$img = Get-ChildItem 'C:\Windows\System32\DriverStore\FileRepository' -Filter 'bc250kmd.sys' -Recurse -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 3
foreach ($f in $img) { "driverstore $((Get-FileHash -LiteralPath $f.FullName).Hash.Substring(0,8)) $($f.Directory.Name)" }
$ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Display'; StartTime = $boot } -ErrorAction SilentlyContinue)
'Display provider events since the boot: {0} (4101: {1})' -f $ev.Count, @($ev | Where-Object { $_.Id -eq 4101 }).Count
$bc = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 41, 1001; StartTime = $boot.AddHours(-6) } -ErrorAction SilentlyContinue)
'bugcheck/unexpected-shutdown events in the last 6 h before this boot and since: {0}' -f $bc.Count

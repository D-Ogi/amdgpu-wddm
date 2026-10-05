# KMD binding state after an interrupted promotion: device driver version/problem, published bc250kmd INFs with
# versions and hashes, Parameters (start bookkeeping), class-key graphics registration, dump files and the BugCheck
# event. Read-only.
$gpu = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -match 'VEN_1002&DEV_13FE' } | Select-Object -First 1
if (!$gpu) { $gpu = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VEN_1002&DEV_13FE' } | Select-Object -First 1 }
"device $($gpu.InstanceId) status $($gpu.Status) problem $($gpu.Problem)"
foreach ($k in 'DEVPKEY_Device_DriverVersion','DEVPKEY_Device_DriverInfPath','DEVPKEY_Device_Driver','DEVPKEY_Device_ProblemCode','DEVPKEY_Device_ProblemStatus') {
  try { "  $k = $((Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName $k -ErrorAction Stop).Data)" } catch { "  $k : $($_.Exception.Message)" } }
"== published bc250kmd packages"
Get-ChildItem C:\Windows\INF\oem*.inf | Where-Object { Select-String -LiteralPath $_.FullName -Pattern 'bc250kmd' -Quiet } | ForEach-Object {
  $v = (Select-String -LiteralPath $_.FullName -Pattern '^DriverVer').Line
  '{0} {1} {2}' -f $_.Name, (Get-FileHash $_.FullName).Hash.Substring(0,8), $v }
"== service image"
$svc = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd' -ErrorAction SilentlyContinue
"ImagePath $($svc.ImagePath)"
if ($svc.ImagePath) { $p = $svc.ImagePath -replace '^\\SystemRoot', $env:windir -replace '^System32', "$env:windir\System32"
  if (Test-Path $p) { "sys $(( Get-FileHash $p).Hash.Substring(0,8)) $p" } }
"== Parameters"
$par = Get-Item 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' -ErrorAction SilentlyContinue
if ($par) { foreach ($n in $par.GetValueNames()) { $val = $par.GetValue($n); if ($val -is [array]) { $val = ($val -join ',') }; "  $n = $val" } }
"== class key registration"
try { $drv = (Get-PnpDeviceProperty -InstanceId $gpu.InstanceId -KeyName DEVPKEY_Device_Driver).Data
  $ck = Get-Item "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$drv"
  foreach ($n in 'UserModeDriverName','UserModeDriverNameWow','VulkanDriverName','VulkanDriverNameWow','DriverVersion','InfPath') {
    $val = $ck.GetValue($n); if ($val -is [array]) { $val = ($val -join ' | ') }; "  $n = $val" } } catch { "class key: $($_.Exception.Message)" }
"== dumps"
Get-ChildItem C:\Windows\MEMORY.DMP, C:\Windows\Minidump -ErrorAction SilentlyContinue | ForEach-Object {
  if ($_.PSIsContainer) { Get-ChildItem $_.FullName | Sort-Object LastWriteTime | Select-Object -Last 3 | ForEach-Object { '{0} {1} {2}' -f $_.LastWriteTimeUtc.ToString('o'), $_.Length, $_.FullName } }
  else { '{0} {1} {2}' -f $_.LastWriteTimeUtc.ToString('o'), $_.Length, $_.FullName } }
"== BugCheck events (24 h)"
Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1001; StartTime = (Get-Date).AddHours(-24) } -ErrorAction SilentlyContinue |
  Select-Object -First 3 | ForEach-Object { '{0} {1}' -f $_.TimeCreated.ToUniversalTime().ToString('o'), (([string]$_.Message) -replace '\s+', ' ') }
"== scheduled tasks"
Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object { $_.TaskName -match 'BC250|Heartbeat' } | ForEach-Object { "  $($_.TaskName) $($_.State)" }

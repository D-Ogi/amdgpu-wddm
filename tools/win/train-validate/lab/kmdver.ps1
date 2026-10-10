# Read-only identity read of the installed release: the kernel driver version the running driver reports,
# the kernel image in the driver store, the Vulkan ICD the loader takes, the DWM modules, the start-confirm
# task and the release defaults a train is asked about. Generic: it names no train, and -Names replaces the
# list of registry values to read when a train moves another one.
param([string[]]$Names = @('FanLoadBoost', 'ReportAmdDriverVersion', 'EnableRingVmFlush', 'EnableDpAudioContainerId',
                           'EnableDpAudio', 'EnableDpAudioEndpoint', 'EnableDpAudioStream', 'EnableDisplayModes',
                           'CuMode', 'DpmMode', 'DpmMaxMHz', 'DpmIdleMHz', 'HangRecoveryMode', 'KeepLog',
                           'SubmitWatchdogMs'))
$ErrorActionPreference = 'Continue'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
"install root $inst"
'--- driver version the running KMD reports'
(& $cli version 2>&1) | ForEach-Object { '  ' + $_ }
'--- kernel image in the driver store'
$store = Get-ChildItem 'C:\Windows\System32\DriverStore\FileRepository' -Directory -Filter 'bc250kmd.inf_*' -ErrorAction SilentlyContinue
foreach ($d in $store) {
    foreach ($f in @('bc250kmd.sys', 'bc250kmd.inf')) {
        $p = Join-Path $d.FullName $f
        if (Test-Path -LiteralPath $p) {
            '  {0} {1} {2} bytes  {3}' -f $f, (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.Substring(0, 8), (Get-Item -LiteralPath $p).Length, $d.Name
        }
    }
    $inf = Join-Path $d.FullName 'bc250kmd.inf'
    if (Test-Path -LiteralPath $inf) { (Get-Content -LiteralPath $inf | Select-String '^DriverVer') | ForEach-Object { '  ' + $_.Line.Trim() } }
}
'--- release defaults this train is asked about'
$p = Get-ItemProperty $par -ErrorAction SilentlyContinue
$names = @($p.PSObject.Properties.Name)
foreach ($n in $Names) {
    if ($names -contains $n) { '  {0} = {1}' -f $n, $p.$n } else { '  {0} ABSENT' -f $n }
}
'  Parameters values in all: {0}' -f @($names | Where-Object { $_ -notlike 'PS*' }).Count
'--- the Vulkan ICD the loader takes (registered entry)'
$vi = & (Join-Path $inst 'tools\vulkaninfo.exe') --summary 2>&1
($vi | Select-String 'driverName|driverInfo|apiVersion|deviceName|driverVersion|GPU id|conformanceVersion') | ForEach-Object { '  ' + $_.Line.Trim() }
'--- DWM modules of ours'
$dwm = Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1
if ($dwm) {
    '  dwm pid {0} started {1}' -f $dwm.Id, $dwm.StartTime.ToUniversalTime().ToString('o')
    $dwm.Modules | Where-Object { $_.ModuleName -match 'bc250|amdgpu' } | ForEach-Object {
        '  {0} {1} {2} bytes' -f $_.ModuleName, (Get-FileHash -LiteralPath $_.FileName -Algorithm SHA256).Hash.Substring(0, 8), (Get-Item -LiteralPath $_.FileName).Length }
}
'--- start-confirm task'
Get-ScheduledTask -TaskName 'amdgpu-wddm start confirm' -ErrorAction SilentlyContinue |
  ForEach-Object { '  state {0}, last result {1}, last run {2}' -f $_.State, (Get-ScheduledTaskInfo $_).LastTaskResult, (Get-ScheduledTaskInfo $_).LastRunTime }
'--- installed release registry block'
Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' | Select-Object Version, InstalledUtc, InstallRoot | Format-List

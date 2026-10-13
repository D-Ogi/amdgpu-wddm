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
$stateReader = Join-Path $inst 'tools\state-reader.ps1'
if (-not (Test-Path -LiteralPath $stateReader)) { throw 'This train validator requires the schema-2 state reader' }
. $stateReader
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
$p = Read-DriverStateValues -Cli $cli
$availableNames = @($p.Keys)
foreach ($n in $Names) {
    if ($availableNames -contains $n) { '  {0} = {1}' -f $n, $p.$n } else { '  {0} ABSENT' -f $n }
}
'  Effective driver state values in all: {0}' -f $availableNames.Count
# What the running driver made of the gates it reads once at GfxStart: a registry value says what was asked, these
# lines say what the driver did. The kmdver arm of arms.json expects the VM flush line of the release default.
'--- KMD start lines of the GfxStart gates'
(& $cli log 2>&1 | Select-String 'gfx: VMID pool (on|off)|gfx: VM flush ') | Select-Object -First 2 |
  ForEach-Object { '  ' + ($_.Line -replace '^\s*\d+\s+[0-9.]+\s+', '') }
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

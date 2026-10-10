# Read-only health gate after one arm of a train validation, on unit A (Windows PowerShell 5.1, SSH session).
# Generic: it names no train and no package, so one copy serves every train.
#
#   gate.ps1 [-Label <arm id>] [-Lines <n>]
#
# One fact per line, each line a key the host parses (tools/win/train-validate/runner.py). Everything is
# guarded: a section that cannot be read says so and the gate still prints the rest, because a gate that
# throws tells the operator nothing. The gate reads; it never writes and never repairs.
param([string]$Label = '', [int]$Lines = 8)
$ErrorActionPreference = 'Continue'
function Try-Line([string]$key, [scriptblock]$body) {
    try { $v = & $body; if ($null -eq $v) { $v = '' }; "$key $v" } catch { "$key UNREADABLE $($_.Exception.Message)" }
}
$boot = $null
$inst = ''
$cli = ''
"label $Label utc $([DateTime]::UtcNow.ToString('o'))"
Try-Line 'boot' { $script:boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
                  '{0:o} uptime_s {1}' -f $script:boot.ToUniversalTime(), [Math]::Round(([DateTime]::Now - $script:boot).TotalSeconds, 1) }
Try-Line 'installroot' { $script:inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
                         $script:cli = Join-Path $script:inst 'tools\bc250kmd_cli.exe'; $script:inst }
Try-Line 'health:' { (& $script:cli health read 2>&1) -join ' ' }
$log = @()
Try-Line 'log' { $script:log = @(& $script:cli log 2>&1); '{0} lines' -f $script:log.Count }
Try-Line 'counters:' {
    'faults={0} fence_timeouts={1} reset_engine={2} hang_recovery={3}' -f
        @($script:log | Select-String 'GPU FAULT').Count,
        @($script:log | Select-String 'HARDWARE FENCE TIMEOUT').Count,
        @($script:log | Select-String 'ResetEngine node').Count,
        @($script:log | Select-String 'hang recovery|HangRecovery').Count }
@($log | Select-String 'hang|recover|reset|FAULT|TIMEOUT') | Select-Object -Last $Lines |
    ForEach-Object { 'logline ' + $_.Line.Trim().Substring(0, [Math]::Min(200, $_.Line.Trim().Length)) }
Try-Line 'display:' {
    # Get-WinEvent takes StartTime in local time, so the boot time goes in as local (tdr-check.ps1 of b27).
    $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Display'; StartTime = $script:boot } -ErrorAction SilentlyContinue)
    'events={0} id4101={1}' -f $ev.Count, @($ev | Where-Object { $_.Id -eq 4101 }).Count }
Try-Line 'bugchecks:' {
    @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1001;
        ProviderName = 'Microsoft-Windows-WER-SystemErrorReporting'; StartTime = $script:boot } -ErrorAction SilentlyContinue).Count }
Try-Line 'appcrashes:' {
    @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $script:boot } -ErrorAction SilentlyContinue).Count }
Try-Line 'dpm:' { (((& $script:cli dpm read 2>&1) -join ' | ') -split '\|' | Select-Object -Last 1).Trim() }
Try-Line 'fan:' { ((& $script:cli fan 2>&1) -join ' ').Trim() }
Try-Line 'tctl:' { $t = (& $script:cli clock read 2>&1) -join ' '
                   if ($t -match 'temperature_mc=(\d+)') { '{0}' -f ([double]$Matches[1] / 1000) } else { $t } }
Try-Line 'tdr:' { $gd = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -ErrorAction SilentlyContinue
                  'TdrDelay={0} TdrDdiDelay={1} TdrLevel={2}' -f $gd.TdrDelay, $gd.TdrDdiDelay, $gd.TdrLevel }
Try-Line 'adapter:' { $d = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                          Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1
                      'status={0} name={1}' -f $d.Status, $d.FriendlyName }
'gate end'

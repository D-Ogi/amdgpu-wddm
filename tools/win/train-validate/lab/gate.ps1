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
# One log line per stage-1 attempt of DxgkDdiResetEngine (driver/kmd/wddm.c): the SOFT RECOVERED line of a
# recovery and the verdict line of a refusal. The word alone is not an attempt: every boot writes
# 'wddm: HangRecoveryMode 1: a node-0 ResetEngine tries stage-1 soft recovery, verdicts in
# Parameters\HangRecovery'. A count of the lines that hold the switch's name therefore reported one attempt on
# a machine that had never hung, and reported one again when two real attempts had happened: an attempt's own
# lines say "soft recovery", not "hang recovery". The pattern below is kept on a line of its own because the
# host test reads it out of this file (test_train_validate.py).
$HangRecoveryLine = 'ResetEngine node \d+: (SOFT RECOVERED|soft recovery verdict)'
Try-Line 'counters:' {
    'faults={0} fence_timeouts={1} reset_engine={2} hang_recovery={3}' -f
        @($script:log | Select-String 'GPU FAULT').Count,
        @($script:log | Select-String 'HARDWARE FENCE TIMEOUT').Count,
        @($script:log | Select-String 'ResetEngine node').Count,
        @($script:log | Select-String $script:HangRecoveryLine).Count }
# The driver's own counters, flushed to a non-volatile subkey at every verdict (guard.c
# GuardRecordHangRecovery), so they outlive the log ring and the 0x116 that a refusal leads to. Cumulative over
# the life of the install and never reset here: a total, not "since this boot".
Try-Line 'hangrecord:' {
    $r = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters\HangRecovery' -ErrorAction SilentlyContinue
    if ($null -eq $r) { 'absent' }
    else { 'attempts={0} recovered={1} not_drained={2} refused={3} last_verdict={4}' -f
             [int]$r.Attempts, [int]$r.Recovered, [int]$r.NotDrained, [int]$r.Refused, [int]$r.LastVerdict } }
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

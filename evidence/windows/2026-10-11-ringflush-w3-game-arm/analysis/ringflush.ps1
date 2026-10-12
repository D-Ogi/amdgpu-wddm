# Ring VM flush arm of the b26 validation: section 3 of docs/design/gfx-submit-root-serialization.md.
# Copied from scratch\train\b26\validation\ps\ringflush.ps1 for the 2026-10-11 game arm (scratch\ringflush-game);
# 'read' also prints the start health and the node-0 and paging summary lines.
#
# The switch is EnableRingVmFlush under Services\bc250kmd\Parameters, absent in this release (gate 0). The
# driver reads it once in GfxStart, so every change needs a restart; this script only sets or removes the value
# and reads what the running driver says. The restarts are separate calls (restart-now.ps1), as the plan asks.
#
#   -Step check     step 1 of the plan: TdrDelay, HangRecoveryMode, the hang detector, and the value itself
#   -Step set1      write EnableRingVmFlush = 1 (restart afterwards)
#   -Step read      the start line and the summary counters of the running driver
#   -Step clear     remove the value (restart afterwards), which is the rollback of the plan
param([ValidateSet('check', 'set1', 'read', 'clear')][string]$Step = 'read')
$ErrorActionPreference = 'Continue'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$cli = Join-Path $inst 'tools\bc250kmd_cli.exe'
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
"utc $([DateTime]::UtcNow.ToString('o')) boot $($boot.ToString('o')) uptime $([Math]::Round(([DateTime]::UtcNow-$boot).TotalSeconds,1)) s"

function Show-Value {
    $p = Get-ItemProperty $par -ErrorAction SilentlyContinue
    $names = @($p.PSObject.Properties.Name)
    foreach ($n in @('EnableRingVmFlush', 'EnableVmidPool', 'HangRecoveryMode')) {
        if ($names -contains $n) { '  {0} = {1}' -f $n, $p.$n } else { '  {0} ABSENT' -f $n }
    }
    $gd = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -ErrorAction SilentlyContinue
    '  TdrDelay = {0}' -f $gd.TdrDelay
}

switch ($Step) {
    'check' {
        '--- step 1 of the plan: the recovery path before the first arm'
        Show-Value
        '  HangRecovery subkey: ' + [bool](Test-Path "$par\HangRecovery")
        'health: ' + (& $cli health read 2>&1)
        $log = & $cli log 2>&1
        ($log | Select-String 'HangRecoveryMode|hang recovery') | Select-Object -First 3 | ForEach-Object { '  ' + $_.Line.Trim() }
        '  GPU faults {0}, fence timeouts {1}, ResetEngine {2}' -f @($log | Select-String 'GPU FAULT').Count,
            @($log | Select-String 'HARDWARE FENCE TIMEOUT').Count, @($log | Select-String 'ResetEngine node').Count
    }
    'set1' {
        New-ItemProperty -Path $par -Name 'EnableRingVmFlush' -Value 1 -PropertyType DWord -Force | Out-Null
        'EnableRingVmFlush written'
        Show-Value
    }
    'clear' {
        Remove-ItemProperty -Path $par -Name 'EnableRingVmFlush' -ErrorAction SilentlyContinue
        'EnableRingVmFlush removed'
        Show-Value
    }
    'read' {
        Show-Value
        'health: ' + ((& $cli health read 2>&1) -join ' ').Trim()
        '--- the start line the gate writes'
        $log = & $cli log 2>&1
        ($log | Select-String 'VM flush on the ring|VMID pool (on|off)|EnableRingVmFlush|EnableVmidPool') |
            Select-Object -First 6 | ForEach-Object { '  ' + $_.Line.Trim() }
        '--- the summary counters the plan reads'
        $sum = & $cli log summary 2>&1
        ($sum | Select-String 'ring VM flush|VMID pool|node 0 hardware|node 1 \(paging|no TDR|timeout') |
            Select-Object -Last 12 | ForEach-Object { '  ' + $_.Line.Trim() }
        '--- an MMIO flush must not have run (this line must be absent)'
        $mmio = @($log | Select-String 'VMID \d+ root 0x[0-9A-Fa-f]+ flush ->')
        '  gfx: VMID ... root ... flush -> lines: {0}' -f $mmio.Count
        $mmio | Select-Object -First 3 | ForEach-Object { '  ' + $_.Line.Trim() }
        '--- faults and timeouts'
        '  GPU faults {0}, fence timeouts {1}, ResetEngine {2}, wddm timeout {3}' -f @($log | Select-String 'GPU FAULT').Count,
            @($log | Select-String 'HARDWARE FENCE TIMEOUT').Count, @($log | Select-String 'ResetEngine node').Count,
            @($log | Select-String 'wddm: timeout').Count
        $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Display'; StartTime = $boot } -ErrorAction SilentlyContinue)
        '  Display provider events since the boot: {0} (4101: {1})' -f $ev.Count, @($ev | Where-Object { $_.Id -eq 4101 }).Count
    }
}

# BD-124: the stop record of the kernel driver, on hardware. The host suites prove the layout and the readers
# (tools\release\test-start-record.ps1, test-diagnostics.ps1, driver\kmd\test\run_stop_record.ps1); this arm proves
# the one thing they cannot: that the driver really writes the record when Windows stops a start that nothing
# confirmed, and really writes none when it stops a start that a healthy desktop did confirm.
#
# Three readings, in this order, in one session and with no restart:
#   1. with a CONFIRMED start, stop the device (pnputil /disable-device). No new record may appear.
#   2. start it again and stop it about -Gap seconds later, before the start-confirm task confirms the new
#      generation. A fresh record must appear, with Result=stopped, Stage=79 and this boot's BootId.
#   3. read the same record through the shipped reader (bc250kmd_cli), so that what a tester would see is in the
#      evidence next to the raw values.
# The device is enabled again on every exit path, including a failure or a stop of this script, and the arm waits
# for the start health to reach 15 (full, ready, visible and confirmed) before it reports. Read-only apart from the
# disable and the enable of the adapter, which the operator asks for by running this arm.
param([int]$Gap = 10, [int]$ConfirmWaitS = 60, [string]$Hardware = 'PCI\VEN_1002&DEV_13FE')
$ErrorActionPreference = 'Continue'
$prefetch = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction SilentlyContinue).InstallRoot
$cli = if ($inst) { Join-Path $inst 'tools\bc250kmd_cli.exe' } else { $null }
$stateReader = Join-Path $inst 'tools\state-reader.ps1'
if (-not (Test-Path -LiteralPath $stateReader)) { throw 'This train validator requires the schema-2 state reader' }
. $stateReader
$problems = New-Object System.Collections.Generic.List[string]

# This boot's BootId, from the place the driver takes it: KUSER_SHARED_DATA.BootId at 0x7FFE0000 + 0x2C4 (WDK
# 10.0.26100 km\ntddk.h, structure at line 8965, BootId at line 9117, bracketed by the C_ASSERTs of lines 9568 and
# 9569: AlternativeArchitecture 0x2C0, SystemExpirationDate 0x2C8). The PrefetchParameters copy of the counter lags
# a boot: on 22631, 13 minutes in, the page said 388 and the registry 387, so this arm called the fresh record of
# this boot another boot's (BD-130). The registry value stays as the fallback, and the arm prints the source.
$script:BootIdSource = ''
function BootIdNow {
    try {
        $shared = [int64]([int64][Runtime.InteropServices.Marshal]::ReadInt32([IntPtr]0x7FFE02C4) -band [int64]4294967295)
        if ($shared -ne 0) { $script:BootIdSource = 'KUSER_SHARED_DATA'; return [int]$shared }
    } catch { }
    $script:BootIdSource = 'the PrefetchParameters registry value, which can lag this boot by one'
    return [int](Get-ItemProperty -LiteralPath $prefetch -Name BootId -ErrorAction SilentlyContinue).BootId
}
function Record {
    $p = Read-DriverStateValues -Cli $cli -Path 'StopRecord'
    if (-not $p.Count) { return $null }
    $o = [ordered]@{}
    foreach ($n in @('Result', 'Step', 'SubStep', 'Status', 'Stage', 'StartStage', 'HealthFlags', 'BootId',
                     'Generation', 'Epoch', 'Version', 'TimeUtc', 'StopTimeUtc', 'LogLines', 'Detail')) {
        if ($p.ContainsKey($n)) { $o[$n] = $p.$n }
    }
    return [pscustomobject]$o
}
function Show($label, $r) {
    if ($null -eq $r) { "  $label : no record at all"; return }
    foreach ($n in $r.PSObject.Properties.Name) { '  {0} {1} = {2}' -f $label, $n, $r.$n }
}
function HealthFlags {
    if (-not $cli -or -not (Test-Path -LiteralPath $cli)) { return $null }
    $line = (& $cli health read 2>&1 | Select-String 'flags=(\d+)')
    if (-not $line) { return $null }
    return [int]([regex]::Match($line.Line, 'flags=(\d+)').Groups[1].Value)
}
function WaitConfirmed($seconds) {
    $end = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $end) {
        if ((HealthFlags) -eq 15) { return $true }
        Start-Sleep -Seconds 3
    }
    return ((HealthFlags) -eq 15)
}

$dev = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like ($Hardware + '*') } | Select-Object -First 1
if (-not $dev) {
    'stop record arm: REFUSED, no display device of ' + $Hardware
    exit 2
}
"device {0}" -f $dev.InstanceId
$boot = BootIdNow
"boot id {0} (from {1})" -f $boot, $script:BootIdSource
try {
    $before = Record
    Show 'before' $before
    $flags = HealthFlags
    "start health flags before the first stop: {0}" -f $(if ($null -eq $flags) { 'unreadable' } else { $flags })

    '--- 1. a stop of a CONFIRMED start must write no record'
    if ($flags -ne 15) {
        $problems.Add('reading 1 skipped: the start was not confirmed (flags ' + $flags + '), so this stop proves nothing')
    } else {
        & pnputil /disable-device $dev.InstanceId | ForEach-Object { '  ' + $_ }
        Start-Sleep -Seconds 5
        $after = Record
        Show 'after-confirmed-stop' $after
        $isNew = $false
        if ($null -ne $after) {
            if ($null -eq $before) { $isNew = $true }
            elseif ([string]$after.TimeUtc -ne [string]$before.TimeUtc) { $isNew = $true }
        }
        if ($isNew) { $problems.Add('reading 1 FAILED: a confirmed stop wrote a record (' + [string]$after.TimeUtc + ')') }
        else { '  ok: no new record, the older one (if any) stands' }
    }

    '--- 2. a stop of an UNCONFIRMED start must write a fresh record'
    & pnputil /enable-device $dev.InstanceId | ForEach-Object { '  ' + $_ }
    Start-Sleep -Seconds $Gap
    "  start health flags {0} after {1} s, before the stop" -f $(HealthFlags), $Gap
    & pnputil /disable-device $dev.InstanceId | ForEach-Object { '  ' + $_ }
    Start-Sleep -Seconds 5
    $stop = Record
    Show 'unconfirmed-stop' $stop
    if ($null -eq $stop) {
        $problems.Add('reading 2 FAILED: no stop record after a stop of an unconfirmed start')
    } else {
        if ([string]$stop.Result -ne 'stopped') { $problems.Add('reading 2: Result is ' + [string]$stop.Result + ', not stopped') }
        if ([int]$stop.Stage -ne 79) { $problems.Add('reading 2: Stage is ' + [string]$stop.Stage + ', not 79') }
        if ([int]$stop.BootId -ne $boot) { $problems.Add('reading 2: BootId ' + [string]$stop.BootId + ' is not this boot ' + $boot + ' (read from ' + $script:BootIdSource + ')') }
        if ([int]$stop.LogLines -lt 1 -or [int]$stop.LogLines -gt 64) { $problems.Add('reading 2: LogLines ' + [string]$stop.LogLines + ' is outside 1..64') }
        if (-not $stop.StopTimeUtc) { $problems.Add('reading 2: no StopTimeUtc') }
        if ([string]$stop.SubStep -ne 'unconfirmed start') { $problems.Add('reading 2: SubStep is ' + [string]$stop.SubStep) }
        $kept = @((Get-ItemProperty -LiteralPath $par -Name Log -ErrorAction SilentlyContinue).Log)
        "  kept log lines: {0}" -f $kept.Count
        $kept | ForEach-Object { '  stop log ' + $_ }
        if ($kept.Count -ne [int]$stop.LogLines) { $problems.Add('reading 2: LogLines ' + [string]$stop.LogLines + ' against ' + $kept.Count + ' kept lines') }
    }

    '--- 3. the same record through the shipped reader'
    & pnputil /enable-device $dev.InstanceId | ForEach-Object { '  ' + $_ }
    # The start-confirm task runs on its own schedule, so a confirmation that lags is a note, not a defect of the
    # record. A device that does not come back at all is caught by the final status in the restore block below.
    if (-not (WaitConfirmed $ConfirmWaitS)) {
        '  NOTE the start health did not reach 15 within {0} s after the last enable; the next arm reads it again' -f $ConfirmWaitS
    }
    "start health flags at the end: {0}" -f $(HealthFlags)
    if ($cli -and (Test-Path -LiteralPath $cli)) {
        (& $cli stages 2>&1) | ForEach-Object { '  ' + $_ }
    } else { $problems.Add('bc250kmd_cli is not installed, so the shipped reader could not be run') }
}
finally {
    # Whatever happened above, the adapter must not be left disabled.
    $state = (Get-PnpDevice -InstanceId $dev.InstanceId -ErrorAction SilentlyContinue).Status
    if ($state -ne 'OK') {
        '  restore: enabling the adapter again'
        & pnputil /enable-device $dev.InstanceId | ForEach-Object { '  ' + $_ }
        Start-Sleep -Seconds 5
    }
    $final = (Get-PnpDevice -InstanceId $dev.InstanceId -ErrorAction SilentlyContinue).Status
    '  final device status {0}' -f $final
    if ($final -ne 'OK') { $problems.Add('the adapter did not come back: final status ' + [string]$final) }
}
if ($problems.Count -eq 0) {
    'stop record arm: ok, 3 readings'
    exit 0
}
$problems | ForEach-Object { '  problem: ' + $_ }
'stop record arm: {0} problem(s)' -f $problems.Count
exit 1

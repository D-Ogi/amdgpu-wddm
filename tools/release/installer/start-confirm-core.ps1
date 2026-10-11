# Decision rules of the logon start-confirm task (start-confirm.ps1), pure functions without I/O, host-tested by
# tests\test-start-confirm.ps1. The task confirms the boot's own KMD start once the KMD's confirmation milestone is
# met (start_health.c StartHealthRequest: flags FULL|READY|VISIBLE, completions, ready >= 60 s, last completion
# fresh), the same milestone the overlay's auto-confirmation and route.py's dwm-route.ps1 use. It never confirms
# another adapter start (a generation change ends it), never while a kit run presents its own heartbeat (that run
# confirms), and never while a transition task runs.
#
# Exit codes (also the scheduled task's LastTaskResult):
#   0 confirmed (health flags 15 seen, by this task or another client), 1 bound reached unconfirmed,
#   2 left to a kit (a transition task ran, or the bound was reached while the kit heartbeat ran),
#   3 the adapter restarted (generation changed), 4 owner STOP, 5 no full-WDDM start-health reading.

$script:StartConfirmMinReadyMs = 60000   # BC250_START_HEALTH_MIN_MS
$script:StartConfirmFreshMs = 5000       # stricter than BC250_START_HEALTH_FRESH_MS (15000): confirm on a live witness
$script:StartConfirmRetrySeconds = 4     # between two confirm calls of the same task
$script:StartConfirmAllowedTasks = @('BC250 monitor overlay','BC250 net watchdog')
$script:StartConfirmKitHeartbeat = 'Lab-Present-Heartbeat'
# The kits' competing-task pattern (preflight.ps1 / postflight.ps1 of the transition trees).
$script:StartConfirmTransitionPattern = 'BC250|DWM|G0|WSI'

# `bc250kmd_cli health read`: "health abi=1 version=0x<8> flags=<n> generation=<n> epoch=<n> completed=<n> age_ms=<n>
# ready_ms=<n>" (verify-cpu.ps1 Get-KmdReadyHealth reads the same line for one ABI; this one takes any flags).
function ConvertFrom-StartConfirmCliHealth {
 param([AllowEmptyString()][AllowNull()][string]$Text)
 if (!$Text) { return $null }
 $m = [regex]::Matches($Text,'(?m)^health abi=1 version=(0x[0-9A-Fa-f]{8}) flags=([0-9]+) generation=([0-9]+) epoch=([0-9]+) completed=([0-9]+) age_ms=([0-9]+) ready_ms=([0-9]+)\r?$')
 if ($m.Count -ne 1) { return $null }
 $g = $m[0].Groups
 return [ordered]@{version=$g[1].Value; flags=[int]$g[2].Value; generation=[uint64]$g[3].Value; epoch=[uint64]$g[4].Value;
  completed=[uint64]$g[5].Value; age_ms=[uint64]$g[6].Value; ready_ms=[uint64]$g[7].Value; source='cli'}
}

# The C# interop for the overlay's bc250control.dll: StartHealthSnapshot of bc250mon Driver.cs (BC250_ESCAPE_START_HEALTH,
# 96 bytes) and Bc250StartHealth(op, expected generation, expected epoch, out data, bytes). DllImport takes the full
# path, so the task loads its own installed copy and nothing from the search path.
function Get-StartConfirmInteropSource {
 param([Parameter(Mandatory)][ValidatePattern('^[A-Za-z]:\\[^"]+\.dll$')][string]$DllPath)
 return @"
using System;
using System.Runtime.InteropServices;
namespace Bc250StartConfirm {
 [StructLayout(LayoutKind.Sequential)]
 public struct Health {
  public uint Magic, Command, Status, Version;
  public uint NtStatus, AbiVersion, Op, Flags;
  public ulong Generation, Epoch, Completed, LastCompletionAgeMs, ReadyAgeMs;
  public ulong ExpectedGeneration, ExpectedEpoch;
  public uint Reserved0, Reserved1;
 }
 public static class Kmd {
  [DllImport(@"$DllPath", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
  public static extern int Bc250StartHealth(uint op, ulong expectedGeneration, ulong expectedEpoch, out Health data, uint bytes);
 }
}
"@
}

# The running scheduled tasks (names) -> what the task must respect.
function Get-StartConfirmKitState {
 param([AllowEmptyCollection()][string[]]$RunningTasks)
 $names = @($RunningTasks | Where-Object { $_ })
 $transition = @($names | Where-Object { $_ -match $script:StartConfirmTransitionPattern -and $_ -notin $script:StartConfirmAllowedTasks })
 return [ordered]@{transition=($transition -join ','); heartbeat=($names -contains $script:StartConfirmKitHeartbeat)}
}

function New-StartConfirmState {
 param([Parameter(Mandatory)][double]$Seconds)
 return [ordered]@{seconds=$Seconds; generation=$null; yielded=$false; confirmed_here=$false; last_confirm=-1000.0; confirms=0}
}

function Test-StartConfirmEligible {
 param([Parameter(Mandatory)]$Reading)
 return ((($Reading.flags -band 7) -eq 7) -and $Reading.completed -gt 0 -and
         $Reading.ready_ms -ge $script:StartConfirmMinReadyMs -and $Reading.age_ms -le $script:StartConfirmFreshMs)
}

# One poll: action wait | confirm | exit, with the exit code and a reason for the log.
#  $Reading  the start-health reading or $null ($ReadError says why)
#  $Now      seconds since the task started
#  $Kit      Get-StartConfirmKitState
function Get-StartConfirmStep {
 param([Parameter(Mandatory)]$State,$Reading,[Parameter(Mandatory)][double]$Now,[bool]$Stop,$Kit,[string]$ReadError)
 # A confirmed start of the generation first seen ends it before anything else: there is nothing left to do.
 if ($null -ne $Reading -and ($Reading.flags -band 9) -eq 9 -and ($null -eq $State.generation -or $Reading.generation -eq $State.generation)) {
  $by = if ($State.confirmed_here) { 'this task' } else { 'another client' }
  return [ordered]@{action='exit'; code=0; reason="confirmed by ${by}: flags $($Reading.flags) generation $($Reading.generation) epoch $($Reading.epoch)"}
 }
 if ($Stop) { return [ordered]@{action='exit'; code=4; reason='owner STOP: left unconfirmed'} }
 if ($Kit -and $Kit.transition) { return [ordered]@{action='exit'; code=2; reason="transition task running ($($Kit.transition)): its kit confirms"} }
 if ($null -eq $Reading) {
  if ($Now -ge $State.seconds) { return [ordered]@{action='exit'; code=5; reason="no start-health reading within the bound: $ReadError"} }
  return [ordered]@{action='wait'; reason="no reading: $ReadError"}
 }
 if (($Reading.flags -band 1) -eq 0) { return [ordered]@{action='exit'; code=5; reason="not a full-WDDM start (flags $($Reading.flags))"} }
 if ($null -eq $State.generation) { $State.generation = $Reading.generation }
 elseif ($Reading.generation -ne $State.generation) {
  return [ordered]@{action='exit'; code=3; reason="adapter restarted (generation $($State.generation) -> $($Reading.generation)): left to its owner"}
 }
 if ($Now -ge $State.seconds) {
  if ($State.yielded) { return [ordered]@{action='exit'; code=2; reason='bound reached while a kit heartbeat ran: left to the kit'} }
  return [ordered]@{action='exit'; code=1; reason="bound reached unconfirmed (flags $($Reading.flags) completed $($Reading.completed) age_ms $($Reading.age_ms) ready_ms $($Reading.ready_ms))"}
 }
 if ($Kit -and $Kit.heartbeat) { $State.yielded = $true; return [ordered]@{action='wait'; reason='kit heartbeat running: that run confirms'} }
 if ((Test-StartConfirmEligible $Reading) -and ($Now - $State.last_confirm) -ge $script:StartConfirmRetrySeconds) {
  return [ordered]@{action='confirm'; generation=$Reading.generation; epoch=$Reading.epoch; reason="eligible: completed $($Reading.completed) age_ms $($Reading.age_ms) ready_ms $($Reading.ready_ms)"}
 }
 return [ordered]@{action='wait'; reason="waiting: flags $($Reading.flags) completed $($Reading.completed) age_ms $($Reading.age_ms) ready_ms $($Reading.ready_ms)"}
}

# ---- BD-122: why the KMD's last start failed (Parameters\StartRecord) --------------------------------------------
# The KMD writes the reason of every start that did not reach a started device into the registry, with no gate
# (guard.c, the section above GuardRecordStartAttempt). These are the pure functions that turn that record into
# plain words; the callers that read the registry are start-confirm.ps1 and the installer's verify step.
#
# THE READER'S RULE, which the driver's comment states as well: only `Result` says what the record is about.
#   failed / guard   the reason of the NEWEST start attempt; Step, Status and Log belong to it
#   started          the newest attempt reached a started device; the reason values mean nothing
#   attempt          the newest attempt neither failed nor finished: the machine stopped inside the start, or the
#                    write of the record itself was torn. Such a record names NO step: the driver writes it at the
#                    top of a start, before any step runs. How far that start got is in Parameters\LastStage and
#                    Parameters\StageHistory, and these words say so instead of inventing a step.
# The driver writes "attempt" and flushes it before any step of a start can fail, so a failure of an earlier boot
# can never be read as the current one. One case is outside that rule and cannot be inside it: a start refused
# above the attempt record (a device object the driver does not own) leaves the previous start's record standing.
# Therefore every reading also compares TimeUtc with this boot's own start time and says when the record is older
# than this boot. An absent record means a KMD older than 0.7.216.30.
$script:StartRecordKey = 'StartRecord'      # the subkey of the service's Parameters key
$script:StartRecordMaxLogLines = 64         # BC250_START_RECORD_LINES

# The statuses this start path can answer with, from the WDK's ntstatus.h, keyed by the eight hex digits the
# record prints. Strings, not numbers: a hex literal with the high bit set is a negative Int32 in Windows
# PowerShell 5.1 and a positive one in PowerShell 7, and a table a tester's shell reads differently is no table.
$script:StartRecordStatusNames = @{
    '00000000' = 'STATUS_SUCCESS'
    'C0000001' = 'STATUS_UNSUCCESSFUL'
    'C000000D' = 'STATUS_INVALID_PARAMETER'
    'C000000E' = 'STATUS_NO_SUCH_DEVICE'
    'C0000022' = 'STATUS_ACCESS_DENIED'
    'C000009A' = 'STATUS_INSUFFICIENT_RESOURCES'
    'C00000A3' = 'STATUS_DEVICE_NOT_READY'
    'C0000182' = 'STATUS_DEVICE_CONFIGURATION_ERROR'
    'C0000184' = 'STATUS_INVALID_DEVICE_STATE'
    'C0000185' = 'STATUS_IO_DEVICE_ERROR'
    'C00002B6' = 'STATUS_DEVICE_REMOVED'
    'C0000483' = 'STATUS_DEVICE_HARDWARE_ERROR'
    'C01E0100' = 'STATUS_GRAPHICS_NO_VIDEO_MEMORY'
}
# What a step means to somebody who does not read our sources. A step without a line here prints its name alone.
$script:StartRecordStepWords = @{
    'DxgkCbGetDeviceInformation'    = 'Windows would not describe the graphics device to the driver'
    'AcquirePostDisplayOwnership'   = 'Windows would not hand the firmware display over to the driver'
    'PostDisplayGeometry'           = 'the firmware handed over no display: no screen size and no framebuffer address'
    'DisplayMapFramebuffer'         = 'the driver could not map the firmware framebuffer'
    'SmuOwnerStart'                 = 'the driver could not take over the power and clock mailbox'
    'WddmStart'                     = 'the GPU did not finish starting'
    'DisplayPrepareInheritedTiming' = 'the driver could not read the display mode the firmware left'
    'PreviousStopUnconfirmed'       = 'the previous stop of this GPU never completed, so the driver refuses to start it again'
    'BootLoopGuard'                 = 'the driver refused to start itself after starts that no healthy desktop confirmed'
}

function Format-StartRecordStatus {
    param([AllowNull()]$Status)
    if ($null -eq $Status) { return 'no status' }
    $digits = '{0:X8}' -f ([uint32]([int64]$Status -band [int64]4294967295))
    $name = $script:StartRecordStatusNames[$digits]
    if ($name) { return "0x$digits ($name)" }
    return "0x$digits"
}

# This boot's own start time in UTC, for the one question the record cannot answer about itself: is it this boot's?
# The tick count, not Win32_OperatingSystem: this runs at every logon and inside the installer, and a CIM query
# costs seconds there. Its drift over sleep does not matter at the margin below.
function Get-StartRecordBootUtc {
    try { return [datetime]::UtcNow.AddMilliseconds(-[Environment]::TickCount64) } catch { return $null }
}

# True when the record was written before this boot, so it is not about any start of this boot. The two-minute
# margin covers a record written in the first seconds of a boot against a tick count that sleep skewed; a record
# left by an earlier boot is minutes or days old, never seconds.
function Test-StartRecordFromEarlierBoot {
    param([AllowNull()]$Record, [AllowNull()]$BootUtc)
    if ($null -eq $Record -or $null -eq $BootUtc -or -not $Record.TimeUtc) { return $false }
    try {
        $styles = [System.Globalization.DateTimeStyles]::AdjustToUniversal -bor [System.Globalization.DateTimeStyles]::AssumeUniversal
        $when = [datetime]::Parse([string]$Record.TimeUtc, [cultureinfo]::InvariantCulture, $styles)
    } catch { return $false }
    return $when -lt ([datetime]$BootUtc).AddMinutes(-2)
}

# One line a tester can paste and we can read. $Record is $null for an absent record, or a map of the registry
# values: Result, Step, SubStep, Status, Stage, UnconfirmedStarts, Version, TimeUtc, Detail, Log. $BootUtc is this
# boot's start time (Get-StartRecordBootUtc); without it the line simply says nothing about which boot it is from.
function Format-StartRecord {
    param([AllowNull()]$Record, [AllowNull()]$BootUtc)
    if ($null -eq $Record) {
        return 'no record: this driver version does not keep one (before KMD 0.7.216.30), or the key cannot be read'
    }
    $result = [string]$Record.Result
    $step = if ($Record.Step -and $Record.Step -ne '-') { [string]$Record.Step } else { $null }
    $sub = if ($Record.SubStep -and $Record.SubStep -ne '-') { [string]$Record.SubStep } else { $null }
    $where = if ($step -and $sub) { "$step / $sub" } elseif ($step) { $step } else { 'no step named' }
    $words = if ($step -and $script:StartRecordStepWords.ContainsKey($step)) { ': ' + $script:StartRecordStepWords[$step] } else { '' }
    $when = if ($Record.TimeUtc) { [string]$Record.TimeUtc } else { 'no time' }
    $lines = @($Record.Log).Count
    if ($null -eq $Record.Log) { $lines = 0 }
    # Every reading carries the same four facts, whatever the result is: the stage, the time, the boot-loop count
    # (which decides whether the next start is refused at all) and the driver that wrote the record (which tells a
    # record left by the version before an upgrade from one this driver wrote).
    $driver = if ($null -ne $Record.Version) { '0x{0:X8}' -f ([uint32]$Record.Version) } else { 'unknown' }
    $tail = "stage $($Record.Stage), at $when, UnconfirmedStarts $($Record.UnconfirmedStarts), driver $driver"
    $reason = "$tail, $lines log lines kept"
    # The measured values of the refusing step (GitHub #38: the inherited timing's registers), when it kept any.
    if ($Record.Detail) { $reason = "$reason; detail: $([string]$Record.Detail)" }
    $text = switch ($result) {
        'failed'  { "the last start FAILED in $where$words; $(Format-StartRecordStatus $Record.Status); $reason" }
        'guard'   { "the last start was REFUSED by the driver's own boot-loop guard$words; $(Format-StartRecordStatus $Record.Status); $reason" }
        'started' { "the last start reached a started device; $tail" }
        'attempt' { "the last start neither failed nor finished: the machine stopped inside it, or the record's own write was torn; the record names no step, because the driver writes it at the top of a start - how far that start got is in LastStage and StageHistory; $tail" }
        default   { "unreadable record: Result '$result'; $reason" }
    }
    if (Test-StartRecordFromEarlierBoot -Record $Record -BootUtc $BootUtc) {
        $text = "$text; NOTE this record was written before this boot started, so it is not about any start of this boot"
    }
    return $text
}

# The kept ring lines, oldest first, bounded and trimmed so a log file cannot be flooded by a damaged record.
function Get-StartRecordLogLines {
    param([AllowNull()]$Record, [int]$Max = 0)
    if ($null -eq $Record -or $null -eq $Record.Log) { return @() }
    $lines = @(@($Record.Log) | Where-Object { $null -ne $_ -and [string]$_ -ne '' } |
        ForEach-Object { $t = [string]$_; if ($t.Length -gt 159) { $t.Substring(0, 159) } else { $t } })
    if ($lines.Count -gt $script:StartRecordMaxLogLines) { $lines = @($lines[($lines.Count - $script:StartRecordMaxLogLines)..($lines.Count - 1)]) }
    if ($Max -gt 0 -and $lines.Count -gt $Max) { $lines = @($lines[($lines.Count - $Max)..($lines.Count - 1)]) }
    return $lines
}

# The record's values from an OPEN registry key (the service's Parameters\StartRecord). The caller owns the key,
# so the real key of a tester PC and the scratch key of the host test go through the same reader. A REG_DWORD with
# the high bit set (every NTSTATUS failure has it) comes back as a negative Int32, which is why the mask is here
# and not a plain cast. No Result value means no record: a KMD before 0.7.216.30, or a key we cannot read.
function Read-StartRecordFromKey {
    param([AllowNull()]$Key)
    if ($null -eq $Key) { return $null }
    try {
        $r = [ordered]@{}
        foreach ($n in @('Result', 'Step', 'SubStep', 'TimeUtc', 'Detail')) { $r[$n] = [string]$Key.GetValue($n, '') }
        foreach ($n in @('Status', 'Stage', 'UnconfirmedStarts', 'Version')) {
            $v = $Key.GetValue($n, $null)
            $r[$n] = $(if ($null -eq $v) { $null } else { [int64]([int64]$v -band [int64]4294967295) })
        }
        $r['Log'] = @($Key.GetValue('Log', @()))
        if (-not $r.Result) { return $null }
        return $r
    } catch { return $null }
}

# True when the record names a reason for the start that is running now (the reader's rule above). A record from
# before this boot is not one, however it reads: its lines belong to another boot's start and must not be printed
# as this one's.
function Test-StartRecordIsReason {
    param([AllowNull()]$Record, [AllowNull()]$BootUtc)
    if ($null -eq $Record) { return $false }
    if (Test-StartRecordFromEarlierBoot -Record $Record -BootUtc $BootUtc) { return $false }
    return (@('failed', 'guard') -contains [string]$Record.Result)
}

# Decision rules of the logon start-confirm task (start-confirm.ps1), pure functions without I/O, host-tested by
# tests\test-start-confirm.ps1. The task confirms the boot's own KMD start once the KMD's confirmation milestone is
# met (start_health.c StartHealthRequest: flags FULL|READY|VISIBLE, completions, ready >= 60 s, last completion
# fresh or nothing owed, QUIET), the same milestone the overlay's auto-confirmation and route.py's dwm-route.ps1 use. It never confirms
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

# Flag 16 is QUIET (BC250_START_HEALTH_QUIET, 0.7.216.104): every flip programmed in this interval has completed.
# An idle GPU desktop flips about once a minute (the taskbar clock), so its last completion is mostly older than
# any freshness bound; QUIET stands in for it, as it does in the KMD's own CONFIRM. A KMD without the bit never
# sets it, and this task then waits for a fresh flip as before.
function Test-StartConfirmEligible {
 param([Parameter(Mandatory)]$Reading)
 return ((($Reading.flags -band 7) -eq 7) -and $Reading.completed -gt 0 -and
         $Reading.ready_ms -ge $script:StartConfirmMinReadyMs -and
         ($Reading.age_ms -le $script:StartConfirmFreshMs -or ($Reading.flags -band 16) -eq 16))
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
# The stop record (KMD 0.7.216.39 and later, guard.c GuardRecordStopBegin and GuardRecordStopDone). The KMD writes
# it when dxgkrnl stops a device whose start THIS generation never confirmed, and writes nothing at all for a normal
# stop of a confirmed generation. It also writes nothing when the driver itself calls its own stop to unwind a start
# that failed, because StartRecord names the step and the status of such a failure and Windows stopped nothing there.
# Only the diagnostics collector (diagnostics.ps1) reads it, and nothing decides anything from it.
#
# THE STOP READER'S RULE, which differs from the start record's in three ways:
#   Result     'attempt' the write was torn, so the record is incomplete; 'stopping' the entry snapshot is
#              complete and the teardown had not finished; 'stopped' the teardown reached StageStopDone.
#   Status     always 0. StopDevice carries no reason code from dxgkrnl, so 0 here is NOT "no problem" and not
#              the device's CM problem code. The record's Detail value says that in words.
#   age        a confirmed stop writes nothing, so a record of an earlier boot survives a healthy install. Match
#              BootId and TimeUtc against this boot before you present a record as this boot's.
# The stop record carries seven values of its own: StartStage, HealthFlags, BootId and LogLines as REG_DWORD, and
# Generation, Epoch and StopTimeUtc as REG_SZ. Generation and Epoch are decimal strings of unsigned 64-bit numbers,
# which a REG_DWORD would truncate.
$script:StopRecordKey = 'StopRecord'
# The health flags of a start that reached a healthy desktop (BC250_START_HEALTH_* of bc250kmd_escape.h: the start
# completed, the display is up and the first frame was presented). Anything less is an incomplete start.
$script:StartHealthFlagsFull = 7
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
# It is the boot_utc of Get-BootIdentity (release-witness.ps1), the one place that defines this boot:
# Win32_OperatingSystem LastBootUpTime. The callers that matter have that function loaded already (the installer's
# verify step through install.ps1, the start-confirm task); a caller that loads this file alone gets the same
# definition from release-witness.ps1 in this file's own folder, which is where every package and every installation
# puts the two scripts side by side.
# It was a tick count before: [datetime]::UtcNow minus [Environment]::TickCount64. .NET Framework, the runtime of
# Windows PowerShell 5.1 and so of every installer and task run on a tester's PC, has no TickCount64: the property
# read as $null, the subtraction returned the present moment, and every record older than the two-minute margin
# below was marked "written before this boot started". Two field reports show the false note (20261011T220244Z and
# 20261011T222253Z: the record at 22:19:52Z, the boot at 22:19:45Z).
# $null when no definition can answer, never the present moment: a boot time of "now" marks every record of this
# boot as a foreign one, which is the failure this function had.
# The fallback reading is kept here, once per process. A dot-source inside a function leaves its functions in that
# call's scope only, so Get-BootIdentity is gone again on return, and running release-witness.ps1 again also resets
# the cache that script keeps ("read once per process"): without the two variables below every call would pay
# another Win32_OperatingSystem query, which costs seconds on a tester's PC. A caller that has Get-BootIdentity
# loaded at its own top level (install.ps1, start-confirm.ps1) never reaches the fallback and keeps that script's
# own cache, so nothing here caches a reading that is not this function's.
$script:StartRecordBootIdentity = $null
$script:StartRecordBootIdentityRead = $false
# The whole identity of this boot, from the one definition: boot_utc and boot_id. The stop record carries the
# BootId of the boot it was written in, so both halves are read here and both come from the same reading.
function Get-StartRecordBootIdentity {
    $ownReading = $false
    try {
        if (-not (Get-Command -Name Get-BootIdentity -CommandType Function -ErrorAction SilentlyContinue)) {
            if ($script:StartRecordBootIdentityRead) { return $script:StartRecordBootIdentity }
            $ownReading = $true
            $witness = Join-Path $PSScriptRoot 'release-witness.ps1'
            if (Test-Path -LiteralPath $witness) { . $witness }
        }
        $id = $null
        if (Get-Command -Name Get-BootIdentity -CommandType Function -ErrorAction SilentlyContinue) { $id = Get-BootIdentity }
    } catch { $id = $null }
    if ($ownReading) { $script:StartRecordBootIdentityRead = $true; $script:StartRecordBootIdentity = $id }
    return $id
}
function Get-StartRecordBootUtc {
    $utc = $null
    try { $utc = [string](Get-StartRecordBootIdentity).boot_utc } catch { $utc = $null }
    $when = $null
    if ($utc) {
        try {
            $styles = [System.Globalization.DateTimeStyles]::AdjustToUniversal -bor [System.Globalization.DateTimeStyles]::AssumeUniversal
            $when = [datetime]::Parse($utc, [cultureinfo]::InvariantCulture, $styles)
        } catch { $when = $null }
    }
    return $when
}
# THIS BOOT'S BootId, THE VALUE A KMD RECORD IS MATCHED AGAINST
# The driver writes SharedUserData->BootId into the stop record (guard.c), which is KUSER_SHARED_DATA.BootId: the
# page that every process maps read-only at 0x7FFE0000, field offset 0x2C4. The offset is the header's own, not a
# guess: WDK 10.0.26100 km\ntddk.h declares the structure at line 8965 with BootId at line 9117, and its two
# C_ASSERTs at lines 9568 and 9569 put AlternativeArchitecture at 0x2C0 and SystemExpirationDate at 0x2C8, so the
# ULONG between them is at 0x2C4.
# Windows keeps a SECOND copy of that counter in PrefetchParameters\BootId, which Get-BootIdentity reads, and that
# copy LAGS: on 22631, 13 minutes after a boot, the shared page said 388 while the registry still said 387, so a
# fresh record of this boot was shown as an earlier boot's (BD-130). On a machine that has been up for weeks both
# say the same number, which is why the fault hid. The shared page is the reading; the registry value is the
# fallback for a host where the read fails, and every reader names which of the two it used.
$script:SharedBootIdAddress = [IntPtr]0x7FFE02C4
# KUSER_SHARED_DATA.BootId, or $null when the read fails. Host tests set AMDGPU_WDDM_TEST_SHARED_BOOT_ID to a
# number for the value or to 'unreadable' for a host that cannot read the page.
function Get-SharedBootId {
    $forced = $env:AMDGPU_WDDM_TEST_SHARED_BOOT_ID
    if ($forced) {
        if ($forced -eq 'unreadable') { return $null }
        try { return [int64]([int64]$forced -band [int64]4294967295) } catch { return $null }
    }
    try {
        $raw = [Runtime.InteropServices.Marshal]::ReadInt32($script:SharedBootIdAddress)
        return [int64]([int64]$raw -band [int64]4294967295)
    } catch { return $null }
}
# This boot's BootId with the source of the reading, for a reader that prints where its number came from:
#   @{ boot_id = <number or $null>; source = 'KUSER_SHARED_DATA' | 'PrefetchParameters registry' | 'nothing' }
function Get-StartRecordBootIdReading {
    $shared = Get-SharedBootId
    if ($null -ne $shared) { return [ordered]@{ boot_id = $shared; source = 'KUSER_SHARED_DATA' } }
    $id = $null
    try { $id = (Get-StartRecordBootIdentity).boot_id } catch { $id = $null }
    if ($null -eq $id) { return [ordered]@{ boot_id = $null; source = 'nothing' } }
    try { return [ordered]@{ boot_id = [int64]([int64]$id -band [int64]4294967295); source = 'PrefetchParameters registry' } }
    catch { return [ordered]@{ boot_id = $null; source = 'nothing' } }
}
# This boot's BootId as an unsigned 32-bit number, or $null when neither source can be read.
function Get-StartRecordBootId {
    return (Get-StartRecordBootIdReading).boot_id
}
# Where Get-StartRecordBootId took its number from. 'PrefetchParameters registry' is the weaker answer: that value
# can lag this boot by one, so a record of this boot can read as an earlier boot's.
function Get-StartRecordBootIdSource {
    return [string](Get-StartRecordBootIdReading).source
}

# True when the record was written before this boot, so it is not about any start of this boot. The two-minute
# margin covers a record written in the first seconds of a boot against a boot time that the clock moved (a time
# synchronisation right after the boot); a record left by an earlier boot is minutes or days old, never seconds.
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
    # (which decides whether the next start is refused at all) and the escape ABI word of the driver that wrote the
    # record (BC250_KMD_VERSION of bc250kmd_escape.h). That word is NOT the build: two releases share it whenever
    # the escape layout did not move, as 0.7.216.100-tester.28 and tester.29 both do at 0x000700D8, so it can only
    # tell a record apart from one written by a driver of a DIFFERENT ABI. What separates a record of an earlier
    # install or boot from this one's is the boot identity below (BootId first, TimeUtc when there is no BootId).
    $driver = if ($null -ne $Record.Version) { '0x{0:X8}' -f ([uint32]$Record.Version) } else { 'unknown' }
    $tail = "stage $($Record.Stage), at $when, UnconfirmedStarts $($Record.UnconfirmedStarts), driver ABI $driver"
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
# -AllowNoResult keeps a record whose Result value is missing, for the stop record of a driver that writes the
# other values first: the caller then prints what the key holds instead of calling the whole record absent.
function Read-StartRecordFromKey {
    param([AllowNull()]$Key, [switch]$AllowNoResult)
    if ($null -eq $Key) { return $null }
    try {
        $r = [ordered]@{}
        foreach ($n in @('Result', 'Step', 'SubStep', 'TimeUtc', 'Detail')) { $r[$n] = [string]$Key.GetValue($n, '') }
        foreach ($n in @('Status', 'Stage', 'UnconfirmedStarts', 'Version')) {
            $v = $Key.GetValue($n, $null)
            $r[$n] = $(if ($null -eq $v) { $null } else { [int64]([int64]$v -band [int64]4294967295) })
        }
        $r['Log'] = @($Key.GetValue('Log', @()))
        if (-not $r.Result -and -not $AllowNoResult) { return $null }
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

# ---- the stop record: the same key layout plus seven values of its own -------------------------------------------
# Read the stop record from an OPEN registry key (the service's Parameters\StopRecord). The shared values come from
# the start-record reader, with -AllowNoResult, because the KMD writes the other values first and a torn write
# leaves a key with no Result. The seven values of this record are read here:
#   StartStage, HealthFlags, BootId, LogLines   REG_DWORD, as unsigned 32-bit numbers
#   Generation, Epoch                           REG_SZ, decimal strings of unsigned 64-bit numbers. Kept as
#                                               strings: a 64-bit generation does not fit a DWORD, and parsing it
#                                               into a number only risks losing digits that a report needs.
#   StopTimeUtc                                 REG_SZ, the time the teardown finished. Absent until it does.
function Read-StopRecordFromKey {
    param([AllowNull()]$Key)
    if ($null -eq $Key) { return $null }
    $r = Read-StartRecordFromKey $Key -AllowNoResult
    if ($null -eq $r) { return $null }
    try {
        foreach ($n in @('StartStage', 'HealthFlags', 'BootId', 'LogLines')) {
            $v = $Key.GetValue($n, $null)
            $r[$n] = $(if ($null -eq $v) { $null } else { [int64]([int64]$v -band [int64]4294967295) })
        }
        foreach ($n in @('Generation', 'Epoch', 'StopTimeUtc')) { $r[$n] = [string]$Key.GetValue($n, '') }
        return $r
    } catch { return $null }
}

# Is this stop record about the boot that is running now? A confirmed stop writes nothing, so a record of an older
# boot survives a healthy install, and a reader that shows it as this boot's blames the wrong boot. The answer:
#   'this boot'      the record's BootId is this boot's, or no BootId can be compared and the time is in this boot
#   'earlier boot'   the record's BootId is lower, or the time is before this boot started
#   'later boot'     the record's BootId is higher than this boot's, which no record of the past can be
#   'unknown'        nothing can place it: no BootId to compare and no boot time on this computer
# The BootId decides when both sides have one, because a clock that moved cannot change it. 'later boot' is not a
# record of the future: the counter only grows, so a higher number means the two numbers come from two different
# counters, which is what BD-130 was (this boot read from the lagging registry copy against a record written from
# KUSER_SHARED_DATA, 387 against 388, reported as "AN EARLIER BOOT").
function Test-StopRecordBoot {
    param([AllowNull()]$Record, [AllowNull()]$BootUtc, [AllowNull()]$BootId)
    if ($null -eq $Record) { return 'unknown' }
    if ($null -ne $BootId -and $null -ne $Record.BootId) {
        if ([int64]$Record.BootId -eq [int64]$BootId) { return 'this boot' }
        if ([int64]$Record.BootId -gt [int64]$BootId) { return 'later boot' }
        return 'earlier boot'
    }
    if ($null -eq $BootUtc -or -not $Record.TimeUtc) { return 'unknown' }
    if (Test-StartRecordFromEarlierBoot -Record $Record -BootUtc $BootUtc) { return 'earlier boot' }
    return 'this boot'
}

# One line a tester can paste. It never calls Status a verdict: StopDevice carries no reason code from dxgkrnl, so
# the 0 in that value says nothing about why Windows stopped the device, and this line says that in words.
function Format-StopRecord {
    param([AllowNull()]$Record, [AllowNull()]$BootUtc, [AllowNull()]$BootId)
    if ($null -eq $Record) {
        return 'no stop record: a driver before 0.7.216.39 keeps none, Windows stopped no unconfirmed start of this boot, the last start failed and is in StartRecord instead, or the key cannot be read'
    }
    $result = [string]$Record.Result
    $what = switch ($result) {
        'stopped'  { 'Windows stopped the device after a start that nothing confirmed, and the teardown finished' }
        'stopping' { 'Windows stopped the device after a start that nothing confirmed; the record was written at the entry of the stop and the teardown had not finished' }
        'attempt'  { 'the stop record is incomplete: the driver began to write it and the write was torn' }
        ''         { 'the key holds no result value: the driver began to write the record and the write was torn' }
        default    { "unreadable stop record: Result '$result'" }
    }
    $when = $(if ($Record.TimeUtc) { [string]$Record.TimeUtc } else { 'no time' })
    $done = $(if ($Record.StopTimeUtc) { [string]$Record.StopTimeUtc } else { 'the teardown wrote no finish time' })
    $full = $script:StartHealthFlagsFull
    $flags = $(if ($null -eq $Record.HealthFlags) { 'no health flags' } else { "start-health flags $($Record.HealthFlags)$(if (([int64]$Record.HealthFlags -band $full) -ne $full) { ', so the start was not full, ready and visible' } else { '' })" })
    $where = $(if ($null -eq $Record.StartStage) { 'no stage before the stop' } else { "stage $($Record.StartStage) before the stop" })
    $now = $(if ($null -eq $Record.Stage) { 'no stop stage' } else { "stop stage $($Record.Stage)" })
    $who = "generation $(if ($Record.Generation) { [string]$Record.Generation } else { 'unknown' }), epoch $(if ($Record.Epoch) { [string]$Record.Epoch } else { 'unknown' })"
    $boot = $(if ($null -eq $Record.BootId) { 'no BootId' } else { "BootId $($Record.BootId)" })
    $lines = $(if ($null -eq $Record.LogLines) { 'no line count' } else { "$($Record.LogLines) log line(s) kept" })
    $driver = $(if ($null -ne $Record.Version) { '0x{0:X8}' -f ([uint32]$Record.Version) } else { 'unknown' })
    $text = "$what; $where, $now, $flags, $who, $boot, at $when, finished $done, $lines, driver ABI $driver"
    if ($Record.Detail) { $text = "$text; detail: $([string]$Record.Detail)" }
    # Status is in the line for completeness and with its meaning beside it, never as a verdict.
    $text = "$text; Status $(Format-StartRecordStatus $Record.Status), which is no reason: StopDevice gives the driver no code for why Windows stopped the device"
    switch (Test-StopRecordBoot -Record $Record -BootUtc $BootUtc -BootId $BootId) {
        'earlier boot' { $text = "$text; NOTE this record is from an EARLIER boot, so it is not about this boot's start: a stop that a confirmed start preceded writes nothing and leaves the older record standing" }
        'later boot'   { $text = "$text; NOTE the record's BootId is HIGHER than the number read for this boot, which no record of an earlier boot can be: the two numbers come from two different counters, so this record cannot be placed in a boot by its BootId" }
        'unknown'      { $text = "$text; NOTE neither the BootId nor the boot time of this computer can place this record in a boot, so it may be older than this one" }
    }
    return $text
}

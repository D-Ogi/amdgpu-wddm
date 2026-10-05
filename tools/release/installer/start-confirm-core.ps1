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

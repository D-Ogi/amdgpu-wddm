# The GPU clock and the DPM governor's state once a second, with QueryPerformanceCounter ticks, so that
# measure-flash.ps1 can place each flash of the start window against a clock change as well as against a poll step.
# Windows PowerShell 5.1:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File dpm-sample.ps1 -Out <csv> [-Seconds 170] [-PeriodMs 1000] [-Tools <dir>]
#
#   -Tools     the installed tools folder (default C:\Program Files\amdgpu-wddm\tools), where bc250control.dll is
#   -Seconds   how long to sample
#   -PeriodMs  the time from the start of one read to the start of the next
#   -Out       the CSV, one line for each read. qpc is [Diagnostics.Stopwatch]::GetTimestamp() just before the read,
#              the clock of flash-watch.exe and poll-repro.ps1
#
# Each read is Bc250Dpm of bc250control.dll with the 192-byte ABI 2 structure (the 160-byte ABI 1 prefix when the
# driver refuses 192): BC250_ESCAPE_RUN_DPM op READ, the governor's published snapshot, the same read the overlay
# makes. It sends no SMU message, reads no BAR and does not idle the scheduler (bc250kmd_cli.c, Bc250Dpm). The
# layout is BC250_ESCAPE_DPM of driver/kmd/bc250kmd_escape.h. This script changes nothing and writes only -Out.
param([string]$Tools = 'C:\Program Files\amdgpu-wddm\tools', [int]$Seconds = 170, [int]$PeriodMs = 1000,
      [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$dll = [IO.Path]::GetFullPath((Join-Path $Tools 'bc250control.dll'))
Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
namespace Bc250FlashProbe {
    public static class Dpm {
        [DllImport(@"$dll", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        public static extern int Bc250Dpm([Out] byte[] data, uint bytes);
    }
}
"@

$columns = 'sample,qpc,status,abi,flags,mode,reason,throttle,max_mhz,cap_mhz,target_mhz,want_mhz,current_mhz,current_mv,' +
           'observed_mhz,temperature_mc,busy_permille,busy_avg_permille,raises,lowers,thermal_events,errors,resyncs,' +
           'idle_mhz,idle_entries,idle_exits,idle_refusals,idle_ms'
$writer = New-Object IO.StreamWriter($Out, $false, (New-Object Text.UTF8Encoding($false)))
$writer.WriteLine($columns)
$writer.Flush()

$bytes = 192
$clock = [Diagnostics.Stopwatch]::StartNew()
$sample = 0
try {
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
        $due = $sample * $PeriodMs
        $sample++
        $buffer = New-Object byte[] 192
        $qpc = [Diagnostics.Stopwatch]::GetTimestamp()
        $status = [Bc250FlashProbe.Dpm]::Bc250Dpm($buffer, $bytes)
        if ($status -eq [int]0xC000000D -and $bytes -eq 192) {
            # A driver before 0.7.207 knows the ABI 1 prefix alone; the idle fields then stay zero.
            $bytes = 160
            $buffer = New-Object byte[] 192
            $qpc = [Diagnostics.Stopwatch]::GetTimestamp()
            $status = [Bc250FlashProbe.Dpm]::Bc250Dpm($buffer, $bytes)
        }
        $u = { param($o) [BitConverter]::ToUInt32($buffer, $o) }
        $fields = @($sample, $qpc, ('0x{0:X8}' -f $status))
        if ($status -eq 0) {
            $fields += @((& $u 20), ('0x{0:X}' -f (& $u 28)), (& $u 32), (& $u 40), (& $u 44), (& $u 48), (& $u 52), (& $u 56), (& $u 60),
                (& $u 64), (& $u 68), (& $u 72), [BitConverter]::ToInt32($buffer, 80), (& $u 84), (& $u 88), (& $u 92), (& $u 96),
                (& $u 100), (& $u 104), (& $u 108), (& $u 160), (& $u 172), (& $u 176), (& $u 180), [BitConverter]::ToUInt64($buffer, 184))
        } else {
            $fields += @('') * 25
        }
        $writer.WriteLine($fields -join ',')
        $writer.Flush()
        $wait = $due + $PeriodMs - [int]$clock.Elapsed.TotalMilliseconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
    }
} finally { $writer.Dispose() }
exit 0

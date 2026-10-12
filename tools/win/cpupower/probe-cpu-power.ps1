# Read-only CPU power probe for unit A. It changes nothing: it queries the active power
# scheme's processor settings and samples four processor performance counters while idle.
#
# No setting is written. `powercfg /setacvalueindex`, `/setactive` and `/energy` are not used
# (`/energy` runs for 60 s or more and writes a report file).
#
#   python tools/win/target.py ps tools/win/cpupower/probe-cpu-power.ps1 [seconds]
#
# Output: one text block. The caller keeps it as evidence.

param([ValidateRange(1,180)][int]$Seconds = 20)

# PDH's English API is language-neutral; localized PerformanceCounter/Get-Counter paths are not.
# Kept self-contained for target.py and trial staging. test_tool_locale.py checks both copies.
# https://learn.microsoft.com/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw
# https://learn.microsoft.com/windows/win32/api/pdh/nf-pdh-pdhgetformattedcountervalue
function Initialize-EnglishCounter {
 if ('Bc250Tools.EnglishCounter' -as [type]) { return }
 Add-Type -ErrorAction Stop -TypeDefinition @'
using System;
using System.Globalization;
using System.Runtime.InteropServices;
namespace Bc250Tools {
 [StructLayout(LayoutKind.Explicit, Size=16)]
 public struct CounterValue {
  [FieldOffset(0)] public uint Status;
  [FieldOffset(8)] public double Value;
 }
 public interface ICounterApi {
  uint Open(out IntPtr query);
  uint Add(IntPtr query, string path, out IntPtr counter);
  uint Collect(IntPtr query);
  uint Read(IntPtr counter, out CounterValue value);
  void Close(IntPtr query);
 }
 sealed class WindowsCounterApi : ICounterApi {
  [DllImport("pdh.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
  static extern uint PdhOpenQueryW(string source, UIntPtr user, out IntPtr query);
  [DllImport("pdh.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
  static extern uint PdhAddEnglishCounterW(IntPtr query, string path, UIntPtr user, out IntPtr counter);
  [DllImport("pdh.dll", ExactSpelling=true)]
  static extern uint PdhCollectQueryData(IntPtr query);
  [DllImport("pdh.dll", ExactSpelling=true)]
  static extern uint PdhGetFormattedCounterValue(IntPtr counter, uint format, IntPtr type, out CounterValue value);
  [DllImport("pdh.dll", ExactSpelling=true)]
  static extern uint PdhCloseQuery(IntPtr query);
  public uint Open(out IntPtr query) { return PdhOpenQueryW(null, UIntPtr.Zero, out query); }
  public uint Add(IntPtr query, string path, out IntPtr counter) { return PdhAddEnglishCounterW(query, path, UIntPtr.Zero, out counter); }
  public uint Collect(IntPtr query) { return PdhCollectQueryData(query); }
  public uint Read(IntPtr counter, out CounterValue value) {
   // PDH_FMT_DOUBLE | PDH_FMT_NOCAP100: CPU performance can exceed nominal (100%).
   return PdhGetFormattedCounterValue(counter, 0x200 | 0x8000, IntPtr.Zero, out value);
  }
  public void Close(IntPtr query) { PdhCloseQuery(query); }
 }
 public sealed class EnglishCounter : IDisposable {
  readonly ICounterApi api;
  IntPtr query, counter;
  public EnglishCounter(string path) : this(path, new WindowsCounterApi()) {}
  public EnglishCounter(string path, ICounterApi api) {
   if (api == null) throw new ArgumentNullException("api");
   if (String.IsNullOrEmpty(path)) throw new ArgumentException("Counter path missing", "path");
   this.api = api;
   Check(api.Open(out query), "PdhOpenQuery");
   try {
    Check(api.Add(query, path, out counter), "PdhAddEnglishCounter");
    // A rate needs two observations. Prime once now; never interpret an invalid first value as zero.
    Check(api.Collect(query), "PdhCollectQueryData");
   } catch { Dispose(); throw; }
  }
  static void Check(uint status, string call) {
   if (status != 0) throw new InvalidOperationException(call + " status 0x" + status.ToString("X8", CultureInfo.InvariantCulture));
  }
  public double NextValue() {
   if (query == IntPtr.Zero) throw new ObjectDisposedException("EnglishCounter");
   Check(api.Collect(query), "PdhCollectQueryData");
   CounterValue result;
   Check(api.Read(counter, out result), "PdhGetFormattedCounterValue");
   // PDH_CSTATUS_VALID_DATA=0, PDH_CSTATUS_NEW_DATA=1. Other statuses are not readings.
   if (result.Status > 1 || Double.IsNaN(result.Value) || Double.IsInfinity(result.Value))
    throw new InvalidOperationException("Counter data unavailable, status 0x" + result.Status.ToString("X8", CultureInfo.InvariantCulture));
   return result.Value;
  }
  public void Dispose() {
   if (query != IntPtr.Zero) { var old = query; query = IntPtr.Zero; api.Close(old); }
  }
 }
}
'@
}


$ErrorActionPreference = "Stop"
Write-Output "probe-cpu-power 1.0"
Write-Output ("utc " + (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"))
Write-Output ("host " + $env:COMPUTERNAME)
Write-Output ("os " + (Get-CimInstance Win32_OperatingSystem).Version)

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
Write-Output ("cpu " + $cpu.Name.Trim())
Write-Output ("cores " + $cpu.NumberOfCores + " logical " + $cpu.NumberOfLogicalProcessors)
Write-Output ("max-clock-mhz " + $cpu.MaxClockSpeed)
Write-Output ("current-clock-mhz " + $cpu.CurrentClockSpeed)

Write-Output "--- active scheme"
$active = powercfg /getactivescheme
Write-Output $active
$guid = ([regex]::Match($active, "([0-9a-fA-F-]{36})")).Groups[1].Value

Write-Output "--- processor settings of the active scheme (powercfg /qh <scheme> SUB_PROCESSOR)"
# SUB_PROCESSOR is the processor power management subgroup. Query only, no index is set.
powercfg /qh $guid SUB_PROCESSOR

Write-Output "--- idle counter samples"
Write-Output ("samples " + $Seconds + " at 1 s")
$paths = @(
    "\Processor Information(_Total)\% of Maximum Frequency",
    "\Processor Information(_Total)\% Processor Performance",
    "\Processor Information(_Total)\% Processor Time",
    "\Processor Information(_Total)\Processor Frequency")
$rows = @{}
Initialize-EnglishCounter
$samples = @()
try {
    foreach ($path in $paths) { $samples += [Bc250Tools.EnglishCounter]::new($path) }
    for ($i = 0; $i -lt $Seconds; $i++) {
        Start-Sleep -Seconds 1
        for ($j = 0; $j -lt $paths.Count; $j++) {
            $name = $paths[$j].Substring($paths[$j].IndexOf(")\") + 2)
            if (-not $rows.ContainsKey($name)) { $rows[$name] = @() }
            $rows[$name] += $samples[$j].NextValue()
        }
    }
} finally { foreach ($sample in $samples) { $sample.Dispose() } }
foreach ($name in $rows.Keys | Sort-Object) {
    $v = $rows[$name]
    $mean = ($v | Measure-Object -Average).Average
    $min = ($v | Measure-Object -Minimum).Minimum
    $max = ($v | Measure-Object -Maximum).Maximum
    Write-Output ([string]::Format([Globalization.CultureInfo]::InvariantCulture,
        '{0}: n {1} mean {2:0.00} min {3:0.00} max {4:0.00}', $name, $v.Count, $mean, $min, $max))
}
Write-Output "probe-cpu-power end"

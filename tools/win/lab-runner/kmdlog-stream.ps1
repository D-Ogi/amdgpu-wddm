$ErrorActionPreference='Continue';$d=$PSScriptRoot
# Game profile: appends the KMD log, the clock/temperature and the memory counters to disk while the game runs,
# each write through the cache, so that a machine-wide stop leaves what was read on disk.
# Started and ended by the supervisor; ends by itself after 260 s or when kmdlog.stop appears. Since 193 the
# 260 s grow by config.game_seconds' excess over 300 s (as game-runtime.ps1's bound does): 184's stream ended
# 53 s before the corruption onset of a 600 s session, so the paging records of the onset were never captured.
# Every external call is bounded (2 s) and its loss is written: a blocked KMD read must not silence the
# memory samples or the stop check. The interval between records is therefore not a maximum record age.
$out=Join-Path $d 'game-kernel.log'
# Since KMD revision 184: its CLI reads log and journal without adapter synchronization (lever L1); an older KMD
# answers it through the HardwareAccess fallback. Operator samplers use a separate copy (kmd184\sampler).
# The release client for both (tester.10 on): HKLM\SOFTWARE\amdgpu-wddm\Release InstallRoot\tools\bc250kmd_cli.exe.
$cli=Join-Path ([string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction SilentlyContinue).InstallRoot) 'tools\bc250kmd_cli.exe'
$file=New-Object IO.FileStream($out,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
$encoding=New-Object Text.UTF8Encoding($false)
function Put([string]$text){$b=$encoding.GetBytes(('{0:HH:mm:ss.fff} {1}' -f [DateTime]::UtcNow,$text)+"`n");$file.Write($b,0,$b.Length);$file.Flush($true)}
# The installed release's bc250kmd_cli.exe is the only KMD client (owner, 2026-10-08: the harness needs nothing
# from the old lab directories). A missing client, or a query form its usage does not list, is an error that says so.
function Resolve-KmdClient([string]$Release,[string]$Form){
 if(!$Release -or !(Test-Path -LiteralPath $Release)){throw "No release KMD client '$Release' (HKLM\SOFTWARE\amdgpu-wddm\Release InstallRoot\tools\bc250kmd_cli.exe)"}
 $ErrorActionPreference='Continue'
 $usage=try{& $Release 2>&1|Out-String}catch{''}
 if($usage.Contains($Form)){return $Release}
 throw "The release KMD client $Release does not list '$Form'"
}
$clock=try{Resolve-KmdClient $cli 'clock read'}catch{Put ('lost clock client: '+$_.Exception.Message);$cli}
Put ('clock client '+$clock)
# One bounded run of a console tool: its output, or $null with a loss record when it did not end in time.
function Run-Bounded([string]$exe,[string]$arguments,[string]$tag){
 $psi=New-Object Diagnostics.ProcessStartInfo
 $psi.FileName=$exe;$psi.Arguments=$arguments;$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true
 $psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
 $p=$null
 try{
  $p=[Diagnostics.Process]::Start($psi)
  $read=$p.StandardOutput.ReadToEndAsync()
  $null=$p.StandardError.ReadToEndAsync()
  if(!$p.WaitForExit(2000)){
   try{$p.Kill()}catch{}
   Put ('lost '+$tag+': no answer in 2 s')
   return $null
  }
  if(!$read.Wait(1000)){Put ('lost '+$tag+': output not read');return $null}
  return [string]$read.Result
 }catch{Put ('lost '+$tag+': '+$_.Exception.Message);return $null}
 finally{if($p){$p.Dispose()}}
}
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

try { Initialize-EnglishCounter } catch { Put ('lost native counters: '+$_.Exception.Message) }
$timer=[Diagnostics.Stopwatch]::StartNew();$next=-1;$tick=0
# 101 had 548-765 MB available at the menu and the lab slowed (paging is the hypothesis these lines test; review
# 845). CPU of dwm and the game as percent of the whole
# machine (all logical processors) over the measured interval, and Memory\Pages Input/sec from one primed counter
# instance. A changed pid or a failed read gives no sample, never a zero (844).
$cpus=[Environment]::ProcessorCount
$cpuPrev=@{}
$pagesIn=$null
try{$pagesIn=New-Object Bc250Tools.EnglishCounter('\Memory\Pages Input/sec')}catch{Put ('lost pages counter: '+$_.Exception.Message)}
# 149+: the slow-start A/B (engine-slowstart REPORT.md) asks whether the machine itself was slower during
# 139-147: the CPU's actual clock as a percentage of nominal and the disk read rate, both machine-wide.
$cpuPerf=$null;$diskRead=$null;$availableMemory=$null
try{$cpuPerf=New-Object Bc250Tools.EnglishCounter('\Processor Information(_Total)\% Processor Performance')}catch{Put ('lost cpu perf counter: '+$_.Exception.Message)}
try{$diskRead=New-Object Bc250Tools.EnglishCounter('\PhysicalDisk(_Total)\Disk Read Bytes/sec')}catch{Put ('lost disk counter: '+$_.Exception.Message)}
try{$availableMemory=New-Object Bc250Tools.EnglishCounter('\Memory\Available MBytes')}catch{Put ('lost memory counter: '+$_.Exception.Message)}
function Put-Cpu([string]$name){
 try{
  $p=Get-Process $name -ErrorAction Stop|Select-Object -First 1
  $now=$timer.Elapsed.TotalSeconds;$cpu=$p.TotalProcessorTime.TotalSeconds
  $prev=$cpuPrev[$name];$cpuPrev[$name]=@($p.Id,$now,$cpu)
  if(!$prev){return}
  if($prev[0] -ne $p.Id){Put ('cpu '+$name+' pid changed '+$prev[0]+' -> '+$p.Id+', no sample');return}
  $interval=$now-$prev[1]
  Put ([string]::Format([Globalization.CultureInfo]::InvariantCulture,'cpu {0} pid={1} machine_pct={2:0.0} interval_s={3:0.00} cpus={4}',$name,$p.Id,(100*($cpu-$prev[2])/$interval/$cpus),$interval,$cpus))
 }catch{$cpuPrev.Remove($name);Put ('lost cpu '+$name+': '+$_.Exception.Message)}
}
try{
 Put 'stream begin'
 $streamLimit=260
 try{$sc=Get-Content "$d\config.json" -Raw|ConvertFrom-Json;if($sc.game_seconds -and [int]$sc.game_seconds -gt 300){$streamLimit+=[int]$sc.game_seconds-300}}catch{Put ('config not read: '+$_.Exception.Message)}
 Put ('stream limit '+$streamLimit+' s')
 # Game runner: the processes counted are the profile's counters (game-profile.json, staged with the game; witcher3
 # without one). With one name the lines are those of before; with more each line ends with the process name.
 $counters=@('witcher3')
 try{if(Test-Path -LiteralPath "$d\game-profile.json"){$counters=@((Get-Content -LiteralPath "$d\game-profile.json" -Raw|ConvertFrom-Json).processes.counters|Where-Object{$_})}}catch{Put ('profile not read: '+$_.Exception.Message)}
 # The driver's state where this session starts: LOG_SUMMARY writes the WDDM, DPM, interop and OTG tables into the
 # KMD's ring and then reads the ring back (display.c LogEscape, wddm.c WddmSummary), so one call puts the table of
 # this start into the evidence - the first question of a 0x116 triage. It is a HardwareAccess escape, which suspends
 # the GPU scheduler for up to one VSync, so it is sent exactly once, here, before the game is in a frame loop, and
 # never from the loop below. Up to 0.7.212 the loop polled it every 500 ms until the header matched, and discarded
 # every line of it: the counter in the header is already past the summary's own lines, so they went to no file.
 $head=Run-Bounded $cli 'log summary' 'summary'
 if($head){
  # One record, one flush, as an interval of the loop is written: the summary is up to a few hundred lines.
  $block=New-Object Text.StringBuilder
  foreach($line in ($head -split "`r?`n")){
   if($line.TrimEnd()){$null=$block.Append('kmd summary ').Append($line.TrimEnd()).Append("`n")}
   if($line -match 'log\s+(\d+) lines since'){$next=[int64]$Matches[1]}
  }
  $b=$encoding.GetBytes(('{0:HH:mm:ss.fff} driver summary at the stream start' -f [DateTime]::UtcNow)+"`n"+$block.ToString())
  $file.Write($b,0,$b.Length);$file.Flush($true)
  if($next -ge 0){Put ('log lines so far '+$next)}
 }
 while($timer.Elapsed.TotalSeconds -lt $streamLimit -and !(Test-Path -LiteralPath (Join-Path $d 'kmdlog.stop'))){
  if($next -lt 0){
   # The summary above did not answer: the header alone, through GET_LOG, which needs no adapter synchronization.
   # The same line ("log N lines since this driver load") stands at the head of both commands' first page.
   $first=Run-Bounded $cli 'log 0' 'log header'
   if($first -and $first -match 'log\s+(\d+) lines since'){$next=[int64]$Matches[1];Put ('log lines so far '+$next)}
  }else{
   # The newest lines matter most before a stop (068 lost them to a cap that kept the oldest): one write
   # per interval, the newest 600 lines, the number left out stated first.
   $text=Run-Bounded $cli ('log '+$next) 'log'
   if($text){
    $fresh=New-Object Collections.Generic.List[string]
    foreach($line in ($text -split "`r?`n")){
     if($line -match '^\s*(\d+)\s+[0-9.]+\s'){
      $index=[int64]$Matches[1]
      if($index -ge $next){$fresh.Add($line.TrimEnd());$next=$index+1}
     }
    }
    if($fresh.Count){
     $skip=[Math]::Max(0,$fresh.Count-600)
     $block=New-Object Text.StringBuilder
     if($skip){$null=$block.Append('kmd (').Append($skip).Append(" older lines of this interval left out)`n")}
     for($i=$skip;$i -lt $fresh.Count;$i++){$null=$block.Append('kmd ').Append($fresh[$i]).Append("`n")}
     $b=$encoding.GetBytes(('{0:HH:mm:ss.fff} interval of {1} lines' -f [DateTime]::UtcNow,$fresh.Count)+"`n"+$block.ToString())
     $file.Write($b,0,$b.Length);$file.Flush($true)
    }
   }
  }
  if($tick % 4 -eq 0){
   $c=Run-Bounded $clock 'clock read' 'clock'
   if($c){Put ('clock '+$c.Trim())}
   # Process counters from the process table, not from WMI: no call here can wait on a service.
   try{
    foreach($counter in $counters){
     $g=Get-Process $counter -ErrorAction SilentlyContinue|Select-Object -First 1
     if($g){Put ('game pid='+$g.Id+' ws_mb='+[int]($g.WorkingSet64/1MB)+' private_mb='+[int]($g.PrivateMemorySize64/1MB)+' threads='+$g.Threads.Count+' handles='+$g.HandleCount+$(if($counters.Count -gt 1){' name='+$counter}else{''}))}
    }
    if($availableMemory){Put ('memory free_mb='+[int]$availableMemory.NextValue())}
   }catch{Put ('lost counters: '+$_.Exception.Message)}
   Put-Cpu 'dwm';foreach($counter in $counters){Put-Cpu $counter}
   if($pagesIn){try{Put ('memory pages_input_per_s='+[int]$pagesIn.NextValue())}catch{Put ('lost pages counter: '+$_.Exception.Message)}}
   if($cpuPerf){try{Put ('cpu perf_pct='+[int]$cpuPerf.NextValue())}catch{Put ('lost cpu perf counter: '+$_.Exception.Message)}}
   if($diskRead){try{Put ('disk read_mb_per_s='+[int]($diskRead.NextValue()/1MB))}catch{Put ('lost disk counter: '+$_.Exception.Message)}}
  }
  $tick++
  Start-Sleep -Milliseconds 500
 }
 Put 'stream end'
}catch{try{Put ('stream error: '+$_.Exception.Message)}catch{}}
finally{foreach($sample in @($pagesIn,$cpuPerf,$diskRead,$availableMemory)){if($sample){$sample.Dispose()}};$file.Dispose()}

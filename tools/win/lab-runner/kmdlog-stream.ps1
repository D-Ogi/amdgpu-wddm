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
# tester.10's release bc250kmd_cli.exe (built from the KMD branch) has no "health" or "clock" command. A query
# form its usage does not list goes to the client that answered that form before the release layout (07147: health,
# 07136: clock), until a release client carries every form (release/tester11-cli).
function Resolve-KmdClient([string]$Release,[string]$Form){
 $ErrorActionPreference='Continue'
 $usage=try{& $Release 2>&1|Out-String}catch{''}
 if($usage.Contains($Form)){return $Release}
 $legacy=if($Form -like 'clock *'){'C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe'}else{'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'}
 if(Test-Path -LiteralPath $legacy){return $legacy}
 throw "No KMD client answers '$Form': $Release does not list it and $legacy is absent"
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
$timer=[Diagnostics.Stopwatch]::StartNew();$next=-1;$tick=0
# 101 had 548-765 MB available at the menu and the lab slowed (paging is the hypothesis these lines test; Codex
# 845). CPU of dwm and the game as percent of the whole
# machine (all logical processors) over the measured interval, and Memory\Pages Input/sec from one primed counter
# instance. A changed pid or a failed read gives no sample, never a zero (844).
$cpus=[Environment]::ProcessorCount
$cpuPrev=@{}
$pagesIn=$null
try{$pagesIn=New-Object Diagnostics.PerformanceCounter('Memory','Pages Input/sec');$null=$pagesIn.NextValue()}catch{Put ('lost pages counter: '+$_.Exception.Message)}
# 149+: the slow-start A/B (engine-slowstart REPORT.md) asks whether the machine itself was slower during
# 139-147: the CPU's actual clock as a percentage of nominal and the disk read rate, both machine-wide.
$cpuPerf=$null;$diskRead=$null
try{$cpuPerf=New-Object Diagnostics.PerformanceCounter('Processor Information','% Processor Performance','_Total');$null=$cpuPerf.NextValue()}catch{Put ('lost cpu perf counter: '+$_.Exception.Message)}
try{$diskRead=New-Object Diagnostics.PerformanceCounter('PhysicalDisk','Disk Read Bytes/sec','_Total');$null=$diskRead.NextValue()}catch{Put ('lost disk counter: '+$_.Exception.Message)}
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
 while($timer.Elapsed.TotalSeconds -lt $streamLimit -and !(Test-Path -LiteralPath (Join-Path $d 'kmdlog.stop'))){
  if($next -lt 0){
   # The header is asked for again until it was read once.
   $first=Run-Bounded $cli 'log summary' 'log header'
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
    $free=(New-Object Diagnostics.PerformanceCounter('Memory','Available MBytes')).NextValue()
    Put ('memory free_mb='+[int]$free)
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
finally{$file.Dispose()}

param(
 [Parameter(Mandatory=$true)][string]$Out,
 [Parameter(Mandatory=$true)][string]$BatchDirectory,
 [string]$Tools='C:\BC250\m12\system-icd\tools',
 [string]$Icd='C:\BC250\m12\mesa05-queue\vulkan_radeon.dll',
 [ValidateRange(0,100000)][int]$MaxBatches=0
)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\cts-qpa-monitor.ps1"
if(Test-Path $Out){throw 'Output directory must be new'}
if((Get-Process -Id $PID).SessionId -eq 0){throw 'Full CTS includes WSI; use an interactive elevated session'}
$token=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $token.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Elevated telemetry required'}
if(Get-Process deqp-vk -ErrorAction SilentlyContinue){throw 'Another CTS process is active'}
if((Get-FileHash $Icd).Hash -ne '4D027149571DC000DA1E5006E6E393FCA6178DB32F1D9CB25D684E60849A5805'){throw 'Unexpected ICD'}
$manifest=Get-Content (Join-Path $BatchDirectory 'manifest.json') -Raw | ConvertFrom-Json
if($manifest.source_commit -ne 'f6a29701220f34dd1407513bfe80d74ca7b392ce'){throw 'Unexpected CTS source'}
if((Get-FileHash "$Tools\deqp-vk.exe").Hash -ne 'AE7BEFDD190EF08E4A715DE0348734879263E1854E017D67865749905A95A2B6'){throw 'Unexpected CTS executable'}
$sys=Join-Path $env:windir 'System32\vulkan-1.dll'
foreach($name in @('VK_DRIVER_FILES','VK_ICD_FILENAMES','VK_ADD_DRIVER_FILES','VK_LOADER_DRIVERS_SELECT','VK_LOADER_DRIVERS_DISABLE')){Remove-Item "Env:$name" -ErrorAction SilentlyContinue}
$env:RADV_EXPERIMENTAL='sparse';$env:VK_LOADER_DEBUG='driver';$env:BC250_TRACE_SUBMITS='0'
$env:PATH=($env:PATH.Split(';') | Where-Object {$_ -notlike 'C:\BC250\*'}) -join ';'
$env:TEMP='C:\BC250\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Path $Out | Out-Null
function Save($name,$value){$value | ConvertTo-Json -Depth 8 | Set-Content "$Out\$name.json" -Encoding UTF8}
function CheckStop {if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}}
function ReadCli([string]$mode,[string]$path) {
 $si=[Diagnostics.ProcessStartInfo]::new()
 $si.FileName='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
 $si.Arguments="$mode read";$si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true
 $proc=[Diagnostics.Process]::Start($si)
 try {
  $text=$proc.StandardOutput.ReadToEndAsync()
  if(-not $proc.WaitForExit(4000)){ $proc.Kill();throw "$mode timeout" }
  if($proc.ExitCode -ne 0 -or -not $text.Wait(1000)){throw "$mode failed"}
  [IO.File]::WriteAllText($path,$text.Result)
 } finally {$proc.Dispose()}
}

function CheckHealth($index){
 $cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
 $values=@{}
 foreach($mode in @('clock','health')){
  $path="$Out\check-$index-$mode.txt"
  ReadCli $mode $path
  $values[$mode]=[IO.File]::ReadAllText($path)
 }
 if($values.clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/thermal gate'}
 if($values.health -notmatch 'flags=15 generation=(\d+) epoch=(\d+)'){throw 'Health gate'}
 $identity=$Matches[1]+':'+$Matches[2]
 if($script:gpuIdentity -and $script:gpuIdentity -ne $identity){throw 'GPU generation changed'}
 $script:gpuIdentity=$identity
 @{index=$index;utc=[DateTime]::UtcNow.ToString('o');clock=$values.clock;health=$values.health} | ConvertTo-Json -Compress | Add-Content "$Out\health.jsonl" -Encoding UTF8
}

function RunBatch($batch,[int]$number) {
 $list=Join-Path $BatchDirectory $batch.file
 if((Get-FileHash $list).Hash -ne $batch.sha256){throw 'Batch hash mismatch'}
 $cases=[IO.File]::ReadAllLines($list)
 if($cases.Count -ne $batch.count){throw 'Batch count mismatch'}
 $state=New-CtsQpaMonitor $cases 0
 $stem='batch-{0:D5}' -f $number;$qpa="$Out\$stem.qpa"
 $arguments=@("--deqp-caselist-file=$list","--deqp-log-filename=$qpa",'--deqp-watchdog=enable','--deqp-terminate-on-fail=enable','--deqp-terminate-on-device-lost=enable','--deqp-log-images=enable','--deqp-log-shader-sources=enable')
 $psi=[Diagnostics.ProcessStartInfo]::new()
 $psi.FileName="$Tools\deqp-vk.exe";$psi.Arguments=($arguments | ForEach-Object {'"'+$_+'"'}) -join ' '
 $psi.WorkingDirectory=$Tools;$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true
 $psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
 $stdout=[IO.File]::Create("$Out\$stem.out");$stderr=[IO.File]::Create("$Out\$stem.err")
 $child=$null;$reader=$null;$modules=@{}
 $timer=[Diagnostics.Stopwatch]::StartNew();$lastHealth=0
 try{
  $child=[Diagnostics.Process]::Start($psi)
  $copyOut=$child.StandardOutput.BaseStream.CopyToAsync($stdout);$copyErr=$child.StandardError.BaseStream.CopyToAsync($stderr)
  Save "$stem-launch" @{pid=$child.Id;arguments=$arguments;cases_sha256=$batch.sha256;icd=$Icd}
  while($true){
   $child.Refresh();$exited=$child.HasExited
   if(-not $reader -and (Test-Path $qpa)){
    $stream=[IO.File]::Open($qpa,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8)
   }
   if($reader){
    foreach($event in @(Add-CtsQpaText $state $reader.ReadToEnd() $timer.ElapsedMilliseconds)){
     if($event.event -eq 'end'){
      @{batch=$number;case=$event.case;status=$event.status;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Compress | Add-Content "$Out\cases.jsonl" -Encoding UTF8
     }else{Save 'active' @{batch=$number;case=$event.case;utc=[DateTime]::UtcNow.ToString('o')}}
    }
   }
   Assert-CtsQpaDeadline $state $timer.ElapsedMilliseconds
   if(-not $exited){try{foreach($module in $child.Modules){if($module.ModuleName -match 'vulkan'){$modules[$module.FileName]=1}}}catch{}}
   if($exited){break}
   if($timer.ElapsedMilliseconds-$lastHealth -ge 5000){CheckStop;CheckHealth "$number-$($state.Finished)";$lastHealth=$timer.ElapsedMilliseconds}
   Start-Sleep -Milliseconds 100
  }
  if(-not $copyOut.Wait(2000) -or -not $copyErr.Wait(2000)){throw 'Output drain timeout'}
  if($child.ExitCode -ne 0){throw "CTS exit $($child.ExitCode)"}
  Assert-CtsQpaComplete $state
  if(-not $modules.ContainsKey($Icd) -or -not $modules.ContainsKey($sys)){throw 'Loaded ICD/loader witness missing'}
  Save "$stem-result" @{status='COMPLETE';counts=$state.Counts;elapsed_ms=$timer.ElapsedMilliseconds;modules=@($modules.Keys)}
  return $state.Finished
 }finally{
  if($child -and -not $child.HasExited){$child.Kill();$null=$child.WaitForExit(2000)}
  if($reader){$reader.Dispose()}
  if($copyOut){$null=$copyOut.Wait(2000)}
  if($copyErr){$null=$copyErr.Wait(2000)}
  $stdout.Dispose();$stderr.Dispose()
  if($child){$child.Dispose()}
 }
}
$finished=0;$number=0
try{
 CheckStop;CheckHealth 'start'
 Save 'start' @{utc=[DateTime]::UtcNow.ToString('o');pid=$PID;manifest_sha256=(Get-FileHash "$BatchDirectory\manifest.json").Hash;max_batches=$MaxBatches;icd_sha256=(Get-FileHash $Icd).Hash;boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')}
 foreach($batch in $manifest.batches){
  if($MaxBatches -and $number -ge $MaxBatches){break}
  $number++;CheckStop;CheckHealth "before-$number"
  $finished+=RunBatch $batch $number
  CheckHealth "after-$number"
  Save 'progress' @{completed_batches=$number;completed_cases=$finished;utc=[DateTime]::UtcNow.ToString('o')}
 }
 if(-not $MaxBatches -and $finished -ne $manifest.case_count){throw 'Incomplete full selection'}
 Save 'result' @{status='COMPLETE';completed_cases=$finished;completed_batches=$number;full_selection=($number -eq $manifest.batches.Count);note='Linux comparison and capability review remain separate'}
}catch{
 Save 'result' @{status='FAIL';batch=$number;completed_cases=$finished;message=$_.ToString();utc=[DateTime]::UtcNow.ToString('o')}
 throw
}

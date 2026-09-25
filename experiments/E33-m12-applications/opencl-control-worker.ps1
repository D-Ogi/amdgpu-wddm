param(
 [Parameter(Mandatory=$true)][string]$Out,
 [Parameter(Mandatory=$true)][string]$Package,
 [Parameter(Mandatory=$true)][string]$VulkanIcd,
 [Parameter(Mandatory=$true)][string]$Cli,
 [string]$DeviceName='AMD BC-250',
 [switch]$Profile
)
$ErrorActionPreference='Stop'
if(Test-Path $Out){throw 'Result directory already exists'}
if(@(Get-ScheduledTask | Where-Object {$_.TaskName -like 'BC250-M12-cts*' -and $_.State -in @('Running','Queued')}).Count){throw 'CTS is active'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
New-Item -ItemType Directory -Path $Out | Out-Null
function Save($name,$value){$value | ConvertTo-Json -Depth 8 | Set-Content "$Out\$name.json" -Encoding UTF8}
function Health($stage){
 foreach($mode in @('health','clock')){
  $path="$Out\$stage-$mode.txt"
  $p=Start-Process $Cli -ArgumentList $mode,'read' -WindowStyle Hidden -PassThru -RedirectStandardOutput $path
  $null=$p.Handle
  try{
   if(-not $p.WaitForExit(4000)){Stop-Process -Id $p.Id -Force;throw 'Telemetry timeout'}
   if($p.ExitCode -ne 0){throw 'Telemetry failed'}
  }finally{$p.Dispose()}
  $text=[IO.File]::ReadAllText($path)
  if($mode -eq 'clock'){
   if($text -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/temperature gate'}
  }else{
   if($text -notmatch 'flags=15 generation=(\d+) epoch=(\d+)'){throw 'GPU health gate'}
   $id=$Matches[1]+':'+$Matches[2]
   if($script:GpuIdentity -and $script:GpuIdentity -ne $id){throw 'GPU identity changed'}
   $script:GpuIdentity=$id
  }
 }
}
$vendor=Join-Path $Package 'clvk.dll'
$key='HKLM:\SOFTWARE\Khronos\OpenCL\Vendors'
$changed=$false;$child=$null;$stdout=$null;$stderr=$null
try{
 $manifest=Get-Content (Join-Path $Package 'control-manifest.json') -Raw | ConvertFrom-Json
 $required=@{}
 foreach($entry in $manifest.files){
  $path=Join-Path $Package $entry.name
  $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
  if($hash -ne $entry.sha256){throw "Hash mismatch: $($entry.name)"}
  if($entry.name -in @('OpenCL.dll','clvk.dll')){$required[$path]=$hash}
 }
 if($required.Count -ne 2){throw 'Missing loader/vendor identities'}
 if((Get-FileHash -LiteralPath $VulkanIcd -Algorithm SHA256).Hash -ne $manifest.vulkan_sha256){throw 'Vulkan identity mismatch'}
 $required[$VulkanIcd]=$manifest.vulkan_sha256
 $systemLoader=Join-Path $env:windir 'System32\vulkan-1.dll'
 $required[$systemLoader]=(Get-FileHash $systemLoader -Algorithm SHA256).Hash
 Save 'identity' @{required_modules=$required;boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o');utc=[DateTime]::UtcNow.ToString('o')}
 Health 'before'
 $existed=$false;$old=$null
 if(Test-Path $key){
  $rk=Get-Item $key
  $existed=$rk.GetValueNames() -contains $vendor
  if($existed){$old=@{value=$rk.GetValue($vendor);kind=$rk.GetValueKind($vendor).ToString()}}
 }
 Save 'registration-before' @{existed=$existed;old=$old;value_name=$vendor}
 if(-not(Test-Path $key)){New-Item -Path $key -Force | Out-Null}
 New-ItemProperty -Path $key -Name $vendor -PropertyType DWord -Value 0 -Force | Out-Null
 $changed=$true
 foreach($n in @('OCL_ICD_FILENAMES','OCL_ICD_VENDORS','VK_DRIVER_FILES','VK_ICD_FILENAMES','VK_ADD_DRIVER_FILES')){Remove-Item "Env:$n" -ErrorAction SilentlyContinue}
 $env:TEMP='C:\BC250\tmp';$env:TMP=$env:TEMP
 $env:BC250_TRACE_SUBMITS='0';$env:RADV_EXPERIMENTAL='sparse'
 $env:PATH=($env:PATH.Split(';') | Where-Object {$_ -notlike 'C:\BC250\*'}) -join ';'
 # Select device profiling only when clvk supports calibrated timers.
 # Forcing queries otherwise reaches an unset timer function.
 Remove-Item Env:CLVK_QUEUE_PROFILING_USE_TIMESTAMP_QUERIES -ErrorAction SilentlyContinue
 # Host-clock fallback assigns batch intervals to every event. This ordering
 # control uses one command per batch; it is not a throughput measurement.
 foreach($n in @('CLVK_MAX_CMD_BATCH_SIZE','CLVK_MAX_FIRST_CMD_BATCH_SIZE')){
  if($Profile){Set-Item "Env:$n" '1'}else{Remove-Item "Env:$n" -ErrorAction SilentlyContinue}
 }
 $env:CLVK_DYNAMIC_BATCHES='0'
 $psi=[Diagnostics.ProcessStartInfo]::new()
 $psi.FileName=Join-Path $Package 'opencl_content_control.exe'
 $psi.Arguments='"'+$DeviceName+'"';if($Profile){$psi.Arguments+=' --profile'};$psi.WorkingDirectory=$Package
 $psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
 $child=[Diagnostics.Process]::Start($psi);$null=$child.Handle
 $stdout=[IO.File]::Create("$Out\control.out");$stderr=[IO.File]::Create("$Out\control.err")
 $copyOut=$child.StandardOutput.BaseStream.CopyToAsync($stdout)
 $copyErr=$child.StandardError.BaseStream.CopyToAsync($stderr)
 $timer=[Diagnostics.Stopwatch]::StartNew();$modules=@{}
 $healthTimer=[Diagnostics.Stopwatch]::StartNew();$healthIndex=0
 while(-not $child.HasExited -and $timer.Elapsed.TotalSeconds -lt 120){
  try{foreach($m in $child.Modules){if($required.ContainsKey($m.FileName)){$modules[$m.FileName]=1}}}catch{}
  if($healthTimer.Elapsed.TotalSeconds -ge 5){
   if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
   $healthIndex++;Health ('during-'+$healthIndex);$healthTimer.Restart()
  }
  Start-Sleep -Milliseconds 20;$child.Refresh()
 }
 if(-not $child.HasExited){Stop-Process -Id $child.Id -Force;throw 'OpenCL control timeout'}
 if(-not $copyOut.Wait(2000) -or -not $copyErr.Wait(2000)){throw 'Output drain timeout'}
 $stdout.Dispose();$stdout=$null;$stderr.Dispose();$stderr=$null
 Save 'process' @{radv_experimental=$env:RADV_EXPERIMENTAL;profile=[bool]$Profile;max_cmd_batch_size=$env:CLVK_MAX_CMD_BATCH_SIZE;max_first_cmd_batch_size=$env:CLVK_MAX_FIRST_CMD_BATCH_SIZE;dynamic_batches=$env:CLVK_DYNAMIC_BATCHES;profiling_policy='automatic timers, single-command ordering control';gpu_queries_override=$env:CLVK_QUEUE_PROFILING_USE_TIMESTAMP_QUERIES;exit_code=$child.ExitCode;elapsed_ms=$timer.ElapsedMilliseconds;modules=@($modules.Keys)}
 if($child.ExitCode -ne 0){throw 'OpenCL control failed'}
 foreach($name in $required.Keys){if(-not $modules.ContainsKey($name)){throw "Missing module witness: $name"}}
 if([IO.File]::ReadAllText("$Out\control.out") -notmatch 'PASS: 4096 map words, 64 group sums, 0 mismatches'){throw 'Missing content result'}
 Health 'after'
 Save 'result' @{status='PASS';utc=[DateTime]::UtcNow.ToString('o')}
}catch{
 Save 'result' @{status='FAIL';message=$_.ToString();utc=[DateTime]::UtcNow.ToString('o')}
 throw
}finally{
 if($child -and -not $child.HasExited){Stop-Process -Id $child.Id -Force}
 if($stdout){$stdout.Dispose()};if($stderr){$stderr.Dispose()}
 if($changed){
  if($existed){New-ItemProperty -Path $key -Name $vendor -PropertyType $old.kind -Value $old.value -Force | Out-Null}
  else{Remove-ItemProperty -Path $key -Name $vendor}
  Save 'registration-restored' @{utc=[DateTime]::UtcNow.ToString('o');original_value_existed=$existed}
 }
}

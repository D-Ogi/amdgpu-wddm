param([string]$Out,[string]$Tools='C:\BC250\m12\system-icd\tools')
$ErrorActionPreference='Stop'
$root=Split-Path $Tools
$icd='C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$sys=Join-Path $env:windir 'System32\vulkan-1.dll'
foreach($name in @('VK_DRIVER_FILES','VK_ICD_FILENAMES','VK_ADD_DRIVER_FILES','VK_LOADER_DRIVERS_SELECT','VK_LOADER_DRIVERS_DISABLE')){Remove-Item "Env:$name" -ErrorAction SilentlyContinue}
$env:VK_LOADER_DEBUG='driver';$env:BC250_TRACE_SUBMITS='0'
$env:PATH=($env:PATH.Split(';') | Where-Object {$_ -notlike 'C:\BC250\*'}) -join ';'
$env:TEMP='C:\BC250\tmp';$env:TMP=$env:TEMP
$child=$null
function Save($name,$value){$value | ConvertTo-Json -Depth 6 | Set-Content "$Out\$name.json" -Encoding UTF8}
function Run($name,$exe,$arguments,$seconds){
 if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
 # Drain bytes on .NET tasks. PowerShell Start-Process line callbacks stalled
 # the large vulkaninfo report in normal/normal-wait; the native-stream control
 # completes on the unchanged ICD/loader with the full report.
 $psi=[Diagnostics.ProcessStartInfo]::new()
 $psi.FileName=$exe;$psi.Arguments=($arguments | ForEach-Object {'"'+$_+'"'}) -join ' '
 $psi.UseShellExecute=$false;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
 $psi.WorkingDirectory=$Tools
 $script:child=[Diagnostics.Process]::Start($psi)
 $handle=$script:child.Handle
 $stdout=[IO.File]::Create("$Out\$name.out");$stderr=[IO.File]::Create("$Out\$name.err")
 $copyOut=$script:child.StandardOutput.BaseStream.CopyToAsync($stdout)
 $copyErr=$script:child.StandardError.BaseStream.CopyToAsync($stderr)
 $modules=@{}
 $timer=[Diagnostics.Stopwatch]::StartNew()
 try{
  while(-not $script:child.HasExited -and $timer.Elapsed.TotalSeconds -lt $seconds){
   try{foreach($module in $script:child.Modules){if($module.ModuleName -match 'vulkan'){$modules[$module.FileName]=1}}}catch{}
   Start-Sleep -Milliseconds 10
   $script:child.Refresh()
  }
  if(-not $script:child.HasExited){Stop-Process -Id $script:child.Id -Force;throw "Timeout $name"}
  if(-not $copyOut.Wait(2000) -or -not $copyErr.Wait(2000)){throw "Output drain timeout $name"}
  $code=$script:child.ExitCode
  Save "$name-process" @{pid=$script:child.Id;exit_code=$code;modules=@($modules.Keys);elapsed_ms=$timer.ElapsedMilliseconds;command=@($exe)+$arguments}
 }finally{$stdout.Dispose();$stderr.Dispose()}
 $script:child=$null
 if($code -ne 0){throw "Native exit $code in $name"}
 if(-not $modules.ContainsKey($icd) -or -not $modules.ContainsKey($sys)){throw "Loaded module witness missing $name"}
 $err=[IO.File]::ReadAllText("$Out\$name.err")
 if($err -notmatch 'C:\\BC250\\m10\\wsi-final\\(?:\.\\)?vulkan_radeon.dll' -or $err -match 'C:\\BC250\\m8\\vulkan_radeon.dll'){throw "Wrong ICD $name"}
 return [IO.File]::ReadAllText("$Out\$name.out")
}
function CheckHealth($index){
 $cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
 $values=@{}
 foreach($mode in @('clock','health')){
  $path="$Out\check-$index-$mode.txt"
  $p=Start-Process $cli -ArgumentList $mode,'read' -WindowStyle Hidden -PassThru -RedirectStandardOutput $path
  $handle=$p.Handle
  try{
   if(-not $p.WaitForExit(4000)){Stop-Process -Id $p.Id -Force;throw "$mode read timeout"}
   if($p.ExitCode -ne 0){throw "$mode read failed"}
  }finally{$p.Dispose()}
  $values[$mode]=[IO.File]::ReadAllText($path)
 }
 if($values.clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/thermal gate'}
 if($values.health -notmatch 'flags=15 generation=(\d+) epoch=(\d+)'){throw 'Health gate'}
 $identity=$Matches[1]+':'+$Matches[2]
 if($script:gpuIdentity -and $script:gpuIdentity -ne $identity){throw 'GPU generation changed'}
 $script:gpuIdentity=$identity
 @{index=$index;utc=[DateTime]::UtcNow.ToString('o');clock=$values.clock;health=$values.health} | ConvertTo-Json -Compress | Add-Content "$Out\health.jsonl" -Encoding UTF8
}
try{
 if(Test-Path "$Out\start.json"){throw 'Existing run'}
 $token=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
 if(-not $token.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'KMD telemetry requires an elevated CTS worker'}
 Save 'start' @{elevated=$true;utc=[DateTime]::UtcNow.ToString('o');pid=$PID;boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')}
 $index=0;$counts=@{Pass=0;NotSupported=0}
 CheckHealth 0
 $healthTimer=[Diagnostics.Stopwatch]::StartNew()
 foreach($case in [IO.File]::ReadAllLines("$Out\cases.txt")){
  $index++;$stem='{0:D5}' -f $index
  if($healthTimer.Elapsed.TotalSeconds -ge 5){CheckHealth $index;$healthTimer.Restart()}
  Save 'active' @{case=$case;index=$index;utc=[DateTime]::UtcNow.ToString('o')}
  $arguments=@("--deqp-case=$case","--deqp-log-filename=$Out\$stem.qpa",'--deqp-watchdog=enable','--deqp-log-images=enable','--deqp-log-shader-sources=enable')
  $null=Run $stem "$Tools\deqp-vk.exe" $arguments 45
  $qpa=[IO.File]::ReadAllText("$Out\$stem.qpa")
  $results=[regex]::Matches($qpa,'<Result StatusCode="([^"]+)">')
  if($results.Count -ne 1){throw "Missing or multiple CTS results: $case"}
  $status=$results[0].Groups[1].Value
  @{index=$index;case=$case;status=$status;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Compress | Add-Content "$Out\cases.jsonl" -Encoding UTF8
  if($status -notin @('Pass','NotSupported')){throw ("CTS {0}: {1}" -f $status,$case)}
  $counts[$status]++
  Save 'progress' @{index=$index;case=$case;counts=$counts;utc=[DateTime]::UtcNow.ToString('o')}
 }
 CheckHealth 'final'
 Save 'result' @{status='COMPLETE';counts=$counts;utc=[DateTime]::UtcNow.ToString('o');note='Development shard only; Linux parity and full must-pass remain open'}
}catch{
 Save 'result' @{status='FAIL';case=$case;index=$index;message=$_.ToString();utc=[DateTime]::UtcNow.ToString('o')}
 throw
}finally{if($child -and -not $child.HasExited){Stop-Process -Id $child.Id -Force}}

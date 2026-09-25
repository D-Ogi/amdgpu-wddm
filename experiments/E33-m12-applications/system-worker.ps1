param([string]$Out,[switch]$NoPresent)
$ErrorActionPreference='Stop'
$root='C:\BC250\m12\system-icd'
$icd='C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$sys=Join-Path $env:windir 'System32\vulkan-1.dll'
$cleared=@()
foreach($name in @('VK_DRIVER_FILES','VK_ICD_FILENAMES','VK_ADD_DRIVER_FILES','VK_LOADER_DRIVERS_SELECT','VK_LOADER_DRIVERS_DISABLE')){
 if(Test-Path "Env:$name"){$cleared+=$name;Remove-Item "Env:$name"}
}
$env:VK_LOADER_DEBUG='driver'
$env:BC250_TRACE_SUBMITS='0'
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
 $psi.WorkingDirectory="$root\tools"
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
try{
 if(Test-Path $Out){throw 'Existing result folder'}
 New-Item -ItemType Directory $Out | Out-Null
 Save 'start' @{utc=[DateTime]::UtcNow.ToString('o');pid=$PID;cleared_overrides=$cleared;elevated=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator);boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')}
 if(Test-Path "$root\tools\vulkan-1.dll"){throw 'Adjacent loader invalidates system test'}
 $null=Run 'vulkaninfo' "$root\tools\vulkaninfo.exe" @('--text','--show-formats') 40
 $compute=Run 'compute' "$root\tools\vkcompute.exe" @('C:\BC250\m8\spv','--runs','3') 120
 if($compute -notmatch '8 test\(s\) run, 0 mismatch\(es\)'){throw 'Compute suite result'}
 $reference=Get-Content C:\BC250\m11\compute-reference.json -Raw | ConvertFrom-Json
 foreach($item in $reference.PSObject.Properties){
  if($compute -notmatch ('(?m)^'+[regex]::Escape($item.Name)+'\s+n=\d+\s+hash=0x'+$item.Value+'\s+cpu_hash=0x'+$item.Value+'\s+match=yes')){throw "Compute reference mismatch $($item.Name)"}
 }
 if(-not $NoPresent){$null=Run 'cube' "$root\tools\vkcube.exe" @('--c','600','--width','640','--height','480','--suppress_popups') 45}
 Save 'result' @{status='PASS';utc=[DateTime]::UtcNow.ToString('o');no_present=[bool]$NoPresent}
}catch{
 if(Test-Path $Out){Save 'result' @{status='FAIL';message=$_.ToString();utc=[DateTime]::UtcNow.ToString('o')}}
 throw
}finally{if($child -and -not $child.HasExited){Stop-Process -Id $child.Id -Force}}

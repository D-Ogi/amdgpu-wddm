param([string]$Out)
$ErrorActionPreference='Stop'
$Package='C:\BC250\m12\zink-stage001\zink-control-package028'
$Cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
if([Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0){throw 'Interactive session required'}
if(Test-Path $Out){throw 'Existing result'}
New-Item -ItemType Directory $Out | Out-Null
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

$p=$null
try{
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
 $manifest=Get-Content "$Package\manifest.json" -Raw | ConvertFrom-Json
 foreach($f in $manifest.files){if((Get-FileHash "$Package\$($f.name)").Hash -ne $f.sha256){throw 'Package hash mismatch'}}
 Health 'before'
 $env:GALLIUM_DRIVER='zink'
 $env:MESA_LOADER_DRIVER_OVERRIDE='zink'
 $env:RADV_EXPERIMENTAL='sparse'
 [Environment]::SetEnvironmentVariable('RADV_DEBUG',$null,'Process')
 $env:MESA_SHADER_CACHE_DISABLE='true'
 [Environment]::SetEnvironmentVariable('PIGLIT_DEFAULT_SIZE',$null,'Process')
 $env:MESA_SHADER_CACHE_DIR="$Out\cache"
 foreach($v in @('MESA_GL_VERSION_OVERRIDE','MESA_GLSL_VERSION_OVERRIDE','LIBGL_ALWAYS_SOFTWARE','VK_ICD_FILENAMES','VK_DRIVER_FILES')){[Environment]::SetEnvironmentVariable($v,$null,'Process')}
 $args="-auto";$env:PIGLIT_PLATFORM='wgl'
 Save 'identity' @{package=$Package;manifest_sha256=(Get-FileHash "$Package\manifest.json").Hash;command=$args;gallium='zink';version_override=$false;boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')}
 $p=Start-Process "$Package\gl-1.0-swapbuffers-behavior.exe" -WorkingDirectory $Package -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Out\stdout.txt" -RedirectStandardError "$Out\stderr.txt"
 $null=$p.Handle;$deadline=[DateTime]::UtcNow.AddSeconds(45);$modules=@{};$last=[DateTime]::UtcNow
 while(-not $p.HasExited){
  try{$p.Refresh();foreach($m in $p.Modules){$modules[$m.FileName]=$true}}catch{}
  if([DateTime]::UtcNow -ge $deadline){throw 'wflinfo timeout'}
  if(([DateTime]::UtcNow-$last).TotalSeconds -ge 5){Health 'during';$last=[DateTime]::UtcNow;if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}}
  Start-Sleep -Milliseconds 10
 }
 $p.WaitForExit();Save 'process' @{exit_code=$p.ExitCode;modules=@($modules.Keys)}
 if($p.ExitCode -ne 0){throw 'wflinfo failed'}
 if((Get-Content "$Out\stdout.txt" -Raw) -notmatch '"result":\s*"pass"'){throw 'Pixel oracle did not pass'}
 Health 'after' 
 Save 'result' @{status='PROBE_COMPLETED';conformance=$false;utc=[DateTime]::UtcNow.ToString('o')}
}catch{Save 'result' @{status='FAIL';message=$_.Exception.Message;utc=[DateTime]::UtcNow.ToString('o')}}
finally{if($p){if(-not $p.HasExited){$p.Kill();$p.WaitForExit()};$p.Dispose()}}

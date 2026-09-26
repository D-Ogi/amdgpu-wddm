param([string]$RunId,[switch]$Control)
$ErrorActionPreference='Stop'
$Root='C:\BC250\m12'
$Package="$Root\piglit-full001"
$Out="$Root\$RunId"
$Cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
if([Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0){throw 'Interactive session required'}
if(Test-Path $Out){throw 'Existing output'}
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

$p=$null;$changed=$false;$b=$null
try{
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
 if(Get-Process deqp-vk -ErrorAction SilentlyContinue){throw 'CTS active'}
 $b=Get-Content "$Root\cts-sparse-full151q\registration-before.json" -Raw | ConvertFrom-Json
 $b.candidate_manifest="$Root\mesa05-ib-collection\radeon_icd.json"
 $names=@((Get-ItemProperty $b.class_path).VulkanDriverName)
 if($names.Count -ne 1 -or $names[0] -ne $b.old_manifest){throw 'Unexpected current ICD'}
 if((Get-Item $b.global_path).GetValue($b.old_manifest) -ne 0){throw 'Baseline disabled'}
 $hashes=@{}
 foreach($pair in @(
  @("$Root\mesa05-ib-collection\vulkan_radeon.dll",'8B5EC055501CD042F723F9EBC9FC29787A6173C61937A739DD7C4C520DBD2AA6'),
  @("$Package\piglit\bin\libgallium_wgl.dll",'DFC11A1421C4DDDB95AB348B1CCFACAC2FD8459E1E80E0F81D4D4C89DB239D24'),
  @("$Package\piglit\bin\opengl32.dll",'2B1FE59C27D4C21B68C7F429FA79F29730F2C6A02BEA7519400B7532FBBCB480'))){
   $hash=(Get-FileHash $pair[0]).Hash
   if($hash -ne $pair[1]){throw 'Artifact hash mismatch'}
   $hashes[$pair[0]]=$hash
 }
 Health 'before'
 Save 'registration-before' $b
 $changed=$true
 New-ItemProperty $b.global_path -Name $b.old_manifest -PropertyType DWord -Value 1 -Force | Out-Null
 New-ItemProperty $b.global_path -Name $b.candidate_manifest -PropertyType DWord -Value 0 -Force | Out-Null
 New-ItemProperty $b.class_path -Name VulkanDriverName -PropertyType MultiString -Value @($b.candidate_manifest) -Force | Out-Null
 $env:PIGLIT_BUILD_DIR="$Package\piglit";$env:PIGLIT_SOURCE_DIR=$env:PIGLIT_BUILD_DIR;$env:PIGLIT_PLATFORM='wgl'
 $env:GALLIUM_DRIVER='zink';$env:MESA_LOADER_DRIVER_OVERRIDE='zink';$env:RADV_EXPERIMENTAL='sparse'
 $env:MESA_SHADER_CACHE_DIR="$Out\cache";$env:BC250_CLI=$Cli;$env:BC250_PIGLIT_EVENTS="$Out\events.jsonl"
 foreach($v in @('MESA_GL_VERSION_OVERRIDE','MESA_GLSL_VERSION_OVERRIDE','LIBGL_ALWAYS_SOFTWARE','VK_ICD_FILENAMES','VK_DRIVER_FILES')){[Environment]::SetEnvironmentVariable($v,$null,'Process')}
 $selection=''
 if($Control){$selection='-t ^fast_color_clear@fcc-blit-between-clears$ '}
 $arguments="$Root\piglit-guard.py ${selection}quick $Out\results"
 Save 'identity' @{hashes=$hashes;command=$arguments;control=[bool]$Control;boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o');guard_sha256=(Get-FileHash "$Root\piglit-guard.py").Hash}
 $p=Start-Process "$Package\python\python.exe" -WorkingDirectory "$Package\piglit" -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Out\stdout.txt" -RedirectStandardError "$Out\stderr.txt"
 $null=$p.Handle;$deadline=[DateTime]::UtcNow.AddHours(10)
 while(-not $p.HasExited){
  if([DateTime]::UtcNow -gt $deadline){throw 'Worker deadline'}
  if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
  Start-Sleep -Seconds 2
 }
 $p.WaitForExit()
 Save 'process' @{exit_code=$p.ExitCode;utc=[DateTime]::UtcNow.ToString('o')}
 if($p.ExitCode -ne 0){throw 'Piglit runner failed; inspect events and upstream results'}
 Health 'after'
 Save 'result' @{status='COMPLETED';control=[bool]$Control;utc=[DateTime]::UtcNow.ToString('o')}
}catch{Save 'result' @{status='FAIL';message=$_.Exception.Message;utc=[DateTime]::UtcNow.ToString('o')}}
finally{
 if($p){if(-not $p.HasExited){& taskkill.exe /PID $p.Id /T /F | Out-Null;$p.WaitForExit(5000) | Out-Null};$p.Dispose()}
 if($changed){
  New-ItemProperty $b.class_path -Name VulkanDriverName -PropertyType MultiString -Value @($b.driver_names) -Force | Out-Null
  New-ItemProperty $b.global_path -Name $b.old_manifest -PropertyType DWord -Value 0 -Force | Out-Null
  Remove-ItemProperty $b.global_path -Name $b.candidate_manifest -ErrorAction Stop
  Save 'registration-restored' @{old_value=(Get-Item $b.global_path).GetValue($b.old_manifest);candidate_present=((Get-Item $b.global_path).GetValueNames() -contains $b.candidate_manifest);driver_names=@((Get-ItemProperty $b.class_path).VulkanDriverName)}
 }
}

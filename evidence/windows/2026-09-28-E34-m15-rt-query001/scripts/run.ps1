param([string]$IcdHash)
$ErrorActionPreference='Stop'
$d=$PSScriptRoot
if((Get-FileHash (Join-Path $d 'cts-direct.dll')).Hash -ne '47C4FF5C32B52A99C322477350EF7457FAFBAA098E6D23822B4CA82B02797CB8'){throw 'Adapter hash'}
$exe='C:\BC250\m12\cts-release-tools\deqp-vk.exe'
if((Get-FileHash $exe).Hash -ne 'AE7BEFDD190EF08E4A715DE0348734879263E1854E017D67865749905A95A2B6'){throw 'CTS hash'}
$icd=Join-Path $d 'amdgpu_wddm_radv.dll'
if((Get-FileHash $icd).Hash -ne $IcdHash){throw 'ICD hash'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'STOP'}
$env:VK_DRIVER_FILES=Join-Path $d 'candidate.json';$env:VK_ICD_FILENAMES=$env:VK_DRIVER_FILES
$env:VK_LOADER_DEBUG='driver';$env:RADV_DEBUG='info,nort';$env:RADV_EXPERIMENTAL=''
$case='dEQP-VK.ray_query.builtin.flow.comp.triangles'
$arguments=@("--deqp-case=$case","--deqp-log-filename=$d\case.qpa","--deqp-vk-library-path=$d\cts-direct.dll",'--deqp-watchdog=enable' )
$watch=[Diagnostics.Stopwatch]::StartNew()
$p=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$d\stdout.txt" -RedirectStandardError "$d\stderr.txt"
$handle=$p.Handle;$seen=$false;$timeout=$false;$lastTemp=-5;$temperature=$null
try {
 while(!$p.HasExited){
  try {foreach($m in $p.Modules){if($m.FileName -ieq $icd){$seen=$true}}} catch {}
  if($watch.Elapsed.TotalSeconds -ge 20){$timeout=$true;break}
  if($watch.Elapsed.TotalSeconds-$lastTemp -ge 5){
   if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 1).stop){throw 'STOP'}
   $raw=(& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1|Out-String)
   if($LASTEXITCODE -ne 0 -or $raw -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)'){throw 'Temperature unavailable'}
   $temperature=[double]$Matches[1];if($temperature -ge 85){throw 'Thermal stop'}
   $lastTemp=$watch.Elapsed.TotalSeconds
  }
  $null=$p.WaitForExit(20);$p.Refresh()
 }
}finally{
 if(!$p.HasExited){$p.Kill();if(!$p.WaitForExit(5000)){throw 'Child termination failed'}}
 $p.Refresh()
 [ordered]@{case=$case;exit=$p.ExitCode;timeout=$timeout;icd_seen=$seen;icd_sha256=$IcdHash;temperature=$temperature;seconds=$watch.Elapsed.TotalSeconds;mode='info,nort';utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json|Set-Content "$d\case.json"
}
if($timeout -or !$seen){throw 'Timeout or missing ICD witness'}
$qpa=Get-Content "$d\case.qpa" -Raw
$results=[regex]::Matches($qpa,'<Result StatusCode="([^"]+)">')
if($results.Count -ne 1 -or $results[0].Groups[1].Value -ne 'Fail'){throw 'Expected one completed negative-control Fail; inspect logs'}

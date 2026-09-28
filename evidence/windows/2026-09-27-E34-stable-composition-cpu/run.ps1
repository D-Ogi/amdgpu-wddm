$ErrorActionPreference='Stop'
$out='C:\BC250\m13\dwm-hosted036'
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
if(Test-Path "$out\start.json"){throw 'Existing run'}
function Snapshot {
 $health=& $cli health read | Out-String
 if($LASTEXITCODE -ne 0 -or $health -notmatch 'version=0x000700A4 flags=15'){throw 'KMD164 health gate'}
 $clock=& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read | Out-String
 if($LASTEXITCODE -ne 0 -or $clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/temperature gate'}
 $image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
 if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
 if($image.StartsWith('\??\')){$image=$image.Substring(4)}
 $kmd=(Get-FileHash $image).Hash
 if($kmd -ne '9B9B99D3F3FA2A32816E71C8754A6BB6349A427C33CCCD1E9B09C08761849743'){throw 'Unexpected KMD SYS'}
 $umd=(Get-FileHash 'C:\BC250\m11\resource-close\bc250d3d.dll').Hash
 $icd=(Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash
 if($umd -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA' -or $icd -ne 'CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157'){throw 'Baseline UMD/ICD mismatch'}
 $os=Get-CimInstance Win32_OperatingSystem
 return @{utc=[DateTime]::UtcNow.ToString('o');boot=$os.LastBootUpTime.ToString('o');health=$health;clock=$clock;kmd=$kmd;umd=$umd;icd=$icd;dwm=@(Get-Process dwm | Select-Object -ExpandProperty Id);free_kib=$os.FreePhysicalMemory;total_kib=$os.TotalVisibleMemorySize}
}


$code=1;$reason='';$p=$null;$before=$null
try {
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
 $manifest=Get-Content "$out\manifest.json" -Raw | ConvertFrom-Json
 foreach($item in $manifest.PSObject.Properties){if((Get-FileHash (Join-Path $out $item.Name)).Hash -ne $item.Value){throw "Artifact mismatch: $($item.Name)"}}
 $before=Snapshot
 $before | ConvertTo-Json -Depth 5 | Set-Content "$out\before.json"
 @{utc=[DateTime]::UtcNow.ToString('o');pid=$PID} | ConvertTo-Json | Set-Content "$out\start.json"
 $p=Start-Process "$out\composition-control.exe" -WindowStyle Normal -PassThru
 $handle=$p.Handle
 @{pid=$p.Id;start=$p.StartTime.ToUniversalTime().ToString('o')} | ConvertTo-Json | Set-Content "$out\process.json"
 & "$out\wait-stable-composition.ps1" -Directory $out -TaskName 'BC250-G0-Composition036'
 if(!(Test-Path "$out\baseline-ready.json")){throw 'Baseline not ready'}
 Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$out\screen.png" -TimeoutSec 5
 $initial=Get-Content "$out\control-heartbeat.json" -Raw | ConvertFrom-Json
 $initial | ConvertTo-Json -Depth 5 | Set-Content "$out\initial-heartbeat.json"
 $check=& "$out\check-stable-composition.ps1" -Image "$out\screen.png" -Heartbeat "$out\initial-heartbeat.json"
 $check | ConvertTo-Json -Depth 5 | Set-Content "$out\screen-check.json"
 if(!$check.pass){throw 'Independent screenshot failed'}
 [IO.File]::WriteAllText("$out\animate",[DateTime]::UtcNow.ToString('o'))
 $timer=[Diagnostics.Stopwatch]::StartNew();$moving=$null
 while($timer.Elapsed.TotalSeconds -lt 8){
  if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
  if($p.HasExited){throw 'Control exited early'}
  $moving=Get-Content "$out\control-heartbeat.json" -Raw | ConvertFrom-Json
  if($moving.animating -eq $true -and $moving.frames -ge 20 -and ($moving.moving_client -join ',') -ne ($initial.moving_client -join ',')){break}
  Start-Sleep -Milliseconds 200
 }
 $moving | ConvertTo-Json -Depth 5 | Set-Content "$out\moving-heartbeat.json"
 if(!$moving.animating -or $moving.frames -lt 20 -or ($moving.moving_client -join ',') -eq ($initial.moving_client -join ',')){throw 'Animation was not witnessed'}
 [IO.File]::WriteAllText("$out\freeze",[DateTime]::UtcNow.ToString('o'))
 $timer.Restart();$frozen=$null
 while($timer.Elapsed.TotalSeconds -lt 5){
  $frozen=Get-Content "$out\control-heartbeat.json" -Raw | ConvertFrom-Json
  if($frozen.frozen -eq $true -and $frozen.freeze_result -eq 0){break}
  Start-Sleep -Milliseconds 100
 }
 $frozen | ConvertTo-Json -Depth 5 | Set-Content "$out\freeze-receipt.json"
 if(!$frozen.frozen -or $frozen.freeze_result -ne 0){throw 'Freeze not acknowledged'}
 $code=0
} catch {$reason=$_.ToString()+' '+$_.ScriptStackTrace} finally {
 if($p -and !$p.HasExited){$p.Kill();[void]$p.WaitForExit(5000)}
 try {
  $after=Snapshot
  $after | ConvertTo-Json -Depth 5 | Set-Content "$out\after.json"
  if($before -and ($after.boot -ne $before.boot -or (Compare-Object $before.dwm $after.dwm))){$code=1;$reason+=' Identity changed'}
 } catch {$code=1;$reason+=' Closure: '+$_.Exception.Message}
 @{utc=[DateTime]::UtcNow.ToString('o');exit_code=$code;reason=$reason;process_alive=($p -and !$p.HasExited)} | ConvertTo-Json | Set-Content "$out\done.json"
}
exit $code

$ErrorActionPreference='Stop'
$out='C:\BC250\m9\candidate07145\coldboot-01'
$path=Join-Path $out 'startup-health.log'
if(Test-Path $path){exit 20}
$stream=[IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
$writer=New-Object IO.StreamWriter($stream,(New-Object Text.UTF8Encoding($false)))
$writer.AutoFlush=$true
function Note([string]$s){$writer.WriteLine((Get-Date).ToString('o')+' '+$s);$writer.Flush();$stream.Flush($true)}
$cli='C:\BC250\m9\candidate07145\client\bc250kmd_cli.exe'
try {
 Note ('boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o'))
 $cliHash=(Get-FileHash $cli).Hash
 Note ('cli_sha256='+$cliHash)
 if($cliHash -ne '68B346AFFF020631F91B1C26F2B51A656762346D53D6B168992F7A97CF9E3FD4'){throw 'CLI mismatch'}
 $watch=[Diagnostics.Stopwatch]::StartNew()
 while($watch.Elapsed.TotalSeconds -lt 240){
  $r=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
  $health=& $cli health read 2>&1 | Out-String
  $code=$LASTEXITCODE
  Note ('budget='+$r.UnconfirmedStarts+' policy='+$r.EnableFullWddm+' exit='+$code+' '+$health.Trim())
  if($code -eq 0 -and $r.UnconfirmedStarts -eq 0 -and $health -match 'flags=15 '){
   Note 'automatic_confirmation_observed'
   & $cli log | Out-File (Join-Path $out 'confirmed-driver.log') -Encoding UTF8
   Note ('driver_log_exit='+$LASTEXITCODE)
   break
  }
  Start-Sleep -Seconds 5
 }
 Note ('recorder_finished_elapsed_s='+[int]$watch.Elapsed.TotalSeconds)
} catch {Note ('recorder_error='+$_.Exception.Message);exit 1}
finally{$writer.Dispose();$stream.Dispose()}

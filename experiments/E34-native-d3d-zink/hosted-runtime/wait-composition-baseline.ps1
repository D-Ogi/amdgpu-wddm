param([Parameter(Mandatory)][ValidatePattern('^C:\\BC250\\m13\\dwm-hosted[0-9]{3}$')][string]$Directory,
 [Parameter(Mandatory)][ValidatePattern('^BC250-G0-Composition[0-9]{3}$')][string]$TaskName)
$ErrorActionPreference='Stop'
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
if(Test-Path "$Directory\baseline.bmp"){throw 'Baseline already exists; preserve evidence'}
$clock=[Diagnostics.Stopwatch]::StartNew()
$attempt=0
while($clock.Elapsed.TotalSeconds -lt 30){
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
 if((Get-ScheduledTask -TaskName $TaskName).State -ne 'Running'){throw 'Composition task not running'}
 $attempt++;$prefix=Join-Path $Directory ('baseline-attempt-{0:D2}' -f $attempt)
 if(Test-Path "$prefix.json"){throw 'Readiness receipts already exist'}
 $heartbeat=$null
 if(Test-Path "$Directory\control-heartbeat.json"){
  $heartbeat=Get-Content "$Directory\control-heartbeat.json" -Raw | ConvertFrom-Json
 }
 $painted=$heartbeat -and $heartbeat.paint_red -gt 0 -and $heartbeat.paint_blue -gt 0 -and $heartbeat.paint_moving -gt 0
 $receipt=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');attempt=$attempt;elapsed=$clock.Elapsed.TotalSeconds;painted=[bool]$painted;heartbeat=$heartbeat}
 if($painted){
  # Keep every capture. A failed positive control is not silently overwritten.
  $reader=Start-Process -FilePath $cli -ArgumentList @('fbdump',"$prefix.bmp") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$prefix.log" -RedirectStandardError "$prefix.err"
  $handle=$reader.Handle
  $receipt.reader=@{pid=$reader.Id;start=$reader.StartTime.ToUniversalTime().ToString('o')}
  if(-not $reader.WaitForExit(5000)){
   $receipt.timeout=$true
   $receipt | ConvertTo-Json -Depth 6 | Set-Content "$prefix.json"
   Stop-Process -Id $reader.Id -ErrorAction SilentlyContinue
   throw 'Baseline capture timed out; inspect reader, no GPU transition'
  }
  $reader.Refresh()
  $receipt.exit_code=$reader.ExitCode
  if($reader.ExitCode -ne 0){
   $receipt | ConvertTo-Json -Depth 6 | Set-Content "$prefix.json"
   throw 'Baseline capture failed'
  }
  $check=& "$Directory\check-composition-baseline.ps1" -Image "$prefix.bmp"
  $receipt.image=$check
  $receipt | ConvertTo-Json -Depth 6 | Set-Content "$prefix.json"
  if($check.pass){
   Copy-Item -LiteralPath "$prefix.bmp" -Destination "$Directory\baseline.bmp"
   $receipt | ConvertTo-Json -Depth 6 | Set-Content "$Directory\baseline-ready.json"
   return
  }
 }else{$receipt | ConvertTo-Json -Depth 6 | Set-Content "$prefix.json"}
 Start-Sleep -Milliseconds 500
}
throw 'No valid CPU composition baseline within30 seconds; no GPU transition'

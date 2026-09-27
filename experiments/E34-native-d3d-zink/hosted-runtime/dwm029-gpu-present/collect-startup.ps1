$ErrorActionPreference='Stop'
$out='C:\BC250\m13\dwm-hosted029'
. "$PSScriptRoot\durable.ps1"
$stopAt=[DateTime]::UtcNow.AddMinutes(3)
$i=0
$timedOut=$false
try {
 while([DateTime]::UtcNow -lt $stopAt){
  $i++
  $file=Join-Path $out ('startup-log-{0:D3}.txt' -f $i)
  $errorFile=Join-Path $out ('startup-log-{0:D3}.err' -f $i)
  @{utc=[DateTime]::UtcNow.ToString('o');sample=$i} | ConvertTo-Json | Set-Content ($file+'.json')
  $reader=Start-Process -FilePath 'C:\BC250\m8\bc250kmd_cli.exe' -ArgumentList @('log','summary') -WindowStyle Hidden -PassThru -RedirectStandardOutput $file -RedirectStandardError $errorFile
  if(-not $reader.WaitForExit(3000)){
   $timedOut=$true
   @{pid=$reader.Id;start=$reader.StartTime.ToUniversalTime().ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $out 'reader-timeout.json')
   Stop-Process -Id $reader.Id -ErrorAction SilentlyContinue
   break
  }
  Flush-ExistingFile $file
  Flush-ExistingFile $errorFile
  Flush-ExistingFile ($file+'.json')
  Start-Sleep -Seconds 1
 }
} finally {
 @{utc=[DateTime]::UtcNow.ToString('o');samples=$i;reader_timeout=$timedOut} | ConvertTo-Json | Set-Content (Join-Path $out 'collector-done.json')
}

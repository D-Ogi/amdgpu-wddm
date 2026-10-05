$ErrorActionPreference='Stop'
$out='C:\BC250\m12\candidate07157'
for($attempt=1;$attempt -le 30;$attempt++){
 $stdout=Join-Path $out ('ready-v2-{0:D2}.txt' -f $attempt)
 $stderr=Join-Path $out ('ready-v2-{0:D2}.err' -f $attempt)
 if(Test-Path $stdout){throw 'Existing readiness evidence; inspect before rerun'}
 $p=Start-Process -FilePath 'C:\BC250\m8\bc250kmd_cli.exe' -ArgumentList 'info' -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
 $processHandle=$p.Handle
 if(-not $p.WaitForExit(3000)){
  Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
  throw 'Adapter info reader timeout; inspect process and device'
 }
 $p.Refresh()
 $info=Get-Content $stdout -Raw
 if($p.ExitCode -eq 0 -and $info -match '0x0007009D' -and $info -match 'FULL WDDM TABLE'){
  'KMD157 interface ready after attempt='+$attempt
  return
 }
 Start-Sleep -Seconds 1
}
throw 'KMD157 interface did not become ready; preserve evidence and inspect'

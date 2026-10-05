$ErrorActionPreference='Stop'
$out='C:\BC250\m12\candidate07160'
for($attempt=1;$attempt -le 30;$attempt++){
 $stdout=Join-Path $out ('rollback-ready-{0:D2}.txt' -f $attempt)
 $stderr=Join-Path $out ('rollback-ready-{0:D2}.err' -f $attempt)
 if(Test-Path $stdout){throw 'Existing readiness evidence; inspect before rerun'}
 $p=Start-Process -FilePath 'C:\BC250\m8\bc250kmd_cli.exe' -ArgumentList 'info' -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
 $processHandle=$p.Handle
 if(-not $p.WaitForExit(3000)){
  Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
  throw 'Adapter info reader timeout; inspect process and device'
 }
 $p.Refresh()
 $info=Get-Content $stdout -Raw
 if($p.ExitCode -eq 0 -and $info -match '0x0007009F' -and $info -match 'FULL WDDM TABLE'){
  'KMD159 interface ready after attempt='+$attempt
  return
 }
 Start-Sleep -Seconds 1
}
throw 'KMD159 interface did not become ready; preserve evidence and inspect'

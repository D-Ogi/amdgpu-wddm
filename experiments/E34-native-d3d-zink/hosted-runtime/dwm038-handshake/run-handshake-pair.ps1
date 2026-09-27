$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted038'
. "$d\durable.ps1"
$code=1;$failure=''
try {
 if(Test-Path "$d\handshake-started.json"){throw 'Existing handshake pair'}
 $self=Get-Process -Id $PID
 Write-DurableText "$d\handshake-started.json" (@{utc=[DateTime]::UtcNow.ToString('o');pid=$PID;start=$self.StartTime.ToUniversalTime().ToString('o')}|ConvertTo-Json)
 $identity=Get-Content "$d\handshake-dwm.json" -Raw|ConvertFrom-Json
 foreach($case in 'no-paint','gdi-paint'){
  $dir=Join-Path $d $case
  if(Test-Path $dir){throw 'Case already exists'}
  New-Item -ItemType Directory $dir|Out-Null
  Copy-Item "$d\redirblt-probe.exe" "$dir\redirblt-probe.exe"
  $params=@{Directory=$dir;ProbeSha256='0DC75F8C8BDD05ED332322FD862A6634715A3C769FB0FBB520A35917F51181A4';DwmUmdSha256='5C74BF98F2362127173C53F7808950D4B6676E1B1C901A341163F7032AE78965';ExpectedDwmPid=$identity.pid;ExpectedDwmStartUtc=$identity.start;GdiPaint=($case -eq 'gdi-paint')}
  & "$d\run-handshake-observation.ps1" @params
  if($LASTEXITCODE -ne 0){throw "Worker failed: $case"}
  $done=Get-Content "$dir\done.json" -Raw|ConvertFrom-Json
  if($done.exit_code -ne 0 -or $done.process_alive){throw "Worker not cleanly terminal: $case"}
 }
 $code=0
}catch{$failure=$_.ToString()+' '+$_.ScriptStackTrace}
finally{
 Write-DurableText "$d\handshake-done.json" (@{utc=[DateTime]::UtcNow.ToString('o');exit_code=$code;failure=$failure}|ConvertTo-Json)
}
exit $code

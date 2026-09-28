$ErrorActionPreference='Stop'
$d=$PSScriptRoot
. "$d\durable.ps1"
. "$d\native-child-exit.ps1"
$code=125
try {
 if((Get-Process -Id $PID).SessionId -eq 0){throw 'Interactive session required'}
 $p=Start-Process -FilePath "$d\gpu-window-control.exe" -ArgumentList @("$d\gpu-client",'--lower') -WorkingDirectory $d -WindowStyle Hidden -RedirectStandardOutput "$d\client-stdout.log" -RedirectStandardError "$d\client-stderr.log" -PassThru
 Write-DurableText "$d\client-process.json" (@{pid=$p.Id;start=$p.StartTime.ToUniversalTime().ToString('o')}|ConvertTo-Json)
 $code=Wait-NativeChildExit $p 125000
}catch{$_|Out-String|Add-Content "$d\client-worker.log"}
finally{Write-DurableText "$d\client-done.json" (@{exit=$code;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)}
exit $code

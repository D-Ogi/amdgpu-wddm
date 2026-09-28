$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted033'
$name='BC250G0ProcessControl033'
if(Test-Path "$d\process-control.etl"){throw 'Existing control; inspect original'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
$baseline=@(Get-Process dwm | Select-Object -ExpandProperty Id)
& logman start $name -ets -o "$d\process-control.etl" -pf "$d\etw-providers.txt" -f bin -bs 1024 -nb 16 64 *> "$d\process-control-start.log"
if($LASTEXITCODE -ne 0){throw 'Control ETW start failed'}
try {
 $utc=[DateTime]::UtcNow.ToString('o')
 $p=Start-Process "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList @('-NoProfile','-Command','Start-Sleep -Milliseconds 800') -PassThru -WindowStyle Hidden
 $idValue=$p.Id
 if(!$p.WaitForExit(10000)){throw 'Control process still live; inspect original'}
 @{utc=$utc;pid=$idValue;exit_code=$p.ExitCode;baseline_dwm=$baseline;after_dwm=@(Get-Process dwm | Select-Object -ExpandProperty Id)} | ConvertTo-Json | Set-Content "$d\process-control.json"
} finally {
 & logman stop $name -ets *> "$d\process-control-stop.log"
 if($LASTEXITCODE -ne 0){throw 'Control ETW stop failed'}
}
Get-Content "$d\process-control.json"

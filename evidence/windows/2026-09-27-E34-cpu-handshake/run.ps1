$ErrorActionPreference='Stop'
$out='C:\BC250\m13\redirblt-cpu001'
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
Start-Transcript -Path "$out\run.log" | Out-Null
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



$trace=$false;$code=1;$failure='';$before=$null
try {
 if(Test-Path "$out\start.json"){throw 'Existing run'}
 @{utc=[DateTime]::UtcNow.ToString('o');pid=$PID} | ConvertTo-Json | Set-Content "$out\start.json"
 $manifest=Get-Content "$out\manifest.json" -Raw | ConvertFrom-Json
 foreach($prop in $manifest.PSObject.Properties){if((Get-FileHash "$out\$($prop.Name)").Hash -ne $prop.Value){throw 'Stage hash mismatch'}}
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
 $before=Snapshot;$before|ConvertTo-Json -Depth 5|Set-Content "$out\before.json"
 $dwm=@(Get-Process dwm);if($dwm.Count -ne 1){throw 'Ambiguous DWM'}
 $identity=@{pid=$dwm[0].Id;start=$dwm[0].StartTime.ToUniversalTime().ToString('o')}
 $identity|ConvertTo-Json|Set-Content "$out\dwm-identity.json"
 & logman start BC250RedirCpu001 -ets -o "$out\gpu.etl" -pf "$out\etw-providers.txt" -f bin -bs 1024 -nb 32 128 *> "$out\etw-start.log"
 if($LASTEXITCODE -ne 0){throw 'ETW start failed'};$trace=$true
 foreach($case in 'no-paint','gdi-paint'){
  $dir=Join-Path $out $case
  if(Test-Path $dir){throw 'Case already exists'}
  New-Item -ItemType Directory $dir|Out-Null
  Copy-Item "$out\redirblt-probe.exe" "$dir\redirblt-probe.exe"
  $args=@{Directory=$dir;ProbeSha256='0DC75F8C8BDD05ED332322FD862A6634715A3C769FB0FBB520A35917F51181A4';DwmUmdSha256='8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA';ExpectedDwmPid=$identity.pid;ExpectedDwmStartUtc=$identity.start;GdiPaint=($case -eq 'gdi-paint')}
  & "$out\run-handshake-observation.ps1" @args
  if($LASTEXITCODE -ne 0){throw "Worker failed: $case"}
  $done=Get-Content "$dir\done.json" -Raw|ConvertFrom-Json
  if($done.exit_code -ne 0 -or $done.process_alive){throw "Worker not cleanly terminal: $case"}
 }
 $code=0
}catch{$failure=$_.ToString()+' '+$_.ScriptStackTrace}
finally{
 if($trace){& logman stop BC250RedirCpu001 -ets *> "$out\etw-stop.log"}
 try{
  $after=Snapshot;$after|ConvertTo-Json -Depth 5|Set-Content "$out\after.json"
  if($before -and ($before.boot -ne $after.boot -or (Compare-Object $before.dwm $after.dwm))){$code=1;$failure+=' DWM or boot changed'}
 }catch{$code=1;$failure+=' Closure: '+$_.Exception.Message}
 @{utc=[DateTime]::UtcNow.ToString('o');exit_code=$code;failure=$failure}|ConvertTo-Json|Set-Content "$out\done.json"
 Stop-Transcript|Out-Null
}
exit $code

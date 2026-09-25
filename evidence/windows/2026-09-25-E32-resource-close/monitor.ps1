param([string]$Out,[string]$WorkerTask)
$ErrorActionPreference='Stop'
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
$tags=@('25Kd','B2Cg','B2Gm','B2Gx','B2Ih','B2Pw','B2Sp','B2Vs','B2Ww')
$start=Get-Date
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
$generation=$null
$epoch=$null
$lastCheck=0
$lastAudit=[DateTime]::MinValue
$lastIntegrity=Get-Date
function SaveJson($name,$obj){
 $path=Join-Path $Out $name
 [IO.File]::WriteAllText("$path.tmp",($obj | ConvertTo-Json -Depth 8 -Compress),[Text.UTF8Encoding]::new($false))
 if([IO.File]::Exists($path)){[IO.File]::Replace("$path.tmp",$path,[NullString]::Value)}else{[IO.File]::Move("$path.tmp",$path)}
}
function NativeRead($name,$arguments){
 $p=Start-Process -FilePath $cli -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Out\$name-current.txt" -RedirectStandardError "$Out\$name-current.err"
 $keep=$p.Handle
 if(-not $p.WaitForExit(4000)){Stop-Process -Id $p.Id -Force;throw "$name read timed out"}
 if($p.ExitCode -ne 0){throw "$name read failed"}
 return [IO.File]::ReadAllText("$Out\$name-current.txt")
}
function Pool($label){
 $file="$Out\pool-current.txt"
 [IO.File]::WriteAllText($file,'')
 & C:\BC250\m11\poolmon.exe /n $file | Out-Null
 if($LASTEXITCODE -ne 0){throw 'PoolMon snapshot failed'}
 $rows=@()
 $selected=@()
 foreach($line in Get-Content $file){
  if($line -match '^\s*(\S{4})\s+(Paged|Nonp)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)'){
   if($tags -contains $Matches[1]){
    $selected+=$line
    $rows+=@{tag=$Matches[1];type=$Matches[2];allocs=[long]$Matches[3];frees=[long]$Matches[4];count=[long]$Matches[5];bytes=[long]$Matches[6]}
   }
  }
 }
 if(@($rows | Where-Object {$_.tag -eq 'B2Ww'}).Count -eq 0){throw 'Expected pool tag missing'}
 $selected | Set-Content "$Out\pool-$label.txt" -Encoding ASCII
 @{label=$label;utc=[DateTime]::UtcNow.ToString('o');rows=$rows} | ConvertTo-Json -Depth 8 -Compress | Add-Content "$Out\pool.jsonl" -Encoding UTF8
}
function DumpMetadata {
 $paths=@('C:\Windows\LiveKernelReports','C:\Windows\Minidump')
 $rows=@()
 foreach($path in $paths){
  if(Test-Path $path){$rows+=@(Get-ChildItem $path -Recurse -File -Filter '*.dmp' -ErrorAction Stop | ForEach-Object {$_.FullName+'|'+$_.Length+'|'+$_.LastWriteTimeUtc.Ticks})}
 }
 return $rows
}
function Integrity {
 foreach($artifact in (Get-Content "$Out\inputs.json" -Raw | ConvertFrom-Json)){
  if((Get-FileHash -LiteralPath $artifact.Path).Hash -ne $artifact.Hash){throw "Input changed: $($artifact.Path)"}
 }
}
function Audit {
 if((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o') -ne $boot){throw 'Boot changed'}
 $events=@(Get-WinEvent -FilterHashtable @{LogName='System';StartTime=$start;Id=@(4101,1001,41)} -ErrorAction SilentlyContinue | Where-Object {
  ($_.Id -eq 4101 -and $_.ProviderName -eq 'Display') -or $_.ProviderName -match 'WER-SystemErrorReporting|Kernel-Power'
 })
 $wer=@(Get-WinEvent -FilterHashtable @{LogName='Application';StartTime=$start;Id=1001} -ErrorAction SilentlyContinue | Where-Object {$_.Message -match 'LiveKernelEvent'})
 if($events.Count -or $wer.Count){@($events)+@($wer) | Select-Object TimeCreated,Id,ProviderName,Message | ConvertTo-Json -Depth 6 | Set-Content "$Out\failure-events.json";throw 'New TDR/bugcheck/kernel report event'}
 $now=@(DumpMetadata)
 if((($dumpBaseline | Sort-Object) -join "|") -cne (($now | Sort-Object) -join "|")){$now | Set-Content "$Out\failure-dumps.txt";throw 'Kernel dump inventory changed'}
}
try {
 $dumpBaseline=@(DumpMetadata)
 $dumpBaseline | ConvertTo-Json | Set-Content "$Out\dump-baseline.json"
 Pool 'initial'
 SaveJson 'monitor-ready.json' @{pid=$PID;utc=[DateTime]::UtcNow.ToString('o');boot=$boot}
 while($true) {
  if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
  $clock=NativeRead 'clock' @('clock','read')
  if($clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1'){throw 'Clock/telemetry invalid'}
  $temp=[int]$Matches[1]
  if($temp -ge 85000){throw "Temperature limit $temp"}
  $health=NativeRead 'health' @('health','read')
  if($health -notmatch 'flags=15 generation=(\d+) epoch=(\d+) completed=(\d+)'){throw 'GPU not healthy'}
  if($null -eq $generation){$generation=$Matches[1];$epoch=$Matches[2]}
  if($Matches[1] -ne $generation -or $Matches[2] -ne $epoch){throw 'GPU generation/epoch changed'}
  @{utc=[DateTime]::UtcNow.ToString('o');pid=$PID;temperature_mc=$temp;completed=$Matches[3];generation=$generation;epoch=$epoch} | ConvertTo-Json -Compress | Add-Content "$Out\telemetry.jsonl" -Encoding UTF8
  SaveJson 'monitor-heartbeat.json' @{pid=$PID;utc=[DateTime]::UtcNow.ToString('o');temperature_mc=$temp;checkpoint=$lastCheck}
  if((Get-Date)-$lastAudit -gt [TimeSpan]::FromSeconds(60)){Audit;$lastAudit=Get-Date}
  if((Get-Date)-$lastIntegrity -gt [TimeSpan]::FromHours(1)){Integrity;$lastIntegrity=Get-Date}
  if(Test-Path "$Out\checkpoint.request"){
   $request=[int][IO.File]::ReadAllText("$Out\checkpoint.request")
   if($request -gt $lastCheck){
    Start-Sleep -Milliseconds 500
    Pool ('{0:D6}' -f $request)
    Audit
    $lastCheck=$request
    [IO.File]::WriteAllText("$Out\checkpoint.ack",[string]$request)
   }
  }
  if(Test-Path "$Out\worker-result.json"){
   $result=Get-Content "$Out\worker-result.json" -Raw | ConvertFrom-Json
   if($result.status -ne 'PASS'){throw ('Worker failed: '+$result.message)}
   Integrity
   foreach($i in 1..5){Start-Sleep -Seconds 2;Pool "final-$i"}
   Audit
   SaveJson 'monitor-result.json' @{status='PASS';cycles=$lastCheck;utc=[DateTime]::UtcNow.ToString('o');note='Pool acceptance requires analysis of all samples'}
   break
  }
  if((Get-Date)-$start -gt [TimeSpan]::FromSeconds(60) -and -not (Test-Path "$Out\worker-start.json")){throw 'Worker did not start'}
  if((Test-Path "$Out\worker-start.json") -and -not (Test-Path "$Out\worker-result.json") -and (Get-ScheduledTask $WorkerTask).State -ne 'Running'){throw 'Worker task stopped without result'}
  if((Get-Date)-$start -gt [TimeSpan]::FromHours(25)){throw 'Monitor duration limit'}
  Start-Sleep -Seconds 4
 }
} catch {
 [IO.File]::WriteAllText("$Out\stop.txt",$_.ToString())
 SaveJson 'monitor-result.json' @{status='FAIL';message=$_.ToString();utc=[DateTime]::UtcNow.ToString('o')}
 if(Test-Path "$Out\active.json"){
  $active=Get-Content "$Out\active.json" -Raw | ConvertFrom-Json
  if($active.pid -gt 0){
   $p=Get-Process -Id $active.pid -ErrorAction SilentlyContinue
   if($p -and $p.Path -eq $active.exe){Stop-Process -Id $p.Id -Force}
  }
 }
 Stop-ScheduledTask -TaskName $WorkerTask -ErrorAction SilentlyContinue
 exit 1
}

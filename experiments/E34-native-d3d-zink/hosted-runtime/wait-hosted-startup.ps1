param([Parameter(Mandatory)][ValidatePattern('^C:\\BC250\\m13\\dwm-hosted[0-9]{3}$')][string]$Directory,
 [Parameter(Mandatory)][int[]]$PreviousPids,
 [Parameter(Mandatory)][ValidatePattern('^BC250-G0-Composition[0-9]{3}$')][string]$TaskName)
$ErrorActionPreference='Stop'
. "$Directory\durable.ps1"
. "$Directory\hosted-startup-witness.ps1"
if(Test-Path "$Directory\hosted-ready.json"){throw 'Existing readiness evidence; inspect original attempt'}
$cfg=Get-Content "$Directory\manifest.json" -Raw | ConvertFrom-Json
$expected=@($cfg.'router.dll',$cfg.'bc250d3d_zink.dll',$cfg.'vulkan_radeon.dll')
$hashCache=@{}
$clock=[Diagnostics.Stopwatch]::StartNew()
$read={
 $stop=[bool](Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop
 if((Get-ScheduledTask -TaskName $TaskName).State -ne 'Running'){throw 'Composition task ended during hosted startup'}
 $processes=@(Get-Process dwm | Where-Object {$_.Id -notin $PreviousPids})
 if($processes.Count -gt 1){throw 'Ambiguous new DWM identity'}
 if(!$processes.Count){return @{identity=$null;hashes=@();create_success=$false;stop=$stop}}
 $process=$processes[0]
 $modules=@(foreach($m in $process.Modules){
  if($m.ModuleName -notmatch 'bc250|vulkan_radeon'){continue}
  # Trial artifacts are immutable until rollback; hash each observed path once.
  if(!$hashCache.ContainsKey($m.FileName)){$hashCache[$m.FileName]=(Get-FileHash -LiteralPath $m.FileName).Hash}
  @{name=$m.ModuleName;path=$m.FileName;sha256=$hashCache[$m.FileName]}
 })
 $log=Join-Path $Directory ('dwm-'+$process.Id+'.log')
 $text=if(Test-Path $log){[IO.File]::ReadAllText($log)}else{''}
 return @{utc=[DateTime]::UtcNow.ToString('o');identity=($process.Id.ToString()+'/'+$process.StartTime.ToUniversalTime().ToString('o'));pid=$process.Id;
  hashes=@($modules | ForEach-Object {$_.sha256});modules=$modules;create_success=($text -match 'DWM CreateDevice hr=00000000');stop=$stop;
  log_bytes=if(Test-Path $log){(Get-Item $log).Length}else{0}}
}.GetNewClosure()
$writeDurable=${function:Write-DurableText}
$record={param($receipt)
 & $writeDurable (Join-Path $Directory ('hosted-startup-{0:D3}.json' -f $receipt.attempt)) ($receipt | ConvertTo-Json -Depth 6)
}.GetNewClosure()
$milliseconds={return $clock.ElapsedMilliseconds}.GetNewClosure()
$ready=Wait-HostedDwmWitness $expected $read $record $milliseconds {Start-Sleep -Milliseconds 500}
Write-DurableText "$Directory\hosted-ready.json" ($ready | ConvertTo-Json -Depth 6)
$ready

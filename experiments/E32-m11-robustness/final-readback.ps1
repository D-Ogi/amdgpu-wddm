param([Parameter(Mandatory=$true)][string]$Out)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
# Read-only final observation. Never restarts a device, OS, test, or service.
$worker=Get-Content "$Out\worker-result.json" -Raw | ConvertFrom-Json
$monitor=Get-Content "$Out\monitor-result.json" -Raw | ConvertFrom-Json
$launch=Get-Content "$Out\launch.json" -Raw | ConvertFrom-Json
foreach($task in @($launch.worker_task,$launch.monitor_task)){
 if((Get-ScheduledTask $task).State -in @('Running','Queued')){throw 'Run still active'}
}
if(Test-Path "$Out\final-readback.json"){throw 'Final readback already exists; preserve it'}
$started=Get-Content "$Out\worker-start.json" -Raw | ConvertFrom-Json
$ready=Get-Content "$Out\monitor-ready.json" -Raw | ConvertFrom-Json
$since=[DateTimeOffset]::Parse($started.utc).LocalDateTime
function Events($log,$ids){
 try { return @(Get-WinEvent -FilterHashtable @{LogName=$log;StartTime=$since;Id=$ids} -ErrorAction Stop) }
 catch {
  if($_.FullyQualifiedErrorId -like 'NoMatchingEventsFound*'){return @()}
  throw
 }
}
$logChecks=@()
foreach($name in @('System','Application')){
 $log=Get-WinEvent -ListLog $name
 if(-not $log.IsEnabled){throw "Event log disabled: $name"}
 $entry=Get-WinEvent -LogName $name -MaxEvents 1
 if(-not $entry){throw "No positive event-log read: $name"}
 $logChecks+=@{name=$name;record_id=$entry.RecordId;readable=$true;enabled=$true}
}
$system=@(Events 'System' @(4101,1001,41) | Where-Object {
 ($_.Id -eq 4101 -and $_.ProviderName -eq 'Display') -or $_.ProviderName -match 'WER-SystemErrorReporting|Kernel-Power'
})
$application=@(Events 'Application' @(1001) | Where-Object {$_.Message -match 'LiveKernelEvent'})
$events=@($system)+@($application)
$dumpNow=@()
foreach($path in @('C:\Windows\LiveKernelReports','C:\Windows\Minidump')){
 if(Test-Path $path){
  $dumpNow+=@(Get-ChildItem $path -Recurse -File -Filter '*.dmp' -ErrorAction Stop | ForEach-Object {$_.FullName+'|'+$_.Length+'|'+$_.LastWriteTimeUtc.Ticks})
 }
}
$baseline=@()
if(Test-Path "$Out\dump-baseline.json"){
 $raw=Get-Content "$Out\dump-baseline.json" -Raw
 if($raw -and $raw.Trim()){$baseline=@($raw | ConvertFrom-Json)}
}
$dumpEqual=(($baseline | Sort-Object) -join '|') -ceq (($dumpNow | Sort-Object) -join '|')
$inputs=Get-Content "$Out\inputs.json" -Raw | ConvertFrom-Json
$hashes=@($inputs | ForEach-Object {
 $actual=(Get-FileHash -LiteralPath $_.Path).Hash
 @{path=$_.Path;expected=$_.Hash;actual=$actual;equal=($actual -eq $_.Hash)}
})
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
$dwm=@(Get-Process dwm | ForEach-Object {
 $processId=$_.Id
 $_.Modules | Where-Object ModuleName -eq 'bc250d3d.dll' | ForEach-Object {
  @{pid=$processId;path=$_.FileName;hash=(Get-FileHash $_.FileName).Hash}
 }
})
$result=@{
 utc=[DateTime]::UtcNow.ToString('o');boot=$boot;same_boot=($boot -ceq $ready.boot)
 event_logs=$logChecks;events=@($events | Select-Object TimeCreated,Id,ProviderName,Message)
 same_dump_inventory=$dumpEqual;dump_inventory=$dumpNow;inputs=$hashes;dwm=$dwm
 worker_status=$worker.status;monitor_status=$monitor.status
}
$result | ConvertTo-Json -Depth 8 | Set-Content "$Out\final-readback.json" -Encoding UTF8
@{same_boot=$result.same_boot;events=$events.Count;same_dump_inventory=$dumpEqual;changed_inputs=@($hashes | Where-Object equal -eq $false).Count;dwm_modules=$dwm.Count} | ConvertTo-Json -Compress

param([string]$Out)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
if(Test-Path "$Out\final-readback-v3.json"){throw 'Final readback already exists'}
$start=Get-Content "$Out\start.json" -Raw | ConvertFrom-Json
$begin=[DateTime]::Parse($start.utc).ToLocalTime()
$rows=@{utc=[DateTime]::UtcNow.ToString('o');boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')}
$rows.same_boot=$rows.boot -eq $start.boot
$recordedInputs=ConvertFrom-Json ([IO.File]::ReadAllText("$Out\inputs.json"))
$rows.inputs=@(foreach($entry in $recordedInputs){@{path=$entry.Path;expected=$entry.Hash;actual=(Get-FileHash -LiteralPath $entry.Path).Hash}})
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
foreach($mode in @('clock','health')){
 $p=Start-Process $cli -ArgumentList $mode,'read' -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Out\final-$mode.txt"
 $keep=$p.Handle
 if(-not $p.WaitForExit(4000)){Stop-Process -Id $p.Id -Force;throw "$mode read timeout"}
 if($p.ExitCode -ne 0){throw "$mode exit"}
 $rows[$mode]=[IO.File]::ReadAllText("$Out\final-$mode.txt")
}
$rows.events=@();$rows.event_logs=@()
foreach($log in @('System','Application')){
 $config=Get-WinEvent -ListLog $log -ErrorAction Stop
 $latest=Get-WinEvent -LogName $log -MaxEvents 1 -ErrorAction Stop
 $rows.event_logs+=@{name=$log;enabled=$config.IsEnabled;latest_record=$latest.RecordId}
 try{
  $events=@(Get-WinEvent -FilterHashtable @{LogName=$log;StartTime=$begin;Id=@(4101,1001,41)} -ErrorAction Stop)
 }catch{if($_.FullyQualifiedErrorId -notlike 'NoMatchingEventsFound*'){throw};$events=@()}
 $rows.events+=@($events | Where-Object {
  ($_.Id -eq 4101 -and $_.ProviderName -eq 'Display') -or $_.ProviderName -match 'WER-SystemErrorReporting|Kernel-Power' -or ($_.Id -eq 1001 -and $_.Message -match 'LiveKernelEvent')
 } | Select-Object TimeCreated,Id,ProviderName,Message)
}
$rows.new_dumps=@(@('C:\Windows\LiveKernelReports','C:\Windows\Minidump') | Where-Object {Test-Path $_} | ForEach-Object {Get-ChildItem -LiteralPath $_ -Recurse -Filter '*.dmp' -File | Where-Object LastWriteTime -ge $begin | Select-Object FullName,Length,LastWriteTimeUtc})
$rows | ConvertTo-Json -Depth 3 | Set-Content "$Out\final-readback-v3.json" -Encoding UTF8
[pscustomobject]$rows | Select-Object same_boot,clock,health,@{n='new_events';e={@($_.events).Count}},@{n='new_dumps';e={@($_.new_dumps).Count}} | ConvertTo-Json

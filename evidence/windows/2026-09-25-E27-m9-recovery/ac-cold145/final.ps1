$ErrorActionPreference='Stop'
$cli='C:\BC250\m9\candidate07145\client\bc250kmd_cli.exe'
'now='+(Get-Date).ToString('s')
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
& $cli health read
if($LASTEXITCODE -ne 0){throw 'No health reply'}
& $cli clock read
if($LASTEXITCODE -ne 0){throw 'No clock reply'}
'disabled_legacy_task='+[string](Get-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV').State
foreach($p in Get-Process dwm,bc250mon){
 $p.ProcessName+' pid='+$p.Id+' start='+$p.StartTime.ToString('s')
 foreach($m in @($p.Modules | Where-Object {$_.ModuleName -in @('bc250d3d.dll','bc250control.dll')})){
  $p.ProcessName+' module='+$m.FileName+' sha256='+(Get-FileHash $m.FileName).Hash
 }
}
Get-ChildItem C:\BC250\mon\log -Filter '*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 1 | ForEach-Object {Select-String -LiteralPath $_.FullName -Pattern 'full-WDDM start confirmed' | Select-Object -Last 1 | ForEach-Object {$_.Line}}
(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters') | Select-Object UnconfirmedStarts,EnableFullWddm,EnableSdmaIbControl,EnableSdmaVaControl | ConvertTo-Json
'cold145_final_record_complete'

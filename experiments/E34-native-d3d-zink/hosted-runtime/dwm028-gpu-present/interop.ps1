param([Parameter(Mandatory)][ValidateSet(0,1)][int]$Value)
$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted028'
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
$mutex=New-Object Threading.Mutex($false,'Global\BC250G0Interop028')
$locked=$false
try {
 try {$locked=$mutex.WaitOne(30000)} catch [Threading.AbandonedMutexException] {$locked=$true}
 if(!$locked){throw 'Interop mutex timeout; inspect original operation'}
 $gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
 if($gpu.Count -ne 1){throw 'Ambiguous adapter'}
 $version=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
 if($version -ne '0.7.163.1'){throw 'Wrong installed KMD'}
 $image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
 if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
 if($image.StartsWith('\??\')){$image=$image.Substring(4)}
 if((Get-FileHash -LiteralPath $image).Hash -ne 'B602D9A09C695CF8F2D3F5B3AC22A27AF9C1BC88E6F43BB156606F5369D162D8'){throw 'KMD hash mismatch'}
 if($Value -eq 1){
  if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
  $health=& $cli health read | Out-String
  if($LASTEXITCODE -ne 0 -or $health -notmatch 'version=0x000700A3 flags=15'){throw 'Health preflight'}
  $settings=Get-ItemProperty $reg
  if($settings.EnableCddDwmInterop -ne 0 -or $settings.EnableGpuPresentBlit -ne 0 -or $settings.UnconfirmedStarts -ne 0){throw 'Unexpected starting gates'}
  [IO.File]::WriteAllText("$d\interop-pending",[DateTime]::UtcNow.ToString('o'))
 } elseif(!(Test-Path "$d\interop-pending")) {return}
 # Receipt precedes any adapter mutation; restore retries a partially failed start.
 $suffix=if($Value){'on'}else{'off'}
 $tag=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
 & $cli log summary *> "$d\interop-$suffix-$tag-before.log"
 $problem=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
 if($problem -ne 22){
  & pnputil.exe /disable-device $gpu[0].InstanceId *> "$d\interop-$suffix-$tag-disable.log"
  if($LASTEXITCODE -ne 0){throw 'Adapter disable failed'}
 }
 $problem=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
 if($problem -ne 22){throw 'Adapter not disabled'}
 New-ItemProperty $reg -Name EnableCddDwmInterop -Value $Value -PropertyType DWord -Force | Out-Null
 New-ItemProperty $reg -Name EnableGpuPresentBlit -Value $Value -PropertyType DWord -Force | Out-Null
 New-ItemProperty $reg -Name EnableHandleIdentityProbe -Value 1 -PropertyType DWord -Force | Out-Null
 & pnputil.exe /enable-device $gpu[0].InstanceId *> "$d\interop-$suffix-$tag-enable.log"
 if($LASTEXITCODE -ne 0){throw 'Adapter enable failed'}
 $ready=$false
 for($i=0;$i -lt 25;$i++){
  $health=& $cli health read | Out-String
  if($LASTEXITCODE -eq 0 -and $health -match 'version=0x000700A3 flags=(7|15)\b'){$ready=$true;break}
  Start-Sleep -Seconds 1
 }
 if(!$ready){throw 'Adapter readiness not observed; do not rerun trial'}
 & $cli log summary *> "$d\interop-$suffix-$tag-after.log"
 if($LASTEXITCODE -ne 0){throw 'Summary failed'}
 $text=Get-Content "$d\interop-$suffix-$tag-after.log" -Raw
 if($text -notmatch ("CDD interop"+$Value+" GPU Present gate"+$Value+" identity probe1")){throw 'Latched gate witness missing'}
 @{utc=[DateTime]::UtcNow.ToString('o');value=$Value;health=$health} | ConvertTo-Json | Set-Content "$d\interop-$suffix-$tag.json"
 if($Value -eq 0){Remove-Item -LiteralPath "$d\interop-pending" -Force}
} finally {if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}

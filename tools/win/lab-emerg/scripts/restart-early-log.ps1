# Control capture: disable and enable the display device on the deployed KMD (desktop on the CPU route first, by the
# caller) and save the driver's log ring at about +0.2, +0.6, +1.2, +2.5 and +5 s after the enable, before the tail
# wraps. The start is then confirmed by the caller (heartbeat + confirm-current). Elevated (target.py ps).
param([string]$Out = 'C:\BC250\tmp\early-log-185')
$ErrorActionPreference = 'Stop'
$cli = 'C:\BC250\m8\bc250kmd_cli.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$gpu = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VEN_1002&DEV_13FE' } | Select-Object -First 1
$id = $gpu.InstanceId
function Prop($k) { (Get-PnpDeviceProperty -InstanceId $id -KeyName $k).Data }
"before: version $(Prop DEVPKEY_Device_DriverVersion) problem $(Prop DEVPKEY_Device_ProblemCode)"
& pnputil.exe /disable-device $id | Out-Null
if ((Prop DEVPKEY_Device_ProblemCode) -ne 22) { throw 'Disable not observed' }
& pnputil.exe /enable-device $id | Out-Null
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt 30 -and (Prop DEVPKEY_Device_ProblemCode) -ne 0) { Start-Sleep -Milliseconds 20 }
$t0 = [DateTime]::UtcNow
"started after $([math]::Round($sw.Elapsed.TotalSeconds,2)) s: version $(Prop DEVPKEY_Device_DriverVersion) problem $(Prop DEVPKEY_Device_ProblemCode)"
$i = 0
foreach ($at in 0.2, 0.6, 1.2, 2.5, 5.0) {
  while (([DateTime]::UtcNow - $t0).TotalSeconds -lt $at) { Start-Sleep -Milliseconds 20 }
  $i++
  $text = & $cli log | Out-String
  [IO.File]::WriteAllText("$Out\log-$i.txt", "utc $([DateTime]::UtcNow.ToString('o')) exit $LASTEXITCODE`r`n$text")
  "log-$i at +$([math]::Round(([DateTime]::UtcNow - $t0).TotalSeconds, 2)) s, $($text.Length) chars"
}

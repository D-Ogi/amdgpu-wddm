# Early KMD log capture during a candidate start: waits (in-process PnP reads, no process spawns) until the display
# device runs the named driver version with no problem code, then saves the driver's log ring through the CLI at
# about +0.2, +0.6, +1.2, +2.5 and +5 s, before the tail wraps. Read-only towards the driver.
#   lab-emerg.py ps early-log.ps1 600 (parameters through the defaults below; edit Version for a new candidate)
param([string]$Version = '0.7.190.1', [int]$Seconds = 420, [string]$Out = 'C:\BC250\tmp\early-log')
$cli = 'C:\BC250\m8\bc250kmd_cli.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$gpu = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VEN_1002&DEV_13FE' } | Select-Object -First 1
$id = $gpu.InstanceId
$sw = [Diagnostics.Stopwatch]::StartNew()
"start $([DateTime]::UtcNow.ToString('o')) waiting for $Version"
$seen = $false
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
  try {
    $v = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion -ErrorAction Stop).Data
    $p = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode -ErrorAction Stop).Data
  } catch { $v = ''; $p = -1 }
  if ($v -eq $Version -and $p -eq 0) { $seen = $true; break }
  Start-Sleep -Milliseconds 50
}
if (!$seen) { "timeout: version $v problem $p"; exit 1 }
$t0 = [DateTime]::UtcNow
"running $Version at $($t0.ToString('o'))"
$i = 0
foreach ($at in 0.2, 0.6, 1.2, 2.5, 5.0) {
  while (([DateTime]::UtcNow - $t0).TotalSeconds -lt $at) { Start-Sleep -Milliseconds 20 }
  $i++
  $text = & $cli log | Out-String
  [IO.File]::WriteAllText("$Out\log-$i.txt", "utc $([DateTime]::UtcNow.ToString('o')) exit $LASTEXITCODE`r`n$text")
  "log-$i at +$([math]::Round(([DateTime]::UtcNow - $t0).TotalSeconds, 2)) s, $($text.Length) chars"
}

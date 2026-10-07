# C62 sweep cool-down (LAB): fan at 100 % until Tctl is below 62 C and the GPU below 60 C, or $Seconds pass, then the
# fan back to the Standard curve. One telemetry process, one sample per 5 s.
param([int]$Seconds = 300)
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
& $cli fan set 100 ([Math]::Min(300, $Seconds)) 2>&1 | Out-Null
$sw = [Diagnostics.Stopwatch]::StartNew(); $line = ''
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    $t = (& $cli telemetry 2>&1) -join ' '
    $tc = if ($t -match 'tsi_c=([\d.]+)') { [double]$Matches[1] } else { 99 }
    $gc = if ($t -match 'temperature_c=([\d.]+)') { [double]$Matches[1] } else { 99 }
    $line = "cool t=$([int]$sw.Elapsed.TotalSeconds) s tctl=$tc gpu=$gc"
    if ($tc -lt 62 -and $gc -lt 60) { break }
    Start-Sleep -Seconds 5
}
& $cli fan curve standard 2>&1 | Out-Null
$line

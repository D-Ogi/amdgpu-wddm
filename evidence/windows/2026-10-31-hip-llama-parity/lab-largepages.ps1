# C59 large-page arms for the HIP parity work: read or set the next-start LargePages value, and show what this start
# uses (the KMD start log line and the VRAM size the GPU reports). -Set 0|1|2 writes the value for the next start;
# nothing restarts here.
param([ValidateSet('', '0', '1', '2')][string]$Set = '')
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
if ($Set -ne '') { & $cli state set LargePages $Set 2>&1 }
"LargePages (next start) -> $(& $cli state get LargePages 2>&1)"
& $cli log 2>&1 | Select-String -Pattern 'LargePages|large VRAM pages|aligned layout|segment base|memory layout' |
    Select-Object -Last 6 | ForEach-Object { "log: $($_.Line.Trim())" }
& $cli clock read 2>&1 | Select-Object -First 1

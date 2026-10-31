# HIP parity arms after a restart: pin the DPM floor to the measured clock and show clock, CU and state.
# -Off releases the floor. The state store (lab-hip-state.ps1) keeps CuMode and DpmMaxMHz across restarts;
# the floor does not survive a restart.
param([int]$MHz = 1500, [switch]$Off)
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
if ($Off) { & $cli dpm floor off 2>&1 } else { & $cli dpm floor $MHz 2>&1 }
Start-Sleep -Seconds 2
& $cli clock read 2>&1 | Select-Object -First 2
foreach ($n in 'CuMode', 'DpmMaxMHz') { "$n -> $(& $cli state get $n 2>&1)" }

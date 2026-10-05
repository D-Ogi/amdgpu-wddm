$ErrorActionPreference='Stop'
$out='C:\BC250\m9\ops-gpu-009'
if (Test-Path $out) { throw 'Output directory already exists' }
New-Item -ItemType Directory $out | Out-Null
$cli='C:\BC250\m8\bc250kmd_cli.exe'
$gate='C:\BC250\tmp\e19_target.ps1'
function Temperature {
 $raw=& 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
 $raw | Add-Content "$out\temperature.txt"
 if ($raw -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { throw 'No temperature' }
 if ([double]$Matches[1] -ge 85) { throw 'Temperature limit' }
}
function Step([string]$name,[string[]]$arguments) {
 "STEP_BEGIN $name $((Get-Date).ToString('s'))"
 & $cli @arguments > "$out\$name.txt" 2>&1
 if ($LASTEXITCODE -ne 0) { throw "Failed $name" }
 "STEP_END $name $((Get-Date).ToString('s'))"
 Temperature
}
Temperature
try {
 "GATE_BEGIN $((Get-Date).ToString('s'))"
 & $gate -Phase gate -Full 1 -Engines 1 -GpuVa 1 -GpuSubmit 1 -Package C:\BC250\m8 -Tag m9-tiny | Out-File "$out\gate.txt"
 "GATE_END $((Get-Date).ToString('s'))"
 Step 'gart' @('gart','enable')
 Step 'psp' @('psp','load')
 Step 'ih' @('ih','init')
 foreach ($stage in 1..8) { Step "gfx-$stage" @('gfx','run',"$stage") }
 Step 'fence-control' @('fence','gfx','1','ib')
 $batch=@"
@echo off
set VK_DRIVER_FILES=C:\BC250\m9\gather-fence\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set MESA_SHADER_CACHE_DISABLE=true
set BC250_IB_DWORDS=
set PATH=C:\BC250\m8;%PATH%
set GGML_VK_DISABLE_F16=
set GGML_VK_DISABLE_GRAPH_OPTIMIZE=
set GGML_VK_DISABLE_FUSION=
set GGML_VK_DISABLE_ASYNC=
set GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM=
C:\BC250\m8\vkcompute.exe C:\BC250\m8\spv --runs 3 < NUL > C:\BC250\m9\ops-gpu-009\m8.out 2> C:\BC250\m9\ops-gpu-009\m8.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-009\m8.exit
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories260K.gguf -p "Once upon a time" -n 16 --temp 0 --seed 1 -ngl 0 -no-cnv -t 6 < NUL > C:\BC250\m9\ops-gpu-009\cpu.out 2> C:\BC250\m9\ops-gpu-009\cpu.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-009\cpu.exit
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories260K.gguf -p "Once upon a time" -n 16 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 < NUL > C:\BC250\m9\ops-gpu-009\gpu.out 2> C:\BC250\m9\ops-gpu-009\gpu.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-009\gpu.exit
exit /b
"@
 Set-Content "$out\run.cmd" $batch -Encoding ASCII
 $user=(Get-CimInstance Win32_ComputerSystem).UserName
 if (-not $user) { throw 'No interactive session' }
 $action=New-ScheduledTaskAction -Execute "$out\run.cmd" -WorkingDirectory C:\BC250\m8
 $principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
 Register-ScheduledTask -TaskName BC250-M9-Ops -Action $action -Principal $principal -Force | Out-Null
 "OPERATORS_BEGIN $((Get-Date).ToString('s'))"
 Start-ScheduledTask BC250-M9-Ops
 $deadline=(Get-Date).AddSeconds(120)
 do {
  Start-Sleep -Seconds 2
  Temperature
  $state=(Get-ScheduledTask BC250-M9-Ops).State
 } while ($state -eq 'Running' -and (Get-Date) -lt $deadline)
 if ($state -eq 'Running') { throw 'Operator test deadline' }
 foreach ($name in @('m8','cpu','gpu')) {
  if (-not (Test-Path "$out\$name.exit")) { throw "Missing exit witness $name" }
  "RESULT $name exit=$(Get-Content "$out\$name.exit")"
  Get-Content "$out\$name.out" -Tail 26
  if ([int](Get-Content "$out\$name.exit") -ne 0) { throw "Failed $name" }
 }
 $counts=& $cli log summary | Out-String
 $counts | Set-Content "$out\counts.txt"
 $hw=($counts -split "`n" | Where-Object { $_ -match 'node 0 hardware:' } | Select-Object -Last 1)
 $umd=($counts -split "`n" | Where-Object { $_ -match 'summary: umd:' } | Select-Object -Last 1)
 if ($hw -notmatch '0 timeouts, 0 refused' -or $umd -notmatch ', 0 not run') { throw "Hardware execution failed: $hw $umd" }
 $cpu=[IO.File]::ReadAllText("$out\cpu.out").Replace("`r`n","`n")
 $gpu=[IO.File]::ReadAllText("$out\gpu.out").Replace("`r`n","`n")
 if ($cpu -ne $gpu) { throw 'CPU/GPU text mismatch' }
 'TEXT_AND_HARDWARE_PASS'
} finally {
 "RESTORE_BEGIN $((Get-Date).ToString('s'))"
 Get-ScheduledTask BC250-M9-Ops -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
 Get-Process vkcompute,bc250-node-probe,test-backend-ops,llama* -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
 Get-ScheduledTask BC250-M9-Ops -ErrorAction SilentlyContinue | Unregister-ScheduledTask -Confirm:$false -ErrorAction SilentlyContinue
 & $cli log summary | Out-File "$out\kmd.txt"
 & $cli ih state | Out-File "$out\ih-state.txt"
 & $gate -Phase gate -Full 0 -Package C:\BC250\m8 -Tag m9-restored | Out-File "$out\restored.txt"
 Get-Process dwm -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
 & $gate -Phase confirm -Package C:\BC250\m8 -Tag m9-restored
}

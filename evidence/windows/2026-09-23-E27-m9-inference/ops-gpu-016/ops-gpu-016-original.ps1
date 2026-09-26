$ErrorActionPreference='Stop'
$out='C:\BC250\m9\ops-gpu-016'
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
& 'C:\BC250\bc250rd\bc250rd_cli.exe' clock 1000 820 | Out-File "$out\clock.txt"
if ($LASTEXITCODE -ne 0) { throw 'Clock request failed' }
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
set VK_DRIVER_FILES=C:\BC250\m9\quiet-submit\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set MESA_SHADER_CACHE_DISABLE=true
set VK_LOADER_DEBUG=driver
set BC250_TRACE_SUBMITS=0
set BC250_IB_DWORDS=
set PATH=C:\BC250\m8;%PATH%
set GGML_VK_DISABLE_F16=
set GGML_VK_DISABLE_GRAPH_OPTIMIZE=
set GGML_VK_DISABLE_FUSION=
set GGML_VK_DISABLE_ASYNC=
set GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM=
C:\BC250\m8\vkcompute.exe C:\BC250\m8\spv --runs 3 < NUL > C:\BC250\m9\ops-gpu-016\m8.out 2> C:\BC250\m9\ops-gpu-016\m8.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\m8.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
findstr /c:"bc250: progress before submit" C:\BC250\m9\ops-gpu-016\m8.err > NUL
if errorlevel 1 exit /b 2
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-016\stories15M-1.out 2> C:\BC250\m9\ops-gpu-016\stories15M-1.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\stories15M-1.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-016\stories15M-2.out 2> C:\BC250\m9\ops-gpu-016\stories15M-2.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\stories15M-2.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-016\stories15M-3.out 2> C:\BC250\m9\ops-gpu-016\stories15M-3.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\stories15M-3.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -p "Once upon a time" -n 96 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-016\stories15M-4.out 2> C:\BC250\m9\ops-gpu-016\stories15M-4.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\stories15M-4.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-016\tinyllama-1.out 2> C:\BC250\m9\ops-gpu-016\tinyllama-1.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\tinyllama-1.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-016\tinyllama-2.out 2> C:\BC250\m9\ops-gpu-016\tinyllama-2.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\tinyllama-2.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-016\tinyllama-3.out 2> C:\BC250\m9\ops-gpu-016\tinyllama-3.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\tinyllama-3.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
C:\BC250\m9\llama\llama-completion.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -p "The capital of France is" -n 64 --temp 0 --seed 1 -ngl 99 -no-cnv -t 6 --verbose < NUL > C:\BC250\m9\ops-gpu-016\tinyllama-4.out 2> C:\BC250\m9\ops-gpu-016\tinyllama-4.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\tinyllama-4.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
C:\BC250\m9\llama\llama-bench.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -ngl 99 -p 512 -n 128 -r 3 -t 6 -o json -v < NUL > C:\BC250\m9\ops-gpu-016\bench-stories15M.out 2> C:\BC250\m9\ops-gpu-016\bench-stories15M.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\bench-stories15M.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
C:\BC250\m9\llama\llama-bench.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -ngl 99 -p 512 -n 128 -r 3 -t 6 -o json -v < NUL > C:\BC250\m9\ops-gpu-016\bench-tinyllama.out 2> C:\BC250\m9\ops-gpu-016\bench-tinyllama.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\bench-tinyllama.exit
if not "%ERRORLEVEL%"=="0" exit /b 1
C:\BC250\m9\residency-probe.exe 64K vram < NUL > C:\BC250\m9\ops-gpu-016\residency-vram-64K.out 2> C:\BC250\m9\ops-gpu-016\residency-vram-64K.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\residency-vram-64K.exit
if not "%ERRORLEVEL%"=="0" goto skip_vram
C:\BC250\m9\residency-probe.exe 1G vram < NUL > C:\BC250\m9\ops-gpu-016\residency-vram-1G.out 2> C:\BC250\m9\ops-gpu-016\residency-vram-1G.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\residency-vram-1G.exit
:skip_vram
C:\BC250\m9\residency-probe.exe 64K gtt < NUL > C:\BC250\m9\ops-gpu-016\residency-gtt-64K.out 2> C:\BC250\m9\ops-gpu-016\residency-gtt-64K.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\residency-gtt-64K.exit
if not "%ERRORLEVEL%"=="0" goto skip_gtt
C:\BC250\m9\residency-probe.exe 1G gtt < NUL > C:\BC250\m9\ops-gpu-016\residency-gtt-1G.out 2> C:\BC250\m9\ops-gpu-016\residency-gtt-1G.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-016\residency-gtt-1G.exit
:skip_gtt
exit /b
"@
 Set-Content "$out\run.cmd" $batch -Encoding ASCII
 $user=(Get-CimInstance Win32_ComputerSystem).UserName
 if (-not $user) { throw 'No interactive session' }
 $action=New-ScheduledTaskAction -Execute "$out\run.cmd" -WorkingDirectory C:\BC250\m8
 $principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
 Register-ScheduledTask -TaskName BC250-M9-Ops -Action $action -Principal $principal -Force | Out-Null
 "OPERATORS_BEGIN $((Get-Date).ToString('s'))"
 Start-ScheduledTask BC250-M9-Ops
 $deadline=(Get-Date).AddSeconds(300)
 do {
  Start-Sleep -Seconds 2
  Temperature
  $liveCounts=& $cli log summary | Out-String
  $liveCounts | Add-Content "$out\hardware-watch.txt"
  $liveHw=($liveCounts -split "`n" | Where-Object { $_ -match 'node 0 hardware:' } | Select-Object -Last 1)
  $liveUmd=($liveCounts -split "`n" | Where-Object { $_ -match 'summary: umd:' } | Select-Object -Last 1)
  if ($liveCounts -match 'summary: \*\*\* TDR:' -or $liveHw -notmatch '0 timeouts, 0 refused' -or $liveUmd -notmatch ', 0 not run') { throw "Hardware watch failed: $liveHw $liveUmd" }
  $state=(Get-ScheduledTask BC250-M9-Ops).State
 } while ($state -eq 'Running' -and (Get-Date) -lt $deadline)
 if ($state -eq 'Running') { throw 'Operator test deadline' }
 foreach ($name in @('m8','stories15M-1','stories15M-2','stories15M-3','stories15M-4','tinyllama-1','tinyllama-2','tinyllama-3','tinyllama-4')) {
  if (-not (Test-Path "$out\$name.exit")) { throw "Missing exit witness $name" }
  if ([int](Get-Content "$out\$name.exit") -ne 0) { throw "Failed $name" }
  $trace=[IO.File]::ReadAllText("$out\$name.err")
  if ($trace -notmatch 'bc250: progress before submit') { throw "Candidate ICD missing: $name" }
  if ($name -ne 'm8') {
   $offload=[regex]::Match($trace,'offloaded ([0-9]+)/([0-9]+) layers to GPU')
   if (-not $offload.Success -or [int]$offload.Groups[1].Value -le 0 -or $offload.Groups[1].Value -ne $offload.Groups[2].Value) { throw "Incomplete GPU offload: $name" }
   $model=if ($name.StartsWith('stories15M')) { 'stories15M' } else { 'tinyllama' }
   $reference=[IO.File]::ReadAllText("C:\BC250\m9\reference\$model-ngl99.out").Replace("`r`n","`n")
   $actual=[IO.File]::ReadAllText("$out\$name.out").Replace("`r`n","`n")
   if ($actual -ne $reference) { throw "Linux GPU text mismatch: $name" }
  }
  "RESULT $name PASS"
 }
 foreach ($name in @('bench-stories15M','bench-tinyllama')) {
  if (-not (Test-Path "$out\$name.exit") -or [int](Get-Content "$out\$name.exit") -ne 0) { throw "Failed $name" }
  if (-not (Select-String "$out\$name.err" -SimpleMatch 'bc250: progress before submit' -Quiet)) { throw "Candidate missing $name" }
  $rows=Get-Content "$out\$name.out" -Raw | ConvertFrom-Json
  if (@($rows).Count -ne 2) { throw "Expected pp512 and tg128: $name" }
  foreach ($row in $rows) { if ($row.n_gpu_layers -ne 99 -or $row.avg_ts -le 0) { throw "Invalid benchmark $name" } }
  "BENCHMARK $name"
  $rows | Select-Object n_prompt,n_gen,avg_ts,stddev_ts
 }
 $counts=& $cli log summary | Out-String
 $counts | Set-Content "$out\counts.txt"
 $hw=($counts -split "`n" | Where-Object { $_ -match 'node 0 hardware:' } | Select-Object -Last 1)
 $umd=($counts -split "`n" | Where-Object { $_ -match 'summary: umd:' } | Select-Object -Last 1)
 if ($hw -notmatch '0 timeouts, 0 refused' -or $umd -notmatch ', 0 not run') { throw "Hardware execution failed: $hw $umd" }
 $hwMatch=[regex]::Match($hw,'node 0 hardware: ([0-9]+) submitted, ([0-9]+) completed,')
 if (-not $hwMatch.Success -or [long]$hwMatch.Groups[1].Value -le 0 -or $hwMatch.Groups[1].Value -ne $hwMatch.Groups[2].Value) { throw "Hardware completion counts do not match: $hw" }

 foreach ($name in @('residency-vram-64K','residency-vram-1G','residency-gtt-64K','residency-gtt-1G')) {
  if (Test-Path "$out\$name.exit") {
   "RESIDENCY_CONTROL $name exit=$(Get-Content "$out\$name.exit")"
   Get-Content "$out\$name.out" -Tail 5
  } else { "RESIDENCY_CONTROL $name NOT_RUN" }
 }
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

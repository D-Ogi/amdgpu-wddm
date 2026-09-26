$ErrorActionPreference='Stop'
$out='C:\BC250\m9\m8-regression-0756'
New-Item -ItemType Directory -Force $out | Out-Null
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
 & $gate -Phase gate -Full 1 -Engines 1 -GpuVa 1 -GpuSubmit 1 -Package C:\BC250\m8 -Tag m9-regression | Out-File "$out\gate.txt"
 "GATE_END $((Get-Date).ToString('s'))"
 Step 'gart' @('gart','enable')
 Step 'psp' @('psp','load')
 Step 'ih' @('ih','init')
 foreach ($stage in 1..8) { Step "gfx-$stage" @('gfx','run',"$stage") }
 Step 'fence-control' @('fence','gfx','1','ib')
 $batch=@"
@echo off
set VK_DRIVER_FILES=C:\BC250\m8\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set MESA_SHADER_CACHE_DISABLE=true
set BC250_IB_DWORDS=
set PATH=C:\BC250\m8;%PATH%
C:\BC250\m8\vkcompute.exe C:\BC250\m8\spv --runs 3 < NUL > C:\BC250\m9\m8-regression-0756\suite.txt 2> C:\BC250\m9\m8-regression-0756\suite.err
echo %ERRORLEVEL% > C:\BC250\m9\m8-regression-0756\suite.exit
exit /b
"@
 Set-Content "$out\run.cmd" $batch -Encoding ASCII
 $user=(Get-CimInstance Win32_ComputerSystem).UserName
 if (-not $user) { throw 'No interactive session' }
 $action=New-ScheduledTaskAction -Execute "$out\run.cmd" -WorkingDirectory C:\BC250\m8
 $principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
 Register-ScheduledTask -TaskName BC250-M9-Control -Action $action -Principal $principal -Force | Out-Null
 Start-ScheduledTask BC250-M9-Control
 $deadline=(Get-Date).AddSeconds(90)
 do {
  Start-Sleep -Seconds 2
  Temperature
  $state=(Get-ScheduledTask BC250-M9-Control).State
 } while ($state -eq 'Running' -and (Get-Date) -lt $deadline)
 if ($state -eq 'Running') { throw 'M8 regression deadline' }
 if (-not (Test-Path "$out\suite.exit")) { throw 'No exit witness' }
 Get-Content "$out\suite.txt"
 if ([int](Get-Content "$out\suite.exit") -ne 0) { throw 'M8 regression failed' }
} finally {
 Get-ScheduledTask BC250-M9-Control -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
 Get-Process vkcompute -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
 Get-ScheduledTask BC250-M9-Control -ErrorAction SilentlyContinue | Unregister-ScheduledTask -Confirm:$false -ErrorAction SilentlyContinue
 & $cli log | Out-File "$out\kmd.txt"
 & $cli ih state | Out-File "$out\ih-state.txt"
 & $gate -Phase gate -Full 0 -Package C:\BC250\m8 -Tag m9-restored | Out-File "$out\restored.txt"
 Get-Process dwm -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
 & $gate -Phase confirm -Package C:\BC250\m8 -Tag m9-restored
}

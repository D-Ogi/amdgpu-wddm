$ErrorActionPreference = 'Stop'
# llama.cpp pp512/tg128 on the installed release (b19), the same binary, models and arguments as M415 (E27, 2026-09-24),
# but the system-registered ICD and the release's DPM (no fixed clock). One CLI dpm sampler process, 1 s samples.
$out = 'C:\BC250\m9\llama-b19-' + (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
New-Item -ItemType Directory -Force $out | Out-Null
'out=' + $out
'start_utc=' + (Get-Date).ToUniversalTime().ToString('s')
'boot=' + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
$gpu = @(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
'driver_version=' + (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
'icd_hash=' + (Get-FileHash 'C:\Program Files\amdgpu-wddm\vulkan\vulkan_radeon.dll').Hash.Substring(0, 8)
'overlay_summary_paused=' + (Test-Path C:\BC250\mon\graphics-summary.pause)
Get-Process dwm | Select-Object Id, StartTime | Format-Table | Out-String
& $cli dpm 1 | Out-String
& $cli log summary | Out-File "$out\before.log"

$run = @"
@echo off
set VK_LOADER_DEBUG=driver
set MESA_SHADER_CACHE_DISABLE=true
C:\BC250\m9\llama\llama-bench.exe -m C:\BC250\m9\models\stories15M-q4_0.gguf -ngl 99 -p 512 -n 128 -r 3 -t 6 -o json -v < NUL > $out\stories15M.out 2> $out\stories15M.err
echo %ERRORLEVEL% > $out\stories15M.exit
C:\BC250\m9\llama\llama-bench.exe -m C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf -ngl 99 -p 512 -n 128 -r 3 -t 6 -o json -v < NUL > $out\tinyllama.out 2> $out\tinyllama.err
echo %ERRORLEVEL% > $out\tinyllama.exit
"@
$run | Set-Content "$out\run.cmd" -Encoding ASCII
"& cmd.exe /c $out\run.cmd`n[IO.File]::WriteAllText('$out\worker.exit', [string]`$LASTEXITCODE)" | Set-Content "$out\worker.ps1" -Encoding ASCII
$user = (Get-CimInstance Win32_ComputerSystem).UserName
$action = New-ScheduledTaskAction -Execute conhost.exe -Argument "--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File $out\worker.ps1"
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName BC250-LlamaB19 -Action $action -Principal $principal -Force | Out-Null

$sampler = Start-Process -FilePath $cli -ArgumentList 'dpm', '25', '1000' -RedirectStandardOutput "$out\dpm.txt" -NoNewWindow -PassThru
Start-ScheduledTask BC250-LlamaB19
$deadline = (Get-Date).AddSeconds(150)
$stop = $null
do {
    Start-Sleep -Seconds 2
    $last = Get-Content "$out\dpm.txt" -Tail 12 -ErrorAction SilentlyContinue | Where-Object { $_ -match ' C busy' } | Select-Object -Last 1
    if ($last -and $last -match '([0-9.]+) C busy' -and [double]$Matches[1] -ge 89) { $stop = 'Tctl ' + $Matches[1]; break }
} while (-not (Test-Path "$out\worker.exit") -and (Get-Date) -lt $deadline)
if (-not (Test-Path "$out\worker.exit")) { Stop-ScheduledTask BC250-LlamaB19; Get-Process llama-bench -ErrorAction SilentlyContinue | Stop-Process -Force; 'STOPPED ' + $stop }
Unregister-ScheduledTask BC250-LlamaB19 -Confirm:$false
$sampler.WaitForExit(30000) | Out-Null
'end_utc=' + (Get-Date).ToUniversalTime().ToString('s')
foreach ($name in @('stories15M', 'tinyllama')) {
    $exit = if (Test-Path "$out\$name.exit") { (Get-Content "$out\$name.exit").Trim() } else { 'none' }
    "$name exit=$exit"
    $err = Get-Content "$out\$name.err" -Raw -ErrorAction SilentlyContinue
    if ($err -match '([A-Za-z]:\\[^\r\n"]*vulkan_radeon\.dll)') { "$name icd_loaded=" + $Matches[1] }
    if ($exit -eq '0') { (Get-Content "$out\$name.out" -Raw | ConvertFrom-Json) | ForEach-Object { $_ } | Select-Object n_prompt, n_gen, avg_ts, stddev_ts | Format-Table | Out-String }
}
$mhz = Get-Content "$out\dpm.txt" | ForEach-Object { if ($_ -match 'dpm\s+(\d+) MHz') { [int]$Matches[1] } }
$tc = Get-Content "$out\dpm.txt" | ForEach-Object { if ($_ -match '([0-9.]+) C busy') { [double]$Matches[1] } }
$busy = Get-Content "$out\dpm.txt" | ForEach-Object { if ($_ -match 'C busy\s+([0-9.]+)%') { [double]$Matches[1] } }
'dpm_samples=' + $mhz.Count + ' gfx_mhz min/avg/max=' + ($mhz | Measure-Object -Minimum -Average -Maximum | ForEach-Object { '{0}/{1:N0}/{2}' -f $_.Minimum, $_.Average, $_.Maximum })
'tctl max=' + ($tc | Measure-Object -Maximum).Maximum + ' busy_pct avg=' + ('{0:N1}' -f ($busy | Measure-Object -Average).Average)
& $cli log summary | Out-File "$out\after.log"
Get-Process dwm | Select-Object Id, StartTime, Responding | Format-Table | Out-String

# LAB (elevated SSH, session 0): DP audio step 3, trial 2 - the tone and the rate.
# The tone plays in the interactive session through a one-shot scheduled task (t2-child.ps1), as the design's
# step 3 says. While it plays, this script reads the DCCG audio DTO and the AFMT/DP_SEC state by regcalc name.
# Writes nothing to the hardware and no registry value; the task is removed at the end.
# Push first:   target.py push t2-child.ps1 wasapi-probe.cs --to C:\BC250\tmp\dpaudio-step3
param([string]$Dir = 'C:\BC250\tmp\dpaudio-step3', [string]$Task = 'DP audio tone step 3')
$ErrorActionPreference = 'Continue'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$clock = [Diagnostics.Stopwatch]::StartNew()
function Sec([string]$t) { ""; "=== $t  t+$('{0:F1}' -f $clock.Elapsed.TotalSeconds)s  utc $([DateTime]::UtcNow.ToString('HH:mm:ss.fff'))" }
$during = @('mmDCCG_AUDIO_DTO_SOURCE','mmDCCG_AUDIO_DTO1_MODULE','mmDCCG_AUDIO_DTO1_PHASE',
  'mmDIG0_AFMT_CNTL','mmDIG0_AFMT_AUDIO_SRC_CONTROL','mmDIG0_AFMT_AUDIO_PACKET_CONTROL',
  'mmDIG0_AFMT_AUDIO_PACKET_CONTROL2','mmDIG0_AFMT_STATUS','mmDIG0_AFMT_60958_0','mmDIG0_AFMT_INFOFRAME_CONTROL0',
  'mmDP0_DP_SEC_CNTL','mmDP0_DP_SEC_AUD_N','mmDP0_DP_SEC_AUD_M_READBACK','mmDP0_DP_SEC_TIMESTAMP',
  'mmAZALIA_AUDIO_DTO','mmAZALIA_AUDIO_DTO_CONTROL','mmAZALIA_CYCLIC_BUFFER_SYNC',
  'mmAZALIA_APPLICATION_POSITION_IN_CYCLIC_BUFFER','mmAZALIA_OUTPUT_STREAM_ARBITER_CONTROL',
  'mmAZALIA_DATA_DMA_CONTROL','mmAZALIA_BDL_DMA_CONTROL','mmDIO_MEM_PWR_CTRL','mmCLK4_0_CLK4_CLK2_CURRENT_CNT')
function ReadSet([string]$label) {
  Sec "registers $label"
  foreach ($n in $during) {
    $o = (& $cli read $n 2>&1 | Select-String -Pattern '^read ' | ForEach-Object { ($_ -split ' ')[-1] })
    "{0,-50} {1}" -f $n, $o
  }
}
function Moving([string]$label, [int]$count) {
  Sec "DP_SEC_AUD_M_READBACK repeated ($label)"
  for ($i = 0; $i -lt $count; $i++) {
    $q = [Diagnostics.Stopwatch]::StartNew()
    $m = (& $cli read mmDP0_DP_SEC_AUD_M_READBACK 2>&1 | Select-String -Pattern '^read ' | ForEach-Object { ($_ -split ' ')[-1] })
    $p = (& $cli read mmAZALIA_APPLICATION_POSITION_IN_CYCLIC_BUFFER 2>&1 | Select-String -Pattern '^read ' | ForEach-Object { ($_ -split ' ')[-1] })
    "i=$i M=$m APP_POS=$p read_ms=$('{0:F0}' -f $q.Elapsed.TotalMilliseconds)"
    Start-Sleep -Milliseconds 400
  }
}

Sec 'setup'
"host $env:COMPUTERNAME utc $([DateTime]::UtcNow.ToString('o'))"
Remove-Item -LiteralPath (Join-Path $Dir 'done.json') -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath $Dir -Filter 'begin-*' -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath $Dir -Filter 'end-*' -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $Dir 'child-out.txt') -ErrorAction SilentlyContinue
$exe = Join-Path $Dir 'wasapi-probe.exe'
$src = Join-Path $Dir 'wasapi-probe.cs'
if (-not (Test-Path -LiteralPath $exe) -or ((Get-Item $src).LastWriteTime -gt (Get-Item $exe).LastWriteTime)) {
  & "$env:windir\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /optimize /platform:x64 "/out:$exe" $src
  if ($LASTEXITCODE -ne 0) { 'csc failed'; exit 1 }
}
"probe $exe sha256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.Substring(0,8))"
$sess = (Get-Process -Name explorer -ErrorAction SilentlyContinue | Select-Object -First 1).SessionId
"interactive explorer session: $(if ($null -eq $sess) { 'none found' } else { $sess })"
& $cli read mmTHM_TCON_CUR_TMP 2>&1 | Select-Object -Last 1
(& $cli dpm 1 2>&1 | Select-String 'temperature_c') -join ' '

ReadSet 'before'

Sec 'start the one-shot interactive task'
Unregister-ScheduledTask -TaskName $Task -Confirm:$false -ErrorAction SilentlyContinue
$act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$Dir\t2-child.ps1`" -Dir `"$Dir`""
$pr = New-ScheduledTaskPrincipal -UserId 'bc250' -LogonType Interactive -RunLevel Highest
$st = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 120) -AllowStartIfOnBatteries
$null = Register-ScheduledTask -TaskName $Task -Action $act -Principal $pr -Settings $st
Start-ScheduledTask -TaskName $Task
"started, waiting for the first arm"
$d = [Diagnostics.Stopwatch]::StartNew()
while ($d.Elapsed.TotalSeconds -lt 30 -and -not (Test-Path -LiteralPath (Join-Path $Dir 'begin-excl-poll-48000'))) { Start-Sleep -Milliseconds 200 }
if (-not (Test-Path -LiteralPath (Join-Path $Dir 'begin-excl-poll-48000'))) { 'the first arm did not begin within 30 s' }
else {
  "arm 1 began at $(Get-Content -LiteralPath (Join-Path $Dir 'begin-excl-poll-48000'))"
  Start-Sleep -Milliseconds 2000
  ReadSet 'during arm 1 (excl-poll 48 kHz, about 2 s in)'
  Moving 'during arm 1' 6
}
# wait for arm 3 to end, then watch the PlaySync arm
$d = [Diagnostics.Stopwatch]::StartNew()
while ($d.Elapsed.TotalSeconds -lt 60 -and -not (Test-Path -LiteralPath (Join-Path $Dir 'begin-playsync'))) { Start-Sleep -Milliseconds 300 }
if (Test-Path -LiteralPath (Join-Path $Dir 'begin-playsync')) {
  "playsync arm began at $(Get-Content -LiteralPath (Join-Path $Dir 'begin-playsync'))"
  Start-Sleep -Milliseconds 1500
  ReadSet 'during the PlaySync arm'
  Moving 'during the PlaySync arm' 5
}

Sec 'wait for the child'
$d = [Diagnostics.Stopwatch]::StartNew()
while ($d.Elapsed.TotalSeconds -lt 60 -and -not (Test-Path -LiteralPath (Join-Path $Dir 'done.json'))) { Start-Sleep -Milliseconds 300 }
"done.json present: $(Test-Path -LiteralPath (Join-Path $Dir 'done.json'))"
(Get-ScheduledTask -TaskName $Task -ErrorAction SilentlyContinue).State
(Get-ScheduledTaskInfo -TaskName $Task -ErrorAction SilentlyContinue) | Format-List TaskName,LastRunTime,LastTaskResult,NumberOfMissedRuns | Out-String

ReadSet 'after'

Sec 'child output'
if (Test-Path -LiteralPath (Join-Path $Dir 'child-out.txt')) { Get-Content -LiteralPath (Join-Path $Dir 'child-out.txt') } else { 'no child-out.txt' }

Sec 'MMDevice state after the tone'
$root = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
foreach ($e in (Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
  $state = (Get-ItemProperty -LiteralPath $e.PSPath -Name DeviceState -ErrorAction SilentlyContinue).DeviceState
  $pp = Join-Path $e.PSPath 'Properties'
  $props = Get-ItemProperty -LiteralPath $pp -ErrorAction SilentlyContinue
  $name = $props.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
  "--- id=$($e.PSChildName) state=$state name='$name'"
  foreach ($k in @('{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0','{f19f064d-082c-4e27-bc73-6882a1bb8e4c},3','{e4870e26-3cc5-4cd2-ba46-ca0a9a70ed04},3')) {
    $b = $props.$k
    if ($b -is [byte[]] -and $b.Length -ge 18) {
      "    $k tag=0x{0:X4} ch={1} rate={2} bits={3}" -f ([BitConverter]::ToUInt16($b,0)), ([BitConverter]::ToUInt16($b,2)), ([BitConverter]::ToUInt32($b,4)), ([BitConverter]::ToUInt16($b,14))
    } elseif ($null -ne $b) { "    $k = $b" }
  }
}

Sec 'dpaudio record after the tone'
& $cli dpaudio state 2>&1

Sec 'KMD log: dpaudio and refusal lines'
$log = & $cli log 2>&1
@($log) | Where-Object { $_ -match 'dpaudio|refus|audio' } | ForEach-Object { $_ }

Sec 'temperature and clock'
(& $cli dpm 1 2>&1 | Select-String 'temperature_c') -join ' '

Sec 'remove the one-shot task'
Unregister-ScheduledTask -TaskName $Task -Confirm:$false -ErrorAction SilentlyContinue
"task present after removal: $([bool](Get-ScheduledTask -TaskName $Task -ErrorAction SilentlyContinue))"
Sec 'end'

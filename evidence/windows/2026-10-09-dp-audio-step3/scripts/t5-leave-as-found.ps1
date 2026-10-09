# LAB (elevated SSH): DP audio step 3 - leave the lab as found and prove it.
$ErrorActionPreference = 'Continue'
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$dir = 'C:\BC250\tmp\dpaudio-step3'
"staging directory before: $(Test-Path -LiteralPath $dir)"
Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
"staging directory after:  $(Test-Path -LiteralPath $dir)"
"one-shot task present:    $([bool](Get-ScheduledTask -TaskName 'DP audio tone step 3' -ErrorAction SilentlyContinue))"
$par = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
foreach ($v in 'EnableDpAudio','EnableDpAudioEndpoint','EnableDpAudioStream','EnableDpAudioEdid') {
  $x = (Get-ItemProperty -LiteralPath $par -Name $v -ErrorAction SilentlyContinue).$v
  "switch $v = $(if ($null -eq $x) { 'absent' } else { $x })"
}
"TdrDelay = $((Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -Name TdrDelay -ErrorAction SilentlyContinue).TdrDelay)"
$boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
"boot $($boot.ToUniversalTime().ToString('o'))"
$ev = Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $boot } -ErrorAction SilentlyContinue |
      Where-Object { $_.Id -in 4101,4102,13,41,1001 -or $_.ProviderName -match 'WHEA|Display|BugCheck' }
"faults since boot (Display 4101/4102, WHEA, BugCheck): $(@($ev).Count)"
@($ev) | Select-Object -First 10 | ForEach-Object { "  $($_.TimeCreated.ToUniversalTime().ToString('HH:mm:ss')) $($_.ProviderName) id=$($_.Id) $(($_.Message -split "`n")[0])" }
"--- dpaudio state"
& $cli dpaudio state 2>&1 | Select-String -Pattern 'state |starts |stream on|reference clock' | ForEach-Object { $_.Line }
"--- temperature and clock"
(& $cli dpm 1 2>&1 | Select-String 'temperature_c') -join ' '

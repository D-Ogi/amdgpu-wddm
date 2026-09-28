# witcher3-dx12-004: as run 003, but PresentMon with --no_track_display --no_track_gpu --no_track_input
# (present cadence only), because run 003 produced no CSV rows with display tracking on.
# Same package, ICD, environment and restoration as run 002 (menu -> load a save -> 3D scene, 600 s bound,
# stop file C:\BC250\m12\witcher3-dx12\stop-004, automatic screenshots every 30 s, input from the separate
# BC250-Witcher-Input task). Differences: PresentMon 2.6.0 records every present of witcher3.exe to
# presentmon.csv (ETW, started before the game, stopped after it), and no DXVK frame cap (run 002 had
# dxgi.maxFrameRate = 30 in dxvk.conf, which vkd3d-proton's swapchain does not read anyway; VKD3D_FRAME_RATE
# stays unset = uncapped by vkd3d). The game's own vsync/frame limit settings are whatever user.settings holds
# (hash recorded before and after, restored if the game rewrites it).
$ErrorActionPreference = 'Stop'
$out = 'C:\BC250\m12\witcher3-dx12-004'
$stopFile = 'C:\BC250\m12\witcher3-dx12\stop-004'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3\bin\x64_dx12'
$pkg = 'C:\BC250\m12\fl-probe001'
$hashes = @{
  'd3d12.dll'     = '7B77ED5C107033DCB33D339D5AE54122D94A37A78DD817590FC3D4142C431887'
  'd3d12core.dll' = '90B1DAD6441BB2A6569681011D75770D5BCFE79882E4AA3DBBA21FF419EC9ECC'
  'dxgi.dll'      = '2E674A56A48B2739B2B636105F7CDFA14A4C1964C278BCE6F9EE4C31ED50C05F'
}
$active = 'C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$icdHash = '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'
$pm = 'C:\BC250\m12\presentmon\PresentMon-2.6.0-x64.exe'
$pmHash = 'B2A706BC6AD475749E3B7E3409263AA1E6906D45BDCF993F6DBC0F660188F1AF'
$pmSession = 'BC250W3'
$cli = 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
$settings = Join-Path $env:USERPROFILE 'Documents\The Witcher 3\user.settings'
$saves = Join-Path $env:USERPROFILE 'Documents\The Witcher 3\gamesaves'
$bound = 600

function Stop-Requested { try { (Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop } catch { Test-Path -LiteralPath 'C:\BC250\STOP' } }
function Temp-Now { $raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String; if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { [double]$Matches[1] } else { -1 } }

if (Test-Path $out) { throw 'Run exists' }
if (Test-Path $stopFile) { Remove-Item -LiteralPath $stopFile -Force }
if (Stop-Requested) { throw 'Owner STOP' }
if (Get-Process witcher3 -ErrorAction SilentlyContinue) { throw 'Game already running' }
if (Get-Process PresentMon-2.6.0-x64 -ErrorAction SilentlyContinue) { throw 'PresentMon already running' }
if ((Get-FileHash -LiteralPath $active).Hash -ne $icdHash) { throw 'Registered ICD is not 93B1D1FD' }
if ((Get-FileHash -LiteralPath $pm).Hash -ne $pmHash) { throw 'PresentMon hash' }
foreach ($n in $hashes.Keys) { if ((Get-FileHash -LiteralPath "$pkg\$n").Hash -ne $hashes[$n]) { throw "Package hash: $n" } }
$t = Temp-Now; if ($t -lt 0 -or $t -ge 85) { throw "Temperature $t" }
New-Item -ItemType Directory -Path $out, "$out\mesa-cache", "$out\vkd3d-cache" | Out-Null
"start " + (Get-Date -Format o)
"temperature_before=$t"
& $cli health read | Tee-Object -FilePath "$out\before-health.txt"
& $cli clock read | Tee-Object -FilePath "$out\before-clock.txt"
"exe " + (Get-FileHash -LiteralPath "$game\witcher3.exe").Hash
$settingsBefore = $null
if (Test-Path -LiteralPath $settings) { Copy-Item -LiteralPath $settings -Destination "$out\user.settings.before"; $settingsBefore = (Get-FileHash -LiteralPath $settings).Hash; "settings_before=$settingsBefore" }
$savesBefore = (Get-ChildItem -LiteralPath $saves -File -ErrorAction SilentlyContinue | Measure-Object).Count
"saves_before=$savesBefore"
foreach ($n in $hashes.Keys) {
  if (Test-Path -LiteralPath "$game\$n") { Copy-Item -LiteralPath "$game\$n" -Destination "$out\original-$n"; "original present: $n" }
  Copy-Item -LiteralPath "$pkg\$n" -Destination "$game\$n" -Force
}
'package placed'

$child = $null
$pmProc = $null
$code = 0
try {
  # PresentMon first, so the first presents of the game are in the trace. Own ETW session name, console
  # statistics off, CSV with v2 metrics and the display metadata columns, exits by itself when the game exits.
  $pmArgs = "--process_name witcher3.exe --output_file `"$out\presentmon.csv`" --session_name $pmSession --stop_existing_session --no_console_stats --qpc_time_ms --no_track_display --no_track_gpu --no_track_input --terminate_on_proc_exit"
  $pmProc = Start-Process -FilePath $pm -ArgumentList $pmArgs -WorkingDirectory $out -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out\presentmon.stdout.txt" -RedirectStandardError "$out\presentmon.stderr.txt"
  Start-Sleep -Seconds 3
  if ($pmProc.HasExited) { throw "PresentMon exited early with $($pmProc.ExitCode): $(Get-Content "$out\presentmon.stderr.txt" -Raw)" }
  "presentmon pid=$($pmProc.Id)"
  "logman: " + ((& logman query $pmSession -ets 2>&1 | Where-Object { $_ -match 'Status|Name|Provider|Microsoft|Error|not' }) -join ' | ')
  & logman query $pmSession -ets 2>&1 | Set-Content "$out\logman-session.txt"

  $env:RADV_EXPERIMENTAL = 'sparse'
  $env:MESA_SHADER_CACHE_DIR = "$out\mesa-cache"
  $env:VKD3D_DEBUG = 'warn'
  $env:VKD3D_LOG_FILE = "$out\vkd3d.log"
  $env:VKD3D_SHADER_CACHE_PATH = "$out\vkd3d-cache"
  $env:DXVK_LOG_PATH = $out
  $env:DXVK_LOG_LEVEL = 'info'
  $env:DXVK_CONFIG_FILE = "$out\dxvk.conf"
  "# run 004: no frame cap (run 002 had dxgi.maxFrameRate = 30 here)" | Set-Content -LiteralPath $env:DXVK_CONFIG_FILE -Encoding ASCII
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = "$game\witcher3.exe"
  $psi.WorkingDirectory = $game
  $psi.UseShellExecute = $false
  $child = [System.Diagnostics.Process]::Start($psi)
  $null = $child.Handle
  @{ pid = $child.Id; session = $child.SessionId; presentmon_pid = $pmProc.Id; boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o'); icd = $icdHash; utc = [DateTime]::UtcNow.ToString('o'); qpc_start_ms = [double][Diagnostics.Stopwatch]::GetTimestamp() * 1000.0 / [Diagnostics.Stopwatch]::Frequency } | ConvertTo-Json | Set-Content "$out\launch.json"
  "pid=$($child.Id)"
  $timer = [Diagnostics.Stopwatch]::StartNew()
  $seen = @{}
  $nextShot = 30
  $taken = 0
  $lastTitle = ''
  $endReason = 'bound'
  while (-not $child.HasExited -and $timer.Elapsed.TotalSeconds -lt $bound) {
    try {
      foreach ($m in $child.Modules) {
        if ($m.ModuleName -in @('dxgi.dll', 'd3d12.dll', 'd3d12core.dll', 'd3d11.dll', 'vulkan-1.dll', 'vulkan_radeon.dll', 'dcomp.dll') -and -not $seen.ContainsKey($m.FileName)) {
          $seen[$m.FileName] = @{ path = $m.FileName; sha256 = (Get-FileHash -LiteralPath $m.FileName).Hash.Substring(0, 8); t = [int]$timer.Elapsed.TotalSeconds }
          "module $([int]$timer.Elapsed.TotalSeconds)s $($m.FileName) $($seen[$m.FileName].sha256)"
        }
      }
    } catch {}
    $seen | ConvertTo-Json -Depth 4 | Set-Content "$out\modules.json"
    $child.Refresh()
    if ($child.MainWindowTitle -and $child.MainWindowTitle -ne $lastTitle) { $lastTitle = $child.MainWindowTitle; "window $([int]$timer.Elapsed.TotalSeconds)s '$lastTitle'" }
    if (Stop-Requested) { throw 'Owner STOP' }
    if (Test-Path -LiteralPath $stopFile) { $endReason = 'stop-file'; "stop file seen at $([int]$timer.Elapsed.TotalSeconds)s"; break }
    $t = Temp-Now
    $csvBytes = if (Test-Path -LiteralPath "$out\presentmon.csv") { (Get-Item -LiteralPath "$out\presentmon.csv").Length } else { 0 }
    "t=$([int]$timer.Elapsed.TotalSeconds) tctl=$t ws_mb=$([int]($child.WorkingSet64 / 1MB)) responding=$($child.Responding) csv_bytes=$csvBytes pm_alive=$(-not $pmProc.HasExited)"
    if ($t -ge 85) { throw 'Thermal stop' }
    if ($timer.Elapsed.TotalSeconds -ge $nextShot) {
      try {
        Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile ("$out\auto-{0:d3}.png" -f $taken) -TimeoutSec 5
        "auto screenshot $taken at $([int]$timer.Elapsed.TotalSeconds)s"
      } catch { "auto screenshot $taken failed: $($_.Exception.Message)" }
      $taken++
      $nextShot += 30
    }
    Start-Sleep -Seconds 3
  }
  @{ elapsed_seconds = [int]$timer.Elapsed.TotalSeconds; has_exited = $child.HasExited; exit_code = $(if ($child.HasExited) { $child.ExitCode } else { $null }); end_reason = $endReason; auto_screenshots = $taken; last_title = $lastTitle } | ConvertTo-Json | Set-Content "$out\result.json"
  Get-Content "$out\result.json"
} catch {
  "RUN_ERROR $($_.Exception.Message)"
  $_ | Out-String | Set-Content "$out\error.txt"
  $code = 1
} finally {
  if ($child -and -not $child.HasExited) { "killing pid $($child.Id)"; Stop-Process -Id $child.Id -Force; Start-Sleep -Seconds 2 }
  Get-Process witcher3 -ErrorAction SilentlyContinue | ForEach-Object { "stray witcher3 $($_.Id)"; Stop-Process -Id $_.Id -Force }
  # Let PresentMon see the process exit and flush its CSV, then make sure both the process and the ETW session are gone.
  if ($pmProc) {
    if (-not $pmProc.WaitForExit(15000)) { "presentmon still alive after 15 s, stopping"; Stop-Process -Id $pmProc.Id -Force }
    else { "presentmon exit=$($pmProc.ExitCode)" }
  }
  try { & $pm --session_name $pmSession --terminate_existing_session --no_csv 2>&1 | Out-Null } catch {}
  "presentmon_csv_bytes=" + $(if (Test-Path -LiteralPath "$out\presentmon.csv") { (Get-Item -LiteralPath "$out\presentmon.csv").Length } else { 0 })
  foreach ($n in $hashes.Keys) {
    if (Test-Path -LiteralPath "$out\original-$n") { Copy-Item -LiteralPath "$out\original-$n" -Destination "$game\$n" -Force; "restored original $n" }
    elseif ((Test-Path -LiteralPath "$game\$n") -and (Get-FileHash -LiteralPath "$game\$n").Hash -eq $hashes[$n]) { Remove-Item -LiteralPath "$game\$n" -Force; "removed $n" }
    elseif (Test-Path -LiteralPath "$game\$n") { "LEFT UNKNOWN FILE $n"; $code = 2 }
  }
  if ($settingsBefore -and (Test-Path -LiteralPath $settings)) {
    $after = (Get-FileHash -LiteralPath $settings).Hash
    if ($after -ne $settingsBefore) { Copy-Item -LiteralPath $settings -Destination "$out\user.settings.after"; Copy-Item -LiteralPath "$out\user.settings.before" -Destination $settings -Force; "settings rewritten by game, restored (after copy kept)" } else { 'settings unchanged' }
  }
  "saves_after=" + ((Get-ChildItem -LiteralPath $saves -File -ErrorAction SilentlyContinue | Measure-Object).Count) + " (before $savesBefore)"
  if (Test-Path -LiteralPath $stopFile) { Remove-Item -LiteralPath $stopFile -Force }
  "icd_after=" + (Get-FileHash -LiteralPath $active).Hash.Substring(0, 8)
  & $cli health read | Tee-Object -FilePath "$out\after-health.txt"
  & $cli clock read | Tee-Object -FilePath "$out\after-clock.txt"
  "temperature_after=$(Temp-Now)"
  "dwm=" + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
  "end " + (Get-Date -Format o)
}
"run_exit=$code"
exit $code

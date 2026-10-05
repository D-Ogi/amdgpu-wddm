# wsi-kmt-004: plumbing control (H1) of the Vulkan WSI KMT present path on candidate ICD 0CD4A98D
# 004: 11BA2143 plus a VidPn source 0 exclusive-ownership check logged at present-context creation (fork 1d7ac9ca).
# (fork amdgpu-wddm/radv-wddm2-wsi-kmt aa8296c0: B9F4C95B plus SubRectCnt >= 1, the documented Blt requirement that
# run 002 measured as STATUS_INVALID_PARAMETER for every present). Run 001 showed that
# the loader ignores VK_DRIVER_FILES in the elevated task (baseline 93B1D1FD loaded), so this run swaps the
# candidate in place of the registered ICD for its duration (E43 method) and restores it in finally. Stage A: vkcube on the
# KMT path (D3DKMTPresent of the linear swapchain image, no CPU copy). Stage B: the same vkcube with
# BC250_WSI_CPU_PRESENT=1 (the GDI path) as the matched control. PresentMon records both. KMD counters
# (log summary, health) before and after. No UMD/DWM/KMD change, no registry change.
$ErrorActionPreference = 'Stop'
$out = 'C:\BC250\m12\wsi-kmt-004'
$candidate = 'C:\BC250\m12\icd-candidates\vulkan_radeon.0CD4A98D.dll'
$candidateHash = '0CD4A98DD80AE1248CFA1E6D4C1EB650DCF217FA9A840C002EDC4BB7EB0ADB47'
$registered = 'C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$baselineHash = '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'
$vkcube = 'C:\BC250\m10\wsi-final\vkcube.exe'
$vkcubeHash = '87A96AF3A4677C4AE0BC293625B9F18BFE80A41362992BC03CA2AA3CADE37721'
$pm = 'C:\BC250\m12\presentmon\PresentMon-2.6.0-x64.exe'
$pmHash = 'B2A706BC6AD475749E3B7E3409263AA1E6906D45BDCF993F6DBC0F660188F1AF'
$cli = 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
$frames = 900
$bound = 75

function Stop-Requested { try { (Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop } catch { Test-Path -LiteralPath 'C:\BC250\STOP' } }
function Temp-Now { $raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String; if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { [double]$Matches[1] } else { -1 } }
function Say($text) { try { Invoke-RestMethod -Method Post -Uri http://127.0.0.1:2250/status -Body (@{ text = $text; level = 'info' } | ConvertTo-Json) -ContentType 'application/json' -TimeoutSec 3 | Out-Null } catch {} }
function Shot($name) { try { Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$out\$name.png" -TimeoutSec 8; "screenshot $name " + (Get-FileHash -LiteralPath "$out\$name.png").Hash.Substring(0, 12) } catch { "screenshot $name failed: $($_.Exception.Message)" } }

if (Test-Path $out) { throw 'Run exists' }
if (Stop-Requested) { throw 'Owner STOP' }
if (Get-Process vkcube -ErrorAction SilentlyContinue) { throw 'vkcube already running' }
if (Get-Process PresentMon-2.6.0-x64 -ErrorAction SilentlyContinue) { throw 'PresentMon already running' }
& logman query BC250WSIKmt004 -ets *> $null
if ($LASTEXITCODE -eq 0) { throw 'ETW session BC250WSIKmt004 already exists' }
if ((Get-FileHash -LiteralPath $registered).Hash -ne $baselineHash) { throw 'Registered ICD is not 93B1D1FD' }
if ((Get-FileHash -LiteralPath $candidate).Hash -ne $candidateHash) { throw 'Candidate hash' }
if ((Get-FileHash -LiteralPath $vkcube).Hash -ne $vkcubeHash) { throw 'vkcube hash' }
if ((Get-FileHash -LiteralPath $pm).Hash -ne $pmHash) { throw 'PresentMon hash' }
$t = Temp-Now; if ($t -lt 0 -or $t -ge 85) { throw "Temperature $t" }
New-Item -ItemType Directory -Path $out, "$out\mesa-cache" | Out-Null
"start " + (Get-Date -Format o)
"temperature_before=$t"
"boot " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
"dwm_before=" + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
& $cli health read *> "$out\before-health.txt"
& $cli clock read *> "$out\before-clock.txt"
& $cli log summary *> "$out\before-log-summary.txt"
& $cli log *> "$out\before-log.txt"
Get-Content "$out\before-log-summary.txt" | Select-Object -First 3

# Registered ICD: keep the baseline copy in the run directory, then swap the candidate in (restored in finally).
Copy-Item -LiteralPath $registered -Destination "$out\baseline-93B1D1FD.dll"
if ((Get-FileHash -LiteralPath "$out\baseline-93B1D1FD.dll").Hash -ne $baselineHash) { throw 'Baseline copy hash' }
$swapped = $false
$etwStarted = $false
$code = 0
$pmProc = $null
$child = $null
try {
  Copy-Item -LiteralPath $candidate -Destination $registered -Force
  $swapped = $true
  "icd_swapped=" + (Get-FileHash -LiteralPath $registered).Hash.Substring(0, 8)
  # PresentMon first, own ETW session, so the first presents of each stage are in the trace.
  $pmArgs = "--process_name vkcube.exe --output_file `"$out\presentmon.csv`" --session_name BC250KMT --stop_existing_session --no_console_stats --qpc_time_ms --write_display_metadata"
  $pmProc = Start-Process -FilePath $pm -ArgumentList $pmArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput "$out\presentmon.stdout.txt" -RedirectStandardError "$out\presentmon.stderr.txt"
  Start-Sleep -Seconds 3
  if ($pmProc.HasExited) { throw "PresentMon exited early with $($pmProc.ExitCode): $(Get-Content "$out\presentmon.stderr.txt" -Raw)" }
  "presentmon_pid=$($pmProc.Id)"
  # Raw DxgKrnl trace (granted for 004): own session, all keywords, verbose, 1 MB buffers, sequential file.
  # Decoded on the development PC (Blit_Info 0xa6 with hwnd/bRedirectedPresent, PresentHistory 0xab/0xd7).
  & logman start BC250WSIKmt004 -p Microsoft-Windows-DxgKrnl 0xFFFFFFFFFFFFFFFF 5 -o "$out\dxgkrnl.etl" -ets -bs 1024 -nb 128 512 -max 1024 *> "$out\logman-start-kmt004.txt"
  "etw_kmt004_start_exit=$LASTEXITCODE"
  $etwStarted = ($LASTEXITCODE -eq 0)

  foreach ($stage in @('A-kmt', 'B-cpu')) {
    if (Stop-Requested) { throw 'Owner STOP' }
    $env:MESA_SHADER_CACHE_DIR = "$out\mesa-cache"
    $env:BC250_WSI_PRESENT_LOG = "$out\present-$stage.csv"
    $env:BC250_TRACE_SUBMITS = '1'
    if ($stage -eq 'B-cpu') { $env:BC250_WSI_CPU_PRESENT = '1' } else { Remove-Item Env:BC250_WSI_CPU_PRESENT -ErrorAction SilentlyContinue }
    Say "wsi-kmt-004 stage $stage : vkcube $frames frames, candidate ICD 0CD4A98D ($(if ($stage -eq 'A-kmt') { 'KMT present, no CPU copy' } else { 'CPU present control' }))"
    "stage $stage start " + (Get-Date -Format o)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'cmd.exe'
    $psi.Arguments = "/c `"`"$vkcube`" --c $frames --width 640 --height 480 --suppress_popups > `"$out\vkcube-$stage.stdout.txt`" 2> `"$out\vkcube-$stage.stderr.txt`"`""
    $psi.WorkingDirectory = $out
    $psi.UseShellExecute = $false
    $child = [System.Diagnostics.Process]::Start($psi)
    $null = $child.Handle
    "pid=$($child.Id) session=$($child.SessionId)"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $seen = @{}
    $shots = @(4, 12)
    $taken = 0
    while (-not $child.HasExited -and $timer.Elapsed.TotalSeconds -lt $bound) {
      try {
        foreach ($p in (Get-Process vkcube -ErrorAction SilentlyContinue)) {
          foreach ($m in $p.Modules) {
            if ($m.ModuleName -in @('vulkan-1.dll', 'vulkan_radeon.dll', 'dxgi.dll', 'd3d12.dll', 'dcomp.dll') -and -not $seen.ContainsKey($m.FileName)) {
              $seen[$m.FileName] = (Get-FileHash -LiteralPath $m.FileName).Hash.Substring(0, 8)
              "module $([int]$timer.Elapsed.TotalSeconds)s $($m.FileName) $($seen[$m.FileName])"
            }
          }
        }
      } catch {}
      if (Stop-Requested) { throw 'Owner STOP' }
      $t = Temp-Now
      $csvBytes = if (Test-Path -LiteralPath "$out\present-$stage.csv") { (Get-Item -LiteralPath "$out\present-$stage.csv").Length } else { 0 }
      "t=$([int]$timer.Elapsed.TotalSeconds) tctl=$t present_csv_bytes=$csvBytes"
      if ($t -ge 85) { throw 'Thermal stop' }
      if ($taken -lt $shots.Count -and $timer.Elapsed.TotalSeconds -ge $shots[$taken]) { Shot "$stage-$taken"; $taken++ }
      Start-Sleep -Seconds 2
    }
    if (-not $child.HasExited) { "stage $stage bound reached, killing"; Stop-Process -Id $child.Id -Force; Start-Sleep -Seconds 1 }
    Get-Process vkcube -ErrorAction SilentlyContinue | ForEach-Object { "stray vkcube $($_.Id)"; Stop-Process -Id $_.Id -Force }
    $child.WaitForExit(5000) | Out-Null
    "stage $stage exit=$($child.ExitCode) elapsed=$([int]$timer.Elapsed.TotalSeconds)s"
    $seen | ConvertTo-Json | Set-Content "$out\modules-$stage.json"
    "stderr lines=" + ((Get-Content "$out\vkcube-$stage.stderr.txt" -ErrorAction SilentlyContinue | Measure-Object -Line).Lines)
    Get-Content "$out\vkcube-$stage.stderr.txt" -ErrorAction SilentlyContinue | Select-String -Pattern 'D3DKMTPresent|present|wddm2|failed|error' | Select-Object -First 12
    & $cli log summary *> "$out\after-$stage-log-summary.txt"
    Get-Content "$out\after-$stage-log-summary.txt" | Select-Object -First 3
    $child = $null
    Start-Sleep -Seconds 2
  }
} catch {
  "RUN_ERROR $($_.Exception.Message)"
  $_ | Out-String | Set-Content "$out\error.txt"
  $code = 1
} finally {
  if ($child -and -not $child.HasExited) { Stop-Process -Id $child.Id -Force }
  Get-Process vkcube -ErrorAction SilentlyContinue | ForEach-Object { "stray vkcube $($_.Id)"; Stop-Process -Id $_.Id -Force }
  if ($pmProc) {
    Start-Sleep -Seconds 3
    if (-not $pmProc.HasExited) { Stop-Process -Id $pmProc.Id -Force; Start-Sleep -Seconds 2 }
    "presentmon_exit=" + $(if ($pmProc.HasExited) { $pmProc.ExitCode } else { 'killed' })
  }
  Get-Process PresentMon-2.6.0-x64 -ErrorAction SilentlyContinue | ForEach-Object { "stray presentmon $($_.Id)"; Stop-Process -Id $_.Id -Force }
  & logman stop BC250KMT -ets *> "$out\logman-stop.txt"
  & logman query BC250WSIKmt004 -ets *> "$out\logman-query-kmt004.txt"
  & logman stop BC250WSIKmt004 -ets *> "$out\logman-stop-kmt004.txt"
  "etw_kmt004_stop_exit=$LASTEXITCODE"
  "etw_kmt004_etl_bytes=" + $(if (Test-Path -LiteralPath "$out\dxgkrnl.etl") { (Get-Item -LiteralPath "$out\dxgkrnl.etl").Length } else { 0 })
  foreach ($n in 'BC250_WSI_PRESENT_LOG', 'BC250_TRACE_SUBMITS', 'BC250_WSI_CPU_PRESENT', 'MESA_SHADER_CACHE_DIR') { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
  if ($swapped) {
    $restored = $false
    for ($i = 0; $i -lt 5 -and -not $restored; $i++) {
      try { Copy-Item -LiteralPath "$out\baseline-93B1D1FD.dll" -Destination $registered -Force; $restored = $true } catch { "icd restore attempt $i failed: $($_.Exception.Message)"; Start-Sleep -Seconds 2 }
    }
    $h = (Get-FileHash -LiteralPath $registered).Hash
    if ($h -ne $baselineHash) { "ICD RESTORE MISMATCH $h"; $code = 3 } else { 'registered ICD restored to 93B1D1FD' }
  }
  "presentmon_csv_bytes=" + $(if (Test-Path -LiteralPath "$out\presentmon.csv") { (Get-Item -LiteralPath "$out\presentmon.csv").Length } else { 0 })
  "registered_icd_after=" + (Get-FileHash -LiteralPath $registered).Hash.Substring(0, 8)
  & $cli health read *> "$out\after-health.txt"
  & $cli clock read *> "$out\after-clock.txt"
  & $cli log summary *> "$out\after-log-summary.txt"
  & $cli log *> "$out\after-log.txt"
  "temperature_after=$(Temp-Now)"
  "dwm_after=" + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
  Shot 'end'
  Say "wsi-kmt-004 finished (exit $code)"
  "end " + (Get-Date -Format o)
}
"run_exit=$code"
exit $code

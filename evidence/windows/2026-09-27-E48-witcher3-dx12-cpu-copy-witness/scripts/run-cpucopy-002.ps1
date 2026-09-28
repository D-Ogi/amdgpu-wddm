# cpucopy-002: copy witness on the Vulkan WSI GDI present path (E47 follow-up, message 056). Candidate 63AF86CB
# (fork amdgpu-wddm/radv-wddm2-gdi-immediate 6358c3a9 = 50E99A84 plus the blit-buffer memory-type witness in the
# present-log header and a copy_cycles column = QueryThreadCycleTime around the copy) swapped in place of the
# registered ICD (E43 method, baseline copy and hash-checked restore in finally), vkcube 900 frames at 1920x1200 (one vkcube size)
# on the CPU present path with the per-present timing log. No PresentMon, no ETW, no UMD/DWM/KMD change.
$ErrorActionPreference = 'Stop'
$out = 'C:\BC250\m12\cpucopy-002'
$registered = 'C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$baselineHash = '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'
$stages = @(
  @{ name = 'A-witness'; path = 'C:\BC250\m12\icd-candidates\vulkan_radeon.63AF86CB.dll'; hash = '63AF86CB862570709956E0376FDBDCB434DB3573E6A99412ED60311CFFAE48DA' }
)
$sizes = @(,@(1920, 1200))
$vkcube = 'C:\BC250\m10\wsi-final\vkcube.exe'
$vkcubeHash = '87A96AF3A4677C4AE0BC293625B9F18BFE80A41362992BC03CA2AA3CADE37721'
$cli = 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
$frames = 900
$bound = 60

function Stop-Requested { try { (Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop } catch { Test-Path -LiteralPath 'C:\BC250\STOP' } }
function Temp-Now { $raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String; if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { [double]$Matches[1] } else { -1 } }
function Say($text) { try { Invoke-RestMethod -Method Post -Uri http://127.0.0.1:2250/status -Body (@{ text = $text; level = 'info' } | ConvertTo-Json) -ContentType 'application/json' -TimeoutSec 3 | Out-Null } catch {} }
function Shot($name) { try { Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:2250/screenshot?scale=1&format=png&overlay=0' -OutFile "$out\$name.png" -TimeoutSec 8; "screenshot $name " + (Get-FileHash -LiteralPath "$out\$name.png").Hash.Substring(0, 12) } catch { "screenshot $name failed: $($_.Exception.Message)" } }

if (Test-Path $out) { throw 'Run exists' }
if (Stop-Requested) { throw 'Owner STOP' }
if (Get-Process vkcube -ErrorAction SilentlyContinue) { throw 'vkcube already running' }
if ((Get-FileHash -LiteralPath $registered).Hash -ne $baselineHash) { throw 'Registered ICD is not 93B1D1FD' }
foreach ($s in $stages) { if ((Get-FileHash -LiteralPath $s.path).Hash -ne $s.hash) { throw "Candidate hash: $($s.name)" } }
if ((Get-FileHash -LiteralPath $vkcube).Hash -ne $vkcubeHash) { throw 'vkcube hash' }
$t = Temp-Now; if ($t -lt 0 -or $t -ge 85) { throw "Temperature $t" }
New-Item -ItemType Directory -Path $out, "$out\mesa-cache" | Out-Null
"start " + (Get-Date -Format o)
"temperature_before=$t"
"boot " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
"dwm_before=" + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
& $cli health read *> "$out\before-health.txt"
& $cli clock read *> "$out\before-clock.txt"
& $cli log summary *> "$out\before-log-summary.txt"
Get-Content "$out\before-health.txt" | Select-Object -First 1

Copy-Item -LiteralPath $registered -Destination "$out\baseline-93B1D1FD.dll"
if ((Get-FileHash -LiteralPath "$out\baseline-93B1D1FD.dll").Hash -ne $baselineHash) { throw 'Baseline copy hash' }
$swapped = $false
$code = 0
$child = $null
try {
  foreach ($s in $stages) {
    if (Stop-Requested) { throw 'Owner STOP' }
    Copy-Item -LiteralPath $s.path -Destination $registered -Force
    $swapped = $true
    "stage $($s.name) icd_swapped=" + (Get-FileHash -LiteralPath $registered).Hash.Substring(0, 8)
    foreach ($sz in $sizes) {
      $tag = "$($s.name)-$($sz[0])x$($sz[1])"
      $env:MESA_SHADER_CACHE_DIR = "$out\mesa-cache"
      $env:BC250_WSI_PRESENT_LOG = "$out\present-$tag.csv"
      Say "cpucopy-002 $tag : vkcube $frames frames on the CPU present path (copy witness candidate 63AF86CB)"
      "run $tag start " + (Get-Date -Format o)
      $psi = New-Object System.Diagnostics.ProcessStartInfo
      $psi.FileName = 'cmd.exe'
      $psi.Arguments = "/c `"`"$vkcube`" --c $frames --width $($sz[0]) --height $($sz[1]) --suppress_popups > `"$out\vkcube-$tag.stdout.txt`" 2> `"$out\vkcube-$tag.stderr.txt`"`""
      $psi.WorkingDirectory = $out
      $psi.UseShellExecute = $false
      $child = [System.Diagnostics.Process]::Start($psi)
      $null = $child.Handle
      "pid=$($child.Id)"
      $timer = [Diagnostics.Stopwatch]::StartNew()
      $seen = @{}
      $shot = $false
      while (-not $child.HasExited -and $timer.Elapsed.TotalSeconds -lt $bound) {
        try {
          foreach ($p in (Get-Process vkcube -ErrorAction SilentlyContinue)) {
            foreach ($m in $p.Modules) {
              if ($m.ModuleName -in @('vulkan-1.dll', 'vulkan_radeon.dll') -and -not $seen.ContainsKey($m.FileName)) {
                $seen[$m.FileName] = (Get-FileHash -LiteralPath $m.FileName).Hash.Substring(0, 8)
                "module $([int]$timer.Elapsed.TotalSeconds)s $($m.FileName) $($seen[$m.FileName])"
              }
            }
          }
        } catch {}
        if (Stop-Requested) { throw 'Owner STOP' }
        $t = Temp-Now
        $csvBytes = if (Test-Path -LiteralPath "$out\present-$tag.csv") { (Get-Item -LiteralPath "$out\present-$tag.csv").Length } else { 0 }
        "t=$([int]$timer.Elapsed.TotalSeconds) tctl=$t present_csv_bytes=$csvBytes"
        if ($t -ge 85) { throw 'Thermal stop' }
        if (-not $shot -and $timer.Elapsed.TotalSeconds -ge 4) { Shot $tag; $shot = $true }
        Start-Sleep -Seconds 2
      }
      if (-not $child.HasExited) { "run $tag bound reached, killing"; Stop-Process -Id $child.Id -Force; Start-Sleep -Seconds 1 }
      Get-Process vkcube -ErrorAction SilentlyContinue | ForEach-Object { "stray vkcube $($_.Id)"; Stop-Process -Id $_.Id -Force }
      $child.WaitForExit(5000) | Out-Null
      "run $tag exit=$($child.ExitCode) elapsed=$([int]$timer.Elapsed.TotalSeconds)s"
      $seen | ConvertTo-Json | Set-Content "$out\modules-$tag.json"
      if (Test-Path -LiteralPath "$out\present-$tag.csv") { Select-String -LiteralPath "$out\present-$tag.csv" -Pattern '^# chain' | ForEach-Object { "header: " + $_.Line } }
      Get-Content "$out\vkcube-$tag.stderr.txt" -ErrorAction SilentlyContinue | Select-String -Pattern 'wddm2|failed|error' | Select-Object -First 6
      $child = $null
      Start-Sleep -Seconds 1
    }
  }
} catch {
  "RUN_ERROR $($_.Exception.Message)"
  $_ | Out-String | Set-Content "$out\error.txt"
  $code = 1
} finally {
  if ($child -and -not $child.HasExited) { Stop-Process -Id $child.Id -Force }
  Get-Process vkcube -ErrorAction SilentlyContinue | ForEach-Object { "stray vkcube $($_.Id)"; Stop-Process -Id $_.Id -Force }
  foreach ($n in 'BC250_WSI_PRESENT_LOG', 'MESA_SHADER_CACHE_DIR') { Remove-Item "Env:$n" -ErrorAction SilentlyContinue }
  if ($swapped) {
    $restored = $false
    for ($i = 0; $i -lt 5 -and -not $restored; $i++) {
      try { Copy-Item -LiteralPath "$out\baseline-93B1D1FD.dll" -Destination $registered -Force; $restored = $true } catch { "icd restore attempt $i failed: $($_.Exception.Message)"; Start-Sleep -Seconds 2 }
    }
    $h = (Get-FileHash -LiteralPath $registered).Hash
    if ($h -ne $baselineHash) { "ICD RESTORE MISMATCH $h"; $code = 3 } else { 'registered ICD restored to 93B1D1FD' }
  }
  "registered_icd_after=" + (Get-FileHash -LiteralPath $registered).Hash.Substring(0, 8)
  & $cli health read *> "$out\after-health.txt"
  & $cli clock read *> "$out\after-clock.txt"
  & $cli log summary *> "$out\after-log-summary.txt"
  Get-Content "$out\after-health.txt" | Select-Object -First 1
  "temperature_after=$(Temp-Now)"
  "dwm_after=" + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
  Say "cpucopy-002 finished (exit $code)"
  "end " + (Get-Date -Format o)
}
"run_exit=$code"
exit $code

# cts-extsem-002: as 001, but without the two watchdog time-limit options that this deqp-vk build (cts-release-tools) rejects (001: every case exit -1, "Unrecognized command line option").
# cts-extsem-002: the 44 dEQP-VK.api.external.semaphore.{opaque_win32,d3d12_fence}.* cases, first on the
# registered baseline ICD 93B1D1FD (control), then on candidate 6661C2D2 swapped in place (fence-share fix),
# then the baseline restored from the run-directory copy and hash-verified. Per case: deqp-vk from
# cts-release-tools (hash checked), 45 s bound, result status parsed from the .qpa; a Fail or Crash is recorded,
# not fatal (diagnostic run). No UMD/DWM/KMD change, no promotion.
$ErrorActionPreference = 'Stop'
$out = 'C:\BC250\m12\cts-extsem-002'
$cases = 'C:\BC250\m12\witcher3-dx12\cts-external-semaphore-win32.txt'
$deqp = 'C:\BC250\m12\cts-release-tools\deqp-vk.exe'
$deqpHash = 'AE7BEFDD190EF08E4A715DE0348734879263E1854E017D67865749905A95A2B6'
$active = 'C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$baselineHash = '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'
$candidate = 'C:\BC250\m12\icd-candidates\vulkan_radeon.6661C2D2.dll'
$candidateHash = '6661C2D2FE1DBAAAAA007D1686A0C42F33E7A9F5204DC0681EF98AA404F952AD'
$cli = 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'

function Stop-Requested { try { (Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop } catch { Test-Path -LiteralPath 'C:\BC250\STOP' } }
function Temp-Now { $raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String; if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { [double]$Matches[1] } else { -1 } }

function Run-Cases($phase, $expectIcd) {
  $dir = "$out\$phase"
  New-Item -ItemType Directory -Path $dir | Out-Null
  $counts = @{}
  $index = 0
  foreach ($case in [IO.File]::ReadAllLines($cases)) {
    if (-not $case.Trim()) { continue }
    $index++
    $stem = '{0:D3}' -f $index
    if (Stop-Requested) { throw 'Owner STOP' }
    $t = Temp-Now; if ($t -ge 85) { throw 'Thermal stop' }
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $deqp
    $psi.Arguments = "--deqp-case=$case --deqp-log-filename=$dir\$stem.qpa --deqp-watchdog=enable"
    $psi.WorkingDirectory = (Split-Path $deqp)
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables['VK_LOADER_DEBUG'] = 'driver'
    $p = [Diagnostics.Process]::Start($psi)
    $null = $p.Handle
    $so = [IO.File]::Create("$dir\$stem.out"); $se = [IO.File]::Create("$dir\$stem.err")
    $co = $p.StandardOutput.BaseStream.CopyToAsync($so); $ce = $p.StandardError.BaseStream.CopyToAsync($se)
    $icdSeen = $false
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
      while (-not $p.HasExited -and $timer.Elapsed.TotalSeconds -lt 45) {
        try { foreach ($m in $p.Modules) { if ($m.FileName -eq $active) { $icdSeen = $true } } } catch {}
        Start-Sleep -Milliseconds 20
        $p.Refresh()
      }
      $timedOut = -not $p.HasExited
      if ($timedOut) { Stop-Process -Id $p.Id -Force }
      $null = $co.Wait(3000); $null = $ce.Wait(3000)
      $code = if ($timedOut) { 'timeout' } else { $p.ExitCode }
    } finally { $so.Dispose(); $se.Dispose() }
    $status = 'NoResult'
    if (Test-Path "$dir\$stem.qpa") {
      $qpa = [IO.File]::ReadAllText("$dir\$stem.qpa")
      $mm = [regex]::Matches($qpa, '<Result StatusCode="([^"]+)">')
      if ($mm.Count -eq 1) { $status = $mm[0].Groups[1].Value } elseif ($mm.Count -gt 1) { $status = 'MultipleResults' }
    }
    $err = if (Test-Path "$dir\$stem.err") { [IO.File]::ReadAllText("$dir\$stem.err") } else { '' }
    $icdInLoaderLog = $err -match 'wsi-final\\(?:\.\\)?vulkan_radeon\.dll'
    $counts[$status] = 1 + $(if ($counts.ContainsKey($status)) { $counts[$status] } else { 0 })
    @{ phase = $phase; index = $index; case = $case; status = $status; exit = $code; ms = $timer.ElapsedMilliseconds; icd_module_seen = $icdSeen; icd_in_loader_log = $icdInLoaderLog; icd_hash = $expectIcd; utc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json -Compress | Add-Content "$out\cases.jsonl" -Encoding UTF8
    "$phase $stem $status exit=$code $([int]$timer.ElapsedMilliseconds)ms $case"
  }
  "$phase summary: " + (($counts.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' ')
  $counts | ConvertTo-Json | Set-Content "$out\$phase-summary.json"
}

if (Test-Path $out) { throw 'Run exists' }
if (Stop-Requested) { throw 'Owner STOP' }
if ((Get-FileHash -LiteralPath $active).Hash -ne $baselineHash) { throw 'Registered ICD is not 93B1D1FD' }
if ((Get-FileHash -LiteralPath $candidate).Hash -ne $candidateHash) { throw 'Candidate hash' }
if ((Get-FileHash -LiteralPath $deqp).Hash -ne $deqpHash) { throw 'deqp-vk hash' }
if (-not (Test-Path $cases)) { throw 'Case list missing' }
$t = Temp-Now; if ($t -lt 0 -or $t -ge 85) { throw "Temperature $t" }
New-Item -ItemType Directory -Path $out | Out-Null
"start " + (Get-Date -Format o)
"temperature_before=$t"
"cases=" + ([IO.File]::ReadAllLines($cases) | Where-Object { $_.Trim() } | Measure-Object).Count
& $cli health read | Tee-Object -FilePath "$out\before-health.txt"
Copy-Item -LiteralPath $active -Destination "$out\baseline-93B1D1FD.dll"
if ((Get-FileHash -LiteralPath "$out\baseline-93B1D1FD.dll").Hash -ne $baselineHash) { throw 'Baseline copy hash' }
$swapped = $false
$code = 0
try {
  Run-Cases 'baseline' $baselineHash
  if (Stop-Requested) { throw 'Owner STOP' }
  Copy-Item -LiteralPath $candidate -Destination $active -Force
  $swapped = $true
  "icd_swapped=" + (Get-FileHash -LiteralPath $active).Hash.Substring(0, 8)
  Run-Cases 'candidate' $candidateHash
} catch {
  "RUN_ERROR $($_.Exception.Message)"
  $_ | Out-String | Set-Content "$out\error.txt"
  $code = 1
} finally {
  Get-Process deqp-vk -ErrorAction SilentlyContinue | ForEach-Object { "stray deqp-vk $($_.Id)"; Stop-Process -Id $_.Id -Force }
  if ($swapped) {
    $restored = $false
    for ($i = 0; $i -lt 5 -and -not $restored; $i++) {
      try { Copy-Item -LiteralPath "$out\baseline-93B1D1FD.dll" -Destination $active -Force; $restored = $true } catch { "icd restore attempt $i failed: $($_.Exception.Message)"; Start-Sleep -Seconds 2 }
    }
    $h = (Get-FileHash -LiteralPath $active).Hash
    if ($h -ne $baselineHash) { "ICD RESTORE MISMATCH $h"; $code = 3 } else { 'registered ICD restored to 93B1D1FD' }
  }
  "icd_after=" + (Get-FileHash -LiteralPath $active).Hash.Substring(0, 8)
  & $cli health read | Tee-Object -FilePath "$out\after-health.txt"
  "temperature_after=$(Temp-Now)"
  "dwm=" + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
  "end " + (Get-Date -Format o)
}
"run_exit=$code"
exit $code

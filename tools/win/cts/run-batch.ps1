<#
Runs one batch of the Vulkan CTS sparse sweep (deqp-vk, vulkan-cts-1.4.6.2) on unit A with a hard wall-clock
bound, and resumes it on the next call when the bound or a crash cut it short.

Windows PowerShell 5.1. No inline commands, no profile. Typical call from the development PC:
    target.py ps <this script> -Index 7 -RunId icd85077e29    (RunId: name it after the ICD under test)
The package layout under -Root (default: this script's directory if it holds bin\deqp-vk.exe, else
C:\BC250\cts) is:
    bin\deqp-vk.exe, bin\vulkan\...        CTS binary and its data directory (archive dir)
    direct\cts-direct.dll                  vkGetInstanceProcAddr forwarder to a sibling amdgpu_wddm_radv.dll
    batches\sparse-NNNN.txt, manifest.json the batch case lists
Results go to <Results>\<RunId>\batch-NNNN\ : attempt-KK.qpa (+ .caselist.txt, .stdout.txt, .stderr.txt,
.json), cases.tsv (every finished case: case, status, attempt, duration_us, details) and summary.json /
summary.txt (counts, failing case names, completion).

ICD selection (-Route):
  direct     (default) the loader is bypassed: --deqp-vk-library-path points at cts-direct.dll, which loads
             the ICD copied next to it by absolute path. Works in the elevated SSH session, where the
             Vulkan loader ignores VK_DRIVER_FILES / VK_ICD_FILENAMES (LoaderDriverInterface.md, "Exception
             for Elevated Privileges"). Default ICD: the accepted D3D12 triplet's
             C:\BC250\m15\registration002\amdgpu_wddm_radv.dll, i.e. the RADV the D3D12 tiled resources use.
  registered the system vulkan-1.dll and whatever ICD is registered under HKLM\SOFTWARE\Khronos\Vulkan\Drivers.
RADV exposes sparse binding to a stand-alone Vulkan process only with RADV_EXPERIMENTAL=sparse (the D3D12
host passes its policy through the instance chain instead), so the child gets -Experimental (default sparse).

Exit codes: 0 batch complete, only Pass/NotSupported (warnings listed); 1 batch complete with other results;
2 incomplete, call again with the same arguments to resume; 3 stop the sweep (device lost, a case timed out,
thermal, owner STOP, ICD/witness/device mismatch, setup error) - inspect the GPU and the logs first; the
same call resumes afterwards (the timed-out or lost case keeps its recorded status and is not rerun).
A process crash is recorded as Crash for the case it was in and the next call continues after it.
#>
param(
    [int]$Index = 0,
    [string]$CaseList = '',
    [string]$Root = '',
    [string]$Results = '',
    [string]$RunId = 'default',
    [ValidateSet('direct', 'registered')][string]$Route = 'direct',
    [string]$IcdPath = 'C:\BC250\m15\registration002\amdgpu_wddm_radv.dll',
    [string]$IcdSha256 = '',
    [string]$Experimental = 'sparse',
    [string]$Perftest = '',   # RADV_PERFTEST for the child when not empty (e.g. rtwave64); pinned per RunId
    [string]$ExpectDevice = 'BC-250',
    [string]$IcdModulePattern = '(?i)radv|radeon|amdgpu',
    [ValidateRange(20, 175)][int]$BoundSeconds = 170,
    [int]$ThermalCheckSeconds = 20,
    [double]$ThermalStopC = 87,
    [string]$TempCli = 'C:\BC250\bc250rd\bc250rd_cli.exe',
    [switch]$NoStopFlag
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # no CLIXML progress records on the ssh stream
$clock = [Diagnostics.Stopwatch]::StartNew()
$utf8 = New-Object Text.UTF8Encoding($false)

# target.py ps copies this script to C:\BC250\tmp before running it, so the package root is not
# necessarily the script's directory: fall back to the lab install path.
if (-not $Root) {
    if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'bin\deqp-vk.exe')) { $Root = $PSScriptRoot } else { $Root = 'C:\BC250\cts' }
}
if (-not $Results) { $Results = Join-Path $Root 'results' }
$bin = Join-Path $Root 'bin'
$deqp = Join-Path $bin 'deqp-vk.exe'

function Write-Text([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, $utf8) }
function Append-Text([string]$Path, [string]$Text) { [IO.File]::AppendAllText($Path, $Text, $utf8) }
function Write-Json([string]$Path, $Object) { Write-Text $Path (($Object | ConvertTo-Json -Depth 6) + "`n") }
function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Utc { [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ') }
# Owner STOP flag of the lab overlay (no overlay = no flag).
function Test-OwnerStop {
    if ($NoStopFlag) { return $false }
    try { return [bool](Invoke-RestMethod -Uri 'http://127.0.0.1:2250/flags' -TimeoutSec 1).stop } catch { return $false }
}
# Tctl from bc250rd_cli (one spawn per call, at most every ThermalCheckSeconds; never a per-second loop).
function Read-Tctl {
    if ($ThermalCheckSeconds -le 0 -or -not (Test-Path -LiteralPath $TempCli)) { return $null }
    $tpsi = New-Object Diagnostics.ProcessStartInfo
    $tpsi.FileName = $TempCli; $tpsi.Arguments = 'temp 1 1'; $tpsi.UseShellExecute = $false
    $tpsi.CreateNoWindow = $true; $tpsi.RedirectStandardOutput = $true
    $t = [Diagnostics.Process]::Start($tpsi)
    $tout = $t.StandardOutput.ReadToEndAsync()
    if (-not $t.WaitForExit(5000)) { try { $t.Kill() } catch {}; return $null }
    if ($tout.Result -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { return [double]$Matches[1] }
    return $null
}

# ---------------------------------------------------------------------------------------------- inputs
if ($CaseList) {
    $listPath = (Resolve-Path -LiteralPath $CaseList).Path
    $batchName = 'list-' + [IO.Path]::GetFileNameWithoutExtension($listPath)
} elseif ($Index -gt 0) {
    $listPath = Join-Path $Root ('batches\sparse-{0:D4}.txt' -f $Index)
    $batchName = 'batch-{0:D4}' -f $Index
} else { Write-Output 'stop: give -Index N or -CaseList PATH'; exit 3 }
if (-not (Test-Path -LiteralPath $listPath)) { Write-Output "stop: no case list $listPath"; exit 3 }
if (-not (Test-Path -LiteralPath $deqp)) { Write-Output "stop: no $deqp"; exit 3 }
$cases = @([IO.File]::ReadAllLines($listPath) | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') })
if ($Index -gt 0) {
    $manifestPath = Join-Path $Root 'batches\manifest.json'
    if (Test-Path -LiteralPath $manifestPath) {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $entry = @($manifest.batches | Where-Object { $_.index -eq $Index })
        if ($entry.Count -ne 1) { Write-Output "stop: batch $Index not in manifest"; exit 3 }
        if ((Get-Sha $listPath) -ne $entry[0].sha256 -or $cases.Count -ne $entry[0].count) {
            Write-Output "stop: $listPath differs from manifest (sha256/count)"; exit 3
        }
    }
}

$runDir = Join-Path $Results $RunId
$batchDir = Join-Path $runDir $batchName
New-Item -ItemType Directory -Force -Path $batchDir | Out-Null
$casesTsv = Join-Path $batchDir 'cases.tsv'

# One runner at a time on the machine; never two deqp-vk processes.
$lockPath = Join-Path $Results 'runner.lock'
if (Test-Path -LiteralPath $lockPath) {
    $owner = 0; [int]::TryParse(([IO.File]::ReadAllText($lockPath)).Trim(), [ref]$owner) | Out-Null
    if ($owner -and (Get-Process -Id $owner -ErrorAction SilentlyContinue)) { Write-Output "stop: runner $owner holds $lockPath"; exit 3 }
}
if (Get-Process -Name 'deqp-vk' -ErrorAction SilentlyContinue) { Write-Output 'stop: another deqp-vk is running'; exit 3 }
Write-Text $lockPath ([string]$PID)

$stopReason = $null
$exitCode = 3
try {
    # ------------------------------------------------------------------------------------- ICD route
    $icdSha = ''
    $libraryArg = $null
    $icdWitnessPath = $null
    if ($Route -eq 'direct') {
        if (-not (Test-Path -LiteralPath $IcdPath)) { throw "stop: ICD $IcdPath not found" }
        $icdSha = Get-Sha $IcdPath
        if ($IcdSha256 -and $icdSha -ne $IcdSha256.ToUpperInvariant()) { throw "stop: ICD hash $icdSha, expected $IcdSha256" }
        $directSrc = Join-Path $Root 'direct\cts-direct.dll'
        $icdDir = Join-Path $runDir ('icd-' + $icdSha.Substring(0, 8))
        New-Item -ItemType Directory -Force -Path $icdDir | Out-Null
        $icdCopy = Join-Path $icdDir 'amdgpu_wddm_radv.dll'
        $directCopy = Join-Path $icdDir 'cts-direct.dll'
        if (-not (Test-Path -LiteralPath $icdCopy) -or (Get-Sha $icdCopy) -ne $icdSha) { Copy-Item -LiteralPath $IcdPath -Destination $icdCopy -Force }
        if (-not (Test-Path -LiteralPath $directCopy) -or (Get-Sha $directCopy) -ne (Get-Sha $directSrc)) { Copy-Item -LiteralPath $directSrc -Destination $directCopy -Force }
        if ((Get-Sha $icdCopy) -ne $icdSha) { throw 'stop: ICD copy differs from the source' }
        $libraryArg = [IO.Path]::GetFullPath($directCopy)
        $icdWitnessPath = [IO.Path]::GetFullPath($icdCopy)
    }

    # A batch keeps one configuration across its attempts; a different ICD needs a different -RunId.
    $configPath = Join-Path $batchDir 'config.json'
    $config = [ordered]@{ route = $Route; icd_source = $IcdPath; icd_sha256 = $icdSha; experimental = $Experimental
        perftest = $Perftest; deqp_sha256 = (Get-Sha $deqp); list_sha256 = (Get-Sha $listPath); list_count = $cases.Count }
    if (Test-Path -LiteralPath $configPath) {
        $old = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
        foreach ($k in @('route', 'icd_sha256', 'experimental', 'perftest', 'deqp_sha256', 'list_sha256')) {
            if ([string]$old.$k -ne [string]$config[$k]) { throw "stop: $k changed against $configPath; use a new -RunId" }
        }
    } else { Write-Json $configPath $config }

    # --------------------------------------------------------------------------------- resume state
    $done = @{}
    if (Test-Path -LiteralPath $casesTsv) {
        foreach ($line in [IO.File]::ReadAllLines($casesTsv)) {
            $f = $line.Split("`t")
            if ($f.Count -ge 2 -and $f[0] -ne 'case') { $done[$f[0]] = $f[1] }
        }
    } else { Write-Text $casesTsv "case`tstatus`tattempt`tduration_us`tdetails`n" }
    $remaining = @($cases | Where-Object { -not $done.ContainsKey($_) })
    $attempt = 1 + @(Get-ChildItem -LiteralPath $batchDir -Filter 'attempt-*.caselist.txt' -ErrorAction SilentlyContinue).Count
    $stem = Join-Path $batchDir ('attempt-{0:D2}' -f $attempt)
    $info = [ordered]@{ batch = $batchName; attempt = $attempt; start_utc = (Utc); remaining_before = $remaining.Count
        route = $Route; icd_sha256 = $icdSha; bound_s = $BoundSeconds; host = $env:COMPUTERNAME }
    # The deferred ICDs read this machine-wide knob file (scratch\m15\native-caps001\icd-cfg.ps1); record it.
    $icdCfg = 'C:\BC250\tmp\amdgpu_wddm_radv.cfg'
    if (Test-Path -LiteralPath $icdCfg) { $info.icd_cfg = ([IO.File]::ReadAllText($icdCfg) -replace "`r?`n", ' ').Trim() } else { $info.icd_cfg = 'absent' }
    $ran = $false

    if ($remaining.Count -gt 0) {
        if (Test-OwnerStop) { throw 'stop: owner STOP flag set' }
        $t0 = Read-Tctl
        if ($t0 -ne $null -and $t0 -ge $ThermalStopC) { throw "stop: Tctl $t0 C before start" }
        $info.tctl_start = $t0

        [IO.File]::WriteAllLines("$stem.caselist.txt", [string[]]$remaining, $utf8)
        $qpaPath = "$stem.qpa"
        $argList = @("--deqp-caselist-file=$stem.caselist.txt", "--deqp-log-filename=$qpaPath", "--deqp-archive-dir=$bin",
            '--deqp-log-images=disable', '--deqp-log-shader-sources=disable', '--deqp-shadercache=disable',
            '--deqp-watchdog=enable', '--deqp-crashhandler=enable', '--deqp-terminate-on-device-lost=enable')
        if ($libraryArg) { $argList += "--deqp-vk-library-path=$libraryArg" }
        $psi = New-Object Diagnostics.ProcessStartInfo
        $psi.FileName = $deqp
        $psi.Arguments = ($argList | ForEach-Object { '"' + $_ + '"' }) -join ' '
        $psi.WorkingDirectory = $bin; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
        foreach ($n in @('VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_ADD_DRIVER_FILES', 'VK_LOADER_DRIVERS_SELECT',
                'VK_LOADER_DRIVERS_DISABLE', 'VK_INSTANCE_LAYERS', 'VK_LOADER_LAYERS_ENABLE', 'VK_LAYER_PATH')) {
            if ($psi.EnvironmentVariables.ContainsKey($n)) { $psi.EnvironmentVariables.Remove($n) }
        }
        $psi.EnvironmentVariables['RADV_EXPERIMENTAL'] = $Experimental
        if ($psi.EnvironmentVariables.ContainsKey('RADV_PERFTEST')) { $psi.EnvironmentVariables.Remove('RADV_PERFTEST') }
        if ($Perftest) { $psi.EnvironmentVariables['RADV_PERFTEST'] = $Perftest }
        $info.arguments = $argList

        $outFile = [IO.File]::Create("$stem.stdout.txt"); $errFile = [IO.File]::Create("$stem.stderr.txt")
        $child = $null; $killed = $null; $witness = $false; $modules = @{}; $copyOut = $null; $copyErr = $null
        try {
            $child = [Diagnostics.Process]::Start($psi)
            $ran = $true
            $info.pid = $child.Id
            $copyOut = $child.StandardOutput.BaseStream.CopyToAsync($outFile)
            $copyErr = $child.StandardError.BaseStream.CopyToAsync($errFile)
            $nextThermal = $clock.Elapsed.TotalSeconds + [Math]::Max($ThermalCheckSeconds, 5)
            $nextStop = $clock.Elapsed.TotalSeconds + 20
            while (-not $child.HasExited) {
                $now = $clock.Elapsed.TotalSeconds
                if ($now -ge $BoundSeconds) { $killed = 'bound'; break }
                if (-not $witness) {
                    try {
                        $child.Refresh()   # Process.Modules is cached until Refresh()
                        foreach ($m in $child.Modules) {
                            $fn = $m.FileName
                            if ($fn -match $IcdModulePattern -or $fn -match '(?i)vulkan-1|cts-direct') { $modules[$fn] = 1 }
                            if ($icdWitnessPath -and $fn -ieq $icdWitnessPath) { $witness = $true }
                        }
                        if (-not $icdWitnessPath -and @($modules.Keys | Where-Object { $_ -match $IcdModulePattern }).Count) { $witness = $true }
                    } catch {}
                }
                if ($ThermalCheckSeconds -gt 0 -and $now -ge $nextThermal) {
                    $tc = Read-Tctl; $info.tctl_last = $tc
                    if ($tc -ne $null -and $tc -ge $ThermalStopC) { $killed = 'thermal'; break }
                    $nextThermal = $now + $ThermalCheckSeconds
                }
                if ($now -ge $nextStop) {
                    if (Test-OwnerStop) { $killed = 'owner-stop'; break }
                    $nextStop = $now + 20
                }
                $null = $child.WaitForExit(200)
            }
        } finally {
            if ($child -and -not $child.HasExited) {
                # Tree kill; taskkill's own complaints (e.g. an already exiting conhost) are not errors here.
                try {
                    $kpsi = New-Object Diagnostics.ProcessStartInfo
                    $kpsi.FileName = "$env:windir\System32\taskkill.exe"; $kpsi.Arguments = "/PID $($child.Id) /T /F"
                    $kpsi.UseShellExecute = $false; $kpsi.CreateNoWindow = $true
                    $kpsi.RedirectStandardOutput = $true; $kpsi.RedirectStandardError = $true
                    $k = [Diagnostics.Process]::Start($kpsi)
                    $kout = $k.StandardOutput.ReadToEndAsync(); $kerr = $k.StandardError.ReadToEndAsync()
                    $null = $k.WaitForExit(10000)
                    $info.taskkill = ($kout.Result + $kerr.Result).Trim()
                } catch { $info.taskkill = $_.Exception.Message }
                if (-not $child.WaitForExit(5000)) { try { $child.Kill() } catch {}; $null = $child.WaitForExit(5000) }
            }
            try { if ($copyOut) { $null = $copyOut.Wait(3000) }; if ($copyErr) { $null = $copyErr.Wait(3000) } } catch {}
            $outFile.Dispose(); $errFile.Dispose()
        }
        $info.exit_code = $child.ExitCode
        $info.killed = $killed
        $info.icd_witness = $witness
        $info.modules = @($modules.Keys)
        $info.deqp_seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 1)

        # ------------------------------------------------------------------------------- parse qpa
        $records = New-Object Collections.ArrayList
        $deviceName = $null
        $sessionEnded = $false
        if (Test-Path -LiteralPath $qpaPath) {
            $fs = [IO.File]::Open($qpaPath, 'Open', 'Read', 'ReadWrite')
            $rd = New-Object IO.StreamReader($fs, $utf8)
            $cur = $null
            try {
                while ($null -ne ($line = $rd.ReadLine())) {
                    if ($line.StartsWith('#sessionInfo deviceName ')) { $deviceName = $line.Substring(24).Trim() }
                    elseif ($line.StartsWith('#endSession')) { $sessionEnded = $true }
                    elseif ($line.StartsWith('#beginTestCaseResult ')) {
                        if ($cur) { [void]$records.Add($cur) }
                        $cur = @{ case = $line.Substring(21).Trim(); status = $null; details = ''; us = ''; ended = $false }
                    } elseif ($line.StartsWith('#endTestCaseResult')) {
                        if ($cur) { $cur.ended = $true; if (-not $cur.status) { $cur.status = 'NoResult' }; [void]$records.Add($cur); $cur = $null }
                    } elseif ($line.StartsWith('#terminateTestCaseResult')) {
                        if ($cur) {
                            $r = $line.Substring(24).Trim(); if (-not $r) { $r = 'Crash' }
                            $cur.ended = $true; $cur.status = $r; $cur.details = 'terminated'; [void]$records.Add($cur); $cur = $null
                        }
                    } elseif ($cur) {
                        if ($line -match '<Result StatusCode="([^"]+)">([^<]*)</Result>') { $cur.status = $Matches[1]; $cur.details = $Matches[2].Trim() }
                        elseif ($line -match '<Number Name="TestDuration"[^>]*>([0-9.]+)</Number>') { $cur.us = $Matches[1] }
                    }
                }
                if ($cur) { [void]$records.Add($cur) }
            } finally { $rd.Dispose() }
        }
        $info.device_name = $deviceName

        $begun = $records.Count
        $deviceLost = $false
        $timedOut = $false
        $sb = New-Object Text.StringBuilder
        foreach ($r in $records) {
            $status = $r.status
            if (-not $r.ended) {
                if ($killed -eq 'bound') {
                    # Retried once from the start of the next attempt; the second time it had the full budget.
                    if ($begun -eq 1) { $status = 'Timeout'; $r.details = "exceeded the $BoundSeconds s bound alone" } else { continue }
                } elseif ($killed) { continue }
                else { $status = 'Crash'; $r.details = ('process exit 0x{0:X8}' -f $child.ExitCode) }
            }
            if ($status -eq 'DeviceLost' -or $r.details -match '(?i)device.?lost') { $deviceLost = $true }
            if ($status -eq 'Timeout') { $timedOut = $true }
            $d = ($r.details -replace "[`t`r`n]", ' ')
            if ($d.Length -gt 200) { $d = $d.Substring(0, 200) }
            [void]$sb.Append("$($r.case)`t$status`t$attempt`t$($r.us)`t$d`n")
            $done[$r.case] = $status
        }
        # A session that ended normally (#endSession) without reaching a listed case: the binary does not
        # have it. After a crash, a watchdog termination or a kill the rest is simply run next time.
        if ($sessionEnded -and -not $killed -and -not $deviceLost) {
            $began = @{}; foreach ($r in $records) { $began[$r.case] = 1 }
            foreach ($c in $remaining) {
                if (-not $began.ContainsKey($c)) { [void]$sb.Append("$c`tNotRun`t$attempt`t`tnot executed by deqp-vk (exit $($child.ExitCode))`n"); $done[$c] = 'NotRun' }
            }
        }
        Append-Text $casesTsv $sb.ToString()
        $info.cases_begun = $begun
        $info.session_ended = $sessionEnded

        if ($killed -eq 'thermal') { $stopReason = "thermal: Tctl $($info.tctl_last) C" }
        elseif ($killed -eq 'owner-stop') { $stopReason = 'owner STOP flag' }
        elseif ($deviceLost) { $stopReason = 'device lost' }
        elseif ($timedOut) { $stopReason = 'a case timed out (watchdog or bound): check the GPU state before the next call' }
        elseif ($ExpectDevice -and $deviceName -and $deviceName -notlike "*$ExpectDevice*") { $stopReason = "device '$deviceName' is not '$ExpectDevice'" }
        elseif ($begun -gt 0 -and -not $witness -and $Route -eq 'registered') { $stopReason = "ICD witness missing (no module matching $IcdModulePattern seen in deqp-vk)" }
        elseif ($begun -eq 0 -and -not $killed) { $stopReason = "deqp-vk ran no case (exit 0x{0:X8}); see $stem.stderr.txt" -f $child.ExitCode }
    }

    # ------------------------------------------------------------------------------------ summary
    $counts = [ordered]@{}
    $bad = New-Object Collections.ArrayList
    $warn = New-Object Collections.ArrayList
    foreach ($c in $cases) {
        if (-not $done.ContainsKey($c)) { continue }
        $s = $done[$c]
        if ($counts.Contains($s)) { $counts[$s]++ } else { $counts[$s] = 1 }
        if ($s -eq 'QualityWarning' -or $s -eq 'CompatibilityWarning') { [void]$warn.Add("$s $c") }
        elseif ($s -ne 'Pass' -and $s -ne 'NotSupported') { [void]$bad.Add("$s $c") }
    }
    $left = @($cases | Where-Object { -not $done.ContainsKey($_) }).Count
    $complete = ($left -eq 0)
    if ($stopReason) { $exitCode = 3 } elseif (-not $complete) { $exitCode = 2 } elseif ($bad.Count) { $exitCode = 1 } else { $exitCode = 0 }
    $info.end_utc = Utc
    $info.elapsed_s = [Math]::Round($clock.Elapsed.TotalSeconds, 1)
    $info.stop_reason = $stopReason
    $info.exit = $exitCode
    if ($ran) { Write-Json "$stem.json" $info }
    $summary = [ordered]@{ batch = $batchName; list = $listPath; total = $cases.Count; done = $cases.Count - $left
        remaining = $left; complete = $complete; counts = $counts; failing = @($bad); warnings = @($warn)
        attempts = $attempt - [int](-not $ran); last_attempt = $info; route = $Route; icd_sha256 = $icdSha; run_id = $RunId
        stop_reason = $stopReason; exit = $exitCode; utc = (Utc) }
    Write-Json (Join-Path $batchDir 'summary.json') $summary
    $txt = New-Object Text.StringBuilder
    [void]$txt.AppendLine("$batchName run=$RunId route=$Route icd=$($icdSha.Substring(0, [Math]::Min(8, $icdSha.Length))) exit=$exitCode complete=$complete done=$($cases.Count - $left)/$($cases.Count) attempts=$($summary.attempts) elapsed=$($info.elapsed_s)s")
    [void]$txt.AppendLine('counts: ' + (($counts.Keys | ForEach-Object { "$_=$($counts[$_])" }) -join ' '))
    if ($stopReason) { [void]$txt.AppendLine("STOP: $stopReason") }
    foreach ($b in $bad) { [void]$txt.AppendLine("FAIL $b") }
    foreach ($w in $warn) { [void]$txt.AppendLine("WARN $w") }
    Write-Text (Join-Path $batchDir 'summary.txt') $txt.ToString()
    Write-Output $txt.ToString().TrimEnd()
} catch {
    $msg = $_.Exception.Message
    Write-Output ("stop: " + $msg)
    try { Write-Json (Join-Path $batchDir 'error.json') @{ utc = (Utc); message = $msg; elapsed_s = $clock.Elapsed.TotalSeconds } } catch {}
    $exitCode = 3
} finally {
    Remove-Item -LiteralPath $lockPath -Force -ErrorAction SilentlyContinue
}
exit $exitCode

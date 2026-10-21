# run-oversize-ib.ps1 - the lab probe of F-q44-mem-4: a test client's oversized IB is refused for its own device only,
# while another process keeps the gfx ring busy, node 0 stays open and the desktop stays up.
#
# Runs ON THE TARGET (Windows PowerShell 5.1, elevated SSH session, session 0), after monfence.exe and gpuload.exe have
# been pushed to -Dir (default C:\BC250\tmp\oversize-ib), for example
#   python tools\win\target.py ps tools\win\monfence\run-oversize-ib.ps1 -ExpectKmdSha256 <sha256>
#
# It refuses to start unless the KMD image the service loads is -ExpectKmdSha256 (or -AnyKmd is given). On a KMD
# without the IB bound of umd_blob.c the oversized IB reaches the ring refusal, which closes node 0 for every process
# and needs a recovery (wddm.c, WddmFailSubmission): that is the defect, not a probe outcome to collect by accident.
#
# Steps, every one bounded, the whole run under a 170 s deadline (inside the three-minute lab bound):
#   1. before: boot time, DWM's process ids, bc250kmd_cli `log` for its last sequence number      (15 s)
#   2. gpuload --seconds -LoadSeconds in the background: the second client, Vulkan compute on the gfx ring, in its
#      own process and device; the probe goes on only once it reports load (its first one-second line, 15 s)
#   3. monfence --oversize-ib: O.good (a packet control on its device), one IB one dword over IB_SIZE,
#      O.refused (its device leaves the active state, without a reset)                           (35 s)
#   4. monfence, phase S only, a new process and device: node 0 still runs work                     (35 s)
#   5. gpuload's end: exit 0 and result=ok, every batch checked while the refusal happened  (-LoadSeconds + 15 s)
#   6. after: bc250kmd_cli `log <next>`, Display 4101 events since step 1, boot time and DWM unchanged (15 s)
#   7. one VERDICT line; exit 0 PASS, 1 FAIL, 3 not run (setup, KMD image, or the load did not start).
# Every line goes to probe.log at once (one append per line), so a lost SSH session still leaves the record.
#
# It writes only below C:\BC250\tmp, starts nothing that outlives it (gpuload gets its stop file, then a kill), and
# changes no clock, registry value, KMD state or overlay setting.

param(
    [string]$Dir = 'C:\BC250\tmp\oversize-ib',
    [string]$Cli = '',
    [string]$ExpectKmdSha256 = '',
    [switch]$AnyKmd,
    [ValidateRange(20, 90)][int]$LoadSeconds = 50
)

$ErrorActionPreference = 'Stop'

# UMD_BLOB_BAD_IB (driver/kmd/umd_blob.h), the "blob" number of the KMD's refusal line. build.ps1 checks this line.
$BadIbCode = 10

$started = Get-Date
$deadline = $started.AddSeconds(170)

function Assert-UnderTmp([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith('C:\BC250\tmp\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to write outside C:\BC250\tmp: $full"
    }
    return $full
}

$stamp = $started.ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$run = Assert-UnderTmp (Join-Path $Dir "run-$stamp")
New-Item -ItemType Directory -Force -Path $run | Out-Null
$log = Join-Path $run 'probe.log'
function Say([string]$Text) {
    Add-Content -LiteralPath $log -Value $Text -Encoding ASCII
    Write-Output $Text
}

function Remaining() { return [int][math]::Floor(($deadline - (Get-Date)).TotalSeconds) }

function Invoke-Bounded([string]$File, [string[]]$Arguments, [string]$StdOut, [int]$Seconds) {
    $Seconds = [math]::Max(1, [math]::Min($Seconds, (Remaining)))
    $sp = @{ FilePath = $File; RedirectStandardOutput = $StdOut; RedirectStandardError = "$StdOut.err"; PassThru = $true; NoNewWindow = $true }
    if ($Arguments -and $Arguments.Count -gt 0) { $sp.ArgumentList = $Arguments }
    $t0 = Get-Date
    $p = Start-Process @sp
    $null = $p.Handle                       # keeps ExitCode readable after exit (Windows PowerShell quirk)
    $exited = $p.WaitForExit($Seconds * 1000)
    $result = @{ Exited = $exited; Code = $null; Killed = $false; Seconds = 0.0 }
    if (-not $exited) {
        try { $p.Kill() } catch { }
        $result.Killed = $true
        $result.Exited = $p.WaitForExit(10000)
    }
    if ($result.Exited -and -not $result.Killed) { $result.Code = $p.ExitCode }
    $result.Seconds = [math]::Round(((Get-Date) - $t0).TotalSeconds, 2)
    return $result
}

# Also for a file another process is still writing (gpuload's output): read with every sharing mode, never throw.
function Read-Lines([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    try {
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try { return @((New-Object System.IO.StreamReader($fs)).ReadToEnd() -split "`r?`n" | Where-Object { $_ -ne '' }) }
        finally { $fs.Dispose() }
    } catch { return @() }
}

function Get-Hash([string]$Path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash }

function Get-BootTime() { return (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('s') }

function Get-DwmIds() { return (@(Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) | Sort-Object) -join ',' }

function Get-Display4101([datetime]$Since) {
    try {
        return @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Display'; Id = 4101; StartTime = $Since } -ErrorAction Stop).Count
    } catch {
        if ($_.FullyQualifiedErrorId -match 'NoMatchingEventsFound') { return 0 }
        return -1
    }
}

$monfence = Join-Path $Dir 'monfence.exe'
$gpuload = Join-Path $Dir 'gpuload.exe'
$stopFile = Join-Path $run 'gpuload.stop'
$load = $null

Say "OVERSIZE-IB start utc=$stamp dir=$run load_s=$LoadSeconds deadline_s=170"
foreach ($f in @($monfence, $gpuload)) {
    if (-not (Test-Path -LiteralPath $f)) { Say "OVERSIZE-IB not run: $f not found"; Say 'VERDICT NOT-RUN setup'; exit 3 }
    Say "client $f sha256=$(Get-Hash $f)"
}
if (-not $Cli) {
    foreach ($c in @('C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe', 'C:\BC250\kmd\bc250kmd_cli.exe', 'C:\BC250\m8\bc250kmd_cli.exe')) {
        if (Test-Path -LiteralPath $c) { $Cli = $c; break }
    }
}
if (-not $Cli -or -not (Test-Path -LiteralPath $Cli)) { Say 'OVERSIZE-IB not run: no bc250kmd_cli.exe (give -Cli)'; Say 'VERDICT NOT-RUN setup'; exit 3 }
Say "cli $Cli sha256=$(Get-Hash $Cli)"

$ip = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd' -Name ImagePath).ImagePath
$sys = $ip -replace '^\\SystemRoot\\', "$env:SystemRoot\" -replace '^\\\?\?\\', '' -replace '^System32\\', "$env:SystemRoot\System32\"
$kmdHash = Get-Hash $sys
Say "kmd image $sys sha256=$kmdHash"
if (-not $AnyKmd) {
    if (-not $ExpectKmdSha256) { Say 'OVERSIZE-IB not run: give -ExpectKmdSha256 (the KMD with the IB bound) or -AnyKmd'; Say 'VERDICT NOT-RUN kmd'; exit 3 }
    if ($kmdHash -ne $ExpectKmdSha256.ToUpperInvariant()) { Say "OVERSIZE-IB not run: the KMD image is not $ExpectKmdSha256"; Say 'VERDICT NOT-RUN kmd'; exit 3 }
}

$failed = New-Object System.Collections.Generic.List[string]
try {
    # 1. before
    $bootBefore = Get-BootTime
    $dwmBefore = Get-DwmIds
    Say "before: boot=$bootBefore dwm=$dwmBefore"
    $r = Invoke-Bounded $Cli @('log') (Join-Path $run 'log-before.txt') 15
    $logFrom = 0
    foreach ($l in (Read-Lines (Join-Path $run 'log-before.txt'))) {
        if ($l -match '^\s*(\d+)\s+\d+\.\d{3} ') { $seq = [int64]$Matches[1]; if ($seq -ge $logFrom) { $logFrom = $seq + 1 } }
    }
    Say "log before: exit=$($r.Code) killed=$($r.Killed) s=$($r.Seconds) next=$logFrom"
    if ($r.Code -ne 0 -or $logFrom -eq 0) { Say 'OVERSIZE-IB not run: the KMD log is unreadable'; Say 'VERDICT NOT-RUN log'; exit 3 }

    # 2. the second client
    $loadOut = Join-Path $run 'gpuload.out'
    $load = Start-Process -FilePath $gpuload -ArgumentList @('--seconds', "$LoadSeconds", '--stop-file', $stopFile) `
        -RedirectStandardOutput $loadOut -RedirectStandardError "$loadOut.err" -PassThru -NoNewWindow
    $null = $load.Handle
    $loadStarted = Get-Date
    # Device creation and calibration come first; the load runs once it prints its first one-second line.
    $loadLines = @()
    while (-not $load.HasExited -and $loadLines.Count -eq 0 -and ((Get-Date) - $loadStarted).TotalSeconds -lt 15) {
        Start-Sleep -Seconds 1
        $loadLines = @(Read-Lines $loadOut | Where-Object { $_ -match '^gpuload t=' })
    }
    Say "load: pid=$($load.Id) running=$(-not $load.HasExited) progress_lines=$($loadLines.Count)"
    if ($load.HasExited -or $loadLines.Count -eq 0) {
        Say 'OVERSIZE-IB not run: the second client is not running'
        Read-Lines $loadOut | Select-Object -Last 3 | ForEach-Object { Say "  | $_" }
        Say 'VERDICT NOT-RUN load'
        exit 3
    }

    # 3. the oversized IB
    $oOut = Join-Path $run 'monfence-oversize.out'
    $r = Invoke-Bounded $monfence @('--oversize-ib', '--watchdog-s', '25') $oOut 35
    $o = Read-Lines $oOut
    foreach ($l in $o) { if ($l -match '^(CHECK|NOTE|RESULT|PHASE) ') { Say "  | $l" } }
    Say "oversize client: exit=$($r.Code) killed=$($r.Killed) s=$($r.Seconds)"
    if ($r.Code -ne 0) { $failed.Add("oversize client exit $($r.Code) killed $($r.Killed)") }
    if (-not ($o | Where-Object { $_ -match '^CHECK O\.good PASS' })) { $failed.Add('O.good not PASS') }
    if (-not ($o | Where-Object { $_ -match '^CHECK O\.refused PASS' })) { $failed.Add('O.refused not PASS') }

    # 4. node 0 still runs a new device's work
    $cOut = Join-Path $run 'monfence-control.out'
    $r = Invoke-Bounded $monfence @('--skip-mt', '--skip-latency', '--watchdog-s', '25') $cOut 35
    $c = Read-Lines $cOut
    foreach ($l in $c) { if ($l -match '^(CHECK|RESULT) ') { Say "  | $l" } }
    Say "control client: exit=$($r.Code) killed=$($r.Killed) s=$($r.Seconds)"
    if ($r.Code -ne 0) { $failed.Add("control client exit $($r.Code) killed $($r.Killed)") }

    # 5. the second client's end
    $left = [int](($loadStarted.AddSeconds($LoadSeconds + 15) - (Get-Date)).TotalSeconds)
    $left = [math]::Max(1, [math]::Min($left, (Remaining) - 25))
    $loadExited = $load.WaitForExit($left * 1000)
    $loadCode = $null
    if ($loadExited) { $loadCode = $load.ExitCode }
    $loadResult = @(Read-Lines $loadOut | Where-Object { $_ -match '^gpuload result=' })
    $resultText = 'none'
    if ($loadResult.Count -gt 0) { $resultText = $loadResult[-1] }
    Say "load: exited=$loadExited exit=$loadCode $resultText"
    if ($loadCode -ne 0 -or $resultText -notmatch 'result=ok') { $failed.Add("second client exit $loadCode, $resultText") }

    # 6. after
    $r = Invoke-Bounded $Cli @('log', "$logFrom") (Join-Path $run 'log-after.txt') 15
    $after = Read-Lines (Join-Path $run 'log-after.txt')
    Say "log after (from $logFrom): exit=$($r.Code) killed=$($r.Killed) s=$($r.Seconds) lines=$($after.Count)"
    $refusals = @($after | Where-Object { $_ -match "wddm: malformed virtual UMD submission fence \d+ rejected \(blob $BadIbCode, single 0, node 0\)" })
    $closures = @($after | Where-Object { $_ -match 'NOT dispatched; node closed|gfx engine fault|wddm: timeout seq' })
    foreach ($l in ($refusals + $closures)) { Say "  kmd | $l" }
    Say "kmd: refusals=$($refusals.Count) node_closures=$($closures.Count)"
    if ($r.Code -ne 0) { $failed.Add("log after exit $($r.Code)") }
    if ($refusals.Count -ne 1) { $failed.Add("$($refusals.Count) refusal lines, expected 1") }
    if ($closures.Count -ne 0) { $failed.Add("$($closures.Count) node-closure or timeout lines") }
    $tdr = Get-Display4101 $started
    $bootAfter = Get-BootTime
    $dwmAfter = Get-DwmIds
    Say "after: display_4101=$tdr boot=$bootAfter dwm=$dwmAfter"
    if ($tdr -ne 0) { $failed.Add("Display 4101 events: $tdr") }
    if ($bootAfter -ne $bootBefore) { $failed.Add('boot time changed') }
    if (-not $dwmAfter -or $dwmAfter -ne $dwmBefore) { $failed.Add("DWM $dwmBefore -> $dwmAfter") }
}
finally {
    if ($load -and -not $load.HasExited) {
        New-Item -ItemType File -Force -Path $stopFile | Out-Null
        if (-not $load.WaitForExit(5000)) { try { $load.Kill() } catch { } }
        Say "load: stopped by the runner (exit $(if ($load.HasExited) { $load.ExitCode } else { 'none' }))"
    }
    if (Test-Path -LiteralPath $stopFile) { Remove-Item -LiteralPath $stopFile -Force }
}

$elapsed = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
if ($failed.Count -eq 0) {
    Say "VERDICT PASS elapsed_s=${elapsed}: refused for its device only, node 0 open, the second client ran through, no TDR"
    exit 0
}
Say "VERDICT FAIL elapsed_s=${elapsed}: $($failed -join '; ')"
exit 1

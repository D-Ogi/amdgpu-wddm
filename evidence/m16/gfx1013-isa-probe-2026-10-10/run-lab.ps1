# Runs dotprobe.exe on unit A: one process, one code object, one kernel, every instruction in
# the order of least risk.
#
#   python bc250-win\tools\win\target.py ps scratch\gfx1013-dot-probe\run-lab.ps1
#   python bc250-win\tools\win\target.py ps scratch\gfx1013-dot-probe\run-lab.ps1 -Op v_dot4_i32_i8
#
# It installs nothing and changes no setting. It reads the kernel driver's health line, the GPU
# clock line and the driver's cumulative counter block (faults, timeouts, resets) before and
# after, and it bounds the process with a hard deadline.
#
# The program itself names each instruction in its result log, on disk and flushed, before it
# launches that instruction, so after a hang the last LAUNCH line in the log is the culprit and
# the last RESULT line is the deepest instruction that answered.

param(
    [string]$Dir = 'C:\BC250\tmp\gfx1013-dot',
    [string]$Op = '',
    [int]$DeadlineSec = 120,
    [int]$WaitTotalMs = 8000,
    [string]$Cli = ''
)
$ErrorActionPreference = 'Continue'

$exe = Join-Path $Dir 'dotprobe.exe'
if (-not (Test-Path -LiteralPath $exe)) { "runner: $exe ABSENT"; exit 2 }
if (-not (Test-Path -LiteralPath (Join-Path $Dir 'amdhip64.dll'))) {
    "runner: $(Join-Path $Dir 'amdhip64.dll') ABSENT"; exit 2
}

function Resolve-Cli([string]$Path) {
    if ($Path) { return $Path }
    if ($env:BC250_KMD_CLI) { return $env:BC250_KMD_CLI }
    $rel = Get-ItemProperty -Path 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name 'InstallRoot' -ErrorAction SilentlyContinue
    if ($rel) {
        $p = Join-Path $rel.InstallRoot 'tools\bc250kmd_cli.exe'
        if (Test-Path -LiteralPath $p) { return $p }
    }
    $found = @(Get-ChildItem -LiteralPath 'C:\BC250' -Recurse -Depth 2 -Filter 'bc250kmd_cli.exe' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($found.Count) { return $found[0].FullName }
    return ''
}
$Cli = Resolve-Cli $Cli
if (-not $Cli) { 'runner: no bc250kmd_cli.exe found; health and clock cannot be read' }

function Utc() { [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ') }

function Read-ClockLine() {
    if (-not $Cli) { return 'cli ABSENT' }
    return ((& $Cli clock read 2>&1 | Out-String) -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 1).Trim()
}

function Read-Tctl() {
    $line = Read-ClockLine
    if ($line -match 'temperature_mc=(\d+)') { return [double]$Matches[1] / 1000 }
    if ($line -match '\s([0-9]+\.[0-9])\s*C\s') { return [double]$Matches[1] }
    return $null
}

function Read-HealthLine() {
    if (-not $Cli) { return 'cli ABSENT' }
    return ((& $Cli health read 2>&1 | Out-String) -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 1).Trim()
}

function Read-DpmLine() {
    if (-not $Cli) { return 'cli ABSENT' }
    return ((& $Cli dpm 2>&1 | Out-String) -split "`r?`n" | Where-Object { $_ -match 'cap \d+ max \d+' } | Select-Object -First 1).Trim()
}

# The driver's cumulative counter block (docs/design/kmd-log-ring.md). "log summary only"
# answers the block beside the ring and costs one escape.
function Read-Summary() {
    if (-not $Cli) { return @('cli ABSENT') }
    $lines = @(& $Cli log summary only 2>&1 | ForEach-Object { "$_" } |
        Where-Object { $_ -match '^\s*\d+\s+\d+\.\d{3} ' })
    $starts = @(for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match 'wddm summary: node 0 hardware') { $i } })
    if ($starts.Count -ge 1) {
        return @($lines[$starts[$starts.Count - 1]..($lines.Count - 1)] |
            Where-Object { $_ -notmatch ' gfx: (job|VMID)' })
    }
    return @("no summary block (lines $($lines.Count))")
}

function Summary-Counters([string[]]$Block) {
    $wanted = @($Block | Where-Object { $_ -match 'fault|timeout|reset|hang|TDR' })
    if ($wanted.Count -eq 0) { return '(no fault, timeout or reset line in the block)' }
    return ($wanted -join ' | ')
}

$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$logDir = Join-Path $Dir "run-$stamp"
New-Item -ItemType Directory -Force $logDir | Out-Null
$resultLog = Join-Path $logDir 'dotprobe-results.txt'

"runner: utc $(Utc) dir $Dir deadline $DeadlineSec s wait-total $WaitTotalMs ms"
"runner: cli $Cli"
"runner: exe sha256 $((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash)"
"runner: amdhip64 sha256 $((Get-FileHash -LiteralPath (Join-Path $Dir 'amdhip64.dll') -Algorithm SHA256).Hash)"
"runner: boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))"

$beforeBlock = Read-Summary
Set-Content -LiteralPath (Join-Path $logDir 'summary-before.txt') -Value $beforeBlock -Encoding UTF8
$beforeCounters = Summary-Counters $beforeBlock
$healthBefore = Read-HealthLine
$clockBefore = Read-ClockLine
"runner: health before   $healthBefore"
"runner: clock  before   $clockBefore"
"runner: dpm    before   $(Read-DpmLine)"
"runner: counters before $beforeCounters"

$tBefore = Read-Tctl
if ($null -ne $tBefore -and $tBefore -ge 87) { "runner: REFUSED, Tctl $tBefore C is at or above 87 C"; exit 3 }

$argline = "--log `"$resultLog`" --wait-total $WaitTotalMs"
if ($Op) { $argline = "--op $Op " + $argline }
"runner: command dotprobe.exe $argline"
"runner: start $(Utc)"

# System.Diagnostics.Process rather than Start-Process: Start-Process -PassThru leaves ExitCode
# null after a timed WaitForExit (MEASURED on the development PC, 2026-10-10), and the exit code
# is part of the answer.
$sw = [Diagnostics.Stopwatch]::StartNew()
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = $argline
$psi.WorkingDirectory = $Dir
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$proc = [System.Diagnostics.Process]::Start($psi)
# Both pipes are read asynchronously: a full pipe would block the child, and the deadline would
# then measure our own deadlock instead of the device.
$stdoutTask = $proc.StandardOutput.ReadToEndAsync()
$stderrTask = $proc.StandardError.ReadToEndAsync()
$exited = $proc.WaitForExit($DeadlineSec * 1000)
$sw.Stop()

$code = $null
$status = ''
if (-not $exited) {
    "runner: PAST THE DEADLINE after $DeadlineSec s: killing the process"
    try { $proc.Kill() } catch { "runner: kill failed: $_" }
    try { [void]$proc.WaitForExit(5000) } catch { }
    $status = 'HANG'
} else {
    $code = $proc.ExitCode
    switch ($code) {
        0 { $status = 'ALL CORRECT' }
        3 { $status = 'AT LEAST ONE WRONG RESULT' }
        1 { $status = 'API FAILURE' }
        2 { $status = 'USAGE' }
        4 { $status = 'NOT IN THIS BUILD' }
        default { $status = "EXIT $code" }
    }
}

$text = @()
try { $text += @(($stdoutTask.Result -split "`r?`n") | Where-Object { $_ -ne '' }) } catch { }
try { $text += @(($stderrTask.Result -split "`r?`n") | Where-Object { $_ -ne '' } |
        ForEach-Object { "stderr: $_" }) } catch { }
Set-Content -LiteralPath (Join-Path $logDir 'stdout.txt') -Value $text -Encoding UTF8
$text | ForEach-Object { "| $_" }

"runner: $status after $([math]::Round($sw.Elapsed.TotalSeconds,2)) s, exit $code"

# After a hang, the result log on disk is the record: the last LAUNCH line names the instruction
# that did not come back.
if (Test-Path -LiteralPath $resultLog) {
    $logged = @(Get-Content -LiteralPath $resultLog)
    $lastLaunch = @($logged | Where-Object { $_ -match '^LAUNCH ' }) | Select-Object -Last 1
    $lastResult = @($logged | Where-Object { $_ -match '^RESULT ' }) | Select-Object -Last 1
    "runner: last LAUNCH  $lastLaunch"
    "runner: last RESULT  $lastResult"
    "runner: RESULT lines $(@($logged | Where-Object { $_ -match '^RESULT ' }).Count) of 12"
} else {
    "runner: the result log $resultLog was never created"
}

$afterBlock = Read-Summary
Set-Content -LiteralPath (Join-Path $logDir 'summary-after.txt') -Value $afterBlock -Encoding UTF8
$afterCounters = Summary-Counters $afterBlock
$healthAfter = Read-HealthLine
"runner: health after    $healthAfter"
"runner: clock  after    $(Read-ClockLine)"
"runner: dpm    after    $(Read-DpmLine)"
"runner: counters after  $afterCounters"
"runner: health changed   $($healthAfter -ne $healthBefore)"
"runner: counters changed $($afterCounters -ne $beforeCounters)"
"runner: logs $logDir"
"runner: end $(Utc)"
exit 0

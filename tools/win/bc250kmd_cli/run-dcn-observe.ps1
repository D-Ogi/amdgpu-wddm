param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedSha256,
    [Parameter(Mandatory)][ValidatePattern('^0x[0-9a-fA-F]{8}$')][string]$ExpectedVersion,
    [Parameter(Mandatory)][string]$Out,
    [ValidateRange(1,40)][int]$Samples = 20,
    [ValidateRange(100,500)][int]$IntervalMs = 250
)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run elevated on the lab' }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$actual = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
if ($actual -ne $ExpectedSha256) { throw 'Observer hash mismatch' }
if (Test-Path -LiteralPath $Out) { throw 'Use a new evidence directory' }
New-Item -ItemType Directory -Path $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$started = [DateTime]::UtcNow.ToString('o')
$process = Start-Process -FilePath $Exe -ArgumentList @('--expected-version', $ExpectedVersion,
    '--samples', "$Samples", '--interval-ms', "$IntervalMs") -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput "$Out\samples.jsonl" -RedirectStandardError "$Out\errors.jsonl"
# Retain the native handle before exit. Windows PowerShell 5.1 can otherwise
# report HasExited=True with ExitCode=$null on Start-Process -PassThru objects.
$null = $process.Handle
$timedOut = !$process.WaitForExit(30000)
$killError = $null
if ($timedOut) {
    try { $process.Kill() } catch { $killError = $_.Exception.Message }
    # Terminating user mode is not a GPU recovery or proof that a blocked kernel call returned.
} else { $process.WaitForExit() }
$process.Refresh()
$exited = $process.HasExited
$exitCode = if ($exited) { $process.ExitCode } else { $null }
@{schema=1; started_utc=$started; ended_utc=[DateTime]::UtcNow.ToString('o');
  executable_sha256=$actual; expected_kmd_version=$ExpectedVersion; samples=$Samples;
  interval_ms=$IntervalMs; timed_out=$timedOut; process_exited=$exited;
  exit_code=$exitCode; kill_error=$killError; no_driver_change=$true} |
    ConvertTo-Json | Set-Content -LiteralPath "$Out\run.json" -Encoding UTF8
if ($timedOut) { throw 'Observer exceeded 30 seconds. Preserve evidence and inspect lab health. Do not retry automatically.' }
if ($exitCode -ne 0) { throw "Observer refused or failed: $exitCode. Preserve raw output." }

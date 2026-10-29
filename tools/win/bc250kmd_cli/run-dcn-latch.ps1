param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedSha256,
    [Parameter(Mandatory)][ValidatePattern('^0x[0-9a-fA-F]{8}$')][string]$ExpectedVersion,
    [Parameter(Mandatory)][ValidateRange(0,2)][int]$Arm,
    [Parameter(Mandatory)][string]$Out
)
# Slot owner must freeze and verify the loaded LAB KMD and boot identity before
# invoking this runner. ExpectedVersion is an ABI word, NOT an image hash.
# Run each arm separately; never continue past a failed control or cleanup.
$ErrorActionPreference='Stop'
$principal=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {throw 'Run elevated on the lab'}
$Exe=(Resolve-Path -LiteralPath $Exe).Path
$actual=(Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
if ($actual -ne $ExpectedSha256) {throw 'Latch client hash mismatch'}
if (Test-Path -LiteralPath $Out) {throw 'Use a new evidence directory'}
New-Item -ItemType Directory -Path $Out | Out-Null
$Out=(Resolve-Path -LiteralPath $Out).Path
$started=[DateTime]::UtcNow.ToString('o')
$process=Start-Process -FilePath $Exe -ArgumentList @('--expected-version',$ExpectedVersion,'--arm',"$Arm") `
    -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Out\samples.jsonl" -RedirectStandardError "$Out\errors.jsonl"
# Keep the native handle before exit, including on Windows PowerShell 5.1.
$null=$process.Handle
$timedOut=!$process.WaitForExit(30000)
$killError=$null
if ($timedOut) {
    try {$process.Kill()} catch {$killError=$_.Exception.Message}
    # A killed client is NOT proof that its kernel operation returned or that
    # the display recovered. Preserve the evidence and inspect the lab.
} else {$process.WaitForExit()}
$process.Refresh()
$exited=$process.HasExited
$exitCode=if ($exited) {$process.ExitCode} else {$null}
@{schema=1; started_utc=$started; ended_utc=[DateTime]::UtcNow.ToString('o');
  executable_sha256=$actual; expected_kmd_version=$ExpectedVersion; arm=$Arm;
  timed_out=$timedOut; process_exited=$exited; exit_code=$exitCode; kill_error=$killError;
  loaded_kmd_identity_verified_by_runner=$false; hardware_verdict=$null; no_driver_change=$true} |
    ConvertTo-Json | Set-Content -LiteralPath "$Out\run.json" -Encoding UTF8
if ($timedOut) {throw 'Latch probe exceeded 30 seconds. Inspect lab health. Do not retry automatically.'}
if ($null -eq $exitCode -or $exitCode -ne 0) {throw "Latch probe refused or failed: $exitCode. Preserve raw output and stop."}

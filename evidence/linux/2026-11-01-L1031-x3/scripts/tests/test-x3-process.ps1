# LOCAL: starts only hidden, owned, disposable process trees. No GUI probe.
param([Parameter(Mandatory=$true)][string]$Out)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
. (Join-Path $PSScriptRoot 'x3-process.ps1')
if (Test-Path -LiteralPath $Out) { throw 'use a fresh receipt directory' }
$parent = (Resolve-Path -LiteralPath (Split-Path -Parent $Out)).Path
if ($parent -cne (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\recovery')).Path) { throw 'test output must be under Linux recovery directory' }
New-Item -ItemType Directory -Path $Out | Out-Null
$fixture = Join-Path $Out 'child fixture.ps1'
Copy-Item -LiteralPath (Join-Path $parent 'x3-child-fixture.ps1') -Destination $fixture
$ps = "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe"
$results = @()
foreach ($mode in @('ok','fail','hang','orphan','tree')) {
    $seconds = if ($mode -in @('hang','tree')) { 3 } else { 6 }
    $prefix = Join-Path $Out $mode
    $pidFile = "$prefix-pid.json"
    $value = 'spaces "quoted" trailing\'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $failed = $false; $message = ''; $output = ''
    try {
        $output = Invoke-X3Bounded -File $ps -Arguments @('-NoProfile','-NonInteractive','-WindowStyle','Hidden','-File',$fixture,'-Mode',$mode,'-PidFile',$pidFile,'-Value',$value) -Seconds $seconds -ReceiptPrefix $prefix
    } catch { $failed = $true; $message = $_.Exception.Message }
    $expectedFailure = $mode -in @('fail','hang','tree')
    if ($failed -ne $expectedFailure) { throw "$mode unexpected outcome: $message" }
    $receipt = Get-Content -LiteralPath "$prefix.receipt.json" -Raw | ConvertFrom-Json
    if (-not $receipt.job_empty) { throw "$mode job remains active" }
    if ($receipt.timed_out -ne ($mode -in @('hang','tree'))) { throw "$mode wrong timeout receipt" }
    if ($mode -eq 'fail' -and $receipt.child_exit -ne 41) { throw 'lost child failure exit' }
    if ($watch.Elapsed.TotalSeconds -gt $seconds + 2.5) { throw "$mode exceeded bounded cleanup budget" }
    if ($mode -eq 'ok' -and $output.Trim() -cne $value) { throw 'argument quoting failed' }
    if ($mode -in @('orphan','tree')) {
        $id = Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json
        $live = Get-Process -Id $id.pid -ErrorAction SilentlyContinue
        if ($live -and -not $live.HasExited -and $live.StartTime.ToUniversalTime().ToString('o') -eq $id.start) { throw 'descendant survived job termination' }
    }
    $results += @{case=$mode; rejected=$failed; seconds=$watch.Elapsed.TotalSeconds; receipt=$receipt; message=$message}
}
$results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Out 'results.json')
'PASS: 5 real hidden process controls: success/quoting, child error, timeout, orphan cleanup, timed-out tree. No GUI probe.'

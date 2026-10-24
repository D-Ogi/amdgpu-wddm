param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$Out,
    [ValidateSet('default','nvidia','warp')][string]$Adapter = 'default',
    [switch]$Inject,
    [switch]$NoDebug
)
$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path -LiteralPath $Exe).Path
$outputPath = [IO.Path]::GetFullPath($Out)
if (Test-Path -LiteralPath $outputPath) { throw 'Use a new output directory to preserve previous evidence.' }
New-Item -ItemType Directory -Path $outputPath | Out-Null
$arguments = '--adapter ' + $Adapter
if ($Inject) { $arguments += ' --inject' }
if (-not $NoDebug) { $arguments += ' --debug' }
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $exePath
$start.Arguments = $arguments
$start.WorkingDirectory = Split-Path -Parent $exePath
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $start
$watch = [Diagnostics.Stopwatch]::StartNew()
$hash = (Get-FileHash -LiteralPath $exePath).Hash
try {
    if (-not $process.Start()) { throw 'Client did not start.' }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $timeout = -not $process.WaitForExit(170000)
    if ($timeout) { $process.Kill() }
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    $outText = $stdout.GetAwaiter().GetResult()
    $errText = $stderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $outputPath 'stdout.log'), $outText)
    [IO.File]::WriteAllText((Join-Path $outputPath 'stderr.log'), $errText)
    $summary = @($outText -split '\r?\n' | Where-Object { $_ -match '^RESULT ' })
    $expected = if ($Inject) { 'RESULT FAIL checks=1215216 mismatches=2316 debug_errors=0 injection=1' }
                else { 'RESULT PASS checks=1215216 mismatches=0 debug_errors=0 injection=0' }
    $expectedExit = if ($Inject) { 1 } else { 0 }
    $accepted = -not $timeout -and $exitCode -eq $expectedExit -and $summary.Count -eq 1 -and $summary[0] -ceq $expected
    $accepted = $accepted -and (Get-FileHash -LiteralPath $exePath).Hash -eq $hash
    [ordered]@{
        accepted = $accepted; timed_out = $timeout; exit_code = $exitCode
        seconds = $watch.Elapsed.TotalSeconds; arguments = $arguments
        executable_sha256 = $hash; debug_layer_enabled = -not $NoDebug
        expected_summary = $expected; observed_summary = $summary
        stdout_sha256 = (Get-FileHash -LiteralPath (Join-Path $outputPath 'stdout.log')).Hash
        stderr_sha256 = (Get-FileHash -LiteralPath (Join-Path $outputPath 'stderr.log')).Hash
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outputPath 'result.json') -Encoding UTF8
    if (-not $accepted) { throw 'Cube/mip control failed; see result.json and logs.' }
    Write-Host 'Accepted expected client result (including exit code).'
} finally { $process.Dispose() }

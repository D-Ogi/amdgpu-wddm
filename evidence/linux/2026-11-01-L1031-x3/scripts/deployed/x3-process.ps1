# Pure definitions. The exact owned-Job helper is built from the repository's
# tools/win/kmd-deploy/template/kmd168-transition/bounded-child.cpp.
function ConvertTo-X3NativeArgument {
    param([AllowEmptyString()][string]$Value)
    $b = New-Object Text.StringBuilder
    [void]$b.Append('"'); $slashes = 0
    foreach ($c in $Value.ToCharArray()) {
        if ($c -eq '\') { $slashes++; continue }
        $count = if ($c -eq '"') { $slashes * 2 } else { $slashes }
        if ($count) { [void]$b.Append(('\' * $count)) }; $slashes = 0
        if ($c -eq '"') { [void]$b.Append('\') }
        [void]$b.Append($c)
    }
    if ($slashes) { [void]$b.Append(('\' * ($slashes * 2))) }
    [void]$b.Append('"'); return $b.ToString()
}
function Invoke-X3Bounded {
    param([string]$File, [string[]]$Arguments,
          [ValidateRange(3,10)][int]$Seconds = 10,
          [string]$ReceiptPrefix = (Join-Path $PSScriptRoot ('x3-reader-' + [Guid]::NewGuid().ToString('N'))))
    $tool = Join-Path $PSScriptRoot 'bounded-child.exe'
    $expected = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'bounded-child.sha256') -Raw).Trim()
    if ($expected -notmatch '^[0-9a-fA-F]{64}$' -or (Get-FileHash -LiteralPath $tool -Algorithm SHA256).Hash -ine $expected) { throw 'bounded helper hash differs' }
    $frequency = [double][Diagnostics.Stopwatch]::Frequency
    $deadline = [Diagnostics.Stopwatch]::GetTimestamp() + [long]($Seconds * $frequency)
    # The helper reserves its final second for job closure. The supervisor gets
    # one additional second for receipt delivery, plus one for forced closure.
    $supervisor = $deadline + [long]$frequency
    $values = @([string]$deadline, "$ReceiptPrefix.out", "$ReceiptPrefix.err", $File) + $Arguments
    $p = New-Object Diagnostics.Process
    $p.StartInfo.FileName = $tool
    $p.StartInfo.Arguments = ($values | ForEach-Object { ConvertTo-X3NativeArgument $_ }) -join ' '
    $p.StartInfo.UseShellExecute = $false
    $p.StartInfo.CreateNoWindow = $true
    $p.StartInfo.RedirectStandardOutput = $true
    $p.StartInfo.RedirectStandardError = $true
    try {
        if (-not $p.Start()) { throw 'cannot start owned-Job helper' }
        $stdout = $p.StandardOutput.ReadToEndAsync()
        $stderr = $p.StandardError.ReadToEndAsync()
        $wait = [int][Math]::Max(0, [Math]::Floor(($supervisor - [Diagnostics.Stopwatch]::GetTimestamp()) * 1000 / $frequency))
        if (-not $p.WaitForExit($wait)) {
            $p.Kill() # Last job handle closes; KILL_ON_JOB_CLOSE terminates descendants.
            if (-not $p.WaitForExit(1000)) { throw 'helper failed to exit after kill; tree closure unconfirmed' }
            throw 'helper deadline exceeded; no acknowledgement, tree closure unconfirmed'
        }
        $wait = [int][Math]::Max(0, [Math]::Floor(($supervisor - [Diagnostics.Stopwatch]::GetTimestamp()) * 1000 / $frequency))
        if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout, $stderr), $wait)) { throw 'helper receipt EOF deadline exceeded' }
        # Result cannot wait: both tasks are complete. Child output is in files,
        # never inherited receipt pipes, and the complete job must be empty.
        $receiptText = $stdout.Result
        [IO.File]::WriteAllText("$ReceiptPrefix.receipt.json", $receiptText)
        $receipt = $receiptText | ConvertFrom-Json
        if ($p.ExitCode -ne 0 -or $stderr.Result.Trim() -or -not $receipt.job_empty -or -not $receipt.root_exit_observed -or $receipt.child_exit -ne 0 -or $receipt.timed_out) { throw "bounded reader failed ($($p.ExitCode)); no acknowledgement" }
        foreach ($suffix in @('out','err')) {
            if ((Get-Item -LiteralPath "$ReceiptPrefix.$suffix").Length -gt 65536) { throw 'reader output exceeds 64 KiB' }
        }
        if ([IO.File]::ReadAllText("$ReceiptPrefix.err").Trim()) { throw 'bounded reader wrote stderr; no acknowledgement' }
        return [IO.File]::ReadAllText("$ReceiptPrefix.out")
    } finally { $p.Dispose() }
}

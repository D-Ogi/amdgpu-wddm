#Requires -Version 7.0
# The receipt writer of build-release-set.ps1: a JSON document written to a part file and moved over the receipt, so
# that a killed scheduler leaves either the preceding receipt or the complete new one. A module of its own so that its
# test (test_build_release_set.py, JsonReceiptMove) runs the same function that the tool runs.
#
#   Write-Json -Path <receipt> -Data <object> [-RetrySeconds 5] [-RetryStepMs 50]
#
# BD-345: a reader that has the receipt open without delete sharing (Python's open() never shares delete) makes the
# move over the receipt fail. Windows reports it as access denied (0x80070005) for MoveFileEx with replace, and as a
# sharing violation (0x80070020) for some other holders; both end when the reader closes the file. The move is tried
# again for a short bound; after the bound the last error goes to the caller. The part file is deleted on every path
# that does not move it, so a failure never leaves it behind.

$script:Retryable = @(0x80070005, 0x80070020)

function Test-JsonReceiptRetryable([System.Exception]$Exception) {
    for ($e = $Exception; $e; $e = $e.InnerException) {
        if ($script:Retryable -contains $e.HResult) { return $true }   # both literals are negative Int32 values
    }
    return $false
}

function Write-Json {
    param(
        [Parameter(Mandatory, Position = 0)][string]$Path,
        [Parameter(Mandatory, Position = 1)][AllowNull()]$Data,
        [ValidateRange(0, 60)][double]$RetrySeconds = 5,
        [ValidateRange(1, 1000)][int]$RetryStepMs = 50
    )
    New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
    $part = "$Path.$PID.$([Guid]::NewGuid().ToString('N')).part"
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Data | ConvertTo-Json -Depth 40) + "`n")
        $stream = [IO.File]::Open($part, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
        $deadline = [Diagnostics.Stopwatch]::StartNew()
        while ($true) {
            try { [IO.File]::Move($part, $Path, $true); break }
            catch {
                if (-not (Test-JsonReceiptRetryable $_.Exception) -or $deadline.Elapsed.TotalSeconds -ge $RetrySeconds) { throw }
                Start-Sleep -Milliseconds $RetryStepMs
            }
        }
    } finally {
        if ([IO.File]::Exists($part)) { [IO.File]::Delete($part) }
    }
}

Export-ModuleMember -Function Write-Json

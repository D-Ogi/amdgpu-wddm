#Requires -Version 7.0
# Concurrent copies of the face tests share TEMP deliberately. Each test must own its files:
# the old fixed packed-record name let one process remove another process's dispatch evidence.
param(
    [Parameter(Mandatory)][string]$BinDir,
    [Parameter(Mandatory)][string]$Mock,
    [Parameter(Mandatory)][string]$CodeObject,
    [Parameter(Mandatory)][string]$Work,
    [ValidateRange(2, 32)][int]$Copies = 8,
    [ValidateRange(1, 10)][int]$Rounds = 3
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Work = [IO.Path]::GetFullPath($Work)
New-Item -ItemType Directory -Force $Work | Out-Null
$temp = Join-Path $Work ('shared-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temp | Out-Null
$sentinels = @('bc250-face-packed-record.txt', 'bc250-face-errors-tiny.hsaco')
$sentinelText = 'Another invocation owns this file.'
foreach ($name in $sentinels) { [IO.File]::WriteAllText((Join-Path $temp $name), $sentinelText) }
$runs = @()
for ($round = 0; $round -lt $Rounds; $round++) {
    $active = @()
    try {
        foreach ($kind in @('packed', 'errors')) {
            for ($copy = 0; $copy -lt $Copies; $copy++) {
                $info = [Diagnostics.ProcessStartInfo]::new()
                $info.FileName = Join-Path $BinDir "test_amd_face_$kind.exe"
                $info.UseShellExecute = $false
                $info.CreateNoWindow = $true
                $info.RedirectStandardOutput = $true
                $info.RedirectStandardError = $true
                $info.Environment['TEMP'] = $temp
                $info.Environment['TMP'] = $temp
                $info.ArgumentList.Add($Mock)
                if ($kind -eq 'packed') { $info.ArgumentList.Add($CodeObject) }
                $p = [Diagnostics.Process]::Start($info)
                $active += [pscustomobject]@{ process = $p; kind = $kind; copy = $copy
                    stdout = $p.StandardOutput.ReadToEndAsync(); stderr = $p.StandardError.ReadToEndAsync() }
            }
        }
        foreach ($run in $active) {
            $p = $run.process
            $timedOut = -not $p.WaitForExit(15000)
            if ($timedOut) { $p.Kill($true); $p.WaitForExit() }
            $body = $run.stdout.GetAwaiter().GetResult() + $run.stderr.GetAwaiter().GetResult()
            $ok = -not $timedOut -and $p.ExitCode -eq 0 -and $body -match '(?m)^PASS:' -and $body -notmatch '(?m)^FAIL:'
            $name = "$round-$($run.kind)-$($run.copy)"
            [IO.File]::WriteAllText((Join-Path $Work "$name.log"), $body)
            $runs += [pscustomobject]@{ name = $name; exit_code = $p.ExitCode; timed_out = $timedOut; pass = $ok }
        }
    } finally {
        foreach ($run in $active) {
            if (-not $run.process.HasExited) { $run.process.Kill($true); $run.process.WaitForExit() }
            $run.process.Dispose()
        }
    }
}
$sentinelsIntact = $true
foreach ($name in $sentinels) {
    $path = Join-Path $temp $name
    if (-not [IO.File]::Exists($path) -or [IO.File]::ReadAllText($path) -cne $sentinelText) { $sentinelsIntact = $false }
    if ([IO.File]::Exists($path)) { [IO.File]::Delete($path) }
}
$left = @(Get-ChildItem -LiteralPath $temp -Force)
$passed = @($runs | Where-Object pass).Count
$failed = $runs.Count - $passed
@{ runs = $runs; passed = $passed; failed = $failed; temp_entries_left = $left.Count; sentinels_intact = $sentinelsIntact } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Work 'result.json') -Encoding utf8
if (-not $left.Count) { [IO.Directory]::Delete($temp) }
Write-Host "test-amd-face-parallel: $passed PASS, $failed FAIL, $($left.Count) temp entries left, other files intact: $sentinelsIntact"
if ($failed -or $left.Count -or -not $sentinelsIntact) { exit 1 }
exit 0
